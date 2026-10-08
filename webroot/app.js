/* OPPO Battery Health — WebUI
 *
 * 两种取数方式:
 *   1. KernelSU / APatch 内置 WebUI: 走 ksu.exec 桥直接执行模块脚本
 *   2. 普通浏览器 (Magisk 用户): 走 http://127.0.0.1:<port>/cgi-bin/read
 *
 * 注意: 传给 ksu.exec 的 shell 命令里不能出现 exit，否则会终止常驻 shell。
 */

const MOD_ROOT = '/data/adb/modules/oppo_battery_health';
const SCRIPT = MOD_ROOT + '/system/bin/batteryhealth';
const REFRESH_MS = 15000;
const CGI = './cgi-bin/read';

let autoTimer = null;
let lastData = null;

/* ---------------------------------------------------------------- bridge */

function exec(cmd, timeout = 30000) {
  return new Promise((resolve) => {
    const id = 'cb_' + Math.random().toString(36).slice(2);
    let done = false;
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      delete window[id];
      resolve({ errno: -1, stdout: '', stderr: '命令超时' });
    }, timeout);
    window[id] = (errno, stdout, stderr) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      delete window[id];
      resolve({ errno, stdout: stdout || '', stderr: stderr || '' });
    };
    try {
      const b = bridge();
      if (!b) throw new Error('no bridge');
      b.exec(cmd, '{}', id);
    } catch (e) {
      if (done) return;
      done = true;
      clearTimeout(timer);
      delete window[id];
      resolve({ errno: -2, stdout: '', stderr: '未找到 WebUI 桥（ksu/ap）。请在 KernelSU 或 APatch 管理器中打开本页面。' });
    }
  });
}

function bridge() {
  if (window.ksu && typeof window.ksu.exec === 'function') return window.ksu;
  if (window.kernelsu && typeof window.kernelsu.exec === 'function') return window.kernelsu;
  if (window.ap && typeof window.ap.exec === 'function') return window.ap;
  if (window.apatch && typeof window.apatch.exec === 'function') return window.apatch;
  return null;
}

async function sh(cmd, timeout) {
  const r = await exec(cmd, timeout);
  if (r.errno !== 0 && !String(r.stdout).trim())
    throw new Error((r.stderr || ('exit ' + r.errno)).trim());
  return r.stdout;
}

/* ---------------------------------------------------------------- utils */

function $(id) { return document.getElementById(id); }

let toastTimer = null;
function toast(msg, isErr) {
  const el = $('toast');
  el.textContent = msg;
  el.className = 'toast show' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast'; }, 2800);
}

function parseKV(text) {
  const o = {};
  String(text).split('\n').forEach((line) => {
    const i = line.indexOf('=');
    if (i > 0) o[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  });
  return o;
}

function row(label, value, hint, cls) {
  const d = document.createElement('div');
  d.className = 'row' + (cls ? ' ' + cls : '');
  const l = document.createElement('div');
  l.className = 'lbl';
  l.textContent = label;
  const v = document.createElement('div');
  v.className = 'val';
  v.textContent = (value === undefined || value === null || value === '') ? '—' : value;
  d.appendChild(l);
  d.appendChild(v);
  if (hint) {
    const h = document.createElement('div');
    h.className = 'hint';
    h.textContent = hint;
    d.appendChild(h);
  }
  return d;
}

function render(container, rows) {
  container.textContent = '';
  rows.forEach((r) => container.appendChild(r));
}

function levelClass(level) {
  if (!level) return 'lv-none';
  if (level.indexOf('优') >= 0) return 'lv-good';
  if (level.indexOf('良') >= 0) return 'lv-ok';
  if (level.indexOf('差') >= 0) return 'lv-bad';
  return 'lv-none';
}

/* ---------------------------------------------------------------- read */

async function readAll(silent) {
  const btn = $('readBtn');
  if (!silent) {
    btn.disabled = true;
    btn.textContent = '读取中…';
  }
  try {
    const out = await readRaw();
    const d = parseKV(out);
    lastData = d;
    renderAll(d);
    if (!silent) toast('读取完成');
  } catch (e) {
    if (!silent) toast('读取失败: ' + e.message, true);
    $('sub').textContent = '读取失败: ' + e.message;
  } finally {
    if (!silent) {
      btn.disabled = false;
      btn.textContent = '读取电池健康';
    }
  }
}

/* 优先用 WebUI 桥；没有桥时退回本地 httpd 的 CGI */
async function readRaw() {
  if (bridge()) {
    return await sh('sh ' + SCRIPT + ' --kv --extra', 40000);
  }
  const r = await fetch(CGI + '?t=' + Date.now(), { cache: 'no-store' });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return await r.text();
}

function renderAll(d) {
  const ok = d.ok === '1' && d.percent;
  const pct = ok ? Number(d.percent) : 0;
  const level = ok ? d.level : '未读取';

  /* hero — raw data with 2 decimals, plus the official grade and legend */
  $('pct').textContent = ok ? d.percent : '--';

  const chip = $('levelChip');
  chip.textContent = ok ? (level + (d.level_en ? ' · ' + d.level_en : '')) : '未读取';
  chip.className = 'chip ' + levelClass(level);

  const bar = $('bar');
  bar.style.width = Math.max(0, Math.min(100, pct)) + '%';
  bar.className = levelClass(level);

  $('legend').textContent = d.legend ||
    ('优：' + (d.good || 80) + '-100%，良：' + (d.excellent || 60) + '-' + (d.good || 80) +
     '%，差：0-' + (d.excellent || 60) + '%');

  const st = d.status;
  $('sub').textContent = ok
    ? (st === 'unsupported' ? '官方结果：不支持'
        : st === 'abnormal' ? '官方结果：不通过（容量偏低）'
        : '官方结果：正常')
    : '未能读取：缺少官方节点';

  $('ref').textContent = ok
    ? '判级用未舍入值 ' + (d.ratio_exact || d.percent) +
      '　官方记录值 ' + (d.percent_int || '—')
    : '';
  $('hero').className = 'hero ' + levelClass(level);

  /* core — 030103 official fields */
  const core = [
    row('原始数据', ok ? d.percent + ' %' : '—', 'FCC ÷ 设计容量 × 100，保留两位小数'),
    row('等级', ok ? d.level : '—', '官方界面显示的就是这个'),
    row('区间说明', d.legend || '—', '与官方 detail 文案一致'),
    row('判级比值', ok ? (d.ratio_exact || d.percent) + ' %' : '—', '未舍入，等级由它判定'),
    row('记录值', d.percent_int ? d.percent_int + ' %' : '—', '官方记录中的 (int) 值'),
    row('官方项', d.item_no || '—',
        st === 'unsupported' ? '官方结果为不支持'
          : st === 'abnormal' ? '官方标记为不通过'
          : '官方结果为正常'),
    row('满充容量 FCC', d.fcc ? d.fcc + ' mAh' : '—', d.fcc_src),
    row('设计容量', d.design ? d.design + ' mAh' : '—', d.design_src),
    row('有效标志 flag', d.flag, d.flag === '1' ? '官方判定可用' : '官方结果为不支持'),
    row('阈值', 'good ' + (d.good || '—') + ' / excellent ' + (d.excellent || '—'), '项目号 ' + (d.project || '未知')),
    row('机型', d.model || '—', (d.device || '') + (d.rom ? ' · ' + d.rom : '')),
  ];
  render($('coreRows'), core);
  $('coreCount').textContent = ok ? '已读取' : '—';
  $('device').textContent = d.model || 'unknown';

  /* live — extra telemetry, not part of 030103 */
  const live = [
    row('当前电量', d.soc ? d.soc + ' %' : '—'),
    row('电压', d.voltage_mv ? d.voltage_mv + ' mV' : '—'),
    row('电流', d.current_ma !== undefined && d.current_ma !== '' ? d.current_ma + ' mA' : '—'),
    row('温度', d.temp_c ? d.temp_c + ' ℃' : '—'),
    row('充电状态', d.charge_status || '—'),
    row('电池健康', d.health || '—', '系统枚举值，不是健康度'),
    row('循环次数', d.cycle_count || '—'),
  ];
  render($('liveRows'), live);

  /* raw */
  const raw = [
    row('FCC 节点', d.fcc_src || '—'),
    row('HAL 读数', d.fcc_hal ? d.fcc_hal + ' mAh' : '不可用（HIDL 或服务未启动）'),
    row('节点读数', d.fcc_node ? d.fcc_node + ' mAh' : '—'),
    row('设计容量节点', d.design_src || '—'),
    row('uevent', d.uevent || '—'),
    row('读取时间', d.time || '—'),
  ];
  render($('rawRows'), raw);
}

/* ---------------------------------------------------------------- auto */

function autoToggle() {
  const sw = $('autoSwitch');
  if (autoTimer) {
    clearInterval(autoTimer);
    autoTimer = null;
    sw.classList.remove('on');
    $('autoDesc').textContent = '每 15 秒重新读取一次';
    toast('已关闭自动刷新');
    return;
  }
  autoTimer = setInterval(() => readAll(true), REFRESH_MS);
  sw.classList.add('on');
  $('autoDesc').textContent = '已开启，每 15 秒刷新';
  toast('已开启自动刷新');
}

/* ---------------------------------------------------------------- misc */

function toggleRaw() {
  const box = $('rawRows');
  const arrow = $('rawArrow');
  const show = box.style.display === 'none';
  box.style.display = show ? 'block' : 'none';
  arrow.classList.toggle('open', show);
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden && autoTimer) {
    clearInterval(autoTimer);
    autoTimer = null;
    $('autoSwitch').classList.remove('on');
    $('autoDesc').textContent = '每 15 秒重新读取一次';
  }
});

(async function init() {
  try {
    if (bridge()) {
      const model = (await sh('getprop ro.product.model')).trim();
      if (model) $('device').textContent = model;
    }
  } catch (e) { /* ignore */ }
  /* No reading here on purpose: the page stays idle until the user presses
     读取电池健康. */
})();
