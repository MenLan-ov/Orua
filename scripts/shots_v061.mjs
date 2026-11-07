// 截图当前各主要页面，用于 UI 打磨判断。
import http from 'node:http';
import fs from 'node:fs';

const PORT = process.env.CDP_PORT || '9283';
const OUT = 'dist/screenshots';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function getJson(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: PORT, path }, (res) => {
      let d = '';
      res.on('data', (c) => (d += c));
      res.on('end', () => { try { resolve(JSON.parse(d)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}

async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  let target = null;
  for (let i = 0; i < 40 && !target; i++) {
    try {
      const list = await getJson('/json/list');
      target = list.find((t) => t.type === 'page' && t.url.includes('app.local'));
    } catch (e) { /* 等待 */ }
    if (!target) await sleep(1000);
  }
  if (!target) throw new Error('找不到页面');

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  let id = 0;
  const pending = new Map();
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(JSON.stringify(m.error)));
      else resolve(m.result);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const mid = ++id;
    pending.set(mid, { resolve, reject });
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
  const evaluate = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error('JS: ' + (r.exceptionDetails.exception?.description || ''));
    return r.result.value;
  };
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${OUT}/${name}.png`, Buffer.from(r.data, 'base64'));
    console.log('saved', `${name}.png`);
  };

  const sizes = JSON.parse(process.env.SIZES || '[{"w":1280,"h":860,"tag":"def"}]');
  for (const s of sizes) {
    await send('Emulation.setDeviceMetricsOverride', {
      width: s.w, height: s.h, deviceScaleFactor: 1, mobile: false,
    });
    await sleep(700);

    for (const view of ['personas', 'bots', 'messages', 'memory', 'plugins']) {
      await evaluate(`(async () => { await switchView('${view}'); })()`);
      await sleep(900);
      await shot(`${s.tag}-${view}`);
    }
  }

  // 人设编辑弹窗
  await send('Emulation.setDeviceMetricsOverride', {
    width: 1280, height: 860, deviceScaleFactor: 1, mobile: false,
  });
  await evaluate(`(async () => {
    await switchView('personas');
    document.getElementById('btn-persona-add').click();
  })()`);
  await sleep(700);
  await shot('modal-persona');

  ws.close();
  process.exit(0);
}

main().catch((e) => { console.error('失败:', e.message); process.exit(1); });
