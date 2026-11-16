/* ============================================================================
 *  05_audio.ino —— AI 桌面机器人 · 音频自测（INMP441 麦克风 + MAX98357A 功放）
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    用 ESP32-S3（N16R8）把"听得见"和"放得出"一次验证清楚，一共三级，用下面
 *    的 TEST_LEVEL 选：
 *      第 1 级：只用功放，在 RAM 里合成 1kHz 正弦波播 2 秒 → 应该听到"嘟"
 *      第 2 级：只用麦克风，采 3 秒，串口边采边打印 RMS/峰值条形图和分贝估计
 *      第 3 级：录 3 秒 → 先释放 RX → 再初始化 TX 回放 → 听到自己刚才的声音，
 *               然后循环"录 → 放 → 打印本轮峰值"
 *    ★ 零基础用户请看：先只跑第 1 级（听到嘟声就说明功放+接线+时钟都对），
 *      再改 2 跑麦克风，最后改 3 跑录放。一次只改这个数字。
 *
 *  【接线】（详见 docs/01_接线总表.md 第 3.3、3.4 节）
 *    INMP441 麦克风： VDD→3V3   GND→GND   L/R→GND（选左声道，不接会没声音）
 *                     SCK/BCLK→GPIO39   WS→GPIO40   SD/DOUT→GPIO41
 *    MAX98357A 功放： VIN→5V    GND→GND   SD/SHDN→3V3（拉高才开机，接低=静音；
 *                     SD 电压 >1.4V 时功放选"左声道"输出，见下面的常见错误）
 *                     GAIN→悬空（默认 9dB）   BCLK→GPIO39   LRC→GPIO40
 *                     DIN→GPIO42
 *    2831 喇叭：插功放上的 PH2.0 座（别把喇叭线接到电源上）
 *    ★ 麦克风和功放共用 BCLK(GPIO39) 与 WS(GPIO40) 两根线——这块板的 30 个可用
 *      引脚被 7 个模块用满了。所有模块的 GND 必须接到同一个电网。
 *    ★ 代码里的引脚全部来自 pinmap.h 的 PIN_I2S_* 宏，不写死数字。
 *
 *  【为什么共用时钟还能工作：分时复用】
 *    I2S 是"主机出时钟、从机跟着走"的总线：BCLK/WS 只能有一套频率参数。而麦克风
 *    需要 32bit slot，功放需要 16bit slot，两者不能同时成立。所以本程序这样做：
 *      录音：i2s_new_channel → init(RX, 32bit) → enable(RX) → i2s_channel_read ...
 *      放音：先 i2s_channel_disable(RX) + i2s_del_channel(RX) 把总线释放，
 *            再 i2s_new_channel → init(TX, 16bit) → enable(TX) → i2s_channel_write ...
 *    同一时刻只开一个方向，绝不并行。并行就变成 full-duplex（全双工），BCLK 频率
 *    要求冲突，很容易初始化失败。代码里 audioTxStart() 会先释放 RX，
 *    audioRxStart() 会先释放 TX，两个方向互相兜底。
 *
 *  【预期现象】（串口监视器 115200）
 *    第 1 级：串口先出现 [OK] I2S TX 已启动，听到 2 秒"嘟"声；之后每 8 秒自动再
 *            响一次，方便反复确认。
 *    第 2 级：先出现 [OK] I2S RX 已启动，然后条形图随你说话起伏；
 *            静音时 RMS 很小（一般 < 300），正常说话明显变大（一般 > 1000），
 *            拍手/大声喊能冲到 40 格以上。
 *    第 3 级：每轮先"录 3 秒"（条形图起伏），再打印 [OK] I2S RX 已释放，
 *            然后 [OK] I2S TX 已启动 并把刚才的声音播一遍，最后打印本轮峰值，
 *            约 10 秒一轮循环。
 *
 *  【常见错误】
 *    - 第 1 级没声音：功放 SD/SHDN 没接 3V3（接地就是静音）、VIN 没接 5V、
 *      喇叭没插到功放座上、或 BCLK/WS 接错（必须和麦克风共用 GPIO39/40）。
 *    - 有"沙沙"声但音乐/嘟声很小：确认 GAIN 悬空（9dB）。MAX98357A 用 SD 脚的电压
 *      选声道：接 3V3（>1.4V）输出**左声道**、约 0.77~1.4V 输出右声道、0.16~0.77V 输出
 *      (L+R)/2 混音。本程序把单声道数据**同时送到左右两个 slot**，所以左声道里也有
 *      数据，SD 接 3V3 照样有声音，不会"只剩一半"。
 *    - 第 2 级条纹丝不动：INMP441 的 L/R 必须接 GND（接 3V3 就变右声道，代码取
 *      左声道自然安静）；SD 要接 GPIO41；VDD 接 3V3（接 5V 会烧麦克风）。
 *    - 第 2 级数值一直很小、几乎不变：先关掉串口监视器的"自动滚动"，对着麦克风说话
 *      看条形图是否起伏；完全不动就按上一条逐项查接线。
 *    - 第 3 级播放时板子重启：5V 上必须并 1000µF 电解电容。本程序已经把播放幅度
 *      限制在满量程 60%（≈19660/32767）以内，就是为了减小电流冲击。
 *    - 第 3 级提示 ps_malloc 失败：Arduino IDE → 工具 → PSRAM 选 "OPI PSRAM"
 *      （N16R8 = 8MB OPI PSRAM），然后重新编译烧录。
 *    - 串口乱码：波特率不是 115200；或插错 Type-C 口（烧录/串口必须用 UART 口）。
 *    - 反复编译不过：确认开发板选 ESP32S3 Dev Module、Arduino core 是 3.x。
 *
 *  【待核实】（不确定的地方写清楚，不瞎编）
 *    - 本机没装 arduino-esp32 core，本文件未实际编译过。API 依据 ESP-IDF v5.5
 *      （arduino-esp32 3.3.x）的 driver/i2s_std.h。core 3.0.x 的宏字段略有差异，
 *      但这里全部用官方 I2S_STD_* / I2S_CHANNEL_* 宏，正常应能编译。
 *    - 电平判读阈值（静音 RMS<300、说话 >1000）是按 INMP441 24bit 左对齐右移 16
 *      位后的估算值；实测偏大/偏小时改 kQuietRms / kLoudRms 即可，不影响录音本身。
 *    - INMP441 固定是 Philips 格式。若电平表一直不动，可把 audioRxStart() 里的
 *      槽格式换成 I2S_STD_MSB_SLOT_DEFAULT_CONFIG 再试一次。
 * ==========================================================================*/

#include "pinmap.h"              // 引脚唯一权威来源，禁止在本文件写死 GPIO 数字
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"     // IDF 新 I2S API（配合 i2s_common.h）
#include "esp32-hal-psram.h"    // ps_malloc / psramFound
#include "esp_heap_caps.h"      // heap_caps_get_free_size(MALLOC_CAP_SPIRAM)

/* ==========================================================================
 *  用户唯一要改的地方：测试级别
 *    1 = 只放音（先跑这个）
 *    2 = 只录音（看电平表）
 *    3 = 录 3 秒 → 回放，循环演示
 * ==========================================================================*/
#define TEST_LEVEL 1

#if (TEST_LEVEL < 1) || (TEST_LEVEL > 3)
#error "TEST_LEVEL 只能是 1、2、3"
#endif

/* ==========================================================================
 *  参数
 * ==========================================================================*/
static constexpr uint32_t kSampleRate   = 16000;   // 16kHz，麦克风和功放都用它
static constexpr uint32_t kDmaDescNum    = 8;      // DMA 描述符个数（8 块 ≈ 240ms 缓冲）
static constexpr uint32_t kDmaFrameNum   = 480;    // 每块 DMA 的帧数；480*4B=1920B < 4092B 上限
static constexpr size_t   kMeterFrames   = kSampleRate / 10;  // 电平表每 ~100ms 刷新一次

// 电平判读阈值（只影响串口里 [OK]/[WARN] 的判定，不影响录音）
static constexpr int32_t  kQuietRms      = 300;    // 低于它基本是静音
static constexpr int32_t  kLoudRms      = 1000;    // 高于它说明麦克风正常收到说话声

// 第 3 级录音缓冲：3 秒 × 16kHz × 2 字节 = 96000 字节，放 PSRAM
static constexpr uint32_t kRecordSeconds = 3;
static constexpr size_t   kRecordSamples = (size_t)kSampleRate * kRecordSeconds;
static constexpr size_t   kRecordBytes   = kRecordSamples * sizeof(int16_t);

// 播放幅度上限：满量程的 60%（防止 5V 电流冲击把主控拉重启）
static constexpr float    kMaxPlayAmp    = 0.60f * 32767.0f;   // ≈ 19660
static constexpr float    kMaxPlayGain   = 4.0f;               // 回放最多放大 4 倍

/* ==========================================================================
 *  全局状态
 * ==========================================================================*/
static i2s_chan_handle_t g_tx = NULL;      // 播放通道（同一时刻最多一个）
static i2s_chan_handle_t g_rx = NULL;      // 录音通道（同一时刻最多一个）
static int16_t *g_recBuf = NULL;           // 第 3 级 PSRAM 录音缓冲

static int32_t g_capPeak = 0;              // 最近一次采集的峰值
static int32_t g_capRms  = 0;              // 最近一次采集的 RMS

/* ==========================================================================
 *  小工具
 * ==========================================================================*/
static void printI2sErr(const char *what, esp_err_t err) {
  Serial.printf("[FAIL] %s 出错: %s (err=%d)\n", what, esp_err_to_name(err), (int)err);
}

// 倒计时提示（给零基础用户留出准备时间）
static void countdown(int seconds) {
  for (int i = seconds; i > 0; i--) {
    Serial.printf("[..] %d...\n", i);
    delay(1000);
  }
}

// 打印条幅和公共配置
static void printBanner() {
  Serial.println();
  Serial.println("==================================================");
  Serial.println(" AI-Robot · 05_audio  音频自测（INMP441 + MAX98357A）");
  Serial.println("==================================================");
  Serial.printf(" TEST_LEVEL = %d   （1=只放音 2=只录音 3=录放循环）\n", TEST_LEVEL);
  Serial.printf(" 采样率 = %lu Hz    RX = 32bit slot / 单声道(左)   TX = 16bit / 单声道\n",
                (unsigned long)kSampleRate);
  Serial.printf(" 共用时钟: BCLK = GPIO%d   WS/LRC = GPIO%d\n", PIN_I2S_BCLK, PIN_I2S_WS);
  Serial.printf(" 麦克风 SD = GPIO%d (输入)   功放 DIN = GPIO%d (输出)\n",
                PIN_I2S_MIC_SD, PIN_I2S_AMP_DIN);
  Serial.println(" 规则：同一时刻只开 RX 或 TX 之一（分时复用）");
  Serial.println("==================================================");
}

/* ==========================================================================
 *  I2S 释放 / 启动
 *  ★ 分时复用的核心：先 disable 再 del_channel，把 BCLK/WS 彻底交还
 * ==========================================================================*/
static void audioRxStop() {
  if (g_rx == NULL) return;
  esp_err_t e1 = i2s_channel_disable(g_rx);
  esp_err_t e2 = i2s_del_channel(g_rx);
  g_rx = NULL;
  if (e1 == ESP_OK && e2 == ESP_OK) {
    Serial.println("[OK] I2S RX 已释放（BCLK/WS 交还给功放用）");
  } else {
    Serial.printf("[WARN] 释放 RX 时有小问题: disable=%d del=%d（通常不影响继续）\n",
                  (int)e1, (int)e2);
  }
}

static void audioTxStop() {
  if (g_tx == NULL) return;
  esp_err_t e1 = i2s_channel_disable(g_tx);
  esp_err_t e2 = i2s_del_channel(g_tx);
  g_tx = NULL;
  if (e1 == ESP_OK && e2 == ESP_OK) {
    Serial.println("[OK] I2S TX 已释放（BCLK/WS 交还给麦克风用）");
  } else {
    Serial.printf("[WARN] 释放 TX 时有小问题: disable=%d del=%d（通常不影响继续）\n",
                  (int)e1, (int)e2);
  }
}

// 停止并删除通道（内部用，失败也只打印）
static void cleanupTxOnFail() {
  if (g_tx == NULL) return;
  i2s_channel_disable(g_tx);
  i2s_del_channel(g_tx);
  g_tx = NULL;
}

static void cleanupRxOnFail() {
  if (g_rx == NULL) return;
  i2s_channel_disable(g_rx);
  i2s_del_channel(g_rx);
  g_rx = NULL;
}

/* ---------------- 播放：TX 16bit 单声道 16kHz ---------------- */
static bool audioTxStart() {
  if (g_tx != NULL) return true;          // 已经开着，直接用

  // ★ 强约束：开 TX 之前必须确认 RX 已释放
  if (g_rx != NULL) {
    Serial.println("[..] 检测到 RX 还开着，先释放 RX（分时复用：不能同时开）");
    audioRxStop();
  }

  // 1) 申请 I2S 通道（只申请 TX）
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = kDmaDescNum;
  chan_cfg.dma_frame_num = kDmaFrameNum;
  chan_cfg.auto_clear    = true;          // 没数据时自动补 0，避免功放"嗡嗡"
  esp_err_t err = i2s_new_channel(&chan_cfg, &g_tx, NULL);
  if (err != ESP_OK) {
    printI2sErr("i2s_new_channel(TX)", err);
    g_tx = NULL;
    return false;
  }

  // 2) 配成标准模式：16bit、单声道
  //    MAX98357A 的 SD 接 3V3（>1.4V）时**只输出左声道**（SD 在不同电压区间会选
  //    左/右/(L+R)/2，接 3V3 是左声道）。所以我们把同一份单声道数据同时写进左右
  //    两个 slot —— 这样无论 SD 怎么接、以后换模块都不用改代码。
  //    注意：这里**显式**写 slot_mask，不要依赖库的默认值（不同 IDF 版本默认值不一样，
  //    会出现"播放正常但某个声道没声"的怪问题）。
  i2s_std_config_t cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)PIN_I2S_BCLK,
          .ws   = (gpio_num_t)PIN_I2S_WS,
          .dout = (gpio_num_t)PIN_I2S_AMP_DIN,
          .din  = I2S_GPIO_UNUSED,
      },
  };
  /* 显式写清楚：单声道时把同一份数据送到【左右两个 slot】，不靠默认值 */
  cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  err = i2s_channel_init_std_mode(g_tx, &cfg);
  if (err != ESP_OK) {
    printI2sErr("i2s_channel_init_std_mode(TX)", err);
    cleanupTxOnFail();
    return false;
  }
  err = i2s_channel_enable(g_tx);
  if (err != ESP_OK) {
    printI2sErr("i2s_channel_enable(TX)", err);
    cleanupTxOnFail();
    return false;
  }

  Serial.printf("[OK] I2S 配置完成：TX %lu Hz / 16bit / 单声道\n", (unsigned long)kSampleRate);
  Serial.printf("[OK] I2S TX 已启动：BCLK=GPIO%d  WS=GPIO%d  DIN=GPIO%d（MAX98357A）\n",
                PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_AMP_DIN);
  return true;
}

/* ---------------- 录音：RX 32bit slot 单声道（只取左声道）16kHz ---------------- */
static bool audioRxStart() {
  if (g_rx != NULL) return true;

  // ★ 强约束：开 RX 之前必须确认 TX 已释放
  if (g_tx != NULL) {
    Serial.println("[..] 检测到 TX 还开着，先释放 TX（分时复用：不能同时开）");
    audioTxStop();
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = kDmaDescNum;
  chan_cfg.dma_frame_num = kDmaFrameNum;
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &g_rx);
  if (err != ESP_OK) {
    printI2sErr("i2s_new_channel(RX)", err);
    g_rx = NULL;
    return false;
  }

  // INMP441 每个样本是 24bit，左对齐放在 32bit slot 的高 24 位里。
  // L/R 脚接 GND 时麦克风把数据放在 Philips 格式的"左声道"→ 只取 I2S_STD_SLOT_LEFT。
  i2s_std_config_t cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)PIN_I2S_BCLK,
          .ws   = (gpio_num_t)PIN_I2S_WS,
          .dout = I2S_GPIO_UNUSED,
          .din  = (gpio_num_t)PIN_I2S_MIC_SD,
      },
  };
  cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // 单声道取左声道

  err = i2s_channel_init_std_mode(g_rx, &cfg);
  if (err != ESP_OK) {
    printI2sErr("i2s_channel_init_std_mode(RX)", err);
    cleanupRxOnFail();
    return false;
  }
  err = i2s_channel_enable(g_rx);
  if (err != ESP_OK) {
    printI2sErr("i2s_channel_enable(RX)", err);
    cleanupRxOnFail();
    return false;
  }

  Serial.printf("[OK] I2S 配置完成：RX %lu Hz / 32bit slot / 单声道(左声道)\n",
                (unsigned long)kSampleRate);
  Serial.printf("[OK] I2S RX 已启动：BCLK=GPIO%d  WS=GPIO%d  SD=GPIO%d（INMP441）\n",
                PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_MIC_SD);
  return true;
}

/* ==========================================================================
 *  播放
 * ==========================================================================*/
// 写一块（≤kDmaFrameNum 个样本），部分写入会继续补写
static bool writeChunk(const int16_t *data, size_t nSamples) {
  size_t sent = 0;
  while (sent < nSamples) {
    size_t written = 0;
    esp_err_t err = i2s_channel_write(g_tx, data + sent,
                                      (nSamples - sent) * sizeof(int16_t),
                                      &written, 1000);
    if (err != ESP_OK) {
      printI2sErr("i2s_channel_write", err);
      return false;
    }
    if (written == 0) {
      Serial.println("[FAIL] i2s_channel_write 写了 0 字节（通道可能已停）");
      return false;
    }
    sent += written / sizeof(int16_t);
  }
  return true;
}

// 播一段 int16 单声道数据；内部按 DMA 块大小喂数据
// 注意：本函数会打开 TX，但【不关闭】TX，由调用者决定何时 audioTxStop()
static bool playSamples(const int16_t *data, size_t nSamples) {
  if (data == NULL || nSamples == 0) return false;
  if (!audioTxStart()) return false;      // 内部会先释放 RX

  size_t sent = 0;
  while (sent < nSamples) {
    size_t n = nSamples - sent;
    if (n > kDmaFrameNum) n = kDmaFrameNum;
    if (!writeChunk(data + sent, n)) return false;
    sent += n;
  }
  // DMA 里最多还压着 8 块（≈240ms）的数据没播完，等一下再关通道，
  // 否则结尾会被"吞"掉几十毫秒。
  delay(250);
  return true;
}

// 在 RAM 里合成正弦波并播放（不占用大缓冲，一块一块生成）
static void playTone(uint32_t freqHz, uint32_t ms) {
  if (!audioTxStart()) {
    Serial.println("[FAIL] TX 启动失败，放不了音");
    return;
  }
  static constexpr size_t kChunkSamples = 320;                    // 20ms 一块
  const size_t total = (size_t)kSampleRate * ms / 1000;           // 总样本数
  const size_t fade  = (size_t)(kSampleRate / 100);               // 10ms 淡入淡出
  const float  amp   = kMaxPlayAmp;                               // 满量程 60%
  const float  step  = 2.0f * (float)M_PI * (float)freqHz / (float)kSampleRate;

  int16_t buf[kChunkSamples];
  float phase = 0.0f;
  size_t done = 0;
  bool ok = true;
  while (done < total && ok) {
    size_t n = total - done;
    if (n > kChunkSamples) n = kChunkSamples;
    for (size_t i = 0; i < n; i++) {
      size_t k = done + i;                                        // 全局第几个样本
      float env = 1.0f;
      if (k < fade) {
        env = (float)k / (float)fade;                             // 淡入，防"啪"
      } else if (k + fade >= total) {
        env = (float)(total - 1 - k) / (float)fade;               // 淡出
      }
      buf[i] = (int16_t)(sinf(phase) * amp * env);
      phase += step;
      if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
    ok = writeChunk(buf, n);
    done += n;
  }
  delay(250);
  if (ok) {
    Serial.printf("[OK] 已播放 %lu ms / %lu Hz 正弦波（幅度 = 满量程 60%%）\n",
                  (unsigned long)ms, (unsigned long)freqHz);
  } else {
    Serial.println("[FAIL] 播放中途出错");
  }
}

/* ==========================================================================
 *  电平表 & 采集
 * ==========================================================================*/
// 打印一行条形图：0~50 格对应 -60dBFS ~ 0dBFS
static void meterPrint(const int16_t *buf, size_t n) {
  if (buf == NULL || n == 0) return;

  // INMP441 有很小的直流偏置，先减掉窗口均值再算 RMS 更准
  int64_t sum = 0;
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    sum += (int32_t)buf[i];
    int32_t a = (buf[i] < 0) ? -(int32_t)buf[i] : (int32_t)buf[i];
    if (a > peak) peak = a;
  }
  const int32_t mean = (int32_t)(sum / (int64_t)n);

  uint64_t sumSq = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = (int32_t)buf[i] - mean;
    sumSq += (uint64_t)(v * v);
  }
  const int32_t rms = (int32_t)sqrt((double)sumSq / (double)n);

  // 分贝估计：dBFS = 20*log10(rms / 满量程)
  int db = -90;
  if (rms > 0) {
    db = (int)lround(20.0 * log10((double)rms / 32767.0));
  }

  // -60..0 dBFS 映射到 0..50 格
  int len = (db + 60) * 50 / 60;
  if (len < 0) len = 0;
  if (len > 50) len = 50;

  char bar[51];
  for (int i = 0; i < 50; i++) bar[i] = (i < len) ? '#' : '.';
  bar[50] = '\0';

  Serial.printf("RMS=%5d 峰值=%5d %4d dBFS |%s|\n", (int)rms, (int)peak, db, bar);
}

// 采集 ms 毫秒：dst 非空则存成 int16 样本；withMeter 为 true 时边采边打印条形图
// 结束时把峰值/RMS 写到 g_capPeak / g_capRms，并打印一行 [OK]
static bool captureMs(int16_t *dst, uint32_t ms, bool withMeter) {
  const size_t nSamples = (size_t)kSampleRate * ms / 1000;
  static int32_t raw[kDmaFrameNum];                        // 一次读一块 DMA
  static int16_t win[kMeterFrames + kDmaFrameNum];         // 100ms 统计窗口

  uint64_t sumSq = 0;
  int32_t  peak  = 0;
  size_t   total = 0;
  size_t   winLen = 0;

  while (total < nSamples) {
    size_t want = nSamples - total;
    if (want > kDmaFrameNum) want = kDmaFrameNum;

    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(g_rx, raw, want * sizeof(int32_t),
                                     &bytesRead, 1000);
    if (err != ESP_OK) {
      printI2sErr("i2s_channel_read", err);
      return false;
    }
    size_t frames = bytesRead / sizeof(int32_t);
    if (frames == 0) {                                     // 罕见，等一下再读
      delay(2);
      continue;
    }
    if (frames > want) frames = want;

    for (size_t i = 0; i < frames; i++) {
      // INMP441：24bit 有效数据左对齐在 32bit slot 的 bit31..bit8，
      // 右移 16 位（= data>>8>>8）正好留下高 16 位有效音频，转 int16 不丢符号。
      int16_t s = (int16_t)(raw[i] >> 16);
      if (dst != NULL) dst[total + i] = s;

      int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
      if (a > peak) peak = a;
      sumSq += (uint64_t)((int32_t)s * (int32_t)s);

      if (withMeter) win[winLen++] = s;
    }
    total += frames;

    // 每攒满约 100ms 打印一次电平
    if (withMeter && winLen >= kMeterFrames) {
      meterPrint(win, winLen);
      winLen = 0;
    }
  }
  if (withMeter && winLen > 0) meterPrint(win, winLen);

  g_capPeak = peak;
  g_capRms  = (total > 0) ? (int32_t)sqrt((double)sumSq / (double)total) : 0;

  Serial.printf("[OK] 采集结束：%lu 个样本（%lu ms）| 峰值=%d | 整体 RMS=%d\n",
                (unsigned long)total, (unsigned long)ms,
                (int)g_capPeak, (int)g_capRms);

  // 判读：阈值只是参考线，不同麦克风灵敏度不同，以"条形图是否随说话起伏"为准
  Serial.println("[..] 判读：静音 RMS 应很小（参考线 300）；说话时明显变大（参考线 1000）");
  if (g_capRms < kQuietRms) {
    Serial.println("[WARN] 这次 RMS 低于 300 的参考线。如果你刚才在说话，按顺序查：");
    Serial.println("       1) INMP441 的 L/R 是否接 GND（接 3V3 会变右声道）");
    Serial.println("       2) SD/DOUT 是否接 GPIO41；3) VDD 是否 3V3（不是 5V）");
    Serial.println("       4) VDD/GND 是否插到底；5) 是否真的选中了 UART 串口");
    Serial.println("       （若条形图明显随说话起伏，说明麦克风其实是好的，只是离得远/说得轻）");
  } else if (g_capRms >= kLoudRms) {
    Serial.println("[OK] 电平正常：麦克风工作正常（RMS ≥ 1000）");
  } else {
    Serial.println("[..] 有声音但偏小（RMS 在 300~1000 之间）：离麦克风近一点（10~20cm）");
  }
  return true;
}

/* ==========================================================================
 *  第 3 级用：PSRAM 缓冲、归一化、淡入淡出
 * ==========================================================================*/
static bool allocRecordBuffer() {
  if (g_recBuf != NULL) return true;

  Serial.printf("[..] 申请录音缓冲：%lu 字节（3 秒 × 16kHz × 2 字节）\n",
                (unsigned long)kRecordBytes);
  if (psramFound()) {
    Serial.printf("[OK] PSRAM 已启用，当前空闲 %lu 字节\n",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    g_recBuf = (int16_t *)ps_malloc(kRecordBytes);
  } else {
    Serial.println("[WARN] 没检测到 PSRAM：请在 工具→PSRAM 选 OPI PSRAM 后重新烧录");
  }
  if (g_recBuf == NULL) {
    Serial.println("[WARN] ps_malloc 失败，改用内部 RAM 再试一次");
    g_recBuf = (int16_t *)malloc(kRecordBytes);
  }
  if (g_recBuf == NULL) {
    Serial.println("[FAIL] 96KB 缓冲申请失败：把 PSRAM 设为 OPI PSRAM 后重新编译烧录");
    return false;
  }
  memset(g_recBuf, 0, kRecordBytes);
  Serial.println("[OK] 录音缓冲就绪（3 秒 × 16kHz × int16）");
  return true;
}

// 把录音归一化到"满量程 60% 以内"，并回报原始峰值 / 实际增益
static void normalizeTo60(int16_t *buf, size_t n, int32_t *outPeak, int32_t *outGainPct) {
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t a = (buf[i] < 0) ? -(int32_t)buf[i] : (int32_t)buf[i];
    if (a > peak) peak = a;
  }

  float gain = 1.0f;
  if (peak > 0) gain = kMaxPlayAmp / (float)peak;
  if (gain > kMaxPlayGain) gain = kMaxPlayGain;     // 最多放大 4 倍，别把底噪放大成噪声
  if (gain < 0.0f) gain = 0.0f;

  const int32_t cap = (int32_t)kMaxPlayAmp;
  int32_t maxOut = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = (int32_t)lroundf((float)buf[i] * gain);
    if (v >  cap) v =  cap;                          // 硬上限：绝不超过 60%
    if (v < -cap) v = -cap;
    buf[i] = (int16_t)v;
    int32_t a = (v < 0) ? -v : v;
    if (a > maxOut) maxOut = a;
  }

  if (outPeak != NULL)    *outPeak = peak;
  if (outGainPct != NULL) *outGainPct = (int32_t)lroundf(gain * 100.0f);
  Serial.printf("[OK] 回放幅度已限制：原始峰值=%d，增益=%lu%%，最终峰值=%d（上限=%d）\n",
                (int)peak, (unsigned long)((outGainPct != NULL) ? *outGainPct : 0),
                (int)maxOut, (int)cap);
}

// 首尾各淡入/淡出 10ms，避免回放起止"啪"一声
static void applyFade(int16_t *buf, size_t n, size_t fadeLen) {
  if (buf == NULL || n < 2) return;
  if (fadeLen * 2 > n) fadeLen = n / 2;
  for (size_t i = 0; i < fadeLen; i++) {
    int32_t g = (int32_t)((float)i / (float)fadeLen * 1000.0f);   // 0..1000
    int16_t *head = &buf[i];
    int16_t *tail = &buf[n - 1 - i];
    *head = (int16_t)((int32_t)(*head) * g / 1000);
    *tail = (int16_t)((int32_t)(*tail) * g / 1000);
  }
}

/* ==========================================================================
 *  三级演示
 * ==========================================================================*/
// 第 1 级：只放音
static void demoLevel1Once() {
  Serial.println();
  Serial.println("----- 第 1 级：I2S TX 播放 1kHz 正弦波 2 秒 -----");
  Serial.println("[..] 这一级完全不碰麦克风，只证明功放 + 接线 + 时钟正常");
  playTone(1000, 2000);
  Serial.println("[OK] 第 1 级完成。应该听到一声清楚的\"嘟\"。没听到请先查功放 SD/VIN/喇叭");
}

// 第 2 级：只录音，看电平表
static void demoLevel2Once(uint32_t round) {
  Serial.println();
  Serial.printf("----- 第 2 级 · 第 %lu 次：采集 3 秒并打印电平表 -----\n",
                (unsigned long)round);
  Serial.println("[..] 判读标准：静音 RMS < 300；正常说话 RMS > 1000；条形图 >40 格算很响");
  Serial.println("[..] 请对着麦克风说话 / 拍手，看条形图起伏");
  // 先倒计时再开 RX：避免 RX 空转 2 秒、DMA 缓冲区被覆盖
  countdown(2);
  if (!audioRxStart()) {
    Serial.println("[FAIL] I2S RX 启动失败，本轮跳过（先确认第 1 级能听到声音）");
    delay(2000);
    return;
  }
  Serial.println("[..] 开始采集！");
  captureMs(NULL, 3000, true);
  audioRxStop();
  delay(1000);
}

// 第 3 级：录 3 秒 → 释放 RX → 初始化 TX 回放 → 打印本轮峰值
static void demoLevel3Round(uint32_t round) {
  Serial.println();
  Serial.println("==================================================");
  Serial.printf(" 第 3 级 · 第 %lu 轮：录 3 秒 → 释放 RX → 回放\n",
                (unsigned long)round);
  Serial.println("==================================================");

  if (g_recBuf == NULL) {
    Serial.println("[FAIL] 录音缓冲为空（申请失败），无法继续。请开启 OPI PSRAM 后重烧");
    delay(5000);
    return;
  }

  // ---- 1. 录 ----
  // 先倒计时再开 RX：避免 RX 空转 2 秒、DMA 缓冲区被覆盖
  Serial.println("[..] 2 秒后开始录音，请对着麦克风说话");
  countdown(2);
  if (!audioRxStart()) {
    Serial.println("[FAIL] I2S RX 启动失败，本轮跳过");
    delay(2000);
    return;
  }
  Serial.println("[..] 开始录音！");
  const bool ok = captureMs(g_recBuf, kRecordSeconds * 1000, true);

  // ---- 2. 录完立刻释放 RX（分时复用的关键一步）----
  audioRxStop();
  if (!ok) {
    Serial.println("[FAIL] 录音失败，本轮跳过");
    delay(2000);
    return;
  }

  // ---- 3. 归一化到 60% 以内 + 首尾淡入淡出 ----
  int32_t recPeak = 0;
  int32_t gainPct = 0;
  normalizeTo60(g_recBuf, kRecordSamples, &recPeak, &gainPct);
  applyFade(g_recBuf, kRecordSamples, kSampleRate / 100);

  // ---- 4. 初始化 TX 回放 ----
  Serial.println("[..] 正在初始化 I2S TX 回放（同一时刻只开一个方向）");
  if (playSamples(g_recBuf, kRecordSamples)) {
    Serial.println("[OK] 回放完成 —— 刚才听到的就是你自己 3 秒前的声音");
  } else {
    Serial.println("[FAIL] 回放失败：检查功放 SD→3V3、VIN→5V、喇叭是否插好");
  }
  audioTxStop();

  Serial.printf("[OK] 本轮峰值 = %d（第 %lu 轮结束，约 3 秒后开始下一轮）\n",
                (int)recPeak, (unsigned long)round);
  delay(3000);
}

/* ==========================================================================
 *  Arduino 入口
 * ==========================================================================*/
void setup() {
  Serial.begin(115200);
  delay(300);
  printBanner();

#if TEST_LEVEL == 1
  Serial.println("【第 1 级】只验证功放（不碰麦克风）");
  Serial.println("预期：先看到 [OK] I2S TX 已启动，然后听到 2 秒\"嘟\"声");
  demoLevel1Once();

#elif TEST_LEVEL == 2
  Serial.println("【第 2 级】只验证麦克风（不碰功放）");
  Serial.println("预期：先看到 [OK] I2S RX 已启动，然后条形图随你说话起伏");
  demoLevel2Once(1);

#elif TEST_LEVEL == 3
  Serial.println("【第 3 级】录 3 秒 → 回放，循环演示");
  Serial.println("预期：每轮先录 3 秒（条形图起伏），再听到刚才的声音，最后打印本轮峰值");
  allocRecordBuffer();

#endif
}

void loop() {
#if TEST_LEVEL == 1
  // 第 1 级：每 8 秒自动重播一次，方便反复确认
  delay(8000);
  Serial.println();
  Serial.println("[..] 再次播放 1kHz 正弦波 2 秒（TX 一直开着，无需重新配置）");
  playTone(1000, 2000);

#elif TEST_LEVEL == 2
  // 第 2 级：每轮重新初始化 RX，反复采 3 秒
  static uint32_t round = 1;
  delay(2000);
  round++;
  demoLevel2Once(round);

#elif TEST_LEVEL == 3
  // 第 3 级：一轮一轮地"录 → 放"
  static uint32_t round = 0;
  round++;
  demoLevel3Round(round);

#endif
}
