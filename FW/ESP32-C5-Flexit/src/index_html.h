#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flexit Panel</title>
<style>
  :root { color-scheme: dark; }
  body { font: 16px system-ui, sans-serif; margin: 0; padding: 1.2rem;
         background: #15171c; color: #e6e6e6; max-width: 540px; }
  h1 { font-size: 1.2rem; margin: 0 0 1rem; }
  h2 { font-size: 1rem; margin: 1.5rem 0 0.6rem; color: #9ca3af;
       text-transform: uppercase; letter-spacing: 0.05em; }
  .row { display: grid; grid-template-columns: 1fr 1fr 1fr; align-items: center;
         gap: 0.6rem; padding: 0.6rem 0; border-bottom: 1px solid #2a2d33; }
  .name { font-weight: 600; }
  button { background: #2563eb; color: #fff; border: 0; padding: 0.7rem 1rem;
           border-radius: 0.5rem; font-size: 1rem; cursor: pointer; }
  button:active { background: #1e40af; }
  button:disabled { background: #444; cursor: wait; }
  button.danger { background: #b91c1c; }
  button.danger:active { background: #7f1d1d; }
  .led { display: inline-block; width: 1rem; height: 1rem; border-radius: 50%;
         background: #333; box-shadow: inset 0 0 4px #000; vertical-align: middle;
         transition: background 120ms, box-shadow 120ms; }
  .led.on { background: #f59e0b; box-shadow: 0 0 10px #f59e0b, inset 0 0 4px #000; }
  .led-label { margin-left: 0.5rem; vertical-align: middle; color: #aaa; }
  .status { font-size: 0.85rem; color: #888; margin-top: 1rem; }
  .ok { color: #4ade80; }
  .err { color: #f87171; }
  .net { background: #1f2229; border: 1px solid #2a2d33; border-radius: 0.5rem;
         padding: 0.8rem; margin-bottom: 1rem; font-size: 0.9rem; }
  .net .pill { display: inline-block; padding: 0.1rem 0.5rem; border-radius: 0.4rem;
               font-size: 0.75rem; font-weight: 700; margin-right: 0.4rem;
               text-transform: uppercase; letter-spacing: 0.05em; }
  .pill.sta { background: #166534; color: #d1fae5; }
  .pill.ap  { background: #92400e; color: #fef3c7; }
  .form { display: grid; gap: 0.5rem; margin-top: 0.6rem; }
  .form label { font-size: 0.8rem; color: #9ca3af; }
  .form input { background: #0f1115; color: #e6e6e6; border: 1px solid #2a2d33;
                border-radius: 0.4rem; padding: 0.55rem 0.7rem; font: inherit; }
  .form input:focus { outline: 2px solid #2563eb; border-color: transparent; }
  .form .actions { display: flex; gap: 0.5rem; margin-top: 0.3rem; }
  .msg { font-size: 0.85rem; margin-top: 0.4rem; min-height: 1.1em; }
</style>
</head>
<body>
<h1>Flexit Knappepanel</h1>

<div class="net" id="net">
  <span class="pill" id="netPill">...</span>
  <span id="netDesc">loading...</span>
</div>

<h2>Knapper</h2>
<div id="rows"></div>

<h2>Pulse duration</h2>
<div class="form">
  <label for="pulseMs">Length of the simulated button press (ms). Try 100&ndash;150 ms if a LED only blinks instead of latching.</label>
  <div class="actions" style="align-items:center">
    <input id="pulseMs" type="number" min="20" max="2000" step="10" style="width:7rem">
    <span style="color:#9ca3af">ms</span>
    <button id="pulseSaveBtn" style="margin-left:auto">Save</button>
  </div>
  <div class="msg" id="pulseMsg"></div>
</div>

<h2>MQTT</h2>
<div class="form">
  <div style="font-size:0.85rem">
    Status: <span id="mqttStatus" class="pill ap">unknown</span>
    <span id="mqttDetail" style="color:#9ca3af; margin-left:0.4rem"></span>
  </div>
  <label for="mqttHost">Server (leave blank to disable MQTT)</label>
  <input id="mqttHost" autocomplete="off" placeholder="e.g. 192.168.1.10 or mqtt.example.com" maxlength="64">
  <div style="display:grid; grid-template-columns: 1fr 6rem; gap:0.5rem">
    <div>
      <label for="mqttUser">Username (optional)</label>
      <input id="mqttUser" autocomplete="off" maxlength="64">
    </div>
    <div>
      <label for="mqttPort">Port</label>
      <input id="mqttPort" type="number" min="1" max="65535" value="1883">
    </div>
  </div>
  <label for="mqttPass">Password (optional)</label>
  <input id="mqttPass" type="password" autocomplete="new-password" maxlength="64">
  <label for="mqttBase">Base topic</label>
  <input id="mqttBase" autocomplete="off" placeholder="flexit" maxlength="32">
  <label for="mqttPubInterval">Heartbeat republish interval (s, 0 = on change only)</label>
  <input id="mqttPubInterval" type="number" min="0" max="3600" step="1" value="0" style="width:7rem">
  <div style="font-size:0.78rem; color:#9ca3af; line-height:1.4">
    Topics relative to base:<br>
    &nbsp;&nbsp;<code>&lt;base&gt;/led/1..5/state</code> &mdash; retained <code>ON</code>/<code>OFF</code><br>
    &nbsp;&nbsp;<code>&lt;base&gt;/sw/1..5/trigger</code> &mdash; any payload fires a pulse; numeric payload = ms override<br>
    &nbsp;&nbsp;<code>&lt;base&gt;/status</code> &mdash; LWT, retained <code>online</code>/<code>offline</code><br>
    Client ID: <code id="mqttClientId">&mdash;</code>
  </div>
  <div class="actions">
    <button id="mqttSaveBtn">Save</button>
  </div>
  <div class="msg" id="mqttMsg"></div>
</div>

<h2>Wi-Fi</h2>
<div class="form">
  <label for="ssid">SSID</label>
  <input id="ssid" autocomplete="off" placeholder="your network name" maxlength="32">
  <label for="pass">Password (leave blank for open network)</label>
  <input id="pass" type="password" autocomplete="new-password" placeholder="********" maxlength="64">
  <div class="actions">
    <button id="saveBtn">Save &amp; reboot</button>
    <button id="clearBtn" class="danger">Forget &amp; reboot to AP</button>
  </div>
  <div class="msg" id="wifiMsg"></div>
</div>

<div class="status">Connection: <span id="conn" class="err">offline</span></div>

<script>
const N = 5;
const rows = document.getElementById('rows');
for (let i = 1; i <= N; i++) {
  const row = document.createElement('div');
  row.className = 'row';
  row.innerHTML =
    `<div class="name">Knapp ${i}</div>
     <div><button data-sw="${i}">Trykk</button></div>
     <div><span class="led" id="led${i}"></span>
          <span class="led-label">Led ${i}</span></div>`;
  rows.appendChild(row);
}

const conn = document.getElementById('conn');
const netPill = document.getElementById('netPill');
const netDesc = document.getElementById('netDesc');
const ssidInput = document.getElementById('ssid');
const passInput = document.getElementById('pass');
const wifiMsg = document.getElementById('wifiMsg');
let online = false;
let ssidPrefilled = false;

function setOnline(v) {
  if (v === online) return;
  online = v;
  conn.textContent = v ? 'online' : 'offline';
  conn.className = v ? 'ok' : 'err';
}

document.querySelectorAll('button[data-sw]').forEach(btn => {
  btn.addEventListener('click', async () => {
    const i = btn.dataset.sw;
    btn.disabled = true;
    try {
      const r = await fetch(`/trigger?sw=${i}`, { method: 'POST' });
      if (!r.ok) throw new Error(r.status);
    } catch (e) { console.error(e); }
    setTimeout(() => btn.disabled = false, 350);
  });
});

document.getElementById('saveBtn').addEventListener('click', async () => {
  const ssid = ssidInput.value.trim();
  if (!ssid) { wifiMsg.textContent = 'SSID required'; wifiMsg.className = 'msg err'; return; }
  const pass = passInput.value;
  const body = new URLSearchParams({ ssid, pass }).toString();
  wifiMsg.textContent = 'Saving...'; wifiMsg.className = 'msg';
  try {
    const r = await fetch('/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    if (!r.ok) throw new Error(r.status);
    wifiMsg.textContent = 'Saved. Rebooting and trying to join "' + ssid + '"...';
    wifiMsg.className = 'msg ok';
  } catch (e) {
    wifiMsg.textContent = 'Failed: ' + e.message;
    wifiMsg.className = 'msg err';
  }
});

document.getElementById('clearBtn').addEventListener('click', async () => {
  if (!confirm('Forget saved Wi-Fi and reboot into AP mode?')) return;
  wifiMsg.textContent = 'Clearing...'; wifiMsg.className = 'msg';
  try {
    const r = await fetch('/wifi/clear', { method: 'POST' });
    if (!r.ok) throw new Error(r.status);
    wifiMsg.textContent = 'Cleared. Rebooting to AP "Flexit-Setup"...';
    wifiMsg.className = 'msg ok';
  } catch (e) {
    wifiMsg.textContent = 'Failed: ' + e.message;
    wifiMsg.className = 'msg err';
  }
});

async function refreshNet() {
  try {
    const r = await fetch('/netinfo', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const n = await r.json();
    netPill.className = 'pill ' + (n.mode === 'STA' ? 'sta' : 'ap');
    netPill.textContent = n.mode;
    if (n.mode === 'STA') {
      netDesc.textContent = `${n.ssid} · ${n.ip} · RSSI ${n.rssi} dBm`;
    } else {
      netDesc.textContent = `${n.ssid} · ${n.ip}`;
    }
    if (!ssidPrefilled && n.savedSsid && !ssidInput.value) {
      ssidInput.value = n.savedSsid;
      ssidPrefilled = true;
    }
  } catch {
    netPill.className = 'pill';
    netPill.textContent = '?';
    netDesc.textContent = 'unreachable';
  }
}

async function poll() {
  try {
    const r = await fetch('/status', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const s = await r.json();
    setOnline(true);
    (s.leds || []).forEach((on, idx) => {
      const el = document.getElementById('led' + (idx + 1));
      if (el) el.classList.toggle('on', !!on);
    });
  } catch {
    setOnline(false);
  }
}

const pulseInput = document.getElementById('pulseMs');
const pulseMsg = document.getElementById('pulseMsg');

async function loadConfig() {
  try {
    const r = await fetch('/config', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const c = await r.json();
    pulseInput.min = c.min;
    pulseInput.max = c.max;
    pulseInput.value = c.pulseMs;
  } catch (e) {
    pulseMsg.textContent = 'Could not load config: ' + e.message;
    pulseMsg.className = 'msg err';
  }
}

document.getElementById('pulseSaveBtn').addEventListener('click', async () => {
  const v = parseInt(pulseInput.value, 10);
  if (!Number.isFinite(v)) { pulseMsg.textContent = 'enter a number'; pulseMsg.className = 'msg err'; return; }
  pulseMsg.textContent = 'Saving...'; pulseMsg.className = 'msg';
  try {
    const r = await fetch('/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ pulseMs: String(v) }).toString()
    });
    if (!r.ok) throw new Error((await r.text()) || r.status);
    pulseMsg.textContent = 'Saved (' + v + ' ms).';
    pulseMsg.className = 'msg ok';
  } catch (e) {
    pulseMsg.textContent = 'Failed: ' + e.message;
    pulseMsg.className = 'msg err';
  }
});

const mqttHost = document.getElementById('mqttHost');
const mqttPort = document.getElementById('mqttPort');
const mqttUser = document.getElementById('mqttUser');
const mqttPass = document.getElementById('mqttPass');
const mqttBase = document.getElementById('mqttBase');
const mqttPubInterval = document.getElementById('mqttPubInterval');
const mqttClientIdEl = document.getElementById('mqttClientId');
const mqttMsg  = document.getElementById('mqttMsg');
const mqttStatus = document.getElementById('mqttStatus');
const mqttDetail = document.getElementById('mqttDetail');
let mqttPrefilled = false;

async function refreshMqtt() {
  try {
    const r = await fetch('/mqtt', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const m = await r.json();
    if (!mqttPrefilled) {
      mqttHost.value = m.host || '';
      mqttPort.value = m.port || 1883;
      mqttUser.value = m.user || '';
      mqttBase.value = m.base || 'flexit';
      mqttPubInterval.value = (m.pubIntervalSec != null) ? m.pubIntervalSec : 0;
      if (m.pubIntervalMax) mqttPubInterval.max = m.pubIntervalMax;
      mqttPrefilled = true;
    }
    if (m.clientId) mqttClientIdEl.textContent = m.clientId;
    if (!m.enabled) {
      mqttStatus.textContent = 'disabled';
      mqttStatus.className = 'pill';
      mqttDetail.textContent = '';
    } else if (m.connected) {
      mqttStatus.textContent = 'connected';
      mqttStatus.className = 'pill sta';
      mqttDetail.textContent = `${m.host}:${m.port}`;
    } else {
      mqttStatus.textContent = m.state || 'disconnected';
      mqttStatus.className = 'pill ap';
      mqttDetail.textContent = `${m.host}:${m.port}`;
    }
  } catch (e) {
    mqttStatus.textContent = '?';
    mqttStatus.className = 'pill';
    mqttDetail.textContent = '';
  }
}

document.getElementById('mqttSaveBtn').addEventListener('click', async () => {
  const body = new URLSearchParams({
    host: mqttHost.value.trim(),
    port: String(parseInt(mqttPort.value, 10) || 1883),
    user: mqttUser.value,
    pass: mqttPass.value,
    base: (mqttBase.value.trim() || 'flexit'),
    pubIntervalSec: String(parseInt(mqttPubInterval.value, 10) || 0),
  }).toString();
  mqttMsg.textContent = 'Saving...'; mqttMsg.className = 'msg';
  try {
    const r = await fetch('/mqtt', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    if (!r.ok) throw new Error((await r.text()) || r.status);
    mqttMsg.textContent = mqttHost.value.trim()
      ? 'Saved. Reconnecting to broker...'
      : 'Saved. MQTT disabled.';
    mqttMsg.className = 'msg ok';
    mqttPass.value = '';
    setTimeout(refreshMqtt, 500);
    setTimeout(refreshMqtt, 2000);
  } catch (e) {
    mqttMsg.textContent = 'Failed: ' + e.message;
    mqttMsg.className = 'msg err';
  }
});

loadConfig();
refreshNet();
setInterval(refreshNet, 4000);
refreshMqtt();
setInterval(refreshMqtt, 3000);
poll();
setInterval(poll, 400);
</script>
</body>
</html>)HTML";
