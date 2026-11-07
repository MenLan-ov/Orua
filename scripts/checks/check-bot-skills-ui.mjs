#!/usr/bin/env node
/*
 * Task 9 修复轮的可重跑回归检查（round 1：I-1 + I-2；round 2：N-1 ~ N-4）
 *   I-1：服务端技能未确认加载（首次进入且 GET 失败）时，插件页勾选行必须禁用且提交路径拒绝执行。
 *   I-2：同一时刻只允许一个技能 PUT 在飞；连点两次 → 最终服务端集合 == 用户最后一次操作。
 *   C：真正「零技能」的 Bot 仍可勾选提交。
 *   D：Bot 编辑器提交路径与插件页共用同一个 helper（payload 形状一致、skill_id 为字符串）。
 *   E（N-1）：单飞/待提交槽必须按 Bot 区分——Bot2 的提交不得吞掉 Bot1 排队中的意图，两个缓存各自对齐。
 *   F（N-2）：读取当前状态抛错后，任何 Bot 的提交路径都不能被永久卡死。
 *   G（N-3/N-4）：PUT 失败要回滚乐观缓存、错误按 job 归属上报；PUT 成功要复原「已确认加载」，之后仍能直接提交。
 *
 * 用法：
 *   node scripts/checks/check-bot-skills-ui.mjs         # 跑当前仓库的 app.js，7 个 case
 *   node scripts/checks/check-bot-skills-ui.mjs --app <path/to/app.js>  # 跑副本，验证修复前的行为
 * 全部数据来自内置 stub 假后端，不读写 data/app.db，也不启动真实后端。
 */
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const appPath = argv.includes('--app')
  ? path.resolve(argv[argv.indexOf('--app') + 1])
  : path.resolve(here, '../../src/AiCompanion.App/wwwroot/js/app.js');
const only = argv.includes('--case') ? argv[argv.indexOf('--case') + 1] : null;
const appSrc = fs.readFileSync(appPath, 'utf8');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---------------- 最小 DOM 桩 ---------------- */
function makeInputEl(tagHtml) {
  const inp = new El('input');
  const ds = /data-(skill|beskill)="([^"]*)"/.exec(tagHtml);
  if (ds) inp.dataset[ds[1]] = ds[2];
  inp.checked = /\bchecked\b/.test(tagHtml);
  inp.disabled = /\bdisabled\b/.test(tagHtml);
  return inp;
}

class El {
  constructor(tag = 'div', id = '') {
    this.tagName = String(tag).toUpperCase();
    this.id = id;
    this.children = [];
    this.parentElement = null;
    this.dataset = {};
    this.style = {};
    this._html = '';
    this._inputs = [];
    this._listeners = {};
    this.value = '';
    this.textContent = '';
    this.checked = false;
    this.disabled = false;
    this.scrollTop = 0;
    this.scrollHeight = 0;
    const set = new Set();
    this.classList = {
      add: (c) => set.add(c),
      remove: (c) => set.delete(c),
      contains: (c) => set.has(c),
      toggle: (c, force) => (force === undefined ? (set.has(c) ? set.delete(c) : set.add(c)) : (force ? set.add(c) : set.delete(c))),
    };
  }
  get innerHTML() { return this._html; }
  set innerHTML(v) {
    this._html = String(v ?? '');
    this.children = [];
    this._inputs = [...this._html.matchAll(/<input\b[^>]*>/g)].map((m) => {
      const inp = makeInputEl(m[0]);
      inp.parentElement = this;
      return inp;
    });
  }
  appendChild(c) { this.children.push(c); c.parentElement = this; return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; }
  remove() { this.parentElement?.removeChild(this); }
  allInputs() { return [...this._inputs, ...this.children.flatMap((c) => c.allInputs())]; }
  querySelector(sel) {
    if (sel === 'input') return this.allInputs()[0] || null;
    if (sel === 'input:checked') return this.allInputs().find((i) => i.checked) || null;
    const m = /^\[data-([\w-]+)\]/.exec(sel);
    if (m) return this.allInputs().find((i) => i.dataset[m[1]] !== undefined) || null;
    return null;
  }
  querySelectorAll(sel) {
    const all = this.allInputs();
    if (sel === 'input:checked') return all.filter((i) => i.checked);
    return all;
  }
  addEventListener(type, fn) { (this._listeners[type] ||= []).push(fn); }
  dispatch(type, extra = {}) {
    if (type === 'change' && this.disabled) return; // 真实浏览器里禁用控件不派发 change
    (this._listeners[type] || []).forEach((fn) => fn({ type, target: this, preventDefault() {}, stopPropagation() {}, ...extra }));
  }
  setAttribute(k, v) { this[k] = v; }
  getAttribute(k) { return this[k] ?? null; }
  getBoundingClientRect() { return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
  focus() {}
  blur() {}
  scrollIntoView() {}
  select() {}
  closest() { return null; }
}

function makeDocument() {
  const registry = new Map();
  const listeners = {};
  const doc = {
    registry,
    documentElement: new El('html'),
    body: new El('body'),
    listeners,
    getElementById(id) {
      if (!registry.has(id)) registry.set(id, new El('div', id));
      return registry.get(id);
    },
    createElement: (tag) => new El(tag),
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: (t, fn) => { (listeners[t] ||= []).push(fn); },
  };
  return doc;
}

/* ---------------- 假后端 ---------------- */
const SKILLS = [
  { id: 's1', name: '计算器', description: '四则运算' },
  { id: 's2', name: '时间日期', description: '当前时间' },
  { id: 's3', name: '网页搜索', description: '联网搜索' },
];
function makeEnv(opts = {}) {
  const doc = makeDocument();
  const server = {
    botSkills: new Map([[1, [{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }]], [2, []]]),
    applied: [],
    inFlight: 0,
    maxInFlight: 0,
    inFlightByBot: new Map(),
    maxInFlightByBot: new Map(),
    putSeq: 0,
  };
  const calls = [];
  const toasts = [];
  const api = 'http://stub';
  const sandbox = {
    console,
    document: doc,
    navigator: { clipboard: { writeText: async () => {} } },
    fetch: async (url, o = {}) => {
      const method = (o.method || 'GET').toUpperCase();
      const route = String(url).replace(api, '');
      calls.push({ method, route, body: o.body ? JSON.parse(o.body) : null });
      const reply = (data, ok = true, status = 200) => ({
        ok, status, json: async () => (ok ? { code: 0, data } : { code: status, message: '注入故障' }),
      });
      if (method === 'GET' && route === '/api/v1/skills') return reply({ skills: SKILLS });
      if (method === 'GET' && route === '/api/v1/config') return reply({});
      const m = /^\/api\/v1\/bots\/(\d+)\/skills$/.exec(route);
      if (m && method === 'GET') {
        if (opts.failBotSkills) return reply(null, false, 500);
        return reply({ skills: server.botSkills.get(Number(m[1])) || [] });
      }
      if (m && method === 'PUT') {
        const botId = Number(m[1]);
        const skills = JSON.parse(o.body).skills;
        const mySeq = ++server.putSeq;
        server.inFlight += 1;
        server.maxInFlight = Math.max(server.maxInFlight, server.inFlight);
        server.inFlightByBot.set(botId, (server.inFlightByBot.get(botId) || 0) + 1);
        server.maxInFlightByBot.set(botId, Math.max(server.maxInFlightByBot.get(botId) || 0, server.inFlightByBot.get(botId)));
        // 完成时才落库：模拟 httplib 多线程下「完成顺序 ≠ 发送顺序」
        await sleep(opts.putDelay ? opts.putDelay(mySeq, botId) : 0);
        server.inFlight -= 1;
        server.inFlightByBot.set(botId, server.inFlightByBot.get(botId) - 1);
        if (opts.failPut && opts.failPut(botId, mySeq)) return reply(null, false, 400);
        server.applied.push({ botId, skills, putSeq: mySeq });
        server.botSkills.set(botId, skills);
        return reply({});
      }
      if (method === 'POST' && route === '/api/v1/bots') return reply({ id: 9, name: JSON.parse(o.body).name });
      if (method === 'PATCH' && /^\/api\/v1\/bots\/\d+$/.test(route)) return reply({ id: Number(/(\d+)$/.exec(route)[1]) });
      if (method === 'GET' && route === '/api/v1/bots') return reply({ bots: [{ id: 1, name: 'Bot1' }, { id: 2, name: 'Bot2' }, { id: 9, name: '新Bot' }] });
      if (method === 'GET' && /^\/api\/v1\/bots\/\d+\/(memories|sessions)/.test(route)) return reply({ items: [] });
      return reply({});
    },
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); t.unref?.(); return t; },
    clearTimeout,
    setImmediate: (fn) => setImmediate(fn),
    API_BASE_HINT: api,
    __toasts: toasts,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.__API_BASE__ = api;
  const ctx = vm.createContext(sandbox);
  vm.runInContext(appSrc, ctx);
  const probe = (expr) => vm.runInContext(expr, ctx);
  probe(`App.state.bots = [{ id: 1, name: 'Bot1' }, { id: 2, name: 'Bot2' }]`);
  // 记录每次 toast 文案：错误归属（哪个 job 的失败报给了哪个调用方）只能从这里观测
  probe(`(() => { const orig = toast; toast = (m) => { __toasts.push(String(m)); return orig(m); }; })()`);
  return { ctx, sandbox, doc, server, calls, toasts, probe, sleep,
    inputsIn(id) { return doc.getElementById(id).allInputs(); },
    row(skillId) { return doc.getElementById('bot-skills-list').allInputs().find((i) => i.dataset.skill === skillId); },
    click(skillId, on) { const r = this.row(skillId); r.checked = on; r.dispatch('change'); return r; },
    selectBot(botId) { const sel = doc.getElementById('skill-bot-select'); sel.value = botId; sel.dispatch('change'); },
    putsTo(botId) { return calls.filter((c) => c.method === 'PUT' && c.route === `/api/v1/bots/${botId}/skills`); },
    toastText() { return doc.getElementById('toast').textContent; },
    async flush(ms = 120) {
      const until = Date.now() + ms;
      while (Date.now() < until) { await new Promise((r) => setImmediate(r)); await sleep(5); }
    } };
}

/* ---------------- 断言 ---------------- */
const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  → ${detail}` : ''}`);
}

/* ---------------- case A：I-1 加载失败不得提交 ---------------- */
async function caseA() {
  console.log('\n--- case A（I-1：技能加载失败 → 行禁用 + 提交拒绝 + 可恢复）---');
  const env = makeEnv({ failBotSkills: true });
  await env.probe(`switchView('plugins')`);
  await env.flush();
  const rows = env.inputsIn('bot-skills-list');
  check('A1 GET 失败后仍渲染出 3 行', rows.length === 3, `rows=${rows.length}`);
  check('A2 三行全部 disabled（不给可提交的空状态）', rows.every((r) => r.disabled), JSON.stringify(rows.map((r) => r.disabled)));
  check('A3 行内无勾选', rows.every((r) => !r.checked));
  const puts = env.calls.filter((c) => c.method === 'PUT');
  check('A4 加载失败 → 0 次 PUT', puts.length === 0, `puts=${puts.length}`);
  // 派发（禁用控件不派发）+ 直接调提交路径，验证 helper 自身也拒绝
  rows[0].dispatch('change');
  await env.flush(30);
  check('A5 强行派发 change 后仍无 PUT', env.calls.filter((c) => c.method === 'PUT').length === 0);
  let refused;
  try {
    refused = await env.probe(`submitBotSkills(1, () => []).then(() => 'resolved', (e) => 'rejected:' + e.message)`);
  } catch (e) { refused = 'throws:' + e.message; }
  check('A6 提交路径对未确认 Bot 直接拒绝', String(refused).startsWith('rejected:'), String(refused));
  check('A7 服务端真实技能未被清空', JSON.stringify(env.server.botSkills.get(1)) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }]), JSON.stringify(env.server.botSkills.get(1)));
  // 恢复：重新选择/刷新该 Bot → 加载成功 → 行可用 → 提交不丢技能
  env.probe(`(() => { App.state.skills = []; App.state.botSkills = {}; })()`);
  const env2 = makeEnv({ failBotSkills: false });
  await env2.probe(`switchView('plugins')`);
  await env2.flush();
  const ok2 = env2.inputsIn('bot-skills-list');
  check('A8 正常加载后行可编辑（无 disabled）', ok2.length === 3 && ok2.every((r) => !r.disabled));
  check('A9 勾选状态来自服务端（s1/s2 已勾选）', ok2.filter((r) => r.checked).map((r) => r.dataset.skill).join(',') === 's1,s2', ok2.map((r) => `${r.dataset.skill}:${r.checked}`).join(','));
  const c = ok2.find((r) => r.dataset.skill === 's3');
  c.checked = true;
  c.dispatch('change');
  await env2.flush();
  const last = env2.server.applied.at(-1);
  check('A10 恢复后勾选不丢既有技能', !!last && last.skills.length === 3 && last.skills.every((s) => typeof s.skill_id === 'string'), JSON.stringify(last?.skills));
}

/* ---------------- case B：I-2 串行化提交 ---------------- */
async function caseB() {
  console.log('\n--- case B（I-2：连点两次 + 乱序完成 → 最终集合 = 最后一次操作）---');
  const env = makeEnv({ putDelay: (putSeq) => (putSeq === 1 ? 40 : 1) }); // 首发慢、尾随快：并发时后者会先落库
  await env.probe(`switchView('plugins')`);
  await env.flush();
  const rows = env.inputsIn('bot-skills-list');
  const r1 = rows.find((r) => r.dataset.skill === 's1');
  const r2 = rows.find((r) => r.dataset.skill === 's2');
  check('B0 初始勾选 s1+s2', r1.checked && r2.checked);
  r1.checked = false; r1.dispatch('change');      // 用户操作 1：取消 s1
  await new Promise((r) => setImmediate(r));
  r2.checked = false; r2.dispatch('change');      // 用户操作 2：取消 s2（第一次仍在飞）
  await env.flush(200);
  const puts = env.calls.filter((c) => c.method === 'PUT');
  check('B1 恰有 2 次 PUT（首发 + 尾随补交）', puts.length === 2, `puts=${puts.length}`);
  check('B2 同一时刻最多 1 个 PUT 在飞', env.server.maxInFlight === 1, `maxInFlight=${env.server.maxInFlight}`);
  check('B3 payload 形状为 {skills:[{skill_id:string, enabled:true}]}',
    puts.every((p) => Array.isArray(p.body.skills) && p.body.skills.every((s) => typeof s.skill_id === 'string' && s.enabled === true && Object.keys(s).length === 2)),
    JSON.stringify(puts.map((p) => p.body.skills)));
  check('B4 发送顺序：[s2] 然后 []', JSON.stringify(puts.map((p) => p.body.skills.map((s) => s.skill_id))) === JSON.stringify([['s2'], []]), JSON.stringify(puts.map((p) => p.body.skills)));
  check('B5 落库顺序 = 发送顺序（无并发交叉）', JSON.stringify(env.server.applied.map((a) => a.skills.map((s) => s.skill_id))) === JSON.stringify([['s2'], []]), JSON.stringify(env.server.applied));
  check('B6 服务端最终集合 = 用户最后一次操作（空）', (env.server.botSkills.get(1) || []).length === 0, JSON.stringify(env.server.botSkills.get(1)));
  check('B7 缓存与写入结果一致（下次打开不会误判）', JSON.stringify(env.probe(`App.state.botSkills[1]`)) === '[]', JSON.stringify(env.probe(`App.state.botSkills[1]`)));
  // 再连点两次开启：最终应是两条
  const rows2 = env.inputsIn('bot-skills-list');
  rows2[0].checked = true; rows2[0].dispatch('change');
  await new Promise((r) => setImmediate(r));
  rows2[1].checked = true; rows2[1].dispatch('change');
  await env.flush(200);
  check('B8 反向连点后服务端 = 两条开启', JSON.stringify((env.server.botSkills.get(1) || []).map((s) => s.skill_id)) === JSON.stringify(['s1', 's2']), JSON.stringify(env.server.botSkills.get(1)));
  check('B9 全程仍无并发 PUT', env.server.maxInFlight === 1, `maxInFlight=${env.server.maxInFlight}`);
}

/* ---------------- case C：真正零技能的 Bot 可用 ---------------- */
async function caseC() {
  console.log('\n--- case C（服务端确认「零技能」的 Bot 仍然可勾选提交）---');
  const env = makeEnv();
  env.probe(`App.state.pluginBotId = 2`);
  await env.probe(`switchView('plugins')`);
  await env.flush();
  const rows = env.inputsIn('bot-skills-list');
  check('C1 零技能 Bot 的行不禁用', rows.length === 3 && rows.every((r) => !r.disabled), JSON.stringify(rows.map((r) => r.disabled)));
  check('C2 初始全部未勾选', rows.every((r) => !r.checked));
  rows[1].checked = true;
  rows[1].dispatch('change');
  await env.flush();
  check('C3 勾选后恰 1 次 PUT，payload 为该技能', env.calls.filter((c) => c.method === 'PUT').length === 1
    && JSON.stringify(env.server.botSkills.get(2)) === JSON.stringify([{ skill_id: 's2', enabled: true }]), JSON.stringify(env.server.botSkills.get(2)));
  rows[1].checked = false;
  env.inputsIn('bot-skills-list').find((r) => r.dataset.skill === 's2').checked = false;
  env.inputsIn('bot-skills-list').find((r) => r.dataset.skill === 's2').dispatch('change');
  await env.flush();
  check('C4 取消后 PUT [] 确实清空', JSON.stringify(env.server.botSkills.get(2)) === '[]', JSON.stringify(env.server.botSkills.get(2)));
}

/* ---------------- case D：编辑器路径与 helper 一致 ---------------- */
async function caseD() {
  console.log('\n--- case D（Bot 编辑器提交走同一个 helper，payload 形状一致）---');
  const setBotForm = (env, name, prompt) => {
    env.doc.getElementById('be-name').value = name;
    env.doc.getElementById('be-prompt').value = prompt;
  };
  const env = makeEnv();
  await env.probe(`App.run('open-bot-editor', App.state.bots[0])`);
  await env.flush();
  setBotForm(env, '小O', '你好');
  const be = env.inputsIn('be-skills');
  check('D1 编辑器预勾选 = 服务端集合', be.filter((i) => i.checked).map((i) => i.dataset.beskill).join(',') === 's1,s2', be.map((i) => `${i.dataset.beskill}:${i.checked}`).join(','));
  be.find((i) => i.dataset.beskill === 's3').checked = true;
  await env.probe(`App.run('save-bot')`);
  await env.flush();
  const put = env.calls.filter((c) => c.method === 'PUT' && /\/bots\/1\/skills$/.test(c.route));
  check('D2 保存 Bot 触发 1 次技能 PUT', put.length === 1, `puts=${put.length} calls=${env.calls.map((c) => c.method + ' ' + c.route).join('|')} toast=${env.toastText()}`);
  check('D3 payload = 三条启用、skill_id 为字符串', JSON.stringify(put[0]?.body.skills) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }, { skill_id: 's3', enabled: true }]), JSON.stringify(put[0]?.body.skills));
  check('D4 服务端与缓存同步', JSON.stringify(env.probe(`App.state.botSkills[1]`)) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }, { skill_id: 's3', enabled: true }]), JSON.stringify(env.probe(`App.state.botSkills[1]`)));
  // 加载失败时编辑器不得提交（round 0 的 botSkillsAligned 守卫仍在）
  const envF = makeEnv({ failBotSkills: true });
  await envF.probe(`App.run('open-bot-editor', App.state.bots[0])`);
  await envF.flush();
  setBotForm(envF, '小O', '你好');
  check('D5 编辑器读取失败 → 技能区空白', envF.inputsIn('be-skills').length === 0);
  await envF.probe(`App.run('save-bot')`);
  await envF.flush();
  check('D6 编辑器读取失败 → 不发 PUT，服务端技能完好', envF.calls.filter((c) => c.method === 'PUT').length === 0
    && envF.calls.some((c) => c.route === '/api/v1/bots/1')
    && (envF.server.botSkills.get(1) || []).length === 2, `calls=${envF.calls.map((c) => c.method + ' ' + c.route).join('|')}`);
  // 新建 Bot（服务端技能集必为空）可正常提交
  const envN = makeEnv();
  await envN.probe(`App.run('open-bot-editor', null)`);
  await envN.flush();
  envN.doc.getElementById('modal-bot').dataset.botId = '';
  setBotForm(envN, '新Bot', 'hi');
  envN.inputsIn('be-skills').find((i) => i.dataset.beskill === 's1').checked = true;
  await envN.probe(`App.run('save-bot')`);
  await envN.flush();
  const putN = envN.calls.filter((c) => c.method === 'PUT');
  check('D7 新建 Bot 也能提交所选技能', putN.length === 1 && putN[0].route === '/api/v1/bots/9/skills'
    && JSON.stringify(putN[0].body.skills) === JSON.stringify([{ skill_id: 's1', enabled: true }]), JSON.stringify(putN.map((p) => [p.route, p.body])));
}

/* ---------------- case E：N-1 跨 Bot 并发（单飞/待提交槽必须按 Bot 区分） ---------------- */
async function caseE() {
  console.log('\n--- case E（N-1：Bot1 排队中的意图不得被 Bot2 的提交吞掉，两个缓存各自对齐）---');
  const env = makeEnv({ putDelay: (putSeq, botId) => (botId === 1 && putSeq === 1 ? 60 : 1) });
  await env.probe(`switchView('plugins')`);
  await env.flush();
  env.click('s1', false);                                     // Bot1 操作 1 → PUT 在飞（慢）
  await new Promise((r) => setImmediate(r));
  env.click('s2', false);                                     // Bot1 操作 2 → 排入 Bot1 的待提交槽
  await new Promise((r) => setImmediate(r));
  env.selectBot(2);                                           // 真实 #skill-bot-select change
  await env.flush(15);                                        // 只等 GET 回来，Bot1 的首发 PUT 仍在飞
  const rows2 = env.inputsIn('bot-skills-list');
  check('E1 切到 Bot2 后其行可编辑', rows2.length === 3 && rows2.every((r) => !r.disabled), JSON.stringify(rows2.map((r) => r.disabled)));
  env.click('s1', true);                                      // Bot2 操作（不应毁掉 Bot1 排队的意图）
  await env.flush(400);
  check('E2 Bot1 恰 2 次 PUT（首发 + 尾随补交），Bot2 1 次', env.putsTo(1).length === 2 && env.putsTo(2).length === 1,
    `puts1=${env.putsTo(1).length} puts2=${env.putsTo(2).length}`);
  check('E3 Bot1 服务端 = Bot1 最后一次操作（全取消）', JSON.stringify(env.server.botSkills.get(1)) === '[]', JSON.stringify(env.server.botSkills.get(1)));
  check('E4 Bot1 缓存与服务端对齐（未被 Bot2 的队列跳过回写）', JSON.stringify(env.probe(`App.state.botSkills[1]`)) === JSON.stringify(env.server.botSkills.get(1)),
    `cache1=${JSON.stringify(env.probe(`App.state.botSkills[1]`))} server1=${JSON.stringify(env.server.botSkills.get(1))}`);
  check('E5 Bot2 服务端 + 缓存 = Bot2 最后一次操作', JSON.stringify(env.server.botSkills.get(2)) === JSON.stringify([{ skill_id: 's1', enabled: true }])
    && JSON.stringify(env.probe(`App.state.botSkills[2]`)) === JSON.stringify([{ skill_id: 's1', enabled: true }]),
    `server2=${JSON.stringify(env.server.botSkills.get(2))} cache2=${JSON.stringify(env.probe(`App.state.botSkills[2]`))}`);
  check('E6 每个 Bot 各自最多 1 个 PUT 在飞', (env.server.maxInFlightByBot.get(1) || 0) === 1 && (env.server.maxInFlightByBot.get(2) || 0) === 1,
    JSON.stringify([...env.server.maxInFlightByBot.entries()]));
  env.selectBot(1);                                           // 切回 Bot1：应看到「最后一次操作」而不是回退的旧集合
  await env.flush(150);
  const back = env.inputsIn('bot-skills-list');
  check('E7 切回 Bot1 行可编辑', back.length === 3 && back.every((r) => !r.disabled), JSON.stringify(back.map((r) => r.disabled)));
  check('E8 切回 Bot1 勾选 = 用户最后一次操作（无回退）', back.every((r) => !r.checked), back.map((r) => `${r.dataset.skill}:${r.checked}`).join(','));
}

/* ---------------- case F：N-2 读取当前状态抛错不得永久卡死提交 ---------------- */
async function caseF() {
  console.log('\n--- case F（N-2：read 抛错后不得把提交路径焊死，任何 Bot 都还能继续保存）---');
  const env = makeEnv();
  await env.probe(`switchView('plugins')`);
  await env.flush();
  const first = await env.probe(`submitBotSkills(1, () => { throw new Error('注入：状态读取失败'); }).then(() => 'resolved', (e) => 'rejected:' + e.message)`);
  check('F1 read 抛错上报给该次调用方', String(first).startsWith('rejected:'), String(first));
  env.doc.getElementById('btn-skill-refresh').dispatch('click');   // 真实「刷新」按钮恢复可信态
  await env.flush(80);
  check('F2 刷新后行恢复可编辑', env.inputsIn('bot-skills-list').every((r) => !r.disabled));
  env.click('s3', true);
  await env.flush(200);
  check('F3 抛错后 Bot1 仍能提交（未被永久卡死）', env.putsTo(1).length === 1
    && JSON.stringify(env.server.botSkills.get(1)) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }, { skill_id: 's3', enabled: true }]),
    `puts1=${env.putsTo(1).length} server1=${JSON.stringify(env.server.botSkills.get(1))} toast=${env.toastText()}`);
  const other = await env.probe(`loadBotSkills(2).then(() => submitBotSkills(2, () => [{ skill_id: 's1', enabled: true }])).then(() => 'resolved', (e) => 'rejected:' + e.message)`);
  await env.flush(200);
  check('F4 抛错不波及其他 Bot 的提交', String(other) === 'resolved' && env.putsTo(2).length === 1
    && JSON.stringify(env.server.botSkills.get(2)) === JSON.stringify([{ skill_id: 's1', enabled: true }]),
    `res=${String(other)} puts2=${env.putsTo(2).length} server2=${JSON.stringify(env.server.botSkills.get(2))}`);
  check('F5 抛错后两 Bot 各自仍能各发 1 个 PUT，且同 Bot 从不并发', [...env.server.maxInFlightByBot.values()].every((v) => v <= 1) && env.server.maxInFlightByBot.size === 2,
    JSON.stringify([...env.server.maxInFlightByBot.entries()]));
}

/* ---------------- case G：N-3 / N-4 PUT 失败回滚 + 成功复原可信态 ---------------- */
async function caseG() {
  console.log('\n--- case G（N-3/N-4：PUT 失败回滚缓存与错误归属；PUT 成功复原「已确认加载」）---');
  const env = makeEnv({ failPut: () => true });
  await env.probe(`switchView('plugins')`);
  await env.flush();
  env.click('s3', true);
  await env.flush(150);
  check('G1 PUT 失败仍只发 1 次请求', env.putsTo(1).length === 1, `puts=${env.putsTo(1).length}`);
  check('G2 失败后缓存回滚到服务端真值（乐观勾选不留存）',
    JSON.stringify(env.probe(`App.state.botSkills[1]`)) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }]),
    JSON.stringify(env.probe(`App.state.botSkills[1]`)));
  check('G3 服务端集合未被改动', JSON.stringify(env.server.botSkills.get(1)) === JSON.stringify([{ skill_id: 's1', enabled: true }, { skill_id: 's2', enabled: true }]), JSON.stringify(env.server.botSkills.get(1)));
  check('G4 失败按既有约定抛出服务端文案', env.toasts.includes('注入故障'), JSON.stringify(env.toasts));
  check('G5 失败后行转禁用（I-1 门未被削弱）', env.row('s1').disabled === true);
  env.doc.getElementById('btn-skill-refresh').dispatch('click');
  await env.flush(150);
  const reloaded = env.inputsIn('bot-skills-list');
  check('G6 刷新后展示服务端真值：s1/s2 勾选、s3 未勾选且可编辑',
    reloaded.every((r) => !r.disabled) && reloaded.filter((r) => r.checked).map((r) => r.dataset.skill).join(',') === 's1,s2',
    reloaded.map((r) => `${r.dataset.skill}:${r.checked}`).join(','));
  // 首发失败 + 尾随补交成功：错误只报给首发调用方，成功后行必须重新可编辑
  const envB = makeEnv({ failPut: (botId, putSeq) => putSeq === 1, putDelay: (putSeq) => (putSeq === 1 ? 40 : 1) });
  await envB.probe(`switchView('plugins')`);
  await envB.flush();
  envB.click('s1', false);
  await new Promise((r) => setImmediate(r));
  envB.click('s2', false);
  await envB.flush(300);
  check('G7 失败按 job 归属上报，成功的那次不被谎报为失败', envB.toasts.includes('注入故障') && envB.toasts.filter((t) => t === '技能已保存').length === 1, JSON.stringify(envB.toasts));
  check('G8 尾随补交后服务端 = 最后一次操作且缓存对齐', JSON.stringify(envB.server.botSkills.get(1)) === '[]'
    && JSON.stringify(envB.probe(`App.state.botSkills[1]`)) === '[]', `server=${JSON.stringify(envB.server.botSkills.get(1))} cache=${JSON.stringify(envB.probe(`App.state.botSkills[1]`))}`);
  check('G9 成功路径复原「已确认加载」→ 行重新可编辑（不再需要刷新解锁）', envB.row('s3') && !envB.row('s3').disabled, String(envB.row('s3') && envB.row('s3').disabled));
  envB.click('s3', true);
  await envB.flush(300);
  check('G10 之后再勾选可直接提交（无 400/刷新死循环）', envB.putsTo(1).length === 3
    && JSON.stringify(envB.server.botSkills.get(1)) === JSON.stringify([{ skill_id: 's3', enabled: true }]),
    `puts=${envB.putsTo(1).length} server=${JSON.stringify(envB.server.botSkills.get(1))}`);
}

const cases = { A: caseA, B: caseB, C: caseC, D: caseD, E: caseE, F: caseF, G: caseG };
console.log(`检查目标文件：${appPath}`);
for (const [k, fn] of Object.entries(cases)) {
  if (only && k !== only) continue;
  try { await fn(); } catch (e) { check(`${k} 执行`, false, e && e.message); }
}
const failed = results.filter((r) => !r.ok);
console.log(`\n合计 ${results.length} 条断言，PASS ${results.length - failed.length}，FAIL ${failed.length}`);
if (failed.length) { console.log(failed.map((f) => `  FAIL ${f.name}${f.detail ? ` → ${f.detail}` : ''}`).join('\n')); process.exitCode = 1; }
