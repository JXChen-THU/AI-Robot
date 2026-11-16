/* ============================================================================
 *  AI-Robot · 07_main · 云语音三件套（ASR 语音识别 / LLM 大模型 / TTS 语音合成）
 *  文件: voice_cloud.h          需要和 07_main.ino、secrets.h 放在同一个文件夹
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    按一下板载 BOOT 键（或手机网页点“说话”），机器人就把你 3 秒钟的话
 *    送上云端：语音 → 文字（ASR）→ 大模型思考（LLM）→ 一句回答 + 一个动作
 *    指令 → 文字转语音（TTS）从喇叭放出来，同时让主程序去执行动作。
 *
 *    两个后端，改 secrets.h 里的 VOICE_BACKEND 一行就能切：
 *      1) 百度智能云（默认）：ASR + TTS 用百度，LLM 用 OpenAI 兼容（DeepSeek）
 *      2) OpenAI 兼容一站式（SiliconFlow 硅基流动）：一个 Key 全包
 *
 *  【接线】（引脚全部来自 pinmap.h，绝不写死数字）
 *    INMP441 麦克风：VDD→3V3  GND→GND  L/R→GND（必须接地！）
 *                    SCK→PIN_I2S_BCLK(GPIO39)  WS→PIN_I2S_WS(GPIO40)
 *                    SD→PIN_I2S_MIC_SD(GPIO41)
 *    MAX98357A 功放：VIN→5V  GND→GND  SD→3V3（不接会静音）
 *                    BCLK→PIN_I2S_BCLK(GPIO39)  LRC→PIN_I2S_WS(GPIO40)
 *                    DIN→PIN_I2S_AMP_DIN(GPIO42)  喇叭接功放 PH2.0 座
 *    麦克风和功放【共用】BCLK/WS 两根线 —— 所以录和放必须“分时复用”：
 *    录音时只开 I2S 接收通道，录完立刻 i2s_del_channel() 释放；
 *    播放时交给 ESP32-audioI2S 自己开 I2S 发送通道。两者绝不并行。
 *
 *  【预期现象】（串口 115200，每一步都有中文日志）
 *    [VOICE] 初始化完成: 后端=百度, PSRAM可用, 录音缓冲 96044 字节
 *    [VOICE] 开始录音 3 秒...
 *    [VOICE] 录音完成: 48000 采样, WAV=96044 字节, 平均电平=812 峰值=9031
 *    [ASR  ] 识别结果: 往前走一点
 *    [LLM  ] 回复原文: 好呀，我往前挪一点|FORWARD
 *    [LLM  ] 解析: 文本="好呀，我往前挪一点" 动作=FORWARD
 *    [TTS  ] 开始播放: http://tsn.baidu.com/text2audio?tex=... (长度 486)
 *    [TTS  ] 播放结束, 用时 2.4 秒
 *    喇叭里听到“好呀，我往前挪一点”，同时小车前进。
 *
 *  【常见错误】
 *    1) 编译报 "缺少 secrets.h" → 把 secrets.example.h 复制改名为 secrets.h
 *    2) 编译报找不到 Audio.h → Arduino IDE 库管理器装 "ESP32-audioI2S" (schreibfaul1)
 *    3) 编译报找不到 ArduinoJson.h → 库管理器装 "ArduinoJson" (Benoit Blanchon)
 *    4) 录音电平一直是 0~10（串口“平均电平”很小）→ 麦克风 L/R 没接 GND，
 *       或 SD 没插到 GPIO41，或 VDD 接到 5V（INMP441 接 5V 会烧）
 *    5) 录音电平很大但全是噪音、识别不出来 → 先跑 05_audio 测试 sketch 确认；
 *       若串口报 I2S 初始化失败 code=259，见文件末尾“待核实”第 2 条
 *    6) 百度 err_no=3301 音频质量差 / 3300 参数错 / 3302 鉴权失败
 *       → 3302 多半是 Key 错或没开通“短语音识别”
 *    7) 大模型返回 402 → DeepSeek 账户没余额；401 → Key 错
 *    8) 喇叭没声音 → 功放 SD 脚要接 3V3；VIN 接 5V（接 3V3 也能响但很小声）
 *    9) 播报是“沙沙”噪音 → TTS 返回的其实是错误 JSON（token 过期/额度用完），
 *       串口 [TTS] 那行会把 URL 打出来，用浏览器打开这个 URL 看看返回内容
 *
 *  【安全说明 · 必读】
 *    HTTPS 请求里用了 client.setInsecure()：不校验服务器证书。
 *    这只能用于“家里局域网 + 实验课”场景，它挡不住中间人攻击。
 *    要把机器人放到不可信网络，请改用根证书校验（setCACert）或证书指纹。
 *
 *  【依赖库】（Arduino IDE → 工具 → 管理库 里搜名字装）
 *    - ESP32-audioI2S   (schreibfaul1)   ：mp3/aac 解码 + I2S 播放
 *    - ArduinoJson      (Benoit Blanchon)：解析云端 JSON（v6/v7 都兼容）
 *    - 其余 WiFi/HTTPClient/I2S/SPIFFS 都是 ESP32 Arduino core 3.x 自带
 *  ==========================================================================*/

#pragma once

#include <Arduino.h>

/* --------------------------------------------------------------------------
 * 公共类型：不管是否编译语音功能都要有，主程序才不用到处写 #if
 * ------------------------------------------------------------------------*/
enum VoiceState : uint8_t {
  VOICE_IDLE = 0,     /* 待机：没在说话 */
  VOICE_LISTENING,    /* 倾听：正在录音 */
  VOICE_THINKING,     /* 思考：ASR + 大模型请求中 */
  VOICE_SPEAKING,     /* 说话：TTS 播放中 */
  VOICE_ERROR         /* 出错：看串口/网页提示 */
};

/* 状态回调：主程序接上这个回调去切换屏幕表情。
 * 注意：回调可能在 core0 的音频任务里被调用，里面只做“记一个状态变量”，
 *       不要在里面做网络请求或长延时。 */
typedef void (*VoiceStateCb)(VoiceState state, const String &detail);

/* 一次完整对话的结果 */
struct VoiceReply {
  bool   ok = false;      /* 整条链路是否成功 */
  String heard;           /* ASR 听到的文字 */
  String reply;           /* 大模型回答的文字（已去掉动作指令） */
  String action;          /* 动作指令（大写），解析失败为 "CHAT" */
  String error;           /* 失败原因（中文，可直接显示给用户） */
};

#ifndef ENABLE_VOICE
#define ENABLE_VOICE 1
#endif

#if ENABLE_VOICE

/* ============================ 依赖头文件 ================================= */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <driver/i2s_std.h>
#include <esp32-hal-psram.h>
#include <ArduinoJson.h>
#include <Audio.h>

#include "pinmap.h"          /* 引脚唯一来源：只用 PIN_* 宏 */

/* 找不到 secrets.h 时给出人话报错，而不是一堆看不懂的编译错误 */
#if !__has_include("secrets.h")
#error "缺少 firmware/07_main/secrets.h：请把 secrets.example.h 复制一份改名为 secrets.h，并填好 WiFi 和 Key"
#endif
#include "secrets.h"

/* ============================ 可调参数 =================================== */
#ifndef VOICE_REC_SECONDS
#define VOICE_REC_SECONDS     3       /* 一次录几秒（3 秒足够说一句话） */
#endif
#define VOICE_SAMPLE_RATE       16000   /* 百度短语音识别要求 16k 采样率 */
#define VOICE_SAMPLES           (VOICE_SAMPLE_RATE * VOICE_REC_SECONDS)
#define VOICE_PCM_BYTES         ((size_t)VOICE_SAMPLES * 2)   /* 16bit 单声道 */
#define VOICE_WAV_HDR_BYTES     44      /* 标准 WAV 文件头长度 */

#define VOICE_TTS_MAX_CHARS     100     /* 播报最多 100 个汉字（URL 别太长） */
#define VOICE_TTS_VOLUME        12      /* 0~21，12≈57%，防电流冲击（要求≤60%） */
#define VOICE_HTTP_TIMEOUT_MS   15000   /* 单次 HTTPS 请求超时 */
#define VOICE_PLAY_TIMEOUT_MS   30000   /* 单次播放最长 30 秒 */

/* 麦克风增益：INMP441 声音偏小可改成 2 或 3（会放大底噪，建议 1 或 2） */
#ifndef VOICE_MIC_GAIN
#define VOICE_MIC_GAIN          1
#endif

/* INMP441 的 L/R 接 GND = 左声道。绝大多数板子用 MONO 单声道模式即可；
 * 如果你的录音全是 0 或全是噪点，把下面改成 1 试试“双声道取左声道”模式。 */
#ifndef VOICE_MIC_STEREO
#define VOICE_MIC_STEREO        0
#endif

/* 后端编号自检 */
#if (VOICE_BACKEND != 1) && (VOICE_BACKEND != 2)
#error "VOICE_BACKEND 只能是 1(百度) 或 2(OpenAI兼容/SiliconFlow)，请看 secrets.h"
#endif

/* 调试日志开关（缺省打开；Key 会打码，不会泄露） */
#ifndef VOICE_DEBUG_LOG
#define VOICE_DEBUG_LOG 1
#endif
#if VOICE_DEBUG_LOG
#define VLOG(...) do { Serial.printf(__VA_ARGS__); } while (0)
#else
#define VLOG(...) do {} while (0)
#endif

/* 一些 secrets.h 里可能没写的调音参数，给个默认值 */
#ifndef BAIDU_DEV_PID
#define BAIDU_DEV_PID 1537
#endif
#ifndef BAIDU_TTS_PER
#define BAIDU_TTS_PER 0
#endif
#ifndef BAIDU_TTS_SPD
#define BAIDU_TTS_SPD 5
#endif
#ifndef BAIDU_TTS_PIT
#define BAIDU_TTS_PIT 5
#endif
#ifndef BAIDU_TTS_VOL
#define BAIDU_TTS_VOL 5
#endif
#ifndef LLM_MAX_TOKENS
#define LLM_MAX_TOKENS 120
#endif
#ifndef LLM_TEMPERATURE
#define LLM_TEMPERATURE 0.7
#endif
#ifndef SF_TTS_FORMAT
#define SF_TTS_FORMAT "mp3"
#endif
#ifndef SF_LLM_MODEL
#define SF_LLM_MODEL "Qwen/Qwen2.5-7B-Instruct"
#endif

/* ============================ 全局状态 =================================== */
static Audio           vc_audio;                 /* 播放器（mp3 解码 + I2S 输出） */
static VoiceStateCb    vc_stateCb      = nullptr;/* 状态回调（主程序注册） */
static VoiceState      vc_state        = VOICE_IDLE;
static volatile bool   vc_busy         = false;  /* 正在对话（主程序用来锁电机） */
static String          vc_lastError;             /* 最近一次错误（网页可显示） */

static uint8_t        *vc_wavBuf       = nullptr; /* PSRAM 里的 WAV 缓冲 */
static size_t          vc_wavBytes     = 0;       /* 有效 WAV 字节数（含 44 头） */
static uint32_t        vc_lastAvgLevel = 0;       /* 最近一次录音平均电平 */
static uint32_t        vc_lastPeakLevel= 0;       /* 最近一次录音峰值电平 */

/* 百度 access_token 缓存（有效期 30 天，没必要每句话都换一次） */
static char            vc_baiduToken[256] = {0};
static uint32_t        vc_baiduTokenExpireAt = 0;

/* 多轮对话记忆（只记 3 轮，只存文字，放堆里很小） */
#define VC_HISTORY_ROUNDS 3
static String          vc_histUser[VC_HISTORY_ROUNDS];
static String          vc_histBot[VC_HISTORY_ROUNDS];
static int             vc_histCount = 0;

/* ============================ 小工具 ===================================== */

static const char *voiceLastError() { return vc_lastError.c_str(); }
static bool voiceIsBusy() { return vc_busy; }
static VoiceState voiceState() { return vc_state; }
/* 最近一次录音的电平（网页 /api/status 可以显示，方便排查麦克风） */
static uint32_t voiceLastAvgLevel() { return vc_lastAvgLevel; }
static uint32_t voiceLastPeakLevel() { return vc_lastPeakLevel; }

static const char *voiceStateName(VoiceState s) {
  switch (s) {
    case VOICE_LISTENING: return "LISTENING";
    case VOICE_THINKING:  return "THINKING";
    case VOICE_SPEAKING:  return "SPEAKING";
    case VOICE_ERROR:     return "ERROR";
    default:              return "IDLE";
  }
}

static void vcNotify(VoiceState s, const String &detail) {
  vc_state = s;
  if (s == VOICE_ERROR) vc_lastError = detail;
  VLOG("[VOICE] 状态=%s %s\n", voiceStateName(s), detail.c_str());
  if (vc_stateCb) vc_stateCb(s, detail);   /* 交给主程序去换表情 */
}

/* 注册状态回调（主程序 setup 里调用一次） */
static void voiceSetStateCallback(VoiceStateCb cb) { vc_stateCb = cb; }

/* 打码：串口里只显示 Key 的前 4 位和后 2 位，防止截图泄露 */
static String vcMask(const char *s) {
  if (!s || !*s) return "(空)";
  String v(s);
  if (v.length() <= 8) return "****";
  return v.substring(0, 4) + "****" + v.substring(v.length() - 2);
}

/* secrets.h 里的值是否还是占位符（"★填这里..."） */
static bool vcIsFilled(const char *s) {
  if (!s || !*s) return false;
  if (strstr(s, "填这里") != nullptr) return false;
  return true;
}

/* UTF-8 百分号编码：中文一个字 3 字节 → 9 个 %XX。
 * 百度 TTS 的 tex 参数、以及自己拼 URL 时必须用它。 */
static String vcUrlEncode(const String &in) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3 + 8);
  const uint8_t *p = (const uint8_t *)in.c_str();
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    /* 只保留 RFC3986 的 unreserved 字符，其余全部编码 */
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

/* JSON 字符串转义：把 " \ 换行 等转义掉，中文原样保留（JSON 允许 UTF-8） */
static String vcJsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 32);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char tmp[8];
          snprintf(tmp, sizeof(tmp), "\\u%04x", (uint8_t)c);
          out += tmp;
        } else {
          out += c;   /* UTF-8 多字节原样放行 */
        }
    }
  }
  return out;
}

/* 按“字符”安全截断 UTF-8 字符串（不会把汉字砍成半个） */
static void vcUtf8Truncate(String &s, size_t maxChars) {
  size_t chars = 0, i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    if ((c & 0xC0) != 0x80) {           /* 只数“首字节” */
      if (chars >= maxChars) { s = s.substring(0, i); return; }
      chars++;
    }
    i++;
  }
}

/* ArduinoJson v6 / v7 的文档对象写法不一样，用宏屏蔽差异 */
#if ARDUINOJSON_VERSION_MAJOR >= 7
#define VC_JSON_DOC(name) JsonDocument name
#else
#define VC_JSON_DOC(name) DynamicJsonDocument name(8192)
#endif

/* ======================= HTTPS / HTTP 请求封装 =========================== */
/* 说明：所有云端 REST 请求都是 HTTPS，走 WiFiClientSecure + setInsecure()。
 * 静态复用同一个 TLS 客户端，省内存；云语音只在 core0 一个任务里跑，
 * 不会出现两个请求同时用它的竞态。 */

static bool vcHttpPrepare(HTTPClient &http, const String &url,
                          const char *contentType, const String &auth) {
  static WiFiClientSecure tls;
  tls.setInsecure();                 /* ← 不校验证书，仅限家用实验，见文件头 */
  if (!http.begin(tls, url)) return false;
  http.setTimeout(VOICE_HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (contentType) http.addHeader("Content-Type", contentType);
  if (auth.length() > 0) http.addHeader("Authorization", auth);
  return true;
}

/* GET，返回 HTTP 状态码；出错返回负数。resp 拿响应体。 */
static int vcHttpGet(const String &url, String &resp, const String &auth = "") {
  HTTPClient http;
  resp = "";
  if (!vcHttpPrepare(http, url, nullptr, auth)) {
    VLOG("[HTTP] begin 失败: %s\n", url.c_str());
    return -1;
  }
  int code = http.GET();
  if (code > 0) resp = http.getString();
  http.end();
  return code;
}

/* POST 原始字节（JSON / WAV / multipart 都走这里），返回状态码。 */
static int vcHttpPost(const String &url, const uint8_t *payload, size_t len,
                      const char *contentType, const String &auth, String &resp) {
  HTTPClient http;
  resp = "";
  if (!vcHttpPrepare(http, url, contentType, auth)) {
    VLOG("[HTTP] begin 失败: %s\n", url.c_str());
    return -1;
  }
  int code = http.POST(const_cast<uint8_t *>(payload), len);
  if (code > 0) resp = http.getString();
  http.end();
  return code;
}

/* 把响应体截短再打印，防止串口刷屏 */
static String vcShort(const String &s, size_t maxLen = 220) {
  if (s.length() <= maxLen) return s;
  return s.substring(0, maxLen) + "...(截断)";
}

/* ============================ 录音（I2S RX） ============================ */
/* 关键点：用 IDF 新 I2S API（driver/i2s_std.h），16kHz、32bit 槽宽，
 * 读出来右移 16 位变 int16（INMP441 是 24bit 左对齐）。录完必须
 * i2s_del_channel() 释放，否则 ESP32-audioI2S 播放时会抢不到 I2S。 */

/* 编码标准 WAV 头（44 字节，PCM 16bit 单声道 16kHz） */
static void vcBuildWavHeader(uint8_t *buf, uint32_t pcmBytes) {
  uint32_t chunkSize = 36 + pcmBytes;
  uint32_t sampleRate = VOICE_SAMPLE_RATE;
  uint32_t byteRate = sampleRate * 2;   /* 单声道 × 16bit */
  uint16_t blockAlign = 2;

  memcpy(buf + 0, "RIFF", 4);
  memcpy(buf + 4, &chunkSize, 4);
  memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4);
  uint32_t subChunk1 = 16;
  memcpy(buf + 16, &subChunk1, 4);      /* fmt 块长度 */
  uint16_t audioFormat = 1;             /* 1 = PCM */
  uint16_t channels = 1;
  memcpy(buf + 20, &audioFormat, 2);
  memcpy(buf + 22, &channels, 2);
  memcpy(buf + 24, &sampleRate, 4);
  memcpy(buf + 28, &byteRate, 4);
  memcpy(buf + 32, &blockAlign, 2);
  uint16_t bitsPerSample = 16;
  memcpy(buf + 34, &bitsPerSample, 2);
  memcpy(buf + 36, "data", 4);
  memcpy(buf + 40, &pcmBytes, 4);
}

/* 申请 WAV 缓冲：3 秒 = 48000 采样 × 2 字节 + 44 头 ≈ 96KB → 放 PSRAM */
static bool vcAllocWavBuf() {
  if (vc_wavBuf) return true;
  size_t need = VOICE_WAV_HDR_BYTES + VOICE_PCM_BYTES;
  if (psramFound()) {
    vc_wavBuf = (uint8_t *)ps_malloc(need);
  }
  if (!vc_wavBuf) {
    VLOG("[VOICE] PSRAM 不可用，录音缓冲改放内部 RAM（%u 字节）\n", (unsigned)need);
    vc_wavBuf = (uint8_t *)malloc(need);
  }
  if (!vc_wavBuf) {
    vc_lastError = "内存不足：申请 96KB 录音缓冲失败";
    VLOG("[VOICE] %s\n", vc_lastError.c_str());
    return false;
  }
  return true;
}

/* 立刻停掉播放，把 I2S 让给录音（分时复用铁律） */
static void vcAudioStopAndWait() {
  if (vc_audio.isRunning()) {
    vc_audio.stopSong();
  }
  /* 给 audio 库一点时间把 I2S 通道彻底还给系统 */
  for (int i = 0; i < 20 && vc_audio.isRunning(); i++) {
    vc_audio.loop();
    delay(10);
  }
  delay(50);
}

/* 录 3 秒 → 写入 PSRAM 里的 WAV。成功返回 true。 */
static bool vcRecord3s() {
  if (!vcAllocWavBuf()) return false;

  /* 播放中也能按下说话：先停喇叭再录音。
   * 【重要】ESP32-audioI2S 释放 I2S 通道的时机取决于库版本，偶尔会晚一点。
   * 这里做 3 次"停播放 + 等一下 + 建通道"重试，否则会出现"第 2 次说话录不到音"
   * （报 code=259 / ESP_ERR_INVALID_STATE）。见 docs/07_已知限制与待验证.md 第 2 节。          */
  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 8;
  chanCfg.dma_frame_num = 256;
  chanCfg.auto_clear    = true;

  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 3 && rx == nullptr; attempt++) {
    vcAudioStopAndWait();
    if (attempt) delay(120);                 /* 给它一点时间真正松开 I2S */
    err = i2s_new_channel(&chanCfg, nullptr, &rx);
    if (err != ESP_OK || rx == nullptr) { rx = nullptr; }
  }
  if (err != ESP_OK || rx == nullptr) {
    vc_lastError = String("I2S 录音通道创建失败(code=") + String((int)err) + ")";
    VLOG("[VOICE] %s\n", vc_lastError.c_str());
    return false;
  }

  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        VOICE_MIC_STEREO ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
  stdCfg.gpio_cfg.mclk = GPIO_NUM_NC;                    /* INMP441 不需要 MCLK */
  /* 【重要】必须显式指定取左声道：INMP441 的 L/R 脚接 GND = 左声道。
   * 不能依赖 I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG 的默认值——某些 IDF 版本
   * 在 MONO 模式下默认仍是 BOTH，可能直接初始化报错，或采到错的声道导致识别为空。
   * 见 docs/07_已知限制与待验证.md 第 2 节。                                                   */
  if (!VOICE_MIC_STEREO) {
    stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  }
  stdCfg.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
  stdCfg.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_WS;
  stdCfg.gpio_cfg.dout = GPIO_NUM_NC;                    /* 只听不说 */
  stdCfg.gpio_cfg.din  = (gpio_num_t)PIN_I2S_MIC_SD;
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv   = false;

  err = i2s_channel_init_std_mode(rx, &stdCfg);
  if (err != ESP_OK) {
    i2s_del_channel(rx);
    vc_lastError = String("I2S 初始化失败(code=") + String((int)err) + ")";
    VLOG("[VOICE] %s（若 code=259/ESP_ERR_INVALID_STATE，是 audio 库还占着 I2S）\n",
         vc_lastError.c_str());
    return false;
  }
  i2s_channel_enable(rx);

  /* 录音：32bit 读入 → 右移 16 位转 int16 */
  const int kChunkWords = 256;
  int32_t rawBuf[kChunkWords];
  int16_t *pcm = (int16_t *)(vc_wavBuf + VOICE_WAV_HDR_BYTES);
  size_t got = 0;
  uint64_t sumAbs = 0;
  uint32_t peak = 0;
  uint32_t t0 = millis();

  VLOG("[VOICE] 开始录音 %d 秒...\n", VOICE_REC_SECONDS);

  while (got < VOICE_SAMPLES) {
    size_t bytesRead = 0;
    err = i2s_channel_read(rx, rawBuf, sizeof(rawBuf), &bytesRead, pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
      VLOG("[VOICE] 读取失败(code=%d)，提前结束\n", (int)err);
      break;
    }
    size_t words = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < words && got < VOICE_SAMPLES; i++) {
#if VOICE_MIC_STEREO
      if ((i & 1) != 0) continue;               /* 立体声模式只取左声道 */
#endif
      int32_t s = rawBuf[i] >> 16;              /* 24bit 左对齐 → 取高 16 位 */
      if (VOICE_MIC_GAIN != 1) s *= VOICE_MIC_GAIN;
      if (s > 32767)  s = 32767;
      if (s < -32768) s = -32768;
      pcm[got++] = (int16_t)s;
      uint32_t a = (s < 0) ? (uint32_t)(-s) : (uint32_t)s;
      sumAbs += a;
      if (a > peak) peak = a;
    }
    if (millis() - t0 > (uint32_t)VOICE_REC_SECONDS * 1000 + 3000) {
      VLOG("[VOICE] 录音超时保护触发，按已录到的长度继续\n");
      break;
    }
    vTaskDelay(1);                               /* 让出 CPU，别把看门狗饿死 */
  }

  /* 必须释放：下一个用 I2S 的是 audio 库（播放） */
  i2s_channel_disable(rx);
  i2s_del_channel(rx);
  delay(30);

  if (got < VOICE_SAMPLE_RATE / 2) {             /* 少于 0.5 秒视为失败 */
    vc_lastError = "录音失败：只读到很少的数据（检查麦克风接线）";
    VLOG("[VOICE] %s\n", vc_lastError.c_str());
    return false;
  }

  uint32_t pcmBytes = (uint32_t)got * 2;
  vcBuildWavHeader(vc_wavBuf, pcmBytes);
  vc_wavBytes = VOICE_WAV_HDR_BYTES + pcmBytes;
  vc_lastAvgLevel  = (uint32_t)(sumAbs / (got ? got : 1));
  vc_lastPeakLevel = peak;

  VLOG("[VOICE] 录音完成: %u 采样, WAV=%u 字节, 平均电平=%u 峰值=%u\n",
       (unsigned)got, (unsigned)vc_wavBytes, (unsigned)vc_lastAvgLevel,
       (unsigned)vc_lastPeakLevel);
  if (vc_lastPeakLevel < 50) {
    VLOG("[VOICE] ⚠ 电平太小：麦克风 L/R 要接 GND，SD 接 GPIO41，VDD 接 3V3\n");
  }
  return true;
}

/* ============================ 播放（I2S TX） ============================ */
/* 交给 ESP32-audioI2S：它自己开 I2S 发送通道、解 mp3、喂 DMA。
 * 注意 loop() 必须持续调用，否则声音会卡顿。 */

static void vcAudioSetup() {
  /* 参数：BCLK, LRC(WS), DOUT。MCLK 不用（默认 I2S_GPIO_UNUSED）
   * 新版库 setPinout 返回 bool，老版返回 void —— 这里故意不接返回值，两个版本都能编。 */
  vc_audio.setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_AMP_DIN);
  vc_audio.setVolume(VOICE_TTS_VOLUME);
}

/* 等待播放开始（最多 5 秒），成功返回 true */
static bool vcWaitStart(uint32_t timeoutMs = 5000) {
  uint32_t t0 = millis();
  while (!vc_audio.isRunning() && millis() - t0 < timeoutMs) {
    vc_audio.loop();
    delay(5);
  }
  return vc_audio.isRunning();
}

/* 播放直到结束（带超时保护）。正常播完返回 true。 */
static bool vcWaitFinish(uint32_t timeoutMs = VOICE_PLAY_TIMEOUT_MS) {
  uint32_t t0 = millis();
  while (vc_audio.isRunning()) {
    vc_audio.loop();
    delay(1);
    if (millis() - t0 > timeoutMs) {
      VLOG("[TTS  ] 播放超时，强制停止\n");
      vc_audio.stopSong();
      vc_lastError = "TTS 播放超时";
      return false;
    }
  }
  vc_audio.stopSong();
  VLOG("[TTS  ] 播放结束, 用时 %.1f 秒\n", (millis() - t0) / 1000.0f);
  delay(100);   /* 给 audio 库释放 I2S 的时间，之后录音才不会抢不到 */
  return true;
}

/* 播放一个 URL（http 流式：百度 TTS 走这条，省内存不落盘） */
static bool vcPlayUrl(const String &url) {
  vc_audio.stopSong();
  vc_lastError = "";
  VLOG("[TTS  ] 开始播放: %s (长度 %u)\n", vcShort(url, 120).c_str(), (unsigned)url.length());
  vc_audio.connecttohost(url.c_str());   /* 返回值在 v2/v3 都可用，但为兼容老版不接 */
  if (!vcWaitStart()) {
    vc_lastError = "TTS 播放失败：URL 打不开（浏览器打开串口那行 URL 看返回内容）";
    VLOG("[TTS  ] %s\n", vc_lastError.c_str());
    return false;
  }
  return vcWaitFinish();
}

/* 播放 SPIFFS 里的本地文件（SiliconFlow TTS 走这条：先下载 mp3 再播） */
static bool vcPlayFile(const char *path) {
  vc_audio.stopSong();
  vc_lastError = "";
  VLOG("[TTS  ] 开始播放本地文件: %s\n", path);
  vc_audio.connecttoFS(SPIFFS, path);
  if (!vcWaitStart()) {
    vc_lastError = String("本地音频打不开/解码失败: ") + path;
    VLOG("[TTS  ] %s\n", vc_lastError.c_str());
    return false;
  }
  return vcWaitFinish();
}

/* ==================== 后端 1：百度智能云（ASR + TTS） =================== */
#if VOICE_BACKEND == 1

/* 流程：① oauth2 token（30 天有效，缓存）② POST WAV 到 vop.baidu.com */
static bool vcBaiduEnsureToken(bool force) {
  uint32_t now = millis();
  /* 用带符号减法判断过期，避免 millis() 49 天溢出问题 */
  if (!force && vc_baiduToken[0] != '\0' &&
      (int32_t)(vc_baiduTokenExpireAt - now) > 0) {
    return true;
  }

  String url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials";
  url += "&client_id=";     url += BAIDU_API_KEY;
  url += "&client_secret="; url += BAIDU_SECRET_KEY;

  VLOG("[ASR  ] 换百度 access_token ...\n");
  String resp;
  int code = vcHttpGet(url, resp);
  if (code != 200) {
    vc_lastError = String("百度 token 请求失败 HTTP=") + String(code);
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }

  VC_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) {
    vc_lastError = "百度 token 返回的不是 JSON";
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  const char *tok = doc["access_token"] | "";
  int expiresIn = doc["expires_in"] | 0;
  if (tok[0] == '\0') {
    const char *ec = doc["error"] | "unknown";
    const char *ed = doc["error_description"] | "";
    vc_lastError = String("百度 token 失败: ") + ec + " " + ed;
    VLOG("[ASR  ] %s（检查 BAIDU_API_KEY / BAIDU_SECRET_KEY 是否贴反）\n",
         vc_lastError.c_str());
    return false;
  }

  strncpy(vc_baiduToken, tok, sizeof(vc_baiduToken) - 1);
  vc_baiduToken[sizeof(vc_baiduToken) - 1] = '\0';
  /* 提前 10 分钟过期，避免刚好卡在边界 */
  uint32_t ms = (expiresIn > 600 ? (uint32_t)expiresIn - 600 : 60) * 1000UL;
  vc_baiduTokenExpireAt = now + ms;
  VLOG("[ASR  ] token 获取成功（%d 秒有效，已缓存）\n", expiresIn);
  return true;
}

static bool vcBaiduAsr(String &outText) {
  outText = "";
  if (!vcBaiduEnsureToken(false)) return false;
  if (vc_wavBytes <= VOICE_WAV_HDR_BYTES) {
    vc_lastError = "还没有录音数据，无法识别";
    return false;
  }

  String url = "https://vop.baidu.com/server_api?cuid=";
  url += BAIDU_CUID;
  url += "&token="; url += vc_baiduToken;
  url += "&dev_pid="; url += String(BAIDU_DEV_PID);

  VLOG("[ASR  ] 上传 WAV(%u 字节) 到百度短语音识别 ...\n", (unsigned)vc_wavBytes);
  String resp;
  int code = vcHttpPost(url, vc_wavBuf, vc_wavBytes,
                        "audio/wav; rate=16000", "", resp);
  if (code != 200) {
    vc_lastError = String("百度 ASR 请求失败 HTTP=") + String(code);
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }

  VC_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) {
    vc_lastError = "百度 ASR 返回的不是 JSON";
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  int errNo = doc["err_no"] | -1;
  const char *errMsg = doc["err_msg"] | "";
  if (errNo != 0) {
    vc_lastError = String("百度 ASR err_no=") + String(errNo) + " " + errMsg;
    VLOG("[ASR  ] %s（3301=听不清/太吵, 3302=鉴权失败, 3307=服务端忙）\n",
         vc_lastError.c_str());
    return false;
  }
  const char *r0 = doc["result"][0] | "";
  outText = String(r0);
  outText.trim();
  if (outText.length() == 0) {
    vc_lastError = "百度 ASR 识别结果为空（没听到人说话？）";
    VLOG("[ASR  ] %s\n", vc_lastError.c_str());
    return false;
  }
  VLOG("[ASR  ] 识别结果: %s\n", outText.c_str());
  return true;
}

/* TTS：URL 直接丢给 audioI2S 的 connecttohost 流式播放（http，不走 TLS 省内存）。
 * 注意：ASR 的 token 参数名是 token，TTS 的是 tok —— 百度历史遗留，不是笔误。 */
static bool vcBaiduTts(const String &textIn) {
  String text = textIn;
  text.trim();
  vcUtf8Truncate(text, VOICE_TTS_MAX_CHARS);
  if (text.length() == 0) return false;
  if (!vcBaiduEnsureToken(false)) return false;   /* TTS 与 ASR 共用同一个 token */

  String url = "http://tsn.baidu.com/text2audio?tex=";
  url += vcUrlEncode(text);
  url += "&lan=zh&cuid="; url += BAIDU_CUID;
  url += "&ctp=1&tok=";   url += vc_baiduToken;
  url += "&per="; url += String(BAIDU_TTS_PER);
  url += "&spd="; url += String(BAIDU_TTS_SPD);
  url += "&pit="; url += String(BAIDU_TTS_PIT);
  url += "&vol="; url += String(BAIDU_TTS_VOL);
  url += "&aue=3";          /* 3 = mp3，audioI2S 直接解码 */

  return vcPlayUrl(url);
}

#endif  /* VOICE_BACKEND == 1 */

/* ==================== 后端 2：SiliconFlow（ASR + TTS） =================== */
#if VOICE_BACKEND == 2
/* ASR：multipart/form-data POST /v1/audio/transcriptions
 * TTS：POST /v1/audio/speech 拿 mp3 → 存 SPIFFS → connecttoFS 播放
 * 文档：https://docs.siliconflow.cn/docs/userguide/capabilities/text-to-speech */

static bool vcSiliconFlowAsr(String &outText) {
  outText = "";
  if (vc_wavBytes <= VOICE_WAV_HDR_BYTES) {
    vc_lastError = "还没有录音数据，无法识别";
    return false;
  }

  const char *boundary = "----AIRobotBoundary7d1f3a";
  String head;
  head.reserve(400);
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  head += SF_ASR_MODEL; head += "\r\n";
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  size_t total = head.length() + vc_wavBytes + tail.length();
  uint8_t *body = (uint8_t *)(psramFound() ? ps_malloc(total) : malloc(total));
  if (!body) {
    vc_lastError = "内存不足：拼不出 ASR 上传包";
    VLOG("[ASR  ] %s (需要 %u 字节)\n", vc_lastError.c_str(), (unsigned)total);
    return false;
  }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), vc_wavBuf, vc_wavBytes);
  memcpy(body + head.length() + vc_wavBytes, tail.c_str(), tail.length());

  String url = String(SF_BASE_URL) + "/audio/transcriptions";
  String ctype = String("multipart/form-data; boundary=") + boundary;
  String auth = String("Bearer ") + SF_API_KEY;

  VLOG("[ASR  ] 上传 WAV(%u 字节) 到 SiliconFlow ...\n", (unsigned)vc_wavBytes);
  String resp;
  int code = vcHttpPost(url, body, total, ctype.c_str(), auth, resp);
  free(body);
  if (code != 200) {
    vc_lastError = String("SiliconFlow ASR 请求失败 HTTP=") + String(code);
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }

  VC_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) {
    vc_lastError = "SiliconFlow ASR 返回的不是 JSON";
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  const char *txt = doc["text"] | "";
  outText = String(txt);
  outText.trim();
  if (outText.length() == 0) {
    vc_lastError = "SiliconFlow ASR 识别结果为空";
    VLOG("[ASR  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  VLOG("[ASR  ] 识别结果: %s\n", outText.c_str());
  return true;
}

static bool vcSiliconFlowTts(const String &textIn) {
  String text = textIn;
  text.trim();
  vcUtf8Truncate(text, VOICE_TTS_MAX_CHARS);
  if (text.length() == 0) return false;

  String body;
  body.reserve(400);
  body += "{\"model\":\""; body += vcJsonEscape(SF_TTS_MODEL);
  body += "\",\"input\":\""; body += vcJsonEscape(text);
  body += "\",\"voice\":\""; body += vcJsonEscape(SF_TTS_VOICE);
  body += "\",\"response_format\":\""; body += SF_TTS_FORMAT;
  body += "\",\"speed\":1.0}";

  String url = String(SF_BASE_URL) + "/audio/speech";
  String auth = String("Bearer ") + SF_API_KEY;

  VLOG("[TTS  ] 请求 SiliconFlow 合成语音（%u 字节文本）...\n", (unsigned)text.length());

  /* 这里不能像 ASR 那样把响应收进 String（mp3 有几十~几百 KB），
   * 必须边收边写 SPIFFS。用 HTTPClient 的流式接口。 */
  HTTPClient http;
  if (!vcHttpPrepare(http, url, "application/json", auth)) {
    vc_lastError = "TTS 请求初始化失败";
    return false;
  }
  int code = http.POST(const_cast<uint8_t *>((const uint8_t *)body.c_str()), body.length());
  if (code != 200) {
    String resp = http.getString();
    vc_lastError = String("SiliconFlow TTS 请求失败 HTTP=") + String(code);
    VLOG("[TTS  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    http.end();
    return false;
  }

  if (!SPIFFS.begin(true)) {
    vc_lastError = "SPIFFS 挂载失败（分区表要带 SPIFFS）";
    VLOG("[TTS  ] %s\n", vc_lastError.c_str());
    http.end();
    return false;
  }
  File f = SPIFFS.open("/tts.mp3", FILE_WRITE);
  if (!f) {
    vc_lastError = "打不开 SPIFFS:/tts.mp3（空间满？）";
    VLOG("[TTS  ] %s\n", vc_lastError.c_str());
    http.end();
    return false;
  }
  int written = http.writeToStream(&f);   /* 把 mp3 流原样写进文件 */
  size_t fileSize = f.size();
  f.close();
  http.end();

  if (written <= 0 || fileSize < 1024) {
    vc_lastError = String("TTS 音频下载失败(写入 ") + String(written) + " 字节)";
    VLOG("[TTS  ] %s\n", vc_lastError.c_str());
    return false;
  }
  VLOG("[TTS  ] 下载完成 %u 字节，开始播放\n", (unsigned)fileSize);
  bool ok = vcPlayFile("/tts.mp3");
  SPIFFS.remove("/tts.mp3");   /* 播完就删，别占 flash */
  return ok;
}

#endif  /* VOICE_BACKEND == 2 */

/* ============================ 后端分发 ================================== */
static bool vcAsr(String &text) {
#if VOICE_BACKEND == 2
  return vcSiliconFlowAsr(text);
#else
  return vcBaiduAsr(text);
#endif
}

static bool vcTts(const String &text) {
#if VOICE_BACKEND == 2
  return vcSiliconFlowTts(text);
#else
  return vcBaiduTts(text);
#endif
}

/* ============================ LLM 大模型 ================================ */
/* OpenAI 兼容 /chat/completions。默认 DeepSeek：
 *   base=https://api.deepseek.com  model=deepseek-chat
 * 系统提示词强制它只输出一行 "回复文本|动作指令"，动作只能从 11 个枚举里选。 */

static const char *VC_SYSTEM_PROMPT =
    "你叫小派，是一台桌面机器人小车的中文语音助手。你说话要短、口语化，"
    "每次回复不超过30个汉字。"
    "你必须严格只输出一行文本，格式为：回复文本|动作指令 。"
    "动作指令只能从下面 11 个英文单词里选一个，不能自创："
    "FORWARD(前进), BACK(后退), LEFT(左转), RIGHT(右转), STOP(停止), "
    "PAN_LEFT(摄像头/云台向左转), PAN_RIGHT(云台向右转), TILT_UP(抬头), "
    "TILT_DOWN(低头), LOOK(只是看看/回应), CHAT(纯聊天，不动)。"
    "规则：1) 不需要移动就用 CHAT。2) 回复文本里绝对不能出现竖线|，不能换行，"
    "不能加解释、不能加括号说明。3) 用户说前进/往前/走 → FORWARD；"
    "退/后 → BACK；左转/往左 → LEFT；右转/往右 → RIGHT；停/别动 → STOP；"
    "看左边/转过去看看 → PAN_LEFT；看右边 → PAN_RIGHT；抬头/往上看 → TILT_UP；"
    "低头/往下看 → TILT_DOWN。4) 前方危险或有障碍 → STOP。"
    "例子：用户说“往前走一点”，你输出：好呀，我往前挪一点|FORWARD";

/* 大模型用哪家/哪个 Key：
 * - backend 1：用 secrets.h 里的 LLM_*（默认 DeepSeek）
 * - backend 2：如果 LLM_API_KEY 没填，自动借用 SiliconFlow 的 Key 和模型，
 *              这样“一个 Key 全包”真的能跑起来。 */
static const char *vcLlmApiKey() {
#if VOICE_BACKEND == 2
  if (!vcIsFilled(LLM_API_KEY) && vcIsFilled(SF_API_KEY)) return SF_API_KEY;
#endif
  return LLM_API_KEY;
}

static String vcLlmBaseUrl() {
#if VOICE_BACKEND == 2
  if (!vcIsFilled(LLM_API_KEY) && vcIsFilled(SF_API_KEY)) return String(SF_BASE_URL);
#endif
  return String(LLM_BASE_URL);
}

static const char *vcLlmModelName() {
#if VOICE_BACKEND == 2
  if (!vcIsFilled(LLM_API_KEY) && vcIsFilled(SF_API_KEY)) return SF_LLM_MODEL;
#endif
  return LLM_MODEL;
}

/* 拼 chat/completions 的 URL：base 已带 /v1 就不重复加 */
static String vcChatUrl() {
  String base = vcLlmBaseUrl();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (base.endsWith("/v1")) return base + "/chat/completions";
  return base + "/v1/chat/completions";
}

/* 动作枚举校验 */
static bool vcIsValidAction(const String &a) {
  static const char *acts[11] = {"FORWARD", "BACK", "LEFT", "RIGHT", "STOP",
                                 "PAN_LEFT", "PAN_RIGHT", "TILT_UP", "TILT_DOWN",
                                 "LOOK", "CHAT"};
  for (int i = 0; i < 11; i++) {
    if (a == acts[i]) return true;
  }
  return false;
}

/* 把模型回复拆成“文本 + 动作”。容错：找不到竖线就当纯聊天。 */
static void vcParseReply(const String &rawIn, String &text, String &action) {
  String raw = rawIn;
  raw.replace("`", "");            /* 模型偶尔会加 markdown 反引号 */
  raw.replace("\r", " ");
  raw.replace("\n", " ");
  raw.trim();
  if (raw.length() >= 2 && raw.startsWith("\"") && raw.endsWith("\"")) {
    raw = raw.substring(1, raw.length() - 1);
    raw.trim();
  }

  int bar = raw.lastIndexOf('|');  /* 用最后一个竖线，容忍文本里的竖线 */
  if (bar < 0) {
    text = raw;
    action = "CHAT";
    VLOG("[LLM  ] 没找到 '|'，按纯聊天处理\n");
  } else {
    String t = raw.substring(0, bar);
    String a = raw.substring(bar + 1);
    t.trim();
    a.trim();
    a.toUpperCase();
    int paren = a.indexOf('(');   /* 去掉 "FORWARD(前进)" 这种括号说明 */
    if (paren > 0) {
      a = a.substring(0, paren);
      a.trim();
    }
    if (vcIsValidAction(a)) {
      text = t;
      action = a;
    } else {
      VLOG("[LLM  ] 动作 \"%s\" 不在枚举里，回退成 CHAT\n", a.c_str());
      text = raw;
      action = "CHAT";
    }
  }
  text.trim();
  if (text.length() == 0) text = "好的";
  vcUtf8Truncate(text, VOICE_TTS_MAX_CHARS);
}

/* 发一次大模型请求，成功时 rawOut = 模型输出的整行（未拆分） */
static bool vcLlmChat(const String &userText, String &rawOut) {
  rawOut = "";

  String body;
  body.reserve(3072);
  body += "{\"model\":\"";        body += vcJsonEscape(vcLlmModelName());
  body += "\",\"messages\":[";
  body += "{\"role\":\"system\",\"content\":\"";
  body += vcJsonEscape(VC_SYSTEM_PROMPT);
  body += "\"}";
  for (int i = 0; i < vc_histCount; i++) {          /* 带上前几轮的记忆 */
    body += ",{\"role\":\"user\",\"content\":\"";
    body += vcJsonEscape(vc_histUser[i]);
    body += "\"},{\"role\":\"assistant\",\"content\":\"";
    body += vcJsonEscape(vc_histBot[i]);
    body += "\"}";
  }
  body += ",{\"role\":\"user\",\"content\":\"";
  body += vcJsonEscape(userText);
  body += "\"}],\"max_tokens\":";
  body += String(LLM_MAX_TOKENS);
  body += ",\"temperature\":";
  body += String(LLM_TEMPERATURE, 2);
  body += ",\"stream\":false}";

  String auth = String("Bearer ") + vcLlmApiKey();
  String url = vcChatUrl();
  VLOG("[LLM  ] POST %s model=%s max_tokens=%d\n",
       url.c_str(), vcLlmModelName(), (int)LLM_MAX_TOKENS);

  String resp;
  int code = vcHttpPost(url, (const uint8_t *)body.c_str(), body.length(),
                        "application/json", auth, resp);
  if (code != 200) {
    vc_lastError = String("大模型请求失败 HTTP=") + String(code);
    VLOG("[LLM  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }

  VC_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) {
    vc_lastError = "大模型返回的不是 JSON";
    VLOG("[LLM  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  /* 有的服务出错时也返回 200，错误在 error 字段里 */
  const char *errMsg = doc["error"]["message"] | "";
  if (errMsg[0] != '\0') {
    vc_lastError = String("大模型返回错误: ") + errMsg;
    VLOG("[LLM  ] %s\n", vc_lastError.c_str());
    return false;
  }
  const char *content = doc["choices"][0]["message"]["content"] | "";
  if (content[0] == '\0') {
    vc_lastError = "大模型回复为空（可能是 max_tokens 太小或被限流）";
    VLOG("[LLM  ] %s: %s\n", vc_lastError.c_str(), vcShort(resp).c_str());
    return false;
  }
  rawOut = String(content);
  rawOut.trim();
  VLOG("[LLM  ] 回复原文: %s\n", rawOut.c_str());
  return true;
}

/* 记住一轮对话（供下一句参考），最多 3 轮 */
static void vcHistoryPush(const String &userText, const String &botText) {
  if (vc_histCount >= VC_HISTORY_ROUNDS) {
    for (int i = 1; i < VC_HISTORY_ROUNDS; i++) {
      vc_histUser[i - 1] = vc_histUser[i];
      vc_histBot[i - 1]  = vc_histBot[i];
    }
    vc_histCount = VC_HISTORY_ROUNDS - 1;
  }
  vc_histUser[vc_histCount] = userText;
  vc_histBot[vc_histCount]  = botText;
  vc_histCount++;
}

/* 清空对话记忆（网页上点“忘掉刚才”可以调） */
static void voiceClearHistory() {
  for (int i = 0; i < VC_HISTORY_ROUNDS; i++) {
    vc_histUser[i] = "";
    vc_histBot[i]  = "";
  }
  vc_histCount = 0;
}

/* ============================ 对外主流程 ================================ */

/* 初始化：打印配置、设好喇叭引脚和音量、必要时挂 SPIFFS。
 * 在 setup() 里、WiFi 连接之后调用一次。返回值仅表示“本模块初始化是否正常”。 */
static bool voiceSetup() {
  Serial.println();
  Serial.println(F("========== 云语音模块初始化 =========="));
  Serial.printf("[VOICE] 后端 = %d (%s)\n", VOICE_BACKEND,
                VOICE_BACKEND == 2 ? "OpenAI兼容/SiliconFlow" : "百度智能云");
  Serial.printf("[VOICE] 大模型 = %s @ %s\n", vcLlmModelName(), vcLlmBaseUrl().c_str());
#if VOICE_BACKEND == 2
  Serial.printf("[VOICE] SiliconFlow Key = %s\n", vcMask(SF_API_KEY).c_str());
#else
  Serial.printf("[VOICE] 百度 Key = %s / %s\n",
                vcMask(BAIDU_API_KEY).c_str(), vcMask(BAIDU_SECRET_KEY).c_str());
#endif
  Serial.printf("[VOICE] 录音参数 = %d Hz / 16bit / 单声道 / %d 秒\n",
                VOICE_SAMPLE_RATE, VOICE_REC_SECONDS);
  Serial.printf("[VOICE] PSRAM = %s\n", psramFound() ? "可用" : "不可用");

  if (!vcAllocWavBuf()) return false;

  vcAudioSetup();   /* 告诉 audio 库：BCLK/WS/DIN 三根线在哪，音量 12/21 */

#if VOICE_BACKEND == 2
  if (!SPIFFS.begin(true)) {
    Serial.println(F("[VOICE] ⚠ SPIFFS 挂载失败：后端 2 的 TTS 需要 SPIFFS 分区"));
  }
#endif
  Serial.printf("[VOICE] 初始化完成: 录音缓冲 %u 字节\n",
                (unsigned)(VOICE_WAV_HDR_BYTES + VOICE_PCM_BYTES));
  if (!vcIsFilled(vcLlmApiKey())) {
    Serial.println(F("[VOICE] ⚠ 大模型 Key 还没填，通话会失败（见 secrets.h）"));
  }
  return true;
}

/* 只说话（不录音、不联网思考）。返回是否播放成功。
 * 可以用在“开机打招呼”、网页上的文字播报等场景。 */
static bool voiceSpeak(const String &text) {
  if (WiFi.status() != WL_CONNECTED) {
    vc_lastError = "WiFi 未连接，无法语音合成";
    return false;
  }
  vcNotify(VOICE_SPEAKING, text);
  bool ok = vcTts(text);
  delay(120);
  vcNotify(ok ? VOICE_IDLE : VOICE_ERROR, ok ? String("") : vc_lastError);
  return ok;
}

/* 完整对话：录音 → ASR → LLM → 解析 → TTS。阻塞式，约 6~15 秒。
 * 注意：本函数应该在 core0 的音频任务里调用（主程序负责），不要放在 WiFi 回调里。 */
static bool voiceDialog(VoiceReply &out) {
  out = VoiceReply();
  if (vc_busy) {
    out.error = "上一句还没说完";
    return false;
  }
  vc_busy = true;
  vc_lastError = "";

  if (WiFi.status() != WL_CONNECTED) {
    out.error = "WiFi 未连接";
    vcNotify(VOICE_ERROR, out.error);
    vc_busy = false;
    return false;
  }

  bool ok = false;
  do {
    /* 1) 录音 */
    vcNotify(VOICE_LISTENING, "正在听...（录音 3 秒）");
    if (!vcRecord3s()) { out.error = vc_lastError; break; }

    /* 2) 云端语音识别 */
    vcNotify(VOICE_THINKING, "识别中...");
    if (!vcAsr(out.heard)) { out.error = vc_lastError; break; }

    /* 3) 大模型思考 */
    vcNotify(VOICE_THINKING, "思考中...");
    String raw;
    if (!vcLlmChat(out.heard, raw)) { out.error = vc_lastError; break; }

    /* 4) 拆出“说的话”和“动作” */
    vcParseReply(raw, out.reply, out.action);
    vcHistoryPush(out.heard, out.reply);
    VLOG("[LLM  ] 解析: 文本=\"%s\" 动作=%s\n", out.reply.c_str(), out.action.c_str());

    /* 5) 播报（TTS 失败不算整条链路失败，文字回复仍然有效） */
    vcNotify(VOICE_SPEAKING, out.reply);
    if (!vcTts(out.reply)) {
      VLOG("[TTS  ] ⚠ 播报失败: %s（文字回复照常执行）\n", vc_lastError.c_str());
    }
    ok = true;
  } while (false);

  if (ok) {
    out.ok = true;
    vcNotify(VOICE_IDLE, "");
  } else {
    vcNotify(VOICE_ERROR, out.error);
  }
  delay(100);
  vc_busy = false;
  return ok;
}

/* 自检：不开麦克风也能验证 TTS 链路（网页“调试”或串口命令里可以用） */
static bool voiceSelfTestTts() {
  return voiceSpeak("语音模块自检通过，我准备好啦");
}

#else  /* ================= ENABLE_VOICE = 0：空实现，主程序照常编译 ========= */

static inline bool voiceSetup() { return false; }
static inline void voiceSetStateCallback(VoiceStateCb) {}
static inline bool voiceDialog(VoiceReply &r) {
  r = VoiceReply();
  r.error = "语音功能已关闭（ENABLE_VOICE=0）";
  return false;
}
static inline bool voiceSpeak(const String &) { return false; }
static inline bool voiceSelfTestTts() { return false; }
static inline void voiceClearHistory() {}
static inline bool voiceIsBusy() { return false; }
static inline VoiceState voiceState() { return VOICE_IDLE; }
static inline uint32_t voiceLastAvgLevel() { return 0; }
static inline uint32_t voiceLastPeakLevel() { return 0; }
static inline const char *voiceLastError() { return "语音功能已关闭"; }

#endif  /* ENABLE_VOICE */

/* ============================================================================
 * 【待实测核实清单】（我不确定的 API 细节都写在这里，不硬编）
 * 1) INMP441 单声道读取：L/R 接 GND 时数据在左声道。这里用
 *    I2S_SLOT_MODE_MONO + 32bit 槽宽，按 IDF 文档“单声道模式只取有效通道”。
 *    如果实际录到的是 0 或半速噪音，把 VOICE_MIC_STEREO 改成 1
 *    （立体声模式 + 取左声道）再试。请用 05_audio 的测试 sketch 对一遍。
 * 2) ESP32-audioI2S 版本差异：本文件用了 setPinout / setVolume / connecttohost /
 *    connecttoFS / loop / isRunning / stopSong（v2/v3 都存在）。新版 v3 用
 *    NetworkClient 并自带解码任务，旧版 v2 用 WiFiClient。这里故意不接
 *    connecttohost/connecttoFS 的返回值，避免老版返回 void 时编译不过。
 *    另外“stopSong 之后 audio 库是否彻底 i2s_del_channel 释放 I2S”需要实测：
 *    若串口出现 I2S 初始化失败 code=259(ESP_ERR_INVALID_STATE)，说明没释放，
 *    需要在 vcAudioStopAndWait() 里加长等待或改用独立 I2S 端口（I2S_NUM_1）。
 * 3) 百度 TTS 参数：本文件在要求的 tex/lan/cuid/ctp/tok/per/spd/pit/vol 之外
 *    加了 &aue=3（强制 mp3）。百度文档现在建议显式指定 aue；如果返回的不是
 *    mp3（audioI2S 解码失败），把 aue 去掉或改官方值再试。
 *    还有：ASR 的 token 参数名是 token，TTS 的是 tok（百度历史遗留，不是笔误）。
 * 4) SiliconFlow 模型名会变：SF_ASR_MODEL / SF_TTS_MODEL / SF_TTS_VOICE 以
 *    https://cloud.siliconflow.cn/models?types=speech 当时在线的为准。
 * 5) HTTPClient::writeToStream(&File) 在当前 core 3.x 是公开函数；若某版本
 *    编译报无此成员，改用手写循环：getStreamPtr() + readBytes 写入 File。
 * 6) SPIFFS：backend 2 的 TTS 需要分区表里给 SPIFFS 留空间。Arduino IDE
 *    默认 “Default 4MB with spiffs” 就够；如果用户选了 “Huge APP” 分区表
 *    会没有 SPIFFS，需提示改分区或改用 LittleFS。
 * 7) 录音 3 秒的时长：32000 字节/秒 × 3 = 96KB，放 PSRAM 没问题；
 *    若 PSRAM 初始化失败退到内部 RAM，要确认摄像头缓冲占用后还有 96KB 连续堆。
 * 8) 播报 URL 长度：百度 TTS 把中文百分号编码后每个汉字 9 字节，100 汉字约
 *    900+ 字节 URL。audioI2S 的 URL 缓冲是动态的（ps_ptr），理论上够；若实测
 *    长句播不出来，把 VOICE_TTS_MAX_CHARS 调到 60。
 * 9) 编译期依赖：ESP32 Arduino core 3.x + ESP32-audioI2S v3.x（v2.x 也可，
 *    但 v2.x 没有 setInsecure/NetworkClient 那些，属于音频库内部差异，不影响本文件）。
 * ==========================================================================*/
