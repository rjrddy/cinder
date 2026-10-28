'use strict';
const $ = (id) => document.getElementById(id);
let selected = 0, state = null, busy = false;
const cells = [];
for (let i = 0; i < 32; i++) {
  const cell = document.createElement('button');
  cell.className = 'shard';
  cell.innerHTML = `<span class="id">${String(i).padStart(2, '0')}</span><strong>—</strong><small>waiting</small>`;
  cell.addEventListener('click', () => { selected = i; renderSelected(); });
  $('shards').append(cell); cells.push(cell);
}
function headers() {
  const h = { 'Content-Type': 'application/octet-stream' };
  if ($('token').value) h.Authorization = 'Bearer ' + $('token').value;
  return h;
}
async function request(path, body) {
  const response = await fetch(path, { method: body ? 'POST' : 'GET', headers: headers(), body });
  if (!response.ok) throw new Error(`${response.status}: ${await response.text()}`);
  return response.json();
}
function renderSelected() {
  if (!state) return;
  cells.forEach((cell, i) => cell.setAttribute('aria-pressed', String(i === selected)));
  const s = state.shards[selected];
  $('shard-title').textContent = String(selected).padStart(2, '0');
  $('shard-detail').replaceChildren();
  for (const [label, value] of [['Resident keys', s.keys], ['Payload bytes', s.payload_bytes], ['Payload quota', state.shard_quota], ['Hash buckets', s.buckets], ['Longest bucket chain', s.max_chain]]) {
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = label; dd.textContent = Number(value).toLocaleString();
    $('shard-detail').append(dt, dd);
  }
}
async function refresh() {
  if (busy) return;
  busy = true;
  try {
    state = await request('/api/state');
    $('connection').textContent = '● LIVE / 1s POLL'; $('connection').className = 'online';
    $('payload').textContent = state.payload_bytes.toLocaleString() + ' B';
    $('keys').textContent = state.resident_keys.toLocaleString();
    $('commands').textContent = `${state.commands.toLocaleString()} / ${state.errors.toLocaleString()}`;
    for (const [i, s] of state.shards.entries()) {
      const cell = cells[i]; cell.style.setProperty('--fill', Math.min(60, s.payload_bytes / state.shard_quota * 100));
      cell.classList.toggle('active', s.keys > 0);
      cell.querySelector('strong').textContent = s.keys.toLocaleString();
      cell.querySelector('small').textContent = `${s.payload_bytes.toLocaleString()} B`;
      cell.setAttribute('aria-label', `Shard ${i}, ${s.keys} resident keys, ${s.payload_bytes} payload bytes`);
    }
    renderSelected(); $('rows').replaceChildren();
    for (const r of state.sample) {
      const tr = document.createElement('tr');
      const ttl = r.expires_unix_ms ? Math.max(0, r.expires_unix_ms - Date.now()) + ' ms' : 'persistent';
      for (const value of [r.key, String(r.shard).padStart(2, '0'), r.preview, ttl]) {
        const td = document.createElement('td'); td.textContent = value; tr.append(td);
      }
      $('rows').append(tr);
    }
    if (!state.sample.length) { const tr = document.createElement('tr'), td = document.createElement('td'); td.colSpan = 4; td.textContent = 'The engine is empty. Write a key to begin.'; tr.append(td); $('rows').append(tr); }
  } catch (error) {
    $('connection').textContent = 'OFFLINE / ' + error.message.slice(0, 60); $('connection').className = '';
    $('response').textContent = error.message;
  } finally { busy = false; }
}
function encode(args) {
  const encoder = new TextEncoder(), chunks = [encoder.encode(`*${args.length}\r\n`)];
  for (const a of args) { const v = encoder.encode(a); chunks.push(encoder.encode(`$${v.length}\r\n`), v, encoder.encode('\r\n')); }
  const size = chunks.reduce((sum, c) => sum + c.length, 0), out = new Uint8Array(size); let p = 0;
  for (const chunk of chunks) { out.set(chunk, p); p += chunk.length; } return out;
}
async function command(args) {
  const data = await request('/api/command', encode(args)); $('response').textContent = data.response; return data.response;
}
$('command-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const op = $('operation').value, args = [op];
  if (op !== 'SAVE') args.push($('key').value);
  if (op === 'SET') { args.push($('value').value); if ($('ttl').value) args.push('PX', $('ttl').value); }
  try { await command(args); await refresh(); } catch (e) { $('response').textContent = e.message; }
});
$('connect').addEventListener('click', refresh);
$('seed').addEventListener('click', async () => {
  try { for (let i = 0; i < 64; i++) await command(['SET', `synthetic:${String(i).padStart(2, '0')}`, `payload-${i}`]); $('response').textContent = '64 synthetic keys written through the real RESP command engine.'; await refresh(); }
  catch (e) { $('response').textContent = e.message; }
});
$('expire').addEventListener('click', async () => { try { await command(['SET', 'ephemeral:ember', 'expires through the real engine', 'PX', '5000']); await refresh(); } catch (e) { $('response').textContent = e.message; } });
refresh(); setInterval(refresh, 1000);
function operationFields() {
  const op = $('operation').value;
  $('key').disabled = op === 'SAVE';
  $('value').disabled = op !== 'SET';
  $('ttl').disabled = op !== 'SET';
}
$('operation').addEventListener('change', operationFields);
operationFields();

/* Context motion: served inside the existing whitelisted asset. */
(()=>{
/** Finite, event-driven motion. No entrance timeline, polling, layout reads, or content delays. */
function installMotion(profile) {
    const preference = matchMedia('(prefers-reduced-motion: no-preference)');
    const running = new Set();
    const owned = new WeakMap();
    let observer;
    let events;
    let frame = 0;
    let interacted = false;
    const pending = new Map();
    const snapshots = new WeakMap();
    document.documentElement.dataset.motionProfile = profile.name;
    const play = (element, frames, duration) => {
        if (!preference.matches || !element.isConnected || document.hidden || typeof element.animate !== 'function')
            return;
        if (!(element instanceof HTMLElement) || element.matches('input,textarea,[contenteditable],.drag-handle,.note-body') || element.querySelector('input:focus,textarea:focus,[contenteditable]:focus'))
            return;
        // Stream updates never restart an in-flight effect; each owns at most one compositor animation.
        if (owned.get(element)?.playState === 'running')
            return;
        const animation = element.animate(frames, { duration, easing: 'cubic-bezier(0.16, 1, 0.3, 1)', iterations: 1, fill: 'none' });
        owned.set(element, animation);
        running.add(animation);
        const clear = () => { running.delete(animation); if (owned.get(element) === animation)
            owned.delete(element); };
        animation.onfinish = clear;
        animation.oncancel = clear;
    };
    const flush = () => {
        frame = 0;
        for (const [rule, groups] of pending)
            for (const group of groups) {
                const targets = rule.targets === ':self' ? [group] : Array.from(group.querySelectorAll(rule.targets)).slice(0, 6);
                const history = snapshots.get(group) || new Map();
                const previous = history.get(rule) || [];
                const current = [];
                for (const [index, target] of targets.entries()) {
                    const signature = (group.getAttribute('class') || '') + '|' + group.hasAttribute('open') + '|' + (target.textContent || '').slice(0, 2048) + '|' + target.getAttribute('class') + '|' + target.getAttribute('aria-pressed') + '|' + target.getAttribute('aria-current');
                    current.push(signature);
                    if (previous[index] !== signature) {
                        if (interacted)
                            play(target, rule.frames, rule.duration);
                    }
                }
                history.set(rule, current);
                snapshots.set(group, history);
            }
        pending.clear();
    };
    const enqueue = (rule, group) => {
        if (!group)
            return;
        if (!pending.has(rule))
            pending.set(rule, new Set());
        const groups = pending.get(rule);
        if (groups.size < 4)
            groups.add(group);
        if (!frame)
            frame = requestAnimationFrame(flush);
    };
    const stop = () => {
        observer?.disconnect();
        events?.abort();
        if (frame)
            cancelAnimationFrame(frame);
        frame = 0;
        pending.clear();
        for (const animation of running)
            animation.cancel();
        running.clear();
    };
    const start = () => {
        stop();
        if (!preference.matches)
            return;
        events = new AbortController();
        const signal = events.signal;
        const press = (event) => {
            if (event instanceof KeyboardEvent && !['Enter', ' '].includes(event.key))
                return;
            const source = event.target instanceof Element ? event.target : null;
            const control = source?.closest('button,summary,a.button');
            if (!control || control.matches(':disabled,[aria-disabled="true"],.drag-handle,.note-body'))
                return;
            interacted = true;
            play(control, profile.press, 160);
            if (control.matches(profile.neighbors)) {
                for (const sibling of Array.from(control.parentElement?.children || []).filter(x => x !== control && x.matches(profile.neighbors)).slice(0, 2)) {
                    play(sibling, [{ transform: 'translateX(0) scaleX(1)' }, { transform: 'translateX(2px) scaleX(0.985)', offset: 0.28 }, { transform: 'translateX(0) scaleX(1)' }], 190);
                }
            }
        };
        // Animate only once click targeting is committed; shrinking a pressed hitbox can otherwise lose edge clicks.
        document.addEventListener('click', press, { capture: true, signal });
        document.addEventListener('keydown', press, { capture: true, signal });
        for (const event of ['click', 'change'])
            document.addEventListener(event, () => { interacted = true; }, { capture: true, signal });
        observer = new MutationObserver(records => {
            for (const record of records) {
                const source = record.target instanceof Element ? record.target : record.target.parentElement;
                if (!source)
                    continue;
                for (const rule of profile.rules) {
                    enqueue(rule, source.closest(rule.watch));
                    for (const added of Array.from(record.addedNodes).slice(0, 6))
                        if (added instanceof Element) {
                            enqueue(rule, added.matches(rule.watch) ? added : added.querySelector(rule.watch));
                        }
                }
            }
        });
        observer.observe(document.body, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ['class', 'aria-current', 'aria-pressed', 'open'] });
    };
    preference.addEventListener('change', () => preference.matches ? start() : stop());
    window.addEventListener('pagehide', stop, { once: true });
    window.addEventListener('pageshow', event => { if (event.persisted && preference.matches)
        start(); });
    start();
}
installMotion({
    "name": "cinder",
    "press": [
        {
            "transform": "translateX(2px)"
        },
        {
            "transform": "translateX(-0.4px)",
            "offset": 0.65
        },
        {
            "transform": "translateX(0)"
        }
    ],
    "neighbors": "#shards .shard",
    "rules": [
        {
            "watch": "#shards",
            "targets": ".shard[aria-pressed=\"true\"] strong",
            "frames": [
                {
                    "transform": "translateX(-3px)"
                },
                {
                    "transform": "translateX(0.6px)",
                    "offset": 0.65
                },
                {
                    "transform": "translateX(0)"
                }
            ],
            "duration": 150
        },
        {
            "watch": "#shard-detail",
            "targets": "dd",
            "frames": [
                {
                    "transform": "translateX(4px)"
                },
                {
                    "transform": "translateX(0)"
                }
            ],
            "duration": 160
        },
        {
            "watch": "#response",
            "targets": ":self",
            "frames": [
                {
                    "transform": "translateX(3px)"
                },
                {
                    "transform": "translateX(-0.6px)",
                    "offset": 0.65
                },
                {
                    "transform": "translateX(0)"
                }
            ],
            "duration": 170
        }
    ]
});

})();
