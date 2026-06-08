# OpenNAS FW — Failsafe + Auth · Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development o
> superpowers:executing-plans. Pasos con checkbox (`- [ ]`).
> Firmware **ESP-IDF (C, ESP32-C5)**: la verificación es `idf.py build` + **curl manual contra el
> dispositivo**; los helpers puros llevan test unitario donde el host target lo permita. El token real
> **nunca** se commitea.

**Goal:** Endurecer el firmware del ESP32-C5 para que sea un esclavo seguro: watchdog que sube los
ventiladores a 100% si el cockpit calla 60 s, sin desgaste de flash, y auth por token en los POST.

**Architecture:** El cockpit (lab01) será el cerebro; el ESP expone `/api/status` y acepta `POST
/api/fan`. Este plan añade un watchdog en `fan_control`, quita la persistencia NVS por comando, y un
chequeo de token Bearer en los handlers POST. Parámetros por Kconfig; token efectivo en NVS con
fallback al placeholder de Kconfig.

**Tech Stack:** ESP-IDF · C · FreeRTOS · LEDC/PCNT (ventiladores) · NVS · esp_http_server.

**Referencias:** spec `docs/superpowers/specs/2026-06-08-opennas-fw-failsafe-auth-design.md`.
Código en `3-Firmware/OpenNASFW/`. **OJO:** el árbol del repo OpenNAS tiene cambios de CAD sin
commitear (carpeta `1-CAD/`) — **no tocarlos ni stagearlos**; commitear solo ficheros bajo
`3-Firmware/` y `docs/`.

**Prerrequisito:** entorno ESP-IDF (`idf.py` en PATH, o el `.devcontainer`). Target ya es `esp32c5`.
Si `idf.py` no está disponible en la máquina, el build/flash lo hace el humano; el agente entrega el
código compilable y las instrucciones.

---

## File Structure

```
3-Firmware/OpenNASFW/
├─ main/Kconfig.projbuild          (nuevo) opciones OPENNAS_* (failsafe duty/timeout, auth token)
├─ components/fan_control/
│   ├─ include/fan_control.h       (+ note_command / in_failsafe / ms_since_cmd)
│   └─ src/fan_control.c           (quita NVS-on-set; watchdog; boot a failsafe)
└─ components/http_server/
    └─ src/api_handlers.c          (helper auth + token NVS/Kconfig; wiring de los 3 handlers)
```

---

## Task 1: Parámetros Kconfig

**Files:** Create `3-Firmware/OpenNASFW/main/Kconfig.projbuild`.

- [ ] **Step 1: Crear el Kconfig**

Create `main/Kconfig.projbuild`:
```kconfig
menu "OpenNAS"

config OPENNAS_FAILSAFE_DUTY
    int "Failsafe fan duty (%)"
    range 0 100
    default 100
    help
        Duty al que vuelven ambos ventiladores si no llega comando en el timeout.

config OPENNAS_FAILSAFE_TIMEOUT_S
    int "Failsafe timeout (s)"
    range 5 600
    default 60
    help
        Segundos sin POST /api/fan antes de que el watchdog fuerce el failsafe.

config OPENNAS_AUTH_TOKEN
    string "API auth token (placeholder; el real va por NVS)"
    default "changeme"
    help
        Valor por defecto del token Bearer para los POST. NUNCA pongas el token real
        aquí (sdkconfig se commitea). El real se inyecta en NVS (ver plan, Task 6).

endmenu
```

- [ ] **Step 2: Reconfigurar y verificar que aparecen los símbolos**

Run:
```bash
cd 3-Firmware/OpenNASFW
idf.py reconfigure
grep -E "OPENNAS_FAILSAFE_DUTY|OPENNAS_FAILSAFE_TIMEOUT_S|OPENNAS_AUTH_TOKEN" sdkconfig
```
Expected: las tres líneas `CONFIG_OPENNAS_*` con sus defaults (100, 60, "changeme").

- [ ] **Step 3: Commit**
```bash
git add 3-Firmware/OpenNASFW/main/Kconfig.projbuild 3-Firmware/OpenNASFW/sdkconfig
git commit -m "feat(fw): Kconfig OpenNAS (failsafe duty/timeout, auth token placeholder)"
```

---

## Task 2: `fan_control` — watchdog + sin NVS por comando

**Files:** Modify `components/fan_control/include/fan_control.h`, `components/fan_control/src/fan_control.c`.

- [ ] **Step 1: Ampliar el header**

En `fan_control.h`, añadir tras `fan_ctrl_get_rpm`:
```c
/* Refresca el watchdog: lo llama el handler de POST /api/fan en cada comando. */
void fan_ctrl_note_command(void);

/* true si el watchdog está en failsafe (sin comando reciente). */
bool fan_ctrl_in_failsafe(void);

/* Milisegundos desde el último comando (UINT32_MAX si nunca hubo). */
uint32_t fan_ctrl_ms_since_cmd(void);
```
Añadir `#include <stdbool.h>` arriba si no está.

- [ ] **Step 2: Quitar la persistencia NVS por comando**

En `fan_control.c`, `fan_ctrl_set_duty`:
```c
esp_err_t fan_ctrl_set_duty(uint8_t fan_id, uint8_t percent)
{
    if (fan_id >= FAN_COUNT) return ESP_ERR_INVALID_ARG;
    if (percent > FAN_DUTY_MAX) percent = FAN_DUTY_MAX;
    apply_duty(&s_fans[fan_id], percent);
    return ESP_OK;   /* ya NO persiste en NVS: el cockpit es la fuente de verdad (evita desgaste) */
}
```
Eliminar también `load_duty_from_nvs`/`save_duty_to_nvs` y las macros `NVS_KEY_FAN*` si quedan sin uso
(o dejarlas sin llamar; preferible borrarlas para no dejar código muerto). El campo `nvs_key` de
`fan_ctx_t` puede quitarse.

- [ ] **Step 3: Estado del watchdog + boot a failsafe**

En `fan_control.c`, añadir estado de fichero y usar los Kconfig:
```c
#include "sdkconfig.h"
#include "esp_timer.h"

#define FAILSAFE_DUTY        CONFIG_OPENNAS_FAILSAFE_DUTY
#define FAILSAFE_TIMEOUT_US  ((uint64_t)CONFIG_OPENNAS_FAILSAFE_TIMEOUT_S * 1000000ULL)
#define WATCHDOG_PERIOD_MS   5000

static volatile uint64_t s_last_cmd_us = 0;   /* 0 = nunca hubo comando */
static volatile bool     s_in_failsafe = true;

void fan_ctrl_note_command(void)
{
    s_last_cmd_us = esp_timer_get_time();
    s_in_failsafe = false;
}

bool fan_ctrl_in_failsafe(void) { return s_in_failsafe; }

uint32_t fan_ctrl_ms_since_cmd(void)
{
    if (s_last_cmd_us == 0) return UINT32_MAX;
    return (uint32_t)((esp_timer_get_time() - s_last_cmd_us) / 1000ULL);
}

static void fan_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));
        uint64_t now = esp_timer_get_time();
        bool expired = (s_last_cmd_us == 0) || (now - s_last_cmd_us > FAILSAFE_TIMEOUT_US);
        if (expired && !s_in_failsafe) {
            for (int i = 0; i < FAN_COUNT; ++i) apply_duty(&s_fans[i], FAILSAFE_DUTY);
            s_in_failsafe = true;
        }
    }
}
```
Necesitarás `#include <stdint.h>` y `<stdbool.h>` (probablemente ya vía fan_control.h).

- [ ] **Step 4: Arrancar en failsafe + lanzar el watchdog en `fan_ctrl_init`**

Sustituir, dentro del bucle de init de `fan_ctrl_init`, la carga de NVS por el duty seguro:
```c
    for (int i = 0; i < FAN_COUNT; ++i) {
        err = pcnt_setup_fan(&s_fans[i]);
        if (err != ESP_OK) { ESP_LOGE(TAG, "PCNT fan%d setup failed: %s", i, esp_err_to_name(err)); return err; }
        apply_duty(&s_fans[i], FAILSAFE_DUTY);   /* arranca seguro hasta que el cockpit mande */
        ESP_LOGI(TAG, "fan%d PWM=GPIO%d TACH=GPIO%d failsafe_duty=%u%%",
                 i, s_fans[i].pwm_gpio, s_fans[i].tach_gpio, (unsigned)FAILSAFE_DUTY);
    }
    s_in_failsafe = true;
    s_last_cmd_us = 0;

    BaseType_t r1 = xTaskCreate(fan_tach_task, "fan_tach", 2048, NULL, 5, NULL);
    BaseType_t r2 = xTaskCreate(fan_watchdog_task, "fan_wdt", 2560, NULL, 5, NULL);
    return (r1 == pdPASS && r2 == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
```

- [ ] **Step 5: Compilar**

Run: `cd 3-Firmware/OpenNASFW && idf.py build`
Expected: build OK. Sin warnings de símbolos NVS sin usar (los borraste).

- [ ] **Step 6: Commit**
```bash
git add 3-Firmware/OpenNASFW/components/fan_control
git commit -m "feat(fw): watchdog failsafe + sin persistencia NVS por comando (esclavo)"
```

---

## Task 3: Helper de auth + token efectivo

**Files:** Modify `components/http_server/src/api_handlers.c`.

- [ ] **Step 1: Token efectivo (NVS con fallback a Kconfig) + comparación en tiempo constante**

En `api_handlers.c`, añadir tras los `#include` (añadir `#include "nvs.h"`, `#include "sdkconfig.h"`):
```c
/* Devuelve el token efectivo: NVS opennas/auth_token si existe, si no el de Kconfig.
 * Se cachea en una estática tras la primera lectura. */
static const char *effective_token(void)
{
    static char tok[64];
    static bool loaded = false;
    if (loaded) return tok;
    nvs_handle_t h;
    size_t len = sizeof(tok);
    if (nvs_open("opennas", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "auth_token", tok, &len) != ESP_OK) {
            strlcpy(tok, CONFIG_OPENNAS_AUTH_TOKEN, sizeof(tok));
        }
        nvs_close(h);
    } else {
        strlcpy(tok, CONFIG_OPENNAS_AUTH_TOKEN, sizeof(tok));
    }
    loaded = true;
    return tok;
}

/* Comparación en tiempo constante (evita timing oracle). */
static bool ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i)
        diff |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    return diff == 0;
}

/* true si el header Authorization trae "Bearer <token>" correcto. */
static bool http_auth_ok(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = hdr;
    if (strncmp(p, "Bearer ", 7) != 0) return false;
    p += 7;
    while (*p == ' ') p++;
    return ct_equal(p, effective_token());
}
```
> Nota: `strlcpy` está disponible en ESP-IDF (newlib). Si el toolchain se queja, usar
> `snprintf(tok, sizeof tok, "%s", CONFIG_OPENNAS_AUTH_TOKEN)`.

- [ ] **Step 2: Compilar**

Run: `idf.py build`
Expected: build OK (helpers aún sin usar → puede avisar de "unused"; se usan en Task 4, o marcar y
seguir; si el warning es error, cablear ya en Task 4 antes de compilar).

- [ ] **Step 3: Commit**
```bash
git add 3-Firmware/OpenNASFW/components/http_server/src/api_handlers.c
git commit -m "feat(fw): helper de auth Bearer + token efectivo (NVS/Kconfig, compare ct)"
```

---

## Task 4: Cablear los handlers

**Files:** Modify `components/http_server/src/api_handlers.c`.

- [ ] **Step 1: `h_fan_post` — auth + note_command**

Al inicio de `h_fan_post`, antes de parsear el id:
```c
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
```
Y tras un `fan_ctrl_set_duty` correcto (antes del 204), refrescar el watchdog:
```c
    fan_ctrl_note_command();
```

- [ ] **Step 2: `h_reset_wifi_post` — auth**

Al inicio de `h_reset_wifi_post`:
```c
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
```

- [ ] **Step 3: `h_status_get` — exponer failsafe**

En `h_status_get`, tras el campo `free_heap` (antes de version), añadir:
```c
    n += snprintf(buf + n, sizeof(buf) - n,
                  ",\"failsafe\":%s,\"ms_since_cmd\":%lu",
                  fan_ctrl_in_failsafe() ? "true" : "false",
                  (unsigned long)fan_ctrl_ms_since_cmd());
```
(El buffer es de 768 B; estos campos caben de sobra.)

- [ ] **Step 4: Compilar**

Run: `idf.py build`
Expected: build OK, sin warnings de funciones sin usar (ya se usan todas).

- [ ] **Step 5: Commit**
```bash
git add 3-Firmware/OpenNASFW/components/http_server/src/api_handlers.c
git commit -m "feat(fw): auth en POST (fan/reset_wifi) + failsafe en /api/status"
```

---

## Task 5: Flash/OTA + verificación manual contra el dispositivo

**Files:** ninguno (verificación). Requiere el ESP físico (`192.168.1.153`).

- [ ] **Step 1: Provisionar un token de prueba en NVS (temporal)**

Para probar sin flashear NVS aún, basta con que el token efectivo sea el de Kconfig (`changeme`) — o
poner el real (Task 6). Para esta verificación usa el que devuelva el ESP (por defecto `changeme`).

- [ ] **Step 2: Flashear (USB) o subir por OTA**

Run (USB): `idf.py -p <PUERTO> flash monitor`  ·  o OTA si el `ota_manager` lo permite.
Expected: arranca, conecta a WiFi, log `http server listening on :80`. Al arrancar, ventiladores a 100%
(failsafe) → `curl http://192.168.1.153/api/status` muestra `"failsafe":true`.

- [ ] **Step 3: Auth — POST sin token → 401, con token → 204**

Run:
```bash
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://192.168.1.153/api/fan/0 -d '{"duty":30}'
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://192.168.1.153/api/fan/0 \
  -H "Authorization: Bearer changeme" -d '{"duty":30}'
```
Expected: `401` (sin token) y `204` (con token). Tras el 204, `rpm` del fan 0 baja en `/api/status`.

- [ ] **Step 4: Failsafe — dejar de mandar 60 s → 100%**

Run:
```bash
# tras el 204 anterior, esperar > 60 s sin mandar comandos
sleep 65
curl -s http://192.168.1.153/api/status | grep -o '"failsafe":[a-z]*'
```
Expected: `"failsafe":true` y los rpm de vuelta a ~máx.

- [ ] **Step 5: Salir del failsafe**

Run: `curl -s -X POST http://192.168.1.153/api/fan/0 -H "Authorization: Bearer changeme" -d '{"duty":40}'`
luego `curl -s http://192.168.1.153/api/status | grep -o '"failsafe":[a-z]*'`
Expected: `"failsafe":false`.

- [ ] **Step 6: (No hay commit aquí — es verificación.)** Si algo falla, volver a la task correspondiente.

---

## Task 6: Provisión del token real + docs

**Files:** Create `3-Firmware/OpenNASFW/docs/API.md`; (opcional) `nvs_opennas.csv` **NO commiteado**.

- [ ] **Step 1: Generar el token real y meterlo en NVS (fuera de Git)**

```bash
# token aleatorio
TOK=$(openssl rand -hex 24)
echo "$TOK"   # guárdalo: irá también al .env del cockpit (OPENNAS_TOKEN) cuando se haga el cerebro
# CSV de NVS (NO commitear: contiene el secreto)
cat > /tmp/nvs_opennas.csv <<EOF
key,type,encoding,value
opennas,namespace,,
auth_token,data,string,$TOK
EOF
# generar imagen NVS y flashear a la partición 'nvs' (ver partitions.csv para offset/size)
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate \
  /tmp/nvs_opennas.csv /tmp/nvs_opennas.bin 0x6000
esptool.py -p <PUERTO> write_flash <offset_nvs> /tmp/nvs_opennas.bin
rm /tmp/nvs_opennas.csv /tmp/nvs_opennas.bin
```
> El offset/size de la partición `nvs` salen de `partitions.csv`. Tras esto, `effective_token()` usa el
> token real en vez de `changeme`.

- [ ] **Step 2: Verificar con el token real**

Run: `curl -s -o /dev/null -w "%{http_code}\n" -X POST http://192.168.1.153/api/fan/0 -H "Authorization: Bearer $TOK" -d '{"duty":40}'`
Expected: `204`. Con `changeme` ahora → `401`.

- [ ] **Step 3: Documentar la API**

Create `3-Firmware/OpenNASFW/docs/API.md`: tabla de endpoints (`GET /api/status` con el JSON completo
incluyendo `failsafe`/`ms_since_cmd`; `POST /api/fan/{0,1}` con `Authorization: Bearer` y `{duty}`;
`POST /api/reset_wifi` con auth), el comportamiento del failsafe (100%/60s) y cómo provisionar el token
(Step 1, sin valores reales). Enlazar el spec.

- [ ] **Step 4: Asegurar que el secreto no se cuela + commit**
```bash
cd /Users/mquero/Documents/MQuero/OpenNAS
git status -s | grep -vE "1-CAD" | grep -E "3-Firmware|docs"   # revisar solo lo nuestro
git add 3-Firmware/OpenNASFW/docs/API.md
git diff --staged | grep -iE "auth_token|bearer [0-9a-f]{12}|OPENNAS_TOKEN=" || echo ">> limpio (sin token real)"
git commit -m "docs(fw): API.md (endpoints + auth + failsafe) y provisión de token"
```

- [ ] **Step 5: (Para más adelante)** el mismo token va a `/opt/cockpit/.env` del LXC como `OPENNAS_TOKEN`
  cuando se construya el cerebro de la curva (otro spec). Anotarlo donde guardes secretos.

---

## Self-review (cobertura del spec)
- Spec §3.1 watchdog + sin NVS → Task 2 ✅ · §3.2 Kconfig → Task 1 ✅ · §3.3 auth helper → Task 3 ✅ ·
  §3.4 wiring (auth POST + status failsafe) → Task 4 ✅ · §5 casos límite (boot failsafe, 401, reboot) →
  Tasks 2/4/5 ✅ · §6 secretos (placeholder + NVS) → Tasks 1/6 ✅ · §7 testing (curl) → Task 5 ✅ ·
  §8 deploy (flash/OTA) → Task 5/6 ✅.
- Sin persistencia NVS por comando: Task 2 Step 2.
- Consistencia de nombres: `fan_ctrl_note_command` / `fan_ctrl_in_failsafe` / `fan_ctrl_ms_since_cmd`,
  `http_auth_ok`, `effective_token`, NVS `opennas`/`auth_token`, CONFIG_OPENNAS_* — usados igual en
  header, fan_control.c y api_handlers.c.
- Riesgos señalados: `strlcpy` fallback a snprintf; warnings de "unused" entre Task 3 y 4; offset NVS
  desde partitions.csv; build requiere ESP-IDF.
- No tocar `1-CAD/` (árbol sucio del usuario); commitear solo `3-Firmware/` y `docs/`.
```
