/* ============================================================================
 *  AI-Robot v1.0 · 第 8 关：总控程序（把 6 个模块 + 云语音接成一台机器人）
 *  板卡：ESP32-S3-DevKitC-1 N16R8（16MB Flash + 8MB OPI PSRAM）
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    1) 手机浏览器打开 http://<板子IP>/  → 看摄像头画面 + 遥控
 *    2) 按一下板载 BOOT 键（或点网页"说话"）→ 录音 3 秒 → 云端识别 → 大模型回答
 *       → 喇叭说话 → 同时执行动作（前进/后退/转圈/云台…）
 *    3) 超声波自动避障：< 15cm 强制停车；网页 1.5 秒不发指令自动停车（deadman）
 *  ---------------------------------------------------------------------------
 *  【接线】完全按 docs/01_接线总表.md。本文件只从 pinmap.h 取引脚，不写死任何数字。
 *  ---------------------------------------------------------------------------
 *  【第一次跑之前必须做 3 件事】
 *    (1) 把 secrets.example.h 复制成 secrets.h，填 WiFi + Key（见 docs/05）
 *    (2) Arduino IDE：PSRAM = OPI PSRAM，Partition = 16M Flash (2MB APP/12.5MB SPIFFS)
 *    (3) 装库：TFT_eSPI / ESP32Servo / ESP32-audioI2S(4.x) / ArduinoJson(7.x)
 *        esp32-camera 先不用装（核心可能自带），编译报找不到再装
 *  ---------------------------------------------------------------------------
 *  【预期现象】串口 115200 依次出现 [OK]，最后打印 http://<IP>/
 *  ---------------------------------------------------------------------------
 *  【常见错误】见 docs/04_排错手册.md K 节（云端语音）与 J 节（图传）
 * ==========================================================================*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <ESP32Servo.h>

#include "pinmap.h"

/* ============================ 0. 功能开关 ==================================
 * 排查问题时，从上往下一个个打开（比如屏幕花屏就先把 ENABLE_SCREEN 改 0）
 * ==========================================================================*/
#define ENABLE_SCREEN   1   /* 屏幕表情/状态 */
#define ENABLE_CAMERA   1   /* 摄像头 + 手机图传 */
#define ENABLE_SONAR    1   /* 超声波 + 自动避障 */
#define ENABLE_VOICE    1   /* 云语音（需要 secrets.h 里填 Key；没 Key 先改 0 也能玩） */

/* 运行时参数（想改手感就改这里） */
#define MOTOR_MAX_PERMILLE     620   /* 最高 62% 占空比：保护 5V 电源不被电机拉塌 */
#define MOTOR_RAMP_STEP         25   /* 软启动：每 5ms 最多变 25‰（约 0.12 秒到全速） */
#define MOTOR_TICK_MS            5
#define AVOID_STOP_CM           15   /* 小于这个距离强制停车 */
#define AVOID_RESUME_CM         25   /* 退到这个距离以上才允许再前进（防抖） */
#define DEADMAN_MS            1500   /* 网页超过这么久没发指令就自动停 */
#define SERVO_PAN_MIN           30
#define SERVO_PAN_MAX          150
#define SERVO_TILT_MIN          45
#define SERVO_TILT_MAX         135
#define SERVO_STEP_MS           12   /* 云台每走 1 度花 12ms */

/* 语音库 / 网页页面（都放在 firmware/07_main/ 下） */
#if ENABLE_CAMERA
  #include <esp_camera.h>          /* 关掉摄像头时就不用装 esp32-camera 也能编过 */
#endif
#include "voice_cloud.h"
#include "web_ui.h"

/* 语音关闭时也要能编译：secrets.h 里有 WiFi 账号 */
#if !ENABLE_VOICE
  #if __has_include("secrets.h")
    #include "secrets.h"
  #endif
#endif

/* ============================ 1. 全局对象 ================================= */
static WebServer   server(80);
static TFT_eSPI    tft;
static Servo       servoPan;
static Servo       servoTilt;

static bool        g_camOk      = false;
static bool        g_screenOk   = false;
static bool        g_sonarOk    = false;
static bool        g_avoidOn    = true;
static volatile bool g_wifiOk   = false;
static uint32_t    g_bootMs     = 0;
static uint32_t    g_fps        = 0;
static uint32_t    g_lastMoveMs = 0;

/* 电机状态（只在 core0 的 appTask 里改，core1 只读 —— 用临界区保护） */
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int  g_curL = 0, g_tgtL = 0;
static volatile int  g_curR = 0, g_tgtR = 0;
static volatile int  g_distCm   = 999;
static volatile bool g_avoidHit = false;

/* 上次对话（网页/屏幕显示用） */
static char g_heard[96]  = "";
static char g_reply[96]  = "";
static char g_lastErr[96] = "";

/* 定时动作（"说走两秒"这种） */
static int      g_timedL = 0, g_timedR = 0;
static uint32_t g_timedUntil = 0;

/* ============================ 2. 小工具 =================================== */
static void muxSetStr(char *dst, size_t n, const char *src) {
  portENTER_CRITICAL(&g_mux);
  snprintf(dst, n, "%s", src ? src : "");
  portEXIT_CRITICAL(&g_mux);
}
static String muxGetStr(const char *src, size_t n) {
  char tmp[96];
  portENTER_CRITICAL(&g_mux);
  memcpy(tmp, src, n);
  tmp[n - 1] = 0;
  portEXIT_CRITICAL(&g_mux);
  return String(tmp);
}
static const char *voiceStateTag() {
#if ENABLE_VOICE
  switch (voiceState()) {
    case VOICE_LISTENING: return "LISTENING";
    case VOICE_THINKING:  return "THINKING";
    case VOICE_SPEAKING:  return "SPEAKING";
    case VOICE_ERROR:     return "ERROR";
    default:              return "IDLE";
  }
#else
  return "IDLE";
#endif
}

/* ============================ 3. 电机 ===================================== */
/* 用"方向脚直接吃 PWM"驱动 TB6612（PWMA/PWMB 已接死 3V3），省 2 个引脚。
 * 正数 = 前进，负数 = 后退，0 = 断电滑行。写的是 mV 级别的软启动目标值。   */
/* 注意：core2.x 必须显式给 LEDC 通道号（4~7 避开摄像头的 CHANNEL_0），
 *       所以下面每个调用都带通道号；core3.x 的 ledcAttach 用引脚当句柄，通道号参数被忽略。 */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define MOTOR_ATTACH(pin, ch)      ledcAttach((pin), MOTOR_PWM_FREQ, MOTOR_PWM_RES)
  #define MOTOR_WRITE(pin, ch, duty) ledcWrite((pin), (duty))
#else
  #define MOTOR_ATTACH(pin, ch)      do { ledcSetup((ch), MOTOR_PWM_FREQ, MOTOR_PWM_RES); ledcAttachPin((pin), (ch)); } while (0)
  #define MOTOR_WRITE(pin, ch, duty) ledcWrite((ch), (duty))
#endif

static void motorAttach() {
  pinMode(PIN_MOTOR_AIN1, OUTPUT); pinMode(PIN_MOTOR_AIN2, OUTPUT);
  pinMode(PIN_MOTOR_BIN1, OUTPUT); pinMode(PIN_MOTOR_BIN2, OUTPUT);
  digitalWrite(PIN_MOTOR_AIN1, LOW); digitalWrite(PIN_MOTOR_AIN2, LOW);
  digitalWrite(PIN_MOTOR_BIN1, LOW); digitalWrite(PIN_MOTOR_BIN2, LOW);
  MOTOR_ATTACH(PIN_MOTOR_AIN1, 4);
  MOTOR_ATTACH(PIN_MOTOR_AIN2, 5);
  MOTOR_ATTACH(PIN_MOTOR_BIN1, 6);
  MOTOR_ATTACH(PIN_MOTOR_BIN2, 7);
}

/* 直接把左右轮写成 -1023~1023（内部用） */
static void motorWriteRaw(int l, int r) {
  l = constrain(l, -MOTOR_PWM_MAX, MOTOR_PWM_MAX);
  r = constrain(r, -MOTOR_PWM_MAX, MOTOR_PWM_MAX);
  /* 左轮 A 通道 */
  if (l > 0)      { MOTOR_WRITE(PIN_MOTOR_AIN1, 4, l); MOTOR_WRITE(PIN_MOTOR_AIN2, 5, 0); }
  else if (l < 0) { MOTOR_WRITE(PIN_MOTOR_AIN1, 4, 0); MOTOR_WRITE(PIN_MOTOR_AIN2, 5, -l); }
  else            { MOTOR_WRITE(PIN_MOTOR_AIN1, 4, 0); MOTOR_WRITE(PIN_MOTOR_AIN2, 5, 0); }
  /* 右轮 B 通道 */
  if (r > 0)      { MOTOR_WRITE(PIN_MOTOR_BIN1, 6, r); MOTOR_WRITE(PIN_MOTOR_BIN2, 7, 0); }
  else if (r < 0) { MOTOR_WRITE(PIN_MOTOR_BIN1, 6, 0); MOTOR_WRITE(PIN_MOTOR_BIN2, 7, -r); }
  else            { MOTOR_WRITE(PIN_MOTOR_BIN1, 6, 0); MOTOR_WRITE(PIN_MOTOR_BIN2, 7, 0); }
}

/* 对外：设目标速度（千分比 -1000~1000），真正加速由 motorTick() 慢慢做 */
static void motorSet(int leftPerMille, int rightPerMille) {
  int lim = MOTOR_MAX_PERMILLE * MOTOR_PWM_MAX / 1000;
  int l = constrain(leftPerMille,  -1000, 1000) * MOTOR_PWM_MAX / 1000;
  int r = constrain(rightPerMille, -1000, 1000) * MOTOR_PWM_MAX / 1000;
  l = constrain(l, -lim, lim);
  r = constrain(r, -lim, lim);
  portENTER_CRITICAL(&g_mux);
  g_tgtL = l; g_tgtR = r;
  portEXIT_CRITICAL(&g_mux);
  g_lastMoveMs = millis();
}

/* 立刻断电滑行（最安全的停车，也是上电第一件事） */
static void motorCoastNow() {
  portENTER_CRITICAL(&g_mux);
  g_tgtL = g_tgtR = g_curL = g_curR = 0;
  portEXIT_CRITICAL(&g_mux);
  motorWriteRaw(0, 0);
}

static bool motorBusy() {
  return (g_curL != g_tgtL) || (g_curR != g_tgtR) || (g_tgtL != 0) || (g_tgtR != 0);
}

/* 每 5ms 调一次：软启动 + 定时动作到点收车 */
static void motorTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < MOTOR_TICK_MS) return;
  last = now;

  /* 定时动作到期 → 收车 */
  if (g_timedUntil && (int32_t)(now - g_timedUntil) >= 0) {
    g_timedUntil = 0;
    int l = 0, r = 0;
    portENTER_CRITICAL(&g_mux); g_tgtL = l; g_tgtR = r; portEXIT_CRITICAL(&g_mux);
  }

  int tl, tr, cl, cr;
  portENTER_CRITICAL(&g_mux);
  tl = g_tgtL; tr = g_tgtR; cl = g_curL; cr = g_curR;
  portEXIT_CRITICAL(&g_mux);

  auto step = [](int cur, int target) {
    if (cur < target) { cur += MOTOR_RAMP_STEP; if (cur > target) cur = target; }
    else if (cur > target) { cur -= MOTOR_RAMP_STEP; if (cur < target) cur = target; }
    return cur;
  };
  int nl = step(cl, tl), nr = step(cr, tr);
  if (nl != cl || nr != cr) {
    portENTER_CRITICAL(&g_mux);
    g_curL = nl; g_curR = nr;
    portEXIT_CRITICAL(&g_mux);
    motorWriteRaw(nl, nr);
  }
}

/* ============================ 4. 云台 ===================================== */
static int g_panDeg = 90, g_tiltDeg = 90;      /* 当前角 */
static int g_panTgt = 90, g_tiltTgt = 90;      /* 目标角 */

static void gimbalInit() {
  servoPan.setPeriodHertz(SERVO_FREQ);
  servoTilt.setPeriodHertz(SERVO_FREQ);
  bool ok1 = servoPan.attach(PIN_SERVO_PAN, SERVO_MIN_US, SERVO_MAX_US);
  bool ok2 = servoTilt.attach(PIN_SERVO_TILT, SERVO_MIN_US, SERVO_MAX_US);
  servoPan.write(g_panDeg);
  servoTilt.write(g_tiltDeg);
  Serial.printf("[%s] 云台就绪 PAN=GPIO%d TILT=GPIO%d\n",
                (ok1 && ok2) ? "OK" : "WARN", PIN_SERVO_PAN, PIN_SERVO_TILT);
}

static void gimbalTick() {
  static uint32_t last = 0;
  if (millis() - last < SERVO_STEP_MS) return;
  last = millis();
  bool moved = false;
  if (g_panDeg < g_panTgt)  { g_panDeg++; moved = true; }
  if (g_panDeg > g_panTgt)  { g_panDeg--; moved = true; }
  if (g_tiltDeg < g_tiltTgt) { g_tiltDeg++; moved = true; }
  if (g_tiltDeg > g_tiltTgt) { g_tiltDeg--; moved = true; }
  if (moved) {
    servoPan.write(constrain(g_panDeg, SERVO_PAN_MIN, SERVO_PAN_MAX));
    servoTilt.write(constrain(g_tiltDeg, SERVO_TILT_MIN, SERVO_TILT_MAX));
  }
}

/* 云台动一小步（网页点一下转 15 度） */
static void gimbalNudge(const String &d) {
  int p = g_panTgt, t = g_tiltTgt;
  if (d == "left")   p += 15;
  if (d == "right")  p -= 15;
  if (d == "up")     t -= 15;
  if (d == "down")   t += 15;
  if (d == "center") { p = 90; t = 90; }
  g_panTgt  = constrain(p, SERVO_PAN_MIN, SERVO_PAN_MAX);
  g_tiltTgt = constrain(t, SERVO_TILT_MIN, SERVO_TILT_MAX);
}

/* ============================ 5. 超声波 =================================== */
static int sonarOnceCm() {
  digitalWrite(PIN_SONAR_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_SONAR_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_SONAR_TRIG, LOW);
  unsigned long us = pulseIn(PIN_SONAR_ECHO, HIGH, 25000);   /* 25ms≈4m，绝不卡死 */
  if (us == 0) return -1;
  long cm = (long)((us * 17UL) / 1000UL);
  if (cm < 2 || cm > 450) return -1;
  return (int)cm;
}

static void sonarTick() {
  static uint32_t last = 0;
  if (millis() - last < 120) return;
  last = millis();
  int v[3], n = 0;
  for (int i = 0; i < 3; i++) {
    int c = sonarOnceCm();
    if (c > 0) v[n++] = c;
  }
  int out = -1;
  if (n == 1) out = v[0];
  else if (n == 2) out = (v[0] + v[1]) / 2;
  else if (n == 3) {
    int a = v[0], b = v[1], c = v[2];
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    out = b;
  }
  g_distCm = (out > 0) ? out : 999;
  g_sonarOk = (out > 0);
}

/* ============================ 6. 屏幕 ===================================== */
static void screenInit() {
#if ENABLE_SCREEN
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("AI-Robot booting...", 10, 10, 2);
  g_screenOk = true;
  Serial.println("[OK] 屏幕初始化完成（白屏请查 User_Setup.h / RST / BLK）");
#endif
}

/* 把屏幕画成一张"脸 + 状态栏"：不用 Sprite，省内部 RAM */
static void screenDraw() {
#if ENABLE_SCREEN
  if (!g_screenOk) return;
  static uint32_t last = 0;
  if (millis() - last < 300) return;
  last = millis();

  const char *st = voiceStateTag();
  bool talking = (strcmp(st, "SPEAKING") == 0);
  bool listening = (strcmp(st, "LISTENING") == 0);
  bool thinking = (strcmp(st, "THINKING") == 0);
  int dist = g_distCm;

  /* 脸：两只眼睛 + 一张嘴 */
  tft.fillRect(0, 0, 240, 200, TFT_BLACK);
  uint16_t eye = listening ? TFT_GREEN : (thinking ? TFT_YELLOW : TFT_CYAN);
  int eh = listening ? 34 : 26;
  tft.fillRoundRect(52, 82 - eh / 2, 40, eh, 8, eye);
  tft.fillRoundRect(148, 82 - eh / 2, 40, eh, 8, eye);
  /* 嘴：说话时变成一条上下跳动的线 */
  int mh = talking ? (8 + (millis() / 90) % 12) : 2;
  tft.fillRect(84, 140, 72, mh, TFT_ORANGE);

  /* 状态栏 */
  tft.fillRect(0, 200, 240, 120, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char b1[40], b2[40], b3[40];
  snprintf(b1, sizeof(b1), "%s  %ldcm", st, dist >= 999 ? -1L : (long)dist);
  snprintf(b2, sizeof(b2), "IP %s", g_wifiOk ? WiFi.localIP().toString().c_str() : "no wifi");
  snprintf(b3, sizeof(b3), "avoid:%s  %lufps", g_avoidOn ? "ON" : "OFF", (unsigned long)g_fps);
  tft.drawString(b1, 8, 210, 2);
  tft.drawString(b2, 8, 238, 2);
  tft.drawString(b3, 8, 266, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  String say = muxGetStr(g_reply, sizeof(g_reply));
  if (say.length() == 0) say = muxGetStr(g_heard, sizeof(g_heard));
  tft.drawString(say.substring(0, 22).c_str(), 8, 294, 2);
#endif
}

/* ============================ 7. 摄像头 =================================== */
#if ENABLE_CAMERA
static bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = PIN_CAM_D0; c.pin_d1 = PIN_CAM_D1; c.pin_d2 = PIN_CAM_D2; c.pin_d3 = PIN_CAM_D3;
  c.pin_d4 = PIN_CAM_D4; c.pin_d5 = PIN_CAM_D5; c.pin_d6 = PIN_CAM_D6; c.pin_d7 = PIN_CAM_D7;
  c.pin_xclk = PIN_CAM_XCLK;
  c.pin_pclk = PIN_CAM_PCLK;
  c.pin_vsync = PIN_CAM_VSYNC;
  c.pin_href  = PIN_CAM_HREF;
  c.pin_sccb_sda = PIN_CAM_SIOD;     /* 旧版库叫 pin_sscb_sda，报错就换成那个拼写 */
  c.pin_sccb_scl = PIN_CAM_SIOC;
  c.pin_pwdn  = -1;                  /* 模块 PWDN 直接接 GND */
  c.pin_reset = -1;                  /* 模块 RST  直接接 3V3  */
  c.xclk_freq_hz = 20000000;

  /* 模块没有 XCLK 引脚（自带 24MHz 晶振）时，把这行改成 1 —— 见 docs/01 第 3.1 节 */
  #ifndef CAM_ONBOARD_CLOCK
    #define CAM_ONBOARD_CLOCK 0
  #endif
  #if CAM_ONBOARD_CLOCK
    c.pin_xclk     = -1;
    c.xclk_freq_hz = 24000000;
  #endif
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  c.sccb_i2c_port = 0;

  for (int i = 0; i < 3; i++) {
    esp_err_t err = esp_camera_init(&c);
    if (err == ESP_OK) return true;
    Serial.printf("[FAIL] 摄像头初始化失败 (第%d次) err=0x%x\n", i + 1, err);
    Serial.println("       查: D0~D7 是否插到底 / RST 接 3V3 / PWDN 接 GND / PSRAM 选 OPI PSRAM");
    esp_camera_deinit();
    delay(600);
  }
  return false;
}
#endif

/* ============================ 8. 动作执行 ================================= */
/* 网页/语音都走这里，统一做安全拦截 */
static void doAction(const String &raw) {
  String a = raw; a.toUpperCase();
  bool blocked = g_avoidOn && g_avoidHit;

  if (a == "STOP")      { motorSet(0, 0); g_timedUntil = 0; return; }
  if (a == "PAN_LEFT")  { gimbalNudge("left");   return; }
  if (a == "PAN_RIGHT") { gimbalNudge("right");  return; }
  if (a == "TILT_UP")   { gimbalNudge("up");     return; }
  if (a == "TILT_DOWN") { gimbalNudge("down");   return; }
  if (a == "LOOK")      { gimbalNudge("center"); return; }
  if (a == "CHAT")      { return; }              /* 纯聊天，不动 */

  /* 以下都是"动轮子"的动作：避障说了算 */
  if (blocked && (a == "FORWARD")) {
    Serial.println("[AVOID] 前方 <15cm，拒绝前进命令");
    return;
  }
  if (a == "FORWARD") { motorSet(600, 600);  g_timedUntil = millis() + 1200; }
  else if (a == "BACK")    { motorSet(-600, -600); g_timedUntil = millis() + 1200; }
  else if (a == "LEFT")    { motorSet(-450, 450);  g_timedUntil = millis() + 900; }
  else if (a == "RIGHT")   { motorSet(450, -450);  g_timedUntil = millis() + 900; }
}

/* ============================ 9. HTTP 处理 ================================ */
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", WEB_UI_HTML);
}

#if ENABLE_CAMERA
static void handleStream() {
  /* core 3.x 的 WebServer::client() 返回 NetworkClient&（WiFiClient 的基类），
   * 必须用引用接收；写成 WiFiClient x = ... 会编译报错（基类不能隐式转成派生类）。 */
  NetworkClient &client = server.client();
  client.setNoDelay(true);
  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
               "Cache-Control: no-cache\r\n"
               "Connection: close\r\n\r\n");
  uint32_t frames = 0, t0 = millis();
  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { delay(5); continue; }
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                  (unsigned)fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    frames++;
    if (!client.connected()) break;
    delay(1);                                  /* 喂狗 + 让 core1 不饿死 */
  }
  /* 更新 fps（留给 /api/status 显示） */
  uint32_t dt = millis() - t0;
  if (dt > 500) g_fps = frames * 1000 / dt;
  client.stop();
}

static void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { server.send(500, "text/plain", "camera busy"); return; }
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}
#endif

static void handleStatus() {
  char heard[96], reply[96], last[96];
  portENTER_CRITICAL(&g_mux);
  memcpy(heard, g_heard, sizeof(heard));
  memcpy(reply, g_reply, sizeof(reply));
  memcpy(last,  g_lastErr, sizeof(last));
  portEXIT_CRITICAL(&g_mux);

  char json[640];
  snprintf(json, sizeof(json),
           "{\"ip\":\"%s\",\"rssi\":%d,\"dist\":%d,\"fps\":%lu,\"avoid\":%d,"
           "\"state\":\"%s\",\"busy\":%d,\"heard\":\"%s\",\"reply\":\"%s\","
           "\"last\":\"%s\",\"power\":\"电池\",\"heap\":%u,\"cam\":%d,\"voice\":%d}",
           g_wifiOk ? WiFi.localIP().toString().c_str() : "-",
           g_wifiOk ? WiFi.RSSI() : 0,
           g_distCm >= 999 ? -1 : (int)g_distCm,
           (unsigned long)g_fps,
           g_avoidOn ? 1 : 0,
           voiceStateTag(),
#if ENABLE_VOICE
           voiceIsBusy() ? 1 : 0,
#else
           0,
#endif
           heard[0] ? heard : "", reply[0] ? reply : "", last[0] ? last : "",
           (unsigned)ESP.getFreeHeap(),
           g_camOk ? 1 : 0,
#if ENABLE_VOICE
           1
#else
           0
#endif
  );
  server.send(200, "application/json", json);
}

static void handleMove() {
  String dir = server.arg("dir");
  dir.toLowerCase();
  /* 语音正在听/说的时候锁住电机，避免自己边说话边乱跑 */
#if ENABLE_VOICE
  if (voiceIsBusy() && dir != "stop") {
    server.send(200, "application/json", "{\"ok\":0,\"err\":\"voice busy\"}");
    return;
  }
#endif
  if      (dir == "forward") doAction("FORWARD");
  else if (dir == "back")    doAction("BACK");
  else if (dir == "left")    doAction("LEFT");
  else if (dir == "right")   doAction("RIGHT");
  else                       doAction("STOP");
  g_lastMoveMs = millis();
  server.send(200, "application/json", "{\"ok\":1}");
}

static void handleGimbal() {
  String d = server.arg("d");
  d.toLowerCase();
  gimbalNudge(d);
  server.send(200, "application/json", "{\"ok\":1}");
}

static void handleAvoid() {
  g_avoidOn = (server.arg("on") == "1");
  server.send(200, "application/json", g_avoidOn ? "{\"ok\":1,\"avoid\":1}"
                                                 : "{\"ok\":1,\"avoid\":0}");
}

static void handleStop() {
  motorSet(0, 0);
  motorCoastNow();
  g_timedUntil = 0;
  server.send(200, "application/json", "{\"ok\":1}");
}

/* /api/say：立即返回，真正的录音/对话在 core0 的任务里做，不阻塞网页 */
static volatile bool g_sayRequest = false;
static void handleSay() {
#if ENABLE_VOICE
  g_sayRequest = true;
  server.send(200, "application/json", "{\"ok\":1,\"msg\":\"开始录音\"}");
#else
  server.send(200, "application/json", "{\"ok\":0,\"msg\":\"语音已关闭\"}");
#endif
}

static void handleAction() {
  doAction(server.arg("name"));
  server.send(200, "application/json", "{\"ok\":1}");
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

/* ============================ 10. WiFi ==================================== */
static bool wifiConnect(uint32_t timeoutMs = 20000) {
  Serial.printf("[..] 连接 WiFi \"%s\" (只支持 2.4GHz)\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 /* 关省电：图传更流畅 */
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiOk = true;
    Serial.printf("[OK] WiFi 已连接  IP=%s  信号=%ddBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  g_wifiOk = false;
  Serial.println("[FAIL] WiFi 连不上：确认是 2.4G 热点、SSID/密码没写错、没有中文");
  return false;
}

/* ============================ 11. core0 主任务 ============================ */
/* 这个任务负责：电机软启动、超声波避障、云台、屏幕、按键、语音对话。
 * 放在 core0，core1 留给 loop() 里跑 HTTP（图传很吃时间）。             */
static void appTask(void *arg) {
  (void)arg;
  uint32_t lastFailBeep = 0;

  for (;;) {
    motorTick();
    gimbalTick();
#if ENABLE_SONAR
    sonarTick();
    /* 避障判定（带迟滞，防止在临界距离来回抖） */
    if (g_avoidOn && g_distCm < AVOID_STOP_CM) {
      if (!g_avoidHit) {
        g_avoidHit = true;
        portENTER_CRITICAL(&g_mux);
        g_tgtL = g_tgtR = 0;
        portEXIT_CRITICAL(&g_mux);
        Serial.printf("[AVOID] 距离 %dcm < %dcm，强制停车\n", (int)g_distCm, AVOID_STOP_CM);
      }
    } else if (g_distCm > AVOID_RESUME_CM) {
      g_avoidHit = false;
    }
#endif
    /* deadman：网页 1.5 秒没发指令就自己停 */
    if (millis() - g_lastMoveMs > DEADMAN_MS && !g_timedUntil && motorBusy()) {
      portENTER_CRITICAL(&g_mux);
      g_tgtL = g_tgtR = 0;
      portEXIT_CRITICAL(&g_mux);
    }

    /* 板载 BOOT 键：按一下 = 开始一轮对话（按下为低） */
#if ENABLE_VOICE
    static bool lastBtn = true;
    bool btn = digitalRead(PIN_BOOT_BTN);
    if (lastBtn && !btn && !voiceIsBusy()) g_sayRequest = true;
    lastBtn = btn;
#endif

    /* 收到"说话"请求 → 停车 → 录音对话 → 执行动作 */
#if ENABLE_VOICE
    if (g_sayRequest) {
      g_sayRequest = false;
      motorCoastNow();
      g_timedUntil = 0;
      VoiceReply r;
      Serial.println("[VOICE] ===== 开始一轮对话 =====");
      bool ok = voiceDialog(r);
      if (ok) {
        muxSetStr(g_heard, sizeof(g_heard), r.heard.c_str());
        muxSetStr(g_reply, sizeof(g_reply), r.reply.c_str());
        muxSetStr(g_lastErr, sizeof(g_lastErr), "");
        Serial.printf("[VOICE] 听到:%s | 回复:%s | 动作:%s\n",
                      r.heard.c_str(), r.reply.c_str(), r.action.c_str());
        doAction(r.action);
      } else {
        muxSetStr(g_lastErr, sizeof(g_lastErr), r.error.c_str());
        Serial.printf("[VOICE] 失败: %s\n", r.error.c_str());
        if (millis() - lastFailBeep > 5000) { lastFailBeep = millis(); }
      }
    }
#endif

    screenDraw();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* ============================ 12. setup / loop ============================ */
void setup() {
  Serial.begin(115200);
  delay(300);
  g_bootMs = millis();

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot v1.0 · 总控");
  Serial.println("==================================================");

  /* ★ 安全底线：上电第一件事，把电机写成 0（此时轮子不会动） */
  motorAttach();
  motorCoastNow();
  Serial.println("[OK] 电机已初始化并置为停止");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);        /* 板载 BOOT 键 = 说话键 */
  pinMode(PIN_SONAR_TRIG, OUTPUT);
  pinMode(PIN_SONAR_ECHO, INPUT);
  digitalWrite(PIN_SONAR_TRIG, LOW);

  gimbalInit();
  screenInit();

  Serial.printf("[..] PSRAM: 总 %u KB / 空闲 %u KB\n",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  if (ESP.getPsramSize() == 0) {
    Serial.println("[FAIL] 没检测到 PSRAM！Tools -> PSRAM 必须选 OPI PSRAM");
  }

#if ENABLE_CAMERA
  g_camOk = cameraInit();
  Serial.println(g_camOk ? "[OK] 摄像头就绪" : "[FAIL] 摄像头未就绪（图传不可用，其它功能照常）");
#endif

  bool netOk = wifiConnect();
  if (!netOk) Serial.println("[WARN] 没有网络：只能本地测试电机/云台/屏幕，语音和图传用不了");

#if ENABLE_VOICE
  if (netOk) {
    Serial.println("[..] 初始化云语音...");
    if (voiceSetup()) Serial.println("[OK] 云语音就绪（按 BOOT 键或网页点说话）");
    else              Serial.printf("[FAIL] 云语音初始化失败: %s\n", voiceLastError());
  } else {
    Serial.println("[WARN] 跳过云语音初始化（没网）");
  }
#endif

  /* HTTP 路由（和 web_ui.h 顶部的接口表一一对应） */
  server.on("/",             handleRoot);
  server.on("/status",       handleStatus);
  server.on("/api/status",   handleStatus);
  server.on("/api/move",     handleMove);
  server.on("/api/gimbal",   handleGimbal);
  server.on("/api/avoid",    handleAvoid);
  server.on("/api/stop",     handleStop);
  server.on("/api/say",      handleSay);
  server.on("/api/action",   handleAction);
#if ENABLE_CAMERA
  server.on("/stream",       handleStream);
  server.on("/capture",      handleCapture);
#endif
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[OK] HTTP 服务已启动");

  if (g_wifiOk) {
    Serial.println();
    Serial.printf("  ★ 手机浏览器打开:  http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("  ★ 只想看画面:      http://%s/stream\n", WiFi.localIP().toString().c_str());
    Serial.println();
  }

  /* core0：电机/避障/云台/屏幕/按键/语音
   * 栈给 16KB：里面要跑 HTTPS(TLS 握手很吃栈) + JSON 解析，8KB 容易栈溢出重启 */
  xTaskCreatePinnedToCore(appTask, "appTask", 16384, NULL, 1, NULL, 0);
  Serial.println("[OK] 控制任务已启动 (core0)");

  g_lastMoveMs = millis();
  Serial.println("==================================================");
  Serial.println(" 就绪。按一下 BOOT 键说话，或打开网页遥控。");
  Serial.println("==================================================");
}

void loop() {
  /* core1：只干 HTTP（图传会长时间占住这里，所以控制逻辑都在 core0） */
  server.handleClient();

  /* WiFi 掉线自愈 */
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      g_wifiOk = false;
      Serial.println("[WARN] WiFi 断开，重连中...");
      WiFi.reconnect();
    } else {
      g_wifiOk = true;
    }
  }
  delay(2);
}
