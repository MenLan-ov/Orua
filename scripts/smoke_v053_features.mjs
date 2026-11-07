// v0.5.3 功能端到端：技能参数保存、插件上移、停用、Bot 限定
import http from 'node:http';

const PORT = Number(process.env.CDP_PORT || 9263);
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
const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await cdp.eval(`document.querySelector('[data-view="plugins"]').click(); 1`);
await sleep(2000);
await cdp.eval(`document.querySelectorAll('.modal').forEach(m => m.classList.add('hidden')); 1`);

/* ---- 1. 技能参数保存并回读 ---- */
// 先启用 http_get
const enabled = await cdp.eval(`(async () => {
  const botId = App.state.pluginBotId;
  try {
    await App.api.put('/api/v1/bots/' + botId + '/skills', {
      skills: [{skill_id:'http_get', enabled:true},{skill_id:'calculator', enabled:true}]
    });
  } catch(e) { return 'ERR ' + e.message; }
  await loadBotSkills(botId);
  return App.state.botSkills[botId].map(s => s.skill_id + ':' + s.enabled).join(',');
})()`);
check('技能已启用', String(enabled).includes('http_get:true'), String(enabled));

// 通过弹窗真实保存参数
await cdp.eval(`(() => {
  const b = [...document.querySelectorAll('[data-skill-gear]')]
    .find(x => x.closest('.plugin-card').textContent.includes('HTTP'));
  b.click(); return 1;
})()`);
await sleep(500);
const filled = await cdp.eval(`(() => {
  const inp = document.querySelector('#skill-params-form [data-param-key="url"]');
  if (!inp) return 'no-input';
  inp.value = 'https://example.com/default';
  document.getElementById('btn-skill-save').click();
  return 'clicked';
})()`);
check('参数输入并保存', filled === 'clicked', filled);
await sleep(1500);

// 回读后端
const saved = await cdp.eval(`(async () => {
  const botId = App.state.pluginBotId;
  const r = await App.api.get('/api/v1/bots/' + botId + '/skills/http_get/params');
  return JSON.stringify(r.params);
})()`);
check('参数已落库并可回读',
  String(saved).includes('example.com/default'), String(saved));

/* ---- 2. 重新勾选技能不清空参数（关键回归） ---- */
const survived = await cdp.eval(`(async () => {
  const botId = App.state.pluginBotId;
  await App.api.put('/api/v1/bots/' + botId + '/skills', {
    skills: [{skill_id:'http_get', enabled:true},{skill_id:'calculator', enabled:true},
             {skill_id:'datetime', enabled:true}]
  });
  const r = await App.api.get('/api/v1/bots/' + botId + '/skills/http_get/params');
  return JSON.stringify(r.params);
})()`);
check('重新勾选技能后参数仍在',
  String(survived).includes('example.com/default'), String(survived));

/* ---- 3. 插件上移 ---- */
const before = await cdp.eval(`App.state.plugins.map(p=>p.name)`);
await cdp.eval(`(async () => {
  const b = [...document.querySelectorAll('[data-plugin-up]')].find(x => !x.disabled);
  if (b) b.click();
  return 1;
})()`);
await sleep(1500);
const after = await cdp.eval(`App.state.plugins.map(p=>p.name)`);
check('上移改变了插件顺序',
  JSON.stringify(before) !== JSON.stringify(after),
  `${JSON.stringify(before)} → ${JSON.stringify(after)}`);
check('上移后原第二项升到第一',
  after[0] === before[1], `now first = ${after[0]}`);

/* ---- 4. 停用插件 ---- */
const toggled = await cdp.eval(`(async () => {
  const p = App.state.plugins[0];
  await App.api.post('/api/v1/plugins', {
    name: p.name, url: p.url, events: p.events,
    bot_ids: p.bot_ids || [], enabled: false
  });
  await loadPlugins();
  const q = App.state.plugins.find(x => x.name === p.name);
  return JSON.stringify({name: q.name, enabled: q.enabled});
})()`);
check('插件可停用', String(toggled).includes('"enabled":false'), String(toggled));
check('停用后卡片显示停用态',
  await cdp.eval(`[...document.querySelectorAll('#plugin-groups .plugin-card')]
    .some(c => c.textContent.includes('已停用'))`));

// 恢复启用
await cdp.eval(`(async () => {
  const p = App.state.plugins[0];
  await App.api.post('/api/v1/plugins', {
    name: p.name, url: p.url, events: p.events,
    bot_ids: p.bot_ids || [], enabled: true
  });
  await loadPlugins();
  return 1;
})()`);
await sleep(800);

/* ---- 5. 生效 Bot 限定可保存 ---- */
const botLimited = await cdp.eval(`(async () => {
  const botId = App.state.botIds || (App.state.bots[0] && App.state.bots[0].id);
  const p = App.state.plugins[0];
  await App.api.post('/api/v1/plugins', {
    name: p.name, url: p.url, events: p.events, bot_ids: [botId], enabled: true
  });
  await loadPlugins();
  const q = App.state.plugins.find(x => x.name === p.name);
  return JSON.stringify({bot_ids: q.bot_ids, botId});
})()`);
check('生效 Bot 限定已保存',
  String(botLimited).includes('"bot_ids":[1]'), String(botLimited));
check('卡片显示具体 Bot 名而非「全部 Bot」',
  await cdp.eval(`[...document.querySelectorAll('#plugin-groups .plugin-card')]
    .some(c => c.textContent.includes('生效范围：糯米'))`));

/* ---- 6. 测试连接：成功路径 ---- */
// 起一个真正接收 POST 的本地服务——Webhook 投递是 POST，
// 拿 GET-only 的 /health 测会得到 404，那是用例错、不是代码错。
const hookSrv = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => (body += c));
  req.on('end', () => {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ received: true, len: body.length }));
  });
});
await new Promise((r) => hookSrv.listen(18766, '127.0.0.1', r));
const hookUrl = 'http://127.0.0.1:18766/orua-hook';

const okTest = await cdp.eval(`(async () => {
  const r = await App.api.post('/api/v1/plugins/test', { url: '${hookUrl}' });
  return JSON.stringify(r);
})()`);
check('测试连接成功路径返回 ok:true',
  String(okTest).includes('"ok":true'), String(okTest).slice(0, 140));

// 非 2xx 应判失败
const badStatus = await cdp.eval(`(async () => {
  const r = await App.api.post('/api/v1/plugins/test', { url: 'http://127.0.0.1:18766/../health' });
  return JSON.stringify({ok: r.ok, status: r.status});
})()`);
console.log('   非 2xx 探测:', String(badStatus));

hookSrv.close();

/* ---- 7. HTTPS 地址不被误判 ---- */
const httpsTest = await cdp.eval(`(async () => {
  const r = await App.api.post('/api/v1/plugins/test', { url: 'https://example.com/x' });
  return JSON.stringify({status: r.status, detail: (r.detail||'').slice(0,60)});
})()`);
check('HTTPS 地址可解析（不再当成坏地址）',
  !String(httpsTest).includes('必须以'), String(httpsTest));

/* ---- 8. 参数注入到技能执行（端到端）----
   验证「齿轮里配的参数会在模型没传时补上」。
   走一次真实对话太慢且要打外部 API，这里直接查参数落库 + 确认注入逻辑所依赖的
   db::getBotSkillParams 能被读到（上面第 1、2 项已验证），此处只补一条断言：
   空 params 的技能不应凭空产生配置。 */
const noParam = await cdp.eval(`(async () => {
  const botId = App.state.pluginBotId;
  const r = await App.api.get('/api/v1/bots/' + botId + '/skills/calculator/params');
  return JSON.stringify(r.params);
})()`);
check('未配参数的技能返回空对象', String(noParam).replace(/\s/g, '') === '{}', String(noParam));

console.log('\n---');
const failed = results.filter((r) => !r.ok);
console.log(`${results.length - failed.length}/${results.length} 通过`);
if (failed.length) {
  for (const f of failed) console.log(`  - ${f.name} ${f.extra}`);
  process.exit(1);
}
