// v0.5.3 插件页真机验证：布局、齿轮弹窗、生效 Bot 多选、测试连接、搜索
import http from 'node:http';

const PORT = Number(process.env.CDP_PORT || 9261);

function httpJson(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: PORT, path }, (res) => {
      let b = '';
      res.on('data', (c) => (b += c));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}

class CDP {
  constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); }
  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.addEventListener('open', () => resolve());
      this.ws.addEventListener('error', reject);
      this.ws.addEventListener('message', (ev) => {
        const m = JSON.parse(ev.data);
        if (m.id && this.pending.has(m.id)) {
          const { resolve: r, reject: j } = this.pending.get(m.id);
          this.pending.delete(m.id);
          m.error ? j(new Error(JSON.stringify(m.error))) : r(m.result);
        }
      });
    });
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: expr, returnByValue: true, awaitPromise: true,
    });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + ' :: ' +
      (r.exceptionDetails.exception?.description || ''));
    return r.result.value;
  }
}

const results = [];
function check(name, cond, extra = '') {
  results.push({ name, ok: !!cond, extra });
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}${extra ? ' :: ' + extra : ''}`);
}

const targets = await httpJson('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.local'));
if (!page) { console.error('找不到页面目标'); process.exit(1); }

const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 1) 切到插件页
await cdp.eval(`document.querySelector('[data-view="plugins"]').click(); 1`);
await sleep(2200);

// 2) 标题与控件存在
check('标题含 Plugin 副标',
  await cdp.eval(`!!document.querySelector('#view-plugins h2 .h2-sub')`));
check('搜索框存在',
  await cdp.eval(`!!document.getElementById('plugin-search')`));
check('计数徽章存在',
  await cdp.eval(`!!document.getElementById('plugins-badge')`));
check('刷新按钮存在',
  await cdp.eval(`!!document.getElementById('btn-plugin-refresh')`));

// 3) 分组渲染
const groups = await cdp.eval(`
  [...document.querySelectorAll('#plugin-groups .plugin-group-head h3')].map(h=>h.textContent)`);
check('两个分组（内置技能 / 自制插件）',
  groups.length === 2 && groups[0].includes('内置技能') && groups[1].includes('自制插件'),
  JSON.stringify(groups));

// 4) 技能卡片：能力标签 + 齿轮
const skillCards = await cdp.eval(`document.querySelectorAll('#plugin-groups .plugin-card').length`);
check('技能卡片已渲染', skillCards >= 6, `${skillCards} 张`);
check('技能卡片有能力标签',
  await cdp.eval(`document.querySelectorAll('.plugin-ability').length >= 6`));
check('技能卡片有齿轮按钮',
  await cdp.eval(`document.querySelectorAll('[data-skill-gear]').length >= 6`));

// 5) 徽章格式 N / M 生效
const badge = await cdp.eval(`document.getElementById('plugins-badge').textContent`);
check('徽章格式为 "N / M 生效"', /^\d+ \/ \d+ 生效$/.test(badge.trim()), badge);

// 6) 搜索过滤
const beforeSearch = await cdp.eval(`document.querySelectorAll('#plugin-groups .plugin-card').length`);
await cdp.eval(`(() => {
  const s = document.getElementById('plugin-search');
  s.value = '计算';
  s.dispatchEvent(new Event('input', {bubbles:true}));
  return 1;
})()`);
await sleep(400);
const afterSearch = await cdp.eval(`document.querySelectorAll('#plugin-groups .plugin-card').length`);
check('搜索可过滤卡片', afterSearch < beforeSearch, `${beforeSearch} → ${afterSearch}`);

// 清空搜索
await cdp.eval(`(() => {
  const s = document.getElementById('plugin-search');
  s.value = '';
  s.dispatchEvent(new Event('input', {bubbles:true}));
  return 1;
})()`);
await sleep(400);
const restored = await cdp.eval(`document.querySelectorAll('#plugin-groups .plugin-card').length`);
check('清空搜索后恢复', restored === beforeSearch, `${restored}`);

// 7) 齿轮弹窗：技能参数
await cdp.eval(`document.querySelector('[data-skill-gear]').click(); 1`);
await sleep(500);
check('技能参数弹窗打开',
  await cdp.eval(`!document.getElementById('modal-skill').classList.contains('hidden')`));
const skillTitle = await cdp.eval(`document.getElementById('skill-modal-title').textContent`);
check('弹窗标题含技能名', skillTitle.includes('技能参数'), skillTitle);
check('参数表单有字段或空提示',
  await cdp.eval(`document.querySelectorAll('#skill-params-form [data-param-key]').length > 0 ||
                  document.querySelector('#skill-params-form .plugin-empty') !== null`));
await cdp.eval(`document.querySelector('#modal-skill [data-close="modal-skill"]').click(); 1`);
await sleep(300);

// 8) 齿轮弹窗：插件设置（含生效 Bot 多选 + 启用开关 + 测试连接）
await cdp.eval(`(() => {
  const b = document.querySelector('[data-plugin-gear]');
  if (b) { b.click(); return 1; }
  // 还没有自制插件时，直接开添加弹窗验证表单
  document.getElementById('btn-plugin-add').click();
  return 2;
})()`);
await sleep(500);
check('插件弹窗打开',
  await cdp.eval(`!document.getElementById('modal-plugin').classList.contains('hidden')`));
check('弹窗有生效 Bot 容器',
  await cdp.eval(`!!document.getElementById('plugin-bots')`));
check('弹窗有启用开关',
  await cdp.eval(`!!document.getElementById('plugin-enabled')`));
check('弹窗有测试连接按钮',
  await cdp.eval(`!!document.getElementById('btn-plugin-test')`));

// 9) 测试连接：对不可达地址应给出失败提示
await cdp.eval(`(() => {
  document.getElementById('plugin-url').value = 'http://127.0.0.1:9/none';
  document.getElementById('btn-plugin-test').click();
  return 1;
})()`);
await sleep(3500);
const testBox = await cdp.eval(`(() => {
  const b = document.getElementById('plugin-test-result');
  return { hidden: b.classList.contains('hidden'), cls: b.className, text: b.textContent };
})()`);
check('测试连接返回失败提示',
  !testBox.hidden && testBox.cls.includes('bad') && testBox.text.includes('✗'),
  testBox.text.slice(0, 90));

await cdp.eval(`document.querySelector('#modal-plugin [data-close="modal-plugin"]').click(); 1`);
await sleep(300);

// 10) 页面无 JS 报错
const errs = await cdp.eval(`window.__oruaErrors ? window.__oruaErrors.length : 0`);
check('无未捕获 JS 错误', !errs, `${errs}`);

// 11) 版本号
const ver = await cdp.eval(`document.getElementById('foot-version').textContent`);
check('版本号为 v0.5.3', ver.trim() === 'v0.5.3', ver);

console.log('\n---');
const failed = results.filter((r) => !r.ok);
console.log(`${results.length - failed.length}/${results.length} 通过`);
if (failed.length) {
  console.log('失败项：');
  for (const f of failed) console.log(`  - ${f.name} ${f.extra}`);
  process.exit(1);
}
