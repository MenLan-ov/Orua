// 截「自制插件」分组（滚到底部）
import http from 'node:http';
import fs from 'node:fs';

const PORT = Number(process.env.CDP_PORT || 9263);
const OUT = process.env.OUT_DIR || 'D:/One/dist/screenshots';

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
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text);
    return r.result.value;
  }
}

const targets = await httpJson('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.local'));
const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');
await cdp.send('Page.enable');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.mkdirSync(OUT, { recursive: true });

await cdp.send('Emulation.setDeviceMetricsOverride', {
  width: 1280, height: 860, deviceScaleFactor: 1, mobile: false,
});
await cdp.eval(`document.querySelector('[data-view="plugins"]').click(); 1`);
await sleep(2000);

async function shot(name) {
  const r = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`${OUT}/${name}.png`, Buffer.from(r.data, 'base64'));
  console.log('saved', `${OUT}/${name}.png`);
}

// 滚到自制插件分组
await cdp.eval(`(() => {
  const gs = [...document.querySelectorAll('#plugin-groups .plugin-group')];
  const g = gs[1];
  if (g) g.scrollIntoView({block:'start'});
  return g ? g.querySelectorAll('.plugin-card').length : -1;
})()`).then((n) => console.log('自制插件卡片数:', n));
await sleep(500);
await shot('v053-plugins-bottom');

// 插件齿轮弹窗（编辑态：应回填 URL / 事件 / Bot / 启用态）
const opened = await cdp.eval(`(() => {
  const b = document.querySelector('[data-plugin-gear]');
  if (!b) return 'no-gear';
  b.click();
  return 'clicked';
})()`);
console.log('gear:', opened);
await sleep(600);
const state = await cdp.eval(`(() => ({
  title: document.getElementById('plugin-modal-title').textContent,
  name: document.getElementById('plugin-name').value,
  url: document.getElementById('plugin-url').value,
  enabled: document.getElementById('plugin-enabled').checked,
  events: [...document.querySelectorAll('#plugin-events input:checked')].map(c=>c.dataset.event),
  bots: [...document.querySelectorAll('#plugin-bots input')].map(c=>({n:c.parentElement.textContent.trim(), on:c.checked})),
}))()`);
console.log('编辑态回填:', JSON.stringify(state, null, 2));
await shot('v053-plugin-edit');

console.log('done');
