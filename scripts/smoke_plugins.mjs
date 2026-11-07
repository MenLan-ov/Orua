// 通过 WebView2 CDP 验证 v0.5.3 插件页：布局、添加插件弹窗、真实提交、删除。
// 用法：先启动 AiCompanion.exe（带 --remote-debugging-port），再 node scripts\smoke_plugins.mjs
import http from 'node:http';

const CDP_PORT = Number(process.env.CDP_PORT || 9222);

function get(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: CDP_PORT, path }, (res) => {
      let d = '';
      res.on('data', (c) => (d += c));
      res.on('end', () => resolve(JSON.parse(d)));
    }).on('error', reject);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error('FAIL: ' + msg); process.exit(1); };
const pass = (msg) => console.log('PASS: ' + msg);

const targets = await get('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('index.html'));
if (!page) fail('未找到页面目标');

const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });

let id = 0;
const pending = new Map();
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
  }
};
function send(method, params = {}) {
  return new Promise((resolve) => {
    const mid = ++id;
    pending.set(mid, resolve);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
}
async function evaluate(expression) {
  const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
  if (r.result?.exceptionDetails) {
    throw new Error(r.result.exceptionDetails.exception?.description || 'evaluate failed');
  }
  return r.result?.result?.value;
}

await send('Runtime.enable');

// ---- 1. 切到插件页 ----
await evaluate(`document.querySelector('.tab-btn[data-view="plugins"]').click()`);
await sleep(1500);

const layout = await evaluate(`(() => {
  const view = document.getElementById('view-plugins');
  const visible = !view.classList.contains('hidden');
  const addBtn = !!document.getElementById('btn-plugin-add');
  const skillCards = document.querySelectorAll('#skills-list .skill-card').length;
  const grid = !!document.querySelector('#skills-list.skill-grid');
  const sideSel = !!document.getElementById('skill-bot-select');
  // 检查技能标题是否被压断行：卡片宽度应足够容纳一行
  const card = document.querySelector('#skills-list .skill-card');
  const h4 = card ? card.querySelector('h4') : null;
  const h4Wrapped = h4 ? h4.getBoundingClientRect().height > 26 : false;
  return { visible, addBtn, skillCards, grid, sideSel, h4Wrapped };
})()`);

if (!layout.visible) fail('插件页未显示');
if (!layout.addBtn) fail('缺少「添加插件」按钮');
if (!layout.grid) fail('技能列表未使用网格布局');
if (layout.skillCards < 6) fail(`技能卡片数量异常：${layout.skillCards}`);
if (layout.h4Wrapped) fail('技能标题仍被挤成多行（布局未修好）');
pass(`插件页布局正常（技能 ${layout.skillCards} 个，标题单行）`);

// ---- 2. 打开添加插件弹窗 ----
await evaluate(`document.getElementById('btn-plugin-add').click()`);
await sleep(400);
const modal = await evaluate(`(() => ({
  open: !document.getElementById('modal-plugin').classList.contains('hidden'),
  title: document.getElementById('plugin-modal-title').textContent,
  events: document.querySelectorAll('#plugin-events input').length
}))()`);
if (!modal.open) fail('添加插件弹窗未打开');
if (modal.events < 4) fail(`事件选项不足：${modal.events}`);
pass(`添加插件弹窗打开，标题「${modal.title}」，事件 ${modal.events} 个`);

// ---- 3. 填表并真实点击保存 ----
const marker = 'smoke-plugin-' + Date.now();
await evaluate(`(() => {
  document.getElementById('plugin-name').value = ${JSON.stringify(marker)};
  document.getElementById('plugin-url').value = 'https://example.com/orua-smoke';
  const cb = document.querySelector('#plugin-events input[data-event="chat.completed"]');
  if (cb) cb.checked = true;
  return true;
})()`);
await evaluate(`document.getElementById('btn-plugin-save').click()`);
await sleep(1500);

const afterSave = await evaluate(`(() => {
  const rows = [...document.querySelectorAll('#plugins-list .plugin-row')];
  const names = rows.map((r) => r.querySelector('.plugin-row-name').textContent.trim());
  return {
    modalClosed: document.getElementById('modal-plugin').classList.contains('hidden'),
    names,
    count: document.getElementById('plugins-count').textContent,
  };
})()`);
if (!afterSave.modalClosed) fail('保存后弹窗未关闭');
if (!afterSave.names.includes(marker)) fail(`列表未出现新插件：${JSON.stringify(afterSave.names)}`);
pass(`插件已通过 UI 保存并出现在列表（共 ${afterSave.names.length} 个，计数「${afterSave.count}」）`);

// ---- 4. 校验错误提示 ----
await evaluate(`document.getElementById('btn-plugin-add').click()`);
await sleep(300);
await evaluate(`(() => {
  document.getElementById('plugin-name').value = 'bad';
  document.getElementById('plugin-url').value = 'ftp://nope';
  return true;
})()`);
await evaluate(`document.getElementById('btn-plugin-save').click()`);
await sleep(700);
const errShown = await evaluate(`(() => {
  const el = document.getElementById('plugin-form-error');
  return { hidden: el.classList.contains('hidden'), text: el.textContent.trim() };
})()`);
if (errShown.hidden) fail('非法 URL 未给出错误提示');
pass(`非法 URL 被拦截：「${errShown.text}」`);
await evaluate(`document.querySelector('#modal-plugin [data-close]').click()`);
await sleep(300);

// ---- 5. 删除 ----
await evaluate(`window.confirm = () => true`);
await evaluate(`(() => {
  const btn = [...document.querySelectorAll('[data-plugin-del]')]
    .find((b) => b.dataset.pluginDel === ${JSON.stringify(marker)});
  if (btn) btn.click();
  return !!btn;
})()`);
await sleep(1500);
const afterDel = await evaluate(`(() => {
  const names = [...document.querySelectorAll('#plugins-list .plugin-row-name')]
    .map((e) => e.textContent.trim());
  return { names, stillThere: names.includes(${JSON.stringify(marker)}) };
})()`);
if (afterDel.stillThere) fail('删除后插件仍在列表');
pass('插件已删除并从列表移除');

// ---- 6. 版本号 ----
const ver = await evaluate(`document.getElementById('foot-version').textContent`);
if (ver !== 'v0.5.3') fail(`脚注版本号应为 v0.5.3，实际 ${ver}`);
pass(`脚注版本号 ${ver}`);

console.log('\n全部插件页断言通过。');
ws.close();
