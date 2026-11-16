#include "pinmap.h"

/* ============================================================================
 *  02_motor.ino  ——  TB6612FNG 驱动 4 个 TT 电机（方向脚直接吃 PWM 调速）
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    1) 用 TB6612 让 4 个 TT 减速电机（左 2 个并联、右 2 个并联）正常前进/后退；
 *    2) 演示差速转弯：原地左转 / 原地右转；
 *    3) ★ 特殊设计：TB6612 的 PWMA / PWMB 在硬件上直接接死 3V3（一直 100% 使能），
 *       所以用 AIN1/AIN2/BIN1/BIN2 四个"方向脚"直接输出 PWM 来调速——
 *       前进：AIN1=PWM, AIN2=0；后退：AIN1=0, AIN2=PWM；
 *       两脚都 0 = 滑行（断电，轮子自己滚）；两脚都 1 = 刹车（电机绕组短接）。
 *       （这样能少用 2 个引脚，本板 30 个 GPIO 一个都不剩，详见 docs\01 第 1.3 节）
 *    4) ★ 提供 motorSet(leftPerMille, rightPerMille) 接口（-1000 ~ +1000 千分比），
 *       内部带"加速度限制软启动"：每 5ms 最多变化 20‰。
 *       为什么？电机启动瞬间电流是平时的 5~10 倍，直接给满速会让 5V 被拉塌、
 *       ESP32 掉电重启（现象是"电机一动主控就重启"）。软启动就是为了避免这个。
 *    5) 上电自动跑测试序列：前进 2s → 停 → 后退 2s → 停 → 原地左转 → 原地右转 → 停。
 *
 *  【接线】（详见 docs\01_接线总表.md 第 3.5 节）
 *      TB6612 VM    → 5V            （★ 一定不要接电池 7.4V，TT 电机额定 3~6V）
 *      TB6612 VCC   → 3V3           （逻辑电源）
 *      TB6612 GND   → GND           （必须共地）
 *      TB6612 STBY  → 3V3           （拉高才工作）
 *      TB6612 PWMA  → 3V3           （本项目"方向脚吃 PWM"，PWMA/PWMB 拉满）
 *      TB6612 PWMB  → 3V3
 *      TB6612 AIN1  → GPIO 1        (PIN_MOTOR_AIN1)
 *      TB6612 AIN2  → GPIO 2        (PIN_MOTOR_AIN2)
 *      TB6612 BIN1  → GPIO 45       (PIN_MOTOR_BIN1)
 *      TB6612 BIN2  → GPIO 46       (PIN_MOTOR_BIN2)
 *      AOUT1/AOUT2  → 左侧两个电机（红接红、黑接黑，并联）
 *      BOUT1/BOUT2  → 右侧两个电机（同上）
 *    5V 母排上请并一个 1000µF 电解电容（正极接 5V、负极接 GND），
 *    它是电机启动瞬间的"蓄水池"，没有它很容易重启。
 *
 *  【预期现象】
 *    - 串口打印 [OK] 引脚表和参数；
 *    - 车（建议先架空）按顺序：前进 → 停 → 后退 → 停 → 原地左转 → 原地右转 → 停；
 *    - 每次启动/换向都是"慢慢加速"，没有"咔"的一下猛冲；
 *    - 串口每轮结束打印 [OK] 一轮测试结束，3 秒后自动再来一遍。
 *
 *  【常见错误】
 *    - 一个方向能转、另一个方向不转：PWMA/PWMB 没接到 3V3（这是最常见的）。
 *    - 轮子转向和预期相反：不用重插线！把下面 MOTOR_LEFT_INVERT / MOTOR_RIGHT_INVERT
 *      改成 1，重新烧录即可（左边反了就改 LEFT，右边反了改 RIGHT）。
 *    - 电机一动就重启/串口断：5V 供电不够 → 加 1000µF 电容、用足量电池、
 *      把 MOTOR_MAX_PERMILLE 从 700 降到 500 再试。
 *    - 电机只抖不转：BIN1/BIN2 是 GPIO45/46（启动脚），如果接错到别的脚会抖；
 *      另外检查电池电压是否低于 6V。
 *    - 上电瞬间轮子抽一下：GPIO45/46 是启动配置脚，上电瞬间电平由板子决定，
 *      TB6612 内部下拉会让电机保持静止，抽一下属正常，不会真跑。
 * ==========================================================================*/

#include <esp_arduino_version.h>   // 用来判断 ESP32 Arduino core 是 2.x 还是 3.x

/* ==========================================================================
 *  1. 可调参数（小白只需要改这一小块）
 * ==========================================================================*/
#define MOTOR_LEFT_INVERT    0   // 左轮转向反了 → 改成 1
#define MOTOR_RIGHT_INVERT   0   // 右轮转向反了 → 改成 1

#define MOTOR_MAX_PERMILLE 700   // 最高只给 70% 占空比（保护 5V 电源，见 docs\01 第 1.3 节第 6 条）
#define MOTOR_RAMP_STEP     20   // 软启动：每 5ms 最多变化 20‰（=每秒最多 4000‰，0.25 秒到满速）
#define MOTOR_RAMP_MS        5   // 软启动的时间刻度

#define MOTOR_TEST_SPEED   600   // 自动测试时的前进/后退速度（600‰ = 60%）
#define MOTOR_TURN_SPEED   450   // 原地转弯速度

/* ==========================================================================
 *  2. LEDC(PWM) 两套 API 的兼容层
 *     - ESP32 Arduino core 3.x：ledcAttach(pin, freq, res) / ledcWrite(pin, duty)
 *     - ESP32 Arduino core 2.x：ledcSetup(ch, freq, res) + ledcAttachPin(pin, ch)
 *                              / ledcWrite(ch, duty)
 *     用 ESP_ARDUINO_VERSION_MAJOR 自动判断，你不需要改任何东西。
 * ==========================================================================*/
#if ESP_ARDUINO_VERSION_MAJOR < 3
  // ---- core 2.x：以"通道号"为句柄 ----
  #define MOTOR_LEDC_ATTACH(pin, ch) \
    (ledcSetup((ch), MOTOR_PWM_FREQ, MOTOR_PWM_RES) > 0 ? (ledcAttachPin((pin), (ch)), true) : false)
  #define MOTOR_LEDC_WRITE(pin, ch, duty) ledcWrite((ch), (duty))
#else
  // ---- core 3.x：以"引脚号"为句柄 ----
  #define MOTOR_LEDC_ATTACH(pin, ch)      ledcAttach((pin), MOTOR_PWM_FREQ, MOTOR_PWM_RES)
  #define MOTOR_LEDC_WRITE(pin, ch, duty) ledcWrite((pin), (duty))
#endif

// core 2.x 需要自己挑通道号；core 3.x 会自动分配（这两个宏在 3.x 下不使用）。
// 选 4~7 号通道，把 0~3 号留给舵机等其它 PWM 外设，以后合并代码不会打架。
#define MOTOR_CH_AIN1 4
#define MOTOR_CH_AIN2 5
#define MOTOR_CH_BIN1 6
#define MOTOR_CH_BIN2 7

/* ==========================================================================
 *  3. 内部状态
 * ==========================================================================*/
static int      s_targetL   = 0;   // 目标速度（‰，已做转向修正和限幅）
static int      s_targetR   = 0;
static int      s_curL      = 0;   // 当前真实输出的速度（‰），会一点点追上目标
static int      s_curR      = 0;
static uint32_t s_lastRamp  = 0;   // 上一次做加速度计算的时刻

/* ==========================================================================
 *  4. 底层：把 -1000 ~ +1000 变成四个方向脚上的 PWM
 * ==========================================================================*/
static void motorWrite(int left, int right) {
  int aL = (left  < 0) ? -left  : left;
  int aR = (right < 0) ? -right : right;
  uint32_t dutyL = (uint32_t)aL * (uint32_t)MOTOR_PWM_MAX / 1000UL;
  uint32_t dutyR = (uint32_t)aR * (uint32_t)MOTOR_PWM_MAX / 1000UL;

  // ---- 左轮：A 通道 ----
  if (left > 0) {                                   // 前进：AIN1 = PWM, AIN2 = 0
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN1, MOTOR_CH_AIN1, dutyL);
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN2, MOTOR_CH_AIN2, 0);
  } else if (left < 0) {                            // 后退：AIN1 = 0, AIN2 = PWM
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN1, MOTOR_CH_AIN1, 0);
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN2, MOTOR_CH_AIN2, dutyL);
  } else {                                          // 0：两脚都 0 = 滑行（断电）
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN1, MOTOR_CH_AIN1, 0);
    MOTOR_LEDC_WRITE(PIN_MOTOR_AIN2, MOTOR_CH_AIN2, 0);
  }

  // ---- 右轮：B 通道 ----
  if (right > 0) {
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN1, MOTOR_CH_BIN1, dutyR);
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN2, MOTOR_CH_BIN2, 0);
  } else if (right < 0) {
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN1, MOTOR_CH_BIN1, 0);
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN2, MOTOR_CH_BIN2, dutyR);
  } else {
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN1, MOTOR_CH_BIN1, 0);
    MOTOR_LEDC_WRITE(PIN_MOTOR_BIN2, MOTOR_CH_BIN2, 0);
  }
}

// 把 cur 每次朝 target 挪一步（最多挪 step），到了就停住
static int stepToward(int cur, int target, int step) {
  if (cur < target) {
    cur += step;
    if (cur > target) cur = target;
  } else if (cur > target) {
    cur -= step;
    if (cur < target) cur = target;
  }
  return cur;
}

/* ==========================================================================
 *  5. 对外接口
 * ==========================================================================*/

// 设置左右轮目标速度，-1000 ~ +1000（千分比）。正数=前进，负数=后退。
// 注意：这里只记下目标，真正的加速过程由 motorUpdate() 在 loop() 里分步完成，
//       所以 loop() 必须不停地调用 motorUpdate()（本文件已经写好了）。
void motorSet(int leftPerMille, int rightPerMille) {
  int l = constrain(leftPerMille,  -1000, 1000);
  int r = constrain(rightPerMille, -1000, 1000);

  // 转向修正：用户不用重插线，改上面的宏就行
  if (MOTOR_LEFT_INVERT)  l = -l;
  if (MOTOR_RIGHT_INVERT) r = -r;

  // 限幅保护电源
  if (l >  MOTOR_MAX_PERMILLE) l =  MOTOR_MAX_PERMILLE;
  if (l < -MOTOR_MAX_PERMILLE) l = -MOTOR_MAX_PERMILLE;
  if (r >  MOTOR_MAX_PERMILLE) r =  MOTOR_MAX_PERMILLE;
  if (r < -MOTOR_MAX_PERMILLE) r = -MOTOR_MAX_PERMILLE;

  s_targetL = l;
  s_targetR = r;
}

// 必须每圈都调用！它负责"每 5ms 最多变 20‰"的软启动 / 软减速。
void motorUpdate(void) {
  uint32_t now = millis();
  uint32_t dt  = now - s_lastRamp;
  if (dt < MOTOR_RAMP_MS) return;

  uint32_t steps = dt / MOTOR_RAMP_MS;      // 这次该走几步（掉帧时补上）
  if (steps > 4) steps = 4;                 // 最多补 4 步(=20ms)，防止主程序卡顿后突然猛加速
  s_lastRamp += steps * MOTOR_RAMP_MS;

  int beforeL = s_curL;
  int beforeR = s_curR;
  while (steps--) {
    s_curL = stepToward(s_curL, s_targetL, MOTOR_RAMP_STEP);
    s_curR = stepToward(s_curR, s_targetR, MOTOR_RAMP_STEP);
    if (s_curL == s_targetL && s_curR == s_targetR) break;
  }

  if (s_curL != beforeL || s_curR != beforeR) motorWrite(s_curL, s_curR);
}

// 正常停止：目标设 0，靠 motorUpdate() 平滑减到 0（滑行停，轮子会再滚一小段）。
// 想"立刻"停见 motorCoastNow() / motorBrake()。
void motorStop(void) {
  s_targetL = 0;
  s_targetR = 0;
}

// 立刻断电滑行：直接写 0，不等减速过程。紧急情况用（省电、不伤电机）。
void motorCoastNow(void) {
  s_targetL = 0;
  s_targetR = 0;
  s_curL    = 0;
  s_curR    = 0;
  motorWrite(0, 0);
  s_lastRamp = millis();
}

// 立刻刹车：两个方向脚同时拉高 → TB6612 把电机绕组短接 → 转子被"抱住"。
// 比滑行停得快得多，但会有一点机械冲击，正常停车还是建议 motorStop()。
// ⚠ 只在【低速或已经停住】时用。高速时直接刹车会让电流瞬间冲高，5V 容易被拉塌、
//   主控重启（重则伤驱动板）。高速要停，请先 motorStop() 让它减速，再刹车。
void motorBrake(void) {
  s_targetL = 0;
  s_targetR = 0;
  s_curL    = 0;
  s_curR    = 0;
  MOTOR_LEDC_WRITE(PIN_MOTOR_AIN1, MOTOR_CH_AIN1, MOTOR_PWM_MAX);
  MOTOR_LEDC_WRITE(PIN_MOTOR_AIN2, MOTOR_CH_AIN2, MOTOR_PWM_MAX);
  MOTOR_LEDC_WRITE(PIN_MOTOR_BIN1, MOTOR_CH_BIN1, MOTOR_PWM_MAX);
  MOTOR_LEDC_WRITE(PIN_MOTOR_BIN2, MOTOR_CH_BIN2, MOTOR_PWM_MAX);
  s_lastRamp = millis();
}

/* ==========================================================================
 *  6. 初始化 + 自动测试
 * ==========================================================================*/

// 一边等时间，一边不停喂 motorUpdate()，让软启动正常走
static void pumpMotor(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    motorUpdate();
    delay(1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot v1.0 | 02_motor | TB6612 x4 TT motor");
  Serial.println("==================================================");

  // ---------- 6.1 串口自检：引脚表 ----------
  Serial.println("[OK] 串口正常 (115200)");
  Serial.printf("[OK] 电机方向脚: AIN1=GPIO%d  AIN2=GPIO%d  BIN1=GPIO%d  BIN2=GPIO%d\n",
                PIN_MOTOR_AIN1, PIN_MOTOR_AIN2, PIN_MOTOR_BIN1, PIN_MOTOR_BIN2);
  Serial.println("[OK] PWMA=3V3  PWMB=3V3  STBY=3V3  （方向脚直接输出 PWM）");
  Serial.println("[OK] 左侧=两个左轮并联(A 通道)，右侧=两个右轮并联(B 通道)");
  Serial.printf("[OK] PWM: %d Hz / %d 位 / 0~%d，最高限 %d‰\n",
                MOTOR_PWM_FREQ, MOTOR_PWM_RES, MOTOR_PWM_MAX, MOTOR_MAX_PERMILLE);
  Serial.printf("[OK] 软启动: 每 %dms 最多变化 %d‰\n", MOTOR_RAMP_MS, MOTOR_RAMP_STEP);
  Serial.printf("[OK] 转向修正: LEFT_INVERT=%d  RIGHT_INVERT=%d\n",
                MOTOR_LEFT_INVERT, MOTOR_RIGHT_INVERT);
  Serial.println("[!] 第一次测试请把车架空（轮子不着地），防止接错线冲出去");

  // ---------- 6.2 初始化 4 路 PWM ----------
  bool ok = true;
  if (!MOTOR_LEDC_ATTACH(PIN_MOTOR_AIN1, MOTOR_CH_AIN1)) ok = false;
  if (!MOTOR_LEDC_ATTACH(PIN_MOTOR_AIN2, MOTOR_CH_AIN2)) ok = false;
  if (!MOTOR_LEDC_ATTACH(PIN_MOTOR_BIN1, MOTOR_CH_BIN1)) ok = false;
  if (!MOTOR_LEDC_ATTACH(PIN_MOTOR_BIN2, MOTOR_CH_BIN2)) ok = false;

  if (!ok) {
    Serial.println("[FAIL] PWM(LEDC) 初始化失败：引脚被占用或通道不够，检查接线后重启");
    while (true) delay(1000);
  }
  Serial.println("[OK] 4 路 PWM 初始化完成");

  // 先保证四个方向脚都是低电平 → 电机静止（安全第一）
  motorCoastNow();
  Serial.println("[OK] 电机已置为静止状态");
  Serial.println("[OK] 1.5 秒后开始自动测试序列（请确认车已架空）");
  delay(1500);
}

void loop() {
  Serial.println();
  Serial.println("---- 自动测试开始 ----");

  Serial.println("[OK] 1/6 前进 2 秒（左 +600‰ / 右 +600‰）");
  motorSet(MOTOR_TEST_SPEED, MOTOR_TEST_SPEED);
  pumpMotor(2000);

  Serial.println("[OK] 2/6 滑行停止（软减速 0.7 秒）");
  motorStop();
  pumpMotor(700);

  Serial.println("[OK] 3/6 后退 2 秒（左 -600‰ / 右 -600‰）");
  motorSet(-MOTOR_TEST_SPEED, -MOTOR_TEST_SPEED);
  pumpMotor(2000);
  motorStop();
  pumpMotor(700);

  Serial.println("[OK] 4/6 原地左转（左轮后退 / 右轮前进）");
  motorSet(-MOTOR_TURN_SPEED, MOTOR_TURN_SPEED);
  pumpMotor(1500);
  motorStop();
  pumpMotor(700);

  Serial.println("[OK] 5/6 原地右转（左轮前进 / 右轮后退）");
  motorSet(MOTOR_TURN_SPEED, -MOTOR_TURN_SPEED);
  pumpMotor(1500);
  motorStop();
  pumpMotor(700);

  Serial.println("[OK] 6/6 刹车演示（两脚同时拉高 = 电机绕组短接）");
  motorBrake();
  delay(800);
  motorCoastNow();

  Serial.println("[OK] 一轮测试结束，3 秒后自动重复");
  Serial.printf("[OK] 实测: 加速 %d‰ 用时约 %lu ms\n",
                MOTOR_TEST_SPEED,
                (unsigned long)(MOTOR_TEST_SPEED / MOTOR_RAMP_STEP) * MOTOR_RAMP_MS);
  delay(3000);
}
