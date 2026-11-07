// 干净截图：先关掉所有弹窗
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
  width: 1280, height: 900, deviceScaleFactor: 1, mobile: false,
});
await cdp.eval(`document.querySelector('[data-view="plugins"]').click(); 1`);
await sleep(2000);
// 关掉所有弹窗
await cdp.eval(`document.querySelectorAll('.modal').forEach(m => m.classList.add('hidden')); 1`);
await sleep(200);

async function shot(name) {
  const r = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`${OUT}/${name}.png`, Buffer.from(r.data, 'base64'));
  console.log('saved', `${OUT}/${name}.png`);
}

// 滚到自制插件
await cdp.eval(`(() => {
  const g = document.querySelectorAll('#plugin-groups .plugin-group')[1];
  if (g) g.scrollIntoView({ block: 'start' });
  return 1;
})()`);
await sleep(500);
await shot('v053-plugins-bottom');

// 插件编辑弹窗（有内容的）
await cdp.eval(`document.querySelector('[data-plugin-gear]').click(); 1`);
await sleep(600);
await shot('v053-plugin-edit');

// 技能参数弹窗（HTTP 请求，字段多）
await cdp.eval(`document.querySelector('#modal-plugin [data-close="modal-plugin"]').click(); 1`);
await sleep(300);
await cdp.eval(`(() => {
  const b = [...document.querySelectorAll('[data-skill-gear]')]
    .find(x => x.closest('.plugin-card').textContent.includes('HTTP'));
  (b || document.querySelector('[data-skill-gear]')).click();
  return 1;
})()`);
await sleep(600);
await shot('v053-skill-params');

console.log('done');
