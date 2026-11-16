/* ============================================================================
 *  AI-Robot · 07_main · 密钥与网络配置【模板】
 *  文件: secrets.example.h        （随仓库分发，只有占位符，永远不含真实 Key）
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    把 WiFi 账号、云端 API Key 全部集中在这一个文件里，代码里不再出现任何
 *    字符串常量。你（零代码基础）只需要改这一个文件，就能把机器人连上云端。
 *
 *  【怎么用】（3 步，2 分钟）
 *    1) 在电脑上把本文件【复制一份】，改名为  secrets.h
 *       位置必须是:  firmware\07_main\secrets.h
 *       （Arduino IDE 里这个文件夹叫 07_main，是主程序 sketch 的根目录）
 *    2) 用记事本/Arduino IDE 打开 secrets.h，把下面标着 "★填这里" 的地方
 *       改成你自己的内容。引号要保留，例如:
 *           #define WIFI_SSID  "我的路由器名"
 *    3) 保存，然后把整个 07_main 烧进板子。
 *
 *  【绝对不要做的事】
 *    - 不要把填好 Key 的 secrets.h 发到群里、GitHub、网盘。
 *      仓库里只有 secrets.example.h；如果以后用 git，请在 .gitignore 里加一行:
 *          firmware/07_main/secrets.h
 *    - 不要改这个模板文件本身（它是给所有人看的样板）。
 *
 *  【预期现象】
 *    secrets.h 填对后，串口会打印:
 *      [CFG] WiFi=你的路由器名  VOICE_BACKEND=1(百度)
 *      [CFG] LLM=deepseek-chat @ https://api.deepseek.com
 *      [WiFi] 已连接, IP=192.168.x.x
 *    如果 Key 填错，串口会打印云端返回的具体错误（如 invalid_client / 401），
 *    照着【常见错误】改即可。
 *
 *  【常见错误】
 *    1) 编译报 "缺少 firmware/07_main/secrets.h"
 *       → 你还没复制文件，或文件名多打了后缀（Windows 隐藏扩展名时容易变成
 *         secrets.h.txt）。在文件夹里打开"显示文件扩展名"再改成 secrets.h。
 *    2) 串口打印 "WiFi 连接失败，检查名称/密码"
 *       → 手机热点/路由器是 5G 频段的，ESP32 只认 2.4G；或密码里有空格。
 *    3) 百度返回 "invalid_client"
 *       → API Key 和 Secret Key 贴反了：百度是【先 API Key、后 Secret Key】。
 *    4) 百度返回 "invalid_grant" / 401
 *       → 复制时带了空格或换行；或应用没开通"短语音识别"服务。
 *    5) 大模型返回 401 / Insufficient Balance
 *       → Key 不对，或账户没充值（DeepSeek 需要先充几块钱）。
 * ==========================================================================*/

#pragma once

/* ========================================================================
 * 一、WiFi —— 必填（2.4G 频段！ESP32 不支持 5G WiFi）
 * ======================================================================*/
#define WIFI_SSID      "★填这里：你的 WiFi 名称（2.4G）"
#define WIFI_PASSWORD  "★填这里：你的 WiFi 密码"

/* 可选：连不上时换一个已知热点；如果你家路由器是 2.4G/5G 合一，
 * 建议在路由器后台把 2.4G 单独开一个 SSID，稳定性差别很大。 */
// #define WIFI_SSID_2     "备用热点名"
// #define WIFI_PASSWORD_2 "备用热点密码"


/* ========================================================================
 * 二、语音后端选择  VOICE_BACKEND
 * ------------------------------------------------------------------------
 *   1 = 百度智能云（推荐先跑通）：ASR(短语音识别) + TTS(语音合成) 用百度，
 *       LLM(大模型) 用 OpenAI 兼容接口（默认 DeepSeek）。
 *       特点：百度有免费额度，中文识别/合成效果稳；缺点要申请 2 个平台。
 *   2 = OpenAI 兼容一站式（SiliconFlow 硅基流动）：ASR + TTS + LLM 一个 Key
 *       全搞定，注册快；免费额度用完后按量付费。
 *       特点：只申请 1 家；缺点 TTS 要先下载成 mp3 再播放（多 1~2 秒）。
 *
 *   第一次建议用 1。跑通后再把这里改成 2 对比效果，代码不用动别的。
 * ======================================================================*/
#define VOICE_BACKEND 1


/* ========================================================================
 * 三、百度智能云 Key（VOICE_BACKEND = 1 时必填；= 2 时可留占位符）
 * ------------------------------------------------------------------------
 * 【去哪申请】https://console.bce.baidu.com/
 *   1) 注册/登录百度账号 → 完成实名认证（个人认证即可，免费）
 *   2) 顶部搜索 "语音技术" → 进入 "语音技术控制台"
 *   3) 左侧 "应用列表" → "创建应用"
 *      应用名称随便写（如 ai-robot）；接口选择勾上:
 *         ☑ 短语音识别（语音转文字）
 *         ☑ 语音合成（文字转语音）
 *   4) 创建好后回到应用列表，能看到两串字符:
 *         API Key      → 填到 BAIDU_API_KEY
 *         Secret Key   → 填到 BAIDU_SECRET_KEY
 *   5) 免费额度：在 "概览/资源包" 里领（短语音识别和语音合成都各有一份，
 *      每月/一次性送的量足够玩很久，注意别点成付费套餐）。
 * ======================================================================*/
#define BAIDU_API_KEY      "★填这里：百度应用的 API Key"
#define BAIDU_SECRET_KEY   "★填这里：百度应用的 Secret Key"

/* 百度要求带一个 cuid（客户端唯一标识），随便起个固定字符串即可，
 * 作用只是让百度区分设备；不是密钥，不用保密。 */
#define BAIDU_CUID         "ai-robot-esp32-s3"

/* 百度短语音识别 dev_pid 选择:
 *   1537 = 普通话（带标点，推荐）
 *   1536 = 普通话（纯文字，无标点）
 *   1737 = 英语（不常用）
 * 识别不准时优先换 1536 试试。 */
#define BAIDU_DEV_PID      1537

/* 百度语音合成音色 per:
 *   0 = 度小美（女声，默认，推荐）  1 = 度小宇（男声）
 *   3 = 度逍遥（男声）              4 = 度丫丫（童声）
 * 语速 spd / 音调 pit / 音量 vol 都是 0~15，5 为正常。 */
#define BAIDU_TTS_PER      0
#define BAIDU_TTS_SPD      5
#define BAIDU_TTS_PIT      5
#define BAIDU_TTS_VOL      5


/* ========================================================================
 * 四、大模型（OpenAI 兼容接口）—— 必填
 * ------------------------------------------------------------------------
 * 【默认 DeepSeek】去哪申请: https://platform.deepseek.com/api_keys
 *   1) 手机号注册 → 左侧 "API Keys" → "创建 API Key" → 复制 sk- 开头的一串
 *   2) 注意：DeepSeek 需要先在 "充值" 里充一点钱（几块钱能用很久），
 *      否则会报 402 Insufficient Balance。
 * 【想换别的】任何 OpenAI 兼容服务都行，改 LLM_BASE_URL / LLM_MODEL 即可:
 *   - 硅基流动:  https://api.siliconflow.cn/v1   model 如 Qwen/Qwen2.5-7B-Instruct
 *   - 阿里百炼:  https://dashscope.aliyuncs.com/compatible-mode/v1
 *   - 智谱:      https://open.bigmodel.cn/api/paas/v4
 *   代码会自动补全 "/v1/chat/completions"（base 已带 /v1 就不再重复加）。
 * ======================================================================*/
#define LLM_BASE_URL   "https://api.deepseek.com"
#define LLM_API_KEY    "★填这里：DeepSeek 的 sk-xxxxxxxx"
#define LLM_MODEL      "deepseek-chat"

/* 回复长度上限（tokens）。机器人嘴巴要短，120 以内足够；
 * 调大只是更容易说长句子，TTS 会变慢。 */
#define LLM_MAX_TOKENS 120

/* 可选：对话温度 0~2。0.7 比较活泼，0.3 更听话（更适合控制小车）。 */
#define LLM_TEMPERATURE 0.7


/* ========================================================================
 * 五、SiliconFlow 硅基流动（VOICE_BACKEND = 2 时必填；= 1 时可留占位符）
 * ------------------------------------------------------------------------
 * 【去哪申请】https://cloud.siliconflow.cn/account/ak
 *   1) 手机号注册（新用户送额度）
 *   2) 左侧 "API 密钥" → "新建 API 密钥" → 复制 sk- 开头的一串
 *   3) 这家 ASR / TTS / LLM 都用同一个 Key，所以 VOICE_BACKEND=2 时
 *      LLM_API_KEY 和 SF_API_KEY 填成同一个值即可（也可以只填 SF_API_KEY，
 *      代码会在 backend=2 时自动用它当 LLM Key）。
 *
 * 模型名以官网"模型广场 → 语音"为准，下面三个是 2026-03 时还在线的：
 *   ASR: FunAudioLLM/SenseVoiceSmall     （中文识别快且准）
 *   TTS: FunAudioLLM/CosyVoice2-0.5B     （音色在下面 SF_TTS_VOICE 里选）
 *   音色: FunAudioLLM/CosyVoice2-0.5B:alex  （官方要求音色前带模型名）
 *         其他可选: :anna :bella :claire :diana :benjamin :charles :david
 * ======================================================================*/
#define SF_BASE_URL     "https://api.siliconflow.cn/v1"
#define SF_API_KEY      "★填这里：硅基流动的 sk-xxxxxxxx"
#define SF_ASR_MODEL    "FunAudioLLM/SenseVoiceSmall"
#define SF_TTS_MODEL    "FunAudioLLM/CosyVoice2-0.5B"
#define SF_TTS_VOICE    "FunAudioLLM/CosyVoice2-0.5B:alex"
#define SF_TTS_FORMAT   "mp3"


/* ========================================================================
 * 六、调试开关（新手建议全开，能看清每一步在做什么）
 * ======================================================================*/
/* 1 = 串口打印云端每一步的请求/响应摘要（Key 会自动打码，不会泄露） */
#define VOICE_DEBUG_LOG 1

/* 1 = 每次说话前先用 TTS 播一句"我在听"（方便确认链路通；费一点点额度） */
#define VOICE_SAY_ACK   0


/* ========================================================================
 *  填完后的自检清单（照打勾）
 * ----------------------------------------------------------------------
 *  [ ] WIFI_SSID / WIFI_PASSWORD 是 2.4G WiFi
 *  [ ] VOICE_BACKEND = 1 时：BAIDU_API_KEY / BAIDU_SECRET_KEY / LLM_API_KEY 已填
 *  [ ] VOICE_BACKEND = 2 时：SF_API_KEY 已填（LLM_API_KEY 可同值）
 *  [ ] 本文件另存为 secrets.h，和 07_main.ino 在同一个文件夹
 *  [ ] 百度控制台里"短语音识别"和"语音合成"两个接口都已勾选开通
 *  [ ] DeepSeek 账户里至少有 1 元余额
 * ======================================================================*/
