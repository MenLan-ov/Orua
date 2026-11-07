// 截图插件页：整页 + 齿轮弹窗
import http from 'node:http';
import fs from 'node:fs';

const PORT = Number(process.env.CDP_PORT || 9262);
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

// 窗口固定成常见桌面尺寸，保证截图可比
await cdp.send('Emulation.setDeviceMetricsOverride', {
  width: 1280, height: 860, deviceScaleFactor: 1, mobile: false,
});

await cdp.eval(`document.querySelector('[data-view="plugins"]').click(); 1`);
await sleep(2200);

async function shot(name) {
  const r = await cdp.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
  const p = `${OUT}/${name}.png`;
  fs.writeFileSync(p, Buffer.from(r.data, 'base64'));
  console.log('saved', p);
}

await shot('v053-plugins');

// 技能参数弹窗
await cdp.eval(`(() => {
  const b = [...document.querySelectorAll('[data-skill-gear]')]
    .find(x => x.closest('.plugin-card').textContent.includes('提醒'));
  (b || document.querySelector('[data-skill-gear]')).click();
  return 1;
})()`);
await sleep(600);
await shot('v053-skill-params');
await cdp.eval(`document.querySelector('#modal-skill [data-close="modal-skill"]').click(); 1`);
await sleep(300);

// 插件设置弹窗（添加态，展示生效 Bot 多选 + 测试连接）
await cdp.eval(`document.getElementById('btn-plugin-add').click(); 1`);
await sleep(500);
await shot('v053-plugin-modal');

console.log('done');
