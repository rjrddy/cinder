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
