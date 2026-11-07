#!/usr/bin/env node
/*
 * F12 三项「失败必须可见」的回归闸门（发布版实机验收失败换来的教训）
 *   A｜单聊生成失败：SSE 报 error 后 refresh-after-chat 会整表重载消息区，
 *     错误提示不能被子弹擦掉——重载完成后失败原因仍要在对话区里。
 *   B｜切走再切回同一会话：失败提示仍跟随该会话可见，且不得串到别的会话。
 *   C｜下一次生成成功：旧提示必须消失，不能变成永久疤。
 *   D｜max_tokens 越界（实机库里真存过 1935228928）：保存路径必须拦下，
 *     既不得把垃圾值写进库，也不得静默改成 2048。
 *   E｜模型名为空且设置里也没有默认模型：保存即提示（本次实机失败的第一因）；
 *     有默认模型或 Bot 自己填了模型必须放行——校验不能变成过度拦截。
 *   F｜群房间重建（reconcileGroupMessages）：后端落库的 system 失败留痕要渲染成
 *     居中提示，不能伪装成「用户」发的消息。
 *
 * 用法：
 *   node scripts/checks/check-failure-visibility.mjs                 # 跑当前仓库 app.js
 *   node scripts/checks/check-failure-visibility.mjs --app <path>     # 跑修复前副本，断言必须 FAIL
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
const API = 'http://stub';

const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  → ${detail}` : ''}`);
}

/* ---------------- DOM 桩 ----------------
 * class 选择器双向：innerHTML 里每个 class="…" 叶子解析成一个壳，壳被写入
 * （addBubble 的 showError/finalize、Typewriter 的 textContent 都走这里）会 splice 回
 * 父 innerHTML。只单向记录的话断言读到的是过期快照——那正是「假闭合」的温床。 */
class El {
  constructor(tag = 'div', id = '') {
    this.tagName = String(tag).toUpperCase();
    this.id = id;
    this.children = [];
    this.parentElement = null;
    this.dataset = {};
    this.style = {};
    this._html = '';
    this._shells = new Map();
    // input.value 在真实 DOM 里读出永远是字符串：赋数字会被强制转换（open-bot-editor 赋 b.max_tokens）
    let val = '';
    Object.defineProperty(this, 'value', {
      get: () => val,
      set: (v) => { val = String(v ?? ''); },
    });
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
      _set: set,
    };
    // app.js 全程用 el.className = '…' 赋类名，不是 classList.add。
    // 若 className 只是普通字段，_set 永远为空，「这条气泡渲染成什么」的断言会全瞎。
    Object.defineProperty(this, 'className', {
      get: () => [...set].join(' '),
      set: (v) => {
        set.clear();
        String(v ?? '').split(/\s+/).filter(Boolean).forEach((c) => set.add(c));
      },
    });
  }
  get innerHTML() { return this._html; }
  set innerHTML(v) {
    this._html = String(v ?? '');
    this.children = [];
    this._parse();
  }
  _pat(cls) {
    return new RegExp(`(<([a-zA-Z]+)[^>]*class="[^"]*\\b${cls}\\b[^"]*"[^>]*>)([\\s\\S]*?)(<\\/\\2>)`);
  }
  _splice(cls, inner) {
    this._html = this._html.replace(this._pat(cls), (_s, open, _tag, _body, close) => open + inner + close);
  }
  _parse() {
    this._shells = new Map();
    for (const m of this._html.matchAll(/class="([^"]+)"/g)) {
      for (const raw of String(m[1]).split(/\s+/)) {
        const cls = raw.trim();
        if (!cls || this._shells.has(cls)) continue;
        const kid = new El('div');
        kid.parentElement = this;
        cls.split(/\s+/).forEach((x) => kid.classList.add(x));
        const mm = this._pat(cls).exec(this._html);
        let inner = mm ? mm[3] : '';
        Object.defineProperty(kid, 'innerHTML', {
          get: () => inner,
          set: (v) => { inner = String(v ?? ''); this._splice(cls, inner); },
        });
        Object.defineProperty(kid, 'textContent', {
          get: () => inner.replace(/<[^>]*>/g, ''),
          set: (v) => { inner = String(v ?? ''); this._splice(cls, inner); },
        });
        this._shells.set(cls, kid);
      }
    }
  }
  appendChild(c) { this.children.push(c); c.parentElement = this; return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; }
  remove() { this.parentElement?.removeChild(this); }
  insertAdjacentHTML(_pos, html) { this._html += String(html ?? ''); this._parse(); }
  querySelector(sel) {
    const m = /^\.([\w-]+)$/.exec(sel);
    if (m) return this._shells.get(m[1]) || null;
    return null;
  }
  querySelectorAll() { return []; }
  addEventListener() {}
  setAttribute(k, v) { this[k] = v; }
  getAttribute(k) { return this[k] ?? null; }
  getBoundingClientRect() { return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
  focus() {} blur() {} scrollIntoView() {} select() {} closest() { return null; }
}

/* ---------------- 假后端 ----------------
 * opts: { sessions, bots, messages, groups, groupDetail, groupMessages, config,
 *         streams:[[SSE 帧]…] —— 按第 N 次生成取，越界用最后一组 } */
function makeEnv(opts = {}) {
  const store = {};
  for (const [k, v] of Object.entries(opts.messages || {})) store[k] = v.slice();
  const streams = opts.streams || [[]];
  const groups = opts.groups || [];
  const calls = [];
  const toasts = [];
  const registry = new Map();
  const document = {
    documentElement: new El('html'),
    body: new El('body'),
    getElementById(id) {
      if (!registry.has(id)) registry.set(id, new El('div', id));
      return registry.get(id);
    },
    createElement: (tag) => new El(tag),
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener() {},
  };

  const sse = (frames) => ({
    ok: true,
    status: 200,
    body: {
      getReader() {
        const text = frames.join('');
        let sent = false;
        return {
          async read() {
            if (sent) return { done: true };
            sent = true;
            return { done: false, value: Buffer.from(text, 'utf8') };
          },
        };
      },
    },
  });
  const reply = (data) => ({ ok: true, status: 200, json: async () => ({ code: 0, data }) });
  const chatRoute = (r) => r === '/api/v1/chat/stream' || r === '/api/v1/chat/regenerate';
  let msgSeq = 1000;

  const sandbox = {
    console,
    document,
    navigator: { clipboard: { writeText: async () => {} } },
    location: { search: '' },
    URLSearchParams: class { constructor() {} get() { return null; } },
    TextDecoder,
    AbortController,
    fetch: async (rawUrl, o = {}) => {
      const method = (o.method || 'GET').toUpperCase();
      const route = String(rawUrl).replace(API, '');
      const body = o.body ? JSON.parse(o.body) : {};
      calls.push({ method, route, body });
      if (method === 'POST' && chatRoute(route)) {
        const n = calls.filter((c) => c.method === 'POST' && chatRoute(c.route)).length - 1;
        const frames = streams[Math.min(n, streams.length - 1)] || [];
        const sid = body.session_id;
        if (route === '/api/v1/chat/stream' && body.content)
          (store[sid] ||= []).push({ id: ++msgSeq, role: 'user', content: body.content });
        // 真实后端失败时也发 done（隔离库实测：event: error 后紧跟 event: done），
        // 但不会落库任何助手回复——只有 delta 才产生回复行。
        const text = frames.filter((f) => f.includes('event: delta'))
          .map((f) => /"content":"([^"]*)"/.exec(f)?.[1] || '').join('');
        if (text) (store[sid] ||= []).push({ id: ++msgSeq, role: 'assistant', content: text });
        return sse(frames);
      }
      if (method === 'GET' && route === '/api/v1/config')
        return reply(opts.config || { base_url: '', api_key: '', model: '', theme: 'dark' });
      if (method === 'GET' && route === '/api/v1/sessions')
        return reply(opts.sessions || []);
      if (method === 'GET' && route.startsWith('/api/v1/messages?')) {
        const sid = Number(/session_id=(\d+)/.exec(route)[1]);
        const limit = Number(/limit=(\d+)/.exec(route)?.[1] || 100);
        return reply((store[sid] || []).slice(-limit).reverse());
      }
      if (method === 'GET' && route === '/api/v1/bots')
        return reply({ bots: opts.bots || [], default_bot_id: opts.bots?.[0]?.id || 0 });
      if (method === 'GET' && route === '/api/v1/groups')
        return reply(groups.map((g) => ({ ...g })));
      const gd = /^\/api\/v1\/groups\/(\d+)$/.exec(route);
      if (gd && method === 'GET')
        return reply(opts.groupDetail || groups.find((g) => g.id === Number(gd[1])) || null);
      if (/^\/api\/v1\/groups\/\d+\/messages/.test(route) && method === 'GET')
        return reply((opts.groupMessages || []).slice().reverse());
      if (method === 'POST' && route === '/api/v1/bots') return reply({ id: 9, ...body });
      if (/^\/api\/v1\/bots\/\d+$/.test(route) && method === 'PATCH')
        return reply({ id: Number(/\/(\d+)$/.exec(route)[1]), ...body });
      return reply({});
    },
    setInterval: (fn, ms) => { const t = setInterval(fn, ms); t.unref?.(); return t; },
    clearInterval,
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); t.unref?.(); return t; },
    clearTimeout,
    __toasts: toasts,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.__API_BASE__ = API;

  const ctx = vm.createContext(sandbox);
  vm.runInContext(appSrc, ctx);
  const probe = (expr) => vm.runInContext(expr, ctx);
  probe(`(() => { const orig = toast; toast = (m) => { __toasts.push(String(m)); return orig(m); }; })()`);

  const vis = (id) => document.getElementById(id).children.map((c) => ({
    cls: [...(c.classList._set || [])],
    html: String(c.innerHTML ?? ''),
    text: String(c.textContent ?? ''),
  }));
  return {
    probe, calls, toasts,
    el: (id) => document.getElementById(id),
    run(cmd, ...args) {
      return probe(`App.run(${JSON.stringify(cmd)}, ${args.map((a) => JSON.stringify(a)).join(',')})`);
    },
    toastText: () => String(document.getElementById('toast').textContent),
    // 用户真正看得见的东西：每条气泡的完整 markup + 纯文本
    chatContents: () => vis('messages'),
    groupContents: () => vis('group-messages'),
  };
}

const FAIL_TEXT = '上游服务返回 503（无可用渠道）';
// 与真实后端一致：error 之后仍会发 done（隔离库实测）。只发 error 的假流会漏掉
// 「onDone 把刚记下的失败清掉」这条真实缺陷——闸门必须按服务端真实帧序构造。
const errFrame = (msg) => `event: error\ndata: ${JSON.stringify({ message: msg })}\n\nevent: done\ndata: {}\n\n`;
const okFrames = (text) => [`event: delta\ndata: ${JSON.stringify({ content: text })}\n\n`, 'event: done\ndata: {}\n\n'];
const shows = (items, needle) => items.some((c) => c.html.includes(needle) || c.text.includes(needle));

const CHAT_BASE = {
  sessions: [{ id: 1, title: '糯米', bot_id: 1 }],
  bots: [{ id: 1, name: '糯米', avatar: '🤖' }],
  messages: { 1: [] },
};

/* ---------- A：失败提示不得被重载擦掉（① 的可证伪断言） ---------- */
async function caseA() {
  const env = makeEnv({ ...CHAT_BASE, streams: [[errFrame(FAIL_TEXT)]] });
  env.probe('App.state.currentSessionId = 1');
  env.el('input').value = '在吗';
  await env.run('send-message');
  await sleep(60);   // refresh-after-chat 是 fire-and-forget，等整区重载跑完
  const items = env.chatContents();
  check('A｜单聊失败：重载完成后对话区仍能看到失败原因', shows(items, '503'),
    `chat=${JSON.stringify(items)}`);
  check('A｜重载不能把用户消息一起擦掉', shows(items, '在吗'), `chat=${JSON.stringify(items)}`);
}

/* ---------- B：切走再切回仍在，且不串会话 ---------- */
async function caseB() {
  const env = makeEnv({
    sessions: [{ id: 1, title: '糯米', bot_id: 1 }, { id: 2, title: '别的', bot_id: 1 }],
    bots: [{ id: 1, name: '糯米', avatar: '🤖' }],
    messages: { 1: [], 2: [{ id: 21, role: 'assistant', content: '你好呀' }] },
    streams: [[errFrame(FAIL_TEXT)]],
  });
  env.probe('App.state.currentSessionId = 1');
  env.el('input').value = '在吗';
  await env.run('send-message');
  await sleep(60);
  await env.run('select-session', 2);
  await sleep(30);
  check('B｜失败提示不得串到别的会话', !shows(env.chatContents(), '503'),
    `chat=${JSON.stringify(env.chatContents())}`);
  await env.run('select-session', 1);
  await sleep(30);
  check('B｜切回原会话仍能看到为什么没回复', shows(env.chatContents(), '503'),
    `chat=${JSON.stringify(env.chatContents())}`);
}

/* ---------- C：成功一轮要抹掉旧疤 ---------- */
async function caseC() {
  const env = makeEnv({ ...CHAT_BASE, streams: [[errFrame(FAIL_TEXT)], okFrames('在的')] });
  env.probe('App.state.currentSessionId = 1');
  env.el('input').value = '在吗';
  await env.run('send-message');
  await sleep(60);
  check('C｜前置：失败先要可见', shows(env.chatContents(), '503'));
  env.el('input').value = '再说一次';
  await env.run('send-message');
  await sleep(60);
  const items = env.chatContents();
  check('C｜成功一轮之后不得再显示旧失败', !shows(items, '503'), `chat=${JSON.stringify(items)}`);
  check('C｜成功一轮的回复要显示出来', shows(items, '在的'), `chat=${JSON.stringify(items)}`);
}

/* ---------- D：max_tokens 越界不得入库 ---------- */
async function caseD() {
  const mk = (v) => {
    const env = makeEnv({ bots: [{ id: 1, name: '糯米', avatar: '🤖' }] });
    env.el('be-name').value = '糯米';
    env.el('be-prompt').value = 'x';
    env.el('be-max-tokens').value = v;
    env.el('be-api-model').value = 'gemini-3.8-flash';
    return env;
  };
  const posted = (env) => env.calls.filter((c) => c.method === 'POST' && c.route === '/api/v1/bots');

  const huge = mk('1935228928');
  await huge.run('save-bot');
  await sleep(20);
  check('D｜max_tokens=1935228928 必须被拦下，不得提交', posted(huge).length === 0,
    `calls=${JSON.stringify(posted(huge))} toast=${JSON.stringify(huge.toastText())}`);
  check('D｜拦下要说清允许范围', /1.*65536|之间/.test(huge.toastText()),
    `toast=${JSON.stringify(huge.toastText())}`);

  const zero = mk('0');
  await zero.run('save-bot');
  await sleep(20);
  check('D｜max_tokens=0 同样拦下（不得静默换成 2048）', posted(zero).length === 0,
    `calls=${JSON.stringify(posted(zero))}`);

  const good = mk('4096');
  await good.run('save-bot');
  await sleep(20);
  check('D｜合法值照常提交，且提交用户填的数',
    posted(good).length === 1 && posted(good)[0].body.max_tokens === 4096,
    `calls=${JSON.stringify(posted(good))}`);

  const blank = mk('');
  await blank.run('save-bot');
  await sleep(20);
  check('D｜留空仍走 2048 默认（历史行为不变）',
    posted(blank).length === 1 && posted(blank)[0].body.max_tokens === 2048,
    `calls=${JSON.stringify(posted(blank))}`);
}

/* ---------- E：模型名双空必须提示 ---------- */
async function caseE() {
  const mk = (model, cfgModel) => {
    const env = makeEnv({
      bots: [{ id: 1, name: '糯米', avatar: '🤖' }],
      config: { base_url: 'https://relay.example', api_key: 'sk-****', model: cfgModel, theme: 'dark' },
    });
    // boot 做的事就是 App.state.config = await get('/api/v1/config')；这里等价赋值，
    // 让 save-bot 的「设置里没有默认模型」判断读到 cfgModel。
    env.probe(`App.state.config = ${JSON.stringify({ base_url: 'https://relay.example', api_key: 'sk-****', model: cfgModel, theme: 'dark' })}`);
    env.el('be-name').value = '糯米';
    env.el('be-prompt').value = 'x';
    env.el('be-max-tokens').value = '2048';
    env.el('be-api-model').value = model;
    return env;
  };
  const writes = (env) => env.calls.filter((c) => c.method === 'POST' || c.method === 'PATCH');

  const both = mk('', '');
  await both.run('save-bot');
  await sleep(20);
  check('E｜Bot 模型与全局默认都为空 → 不得提交', writes(both).length === 0,
    `calls=${JSON.stringify(writes(both))}`);
  check('E｜提示要指出去哪儿填模型', /模型/.test(both.toastText()) && /设置/.test(both.toastText()),
    `toast=${JSON.stringify(both.toastText())}`);

  const hasDefault = mk('', 'gemini-3.8-flash');
  await hasDefault.run('save-bot');
  await sleep(20);
  check('E｜全局默认存在时必须放行（不得过度拦截）',
    writes(hasDefault).filter((c) => c.method === 'POST').length === 1,
    `calls=${JSON.stringify(writes(hasDefault))}`);

  const explicit = mk('gemini-3.8-flash', '');
  await explicit.run('save-bot');
  await sleep(20);
  check('E｜Bot 自己填了模型也要放行',
    writes(explicit).filter((c) => c.method === 'POST').length === 1,
    `calls=${JSON.stringify(writes(explicit))}`);
}

/* ---------- F：群聊失败留痕渲染成提示，不伪装成发言 ---------- */
async function caseF() {
  const env = makeEnv({
    groups: [{ id: 7, name: '测试群', status: 'paused', topic: '' }],
    groupDetail: { id: 7, name: '测试群', status: 'paused', topic: '', members: [] },
    groupMessages: [
      { id: 1, bot_id: 1, name: '糯米', avatar: '🤖', role: 'assistant', content: '你好' },
      { id: 2, bot_id: 0, name: '用户', avatar: '🙋', role: 'system', content: `⚠ 糯米 回复失败：${FAIL_TEXT}` },
      { id: 3, bot_id: 0, name: '用户', avatar: '🙋', role: 'system', content: '⚠ 糯米 连续失败 3 次，群聊已自动暂停' },
    ],
  });
  env.probe('App.state.currentGroupId = 7');
  await env.probe('reconcileGroupMessages()');
  await sleep(20);
  const items = env.groupContents();
  const note = items.filter((c) => c.cls.includes('sysnote') && (c.text + c.html).includes('503'));
  const faked = items.filter((c) => c.cls.includes('gmsg') && (c.html + c.text).includes('回复失败'));
  check('F｜留痕渲染成群内提示，重开房间就知道为什么停了', note.length >= 1,
    `items=${JSON.stringify(items)}`);
  check('F｜留痕不得伪装成用户/机器人发的消息', faked.length === 0,
    `items=${JSON.stringify(items)}`);
  check('F｜正常发言照常渲染', items.some((c) => c.cls.includes('gmsg') && c.html.includes('你好')),
    `items=${JSON.stringify(items)}`);
}

const cases = { A: caseA, B: caseB, C: caseC, D: caseD, E: caseE, F: caseF };

async function main() {
  for (const [name, fn] of Object.entries(cases)) {
    if (only && name !== only) continue;
    try {
      await fn();
    } catch (e) {
      check(`${name}｜用例执行`, false, String((e && e.stack) || e));
    }
  }
  const failed = results.filter((r) => !r.ok);
  console.log(`\n${results.length - failed.length}/${results.length} 通过`);
  if (failed.length) process.exitCode = 1;
}
main();
