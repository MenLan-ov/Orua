#!/usr/bin/env node
/*
 * Task 10「到期提醒」+ 终验修复波第二轮（F8/F9）的可重跑 stub 检查。
 *   A：每个 id 一次会话内只提示一次，并 naming 已知 Bot；文案原样落在 toast 元素上。
 *   B：mark-done 用正确的 id 调用 POST /api/v1/reminders/{id}/done；完成后服务端剔除。
 *   C：mark-done 失败不得循环刷屏（同一 id 仍只提示一次）。
 *   D：detached 主窗口守卫——detached=true 时 startReminderPoll 不发任何提醒请求、不建定时器。
 *   E：重叠 tick 不得双飞（单飞闸门）。
 *   F（F8 核心）：一轮 5 条到期时，同一个 tick 只允许一次 toast() 调用；被标 done 的那条必须就是
 *      tick 结束后 toast 元素上的可见文案（正文 + 「另有 N 条待提醒」计数），其余既不记账也不标 done；
 *      连续 5 轮逐条消化，5 轮可见文案各不相同。toast() 是单元素后写覆盖前写，所以「调用列表」
 *      不再算证据，只有 textContent 算——旧断言只看 env.toasts，假闭合就是这么混过去的。
 *   G：一轮 2 条 → 第 1 轮提示最早那条并带「另有 1 条待提醒」，第 2 轮提示剩下那条（无计数后缀）。
 *   H：排队不丢：每轮至多 1 条 done，5 条 5 轮全部落 done，可见文案始终是提醒正文（不再出现纯计数条）。
 *   I：呈现失败（toast 元素写不进）→ 一条都不标 done、一条都不记账。
 *      降级说明：brokenToast 模拟的是「元素不可写」这一呈现失败分支，不是真实的后写覆盖前写；
 *      可见性/覆写的主要证据在 F/G/H（stepTick + assertVisible），本组只作补充覆盖。
 *   J（F9）：群状态变更（SSE status 事件与 group-start/pause/resume/stop 命令）必须同时回写
 *      App.state.groups[].status 并重渲染 #session-list，使「进行中」徽标实时出现；
 *      切回 idle 时徽标消失且「群聊已停止」系统提示只出现一次（prev 语义未被破坏）；
 *      未选中的其它群状态不受影响。
 *   J11（NEW-13）：后端重启把群复位 idle，页面未重载时陈旧徽标必须在 false→true 重连时
 *      随整表刷新清除（刷新与重渲染需串行，并发会把陈旧数组又写回去）；持续在线不重复刷新，
 *      detached 窗口不代主窗口刷新。
 * 全部走内置 stub 假后端，绝不启动真实后端、绝不读写 data/ 或 data/app.db。
 * 用法：node scripts/checks/check-reminders-ui.mjs [--app <path/to/app.js>]
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
const appSrc = fs.readFileSync(appPath, 'utf8');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  → ${detail}` : ''}`);
}

const BOTS = [{ id: 1, name: '小O' }, { id: 2, name: '阿库' }];

// 期望文案形状：正文优先，剩余待发条数以「（另有 N 条待提醒）」并进同一条文案，
// 绝不另起第二次 toast()——一个同步 tick 里只有最后一次 toast() 的文案能被用户看到。
function reminderText(r, pending) {
  const b = BOTS.find((x) => x.id === r.bot_id);
  const base = b ? `${b.name}提醒你：${r.content}` : `提醒你：${r.content}`;
  return pending > 0 ? `${base}（另有 ${pending} 条待提醒）` : base;
}

function makeEl() {
  const set = new Set();
  // innerHTML 用访问器：真实 DOM 里清空 innerHTML 会同步移除子节点，
  // 若只当普通字段，children 会残留 → 未来按 children 长度断言的用例会拿到假结果。
  let html = '';
  const el = {
    value: '', textContent: '', dataset: {}, children: [],
    classList: { add: (c) => set.add(c), remove: (c) => set.delete(c), contains: (c) => set.has(c), toggle: (c, f) => (f ? set.add(c) : set.delete(c)) },
    addEventListener() {},
    // 子节点 innerHTML 累加进父 innerHTML：renderChatList 走「createElement + innerHTML + appendChild」，
    // 只记父元素自身 innerHTML 会永远看不到徽标，J 组就没有证据。
    appendChild(c) { el.children.push(c); html += (c && c.innerHTML) || ''; return c; },
    querySelector() { return null; }, querySelectorAll() { return []; },
    setAttribute() {}, getAttribute() { return null; }, closest() { return null; }, focus() {},
  };
  Object.defineProperty(el, 'innerHTML', {
    get: () => html,
    set: (v) => { html = String(v ?? ''); if (!html) el.children.length = 0; },
  });
  return el;
}

// opts: { due:[{id,bot_id,content,due_at}], failDone:Set<number>, delayMs:number,
//         brokenToast:boolean（元素不可写 = 呈现失败，非覆写丢失） }
function makeEnv(opts = {}) {
  const due = opts.due || [];
  const failDone = opts.failDone || new Set();
  const calls = [];
  const toasts = [];
  const interval = { started: 0, fire: null, period: null };
  const inflight = { get: 0, maxGet: 0 };
  const api = 'http://stub';

  const elRegistry = new Map();
  if (opts.brokenToast) {
    const frozen = makeEl();
    Object.defineProperty(frozen, 'textContent', { get: () => '', set: () => {} });
    elRegistry.set('toast', frozen);
  }
  const document = {
    documentElement: makeEl(), body: makeEl(),
    getElementById(id) { if (!elRegistry.has(id)) elRegistry.set(id, makeEl()); return elRegistry.get(id); },
    createElement: () => makeEl(), querySelector: () => null, querySelectorAll: () => [], addEventListener() {},
  };

  const sandbox = {
    console,
    document,
    navigator: { clipboard: { writeText: async () => {} } },
    location: { search: '' },
    URLSearchParams: class { constructor() {} get() { return null; } },
    fetch: async (url, o = {}) => {
      const method = (o.method || 'GET').toUpperCase();
      const route = String(url).replace(api, '');
      calls.push({ method, route });
      const reply = (data) => ({ ok: true, status: 200, json: async () => ({ code: 0, data }) });
      // 群整表刷新（后端重连路径 J11 用）：返回服务端真值的副本，避免被测代码与桩共享对象
      if (method === 'GET' && route === '/api/v1/groups')
        return reply((opts.groupsList || []).map((g) => ({ ...g })));
      if (method === 'GET' && route === '/api/v1/reminders?due=1') {
        inflight.get += 1;
        inflight.maxGet = Math.max(inflight.maxGet, inflight.get);
        try {
          if (opts.delayMs) await sleep(opts.delayMs);
          return reply({ reminders: due.filter((r) => !r.__done) });
        } finally {
          inflight.get -= 1;
        }
      }
      const m = /^\/api\/v1\/reminders\/(\d+)\/done$/.exec(route);
      if (m && method === 'POST') {
        const id = Number(m[1]);
        if (failDone.has(id)) {
          return { ok: false, status: 500, json: async () => ({ code: 500, message: '注入故障' }) };
        }
        const r = due.find((x) => x.id === id);
        if (r) r.__done = true;
        return reply({});
      }
      // J 组：群命令按服务端契约回一个 state，用于验证四条命令的成功路径也走「回写 + 重渲染」
      const gcmd = /^\/api\/v1\/groups\/(\d+)\/(start|pause|resume|stop)$/.exec(route);
      if (gcmd && method === 'POST') {
        const next = { start: 'running', pause: 'paused', resume: 'running', stop: 'idle' }[gcmd[2]];
        return reply({ state: next });
      }
      return reply({});
    },
    setInterval: (fn, ms) => { interval.started += 1; interval.fire = fn; interval.period = ms; return { id: interval.started }; },
    clearInterval: () => {},
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); t.unref?.(); return t; },
    clearTimeout,
    __toasts: toasts,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.__API_BASE__ = api;

  const ctx = vm.createContext(sandbox);
  vm.runInContext(appSrc, ctx);
  const probe = (expr) => vm.runInContext(expr, ctx);
  probe(`App.state.bots = ${JSON.stringify(BOTS)}`);
  // 包装 toast 时仍然调用真实实现：env.toasts 记「调用列表」，env.toastEl() 记「最终可见」，
  // 两者不等价正是 F8 要抓的东西。
  probe(`(() => { const orig = toast; toast = (m) => {
    __toasts.push(String(m));
    return orig(m);
  }; })()`);

  return {
    probe, calls, toasts, interval, inflight, due,
    el: (id) => document.getElementById(id),
    toastEl() { return document.getElementById('toast').textContent; },
    listHtml() { return document.getElementById('session-list').innerHTML; },
    noteTexts() { return document.getElementById('group-messages').children.map((c) => String(c.textContent)); },
    reminderRouteCount: () => calls.filter((c) => c.route.startsWith('/api/v1/reminders')).length,
    doneCalls: () => calls.filter((c) => c.method === 'POST' && /\/done$/.test(c.route)).map((c) => Number(/reminders\/(\d+)\//.exec(c.route)[1])),
    doneIds: () => calls
      .filter((c) => c.method === 'POST' && /\/done$/.test(c.route))
      .map((c) => Number(/reminders\/(\d+)\//.exec(c.route)[1]))
      .sort((a, b) => a - b),
    surfacedSize: () => probe('remindersSurfaced.size'),
    async tick() { await probe('reminderTick()'); },
  };
}

/* ---------- 每-tick 不变式（F8 的抓手）----------
 * 一个同步 tick 内：toast() 至多调用一次、至多一条被标 done；
 * 一旦有 done，那条的正文必须就是 tick 结束后 toast 元素上的可见文案。 */
function assertVisible(env, label, id) {
  const r = env.due.find((x) => x.id === id);
  const visible = String(env.toastEl());
  check(`${label}｜被标 done 的那条 == toast 元素最终可见文案`,
    !!r && visible.includes(String(r.content)), `visible=${JSON.stringify(visible)} doneId=${id}`);
  return visible;
}

async function stepTick(env, label) {
  const c0 = env.calls.length;
  const t0 = env.toasts.length;
  await env.tick();
  const doneNow = env.calls.slice(c0)
    .filter((c) => c.method === 'POST' && /\/done$/.test(c.route))
    .map((c) => Number(/reminders\/(\d+)\//.exec(c.route)[1]));
  const toastNow = env.toasts.length - t0;
  check(`${label}｜同 tick 至多一次 toast 调用`, toastNow <= 1,
    `本 tick 调用 ${toastNow} 次：${JSON.stringify(env.toasts.slice(t0))}`);
  check(`${label}｜同 tick 至多一条标 done`, doneNow.length <= 1, JSON.stringify(doneNow));
  if (doneNow.length === 1) assertVisible(env, label, doneNow[0]);
  return { doneNow, toastNow, visible: env.toastEl() };
}

/* ---------- A：一次会话只提示一次 + Bot 命名 ---------- */
async function caseA() {
  const env = makeEnv({ due: [{ id: 7, bot_id: 1, content: '喝水<&>', due_at: 'x' }, { id: 8, bot_id: 999, content: '开会', due_at: 'y' }] });
  await stepTick(env, 'A-1');
  await stepTick(env, 'A-2');
  check('A1 两条分两轮各提示一次', env.toasts.length === 2, JSON.stringify(env.toasts));
  check('A2 已知 Bot 带名字', env.toasts[0].startsWith('小O提醒你：喝水<&>'), JSON.stringify(env.toasts));
  check('A3 未知 bot_id 走通用文案', env.toasts[1] === '提醒你：开会', JSON.stringify(env.toasts));
  check('A4 toast 用 textContent 原样承载（未 HTML 化）', env.toastEl() === '提醒你：开会', JSON.stringify(env.toastEl()));
  const before = env.toasts.length;
  await env.tick();
  check('A5 同会话内同 id 不重复提示（本地已记账）', env.toasts.length === before, `${before}→${env.toasts.length}`);
  check('A6 done 请求按正确 id 发出', JSON.stringify(env.doneCalls().sort()) === JSON.stringify([7, 8]), JSON.stringify(env.doneCalls()));
  check('A7 轮询周期 15s（约定值，仅信息）', env.interval.period === null || env.interval.period === 15000, String(env.interval.period));
}

/* ---------- B：成功标记后不再出现在到期集合 ---------- */
async function caseB() {
  const env = makeEnv({ due: [{ id: 3, bot_id: 2, content: '买菜', due_at: 'z' }] });
  await stepTick(env, 'B-1');
  const t1 = env.toasts.length;
  await env.tick();
  await env.tick();
  check('B1 标记完成后服务端剔除，二次 tick 不再提示', env.toasts.length === t1 && t1 === 1, JSON.stringify(env.toasts));
  check('B2 恰一次 done(3)', JSON.stringify(env.doneCalls()) === JSON.stringify([3]), JSON.stringify(env.doneCalls()));
}

/* ---------- C：mark-done 失败不得循环刷屏 ---------- */
async function caseC() {
  const env = makeEnv({ due: [{ id: 5, bot_id: 1, content: '交周报', due_at: 'w' }], failDone: new Set([5]) });
  await stepTick(env, 'C-1');
  await env.tick();
  await env.tick();
  check('C1 done 失败下仍只提示一次（失败即本地记账，drop 到刷新）', env.toasts.length === 1, JSON.stringify(env.toasts));
  check('C2 服务端未落 done（仍到期）但被本地集合挡住', env.toasts.length === 1, `toasts=${env.toasts.length}`);
  check('C3 失败即 drop：本会话不再重复提示也不再重试标记（无死循环）', env.doneCalls().filter((x) => x === 5).length === 1, JSON.stringify(env.doneCalls()));
}

/* ---------- D：detached 主窗口守卫 ---------- */
async function caseD() {
  const env = makeEnv({ due: [{ id: 1, bot_id: 1, content: 'x', due_at: 'd' }] });
  env.probe(`App.state.detached = true`);
  env.probe(`startReminderPoll()`);
  check('D1 detached 时不建定时器', env.interval.started === 0, `started=${env.interval.started}`);
  check('D2 detached 时零提醒请求', env.reminderRouteCount() === 0, JSON.stringify(env.calls));
  env.probe(`App.state.detached = false`);
  env.probe(`startReminderPoll()`);
  check('D3 主窗口建定时器', env.interval.started === 1, `started=${env.interval.started}`);
  env.probe(`startReminderPoll()`);
  check('D4 重复调用不叠加定时器', env.interval.started === 1, `started=${env.interval.started}`);
}

/* ---------- E：重叠 tick 不得双飞 ---------- */
async function caseE() {
  const env = makeEnv({ due: [{ id: 9, bot_id: 1, content: '重叠', due_at: 'e' }], delayMs: 40 });
  const p1 = env.probe(`reminderTick()`);
  const p2 = env.probe(`reminderTick()`);
  const p3 = env.probe(`reminderTick()`);
  await Promise.all([p1, p2, p3]);
  check('E1 三个并发 tick 只允许一个 GET 在飞', env.inflight.maxGet === 1, `maxGet=${env.inflight.maxGet}`);
  check('E2 并发下同一提醒只提示一次', env.toasts.filter((t) => t.startsWith('小O提醒你：重叠')).length === 1, JSON.stringify(env.toasts));
  check('E3 并发下只标 done 一次', JSON.stringify(env.doneIds()) === JSON.stringify([9]), JSON.stringify(env.doneIds()));
}

/* ---------- F（F8 核心）：一轮 5 条到期 → 单 tick 单条可见 + 逐条消化 ---------- */
const fiveDue = () => ([
  { id: 21, bot_id: 1, content: '第五条到', due_at: '2026-01-01T00:05:00Z' },
  { id: 22, bot_id: 2, content: '第二条到', due_at: '2026-01-01T00:04:00Z' },
  { id: 23, bot_id: 1, content: '第三条到', due_at: '2026-01-01T00:01:00Z' },
  { id: 24, bot_id: 2, content: '第四条到', due_at: '2026-01-01T00:02:00Z' },
  { id: 25, bot_id: 1, content: '第一条到', due_at: '2026-01-01T00:03:00Z' },
]);

async function caseF() {
  const five = fiveDue();
  const env = makeEnv({ due: five });
  const step = await stepTick(env, 'F-1');
  check('F1 一轮 5 条到期只产生 1 条 toast 调用（同 tick 多次即违规）', env.toasts.length === 1, JSON.stringify(env.toasts));
  check('F2 唯一一次 toast 是最早一条（第三条到）的正文 + 待提醒计数',
    env.toasts[0] === reminderText(five[2], 4), JSON.stringify(env.toasts[0]));
  check('F3 tick 结束后 toast 元素可见文案 == 本轮被标 done 那条的文案',
    env.toastEl() === env.toasts[0] && JSON.stringify(step.doneNow) === JSON.stringify([23]),
    `visible=${JSON.stringify(env.toastEl())} done=${JSON.stringify(step.doneNow)}`);
  check('F4 本轮只标 done 1 条、只记账 1 条',
    JSON.stringify(env.doneIds()) === JSON.stringify([23]) && env.surfacedSize() === 1,
    `done=${JSON.stringify(env.doneIds())} surfaced=${env.surfacedSize()}`);
  check('F5 其余 4 条既不标 done 也不记账（排队到下一轮，无静默丢失）',
    env.due.filter((r) => r.__done).length === 1, JSON.stringify(env.due.map((r) => [r.id, !!r.__done])));
  const visible = [env.toastEl()];
  for (let i = 1; i < 5; i++) {
    const s = await stepTick(env, `F-消化${i}`);
    visible.push(s.visible);
  }
  const expect = [
    reminderText(five[2], 4), reminderText(five[3], 3), reminderText(five[4], 2),
    reminderText(five[1], 1), reminderText(five[0], 0),
  ];
  check('F6 连续 5 轮 5 条全部标 done', JSON.stringify(env.doneIds()) === JSON.stringify([21, 22, 23, 24, 25]), JSON.stringify(env.doneIds()));
  check('F7 连续 5 轮共 5 次 toast，每轮 done 那条的文案都曾是最终可见文案',
    env.toasts.length === 5 && JSON.stringify(visible) === JSON.stringify(expect), `可见序列=${JSON.stringify(visible)}`);
  const before = env.toasts.length;
  await env.tick();
  check('F8 第 6 轮无新到期不重复提示', env.toasts.length === before, `${before}→${env.toasts.length}`);
}

/* ---------- G：一轮 2 条 → 两轮各一条，第二轮无计数后缀 ---------- */
async function caseG() {
  const env = makeEnv({ due: [
    { id: 31, bot_id: 1, content: '甲', due_at: '2026-01-01T00:09:00Z' },
    { id: 32, bot_id: 2, content: '乙', due_at: '2026-01-01T00:08:00Z' },
  ] });
  await stepTick(env, 'G-1');
  check('G1 第一轮只提示最早那条，计数并进同一条文案',
    env.toasts.length === 1 && env.toastEl() === reminderText(env.due[1], 1), JSON.stringify(env.toasts));
  check('G2 第一轮只标 done 1 条，另一条排队',
    JSON.stringify(env.doneIds()) === JSON.stringify([32]) && env.surfacedSize() === 1,
    `done=${JSON.stringify(env.doneIds())} surfaced=${env.surfacedSize()}`);
  await stepTick(env, 'G-2');
  check('G3 第二轮提示剩下那条，无「另有 N 条待提醒」后缀', env.toastEl() === '小O提醒你：甲', JSON.stringify(env.toastEl()));
  check('G4 无「另有 N 条到期提醒」这类纯计数 toast', env.toasts.every((t) => !/^另有/.test(t)), JSON.stringify(env.toasts));
  check('G5 两条最终都标 done', JSON.stringify(env.doneIds()) === JSON.stringify([31, 32]), JSON.stringify(env.doneIds()));
}

/* ---------- H：排队不丢（每轮至多一条，5 轮内全部消化）---------- */
async function caseH() {
  const env = makeEnv({ due: fiveDue() });
  for (let i = 0; i < 4; i++) await stepTick(env, `H-轮${i + 1}`);
  check('H1 前 4 轮各只标 done 一条（不批量吞）',
    env.doneIds().length === 4 && env.surfacedSize() === 4, `done=${JSON.stringify(env.doneIds())} surfaced=${env.surfacedSize()}`);
  check('H2 未标 done 的条仍在服务端到期集合里（没被静默丢）',
    env.due.filter((r) => !r.__done).length === 1, JSON.stringify(env.doneIds()));
  await stepTick(env, 'H-轮5');
  check('H3 第 5 轮结束后服务端到期集合清空', env.due.every((r) => r.__done), JSON.stringify(env.due.map((r) => [r.id, !!r.__done])));
  check('H4 可见文案始终是提醒正文，未退化为纯计数条',
    env.toasts.length === 5 && env.toasts.every((t) => /提醒你：/.test(t) && !/^另有/.test(t)), JSON.stringify(env.toasts));
  check('H5 五条正文各见过一次', new Set(env.toasts.map((t) => t.slice(t.indexOf('提醒你：')))).size === 5, JSON.stringify(env.toasts));
}

/* ---------- I：呈现失败（元素写不进）→ 全部不标 done ----------
 * brokenToast 模拟「元素不可写」这一呈现失败分支；真实的后写覆盖前写由 F/G/H 的可见性断言覆盖。 */
async function caseI() {
  const env = makeEnv({ due: fiveDue().slice(0, 3), brokenToast: true });
  await stepTick(env, 'I-1');
  check('I1 呈现失败时一条都不标 done', env.doneIds().length === 0, JSON.stringify(env.doneIds()));
  check('I2 呈现失败时一条都不记账', env.surfacedSize() === 0, `size=${env.surfacedSize()}`);
  const env2 = makeEnv({ due: [{ id: 41, bot_id: 1, content: '零到期', due_at: '' }] });
  await stepTick(env2, 'I-2');
  check('I3 空 due_at 也按升序占位并正常标 done', JSON.stringify(env2.doneIds()) === JSON.stringify([41]), JSON.stringify(env2.doneIds()));
}

/* ---------- J（F9）：群状态回写 + 聊天列表徽标 ---------- */
async function caseJ() {
  const env = makeEnv({ due: [], groupsList: [{ id: 101, name: '测试群', status: 'idle', member_count: 2 },
                                              { id: 102, name: '另一个群', status: 'idle', member_count: 3 }] });
  env.probe(`App.state.groups = [
    { id: 101, name: '测试群', status: 'idle', member_count: 2 },
    { id: 102, name: '另一个群', status: 'idle', member_count: 3 }]`);
  env.probe(`App.state.currentGroupId = 101`);
  await env.probe(`renderChatList()`);
  await sleep(5);
  check('J0 初渲染列表数据源就位（无徽标）',
    env.probe(`App.state.groups.length`) === 2 && !/badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));

  env.probe(`handleGroupEvent('status', { state: 'running', turn: 1 })`);
  await sleep(5);
  check('J1 SSE status 事件回写列表数据源 groups[].status',
    env.probe(`App.state.groups[0].status`) === 'running', String(env.probe(`App.state.groups[0].status`)));
  check('J2 聊天列表重渲染并出现「进行中」徽标', /badge-running/.test(env.listHtml()), env.listHtml().slice(0, 200));
  check('J3 房间面板状态同步', env.probe(`App.state.groupStatus`) === 'running', String(env.probe(`App.state.groupStatus`)));
  check('J4 未选中的其它群状态不受影响', env.probe(`App.state.groups[1].status`) === 'idle', String(env.probe(`App.state.groups[1].status`)));

  await env.probe(`App.commands.get('group-pause')()`);
  await sleep(5);
  check('J5 group-pause 成功路径同样回写 + 重渲染',
    env.probe(`App.state.groups[0].status`) === 'paused'
      && env.probe(`App.state.groupStatus`) === 'paused'
      && !/badge-running/.test(env.listHtml()),
    `${String(env.probe(`App.state.groups[0].status`))}|${JSON.stringify(env.listHtml().slice(0, 120))}`);

  await env.probe(`App.commands.get('group-resume')()`);
  await sleep(5);
  check('J6 group-resume 恢复 running 后徽标再次出现',
    env.probe(`App.state.groups[0].status`) === 'running' && /badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));

  env.probe(`handleGroupEvent('status', { state: 'running', turn: 2 })`);
  await sleep(5);
  env.probe(`handleGroupEvent('status', { state: 'idle', turn: 2 })`);
  await sleep(5);
  check('J7 切回 idle 后徽标消失且数据源同步',
    env.probe(`App.state.groupStatus`) === 'idle' && env.probe(`App.state.groups[0].status`) === 'idle'
      && !/badge-running/.test(env.listHtml()),
    `${String(env.probe(`App.state.groups[0].status`))}|${JSON.stringify(env.listHtml().slice(0, 120))}`);
  check('J8 prev 语义保留：「群聊已停止」只出现一次（重复 idle 不补第二遍）',
    env.noteTexts().filter((t) => t === '群聊已停止').length === 1, JSON.stringify(env.noteTexts()));

  await env.probe(`App.commands.get('group-start')()`);
  await sleep(5);
  check('J9 group-start 成功路径回写 + 徽标出现',
    env.probe(`App.state.groups[0].status`) === 'running' && /badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));
  await env.probe(`App.commands.get('group-stop')()`);
  await sleep(5);
  check('J10 group-stop 成功路径回写 + 徽标消失',
    env.probe(`App.state.groups[0].status`) === 'idle' && !/badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));

  /* J11：后端重启复位 idle，页面未重载时陈旧徽标必须随重连消失（NEW-13 残余的修法） */
  env.probe(`App.state.groups[0].status = 'running'`);
  await env.probe(`renderChatList()`);
  check('J11a 负向基线：陈旧 running 确实画出徽标', /badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));
  const groupsGets = () => env.calls.filter((c) => c.method === 'GET' && c.route === '/api/v1/groups').length;
  const beforeGets = groupsGets();
  env.probe(`backendOnline(false)`);
  env.probe(`backendOnline(true)`);
  await sleep(5);
  check('J11b 断连→重连触发整表刷新', groupsGets() === beforeGets + 1, `${beforeGets}→${groupsGets()}`);
  check('J11c 陈旧徽标被服务端真值清除',
    env.probe(`App.state.groups[0].status`) === 'idle' && !/badge-running/.test(env.listHtml()), env.listHtml().slice(0, 120));
  const afterGets = groupsGets();
  env.probe(`backendOnline(true)`);
  env.probe(`backendOnline(true)`);
  await sleep(5);
  check('J11d 持续在线不重复刷新（无抖动）', groupsGets() === afterGets, `${afterGets}→${groupsGets()}`);
  env.probe(`App.state.detached = true`);
  env.probe(`backendOnline(false)`);
  env.probe(`backendOnline(true)`);
  await sleep(5);
  check('J11e detached 窗口不代主窗口刷新', groupsGets() === afterGets, `${afterGets}→${groupsGets()}`);
  env.probe(`App.state.detached = false`);
}

async function main() {
  console.log(`检查目标文件：${appPath}`);
  for (const [k, fn] of Object.entries({ A: caseA, B: caseB, C: caseC, D: caseD, E: caseE,
                                         F: caseF, G: caseG, H: caseH, I: caseI, J: caseJ })) {
    try { await fn(); } catch (e) { check(`${k} 执行`, false, e && e.message); }
  }
  const failed = results.filter((r) => !r.ok);
  console.log(`\n合计 ${results.length} 条断言，PASS ${results.length - failed.length}，FAIL ${failed.length}`);
  if (failed.length) { console.log(failed.map((f) => `  FAIL ${f.name}${f.detail ? ` → ${f.detail}` : ''}`).join('\n')); process.exitCode = 1; }
}
main();
