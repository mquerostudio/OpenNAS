# OpenNAS Firmware (ESP32-C5)

Firmware del controlador de la PCB OpenNAS: controla **2 ventiladores** (PWM + tacómetro), lee la
**actividad de 6 HDD** (pines ACT vía un expansor I2C TCA9554) y expone todo por una **API HTTP**.

## Arquitectura: el ESP es un esclavo

La **lógica de la curva de ventiladores NO vive aquí**. La temperatura de los discos se lee en el
servidor (lab01/cockpit, vía la API de Proxmox SMART), así que el **cerebro** —decidir el duty de los
ventiladores según la temperatura— vive en el cockpit. El ESP32 es un **esclavo**:

- **Expone recursos** (`GET /api/status`): rpm/duty de ventiladores + actividad de cada HDD.
- **Acepta comandos** (`POST /api/fan`): el cerebro le dice a qué duty poner cada ventilador.
- **Se autoprotege** (failsafe): si el cerebro calla, sube los ventiladores al 100% solo.

```
[cockpit lab01]  --GET /api/status-->  [ESP32-C5]   (lee fans + hdds + failsafe)
[cockpit]        --POST /api/fan {duty} + Bearer-->  [ESP32-C5]   (aplica duty, refresca watchdog)
   (si el cockpit calla > 60 s)         [watchdog]  --> ventiladores al 100%, failsafe=true
```

Diseño y decisiones: [`../../docs/superpowers/specs/2026-06-08-opennas-fw-failsafe-auth-design.md`](../../docs/superpowers/specs/2026-06-08-opennas-fw-failsafe-auth-design.md).

## Componentes (`components/`)

| Componente | Qué hace |
|---|---|
| `board` | Init de placa (I2C, pines). Ver `board_pins.h`. |
| `fan_control` | 2 ventiladores: PWM LEDC 25 kHz + tacómetro PCNT. **Watchdog failsafe** + duty solo en RAM. |
| `hdd_monitor` | 6 HDD: lee los pines ACT vía TCA9554 (I2C) a 50 Hz; `active` / `last_active` / `event_count`. |
| `net_manager` | WiFi: modo **AP** (provisioning) o **STA** (conectado al router). Credenciales en NVS. |
| `provisioning` | Portal cautivo (DNS hijack) en modo AP para meter las credenciales WiFi. |
| `http_server` | Servidor httpd :80 + API REST (`api_handlers.c`) + **auth Bearer compartida** (`http_auth_ok`). |
| `ota_manager` | Actualización de firmware por OTA (`POST /api/ota`) + guardia de rollback. |
| `main` | Orquesta el arranque: board → fan → hdd → ota → http_server → (en STA) registra API + OTA. |

## API HTTP

Base: `http://<ip-del-esp>/`. La IP se ve en `GET /api/status` y en el log de arranque.

| Método | Endpoint | Auth | Cuerpo / efecto |
|---|---|---|---|
| GET | `/` | — | Dashboard HTML embebido. |
| GET | `/api/status` | **abierto** | Estado completo (JSON, ver abajo). |
| POST | `/api/fan/{0,1}` | **Bearer** | `{"duty":0..100}` → 204. Aplica el duty y refresca el watchdog. |
| POST | `/api/set_token` | **Bearer** | `{"token":".."}` → fija el token de auth en NVS y lo aplica al instante (sin reboot). |
| POST | `/api/set_wifi` | **Bearer** | `{"ssid":"..","pass":".."}` → guarda creds WiFi y reinicia para reconectar. |
| POST | `/api/reboot` | **Bearer** | Reinicia el ESP. |
| POST | `/api/reset_wifi` | **Bearer** | Borra credenciales WiFi y reinicia a modo AP. |
| POST | `/api/ota` | **Bearer** | Sube una imagen de firmware (binario) y reinicia a ella. |

### `GET /api/status` (esquema)

```json
{
  "fans": [ { "id": 0, "duty": 40, "rpm": 900 }, { "id": 1, "duty": 40, "rpm": 880 } ],
  "hdds": [ { "id": 1, "active": false, "last_ms": 1297, "events": 2239 } ],
  "uptime_s": 1234,
  "free_heap": 210000,
  "failsafe": false,
  "ms_since_cmd": 4200,
  "version": "...",
  "build_date": "...",
  "ip": "10.10.10.50",
  "mac": "3c:dc:75:83:fb:9c"
}
```

> `mac` (la MAC WiFi) sirve para la reserva DHCP en fw01 (dnsmasq). El ESP está en la red del homelab:
> `opennas.lab.mquero.com` = `10.10.10.50` (movido desde la WiFi de casa con "Cambiar WiFi" en el dashboard).

- `failsafe` (bool): true si el watchdog ha forzado el modo seguro (sin comando reciente).
- `ms_since_cmd`: milisegundos desde el último `POST /api/fan` (UINT32_MAX si nunca hubo).

### Autenticación

Los **POST** exigen `Authorization: Bearer <token>`. `GET /api/status` queda abierto (solo lectura).
Un solo token gobierna todos los endpoints de escritura (fan, set_token, set_wifi, reboot, reset_wifi,
ota) — `http_auth_ok` en `http_server.h` es la única puerta de auth.

**Token efectivo:** NVS `opennas/auth_token` si existe; si no, `CONFIG_OPENNAS_AUTH_TOKEN` (Kconfig,
placeholder `changeme`). Comparación en tiempo constante; fail-closed si el token está vacío. Se cambia
desde el dashboard (cajón "Cambiar clave") o por `POST /api/set_token`. El mismo valor va al `.env` del
cockpit (`OPENNAS_TOKEN`) para que el `fan-controller` lo use.

```bash
curl -X POST http://opennas.lab.mquero.com/api/fan/0 -H "Authorization: Bearer <TOKEN>" -d '{"duty":40}'
```

### Failsafe (watchdog de ventiladores)

- Al arrancar: ambos ventiladores a `CONFIG_OPENNAS_FAILSAFE_DUTY` (100%), `failsafe=true`, hasta el
  primer comando.
- Si pasan `CONFIG_OPENNAS_FAILSAFE_TIMEOUT_S` (60 s) sin `POST /api/fan`, el watchdog fuerza ambos
  ventiladores al duty seguro. Sale del failsafe con el siguiente comando.
- `set_duty` **no persiste en NVS** (el cockpit es la fuente de verdad; evita desgaste de flash).

## Configuración (`menuconfig` → menú "OpenNAS")

| Símbolo | Default | Qué |
|---|---|---|
| `OPENNAS_FAILSAFE_DUTY` | `100` | Duty seguro (%) al que van los ventiladores en failsafe. |
| `OPENNAS_FAILSAFE_TIMEOUT_S` | `60` | Segundos sin comando antes de disparar el failsafe. |
| `OPENNAS_AUTH_TOKEN` | `"changeme"` | **Placeholder** del token Bearer. El real va por NVS (ver abajo). |

## Build & flash (ESP-IDF v6.0.1)

El entorno se activa con el script de EIM (no `export.sh`):

```bash
cd 3-Firmware/OpenNASFW
source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v6.0.1/venv"

idf.py build
idf.py -p <PUERTO> flash monitor    # primer flasheo por USB
```

Actualizaciones posteriores: por **OTA** (`POST /api/ota` con la imagen `build/OpenNASFW.bin` + token).
Alternativa: el `.devcontainer/` (imagen `espressif/idf`).

### Provisionar el token real (fuera de Git)

El default `changeme` es un placeholder. El token real se inyecta en NVS (namespace `opennas`, clave
`auth_token`) **sin commitearlo**:

```bash
TOK=$(openssl rand -hex 24)            # guárdalo: también va al .env del cockpit (OPENNAS_TOKEN)
printf 'key,type,encoding,value\nopennas,namespace,,\nauth_token,data,string,%s\n' "$TOK" > /tmp/nvs.csv
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate \
  /tmp/nvs.csv /tmp/nvs.bin 0x6000   # tamaño de la partición nvs en partitions.csv
esptool.py -p <PUERTO> write_flash <offset_nvs> /tmp/nvs.bin
rm /tmp/nvs.csv /tmp/nvs.bin
```

## Provisioning WiFi

Sin credenciales, arranca en modo **AP** con SSID `OpenNAS-setup-<xx>` (abierto). Conéctate y el portal
cautivo (`192.168.4.1`) pide SSID/clave. Se guardan en NVS y reinicia a modo **STA**.
`POST /api/reset_wifi` (con token) las borra y vuelve a AP.
