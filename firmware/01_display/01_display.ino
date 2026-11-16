#include "pinmap.h"

/* ============================================================================
 *  01_display.ino  ——  2.4 寸 ST7789 彩屏：上电自检 + 红绿蓝色块 + 机器人表情
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    1) 点亮 240x320 的 ST7789 彩屏，证明"屏幕接线 + TFT_eSPI 配置"是对的；
 *    2) 上电先自检：串口打印引脚表、屏幕上也列出引脚，出问题好对照；
 *    3) 依次刷 红 / 绿 / 蓝 三色块，肉眼检查有没有坏点、颜色是否正常；
 *    4) 显示 "AI-Robot v1.0" + 中文"桌面机器人" + "Desktop Robot"；
 *    5) 进入"机器人表情"循环：两只眼睛会看来看去、每 3.2 秒眨一次眼，
 *       底部实时显示运行时间和帧率。目标 20fps，用 Sprite 整块推屏 → 不闪。
 *
 *  【接线】（详见 docs\01_接线总表.md 第 3.2 节；电源请务必接 3V3）
 *      屏幕 VCC      → 3V3
 *      屏幕 GND      → GND
 *      屏幕 SCL/SCK  → GPIO 21   (PIN_TFT_SCL)
 *      屏幕 SDA/MOSI → GPIO 47   (PIN_TFT_SDA)
 *      屏幕 CS       → GPIO 3    (PIN_TFT_CS)
 *      屏幕 DC/RS    → GPIO 38   (PIN_TFT_DC)
 *      屏幕 RES/RST  → 3V3       (接死，不占引脚)
 *      屏幕 BLK/LED  → 3V3       (接死，屏幕常亮)
 *    另外必须先做一件事：把 config\User_Setup.h 复制覆盖到
 *    Arduino\libraries\TFT_eSPI\User_Setup.h，否则 TFT_eSPI 不知道引脚和驱动芯片。
 *
 *  【预期现象】
 *    - 串口(115200)依次打印 [OK] ...，包含屏幕引脚表；
 *    - 屏幕依次出现：引脚表 → 红/绿/蓝 → "AI-Robot v1.0 / 桌面机器人"；
 *    - 最后稳定在一个蓝色的"脸"：两只青绿色眼睛左右张望，每 3.2 秒眨一次眼；
 *    - 底部左边 "T+12.3s" 在走，右边 "FPS 19~20"。
 *
 *  【常见错误】
 *    - 白屏 / 花屏 / 只有背光亮：① User_Setup.h 没替换或没重开 IDE；
 *      ② 屏幕 RST、BLK 忘了接 3V3；③ 线太长 → 把 SPI_FREQUENCY 降到 27000000。
 *    - 颜色是底片（黑变白）：在 User_Setup.h 里打开 TFT_INVERSION_ON / OFF 试。
 *    - 红蓝互换：User_Setup.h 里 #define TFT_RGB_ORDER 1。
 *    - 完全不亮：VCC 检查是不是 3V3（不是 5V），GND 有没有共地。
 *    - 编译报错以 "[TFT_eSPI" 开头：说明这份 User_Setup.h 和 pinmap.h 不一致，
 *      按报错里写的那句话改，不要改代码逻辑。
 * ==========================================================================*/

#include <TFT_eSPI.h>            // 屏幕库（Bodmer/TFT_eSPI）
#include <math.h>                // sinf/floorf：画眨眼圆弧和高光时用

/* ==========================================================================
 *  0. 编译期自检：User_Setup.h 必须和 pinmap.h 严格一致
 *     （这样配错时在编译阶段就报错，不会烧进去看白屏）
 * ==========================================================================*/
#if !defined(ST7789_DRIVER) || !defined(TFT_SCLK) || !defined(TFT_MOSI) || \
    !defined(TFT_CS) || !defined(TFT_DC) || !defined(TFT_WIDTH) || !defined(TFT_HEIGHT)
#error "[TFT_eSPI not configured] Copy config/User_Setup.h into Arduino/libraries/TFT_eSPI/User_Setup.h and recompile (see the comment block above)."
#endif

#if (TFT_SCLK != PIN_TFT_SCL) || (TFT_MOSI != PIN_TFT_SDA) || \
    (TFT_CS != PIN_TFT_CS) || (TFT_DC != PIN_TFT_DC)
#error "[TFT_eSPI pins mismatch] User_Setup.h must use SCLK=21, MOSI=47, CS=3, DC=38, i.e. PIN_TFT_SCL/SDA/CS/DC from firmware/_shared/pinmap.h."
#endif

#if (TFT_WIDTH != 240) || (TFT_HEIGHT != 320)
#error "[TFT_eSPI size mismatch] TFT_WIDTH must be 240 and TFT_HEIGHT must be 320."
#endif

#if defined(TFT_RST) && (TFT_RST >= 0)
#error "[TFT_eSPI] TFT_RST must be -1: the screen RST pin is wired to 3V3 in this project."
#endif

#if defined(TFT_BL) && (TFT_BL >= 0)
#error "[TFT_eSPI] TFT_BL must be -1: the screen BLK pin is wired to 3V3 in this project."
#endif

/* ==========================================================================
 *  1. 对象、常量、颜色
 * ==========================================================================*/
TFT_eSPI    tft    = TFT_eSPI();   // 屏幕对象
TFT_eSprite face   = TFT_eSprite(&tft);   // 表情层：先在内存里画好，再整块推屏 → 不闪
TFT_eSprite footer = TFT_eSprite(&tft);   // 底部状态栏：运行时间 + FPS

#define FACE_X     20              // 表情层在屏幕上的左上角
#define FACE_Y    100
#define FACE_W    200
#define FACE_H    120
#define FOOT_Y    286              // 底部状态栏的 y 坐标（286 + 34 = 320，正好到底）
#define FOOT_H     34

#define FRAME_MS            50     // 一帧 50ms = 20fps
#define FOOTER_MS          250     // 底部状态栏每 250ms 刷新一次
#define BLINK_PERIOD_MS   3200     // 每 3.2 秒眨一次眼
#define BLINK_CLOSE_MS     110     // 闭眼用时
#define BLINK_HOLD_MS       80     // 闭住用时
#define BLINK_OPEN_MS      130     // 睁开用时

uint16_t C_BG, C_EYE, C_DIM, C_HEAD, C_MOUTH, C_PUPIL, C_WHITE;  // 颜色（RGB565）

uint32_t t_boot;          // 开机毫秒数
uint32_t t_lastFrame;     // 上一帧的时间
uint32_t t_lastFooter;    // 上次刷新底部的时间
uint32_t t_fpsMark;       // 上次统计 FPS 的时间
uint32_t frames;          // 这一秒画了几帧
int      fps = 0;         // 实测帧率

// 中文点阵数据在文件最末尾定义，这里先声明，供"开屏画面"使用
extern const uint8_t CN_GLYPHS[5][72];

/* ==========================================================================
 *  2. 开屏自检画面
 * ==========================================================================*/

// 屏幕上的引脚表（和串口打印的内容一致，方便不接串口时对照）
void screenSelfTestPins() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(C_EYE, C_BG);
  tft.drawString("Display self-test", 14, 12, 2);
  tft.drawFastHLine(14, 34, 212, C_DIM);

  char buf[40];
  tft.setTextColor(C_HEAD, C_BG);
  int y = 52;
  const int dy = 26;

  snprintf(buf, sizeof(buf), "ST7789 %dx%d", TFT_WIDTH, TFT_HEIGHT);
  tft.drawString(buf, 14, y, 2);  y += dy;
  snprintf(buf, sizeof(buf), "SCLK =GPIO%d", PIN_TFT_SCL);
  tft.drawString(buf, 14, y, 2);  y += dy;
  snprintf(buf, sizeof(buf), "MOSI =GPIO%d", PIN_TFT_SDA);
  tft.drawString(buf, 14, y, 2);  y += dy;
  snprintf(buf, sizeof(buf), "CS   =GPIO%d", PIN_TFT_CS);
  tft.drawString(buf, 14, y, 2);  y += dy;
  snprintf(buf, sizeof(buf), "DC   =GPIO%d", PIN_TFT_DC);
  tft.drawString(buf, 14, y, 2);  y += dy;

  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("RST -> 3V3", 14, y, 2); y += dy;
  tft.drawString("BLK -> 3V3", 14, y, 2); y += dy;
  tft.drawString("VCC -> 3V3", 14, y, 2);
}

// 红 / 绿 / 蓝 全屏色块，肉眼检查屏幕和排线
void screenColorBars() {
  const uint16_t colors[3] = {
    tft.color565(255, 0, 0),      // 红
    tft.color565(0, 255, 0),      // 绿
    tft.color565(0, 0, 255)       // 蓝
  };
  const char *names[3] = { "RED", "GREEN", "BLUE" };

  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < 3; i++) {
    tft.fillScreen(colors[i]);
    tft.setTextColor(TFT_WHITE, colors[i]);
    tft.drawString(names[i], 120, 160, 4);
    Serial.printf("[OK] 色块 %s\n", names[i]);
    delay(600);
  }
}

// 标题画面："AI-Robot v1.0" + 中文"桌面机器人"
void screenTitle() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(C_EYE, C_BG);
  tft.drawString("AI-Robot v1.0", 120, 108, 4);      // 26px 英文大标题

  // 5 个 24x24 汉字，居中排开（不需要字库，数据就在本文件末尾）
  const int cnX = (240 - 5 * 24) / 2;
  for (int i = 0; i < 5; i++) {
    tft.drawBitmap(cnX + i * 24, 150, CN_GLYPHS[i], 24, 24, TFT_WHITE);
  }

  tft.setTextColor(C_HEAD, C_BG);
  tft.drawString("Desktop Robot", 120, 202, 2);

  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("display OK", 120, 228, 1);
  tft.drawString("ST7789 240x320", 120, 242, 1);

  Serial.println("[OK] 标题画面已显示");
  delay(1500);
}

// 表情模式下的固定背景（只画一次，后面用 Sprite 覆盖动态区域）
void screenFaceBackdrop() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_HEAD, C_BG);
  tft.drawString("AI-Robot v1.0", 120, 8, 2);
  tft.drawFastHLine(40, 30, 160, C_DIM);
}

/* ==========================================================================
 *  3. 表情：两只眼睛 + 眨眼 + 底部运行时间
 * ==========================================================================*/

// 画一只眼睛。open = 1.0 完全睁开，0 完全闭上；dx/dy 是眼珠偏移
void drawEye(int16_t cx, int16_t cy, int16_t rx, int16_t ry,
             float open, int16_t dx, int16_t dy, uint16_t eyeColor) {
  int16_t r = (int16_t)(ry * open + 0.5f);

  if (r < 5) {
    // 眯成一条线（眨眼到位的样子）
    face.fillRoundRect(cx - rx, cy - 3, rx * 2, 6, 3, eyeColor);
    return;
  }

  // 眼白（青绿色发光块）
  face.fillEllipse(cx, cy, rx, r, eyeColor);

  // 眼珠：眼睛眯起来时眼珠也变小，避免"眼珠跑出眼眶"
  int16_t pr = (int16_t)(12 * (0.35f + 0.65f * open));
  if (pr < 4) pr = 4;

  int16_t maxDy = r - pr - 1;
  if (maxDy < 0) maxDy = 0;
  int16_t py = cy + dy;
  if (py < cy - maxDy) py = cy - maxDy;
  if (py > cy + maxDy) py = cy + maxDy;

  face.fillCircle(cx + dx, py, pr, C_PUPIL);                     // 瞳孔
  face.fillCircle(cx + dx - pr / 3, py - pr / 2, pr / 3 + 1, C_WHITE); // 高光
}

// 画一帧表情（只画到 face 这个 Sprite 里，还没有推到屏幕）
void drawFaceFrame(uint32_t now) {
  // ---- 呼吸效果：眼睛亮度从 155 到 225 缓慢起伏 ----
  float breath = sinf(now * 0.0012f);
  uint8_t g = (uint8_t)(190 + 35 * breath);
  uint16_t eyeColor = tft.color565(0, g, (uint8_t)(g - 35));

  // ---- 眨眼动画：3.2 秒一次，闭 110ms + 停 80ms + 睁 130ms ----
  float open = 1.0f;
  uint32_t ph = now % BLINK_PERIOD_MS;
  if (ph < BLINK_CLOSE_MS) {
    open = 1.0f - (float)ph / (float)BLINK_CLOSE_MS;
  } else if (ph < BLINK_CLOSE_MS + BLINK_HOLD_MS) {
    open = 0.05f;
  } else if (ph < BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS) {
    open = (float)(ph - BLINK_CLOSE_MS - BLINK_HOLD_MS) / (float)BLINK_OPEN_MS;
  }
  if (open < 0.05f) open = 0.05f;
  if (open > 1.00f) open = 1.00f;

  // ---- 眼珠缓慢左右 / 上下看 ----
  int16_t dx = (int16_t)(9.0f * sinf(now * 0.00085f));
  int16_t dy = (int16_t)(5.0f * sinf(now * 0.00130f + 1.7f));

  face.fillSprite(C_BG);                          // 整个表情层刷成背景色
  face.drawRoundRect(0, 0, FACE_W, FACE_H, 12, C_DIM);   // 一圈淡淡的边框

  drawEye(55,  50, 32, 38, open, dx, dy, eyeColor);      // 左眼
  drawEye(145, 50, 32, 38, open, dx, dy, eyeColor);      // 右眼

  // 嘴：三段折线组成一个小微笑
  face.drawLine(74,  96,  92, 105, C_MOUTH);
  face.drawLine(92, 105, 108, 105, C_MOUTH);
  face.drawLine(108, 105, 126,  96, C_MOUTH);
}

// 底部状态栏：左边运行时间，右边实测 FPS
void drawFooter(uint32_t now) {
  char buf[24];

  if (!footer.created()) return;                 // 内存不足没建成，就不画（不影响表情）
  footer.fillSprite(C_BG);
  footer.drawFastHLine(0, 0, 240, C_DIM);

  uint32_t deci = (now - t_boot) / 100;          // 以 0.1 秒为单位，避免浮点 %
  snprintf(buf, sizeof(buf), "T+%lu.%lus",
           (unsigned long)(deci / 10), (unsigned long)(deci % 10));
  footer.setTextDatum(ML_DATUM);
  footer.setTextColor(C_HEAD, C_BG);
  footer.drawString(buf, 8, 20, 2);

  snprintf(buf, sizeof(buf), "FPS %d", fps);
  footer.setTextDatum(MR_DATUM);
  footer.setTextColor(C_EYE, C_BG);
  footer.drawString(buf, 232, 20, 2);

  footer.pushSprite(0, FOOT_Y);
}

/* ==========================================================================
 *  4. 初始化
 * ==========================================================================*/
void setup() {
  Serial.begin(115200);
  delay(400);                                  // 等串口稳定，免得前面几行看不到

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot v1.0 | 01_display | ST7789 240x320");
  Serial.println("==================================================");

  // ---------- 4.1 上电自检：串口打印"我准备了哪些引脚" ----------
  Serial.println("[OK] 串口正常 (115200)");
  Serial.printf("[OK] 屏幕引脚: SCLK=GPIO%d  MOSI=GPIO%d  CS=GPIO%d  DC=GPIO%d\n",
                PIN_TFT_SCL, PIN_TFT_SDA, PIN_TFT_CS, PIN_TFT_DC);
  Serial.println("[OK] RST->3V3  BLK->3V3  VCC->3V3 (这 3 根不占 GPIO)");
  Serial.printf("[OK] 屏幕规格: %d x %d, SPI %d Hz\n",
                TFT_WIDTH, TFT_HEIGHT, SPI_FREQUENCY);
  Serial.println("[OK] 屏幕检查: 白线/白屏=CS 没接好；花屏=把 SPI 速度降下来");

  // ---------- 4.2 颜色表 ----------
  C_BG    = tft.color565(8, 10, 24);       // 深蓝黑背景
  C_EYE   = tft.color565(0, 220, 190);     // 眼睛 青绿
  C_DIM   = tft.color565(40, 60, 90);      // 暗线
  C_HEAD  = tft.color565(150, 160, 200);   // 灰蓝文字
  C_MOUTH = tft.color565(0, 190, 165);     // 嘴
  C_PUPIL = tft.color565(6, 8, 20);        // 瞳孔（和背景近似的深色）
  C_WHITE = TFT_WHITE;

  // ---------- 4.3 点屏 ----------
  tft.init();
  tft.setRotation(0);                       // 0 = 竖屏 240 宽 x 320 高
  tft.fillScreen(C_BG);
  Serial.println("[OK] TFT_eSPI 初始化完成（白屏请查 User_Setup.h / RST / BLK）");

  // ---------- 4.4 开屏三连：引脚表 -> 色块 -> 标题 ----------
  Serial.println("[OK] 显示引脚表");
  screenSelfTestPins();
  delay(1500);

  Serial.println("[OK] 开始色块测试");
  screenColorBars();

  Serial.println("[OK] 显示标题");
  screenTitle();

  // ---------- 4.5 建 Sprite（内存里的小画布），然后进入表情模式 ----------
  face.setColorDepth(16);
  if (face.createSprite(FACE_W, FACE_H) == nullptr) {
    Serial.println("[FAIL] 表情层内存不足（createSprite 失败），改用直接画屏（会闪）");
    // 极端情况下的兜底：不建 Sprite，直接往屏幕上画
    while (true) {
      tft.fillRect(FACE_X, FACE_Y, FACE_W, FACE_H, C_BG);
      tft.fillEllipse(FACE_X + 55,  FACE_Y + 50, 32, 38, C_EYE);
      tft.fillEllipse(FACE_X + 145, FACE_Y + 50, 32, 38, C_EYE);
      delay(100);
    }
  }

  footer.setColorDepth(16);
  if (footer.createSprite(240, FOOT_H) == nullptr) {
    Serial.println("[FAIL] 状态栏内存不足（createSprite 失败）");
  }

  screenFaceBackdrop();
  t_boot = millis();
  t_lastFrame = t_boot;
  t_lastFooter = t_boot;
  t_fpsMark = t_boot;

  Serial.println("[OK] 进入表情循环（目标 20fps），底部显示运行时间");
  Serial.println("[OK] 每 3.2 秒眨一次眼；眼珠会自己看来看去");
  Serial.println("--------------------------------------------------");
}

/* ==========================================================================
 *  5. 主循环：20fps 刷新表情 + 4Hz 刷新状态栏
 * ==========================================================================*/
void loop() {
  uint32_t now = millis();

  // ---- 表情：每 50ms 更新一帧 ----
  if (now - t_lastFrame >= FRAME_MS) {
    // 按固定节拍走，保证稳定 20fps；如果卡了超过 4 帧（比如串口打印太多），
    // 就重新对表，不要把积压的帧一次性补出来。
    if (now - t_lastFrame > 4 * FRAME_MS) t_lastFrame = now;
    else                                   t_lastFrame += FRAME_MS;

    drawFaceFrame(now);
    face.pushSprite(FACE_X, FACE_Y);       // 整块推上去，而不是一块块擦 → 不闪
    frames++;
  }

  // ---- 每 1 秒统计一次真实帧率 ----
  if (now - t_fpsMark >= 1000) {
    fps = (int)frames;
    frames = 0;
    t_fpsMark = now;
  }

  // ---- 状态栏：每 250ms 刷新一次（时间走字 + FPS） ----
  if (now - t_lastFooter >= FOOTER_MS) {
    t_lastFooter = now;
    drawFooter(now);
  }
}

/* ==========================================================================
 *  6. 中文点阵（自动生成的数据，直接抄过来用，不用看懂）
 *     字体: 微软雅黑 Bold 24px，"桌面机器人" 5 个字
 *     格式: 每个字 24x24 像素，每行 3 字节，高位在左（和 TFT_eSPI 的
 *           drawBitmap() 一致），共 24 行 x 3 字节 = 72 字节/字
 *     想换别的字：可以保留这个格式自己生成，或者上网搜 "PCtoLCD2002 字模软件"，
 *     按"阴码 + 逐行式 + 高位在前"导出 24x24 点阵即可。
 * ==========================================================================*/
const uint8_t CN_GLYPHS[5][72] = {
  { // 桌
    0x00, 0x38, 0x00,
    0x00, 0x3F, 0xFC,
    0x00, 0x3F, 0xFE,
    0x00, 0x3F, 0xFE,
    0x00, 0x3D, 0x04,
    0x1F, 0xFF, 0xF8,
    0x1F, 0xFF, 0xF8,
    0x1F, 0xFF, 0xF8,
    0x1E, 0x00, 0x78,
    0x1F, 0xFF, 0xF8,
    0x1F, 0xFF, 0xF8,
    0x1E, 0x00, 0x78,
    0x1F, 0xFF, 0xF8,
    0x1F, 0xFF, 0xF8,
    0x1F, 0xFF, 0xF8,
    0x7F, 0xFF, 0xFE,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x03, 0xFF, 0xC0,
    0x0F, 0xFF, 0xF8,
    0xFF, 0xBD, 0xFF,
    0xFE, 0x3C, 0x7E,
    0x78, 0x3C, 0x1E,
    0x00, 0x3C, 0x00,
  },
  { // 面
    0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x00, 0x78, 0x00,
    0x00, 0xF8, 0x00,
    0x3F, 0xFF, 0xFC,
    0x3F, 0xFF, 0xFC,
    0x3F, 0xFF, 0xFC,
    0x39, 0xC3, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xC3, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xFF, 0x9C,
    0x39, 0xC3, 0x9C,
    0x3F, 0xFF, 0xFC,
    0x3F, 0xFF, 0xFC,
    0x3F, 0xFF, 0xFC,
    0x38, 0x00, 0x1C,
    0x00, 0x00, 0x00,
  },
  { // 机
    0x1E, 0x00, 0x00,
    0x1E, 0x3F, 0xF0,
    0x1E, 0x3F, 0xF0,
    0x1E, 0x3F, 0xF0,
    0xFF, 0xBF, 0xF0,
    0xFF, 0xBC, 0xF0,
    0xFF, 0xBC, 0xF0,
    0xFF, 0xBC, 0xF0,
    0x1E, 0x3C, 0xF0,
    0x3E, 0x3C, 0xF0,
    0x3F, 0x3C, 0xF0,
    0x3F, 0xFC, 0xF0,
    0x7F, 0xFC, 0xF0,
    0xFF, 0xBC, 0xF0,
    0xFF, 0x3C, 0xF6,
    0xFE, 0x7C, 0xF7,
    0xFE, 0x78, 0xF7,
    0x5E, 0x78, 0xF7,
    0x1E, 0xF8, 0xF7,
    0x1E, 0xF8, 0xFF,
    0x1F, 0xF0, 0xFF,
    0x1F, 0xE0, 0xFE,
    0x1E, 0xE0, 0x7E,
    0x1C, 0x40, 0x00,
  },
  { // 器
    0x00, 0x00, 0x00,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x38, 0xE7, 0x1C,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xFF, 0xFC,
    0x00, 0x3C, 0xF0,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x01, 0xFF, 0x80,
    0xFF, 0xE7, 0xFF,
    0xFF, 0xC3, 0xFF,
    0xFF, 0xE7, 0xFE,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x38, 0xE7, 0x1C,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x3F, 0xE7, 0xFC,
    0x38, 0xE7, 0x1C,
    0x00, 0x00, 0x00,
  },
  { // 人
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x3C, 0x00,
    0x00, 0x7C, 0x00,
    0x00, 0x7C, 0x00,
    0x00, 0x7E, 0x00,
    0x00, 0x7E, 0x00,
    0x00, 0xFF, 0x00,
    0x00, 0xFF, 0x00,
    0x01, 0xFF, 0x80,
    0x03, 0xE7, 0x80,
    0x03, 0xE7, 0xC0,
    0x07, 0xC3, 0xE0,
    0x0F, 0xC3, 0xF0,
    0x3F, 0x81, 0xFC,
    0x7F, 0x00, 0xFF,
    0xFE, 0x00, 0x7F,
    0x7C, 0x00, 0x3E,
    0x38, 0x00, 0x1C,
    0x00, 0x00, 0x00,
  },
};
