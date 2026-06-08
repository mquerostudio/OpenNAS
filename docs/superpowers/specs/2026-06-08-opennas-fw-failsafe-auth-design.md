# OpenNAS firmware — Failsafe + Auth (esclavo seguro)

> Diseño aprobado · 2026-06-08 · Endurece el firmware del ESP32-C5 para que sea seguro
> gobernarlo desde fuera (el "cerebro" de la curva de ventiladores vivirá en lab01/cockpit;
> ver el roadmap del homelab). El ESP queda como **esclavo**: expone recursos y acepta
> comandos de duty, pero se autoprotege si el cerebro se cae.

---

## 1. Contexto y arquitectura

El firmware (ESP-IDF, ESP32-C5) ya tiene una API HTTP funcional:
- `GET /api/status` → `{fans:[{id,duty,rpm}], hdds:[{id,active,last_ms,events}], uptime_s, …, ip}`
- `POST /api/fan/{0,1}` → `{duty:0..100}` (aplica + **persiste en NVS**)
- `POST /api/reset_wifi` → borra credenciales y reinicia a modo AP
- Provisioning WiFi (AP/STA), OTA, monitor de 6 HDD (pines ACT vía TCA9554), 2 ventiladores (PWM + tach).

**Decisión de arquitectura (sesión 2026-06-08):** la temperatura de los discos la lee lab01 (Proxmox
SMART), así que la **lógica de la curva temp→duty vive en el cockpit (lab01)**, no en el ESP. El ESP es
un **esclavo**: el cockpit lee `/api/status` y manda `POST /api/fan` con el duty calculado.

Este spec **no** construye el cerebro ni el display — solo prepara el esclavo para ser gobernado con
seguridad. Dos cosas faltan y un gotcha que corregir:

1. **Failsafe (watchdog):** si el cockpit calla, los ventiladores deben volver solos a un duty seguro.
2. **Auth:** los `POST` están abiertos (cualquiera en la red mueve ventiladores o borra el WiFi).
3. **Gotcha — desgaste de flash:** `fan_ctrl_set_duty` escribe en NVS en **cada** llamada. Con el cockpit
   mandando duty cada pocos segundos, eso machaca la flash. Hay que dejar de persistir por API.

---

## 2. Alcance

**Dentro (este spec, firmware en `3-Firmware/OpenNASFW`):**
1. **Failsafe**: watchdog que revierte ambos ventiladores a **100%** si pasan **60 s** sin comando.
2. **Sin desgaste de flash**: `set_duty` por API deja de persistir en NVS; el duty vive en RAM.
   Arranque a duty seguro (100%) hasta que el cockpit tome el mando.
3. **Auth por token** (`Authorization: Bearer <token>`) en los **POST** (`/api/fan/*`, `/api/reset_wifi`).
   `GET /api/status` queda abierto.
4. **Exponer estado del failsafe** en `/api/status` (para que el cockpit lo muestre luego).

**Fuera (otros specs / fases):**
- 🧠 Cerebro de la curva (cockpit, repo homelab): lee temp máx de Proxmox + estado del ESP, aplica curva,
  manda duty con heartbeat.
- 📊 Display en la pestaña NAS del cockpit (rpm/duty + actividad HDD + estado curva).
- Identidad estable (IP fija + DNS `opennas`), mover a red del homelab — diferido (ya es alcanzable hoy
  vía NAT de fw01: el cockpit `.30` llega a `192.168.1.153` en ~65 ms, verificado).

---

## 3. Componentes y cambios

### 3.1 `fan_control` — watchdog + sin NVS
- **Quitar persistencia por API:** `fan_ctrl_set_duty` ya **no** llama a `save_duty_to_nvs`. El duty es
  estado en RAM (`s_fans[i].duty_pct`). (La carga de NVS al boot se sustituye por el duty seguro, ver abajo.)
- **Estado del watchdog:** `last_cmd_us` (timestamp del último `set_duty` por API) + flag `in_failsafe`.
- **API nueva:**
  - `void fan_ctrl_note_command(void)` — refresca `last_cmd_us` (la llama el handler de `POST /api/fan`).
  - `bool fan_ctrl_in_failsafe(void)` + `uint32_t fan_ctrl_ms_since_cmd(void)` — para `/api/status`.
- **Tarea watchdog** (`fan_watchdog_task`, cada ~5 s): si `now - last_cmd_us > FAILSAFE_TIMEOUT_S` →
  aplicar `FAILSAFE_DUTY` a ambos ventiladores y marcar `in_failsafe=true`. Un `set_duty` posterior
  limpia el flag. **Arranque**: `in_failsafe=true`, ambos a `FAILSAFE_DUTY` (100%) hasta el primer comando.
- **Interfaz:** el control de PWM/tach no cambia; solo se añade el watchdog y se quita el NVS-on-set.

### 3.2 `Kconfig` del componente — parámetros
Nuevo `components/.../Kconfig.projbuild` (o por componente) con:
- `CONFIG_OPENNAS_FAILSAFE_DUTY` (int, default **100**)
- `CONFIG_OPENNAS_FAILSAFE_TIMEOUT_S` (int, default **60**)
- `CONFIG_OPENNAS_AUTH_TOKEN` (string, default **placeholder vacío/"changeme"** — **nunca** el token real;
  ver §6 secretos)

### 3.3 Auth — token en `http_server`
- Token efectivo: leído de NVS (`opennas` / `auth_token`); si NVS está vacío, se **siembra** desde
  `CONFIG_OPENNAS_AUTH_TOKEN` al arrancar. (Provisión: §6.)
- Helper `bool http_auth_ok(httpd_req_t *req)`: lee el header `Authorization`, espera `Bearer <token>`,
  compara en tiempo constante con el token efectivo. Si falta o no coincide → el handler responde
  **401** y no ejecuta la acción.
- Se aplica en `h_fan_post` y `h_reset_wifi_post`. `h_status_get` y `h_root_get` quedan abiertos.

### 3.4 `api_handlers` — wiring
- `h_fan_post`: primero `http_auth_ok` (401 si no), luego `fan_ctrl_note_command()` + `set_duty`.
- `h_reset_wifi_post`: `http_auth_ok` antes de borrar credenciales.
- `h_status_get`: añadir al JSON `,"failsafe":<bool>,"ms_since_cmd":<u32>` (de `fan_control`).

---

## 4. Flujo

```
[cockpit lab01]  --GET /api/status-->  [ESP] (lee fans+hdds+failsafe)
[cockpit]        --POST /api/fan/{id} {duty} + Bearer token-->  [ESP]
                                           └─ note_command() refresca watchdog, aplica duty (RAM)
   (si el cockpit calla > 60 s)
[ESP watchdog]   --> ambos ventiladores a 100%, in_failsafe=true
   (siguiente comando) --> sale de failsafe
```

## 5. Manejo de errores y casos límite
- **Cockpit nunca conecta / muere:** el ESP arranca y permanece en failsafe (100%) → discos siempre
  refrigerados. Nunca se quedan "fríos por error".
- **Token mal/ausente en POST:** 401, sin efecto, log sin filtrar el token.
- **Corte de red transitorio < 60 s:** no dispara failsafe (el siguiente latido refresca).
- **Reboot del ESP:** vuelve a failsafe hasta que el cockpit retome (idempotente).
- **NVS:** ya no se escribe por comando → sin desgaste. (NVS sigue para wifi creds y el token.)

## 6. Secretos
- **El token de auth es secreto.** `CONFIG_OPENNAS_AUTH_TOKEN` por defecto es un placeholder (`changeme`),
  **no** el real → así `sdkconfig` puede commitearse sin filtrar nada.
- El token **real** se inyecta en NVS del ESP (vía un `sdkconfig.local`/menuconfig no commiteado al
  compilar, o un comando de provisión de NVS). El **mismo** valor se guarda en `/opt/cockpit/.env`
  (`OPENNAS_TOKEN`) cuando se construya el cerebro. Nunca en Git.

## 7. Testing
- **Lógica host-testable:** el parser JSON ya existe; la comparación de token (constante) y el cálculo del
  watchdog (`ms_since_cmd > timeout`) son funciones puras → tests unitarios donde el harness lo permita.
- **Hardware/QEMU (pytest-embedded, `sdkconfig.ci`):** smoke de arranque + registro de endpoints.
- **Pruebas manuales (curl contra el ESP):**
  - `POST /api/fan/0 {duty:30}` sin token → **401**; con token → **204** y rpm baja.
  - Parar los comandos 60 s → `/api/status` muestra `failsafe:true` y rpm sube a ~máx.
  - Mandar un comando → `failsafe:false`.

## 8. Despliegue
- Build con ESP-IDF (`idf.py build`); flash inicial por USB, actualizaciones por **OTA** (el componente
  `ota_manager` ya existe) — no hace falta cable para iterar.
- Tras desplegar: provisionar el token en NVS y dejarlo también listo para el cockpit (`.env`).

## 9. Criterios de "hecho"
- [ ] `set_duty` por API no escribe en NVS (verificable en el código y por ausencia de desgaste).
- [ ] Sin comando 60 s → ambos ventiladores a 100% y `/api/status` `failsafe:true`.
- [ ] Un `POST /api/fan` válido saca del failsafe y aplica el duty.
- [ ] `POST` sin `Bearer <token>` correcto → 401; con token → ok.
- [ ] `GET /api/status` sigue abierto e incluye `failsafe` y `ms_since_cmd`.
- [ ] El token real no está en Git (sdkconfig con placeholder).
- [ ] Firmware desplegable por OTA.
