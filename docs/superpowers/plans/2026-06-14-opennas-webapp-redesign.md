# OpenNAS Webapp Redesign + reboot/set_wifi · Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans o
> superpowers:subagent-driven-development. Pasos con checkbox.
> Firmware ESP-IDF (C) + HTML embebido. Verificación: `idf.py build` + **curl/navegador contra el ESP**
> (no hay unit-tests del HTML embebido). El token real NO se escribe en código/commits.

**Goal:** Rediseñar el dashboard embebido del OpenNAS (paradigma B: monitor + cajón de ajustes), arreglar
la UX del token (un solo campo), y añadir endpoints `POST /api/reboot` y `POST /api/set_wifi` + `mac` en
`/api/status`.

**Architecture:** Solo se tocan `components/http_server/src/api_handlers.c` (3 cambios de C) y
`components/http_server/assets/dashboard.html` (reescritura). El resto del firmware (auth, watchdog,
net_manager, ota_manager…) no cambia.

**Tech Stack:** ESP-IDF v6.0.1 · C · esp_http_server · HTML/CSS/JS embebido (EMBED_TXTFILES).

**Build (patrón EIM):**
```bash
cd 3-Firmware/OpenNASFW
source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" >/dev/null 2>&1
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v6.0.1/venv"
idf.py build
```
**Deploy:** OTA — `curl -X POST --data-binary @build/OpenNASFW.bin -H "Authorization: Bearer <TOKEN>" http://192.168.1.153/api/ota`.
El token real está en `/opt/cockpit/.env` del LXC (`OPENNAS_TOKEN`) y en el gestor del usuario.

> ⚠️ Repo OpenNAS: NO stagear `1-CAD/` ni `.superpowers/`; commitear solo `3-Firmware/` y `docs/`.

**Referencia:** spec `docs/superpowers/specs/2026-06-14-opennas-webapp-redesign-design.md`.

---

## Task 1: Endpoints reboot + set_wifi + `mac` en status

**Files:** Modify `components/http_server/src/api_handlers.c`.

- [ ] **Step 1: Reintroducir el include de `esp_mac.h` y `esp_wifi.h` para la MAC**

En la lista de includes de `api_handlers.c`, añadir (orden alfabético entre los `esp_*`):
```c
#include "esp_mac.h"
#include "esp_wifi.h"
```
(`net_manager.h` ya está incluido; `json_get_str` y `http_auth_ok` ya existen en este fichero.)

- [ ] **Step 2: `mac` en `h_status_get`**

En `h_status_get`, justo después del bloque que añade `"ip"` (el `if (net_mgr_get_ip(&ip)...)`), añadir:
```c
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
```
> El buffer de status es 768B; estos ~25B caben (ya verificado el margen con failsafe).

- [ ] **Step 3: Handler `h_reboot_post`**

Añadir antes de `/* ---------------- POST /api/reset_wifi ---------------- */` (junto a set_token):
```c
/* ---------------- POST /api/reboot ----------------
 * Reinicia el ESP. Autenticado. */
static esp_err_t h_reboot_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok, rebooting\n");
    ESP_LOGW(TAG, "reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}
```

- [ ] **Step 4: Handler `h_set_wifi_post`**

Añadir junto al anterior:
```c
/* ---------------- POST /api/set_wifi ----------------
 * Body {"ssid":"..","pass":".."}. Guarda credenciales y reinicia para conectar
 * a la nueva red. Autenticado. pass puede ir vacío (red abierta). */
static esp_err_t h_set_wifi_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
    char body[256];
    int received = 0;
    while (received < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + received, sizeof(body) - 1 - received);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; break; }
        received += r;
    }
    body[received] = '\0';

    char ssid[33], pass[65];
    if (!json_get_str(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"ssid\"");
        return ESP_FAIL;
    }
    if (!json_get_str(body, "pass", pass, sizeof(pass))) pass[0] = '\0';

    esp_err_t err = net_mgr_save_creds(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok, reconnecting to new network\n");
    ESP_LOGW(TAG, "wifi creds updated via /api/set_wifi, rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}
```

- [ ] **Step 5: Registrar las dos rutas**

En `http_server_register_api`, junto a las `httpd_uri_t`, añadir:
```c
    static const httpd_uri_t u_reboot = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot_post,
    };
    static const httpd_uri_t u_set_wifi = {
        .uri = "/api/set_wifi", .method = HTTP_POST, .handler = h_set_wifi_post,
    };
```
Y sus registros junto a los demás `ESP_ERROR_CHECK(httpd_register_uri_handler(...))`:
```c
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_set_wifi));
```

- [ ] **Step 6: Añadir `esp_wifi` a las deps del componente**

En `components/http_server/CMakeLists.txt`, añadir `esp_wifi` y `esp_netif` a `PRIV_REQUIRES` si no están
(esp_netif ya se usa; esp_wifi es nuevo para `esp_wifi_get_mac`):
```
    PRIV_REQUIRES fan_control hdd_monitor net_manager esp_app_format esp_hw_support nvs_flash esp_wifi
```

- [ ] **Step 7: Build**

Run el patrón EIM + `idf.py build`. Expected: `Project build complete`, sin warnings nuevos.

- [ ] **Step 8: Commit**
```bash
git add 3-Firmware/OpenNASFW/components/http_server/src/api_handlers.c \
        3-Firmware/OpenNASFW/components/http_server/CMakeLists.txt
git commit -m "feat(fw): POST /api/reboot + /api/set_wifi + mac en /api/status"
```

---

## Task 2: Reescribir el dashboard (paradigma B)

**Files:** Overwrite `components/http_server/assets/dashboard.html`.

- [ ] **Step 1: Escribir el nuevo dashboard completo**

Reemplazar TODO el contenido de `components/http_server/assets/dashboard.html` por:

```html
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>OpenNAS</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#c9d1d9;--muted:#8b949e;
    --accent:#58a6ff;--green:#3fb950;--amber:#d29922;--red:#f85149;--blue:#1f6feb;}
  *{box-sizing:border-box}html,body{margin:0;padding:0}
  body{font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);
    color:var(--text);padding:1.25rem;max-width:920px;margin:0 auto}
  header{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);
    padding-bottom:.6rem;margin-bottom:.8rem;gap:1rem;flex-wrap:wrap}
  h1{margin:0;font-size:1.3rem;font-weight:600}
  .meta{color:var(--muted);font-family:ui-monospace,Consolas,monospace;font-size:.8rem;display:flex;
    gap:.8rem;flex-wrap:wrap;align-items:center}
  .meta .online{color:var(--green)}.meta .offline{color:var(--red)}
  h2{font-size:.74rem;color:var(--muted);text-transform:uppercase;letter-spacing:.1em;margin:1.1rem 0 .5rem;font-weight:600}
  .keybar{display:flex;gap:.5rem;align-items:center;margin-bottom:.8rem}
  .keybar input{flex:1;min-width:140px;font:.85rem ui-monospace,Consolas,monospace;color:var(--text);
    background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:.45rem .6rem}
  .keystate{font-size:.8rem;white-space:nowrap}.keystate.ok{color:var(--green)}.keystate.bad{color:var(--red)}
  .banner{background:rgba(248,81,73,.12);border:1px solid var(--red);color:var(--red);border-radius:8px;
    padding:.6rem .9rem;margin-bottom:.8rem;font-weight:600;font-size:.9rem}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:.7rem}
  .card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:.85rem 1rem}
  .fan .row1{display:flex;justify-content:space-between;align-items:baseline}
  .fan .title{font-weight:600}.fan .duty{font-family:ui-monospace,Consolas,monospace;color:var(--accent);font-weight:600}
  .fan input[type=range]{width:100%;accent-color:var(--accent);margin:.7rem 0 .4rem;display:block}
  .fan .rpm{font-family:ui-monospace,Consolas,monospace;font-size:1.5rem;font-weight:600;line-height:1}
  .fan .rpm u{color:var(--muted);font-size:.75rem;margin-left:.25rem;font-style:normal}
  .hdd{display:flex;align-items:center;gap:.8rem}
  .dot{width:13px;height:13px;border-radius:50%;background:var(--green);flex-shrink:0;transition:.15s}
  .dot.active{background:var(--amber);box-shadow:0 0 10px var(--amber)}
  .hdd .name{font-weight:600}.hdd .sub{color:var(--muted);font-size:.76rem;font-family:ui-monospace,Consolas,monospace}
  button{background:var(--card);border:1px solid var(--border);color:var(--text);padding:.45rem 1rem;
    border-radius:6px;cursor:pointer;font:inherit;transition:.15s}
  button:hover{background:#1c2128}
  .gear{background:none;border:none;font-size:1.2rem;padding:.2rem .4rem}
  .btn-accent{background:var(--blue);border:1px solid #388bfd;color:#fff;font-weight:600}
  .btn-accent:hover{background:#388bfd}.btn-accent:disabled{opacity:.5;cursor:not-allowed}
  .btn-danger{background:#3a1115;border:1px solid var(--red);color:var(--red);font-weight:600}
  /* drawer */
  #overlay{position:fixed;inset:0;background:rgba(0,0,0,.5);opacity:0;pointer-events:none;transition:.2s;z-index:5}
  #overlay.open{opacity:1;pointer-events:auto}
  #drawer{position:fixed;top:0;right:0;height:100%;width:min(380px,90vw);background:var(--bg);
    border-left:1px solid var(--border);transform:translateX(100%);transition:transform .2s;z-index:6;
    overflow-y:auto;padding:1.1rem}
  #drawer.open{transform:translateX(0)}
  #drawer .dh{display:flex;justify-content:space-between;align-items:center;margin-bottom:.5rem}
  #drawer h3{font-size:.95rem;margin:1.1rem 0 .5rem}
  .field{display:flex;flex-direction:column;gap:.4rem;margin-bottom:.5rem}
  .field input{font:.85rem ui-monospace,Consolas,monospace;color:var(--text);background:var(--card);
    border:1px solid var(--border);border-radius:6px;padding:.45rem .6rem}
  .row{display:flex;gap:.5rem;flex-wrap:wrap;align-items:center}
  .msg{font-size:.8rem;font-family:ui-monospace,Consolas,monospace;min-height:1em}
  .msg.ok{color:var(--green)}.msg.bad{color:var(--red)}
  .progress{height:6px;background:var(--border);border-radius:3px;overflow:hidden;width:100%;margin-top:.4rem}
  .progress>div{height:100%;background:var(--accent);width:0;transition:.15s}
  .sep{border:none;border-top:1px solid var(--border);margin:1rem 0}
  input[type=file]{font:inherit;color:var(--text);background:var(--card);border:1px solid var(--border);
    border-radius:6px;padding:.35rem .5rem;width:100%}
</style>
</head>
<body>
<header>
  <h1>OpenNAS</h1>
  <div class="meta">
    <span id="status">conectando…</span><span id="ip"></span><span id="version"></span>
    <span id="uptime"></span><span id="heap"></span>
    <span id="keyhdr" class="keystate"></span>
    <button class="gear" onclick="openDrawer()" title="Ajustes">⚙️</button>
  </div>
</header>

<div class="keybar">
  <input type="password" id="key" placeholder="Clave de control" autocomplete="off" oninput="saveKey()">
  <span id="keystate" class="keystate"></span>
</div>

<div id="banner" class="banner" hidden></div>

<h2>Ventiladores</h2>
<div id="fans" class="grid">Cargando…</div>

<h2>Discos duros</h2>
<div id="hdds" class="grid">Cargando…</div>

<!-- drawer -->
<div id="overlay" onclick="closeDrawer()"></div>
<aside id="drawer">
  <div class="dh"><b>Ajustes</b><button class="gear" onclick="closeDrawer()">✕</button></div>

  <h3>Dispositivo</h3>
  <button class="btn-danger" onclick="reboot()">⟳ Reiniciar ESP</button>

  <hr class="sep">
  <h3>Cambiar clave de control</h3>
  <div class="field"><input type="text" id="newKey" placeholder="Nueva clave"></div>
  <div class="row"><button class="btn-accent" onclick="changeKey()">Fijar clave</button></div>
  <div id="keyMsg" class="msg"></div>

  <hr class="sep">
  <h3>Cambiar WiFi</h3>
  <div class="field"><input type="text" id="ssid" placeholder="SSID (red del homelab)"></div>
  <div class="field"><input type="password" id="wpass" placeholder="Contraseña WiFi"></div>
  <div class="row"><button class="btn-accent" onclick="changeWifi()">Conectar a esa red</button></div>
  <div id="wifiMsg" class="msg"></div>

  <hr class="sep">
  <h3>Actualizar firmware (OTA)</h3>
  <input type="file" accept=".bin" id="otaFile">
  <div class="row" style="margin-top:.5rem"><button class="btn-accent" id="otaBtn" onclick="uploadFw()">Actualizar</button></div>
  <div class="progress" id="otaBar" hidden><div id="otaFill"></div></div>
  <div id="otaMsg" class="msg"></div>

  <hr class="sep">
  <h3>WiFi</h3>
  <button class="btn-danger" onclick="resetWifi()">Borrar credenciales WiFi</button>
</aside>

<script>
  const $=(id)=>document.getElementById(id);
  /* ---- clave (token Bearer) ---- */
  const KEY='opennas_token';
  const getKey=()=>localStorage.getItem(KEY)||'';
  function saveKey(){localStorage.setItem(KEY,$('key').value)}
  function authH(extra){const t=getKey();return Object.assign(t?{'Authorization':'Bearer '+t}:{},extra||{})}
  function setKeyState(ok){
    const t=$('keystate'),h=$('keyhdr');
    if(ok===null){t.textContent='';t.className='keystate';h.textContent='';h.className='keystate';return}
    t.textContent=ok?'✓ válida':'✗ clave incorrecta';t.className='keystate '+(ok?'ok':'bad');
    h.textContent=ok?'🔑✓':'🔑✗';h.className='keystate '+(ok?'ok':'bad');
  }
  async function apiPost(path,opts){
    try{const r=await fetch(path,opts);if(r.status===401)setKeyState(false);else if(r.ok)setKeyState(true);return r}
    catch(e){return null}
  }
  /* ---- drawer ---- */
  function openDrawer(){$('drawer').classList.add('open');$('overlay').classList.add('open')}
  function closeDrawer(){$('drawer').classList.remove('open');$('overlay').classList.remove('open')}
  /* ---- formatters ---- */
  const fmtUp=(s)=>{const d=s/86400|0,h=s/3600%24|0,m=s/60%60|0;return d?`${d}d ${h}h`:h?`${h}h ${m}m`:`${m}m`};
  const fmtAgo=(ms)=>ms<1000?`${ms}ms`:ms<60000?`${ms/1000|0}s`:ms<3600000?`${ms/60000|0}m`:`${ms/3600000|0}h`;
  const fmtB=(n)=>n<1024?`${n}B`:n<1048576?`${(n/1024).toFixed(1)}KB`:`${(n/1048576).toFixed(1)}MB`;
  /* ---- control ---- */
  async function setFan(id,duty){await apiPost(`/api/fan/${id}`,{method:'POST',headers:authH({'Content-Type':'application/json'}),body:JSON.stringify({duty})})}
  async function reboot(){
    if(!confirm('¿Reiniciar el ESP? Volverá en unos segundos (failsafe al 100%).'))return;
    const r=await apiPost('/api/reboot',{method:'POST',headers:authH()});
    if(r&&r.status===401){alert('Clave incorrecta');return}
    closeDrawer();$('status').textContent='reiniciando…';$('status').className='offline';
  }
  async function changeKey(){
    const t=$('newKey').value.trim(),m=$('keyMsg');
    if(!t){m.textContent='Escribe una clave';m.className='msg bad';return}
    m.textContent='Fijando…';m.className='msg';
    const r=await apiPost('/api/set_token',{method:'POST',headers:authH({'Content-Type':'application/json'}),body:JSON.stringify({token:t})});
    if(!r){m.textContent='Error de conexión';m.className='msg bad';return}
    if(r.status===401){m.textContent='La clave ACTUAL (arriba) es incorrecta';m.className='msg bad';return}
    if(!r.ok){m.textContent='Error '+r.status;m.className='msg bad';return}
    localStorage.setItem(KEY,t);$('key').value=t;$('newKey').value='';
    m.textContent='Clave fijada y guardada ✓';m.className='msg ok';setKeyState(true);
  }
  async function changeWifi(){
    const ssid=$('ssid').value.trim(),pass=$('wpass').value,m=$('wifiMsg');
    if(!ssid){m.textContent='Escribe el SSID';m.className='msg bad';return}
    if(!confirm(`El ESP se reconectará a "${ssid}" y reiniciará. Tendrás que abrir el dashboard en su nueva IP. ¿Continuar?`))return;
    m.textContent='Guardando y reiniciando…';m.className='msg';
    const r=await apiPost('/api/set_wifi',{method:'POST',headers:authH({'Content-Type':'application/json'}),body:JSON.stringify({ssid,pass})});
    if(!r){m.textContent='Enviado; el ESP está reiniciando. Búscalo en la red nueva.';m.className='msg ok';return}
    if(r.status===401){m.textContent='Clave de control incorrecta';m.className='msg bad';return}
    m.textContent='OK, reconectando a la red nueva. Abre el dashboard en su nueva IP.';m.className='msg ok';
  }
  async function resetWifi(){
    if(!confirm('Borra las credenciales WiFi y reinicia en modo AP. ¿Continuar?'))return;
    const r=await apiPost('/api/reset_wifi',{method:'POST',headers:authH()});
    if(r&&r.status===401){alert('Clave incorrecta');return}
    document.body.innerHTML='<p style="padding:2rem;text-align:center">Reiniciando en modo AP…</p>';
  }
  /* ---- OTA ---- */
  let pollTimer;
  function uploadFw(){
    const f=$('otaFile').files[0];if(!f){alert('Selecciona un .bin');return}
    const btn=$('otaBtn'),bar=$('otaBar'),fill=$('otaFill'),m=$('otaMsg');
    btn.disabled=true;bar.hidden=false;fill.style.width='0';fill.style.background='var(--accent)';
    m.textContent='Subiendo…';m.className='msg';
    const x=new XMLHttpRequest();
    x.upload.onprogress=(e)=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';m.textContent=`Subiendo… ${p}%`}};
    x.onload=()=>{
      if(x.status===200){fill.style.width='100%';fill.style.background='var(--green)';m.textContent='Aceptado. Reiniciando…';m.className='msg ok';
        clearInterval(pollTimer);setTimeout(waitReboot,3000);}
      else{fill.style.background='var(--red)';m.textContent=`Error ${x.status}: ${x.responseText}`;m.className='msg bad';btn.disabled=false;}
    };
    x.onerror=()=>{m.textContent='Error de conexión';m.className='msg bad';btn.disabled=false};
    x.open('POST','/api/ota');const t=getKey();if(t)x.setRequestHeader('Authorization','Bearer '+t);x.send(f);
  }
  async function waitReboot(){
    const m=$('otaMsg');
    for(let i=0;i<30;i++){await new Promise(r=>setTimeout(r,2000));
      try{const r=await fetch('/api/status');if(r.ok){m.textContent='Online con nuevo firmware ✓';m.className='msg ok';
        $('otaBtn').disabled=false;$('otaBar').hidden=true;pollTimer=setInterval(poll,1500);poll();return}}catch(e){}
      m.textContent=`Esperando reconexión… (${(i+1)*2}s)`;}
    m.textContent='Timeout. Recarga la página.';m.className='msg bad';$('otaBtn').disabled=false;
  }
  /* ---- render ---- */
  function renderFans(fans){
    const el=$('fans');
    if(el.children.length!==fans.length){
      el.innerHTML=fans.map(f=>`<div class="card fan"><div class="row1"><span class="title">FAN ${f.id}</span>
        <span class="duty"><span id="fd${f.id}">${f.duty}</span>%</span></div>
        <input type="range" min="0" max="100" value="${f.duty}" id="fs${f.id}"
          oninput="$('fd'+${f.id}).textContent=this.value" onchange="setFan(${f.id},+this.value)">
        <div class="rpm" id="fr${f.id}">${f.rpm}<u>RPM</u></div></div>`).join('');
    }else{fans.forEach(f=>{$('fr'+f.id).innerHTML=`${f.rpm}<u>RPM</u>`;const s=$('fs'+f.id);
      if(document.activeElement!==s){s.value=f.duty;$('fd'+f.id).textContent=f.duty}})}
  }
  function renderHdds(hdds){
    const el=$('hdds');
    if(el.children.length!==hdds.length){
      el.innerHTML=hdds.map(h=>`<div class="card hdd"><div class="dot" id="hd${h.id}"></div>
        <div><div class="name">HDD ${h.id}</div><div class="sub" id="hs${h.id}">—</div></div></div>`).join('');
    }
    hdds.forEach(h=>{$('hd'+h.id).classList.toggle('active',h.active);
      const st=h.active?'activity':(h.events?`idle · ${fmtAgo(h.last_ms)}`:'idle');
      $('hs'+h.id).textContent=`${st} · ${h.events} ev`})
  }
  async function poll(){
    try{
      const r=await fetch('/api/status');if(!r.ok)throw 0;const d=await r.json();
      $('status').textContent='online';$('status').className='online';
      $('ip').textContent=d.ip||'';$('version').textContent=d.version?`fw ${d.version.slice(0,14)}`:'';
      $('uptime').textContent=d.uptime_s!=null?`up ${fmtUp(d.uptime_s)}`:'';
      $('heap').textContent=d.free_heap!=null?`heap ${fmtB(d.free_heap)}`:'';
      renderFans(d.fans);renderHdds(d.hdds);
      const b=$('banner');
      if(d.failsafe){const s=d.ms_since_cmd>=4294967295?'sin comando desde el arranque':`último comando hace ${fmtAgo(d.ms_since_cmd)}`;
        b.textContent=`⚠ FAILSAFE — ventiladores al 100% · ${s}`;b.hidden=false;}else b.hidden=true;
    }catch(e){$('status').textContent='offline';$('status').className='offline';}
  }
  /* ---- init ---- */
  $('key').value=getKey();if(!getKey())setKeyState(null);
  poll();pollTimer=setInterval(poll,1500);
</script>
</body>
</html>
```

- [ ] **Step 2: Build (embebe el HTML nuevo)**

Run el patrón EIM + `idf.py build`. Expected: build OK.

- [ ] **Step 3: Commit**
```bash
git add 3-Firmware/OpenNASFW/components/http_server/assets/dashboard.html
git commit -m "feat(fw): dashboard rediseñado (B: monitor + cajón, clave única, reset/set-wifi)"
```

---

## Task 3: OTA + verificación en el ESP

**Files:** ninguno. Requiere el ESP (`192.168.1.153`) y el token (en `/opt/cockpit/.env`).

- [ ] **Step 1: OTA del binario nuevo**

```bash
TOK=$(ssh root@10.10.10.12 'pct exec 301 -- sh -c "grep ^OPENNAS_TOKEN /opt/cockpit/.env | cut -d= -f2"')
cd 3-Firmware/OpenNASFW
curl -s --max-time 120 -X POST --data-binary @build/OpenNASFW.bin \
  -H "Authorization: Bearer $TOK" http://192.168.1.153/api/ota -w "\nHTTP %{http_code}\n"
```
Expected: `HTTP 200`. Esperar el reboot (~10s) y la ventana de rollback (~30s) antes de seguir.

- [ ] **Step 2: Verificar endpoints**

```bash
ESP=http://192.168.1.153
curl -s -o /dev/null -w "reboot sin token: %{http_code}\n" -X POST $ESP/api/reboot   # 401
curl -s $ESP/api/status | python3 -c "import sys,json;d=json.load(sys.stdin);print('mac:',d.get('mac'));print('failsafe:',d['failsafe'])"
```
Expected: reboot sin token → **401**; status incluye `mac` y `failsafe`.

- [ ] **Step 3: Verificar el dashboard nuevo (python por el NUL del embed)**

```bash
curl -s http://192.168.1.153/ > /tmp/d.html
python3 -c "t=open('/tmp/d.html','rb').read().decode('utf-8','replace'); print({k:t.count(k) for k in ['Clave de control','Reiniciar ESP','Cambiar WiFi','id=\"drawer\"','set_wifi']})"
```
Expected: todos > 0.

- [ ] **Step 4: Verificación HUMANA**

Abrir `http://192.168.1.153/`, pegar la clave (✓ válida), mover un ventilador (baja rpm), abrir el cajón
⚙️, ver Reiniciar/Cambiar clave/Cambiar WiFi/OTA/Borrar WiFi. (NO pulsar Cambiar WiFi salvo que se quiera
re-homear ahora — eso es la tarea del repo homelab.)

- [ ] **Step 5: (sin commit — verificación)**

---

## Task 4: Push

- [ ] **Step 1: Revisar sin secretos + push**
```bash
cd /Users/mquero/Documents/MQuero/OpenNAS
git status -s | grep -vE "1-CAD|.superpowers|.cache" | grep -E "3-Firmware|docs" || echo "(solo lo nuestro)"
git push origin main
```

---

## Self-review (cobertura del spec)
- §2.1 dashboard B → Task 2 ✅ · §2.2 reboot → Task 1 (Step 3,5) ✅ · §2.3 botón reset → Task 2 (drawer) ✅ ·
  §2.4 set_wifi + UI → Task 1 (Step 4,5) + Task 2 ✅ · §2.5 mac → Task 1 (Step 2) ✅.
- §3 UI (clave única, banner, fans, hdds, cajón) → Task 2 (HTML) ✅ · §4 firmware → Task 1 ✅ ·
  §5 errores (401 en indicador) → Task 2 (apiPost/setKeyState) ✅ · §6 testing → Task 3 ✅.
- §8 re-homing: NO está aquí (es tarea del repo homelab, como dice el spec). El `set_wifi`+`mac` que lo
  habilitan, sí.
- Consistencia: `http_auth_ok`, `json_get_str`, `net_mgr_save_creds`, `set_token`, `KEY='opennas_token'`,
  rutas `/api/{reboot,set_wifi}` — coherentes con el firmware existente.
- Secretos: el token real solo se lee del `.env` en runtime; nunca en código/commits.
