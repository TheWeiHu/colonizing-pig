'use strict';
(() => {
  const config = window.PIG_REPLAY_CONFIG || {};
  const enabled = location.origin === config.origin && /^https:\/\//.test(config.endpoint || '') && /^[a-f0-9]{64}$/.test(config.engine || '') && /^[a-f0-9]{64}$/.test(config.model || '');
  const prefix = 'pig-replay-pending-v1:', sentPrefix = 'pig-replay-sent-v1:';
  const memory = new Map(), sent = new Set();
  let busy = false, timer;
  const now = () => Date.now();

  function entries() {
    const records = new Map(memory);
    try {
      for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (!key?.startsWith(prefix)) continue;
        try {
          const entry = JSON.parse(localStorage.getItem(key));
          if (typeof entry.body === 'string' && entry.body.length <= 65536 && Number.isFinite(entry.nextAt)) records.set(key, entry);
        } catch {}
      }
    } catch {}
    return [...records].sort((a,b) => a[1].createdAt - b[1].createdAt);
  }
  function save(key, entry) {
    memory.set(key, entry);
    try { localStorage.setItem(key, JSON.stringify(entry)); } catch {}
  }
  function remove(key) {
    memory.delete(key);
    try { localStorage.removeItem(key); } catch {}
  }
  async function digest(body) {
    const bytes = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(body));
    return [...new Uint8Array(bytes)].map(n => n.toString(16).padStart(2, '0')).join('');
  }
  function wasSent(id) {
    if (sent.has(id)) return true;
    try { return localStorage.getItem(sentPrefix + id) !== null; } catch { return false; }
  }
  function markSent(id) {
    sent.add(id);
    try {
      localStorage.setItem(sentPrefix + id, String(now()));
      const keys = [];
      for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (key?.startsWith(sentPrefix)) keys.push([key, Number(localStorage.getItem(key))]);
      }
      keys.sort((a,b) => b[1] - a[1]);
      for (const [key] of keys.slice(256)) localStorage.removeItem(key);
    } catch {}
  }
  function schedule() {
    clearTimeout(timer);
    const pending = entries();
    if (pending.length) timer = setTimeout(flush, Math.max(1000, Math.min(...pending.map(([,e]) => e.nextAt)) - now()));
  }
  async function drain() {
    if (busy || !enabled || navigator.onLine === false) return;
    busy = true;
    try {
      for (const [key, entry] of entries()) {
        if (entry.nextAt > now()) continue;
        let id;
        try {
          id = await digest(entry.body);
          if (wasSent(id)) { remove(key); continue; }
          const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), 15000);
          let response;
          try {
            response = await fetch(config.endpoint, {method:'POST', headers:{'Content-Type':'application/json'}, body:entry.body, credentials:'omit', keepalive:true, signal:controller.signal});
          } finally { clearTimeout(timeout); }
          if (response.ok) { markSent(id); remove(key); continue; }
          if ([400, 413, 422].includes(response.status)) { remove(key); continue; }
          const retryAfter = Number(response.headers.get('Retry-After'));
          if (retryAfter > 0) entry.nextAt = now() + Math.min(retryAfter * 1000, 86400000);
        } catch {}
        entry.attempt = Math.min((entry.attempt || 0) + 1, 12);
        entry.nextAt = Math.max(entry.nextAt, now() + Math.min(5000 * 2 ** (entry.attempt - 1), 3600000));
        save(key, entry);
      }
    } finally { busy = false; schedule(); }
  }
  function flush() {
    if (!enabled || busy || navigator.onLine === false) return;
    if (navigator.locks) {
      navigator.locks.request('pig-replay-upload', {ifAvailable:true}, lock => {
        if (lock) return drain();
        clearTimeout(timer); timer = setTimeout(flush, 1000);
      }).catch(() => drain());
    } else void drain();
  }
  function enqueue(state, resigned) {
    if (!enabled || !state?.done) return;
    const payload = {version:1, engine:config.engine, model:config.model, seed:state.seed, human:state.human, moves:[...state.history], result:resigned ? 'resigned' : 'completed', winner:state.winner};
    const body = JSON.stringify(payload), pending = entries();
    if (body.length > 65536 || pending.some(([,e]) => e.body === body)) { flush(); return; }
    if (pending.length >= 32) return;
    save(prefix + crypto.randomUUID(), {body, attempt:0, createdAt:now(), nextAt:0});
    flush();
  }
  window.pigReplayUploads = {enqueue, flush, enabled};
  if (!enabled) return;
  document.querySelectorAll('.replay-notice').forEach(node => { node.hidden = false; });
  addEventListener('online', flush);
  addEventListener('storage', event => { if (event.key?.startsWith(prefix)) flush(); });
  addEventListener('pageshow', flush);
  flush();
})();
