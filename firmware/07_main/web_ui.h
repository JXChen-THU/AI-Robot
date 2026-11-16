/* ============================================================================
 *  AI-Robot · 07_main · 手机网页遥控页面（内嵌 HTML/CSS/JS）
 *  文件: web_ui.h            需要和 07_main.ino 放在同一个文件夹
 *  ---------------------------------------------------------------------------
 *  【目标】
 *    手机连同一个 WiFi，浏览器打开  http://<小车IP>/  就能：
 *      · 看摄像头实时画面（MJPEG 流）
 *      · 按住按钮遥控小车（前进/后退/左转/右转/停）
 *      · 控制云台（上/下/左/右/回中）
 *      · 开关自动避障
 *      · 点“说话”让小车录音→云端对话→播报
 *      · 看状态（距离、fps、WiFi 信号、IP、当前表情/状态、上次对话）
 *    显示这一步完全在浏览器里做，不占 ESP32 内存；ESP32 只发一段 HTML。
 *
 *  【接线】
 *    不需要额外接线！页面用到的数据全部来自已有的 7 个模块。
 *    （网页本身走 WiFi；摄像头、电机、云台、超声波都在 07_main.ino 里）
 *
 *  【预期现象】
 *    1) 烧录后串口打印  [WEB] 手机浏览器打开: http://192.168.1.23/
 *    2) 手机连同一个 2.4G WiFi，浏览器输入这个地址：
 *       - 2 秒内出现画面（VGA，约 10~20fps）
 *       - 按住 ↑ 小车前进，松手停下（松手后固件 1.5 秒无指令也会自动停）
 *       - 点“说话”，页面下方出现“正在听… → 思考中… → 说话中…”
 *       - 距离小于 15cm 时“距离”数字变红，小车自动刹停
 *    3) 电脑也能用（键盘方向键 = 前进/后退/左转/右转，空格 = 停）
 *
 *  【常见错误】
 *    1) 页面打不开 / 一直转圈
 *       → 手机没连同一个 WiFi；或路由器开了“AP 隔离”，设备之间互相访问被禁
 *    2) 有页面没画面（画面区域是裂开的图标）
 *       → 摄像头没初始化成功，看串口 [CAM] 报错；先跑 04_camera 测试
 *    3) 按钮点了没反应
 *       → 手机浏览器把 http 页面当不安全页面？其实不会；先看串口有没有收到
 *         “/api/move” 请求日志。若画面在动但按钮无响应，说明 HTTP 服务被
 *         /stream 卡住了 —— 主程序必须用“边推流边处理命令”的写法（见 07_main.ino）
 *    4) 按住前进松手后车还跑
 *       → 固件里没有“掉线/超时自动停”（deadman）。主程序必须实现：
 *         超过 1.5 秒没收到运动指令就 motorStop()。这是安全底线。
 *    5) 点“说话”没反应
 *       → 看串口：ENABLE_VOICE=0？WiFi 没连？语音模块初始化失败？
 *
 *  【和主程序的约定 · HTTP 接口表】（07_main.ino 必须按这个实现）
 *    所有接口都是 GET，返回 JSON；页面只用 fetch()，不需要 POST。
 *    ┌─────────────────────────────┬──────────────────────────────────────────┐
 *    │ 路径                         │ 作用 / 返回                              │
 *    ├─────────────────────────────┼──────────────────────────────────────────┤
 *    │ GET /                       │ 返回本文件里的 HTML 页面                 │
 *    │ GET /stream                 │ MJPEG 流（multipart/x-mixed-replace）      │
 *    │ GET /api/status             │ {"ip":"192.168.1.23","rssi":-58,          │
 *    │                             │  "dist":42,"fps":15,"avoid":1,            │
 *    │                             │  "state":"IDLE","busy":0,"heard":"",       │
 *    │                             │  "reply":"","last":"","power":"电池",      │
 *    │                             │  "heap":180000,"cam":1,"voice":1}          │
 *    │ GET /api/move?dir=forward   │ 前进；dir 取值 forward/back/left/right/stop│
 *    │ GET /api/gimbal?d=left      │ 云台；d 取值 left/right/up/down/center     │
 *    │ GET /api/avoid?on=1         │ 开(1)/关(0) 自动避障                       │
 *    │ GET /api/say                │ 触发一次录音对话（异步，立即返回）          │
 *    │                             │ 返回 {"ok":1} 或 {"ok":0,"err":"..."}      │
 *    │ GET /api/stop               │ 立即停车 + 云台不动（安全按钮）             │
 *    │ GET /api/action?name=LOOK   │ 直接执行一个动作（调试用，可省略）          │
 *    └─────────────────────────────┴──────────────────────────────────────────┘
 *    state 取值: IDLE / LISTENING / THINKING / SPEAKING / ERROR
 *    注意：固件必须在 1.5 秒内没收到新的 /api/move 时自动停车（deadman）。
 *  ==========================================================================*/

#pragma once

#include <Arduino.h>

/* 页面源码放在 Flash 里（PROGMEM），不占 RAM。
 * 主程序用 server.send_P(200, "text/html; charset=utf-8", WEB_UI_HTML) 发送。
 * 注意：字符串里不能出现 )HTMLUI" 这个结束标记，改 HTML 时留意一下。 */
static const char WEB_UI_HTML[] PROGMEM = R"HTMLUI(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#0b1020">
<title>AI-Robot 遥控台</title>
<style>
:root{
  --bg:#0a0f1e; --card:#141b2e; --card2:#1b2438; --line:#2a3450;
  --txt:#e9eefc; --dim:#8b97b8; --accent:#4cc9f0; --accent2:#7b61ff;
  --ok:#3ddc97; --warn:#ffb703; --danger:#ff5d73; --shadow:0 8px 24px rgba(0,0,0,.35);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
html,body{margin:0;padding:0}
body{
  background:radial-gradient(1200px 600px at 50% -10%,#1a2450 0%,var(--bg) 55%) fixed;
  color:var(--txt);
  font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",system-ui,sans-serif;
  font-size:15px;line-height:1.45;
  padding-bottom:env(safe-area-inset-bottom);
}
.wrap{max-width:440px;margin:0 auto;padding:10px 12px 26px}
header{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:6px 2px 10px}
.brand{display:flex;align-items:center;gap:9px;font-weight:700;font-size:17px;letter-spacing:.5px}
.brand .dot{width:9px;height:9px;border-radius:50%;background:var(--danger);box-shadow:0 0 10px var(--danger)}
.brand .dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}
.ip{font-size:12px;color:var(--dim);font-variant-numeric:tabular-nums}
.card{background:linear-gradient(180deg,var(--card),var(--card2));border:1px solid var(--line);
  border-radius:16px;box-shadow:var(--shadow);padding:10px;margin-bottom:12px}
/* ---------- 画面 ---------- */
.cam{position:relative;width:100%;aspect-ratio:4/3;border-radius:12px;overflow:hidden;background:#05070d;border:1px solid var(--line)}
.cam img{width:100%;height:100%;object-fit:cover;display:block}
.cam .hint{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
  color:var(--dim);font-size:13px;text-align:center;padding:20px;background:#05070d}
.cam .badge{position:absolute;top:8px;left:8px;font-size:11px;color:var(--dim);
  background:rgba(0,0,0,.45);padding:3px 8px;border-radius:99px;backdrop-filter:blur(4px)}
/* ---------- 状态 ---------- */
.status{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:10px}
.chip{background:rgba(255,255,255,.04);border:1px solid var(--line);border-radius:11px;padding:7px 4px;text-align:center}
.chip b{display:block;font-size:16px;font-variant-numeric:tabular-nums}
.chip span{display:block;font-size:11px;color:var(--dim);margin-top:1px}
.chip.danger b{color:var(--danger)}
.chip.warn b{color:var(--warn)}
.chip.ok b{color:var(--ok)}
/* ---------- 区块标题 ---------- */
.sec{display:flex;align-items:center;justify-content:space-between;margin:2px 2px 8px}
.sec h2{margin:0;font-size:13px;font-weight:600;color:var(--dim);letter-spacing:1px}
/* ---------- 行驶方向键 ---------- */
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:9px}
.btn{user-select:none;-webkit-user-select:none;border:1px solid var(--line);border-radius:14px;
  background:linear-gradient(180deg,#202a44,#18203a);color:var(--txt);
  font-size:15px;font-weight:600;padding:0;height:66px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;
  transition:transform .05s,background .12s,border-color .12s;cursor:pointer}
.btn .ico{font-size:21px;line-height:1}
.btn .lbl{font-size:11px;color:var(--dim);font-weight:500}
.btn:active,.btn.down{background:linear-gradient(180deg,#2a8fb5,#1c6c8c);
  border-color:var(--accent);transform:scale(.97);box-shadow:0 0 18px rgba(76,201,240,.35)}
.btn.stop{background:linear-gradient(180deg,#3a1c2a,#2a1420);border-color:#5a2a3c}
.btn.stop:active,.btn.stop.down{background:linear-gradient(180deg,#c2324c,#8e2138);border-color:var(--danger);box-shadow:0 0 18px rgba(255,93,115,.4)}
.btn:disabled{opacity:.35;cursor:default}
.blank{visibility:hidden}
/* ---------- 云台 ---------- */
.gimbal{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}
.gbtn{height:52px;border-radius:12px;border:1px solid var(--line);background:#18203a;color:var(--txt);
  font-size:13px;font-weight:600;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:1px;cursor:pointer}
.gbtn .ico{font-size:17px}
.gbtn:active{background:#22305a;border-color:var(--accent2);transform:scale(.97)}
/* ---------- 开关行 ---------- */
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:4px 2px}
.row .name{font-weight:600}
.row .tip{font-size:11px;color:var(--dim);font-weight:400}
.switch{position:relative;width:54px;height:31px;flex:0 0 auto}
.switch input{opacity:0;width:0;height:0;position:absolute}
.slider{position:absolute;inset:0;background:#2a3450;border-radius:99px;transition:.18s;cursor:pointer}
.slider:before{content:"";position:absolute;width:25px;height:25px;left:3px;top:3px;border-radius:50%;
  background:#cfd8f5;transition:.18s}
.switch input:checked + .slider{background:linear-gradient(90deg,#1d7f6a,#3ddc97)}
.switch input:checked + .slider:before{transform:translateX(23px);background:#fff}
/* ---------- 说话 ---------- */
.say{width:100%;height:74px;border:none;border-radius:16px;cursor:pointer;
  background:linear-gradient(135deg,#7b61ff,#4cc9f0);color:#fff;font-size:19px;font-weight:800;
  display:flex;align-items:center;justify-content:center;gap:10px;letter-spacing:1px;
  box-shadow:0 8px 26px rgba(123,97,255,.35);transition:transform .06s,filter .15s}
.say:active{transform:scale(.985);filter:brightness(1.1)}
.say:disabled{filter:grayscale(.65) brightness(.8);box-shadow:none}
.say .mic{font-size:24px}
.sayline{margin-top:9px;font-size:13px;color:var(--dim);text-align:center;min-height:20px}
.sayline b{color:var(--txt)}
.dialog{margin-top:6px;font-size:13px;color:var(--dim);text-align:left;
  background:rgba(255,255,255,.03);border:1px solid var(--line);border-radius:10px;padding:8px 10px;display:none}
.dialog.show{display:block}
.dialog .q{color:var(--accent)}
.dialog .a{color:var(--txt)}
footer{text-align:center;font-size:11px;color:#5d688a;padding-top:6px;line-height:1.7}
@media (max-width:360px){ .btn{height:58px} .chip b{font-size:14px} }
</style>
</head>
<body>
<div class="wrap">

  <header>
    <div class="brand"><span class="dot" id="netdot"></span>AI-Robot</div>
    <div class="ip" id="ip">连接中…</div>
  </header>

  <!-- ============ 1. 摄像头画面 ============ -->
  <div class="card">
    <div class="cam">
      <img id="cam" alt="摄像头画面" src="/stream" onerror="if(window.camError)camError()">
      <div class="hint" id="camhint">摄像头连接中…</div>
      <div class="badge" id="fpsbadge">0 fps</div>
    </div>
    <div class="status">
      <div class="chip" id="chipdist"><b>--</b><span>距离 cm</span></div>
      <div class="chip" id="chipstate"><b>待机</b><span>状态</span></div>
      <div class="chip" id="chiprssi"><b>--</b><span>信号 dBm</span></div>
      <div class="chip" id="chippower"><b>--</b><span>电源</span></div>
    </div>
  </div>

  <!-- ============ 2. 行驶 ============ -->
  <div class="card">
    <div class="sec"><h2>行驶（按住走，松手停）</h2></div>
    <div class="pad">
      <div class="blank"></div>
      <button class="btn" data-move="forward"><span class="ico">▲</span><span class="lbl">前进</span></button>
      <div class="blank"></div>

      <button class="btn" data-move="left"><span class="ico">◀</span><span class="lbl">左转</span></button>
      <button class="btn stop" data-move="stop"><span class="ico">■</span><span class="lbl">停</span></button>
      <button class="btn" data-move="right"><span class="ico">▶</span><span class="lbl">右转</span></button>

      <div class="blank"></div>
      <button class="btn" data-move="back"><span class="ico">▼</span><span class="lbl">后退</span></button>
      <div class="blank"></div>
    </div>
  </div>

  <!-- ============ 3. 云台 ============ -->
  <div class="card">
    <div class="sec"><h2>云台摄像头</h2></div>
    <div class="gimbal">
      <button class="gbtn" onclick="gimbal('left')"><span class="ico">↰</span>左</button>
      <button class="gbtn" onclick="gimbal('up')"><span class="ico">↑</span>上</button>
      <button class="gbtn" onclick="gimbal('center')"><span class="ico">◎</span>回中</button>
      <button class="gbtn" onclick="gimbal('down')"><span class="ico">↓</span>下</button>
      <button class="gbtn" onclick="gimbal('right')"><span class="ico">↱</span>右</button>
    </div>
  </div>

  <!-- ============ 4. 避障开关 ============ -->
  <div class="card">
    <div class="row">
      <div>
        <div class="name">自动避障</div>
        <div class="tip">距离 &lt; 15cm 自动刹车（建议常开）</div>
      </div>
      <label class="switch"><input type="checkbox" id="avoid" checked><span class="slider"></span></label>
    </div>
  </div>

  <!-- ============ 5. 说话 ============ -->
  <div class="card">
    <button class="say" id="saybtn" onclick="say()"><span class="mic">🎤</span><span id="saytxt">说话</span></button>
    <div class="sayline" id="sayline">点一下，说一句 3 秒以内的话</div>
    <div class="dialog" id="dialog">
      <div class="q" id="heard">我说：—</div>
      <div class="a" id="reply">它答：—</div>
    </div>
  </div>

  <footer>
    同一 WiFi 下访问 · 掉线 1.5 秒自动停车<br>
    <span id="footinfo">heap -- · cam -- · voice --</span>
  </footer>
</div>

<script>
/* ============================================================
 * 页面逻辑：只做三件事
 *   1) 每秒拉一次 /api/status 更新状态
 *   2) 按钮 → GET /api/move 等接口
 *   3) 说话按钮 → GET /api/say，然后根据 state 显示进度
 * 画面是浏览器自己从 /stream 拉的 MJPEG，不需要 JS 管。
 * ============================================================ */
var $ = function (id) { return document.getElementById(id); };
var busy = false;                 /* 正在对话（禁用按钮） */
var lastState = 'IDLE';
var camRetry = 0;
var camFailed = false;            /* 画面是否断流 */
var avoidTouched = false;         /* 用户刚手动点过避障开关（别被轮询覆盖） */

function api(path) {
  return fetch(path, { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .catch(function (e) { return { ok: 0, err: String(e) }; });
}

/* ---------- 1. 状态轮询 ---------- */
function updateStatus() {
  api('/api/status').then(function (s) {
    if (s && s.ip) {
      $('ip').textContent = s.ip;
      $('netdot').className = 'dot on';
    } else {
      $('netdot').className = 'dot';
    }
    /* 距离：<0 表示没测到 */
    var d = (typeof s.dist === 'number') ? s.dist : -1;
    var chip = $('chipdist');
    chip.className = 'chip';
    if (d < 0) { $('chipdist').querySelector('b').textContent = '--'; }
    else {
      $('chipdist').querySelector('b').textContent = d;
      if (d < 15) chip.className = 'chip danger';
      else if (d < 30) chip.className = 'chip warn';
      else chip.className = 'chip ok';
    }
    /* 运行状态 */
    var map = { IDLE:'待机', LISTENING:'聆听', THINKING:'思考', SPEAKING:'说话', ERROR:'出错' };
    $('chipstate').querySelector('b').textContent = map[s.state] || '待机';
    $('chiprssi').querySelector('b').textContent = (typeof s.rssi === 'number') ? s.rssi : '--';
    $('chippower').querySelector('b').textContent = s.power || '电池';
    $('fpsbadge').textContent = (s.fps || 0) + ' fps';
    $('footinfo').textContent = 'heap ' + (s.heap || 0) + ' · cam ' + (s.cam ? 'OK' : 'FAIL') + ' · voice ' + (s.voice ? 'ON' : 'OFF');

    /* 避障开关同步固件状态（用户手动点过就不要覆盖） */
    if (typeof s.avoid !== 'undefined' && !avoidTouched) {
      $('avoid').checked = !!s.avoid;
    }

    /* 对话进度 */
    var st = s.state || 'IDLE';
    if (st !== lastState) { lastState = st; }
    if (busy) {
      if (st === 'LISTENING')      $('saytxt').textContent = '正在听…';
      else if (st === 'THINKING')  $('saytxt').textContent = '思考中…';
      else if (st === 'SPEAKING')  $('saytxt').textContent = '说话中…';
      if (st === 'IDLE' || st === 'ERROR') {
        busy = false;
        $('saybtn').disabled = false;
        $('saytxt').textContent = '说话';
        $('sayline').textContent = (st === 'ERROR') ? ('出错了：' + (s.last || '')) : '说完了，可以继续说';
        if (s.heard || s.reply) {
          $('dialog').className = 'dialog show';
          $('heard').textContent = '我说：' + (s.heard || '—');
          $('reply').textContent = '它答：' + (s.reply || '—');
        }
      }
    }
  });
}

/* ---------- 2. 行驶：按住走，松手停 ---------- */
function move(dir) { api('/api/move?dir=' + dir); }
function bindHold(btn) {
  var dir = btn.getAttribute('data-move');
  var start = function (e) { if (e) e.preventDefault(); btn.classList.add('down'); move(dir); };
  var stop  = function (e) { if (e) e.preventDefault(); btn.classList.remove('down'); move('stop'); };
  btn.addEventListener('touchstart', start, { passive: false });
  btn.addEventListener('touchend', stop);
  btn.addEventListener('touchcancel', stop);
  btn.addEventListener('mousedown', start);
  btn.addEventListener('mouseup', stop);
  btn.addEventListener('mouseleave', stop);
}
Array.prototype.forEach.call(document.querySelectorAll('[data-move]'), bindHold);

/* 安全：页面切走/切后台时，一律发停止 */
document.addEventListener('visibilitychange', function () {
  if (document.hidden) move('stop');
});
window.addEventListener('blur', function () { move('stop'); });

/* 电脑键盘：方向键移动，空格停（做实验很方便） */
var kv = null;
document.addEventListener('keydown', function (e) {
  if (e.repeat) return;
  var m = { ArrowUp:'forward', ArrowDown:'back', ArrowLeft:'left', ArrowRight:'right' }[e.key];
  if (m) { e.preventDefault(); kv = m; move(m); }
  if (e.key === ' ') { e.preventDefault(); move('stop'); }
});
document.addEventListener('keyup', function (e) {
  if (e.key && e.key.indexOf('Arrow') === 0 && kv) { kv = null; move('stop'); }
});

/* ---------- 3. 云台 ---------- */
function gimbal(d) { api('/api/gimbal?d=' + d); }

/* ---------- 4. 避障开关 ---------- */
$('avoid').addEventListener('change', function () {
  avoidTouched = true;
  api('/api/avoid?on=' + ($('avoid').checked ? '1' : '0'));
  setTimeout(function () { avoidTouched = false; }, 1500);
});

/* ---------- 5. 说话 ---------- */
function say() {
  if (busy) return;
  busy = true;
  $('saybtn').disabled = true;
  $('saytxt').textContent = '准备中…';
  $('sayline').textContent = '接下来 3 秒请说话（车会先停住）';
  move('stop');
  api('/api/say').then(function (r) {
    if (!r.ok) {
      busy = false;
      $('saybtn').disabled = false;
      $('saytxt').textContent = '说话';
      $('sayline').textContent = '触发失败：' + (r.err || '看看串口日志');
    }
  });
}

/* ---------- 6. 摄像头断流重连 ---------- */
function camError() {
  camFailed = true;
  $('camhint').style.display = 'flex';
  $('camhint').textContent = '画面断开，正在重连…（检查 04_camera）';
  if (camRetry < 30) {
    camRetry++;
    setTimeout(function () {
      $('cam').src = '/stream?t=' + new Date().getTime();
    }, 1500);
  }
}
/* MJPEG 是无限长的图片流，浏览器的 load 事件不一定会触发，
 * 所以这里用定时器兜底：2.5 秒内没报错就把“连接中”的遮罩收起来。 */
setTimeout(function () {
  if (!camFailed) $('camhint').style.display = 'none';
}, 2500);
$('cam').addEventListener('load', function () {
  camFailed = false;
  $('camhint').style.display = 'none';
  camRetry = 0;
});

/* 启动 */
updateStatus();
setInterval(updateStatus, 1000);
</script>
</body>
</html>
)HTMLUI";
