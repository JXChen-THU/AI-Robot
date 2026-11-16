#include "pinmap.h"

/* ============================================================================
 *  06_sonar.ino  ——  HC-SR04 超声波测距（5 次中值滤波 + 超时保护）
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    1) 用 TRIG/ECHO 两个脚测距，串口打印厘米数；
 *    2) ★ 单次测量超时 30ms（30ms 对应约 5 米），收不到回波就打印 [FAIL] 超时，
 *       ★ 绝不死等 —— 用的是 pulseIn(pin, HIGH, 30000) 这个"带超时"的版本；
 *    3) 连续测 5 次，去掉明显错误的值后取中值（中值滤波），数字更稳、不跳；
 *    4) 打印一个 40 格的简易条形图，把手放在前面就能看到条形图变长变短；
 *    5) 顺便演示给后面的"避障"用：距离 < 15cm 就提示"太近，要停"。
 *
 *  【接线】（详见 docs\01_接线总表.md 第 3.7 节）
 *      HC-SR04 VCC  → 3V3        （★ 宽压版可以；如果非要用 5V 供电，
 *                                  ECHO 必须先 1kΩ+2kΩ 分压，否则 5V 打进 ESP32 会烧脚）
 *      HC-SR04 GND  → GND
 *      HC-SR04 TRIG → GPIO 19    (PIN_SONAR_TRIG)
 *      HC-SR04 ECHO → GPIO 20    (PIN_SONAR_ECHO)
 *    ⚠ GPIO19/20 原来是 ESP32-S3 原生 USB 的 D-/D+。改成普通 GPIO 以后：
 *      - 烧录和看串口只用【UART 口】那颗 Type-C；
 *      - 【USB 口】不要插，插了会和超声波抢这两根线。
 *
 *  【预期现象】
 *    - 串口打印 [OK] 引脚表和提示"把手放在前面"；
 *    - 每约 0.5 秒打印一组：5 次采样 -> 中值厘米数 -> 条形图（单位格 5cm，满格 200cm）；
 *    - 手离探头 10cm：约 10 cm，条形图很短；手后退到 80cm：约 80 cm，条形图变长；
 *    - 前面什么都没有（>5 米）：打印 [FAIL] 超时，程序继续跑，不会卡死。
 *
 *  【常见错误】
 *    - 一直 [FAIL] 超时：① ECHO/TRIG 插反了；② VCC 没接或接错；③ 没共地；
 *      ④ 目标太远（>4~5 米）或太斜；⑤ 探头前面有布/海绵等吸音材料。
 *    - 数字乱跳：超声波本来就有 ±1cm 抖动，中值滤波已经帮你去掉大部分；
 *      再稳不了就在读数和目标之间保持"正对、无遮挡"。
 *    - ⚠ 第一次上电前强烈建议：万用表直流档量一下 ECHO 对 GND 的最高电压。
 *      正常应为 ~3.3V；如果量到 ~5V（说明买到的不是宽压版，或者 VCC 接到了 5V），
 *      必须先把 ECHO 用 1kΩ+2kΩ 分压再接到 GPIO20，否则会打穿引脚。
 *    - 插了 USB 口导致连不上/读数乱：拔掉 USB 口，只用 UART 口。
 *    - 烧录时提示串口被占用：先把 TRIG/ECHO 两根线拔掉再烧，烧完插回（可选，一般不用）。
 * ==========================================================================*/

/* ==========================================================================
 *  1. 可调参数
 * ==========================================================================*/
#define SONAR_TIMEOUT_US   30000UL   // 单次测量超时：30000us ≈ 5 米，超了就放弃
#define SONAR_SAMPLES          5     // 每次测 5 下
#define SONAR_SAMPLE_GAP_MS   60     // 两次测量之间隔 60ms（太近会收到上一次的回波，串扰）
#define SONAR_MIN_CM           2     // 小于 2cm 视为无效（贴着探头测不准）
#define SONAR_MAX_CM         450     // 大于 4.5m 视为无效（超出量程）
#define SONAR_BAR_MAX_CM     200     // 条形图满格代表 200cm
#define SONAR_BAR_WIDTH       40     // 条形图 40 格
#define SONAR_NEAR_CM         15     // 小于 15cm 提示"太近"

/* ==========================================================================
 *  2. 单次测距：返回厘米；超时/无效返回 -1
 *     ★ 关键：pulseIn 的第三个参数是超时时间(微秒)。
 *       pulseIn 不带第三个参数时默认等 1 秒，遇到没回波就会僵住 1 秒，
 *       整个程序像死机一样。这里必须带上 30000。
 * ==========================================================================*/
long sonarOnceCm(void) {
  // 给 TRIG 一个 10us 以上的高电平，HC-SR04 就会发 8 个 40kHz 脉冲
  digitalWrite(PIN_SONAR_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_SONAR_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_SONAR_TRIG, LOW);

  // 等 ECHO 变成高电平，并测量高电平持续多久（微秒）
  unsigned long us = pulseIn(PIN_SONAR_ECHO, HIGH, SONAR_TIMEOUT_US);

  if (us == 0) return -1;                    // 超时：没收到回波
  // 声速约 343m/s = 0.0343cm/us，一来一回要除以 2 -> 约 us/58
  // 用整数算：us * 17 / 1000（误差 < 0.1cm，够用且不会溢出）
  return (long)((us * 17UL) / 1000UL);
}

/* ==========================================================================
 *  3. 连续测 5 次，取中值
 *     返回中值厘米数；全部无效返回 -1；validCount 带回"几次是有效值"
 * ==========================================================================*/
long sonarMedianCm(int *validCount) {
  int raw[SONAR_SAMPLES];

  // ---- 3.1 连测 5 次 ----
  for (int i = 0; i < SONAR_SAMPLES; i++) {
    long cm = sonarOnceCm();
    raw[i] = (cm < 0) ? -1 : (int)cm;
    delay(SONAR_SAMPLE_GAP_MS);
  }

  // ---- 3.2 打印 5 个原始值（"--" 表示这次超时） ----
  Serial.print("  5 次采样: ");
  for (int i = 0; i < SONAR_SAMPLES; i++) {
    if (raw[i] < 0) Serial.print("--  ");
    else            Serial.printf("%dcm ", raw[i]);
  }
  Serial.println();

  // ---- 3.3 只留下合理范围内的值 ----
  int v[SONAR_SAMPLES];
  int n = 0;
  for (int i = 0; i < SONAR_SAMPLES; i++) {
    if (raw[i] >= SONAR_MIN_CM && raw[i] <= SONAR_MAX_CM) {
      v[n++] = raw[i];
    }
  }
  if (validCount) *validCount = n;
  if (n == 0) return -1;

  // ---- 3.4 插入排序（只有 5 个数，冒泡/插入随便都行） ----
  for (int i = 1; i < n; i++) {
    int key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = key;
  }

  // ---- 3.5 取正中间那个数 ----
  return (long)v[n / 2];
}

/* ==========================================================================
 *  4. 串口条形图：每格 5cm，满格 200cm
 * ==========================================================================*/
void printBar(long cm) {
  int len = (int)((cm * SONAR_BAR_WIDTH) / SONAR_BAR_MAX_CM);
  if (len < 0) len = 0;
  if (len > SONAR_BAR_WIDTH) len = SONAR_BAR_WIDTH;

  Serial.print("  [");
  for (int i = 0; i < SONAR_BAR_WIDTH; i++) {
    Serial.print((i < len) ? '#' : ' ');
  }
  Serial.print("] ");
  Serial.print(cm);
  Serial.println("cm");
}

/* ==========================================================================
 *  5. 初始化
 * ==========================================================================*/
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot v1.0 | 06_sonar | HC-SR04 ultrasonic");
  Serial.println("==================================================");

  // ---------- 5.1 串口自检 ----------
  Serial.println("[OK] 串口正常 (115200)");
  Serial.printf("[OK] 超声波引脚: TRIG=GPIO%d  ECHO=GPIO%d\n",
                PIN_SONAR_TRIG, PIN_SONAR_ECHO);
  Serial.println("[OK] VCC 接 3V3，GND 必须共地");
  Serial.println("[!] GPIO19/20 原是 USB 的 D-/D+：只用 UART 口，别插 USB 口");
  Serial.printf("[OK] 超时保护: %lu us (约 %d 米)，绝不卡死\n",
                (unsigned long)SONAR_TIMEOUT_US, 5);
  Serial.printf("[OK] 中值滤波: 每次测 %d 下，间隔 %dms，取中值\n",
                SONAR_SAMPLES, SONAR_SAMPLE_GAP_MS);

  // ---------- 5.2 配置引脚 ----------
  pinMode(PIN_SONAR_TRIG, OUTPUT);
  pinMode(PIN_SONAR_ECHO, INPUT);
  digitalWrite(PIN_SONAR_TRIG, LOW);        // 平时保持低电平
  Serial.println("[OK] 引脚配置完成");

  Serial.println("--------------------------------------------------");
  Serial.println("★ 把手放在超声波探头前面 10~30cm 来回移动，");
  Serial.println("★ 看下面的数字和条形图怎么变 —— 这就是避障用的数据");
  Serial.println("--------------------------------------------------");
  delay(600);
}

/* ==========================================================================
 *  6. 主循环：约 0.5 秒测一组
 * ==========================================================================*/
void loop() {
  Serial.println();

  int valid = 0;
  long cm = sonarMedianCm(&valid);

  if (cm < 0) {
    // 全都没收到回波：打印失败原因（绝不死等，马上继续下一轮）
    Serial.println("[FAIL] 超时：没有收到回波");
    Serial.println("       逐项检查: ECHO 线插好? VCC 是 3V3? 共地? 目标在 4 米内且正对?");
  } else {
    Serial.printf("[OK] 距离 = %ld cm  (%d/%d 次有效，取中值)\n",
                  cm, valid, SONAR_SAMPLES);
    printBar(cm);

    if (cm < SONAR_NEAR_CM) {
      Serial.println("       [警告] 太近了！以后跑避障程序，这个距离要停车或后退");
    } else if (cm < 50) {
      Serial.println("       状态: 近，可以慢慢走");
    } else if (cm < 150) {
      Serial.println("       状态: 中，可以正常走");
    } else {
      Serial.println("       状态: 远，随便走");
    }
  }

  delay(150);
}
