// SPDX-License-Identifier: MIT
#pragma once

#include <Arduino.h>

const char WEB_UI[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>LeafFilter Light Test</title>
  <style>
    :root{color-scheme:dark;font-family:system-ui,sans-serif;background:#101411;color:#edf4ee}
    body{max-width:900px;margin:auto;padding:18px}h1{margin-bottom:4px}h2{font-size:1.1rem}
    .muted{color:#9eaaa0}.card{background:#19201b;border:1px solid #344039;border-radius:12px;padding:16px;margin:14px 0}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
    label{display:block;font-size:.85rem;color:#b9c5bb}input,select,button{box-sizing:border-box;width:100%;padding:10px;margin-top:4px;border-radius:8px;border:1px solid #465349;background:#101411;color:#edf4ee}
    input[type=color]{height:44px;padding:3px}input[type=checkbox]{width:auto}.enabled{display:flex;gap:8px;align-items:center;padding-top:22px}
    button{cursor:pointer;background:#247a42;border-color:#329f59;font-weight:650}.danger{background:#7d2525;border-color:#b13b3b}.preset{background:#29342c;border-color:#465349}
    .status{padding:10px;border-radius:8px;background:#101411;white-space:pre-wrap}.zone{border-top:1px solid #344039;padding-top:12px;margin-top:12px}
  </style>
</head>
<body>
  <h1>LeafFilter Light Test</h1>
  <div class="muted">UCS1903 · 400 kHz · two physical pixels per fixture</div>

  <section class="card">
    <h2>Live test</h2>
    <div class="grid">
      <label>Zone<select id="testZone"></select></label>
      <label>Color<input id="color" type="color" value="#100000"></label>
      <label>Brightness<input id="brightness" type="range" min="1" max="255" value="32"></label>
    </div>
    <div class="grid" style="margin-top:10px">
      <button onclick="fireworks()">🎆 Fast Fireworks—all zones</button>
      <button onclick="sendColor()">Apply color</button>
      <button class="preset" onclick="preset(16,0,0)">Dim red</button>
      <button class="preset" onclick="preset(0,16,0)">Dim green</button>
      <button class="preset" onclick="preset(0,0,16)">Dim blue</button>
      <button class="danger" onclick="allOff()">All off</button>
    </div>
    <div id="message" class="status" style="margin-top:10px">Loading…</div>
  </section>

  <section class="card">
    <h2>Zone configuration</h2>
    <div class="muted">Counts are logical LeafFilter/Oelo fixtures. Firmware sends two UCS1903 pixels per fixture. Saving reboots the controller.</div>
    <form id="zones"></form>
    <button onclick="saveZones(event)">Save zones and reboot</button>
  </section>

  <section class="card">
    <h2>Home Wi-Fi</h2>
    <div class="muted">The open OELO_1-23.0 setup AP remains active for LeafFilter app compatibility.</div>
    <form id="network" class="grid">
      <label>Wi-Fi name<input name="ssid" id="ssid" autocomplete="username"></label>
      <label>Password<input name="password" type="password" autocomplete="current-password"></label>
    </form>
    <button onclick="saveNetwork(event)">Save Wi-Fi and reboot</button>
  </section>

<script>
let state;
const orders=['RGB','RBG','GRB','GBR','BRG','BGR'];
const msg=t=>document.getElementById('message').textContent=t;
async function load(){
  state=await (await fetch('/api/status')).json();
  document.getElementById('brightness').value=state.brightness;
  document.getElementById('ssid').value=state.wifi.ssid||'';
  const sel=document.getElementById('testZone'); sel.innerHTML='';
  const form=document.getElementById('zones'); form.innerHTML='';
  state.zones.forEach((z,i)=>{
    sel.insertAdjacentHTML('beforeend',`<option value="${i}">${i+1}: ${z.name}</option>`);
    form.insertAdjacentHTML('beforeend',`<div class="zone grid">
      <label class="enabled"><input type="checkbox" name="en${i}" ${z.enabled?'checked':''}> Enabled</label>
      <label>Name<input name="name${i}" maxlength="24" value="${escapeHtml(z.name)}"></label>
      <label>Fixture count<input name="cnt${i}" type="number" min="1" max="1000" value="${z.count}"></label>
      <label>Color order<select name="ord${i}">${orders.map(o=>`<option ${o===z.order?'selected':''}>${o}</option>`).join('')}</select></label>
    </div>`);
  });
  msg(`AP: ${state.wifi.apSsid} · ${state.wifi.apIp}\nLAN: ${state.wifi.connected?state.wifi.lanIp:'not connected'} · leaflights.local`);
}
function escapeHtml(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
async function preset(r,g,b){await setColor(r,g,b)}
async function sendColor(){const h=document.getElementById('color').value; await setColor(parseInt(h.slice(1,3),16),parseInt(h.slice(3,5),16),parseInt(h.slice(5,7),16));}
async function setColor(r,g,b){
  const q=new URLSearchParams({zone:document.getElementById('testZone').value,r,g,b,brightness:document.getElementById('brightness').value});
  msg(await (await fetch('/api/color?'+q)).text());
}
async function allOff(){msg(await (await fetch('/api/off')).text())}
async function fireworks(){msg(await (await fetch('/api/preset/fast-fireworks')).text())}
async function saveZones(e){e.preventDefault();const p=new URLSearchParams(new FormData(document.getElementById('zones')));msg(await (await fetch('/api/zones',{method:'POST',body:p})).text())}
async function saveNetwork(e){e.preventDefault();const p=new URLSearchParams(new FormData(document.getElementById('network')));msg(await (await fetch('/api/network',{method:'POST',body:p})).text())}
load().catch(e=>msg('Error: '+e));
</script>
</body>
</html>
)HTML";
