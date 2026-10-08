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
const CGI = './cgi-bin/read';
const RESULT_CODES = {
  t03010301: '正常',
  t03010302: '容量偏低/不通过',
  t2: '不支持或数据缺失',
};

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

function nextPaint() {
  return new Promise((resolve) => {
    if (typeof requestAnimationFrame === 'function') {
      requestAnimationFrame(() => requestAnimationFrame(resolve));
    } else {
      setTimeout(resolve, 50);
    }
  });
}

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

function row(label, value) {
  const d = document.createElement('div');
  d.className = 'row';
  const l = document.createElement('div');
  l.className = 'lbl';
  l.textContent = label;
  const v = document.createElement('div');
  v.className = 'val';
  v.textContent = (value === undefined || value === null || value === '') ? '—' : value;
  d.appendChild(l);
  d.appendChild(v);
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

async function readAll() {
  const btn = $('readBtn');
  btn.disabled = true;
  btn.textContent = '读取中…';
  try {
    await nextPaint();
    const out = await readRaw();
    const d = parseKV(out);
    renderAll(d);
    toast('读取完成');
  } catch (e) {
    toast('读取失败: ' + e.message, true);
    $('sub').textContent = '读取失败: ' + e.message;
  } finally {
    btn.disabled = false;
    btn.textContent = '读取电池健康';
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

  /* hero - raw data, level and legend */
  $('pct').textContent = ok ? d.percent : '--';

  const chip = $('levelChip');
  chip.textContent = ok ? level : '未读取';
  chip.className = 'chip ' + levelClass(level);

  const bar = $('bar');
  bar.style.width = Math.max(0, Math.min(100, pct)) + '%';
  bar.className = levelClass(level);

  const legend = d.legend ||
    ('优：' + (d.good || 80) + '-100%，良：' + (d.excellent || 60) + '-' + (d.good || 80) +
     '%，差：0-' + (d.excellent || 60) + '%');
  $('legend').textContent = legend;
  const legendLines = legend.split(/[，,]/).map((s) => s.trim()).filter(Boolean).join('\n');

  const st = d.status;
  $('sub').textContent = ok
    ? (st === 'unsupported' ? '当前设备不支持'
        : st === 'abnormal' ? '容量偏低'
        : '状态正常')
    : '未能读取有效数据';

  $('hero').className = 'hero ' + levelClass(level);

  /* core - data fields only */
  const core = [
    row('原始数据', ok ? d.percent + ' %' : '—'),
    row('电池健康', ok ? d.level : '—'),
    row('区间说明', legendLines || '—'),
    row('判级比值', ok ? (d.ratio_exact || d.percent) + ' %' : '—'),
    row('记录值', d.percent_int ? d.percent_int + ' %' : '—'),
    row('结果代码', d.item_no
      ? d.item_no + '：' + (RESULT_CODES[d.item_no] || '未知结果')
      : '—'),
    row('满充容量 FCC', d.fcc ? d.fcc + ' mAh' : '—'),
    row('设计容量', d.design ? d.design + ' mAh' : '—'),
    row('有效标志 flag', d.flag),
    row('设备代号', d.project || '—'),
    row('机型', d.model || '—'),
    row('读取时间', d.time || '—'),
  ];
  render($('coreRows'), core);
  $('coreCount').textContent = ok ? '已读取' : '—';
  $('device').textContent = d.model || 'unknown';

  /* live - extra telemetry */
  const live = [
    row('当前电量', d.soc ? d.soc + ' %' : '—'),
    row('电压', d.voltage_mv ? d.voltage_mv + ' mV' : '—'),
    row('电流', d.current_ma !== undefined && d.current_ma !== '' ? d.current_ma + ' mA' : '—'),
    row('温度', d.temp_c ? d.temp_c + ' ℃' : '—'),
    row('充电状态', d.charge_status || '—'),
    row('循环次数', d.cycle_count || '—'),
  ];
  render($('liveRows'), live);
}

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
