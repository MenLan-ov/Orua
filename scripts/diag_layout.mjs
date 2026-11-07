// 布局诊断：在不同窗口尺寸下量关键元素的位置，找出被挤出视口的部件
import http from 'node:http';
import fs from 'node:fs';

const PORT = Number(process.env.CDP_PORT || 9271);
const OUT = 'D:/One/dist/screenshots';

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

const targets = await httpJson('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.local'));
const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');
await cdp.send('Page.enable');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.mkdirSync(OUT, { recursive: true });

const probe = `(() => {
  const r = (el) => {
    if (!el) return null;
    const b = el.getBoundingClientRect();
    return { x: Math.round(b.x), y: Math.round(b.y),
             w: Math.round(b.width), h: Math.round(b.height),
             bottom: Math.round(b.bottom), right: Math.round(b.right) };
  };
  const de = document.documentElement;
  return {
    viewport: { w: innerWidth, h: innerHeight },
    docScroll: { sw: de.scrollWidth, sh: de.scrollHeight, cw: de.clientWidth, ch: de.clientHeight },
    bodyScroll: { sw: document.body.scrollWidth, sh: document.body.scrollHeight },
    app: r(document.getElementById('app')),
    main: r(document.getElementById('main')),
    sidebar: r(document.getElementById('sidebar')),
    content: r(document.getElementById('content')),
    viewMessages: r(document.getElementById('view-messages')),
    composer: r(document.getElementById('composer')),
    input: r(document.getElementById('input')),
    sendBtn: r(document.getElementById('btn-send')),
    chatHead: r(document.querySelector('.chat-head')),
    messages: r(document.getElementById('messages')),
  };
})()`;

const sizes = [
  { w: 1484, h: 420, label: 'wide-short' },   // 图 1 形态：宽而矮
  { w: 214, h: 1152, label: 'narrow-tall' },  // 图 2 形态：极窄
  { w: 988, h: 655, label: 'normal' },
  { w: 1280, h: 860, label: 'default' },
];

for (const s of sizes) {
  // 必须先把视图切回聊天页再量：否则 view-messages / composer 全是 hidden，
  // 量出来的宽高都是 0，看着像布局坏了、其实只是没显示。
  await cdp.eval(`(async () => {
    if (typeof switchView === 'function') await switchView('messages');
  })()`);
  await sleep(500);
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: s.w, height: s.h, deviceScaleFactor: 1, mobile: false,
  });
  await sleep(400);
  const info = await cdp.eval(probe);
  console.log(`\n=== ${s.label} ${s.w}x${s.h} ===`);
  console.log(JSON.stringify(info, null, 1));
  const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`${OUT}/layout-${s.label}.png`, Buffer.from(shot.data, 'base64'));
}
console.log('\ndone');
