/* ============================================================================
 *  AI-Robot · 04 摄像头图传（OV2640 + WiFi MJPEG）
 *  单文件、可独立烧录 —— 这一关只验证"摄像头能出图、手机能看"
 *  硬件：ESP32-S3-DevKitC-1 N16R8 (16MB Flash + 8MB OPI PSRAM)
 *  摄像头：DFRobot Gravity 版 OV2640（16 根独立排针的 DVP 并口，不是 FPC 软排线）
 * ----------------------------------------------------------------------------
 *  【目标】
 *    1. 用 OV2640 采集 JPEG 画面（VGA 640x480，质量 12）
 *    2. 连 2.4GHz WiFi，手机浏览器打开 http://<IP>/ 就能看实时画面
 *    3. 顺便提供：/stream（MJPEG 直播流）、/capture（拍单张）、/status（排错 JSON）
 *    4. 初始化失败时用中文把可能原因直接写在串口里，并自动重试 3 次
 *
 *  【接线】（全部免焊接杜邦线；摄像头 VCC 一定是 3V3，千万别接 5V）
 *    ================== 摄像头丝印 -> 接到哪里 ==================
 *    3V3 / VCC        ->  3V3（3.3V！接 5V 当场烧）
 *    GND              ->  GND
 *    SCL / SIOC       ->  pinmap.h 的 PIN_CAM_SIOC
 *    SDA / SIOD       ->  pinmap.h 的 PIN_CAM_SIOD
 *    VS / VSYNC       ->  PIN_CAM_VSYNC
 *    HREF             ->  PIN_CAM_HREF
 *    PCLK             ->  PIN_CAM_PCLK
 *    XCLK / MCLK      ->  PIN_CAM_XCLK（这根必须接，不是可选）
 *    D0 ~ D7          ->  PIN_CAM_D0 ~ PIN_CAM_D7（一根一根按顺序，别整排插反）
 *    RST              ->  直接插 3V3（插死，不占 GPIO）
 *    PWDN             ->  直接插 GND（插死，不占 GPIO）
 *    ============================================================
 *    · 具体 GPIO 编号只看 firmware/_shared/pinmap.h，本文件一个数字都没写死。
 *    · 开机时串口会把"本程序实际使用的引脚表"打印一遍，拿它和你手上的接线对照。
 *    · 杜邦线尽量短（15cm 以内），D0~D7 不要绕成大圈，否则花屏/黑屏/随机失败。
 *    · 所有模块的 GND 一定要接到同一个电网，少一根 GND 会表现为"代码有 bug"。
 *
 *  【预期现象】
 *    1. 串口（115200）先打印引脚表、PSRAM 状态，然后开始连 WiFi
 *    2. 连不上会每 5 秒重试，并打印一句中文排错提示
 *    3. 连上后打印两行网址：
 *         http://<IP>/          ← 手机打开这个看画面
 *         http://<IP>/stream    ← 纯 MJPEG 流（可以贴到别的播放器里）
 *    4. 手机连同一个 2.4G WiFi，打开首页 → 看到实时画面，画面下方有 IP 和一行提示
 *    5. 打开 /status 能看到 fps（流畅时通常 10~20，跟 WiFi 信号和画面复杂程度有关）
 *    6. 打开首页后画面要等 1~3 秒才动（先显示黑框）
 *       → 正常现象：网页服务和摄像头一次只处理一个连接，等一下就好，不用反复刷新
 *       → 个别手机浏览器（如 iPhone 自带 Safari）不支持 MJPEG，页面 5 秒后会自动改成每秒一张
 *
 *  【常见错误】
 *    1. 串口报 0x105 / 0x106 / 0x20001（都是"认不到摄像头"）
 *       → D0~D7 有一根没插到底/插错位置；RST 没接 3V3；PWDN 没接 GND；SIOD/SIOC 接反
 *    2. 串口报 0x101（内存不足）或 0xFFFFFFFF（驱动内部失败）、或一开摄像头就重启
 *       → 最常见还是 Arduino IDE 里 PSRAM 没选成 "OPI PSRAM"（N16R8 必须 OPI，不是 QSPI）
 *    3. WiFi 一直连不上
 *       → ESP32 只支持 2.4GHz！带 "5G" 字样的那一组是 5GHz，永远连不上；
 *         名称/密码不要带中文、空格；手机热点要打开"最大兼容性/2.4GHz 频段"
 *    4. 手机打不开网页
 *       → 手机和机器人不在同一个 WiFi；或手机开着移动数据但有代理；先关掉 4G/5G 试
 *    5. 画面卡住 / 网页一直转圈
 *       → 同一时间只能有一个页面看直播；把旧页面全关掉，等 5 秒再刷新
 *    6. 编译时报 "camera_config_t has no member named pin_sccb_sda"
 *       → esp32-camera 库版本太旧，在库管理器里升级到最新版
 *
 *  【烧录设置】（Arduino IDE 2.x → 工具菜单）
 *    开发板:            ESP32S3 Dev Module
 *    Flash Size:        16MB
 *    PSRAM:             OPI PSRAM        ← 最关键，选错一定失败
 *    Partition Scheme:  16M Flash (3MB APP/9.9MB FATFS)（没有就选 Huge APP）
 *    USB CDC On Boot:   Disabled（保持默认）
 *    端口:              选带 COM 的那个（UART 口 / CP2102）
 *    烧录用 UART 口那颗 Type-C；USB 口平时别插（那两个脚被超声波占了，见 docs/01 第 5 节）
 * ==========================================================================*/

#include "pinmap.h"          /* 引脚唯一权威来源，所有 PIN_CAM_* 都来自这里 */

#include <stdio.h>
#include <string.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include "esp_camera.h"

/* ===========================================================================
 *  ===== 改这里 =====   WiFi 账号密码（整个项目只改这两行）
 * ===========================================================================
 *  1) 把下面引号里的文字换成你自己 WiFi 的名称和密码，别动引号和分号。
 *  2) 【必须是 2.4GHz 热点！！！】ESP32-S3 只支持 2.4GHz，不支持 5GHz。
 *     路由器/手机热点里通常有 "xxx" 和 "xxx_5G" 两组 —— 选没有 5G 的那组。
 *  3) 名称和密码不要带中文、不要带空格（中文 SSID 很多设备握手就失败）。
 *  4) 手机热点：打开"最大兼容性"或"2.4GHz 频段"，否则可能只发射 5GHz。
 *  5) 如果这里还是 YOUR_WIFI_2G_NAME，程序会连不上并在串口里提醒你。
 * =========================================================================*/
#define WIFI_SSID   "YOUR_WIFI_2G_NAME"     /* ← 2.4G 热点名称，不要带中文/空格 */
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"    /* ← WiFi 密码 */

/* ===========================================================================
 *  ===== 可选调整 =====（不认识就别动，保持默认就能用）
 * =========================================================================*/
#define CAM_VFLIP       0    /* 画面上下颠倒就改成 1 */
#define CAM_HMIRROR     0    /* 画面左右镜像就改成 1 */
#define WIFI_SCAN_AT_BOOT 1  /* 1=开机扫描附近热点（方便确认是不是 5GHz）；0=不扫描 */
/* 想换分辨率/画质：改下面 initCamera() 里的 frame_size = FRAMESIZE_VGA 那一行，
 * 以及 jpeg_quality = 12（数字越小画质越好、越卡）。VGA 是手机看图最合适的档位。 */

/* ---------------- 程序内部用到的常量 ---------------- */
#define SERIAL_BAUD             115200  /* 串口监视器波特率，要和 IDE 里选的一致 */
#define CAM_INIT_ATTEMPTS       3       /* 摄像头初始化失败自动重试次数 */
#define WIFI_RETRY_INTERVAL_MS  5000    /* WiFi 连不上时每 5 秒重试一次 */
#define STREAM_CHUNK_BYTES      4096    /* 推流时每块写 4KB，绝不一次性开大 buffer */
#define STREAM_WRITE_TIMEOUT_MS 3000    /* 单块数据 3 秒还没写出去，就认为对端死了 */
#define STREAM_MAX_EMPTY_FRAMES 5       /* 连续 5 次取不到画面就停止推流（每次最多等 4 秒） */

/* ---------------- 全局对象 / 状态 ---------------- */
WebServer server(80);            /* HTTP 服务，端口 80（浏览器默认端口） */

static bool      g_streamActive = false;   /* 当前有没有人在看 /stream */
static float     g_fps          = 0.0f;    /* 最近 1 秒统计出来的帧率 */
static uint32_t  g_totalFrames  = 0;       /* 本次上电累计推出去的帧数 */
static framesize_t g_framesize  = FRAMESIZE_VGA;  /* 实际使用的分辨率 */

/* ===========================================================================
 *  一、小工具函数
 * =========================================================================*/

/* 把分辨率枚举翻译成给用户看的文字 */
static const char *framesizeName(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA: return "QQVGA 160x120";
    case FRAMESIZE_QVGA:  return "QVGA 320x240";
    case FRAMESIZE_CIF:   return "CIF 400x296";
    case FRAMESIZE_VGA:   return "VGA 640x480";
    case FRAMESIZE_SVGA:  return "SVGA 800x600";
    case FRAMESIZE_XGA:   return "XGA 1024x768";
    case FRAMESIZE_HD:    return "HD 1280x720";
    case FRAMESIZE_SXGA:  return "SXGA 1280x1024";
    case FRAMESIZE_UXGA:  return "UXGA 1600x1200";
    default:              return "自定义分辨率";
  }
}

/* 把 WiFi 状态码翻译成中文，方便零基础直接看串口 */
static const char *wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "空闲";
    case WL_NO_SSID_AVAIL:   return "找不到这个 WiFi（名字写错 / 热点没开 / 是 5GHz）";
    case WL_SCAN_COMPLETED:  return "扫描完成";
    case WL_CONNECTED:       return "已连接";
    case WL_CONNECT_FAILED:  return "连接失败（密码错 / 加密方式不支持）";
    case WL_CONNECTION_LOST: return "连接断开";
    case WL_DISCONNECTED:    return "未连接";
    default:                 return "未知状态";
  }
}

/* ===========================================================================
 *  二、开机打印：版本、引脚表、PSRAM 状态
 * =========================================================================*/

static void printBanner() {
  Serial.println();
  Serial.println("======================================================");
  Serial.println(" AI-Robot · 04 摄像头图传 (OV2640 + WiFi MJPEG)");
  Serial.println(" 单文件固件 v1.0 · 目标板 ESP32-S3-N16R8");
  Serial.println("======================================================");
  Serial.printf(" 芯片: %s rev%d | Flash: %u MB | 编译: %s %s\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                (unsigned)(ESP.getFlashChipSize() / (1024UL * 1024UL)),
                __DATE__, __TIME__);
}

/* 把本程序实际使用的引脚全部打印出来：用户拿它和手上的接线一一对照即可 */
static void printPinTable() {
  Serial.println("---------- 本程序实际使用的引脚（对照 docs/01 第 3.1 节）----------");
  Serial.printf("  摄像头 D0    -> GPIO %d\n", (int)PIN_CAM_D0);
  Serial.printf("  摄像头 D1    -> GPIO %d\n", (int)PIN_CAM_D1);
  Serial.printf("  摄像头 D2    -> GPIO %d\n", (int)PIN_CAM_D2);
  Serial.printf("  摄像头 D3    -> GPIO %d\n", (int)PIN_CAM_D3);
  Serial.printf("  摄像头 D4    -> GPIO %d\n", (int)PIN_CAM_D4);
  Serial.printf("  摄像头 D5    -> GPIO %d\n", (int)PIN_CAM_D5);
  Serial.printf("  摄像头 D6    -> GPIO %d\n", (int)PIN_CAM_D6);
  Serial.printf("  摄像头 D7    -> GPIO %d\n", (int)PIN_CAM_D7);
  Serial.printf("  摄像头 XCLK  -> GPIO %d  (MCLK，必须接)\n", (int)PIN_CAM_XCLK);
  Serial.printf("  摄像头 PCLK  -> GPIO %d\n", (int)PIN_CAM_PCLK);
  Serial.printf("  摄像头 VSYNC -> GPIO %d  (VS)\n", (int)PIN_CAM_VSYNC);
  Serial.printf("  摄像头 HREF  -> GPIO %d\n", (int)PIN_CAM_HREF);
  Serial.printf("  摄像头 SIOD  -> GPIO %d  (SDA)\n", (int)PIN_CAM_SIOD);
  Serial.printf("  摄像头 SIOC  -> GPIO %d  (SCL)\n", (int)PIN_CAM_SIOC);
  Serial.println("  摄像头 RST   -> 3V3  直接插死，不占 GPIO（代码里填 -1）");
  Serial.println("  摄像头 PWDN  -> GND  直接插死，不占 GPIO（代码里填 -1）");
  Serial.println("  摄像头 VCC   -> 3V3  绝对不要接 5V");
  Serial.println("  摄像头 GND   -> GND");
  Serial.println("  注: 上面数字如果和你接的不一样，以 pinmap.h 为准，别改代码。");
  Serial.println("------------------------------------------------------------------");
}

/* 检查 PSRAM：摄像头 JPEG 双缓冲必须放 PSRAM，没开 PSRAM 基本一定失败 */
static void printPsramInfo() {
  size_t totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t freePsram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  if (totalPsram == 0) {
    Serial.println();
    Serial.println("【严重警告】没有检测到 PSRAM！摄像头双缓冲要用 PSRAM，下面初始化大概率失败。");
    Serial.println("  → IDE 菜单：工具 → PSRAM → 选 \"OPI PSRAM\"（N16R8 是 OPI，不是 QSPI）");
    Serial.println("  → 同时确认：工具 → Flash Size → \"16MB\"");
    Serial.println("  → 改完这些选项必须重新编译上传，只改选项不重烧没有用");
    Serial.println();
  } else {
    Serial.printf("[PSRAM] 已就绪：共 %u KB，空闲 %u KB\n",
                  (unsigned)(totalPsram / 1024UL), (unsigned)(freePsram / 1024UL));
  }
}

/* ===========================================================================
 *  三、摄像头：初始化（带中文报错 + 自动重试 3 次）
 * =========================================================================*/

/* 按错误码给出中文排查建议 —— 这部分就是给零基础用户的"自救说明书" */
static void printCameraErrorHint(esp_err_t err) {
  const char *name = esp_err_to_name(err);
  if (name == nullptr) {
    name = "UNKNOWN";
  }
  Serial.printf("[摄像头] 初始化失败，错误码 0x%X (%s)\n", (unsigned)err, name);

  if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED ||
      err == ESP_ERR_CAMERA_NOT_DETECTED) {
    /* 0x105 / 0x106 / 0x20001：SCCB 上探测不到摄像头，最常见 */
    Serial.println("  → 认不到摄像头。按顺序检查（90% 是前三条）：");
    Serial.println("     1) D0~D7 八根线有没有一根没插到底、或者插错位置/整排插反");
    Serial.println("     2) RST 必须直接接 3V3，PWDN 必须直接接 GND —— 悬空一定失败");
    Serial.println("     3) SIOD 和 SIOC 有没有接反（SDA=SIOD，SCL=SIOC）");
    Serial.println("     4) 摄像头 3V3 供电接了没有？是不是错接成 5V 了？");
    Serial.println("     5) IDE 里 PSRAM 是否选成 \"OPI PSRAM\"（选错也会连带出各种怪错）");
    Serial.println("     6) 杜邦线太长/太松，拔下来重新插紧，线尽量短于 15cm");
  } else if (err == ESP_ERR_NO_MEM) {
    /* 0x101 */
    Serial.println("  → 内存不够（PSRAM 没开或太小）。检查：");
    Serial.println("     1) 工具 → PSRAM → \"OPI PSRAM\"（N16R8 必须选 OPI）");
    Serial.println("     2) 工具 → Flash Size → \"16MB\"，Board 选 \"ESP32S3 Dev Module\"");
    Serial.println("     3) PSRAM 是硬件焊接的，如果板子不是 N16R8，就得换板子");
  } else if (err == ESP_ERR_INVALID_ARG) {
    /* 0x102 */
    Serial.println("  → 参数不合法，多半是 pinmap.h 被改过/复制错了。");
    Serial.println("     重新跑一次 tools\\sync_pinmap.ps1，再重新编译上传。");
  } else if (err == ESP_ERR_INVALID_STATE) {
    /* 0x103 */
    Serial.println("  → 摄像头驱动状态异常（上一次初始化没清理干净）。");
    Serial.println("     按板子上的 RST 键重启一次；反复出现就把 esp32-camera 库升到最新版。");
  } else if ((int)err == 0x20002) {
    /* ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE */
    Serial.println("  → 帧缓冲申请失败（PSRAM 不足或分辨率太高）。");
    Serial.println("     先把 initCamera() 里的 frame_size 改成 FRAMESIZE_QVGA 再试；");
    Serial.println("     同时确认 PSRAM = OPI PSRAM、Flash Size = 16MB。");
  } else if (err == ESP_FAIL) {
    /* 0xFFFFFFFF：驱动内部失败（实际最常见的一类） */
    Serial.println("  → 驱动内部初始化失败，按顺序查：");
    Serial.println("     1) PSRAM 是否选成 \"OPI PSRAM\"、Flash Size 是否 16MB");
    Serial.println("     2) D0~D7 / XCLK / PCLK / VSYNC / HREF 有没有插错或没插到底");
    Serial.println("     3) RST 是否接 3V3、PWDN 是否接 GND");
    Serial.println("     4) 摄像头 3V3 供电是否接好（电压不稳会导致初始化失败/重启）");
  } else {
    Serial.println("  → 未知错误。先按这三项检查：");
    Serial.println("     1) D0~D7 / RST(3V3) / PWDN(GND) 的接线");
    Serial.println("     2) IDE 里 PSRAM = OPI PSRAM、Flash Size = 16MB");
    Serial.println("     3) 把上面这行错误码完整复制去查 docs/04 排错手册");
  }
  Serial.println();
}

/* 初始化摄像头，失败自动重试。全部失败返回 false */
static bool initCamera() {
  for (int attempt = 1; attempt <= CAM_INIT_ATTEMPTS; attempt++) {
    Serial.printf("[摄像头] 第 %d/%d 次初始化 ...\n", attempt, CAM_INIT_ATTEMPTS);

    /* 先全部清零，再逐项设置。所有引脚都来自 pinmap.h，绝不在代码里写死数字 */
    camera_config_t config = {};

    /* --- 引脚（DVP 并口）--- */
    config.pin_d0 = PIN_CAM_D0;
    config.pin_d1 = PIN_CAM_D1;
    config.pin_d2 = PIN_CAM_D2;
    config.pin_d3 = PIN_CAM_D3;
    config.pin_d4 = PIN_CAM_D4;
    config.pin_d5 = PIN_CAM_D5;
    config.pin_d6 = PIN_CAM_D6;
    config.pin_d7 = PIN_CAM_D7;
    config.pin_xclk  = PIN_CAM_XCLK;
    config.pin_pclk  = PIN_CAM_PCLK;
    config.pin_vsync = PIN_CAM_VSYNC;
    config.pin_href  = PIN_CAM_HREF;
    config.pin_sccb_sda = PIN_CAM_SIOD;   /* 新版 esp32-camera 的字段名 */
    config.pin_sccb_scl = PIN_CAM_SIOC;
    config.pin_pwdn  = -1;   /* PWDN 直接接 GND，不占 GPIO */
    config.pin_reset = -1;   /* RST  直接接 3V3，不占 GPIO */

    /* --- 时钟 / 控制器 --- */
    config.xclk_freq_hz = 20000000;        /* 20MHz 主时钟（S3 + OV2640 的常用值） */

    /* ★★★ 特殊模块适配：如果你的摄像头模块【没有 XCLK 引脚】★★★
     * （模块自带的 24MHz 有源晶振给自己供时钟，见 docs/01 第 3.1 节）
     * 就把下面 CAM_ONBOARD_CLOCK 改成 1：
     *    - 不再由 ESP32 输出时钟（pin_xclk = -1）
     *    - 频率按模块晶振写 24MHz（驱动要靠这个值算 OV2640 内部的 PLL）
     * 如果改完报错 / 一直黑屏，说明这版驱动不支持"外部时钟模式"，
     * 那就只能换一块【带 XCLK 引脚】的模块。                              */
    #ifndef CAM_ONBOARD_CLOCK
      #define CAM_ONBOARD_CLOCK 0
    #endif
    #if CAM_ONBOARD_CLOCK
      config.pin_xclk     = -1;
      config.xclk_freq_hz = 24000000;
    #endif

    config.ledc_channel = LEDC_CHANNEL_0;  /* 和官方 S3 摄像头例程一致 */
    config.ledc_timer   = LEDC_TIMER_0;
    config.sccb_i2c_port = 0;              /* 配置里给了 SDA/SCL 引脚时库会自己初始化 SCCB，
                                            * 这个字段只在 pin_sccb_sda=-1 时才生效；填 0/-1 都行 */

    /* --- 图像参数 --- */
    config.pixel_format = PIXFORMAT_JPEG;  /* 直接出 JPEG，最适合图传 */
    config.frame_size   = FRAMESIZE_VGA;   /* 640x480，手机看图最合适（可改） */
    config.jpeg_quality = 12;              /* 0~63，数字越小越清晰也越卡 */
    config.fb_count     = 2;               /* 双缓冲：一边采集一边发送 */
    config.fb_location  = CAMERA_FB_IN_PSRAM;   /* 帧缓冲必须放 PSRAM */
    config.grab_mode    = CAMERA_GRAB_LATEST;   /* 跟不上时丢旧帧，永远发最新画面 */

    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
      g_framesize = config.frame_size;
      Serial.printf("[摄像头] 初始化成功！分辨率 %s，JPEG 质量 %d，双缓冲，PSRAM\n",
                    framesizeName(g_framesize), 12);
      return true;
    }

    printCameraErrorHint(err);

    /* 清场，避免残留的 I2C / LEDC 资源影响下一次尝试（失败通常只返回错误，不崩） */
    esp_camera_deinit();
    delay(500);
  }
  return false;
}

/* 可选的画面方向修正：默认不动，画面反了就改文件顶部 CAM_VFLIP / CAM_HMIRROR */
static void applySensorTweaks() {
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    Serial.println("[摄像头] 警告：读不到传感器参数，跳过方向设置（不影响出图）");
    return;
  }
  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
  Serial.printf("[摄像头] 方向设置：上下翻转=%d 左右镜像=%d（画面反了就改文件顶部）\n",
                CAM_VFLIP, CAM_HMIRROR);
}

/* 连续 3 次初始化都失败：停下来反复提示，等用户修好接线后按 RST 重启 */
static void cameraFatalHang() {
  Serial.println();
  Serial.println("======================================================");
  Serial.println("【摄像头】连续 3 次初始化失败，程序已暂停（不是死机）。");
  Serial.println(" 请按上面的中文提示检查接线，修好后按板子上的 RST 键重启。");
  Serial.println(" 三个最常见原因：");
  Serial.println("   1) D0~D7 有一根没插好 / 插错位置");
  Serial.println("   2) RST 没接 3V3，或 PWDN 没接 GND");
  Serial.println("   3) IDE 里 PSRAM 没选成 OPI PSRAM");
  Serial.println("======================================================");
  while (true) {
    delay(5000);
    Serial.println("[摄像头] 仍在等待修复…… 修好后按 RST 键重启");
  }
}

/* ===========================================================================
 *  四、WiFi：连 2.4GHz 热点，失败每 5 秒重试 + 中文排错提示
 * =========================================================================*/

/* 每失败一次换一条提示，把用户最可能踩的坑轮流说一遍 */
static void printWiFiHints(int attempt) {
  static const char *hints[] = {
    "ESP32 只支持 2.4GHz！热点名带 5G 的那一组是 5GHz，永远连不上，换另一组。",
    "WiFi 名称/密码不要带中文、不要带空格，区分大小写，检查有没有多打一个空格。",
    "手机热点要打开\"最大兼容性\"或\"2.4GHz 频段\"，否则热点可能只发 5GHz。",
    "热点没开 / 名字改了？先看上面扫描出来的列表里有没有你家热点。",
    "板子离路由器太远，或天线被电机线挡住。图传很吃信号，先拿近一点试。",
    "如果改过路由器密码，记得把文件顶部 WIFI_PASS 也改掉再重新烧录。",
  };
  const int hintCount = (int)(sizeof(hints) / sizeof(hints[0]));
  int idx = (attempt - 1) % hintCount;
  Serial.printf("[WiFi] 排错提示：%s\n", hints[idx]);
}

/* 开机扫描附近热点：看不到你家 WiFi，就说明它大概率是 5GHz 或太远 */
static void scanWiFiOnce() {
  if (!WIFI_SCAN_AT_BOOT) {
    return;
  }
  Serial.println("[WiFi] 扫描附近 2.4GHz 热点（约 2 秒）...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[WiFi] 一个热点都没扫到：可能离路由器太远，或天线被金属/电机线挡住。");
  } else {
    Serial.printf("[WiFi] 扫到 %d 个热点（下面有你家 WiFi 吗？没有的话多半是 5GHz）：\n", n);
    int show = (n > 15) ? 15 : n;
    for (int i = 0; i < show; i++) {
      Serial.printf("   %2d) %-22s 频道%2d  %4d dBm  %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    (int)WiFi.channel(i),
                    (int)WiFi.RSSI(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "开放" : "加密");
    }
    if (n > show) {
      Serial.printf("   …… 还有 %d 个没显示\n", n - show);
    }
  }
  WiFi.scanDelete();
  Serial.println();
}

/* 连 WiFi：无限重试，每 5 秒一次。连上后打印 IP 等信息 */
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          /* 关掉省电模式，否则图传会一顿一顿 */
  WiFi.setAutoReconnect(true);   /* 运行中掉线自动重连 */
  WiFi.disconnect();             /* 清掉上一次残留的连接 */
  delay(200);

  if (strcmp(WIFI_SSID, "YOUR_WIFI_2G_NAME") == 0) {
    Serial.println();
    Serial.println("【提醒】你还没改 WiFi 名称/密码！请改文件顶部 ===== 改这里 ===== 那两行。");
    Serial.println("        改完重新编译上传（不是按 Ctrl+U，要重新点「上传」按钮）。");
  }

  scanWiFiOnce();

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    attempt++;
    Serial.printf("\n[WiFi] 第 %d 次尝试连接 \"%s\" ...（只支持 2.4GHz！）\n",
                  attempt, WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_RETRY_INTERVAL_MS) {
      wl_status_t st = WiFi.status();
      if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) {
        break;   /* 明确失败（找不到热点/密码错），不用傻等满 5 秒 */
      }
      delay(500);
      Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      break;
    }

    Serial.printf("[WiFi] 连接失败：%s（状态码 %d），5 秒后重试\n",
                  wifiStatusText(WiFi.status()), (int)WiFi.status());
    printWiFiHints(attempt);
    WiFi.disconnect();
    delay(300);
  }

  String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("======================================================");
  Serial.printf(" WiFi 已连接！IP = %s\n", ip.c_str());
  Serial.printf("   热点 = %s | 信号 %d dBm | 频道 %d\n",
                WiFi.SSID().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
  Serial.println("======================================================");
}

/* ===========================================================================
 *  五、HTTP 服务：/  /stream  /capture  /status
 * =========================================================================*/

/* 首页：极简中文页面，手机上直接能看（只有标题、IP、画面、一行提示） */
static const char ROOT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>AI-Robot 摄像头</title>
<style>
  html,body{margin:0;padding:0;background:#0d1117;color:#e6edf3;text-align:center;
    font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
  h1{font-size:17px;font-weight:600;margin:14px 0 6px}
  .ip{font-family:Consolas,monospace;font-size:14px;color:#7ee787;margin:0 0 10px}
  img{display:block;width:94vw;max-width:820px;margin:0 auto;border-radius:10px;
    background:#000;min-height:120px}
  .hint{font-size:13px;line-height:1.7;color:#8b949e;margin:12px 16px 22px}
</style>
</head>
<body>
<h1>AI-Robot · 摄像头实时画面</h1>
<p class="ip">http://{{IP}}/</p>
<img id="cam" src="/stream" alt="摄像头画面">
<p class="hint">手机和机器人要连同一个 2.4G WiFi；画面卡住就关掉本页，等 5 秒再刷新。</p>
<script>
/* 兜底：个别手机浏览器（尤其 iPhone 自带 Safari）不支持 MJPEG 直播流。
   5 秒还看不到第一帧，就自动改成"每秒抓一张图"，画面会慢但能动。 */
(function () {
  var img = document.getElementById('cam');
  var t0 = Date.now();
  var timer = setInterval(function () {
    if (img.naturalWidth > 0) { clearInterval(timer); return; }
    if (Date.now() - t0 > 5000) {
      clearInterval(timer);
      img.removeAttribute('src');            /* 先断开直播流，把服务器让出来 */
      var snap = function () { img.src = '/capture?t=' + Date.now(); };
      setTimeout(function () {
        snap();
        setInterval(snap, 700);
      }, 800);
    }
  }, 500);
})();
</script>
</body>
</html>
)rawliteral";

/* 分块把数据写到 TCP 连接里：小段小段写，不一次性 malloc 大图，也不死等
 * 返回 false 表示对端已经断开（浏览器关页面 / 切后台 / WiFi 掉线），调用方应立即收工
 * 用 NetworkClient& 是为了兼容 ESP32 core 3.x 的 WebServer::client() 返回类型 */
static bool writeAll(NetworkClient &client, const uint8_t *data, size_t len) {
  size_t sent = 0;
  uint32_t lastProgress = millis();

  while (sent < len) {
    if (!client.connected()) {
      return false;                              /* 对端已关闭连接 */
    }
    size_t chunk = len - sent;
    if (chunk > STREAM_CHUNK_BYTES) {
      chunk = STREAM_CHUNK_BYTES;                /* 每块最多 4KB */
    }
    size_t n = client.write(data + sent, chunk);
    if (n == 0) {
      /* 发送缓冲满了或对端收得很慢：等 2ms 再试；超过 3 秒没进展就放弃 */
      if (millis() - lastProgress > STREAM_WRITE_TIMEOUT_MS) {
        return false;
      }
      delay(2);
      continue;
    }
    sent += n;
    lastProgress = millis();
    delay(0);                                    /* 让出 CPU，防止看门狗复位 */
  }
  return true;
}

/* / —— 首页 */
static void handleRoot() {
  String page = FPSTR(ROOT_PAGE);
  page.replace("{{IP}}", WiFi.localIP().toString());
  server.send(200, "text/html; charset=utf-8", page);
  Serial.printf("[HTTP] 有人打开了首页（来自 %s）\n",
                server.client().remoteIP().toString().c_str());
}

/* /capture —— 拍单张 JPEG（直接写裸 HTTP 响应，避免 String 截断二进制数据） */
static void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    server.send(503, "text/plain; charset=utf-8",
                "取帧失败：摄像头没出图。请按 RST 重启，或检查 D0~D7 / RST / PWDN 接线。\n");
    Serial.println("[HTTP] /capture 取帧失败");
    return;
  }

  NetworkClient &client = server.client();
  client.setNoDelay(true);

  char head[160];
  int hlen = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Length: %u\r\n"
                      "Cache-Control: no-store\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      (unsigned)fb->len);

  bool ok = (hlen > 0) && writeAll(client, (const uint8_t *)head, (size_t)hlen);
  if (ok) {
    ok = writeAll(client, fb->buf, fb->len);
  }
  Serial.printf("[HTTP] /capture 单张 %u 字节%s\n",
                (unsigned)fb->len, ok ? " 已发送" : " 发送中断");

  esp_camera_fb_return(fb);
  client.stop();
}

/* /status —— JSON：fps、分辨率、PSRAM 空闲、WiFi RSSI，专门用来排错 */
static void handleStatus() {
  static char json[512];
  String ip = WiFi.localIP().toString();

  snprintf(json, sizeof(json),
           "{\"fps\":%.1f,\"framesize\":\"%s\","
           "\"psram_free_kb\":%u,\"psram_total_kb\":%u,"
           "\"rssi_dbm\":%d,\"ip\":\"%s\","
           "\"stream_active\":%s,\"frames_sent\":%lu,\"uptime_s\":%lu,"
           "\"note\":\"fps 只在有人打开 /stream 时统计\"}",
           g_fps,
           framesizeName(g_framesize),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024UL),
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024UL),
           (int)WiFi.RSSI(),
           ip.c_str(),
           g_streamActive ? "true" : "false",
           (unsigned long)g_totalFrames,
           (unsigned long)(millis() / 1000UL));

  server.send(200, "application/json; charset=utf-8", json);
}

/* /stream —— MJPEG 直播流。响应头手动写，之后每帧按边界格式裸发 */
static void handleStream() {
  NetworkClient &client = server.client();
  client.setNoDelay(true);   /* 关掉 Nagle，画面延迟更低 */

  /* ---- 1) MJPEG 响应头（长连接，绝不能带 Content-Length）---- */
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n");
  client.print("Pragma: no-cache\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  Serial.println("[流] 有客户端开始看直播");
  g_streamActive = true;
  g_fps = 0.0f;

  uint32_t winStart  = millis();   /* 帧率统计窗口起点 */
  uint32_t winFrames = 0;
  uint32_t emptyFrames = 0;
  bool clientAlive = true;
  char head[128];

  /* ---- 2) 一帧一帧往外发 ---- */
  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      /* 偶发取帧失败（曝光/供电抖动）等 100ms 再试，连续失败太多就收工 */
      emptyFrames++;
      if (emptyFrames >= STREAM_MAX_EMPTY_FRAMES) {
        Serial.println("[流] 连续取不到画面，停止推流（检查摄像头排线/供电）");
        break;
      }
      delay(100);
      continue;
    }
    emptyFrames = 0;

    /* 每帧格式：--frame + 类型 + 长度 + 空行 + JPEG 数据 + 空行 */
    int hlen = snprintf(head, sizeof(head),
                        "--frame\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %u\r\n"
                        "\r\n",
                        (unsigned)fb->len);

    bool ok = (hlen > 0) && writeAll(client, (const uint8_t *)head, (size_t)hlen);
    if (ok) {
      ok = writeAll(client, fb->buf, fb->len);   /* 数据分块写，不额外 malloc */
    }
    if (ok) {
      ok = writeAll(client, (const uint8_t *)"\r\n", 2);
    }
    esp_camera_fb_return(fb);                   /* 无论成功失败都要归还帧缓冲，否则内存泄漏 */

    if (!ok) {
      clientAlive = false;
      break;
    }

    /* ---- 3) 统计 fps（每秒结算一次，给 /status 用）---- */
    g_totalFrames++;
    winFrames++;
    uint32_t now = millis();
    if (now - winStart >= 1000) {
      g_fps = winFrames * 1000.0f / (float)(now - winStart);
      winFrames = 0;
      winStart = now;
    }

    delay(1);   /* 每帧让出 CPU：喂看门狗，也给 WiFi 协议栈时间 */
  }

  /* ---- 4) 收尾：一定要 stop，否则残留连接会占内存 ---- */
  g_streamActive = false;
  g_fps = 0.0f;
  if (!clientAlive) {
    Serial.println("[流] 客户端断开（浏览器关页面 / 切后台 / 网络差）");
  }
  client.stop();
  Serial.println("[流] 停止推流，可以重新打开页面");
}

/* 不存在的路径 */
static void handleNotFound() {
  String msg = "找不到这个页面。\r\n可用的有： /  /stream  /capture  /status\r\n";
  server.send(404, "text/plain; charset=utf-8", msg);
}

/* 注册路由并启动 80 端口服务 */
static void startHttpServer() {
  server.on("/",        HTTP_GET, handleRoot);
  server.on("/stream",  HTTP_GET, handleStream);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status",  HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] 网页服务已启动（端口 80）");
}

/* 打印网址：方便用户直接抄到手机浏览器 */
static void printAccessInfo() {
  String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("======================================================");
  Serial.println(" 服务已启动！用手机浏览器打开下面网址：");
  Serial.println();
  Serial.printf("   实时画面: http://%s/\n", ip.c_str());
  Serial.printf("   直播流  : http://%s/stream\n", ip.c_str());
  Serial.println();
  Serial.printf("   拍单张  : http://%s/capture\n", ip.c_str());
  Serial.printf("   状态JSON: http://%s/status\n", ip.c_str());
  Serial.println();
  Serial.printf("   信号 %d dBm | 频道 %d | 分辨率 %s\n",
                (int)WiFi.RSSI(), (int)WiFi.channel(), framesizeName(g_framesize));
  Serial.println(" 提示：手机必须连同一个 2.4G WiFi；5G 热点连不上。");
  Serial.println("======================================================");
}

/* ===========================================================================
 *  六、Arduino 入口
 * =========================================================================*/

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);   /* 给串口一点时间，免得开头几行丢失 */

  printBanner();
  printPinTable();
  printPsramInfo();

  /* 1) 摄像头：失败会重试 3 次，全失败就停下来反复提示 */
  if (!initCamera()) {
    cameraFatalHang();   /* 永不返回 */
  }
  applySensorTweaks();

  /* 2) WiFi：连不上就每 5 秒重试，永不返回（用户改好密码后要重新烧录） */
  connectWiFi();

  /* 3) HTTP 服务 */
  startHttpServer();
  printAccessInfo();

  Serial.println("[系统] 准备就绪，等待手机浏览器访问……");
  Serial.println();
}

void loop() {
  /* WebServer 处理网页请求；/stream 会在这里面一直循环，直到对方断开 */
  server.handleClient();

  /* WiFi 掉线自愈：看直播时进不到这里，流断了才会回来 */
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry >= WIFI_RETRY_INTERVAL_MS) {
      lastRetry = millis();
      Serial.printf("[WiFi] 掉线了（%s），正在重连……\n", wifiStatusText(WiFi.status()));
      WiFi.reconnect();
    }
  }

  delay(2);   /* 让出 CPU，降低发热 */
}
