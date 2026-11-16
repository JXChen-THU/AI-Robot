#include "pinmap.h"

/* ============================================================================
 *  03_servo.ino  ——  2 自由度云台（SG90 x2）：回中 + 平滑扫动
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    1) 用 ESP32Servo 库驱动两个 SG90 舵机：一个管水平转头（PAN），一个管抬头低头（TILT）；
 *    2) 上电先把两个舵机回中位（90°, 90°），让云台"眼对前方"；
 *    3) 然后水平在 30°~150°、俯仰在 45°~135° 之间缓慢平滑地扫动（每 20ms 只走 1°），
 *       速度慢、电流小，不会把 5V 拉塌，也不会让底盘"点头"；
 *    4) 留了 SERVO_PAN_INVERT / SERVO_TILT_INVERT 两个开关：装反了改 1 即可，不用拆舵机。
 *
 *  【接线】（详见 docs\01_接线总表.md 第 3.6 节）
 *      水平舵机(PAN)  棕/黑(-) → GND
 *                     红(+)    → 5V     （★ 一定 5V，接 3V3 带不动会抖）
 *                     橙/黄(S) → GPIO 18 (PIN_SERVO_PAN)
 *      俯仰舵机(TILT) 棕/黑(-) → GND
 *                     红(+)    → 5V
 *                     橙/黄(S) → GPIO 48 (PIN_SERVO_TILT)
 *    两个舵机的地线最好各拉一根直接回电池座 GND，不要串在别的模块后面。
 *    （GPIO48 在部分开发板上和板载 RGB 灯共用，扫动时 RGB 灯乱闪是正常现象，不影响功能。）
 *
 *  【预期现象】
 *    - 串口打印 [OK] 引脚表和扫动范围；
 *    - 上电时两个舵机先"甩"到 90°/90° 中位（SG90 没有掉电记忆，这一步跳一下是正常的）；
 *    - 然后云台按 左上 → 左下 → 右下 → 右上 → 回中 的顺序平稳转圈，每段约 2 秒；
 *    - 全程没有猛甩、没有"咔咔"声；串口每换一个角打印一行 [OK]。
 *
 *  【常见错误】
 *    - 舵机只"嗡嗡"抖、不转：红线接到了 3V3（必须 5V），或 5V 电流不够。
 *    - 舵机一转，ESP32 就重启：两个舵机同时启动电流可达 1.4A，5V 被拉塌。
 *      → 加 470µF 电容到舵机附近、用足量电池；把 SERVO_STEP_MS 从 20 改成 30 更慢一些。
 *    - 舵机转到头"咔咔"响、发热：扫动范围设太大，SG90 实际只有约 180°，
 *      本代码已经限制在安全区间（30~150 / 45~135），不要随意改成 0~180。
 *    - 方向装反：改 SERVO_PAN_INVERT / SERVO_TILT_INVERT 为 1，不用拆线。
 *    - 编译提示找不到 Servo：没装库。库管理器搜 "ESP32Servo"（Kevin Harrington 那版）
 *      安装 3.x 版本（老版本 0.x/1.x 在 ESP32 core 3.x 下会编译不过）。
 * ==========================================================================*/

#include <ESP32Servo.h>             // 舵机库（Arduino 官方 Servo 库不支持 ESP32 的 LEDC/MCPWM）

/* ==========================================================================
 *  1. 可调参数（小白只需要改这一小块）
 * ==========================================================================*/
#define SERVO_PAN_INVERT     0    // 水平转反了 → 改成 1
#define SERVO_TILT_INVERT    0    // 俯仰转反了 → 改成 1

#define SERVO_CENTER_DEG    90    // 中位角度
#define SERVO_PAN_MIN       30    // 水平扫动范围
#define SERVO_PAN_MAX      150
#define SERVO_TILT_MIN      45    // 俯仰扫动范围
#define SERVO_TILT_MAX     135

#define SERVO_STEP_DEG       1    // 每一步走 1 度
#define SERVO_STEP_MS       20    // 每 20ms 走一步（= 50°/秒，120° 要 2.4 秒，够慢）
// 如果两个舵机同时动的时候主控会重启，把上面 20 改成 30（更慢、电流更小）再试

#define SERVO_CENTER_SETTLE_MS 600  // 回中后等它到位的时间

/* ==========================================================================
 *  2. 对象与状态
 * ==========================================================================*/
Servo panServo;                    // 水平
Servo tiltServo;                   // 俯仰

static int s_panDeg  = SERVO_CENTER_DEG;   // 逻辑角度（还没做反相）
static int s_tiltDeg = SERVO_CENTER_DEG;

/* ==========================================================================
 *  3. 底层：逻辑角度 → 舵机实际角度（含反相开关）
 * ==========================================================================*/
static void writePan(int logicalDeg) {
  int hw = (SERVO_PAN_INVERT) ? (180 - logicalDeg) : logicalDeg;  // 反相 = 180 减一下
  panServo.write(constrain(hw, 0, 180));
}

static void writeTilt(int logicalDeg) {
  int hw = (SERVO_TILT_INVERT) ? (180 - logicalDeg) : logicalDeg;
  tiltServo.write(constrain(hw, 0, 180));
}

// 朝目标角挪一步（最多 SERVO_STEP_DEG 度），到了就停住
static int stepTowardDeg(int cur, int target) {
  if (cur < target) {
    cur += SERVO_STEP_DEG;
    if (cur > target) cur = target;
  } else if (cur > target) {
    cur -= SERVO_STEP_DEG;
    if (cur < target) cur = target;
  }
  return cur;
}

// 两个舵机同时、平滑地走到指定角度（阻塞式，一步一步走）
static void moveBothTo(int panTarget, int tiltTarget) {
  panTarget  = constrain(panTarget,  0, 180);
  tiltTarget = constrain(tiltTarget, 0, 180);

  while (s_panDeg != panTarget || s_tiltDeg != tiltTarget) {
    s_panDeg  = stepTowardDeg(s_panDeg,  panTarget);
    s_tiltDeg = stepTowardDeg(s_tiltDeg, tiltTarget);
    writePan(s_panDeg);
    writeTilt(s_tiltDeg);
    delay(SERVO_STEP_MS);
  }
}

/* ==========================================================================
 *  4. 初始化
 * ==========================================================================*/
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot v1.0 | 03_servo | SG90 x2 pan/tilt");
  Serial.println("==================================================");

  // ---------- 4.1 串口自检 ----------
  Serial.println("[OK] 串口正常 (115200)");
  Serial.printf("[OK] 水平舵机 PAN  信号 = GPIO%d\n", PIN_SERVO_PAN);
  Serial.printf("[OK] 俯仰舵机 TILT 信号 = GPIO%d\n", PIN_SERVO_TILT);
  Serial.println("[OK] 两个舵机红线接 5V、棕/黑线接 GND（千万别接 3V3）");
  Serial.printf("[OK] 舵机参数: %dHz, 脉宽 %d~%d us (= 0~180 度)\n",
                SERVO_FREQ, SERVO_MIN_US, SERVO_MAX_US);
  Serial.printf("[OK] 扫动范围: 水平 %d~%d 度, 俯仰 %d~%d 度\n",
                SERVO_PAN_MIN, SERVO_PAN_MAX, SERVO_TILT_MIN, SERVO_TILT_MAX);
  Serial.printf("[OK] 转向修正: PAN_INVERT=%d  TILT_INVERT=%d\n",
                SERVO_PAN_INVERT, SERVO_TILT_INVERT);

  // ---------- 4.2 绑定舵机 ----------
  panServo.setPeriodHertz(SERVO_FREQ);    // 标准 50Hz
  tiltServo.setPeriodHertz(SERVO_FREQ);
  panServo.attach(PIN_SERVO_PAN,  SERVO_MIN_US, SERVO_MAX_US);
  tiltServo.attach(PIN_SERVO_TILT, SERVO_MIN_US, SERVO_MAX_US);

  if (!panServo.attached() || !tiltServo.attached()) {
    Serial.println("[FAIL] 舵机绑定失败：PWM 通道不够或被别的功能占用，重启试试");
    while (true) delay(1000);
  }
  Serial.println("[OK] 两个舵机已绑定");

  // ---------- 4.3 回中位 ----------
  // 说明：SG90 掉电后不记得自己在哪，所以第一次给中位时一定会"甩"一下，
  //       这是正常现象；下面所有扫动才是真正的"慢速"运动。
  s_panDeg  = SERVO_CENTER_DEG;
  s_tiltDeg = SERVO_CENTER_DEG;
  writePan(s_panDeg);
  writeTilt(s_tiltDeg);
  Serial.println("[OK] 已发送中位 90°/90°，等待舵机到位...");
  delay(SERVO_CENTER_SETTLE_MS);
  Serial.println("[OK] 开始平滑扫动（每 20ms 走 1 度）");
  Serial.println("--------------------------------------------------");
}

/* ==========================================================================
 *  5. 主循环：矩形扫一圈，回中，再来
 * ==========================================================================*/
void loop() {
  Serial.printf("[OK] -> 左上角: 水平 %d 度 / 俯仰 %d 度\n", SERVO_PAN_MIN, SERVO_TILT_MIN);
  moveBothTo(SERVO_PAN_MIN, SERVO_TILT_MIN);
  delay(300);

  Serial.printf("[OK] -> 左下角: 水平 %d 度 / 俯仰 %d 度\n", SERVO_PAN_MIN, SERVO_TILT_MAX);
  moveBothTo(SERVO_PAN_MIN, SERVO_TILT_MAX);
  delay(300);

  Serial.printf("[OK] -> 右下角: 水平 %d 度 / 俯仰 %d 度\n", SERVO_PAN_MAX, SERVO_TILT_MAX);
  moveBothTo(SERVO_PAN_MAX, SERVO_TILT_MAX);
  delay(300);

  Serial.printf("[OK] -> 右上角: 水平 %d 度 / 俯仰 %d 度\n", SERVO_PAN_MAX, SERVO_TILT_MIN);
  moveBothTo(SERVO_PAN_MAX, SERVO_TILT_MIN);
  delay(300);

  Serial.printf("[OK] -> 回中位: 水平 %d 度 / 俯仰 %d 度\n", SERVO_CENTER_DEG, SERVO_CENTER_DEG);
  moveBothTo(SERVO_CENTER_DEG, SERVO_CENTER_DEG);
  delay(800);
}
