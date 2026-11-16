/* ============================================================================
 *  AI-Robot · 引脚总表（唯一权威来源 / Single Source of Truth）
 *  板卡 : ESP32-S3-DevKitC-1  N16R8  (16MB Flash + 8MB OPI PSRAM)
 *  版本 : v1.0
 *  ---------------------------------------------------------------------------
 *  【铁律】
 *   1) 所有 .ino 一律 #include "pinmap.h"，绝对不要在 .ino 里写死引脚数字。
 *   2) 要改引脚只改这个文件，然后运行 tools\sync_pinmap.ps1 同步到所有 sketch。
 *   3) 本板 30 个可用 GPIO 被 7 个模块用满，一个不剩。想加东西必须先砍东西。
 *   4) 上传代码请用【UART 口】那颗 Type-C；【USB 口】平时不要插（详见 docs/01）。
 * ==========================================================================*/

#pragma once

/* ---------------- 1. 摄像头 OV2640 (DVP 并口, 14 根信号) ---------------- */
#define PIN_CAM_D0        4    /* 摄像头都是 3.3V 信号, 不要接 5V           */
#define PIN_CAM_D1        5
#define PIN_CAM_D2        6
#define PIN_CAM_D3        7
#define PIN_CAM_D4        8
#define PIN_CAM_D5        9
#define PIN_CAM_D6       10
#define PIN_CAM_D7       11
#define PIN_CAM_XCLK     12    /* 主时钟。★本项目实物模块自带 24M 晶振、没有
                                * XCLK 脚，所以这个脚实际【空着】——可以当备用
                                * 引脚（加蜂鸣器/按键/灯都行）。换成带 XCLK 的
                                * 模块时就自动用上它，不用改代码。              */
#define PIN_CAM_PCLK     13    /* 像素时钟, 摄像头输出给 ESP32             */
#define PIN_CAM_VSYNC    14    /* 帧同步                                   */
#define PIN_CAM_HREF     15    /* 行同步                                   */
#define PIN_CAM_SIOD     16    /* SCCB 数据 (类 I2C)                       */
#define PIN_CAM_SIOC     17    /* SCCB 时钟                                */
/* RST 直接插 3V3, PWDN 直接插 GND —— 不占 GPIO */

/* ---------------- 2. 2.4 寸 ST7789 彩屏 (SPI, 4 根) -------------------- */
#define PIN_TFT_SCL      21    /* = SCK                                    */
#define PIN_TFT_SDA      47    /* = MOSI                                   */
#define PIN_TFT_CS        3    /* GPIO3 是 JTAG 源选择脚; 出厂 eFuse 下不生效,
                                * 屏幕 CS 平时是高电平/悬空, 不会误触发。安全。  */
#define PIN_TFT_DC       38    /* v1.1 开发板的板载 RGB 灯在这个脚 -> 会跟着闪,
                                * 属正常现象, 不影响显示                           */
/* TFT RST 插 3V3, BLK 插 3V3 —— 不占 GPIO (屏幕常亮) */

/* ---------------- 3. 音频 I2S (麦克风 + 功放 共用 BCLK/WS) ------------- */
#define PIN_I2S_BCLK     39    /* 录放共用 (分时复用, 不会同时用)          */
#define PIN_I2S_WS       40    /* = LRC / LRCLK                            */
#define PIN_I2S_MIC_SD   41    /* INMP441 的 SD  -> ESP32 (输入)           */
#define PIN_I2S_AMP_DIN  42    /* ESP32 -> MAX98357A 的 DIN (输出)         */

/* ---------------- 4. TB6612FNG 电机驱动 (2 通道带 4 个 TT 电机) -------- */
/* 用"方向脚直接吃 PWM"的方式驱动, 省下 2 个引脚。左轮 A 通道, 右轮 B 通道 */
#define PIN_MOTOR_AIN1    1
#define PIN_MOTOR_AIN2    2
#define PIN_MOTOR_BIN1   45    /* 上电必须为低 -> 模块内部下拉, 电机静止   */
#define PIN_MOTOR_BIN2   46    /* 同上                                     */
/* STBY 直接插 3V3 (常开), VM 接 5V, VCC 接 3V3 */

/* ---------------- 5. 云台 SG90 x2 ------------------------------------- */
#define PIN_SERVO_PAN    18    /* 水平 (左右转头)                          */
#define PIN_SERVO_TILT   48    /* 俯仰 (抬头低头)。板载 RGB 灯: v1.0 板在
                                * GPIO48、v1.1 板在 GPIO38, 所以这个脚和屏幕
                                * DC 脚都可能让 RGB 灯跟着闪 —— 纯外观, 无害 */

/* ---------------- 6. HC-SR04 超声波 (宽压版, 供 3V3) ------------------ */
#define PIN_SONAR_TRIG   19    /* 这两个脚原是 USB D-/D+, 用普通 GPIO 后   */
#define PIN_SONAR_ECHO   20    /* 就【不能插 USB 口】, 见 docs/01           */

/* ---------------- 7. 预留 / 禁碰 -------------------------------------- */
#define PIN_BOOT_BTN      0    /* 板载 BOOT 键: 当"按住说话"用, 不用接线   */
#define PIN_UART_TX      43    /* 串口监视器 (UART 口), 不要接模块         */
#define PIN_UART_RX      44    /* 同上, 且被 CP2102 拉着, 不要用           */

/* ---------------- 8. 编译期自检 (防止手滑改成禁区引脚) ---------------- */
#define ROBOT_GUARD(pin)                                                      \
  static_assert(((pin) != 0) && ((pin) < 26 || (pin) > 37),                   \
                "引脚落在禁区: 0=BOOT 或 26-37=Flash/PSRAM -> " #pin);
/* 26-32 = 板载 Flash, 33-37 = 8MB OPI PSRAM (N16R8 专有), 都不能用 */

ROBOT_GUARD(PIN_CAM_D0)  ROBOT_GUARD(PIN_CAM_D1)  ROBOT_GUARD(PIN_CAM_D2)
ROBOT_GUARD(PIN_CAM_D3)  ROBOT_GUARD(PIN_CAM_D4)  ROBOT_GUARD(PIN_CAM_D5)
ROBOT_GUARD(PIN_CAM_D6)  ROBOT_GUARD(PIN_CAM_D7)  ROBOT_GUARD(PIN_CAM_XCLK)
ROBOT_GUARD(PIN_CAM_PCLK) ROBOT_GUARD(PIN_CAM_VSYNC) ROBOT_GUARD(PIN_CAM_HREF)
ROBOT_GUARD(PIN_CAM_SIOD) ROBOT_GUARD(PIN_CAM_SIOC)
ROBOT_GUARD(PIN_TFT_SCL) ROBOT_GUARD(PIN_TFT_SDA)
ROBOT_GUARD(PIN_TFT_CS)  ROBOT_GUARD(PIN_TFT_DC)
ROBOT_GUARD(PIN_I2S_BCLK) ROBOT_GUARD(PIN_I2S_WS)
ROBOT_GUARD(PIN_I2S_MIC_SD) ROBOT_GUARD(PIN_I2S_AMP_DIN)
ROBOT_GUARD(PIN_MOTOR_AIN1) ROBOT_GUARD(PIN_MOTOR_AIN2)
ROBOT_GUARD(PIN_MOTOR_BIN1) ROBOT_GUARD(PIN_MOTOR_BIN2)
ROBOT_GUARD(PIN_SERVO_PAN)  ROBOT_GUARD(PIN_SERVO_TILT)
ROBOT_GUARD(PIN_SONAR_TRIG) ROBOT_GUARD(PIN_SONAR_ECHO)

/* ---------------- 9. 常用参数 ----------------------------------------- */
#define MOTOR_PWM_FREQ   20000 /* 20kHz, 超出人耳范围, 电机不尖叫        */
#define MOTOR_PWM_RES       10 /* 10 位 -> 0~1023                        */
#define MOTOR_PWM_MAX     1023
#define SERVO_FREQ          50 /* 舵机标准 50Hz                          */
#define SERVO_MIN_US      500 /* 0.5ms -> 0 度  (SG90 约 0~180 度)      */
#define SERVO_MAX_US     2500 /* 2.5ms -> 180 度                        */
