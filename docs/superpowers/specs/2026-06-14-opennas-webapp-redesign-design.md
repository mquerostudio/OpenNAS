# OpenNAS — Rediseño de la webapp + endpoint de reboot

> Diseño aprobado · 2026-06-14 · Rediseña el dashboard embebido del firmware (que creció a
> parches) con jerarquía clara, arregla la UX confusa del token, y añade un botón de reinicio.
> El **firmware core no se toca** (está sólido: failsafe, auth unificada, OTA+rollback, "SAFE TO
> FLASH"); solo se añade un endpoint y se reescribe el HTML embebido.

---

## 1. Contexto y filosofía

Tras añadir la auth a parches, la webapp embebida (`components/http_server/assets/dashboard.html`)
quedó incoherente: dos campos de token confusos, sin jerarquía (mezcla estado, control y setup), y
faltaba lo obvio (reiniciar). La sensación de "base débil" es **la webapp y la UX**, no el firmware.

**Decisiones de la sesión de brainstorming:**
- **El ESP es un esclavo "máquina-primero":** el cockpit (lab01) lo gobierna el 99% del tiempo. La
  webapp es para **setup + diagnóstico + control directo de respaldo** (cuando el cockpit cae).
- **Auth = un token de API (Bearer).** Descartado login usuario+contraseña/sesiones: sirve sobre todo
  a la máquina (el cockpit), y un login obligaría al cockpit a gestionar sesiones (YAGNI + complejidad
  en el ESP). El humano mete el token una vez; el navegador lo recuerda.
- **Paradigma de UI = B (dashboard + cajón):** una vista principal con lo que se mira siempre, y un
  cajón lateral (engranaje) con ajustes/updates a demanda.

---

## 2. Alcance

**Dentro:**
1. Reescribir `dashboard.html` con el paradigma B (estructura + UX del token + banner failsafe).
2. Nuevo endpoint **`POST /api/reboot`** (autenticado) → `esp_restart()`.
3. Botón "Reiniciar ESP" en el cajón.
4. Nuevo endpoint **`POST /api/set_wifi`** (auth) `{ssid,pass}` → `net_mgr_save_creds` + reboot, y UI
   **"Cambiar WiFi"** en el cajón → re-homear el ESP a la WiFi del homelab (la de `ap01`) sin el baile
   del AP/portal.
5. Exponer **`mac`** en `/api/status` (para poder hacer la reserva DHCP en fw01).

> Tarea **separada** (repo homelab, NO en este spec) que esto habilita: reserva DHCP en fw01
> (MAC→`opennas`=`10.10.10.50`) + DNS `opennas.lab.mquero.com` en Unbound + actualizar `OPENNAS_URL`
> del cockpit + alta en `docs/ip-plan.md` (rango `.50–.99` dispositivos fijos). Ver §8.

**Fuera (YAGNI / ya sólido):**
- Login usuario+contraseña, sesiones/cookies.
- Refactor de componentes del firmware (board, fan_control, hdd_monitor, net_manager, provisioning,
  ota_manager) — están bien.
- Versionado de API / contrato formal. La API actual (status/fan/reset_wifi/ota/set_token) se mantiene;
  los errores ya son consistentes (`httpd_resp_send_err`).

---

## 3. Diseño de la webapp (paradigma B)

### Vista principal (siempre visible — "lo que miras")
- **Header:** `OpenNAS` · estado `online/offline` · ip · `fw <ver>` · uptime · heap · indicador 🔑 de
  clave (válida/incorrecta) · icono ⚙️ que abre el cajón.
- **Clave de control:** un único campo compacto (tipo password) bajo el header. Se guarda en
  `localStorage` y se manda como `Authorization: Bearer` en todo POST. Muestra ✓/✗ y, ante un 401,
  "clave incorrecta". (Sustituye a los dos campos confusos actuales.)
- **Banner FAILSAFE:** visible solo cuando `status.failsafe` es true ("⚠ ventiladores al 100% · sin
  comando hace Xs"). Con `ms_since_cmd == UINT32_MAX` → "desde el arranque".
- **Ventiladores:** una tarjeta por ventilador con duty + rpm + slider de override (0–100%). El slider
  manda `POST /api/fan/{id}`. Texto sutil: "el cockpit retoma el mando".
- **Discos:** rejilla de los 6 HDD con punto de actividad (verde idle / ámbar activo) + `events`/edad.

### Cajón lateral (⚙️, deslizable — "lo que tocas a veces")
Encabezado "Ajustes". Contiene, en orden:
1. **⟳ Reiniciar ESP** — con `confirm()` → `POST /api/reboot`.
2. **Cambiar clave de control** — campo "nueva clave" + botón → `POST /api/set_token`; al éxito,
   guarda la nueva en `localStorage` y actualiza el campo de clave principal.
3. **Cambiar WiFi** — campos SSID + clave + botón → `POST /api/set_wifi`; al éxito el ESP reinicia y se
   conecta a la nueva red (aviso: "se reconectará a otra red; abre el dashboard en la IP nueva"). Es la
   vía limpia para moverlo a la WiFi del homelab.
4. **Actualizar firmware (OTA)** — selector de `.bin` + barra de progreso → `POST /api/ota` (con
   header Bearer); espera el reboot y reconecta.
5. **Borrar credenciales WiFi** — `confirm()` → `POST /api/reset_wifi` (vuelve a modo AP).

El cajón se abre/cierra con el engranaje (overlay translúcido en móvil; panel lateral en desktop). Por
defecto **cerrado** (la vista por defecto es el monitor).

### Comportamiento de auth en el cliente
- Helper único `authHeaders()` que añade `Authorization: Bearer <clave>` si hay clave.
- Wrapper de POST que, ante **401**, marca la clave como incorrecta en el indicador del header (un solo
  sitio, no por-acción).
- Los controles (sliders, botones) no se deshabilitan, pero si no hay clave válida los POST dan 401 y
  el indicador lo refleja.

---

## 4. Cambios en el firmware

### `components/http_server/src/api_handlers.c`
- Nuevo handler `h_reboot_post`: `http_auth_ok` → 401 si no; si ok, responde texto breve y
  `esp_restart()` tras un `vTaskDelay` corto (como `h_reset_wifi_post`).
- Nuevo handler `h_set_wifi_post`: auth → `json_get_str` de `ssid` y `pass` → `net_mgr_save_creds(ssid,
  pass)` → responde texto breve → `esp_restart()` (arranca en STA con las nuevas creds). Requiere
  `PRIV_REQUIRES net_manager` (ya está) y el `json_get_str` que ya existe.
- `h_status_get`: añadir `"mac":"aa:bb:.."` al JSON (vía `esp_read_mac`/`esp_wifi_get_mac`; reintroduce
  el include necesario). Para la reserva DHCP en fw01.
- Registrar `POST /api/reboot` y `POST /api/set_wifi` en `http_server_register_api`.
- Reescribir el asset embebido (`assets/dashboard.html`, embebido por `EMBED_TXTFILES`).

No cambia la auth, el watchdog, ni ningún otro componente. `max_uri_handlers` (16) tiene sito de sobra
(quedarían ~8 usados).

### El asset
`components/http_server/assets/dashboard.html` se reescribe entero (estructura B). Sigue siendo un único
fichero embebido — es una sola página y el patrón actual (EMBED_TXTFILES) es adecuado; el problema no
era el single-file sino la organización.

---

## 5. Manejo de errores / casos límite
- **Sin clave / clave incorrecta:** los POST devuelven 401; el indicador del header lo muestra; la vista
  de monitor (GET /api/status, abierta) sigue funcionando.
- **Reboot:** el navegador detecta la caída (el polling falla) y reconecta cuando vuelve (failsafe al
  100%, normal).
- **OTA:** ya gestionado (barra + espera de reboot); ahora con header Bearer.
- **Cambiar clave:** la petición se autentica con la clave ACTUAL; al éxito, el cliente adopta la nueva.
- **Móvil:** el cajón es un overlay; la rejilla se apila.

## 6. Testing / despliegue
- **Build** local con ESP-IDF v6.0.1 (activador EIM + `IDF_PYTHON_ENV_PATH`).
- **Deploy** por OTA al ESP (`192.168.1.153`) con el token.
- **Verificación por curl:** `POST /api/reboot` sin token → 401, con token → reinicia (y `/api/status`
  cae y vuelve); GET `/` sirve el HTML nuevo (markers del rediseño presentes — verificar con `python`
  por el NUL final del embed, no `grep`).
- **Verificación humana:** abrir el dashboard, meter la clave (✓), mover un ventilador, abrir el cajón,
  probar reiniciar.

## 7. Criterios de "hecho"
- [ ] `dashboard.html` reescrito (paradigma B): header con clave única + ✓/✗, banner failsafe, fans con
  slider, HDDs, y cajón ⚙️ con reiniciar/cambiar-clave/OTA/borrar-WiFi.
- [ ] Un solo campo de clave; los dos campos confusos actuales ya no existen.
- [ ] `POST /api/reboot` (auth) funciona: 401 sin token, reinicia con token.
- [ ] Todos los POST mandan `Bearer`; un 401 se refleja en el indicador del header.
- [ ] `POST /api/set_wifi` (auth) guarda creds y reinicia; "Cambiar WiFi" en el cajón funciona.
- [ ] `/api/status` incluye `mac`.
- [ ] Build verde; OTA aplicado; verificado por curl + a ojo en el navegador.
- [ ] Firmware core sin cambios (solo api_handlers + el asset).

---

## 8. Re-homing a la red del homelab (tarea SEPARADA — repo homelab)

Habilitado por `set_wifi`, pero es trabajo de red/infra, no de este spec. Su propio mini-ciclo:

1. **Mover el ESP a la WiFi del homelab:** desde el dashboard → cajón → "Cambiar WiFi" → SSID/clave de
   `ap01` (la WiFi del homelab). El ESP reinicia y pide DHCP a fw01 → IP dinámica en `10.10.10.x`.
2. **Reserva DHCP en fw01:** con la `mac` (de `/api/status`), reservar `opennas` → **`10.10.10.50`**.
3. **DNS:** alta en Unbound `opennas.lab.mquero.com` → `10.10.10.50` (vía API de fw01).
4. **ip-plan:** alta de `opennas` (`.50`, rango `.50–.99` dispositivos fijos) en `docs/ip-plan.md`.
5. **Cockpit:** actualizar `OPENNAS_URL` en `/opt/cockpit/.env` (de `192.168.1.153` →
   `http://opennas.lab.mquero.com` o `10.10.10.50`).

> Consideración: hoy el ESP está en `192.168.1.x` y el cockpit llega por NAT de fw01. Tras el re-homing
> estará en la LAN del homelab directamente (más limpio, con DNS, y encaja con la futura VLAN IoT).
