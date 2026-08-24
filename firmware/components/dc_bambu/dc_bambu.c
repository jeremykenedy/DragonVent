// Bambu Lab LAN MQTT client. Reads the printer's live bed temperature over its
// on-device MQTT-over-TLS broker so AUTO can follow a Bambu print, mirroring the
// Moonraker path. Read-only — no control commands are ever sent to the printer.
//
// Validated against a real Bambu P1S (2026-08-11): connects, subscribes, and
// decodes live report data. Built from the OpenBambuAPI / ha-bambulab protocol
// spec; other models remain unvalidated. Protocol:
//   mqtts://<host>:8883, user "bblp", pass = LAN access code, self-signed cert
//   (CN=serial, connect by IP -> cert verification relaxed). Subscribe
//   device/<serial>/report; publish one "pushall" on connect (P1/A1 send deltas).
// See plans/control-source-bambu-ha.md.
#include "dc_bambu.h"
#include "dc_bambu_parse.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp / strcasecmp for filament zone matching

// Bambu's broker presents a per-device SELF-SIGNED cert (CN=serial) and we reach
// it by IP, so there is no CA to verify against and the client below deliberately
// connects without server-cert verification. esp-tls only permits that when these
// are enabled; without them it refuses to build the TLS context at all and every
// connect fails with "No server verification option set in esp_tls_cfg_t" —
// which reads like a network or credential fault, not a missing build option.
//
// Asserted here rather than left to a comment because a product supplying neither
// still compiles, links and runs, and only fails on a real connection attempt.
// DragonVent shipped in exactly that state; see justinh-rahb/DragonVent#13.
#if !defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) || !defined(CONFIG_ESP_TLS_INSECURE)
#error "dc_bambu requires CONFIG_ESP_TLS_INSECURE=y and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y \
in the product's sdkconfig.defaults. Bambu's LAN broker uses a per-device self-signed cert reached \
by IP, so there is no CA to verify against; without these esp-tls fails SSL setup and the client can \
never connect. Add both, or drop the dc_bambu dependency."
#endif

#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "dc_bambu";

#define NVS_NS   "app_nvs"
#define KEY_HOST "bb_host"
#define KEY_SER  "bb_serial"
#define KEY_CODE "bb_code"

// A full "pushall" report is ~10-15 KB. esp-mqtt fragments payloads larger than
// its RX buffer; we reassemble up to RX_CAP (bed_temper is near the front of the
// print object, so a truncated tail still yields the follow signal).
#define MQTT_BUF   8192
// Measured on a P2S with AMS: a pushall report is 18,922 bytes, so the old
// 16 KB cap silently dropped everything past that offset — wifi_signal (18519)
// and subtask_name (16402) among them. They parsed as absent, not as wrong,
// which is exactly the failure mode that hides. 32 KB leaves real headroom.
#define RX_CAP     32768

// One pushall on connect; required on P1/A1 (delta-only), harmless on X1.
static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";

static SemaphoreHandle_t        s_lock  = NULL;
static dc_bambu_config_t        s_cfg   = {0};
static dc_bambu_status_t        s_status = {
    .state = DC_BAMBU_DISABLED, .bed_temp = NAN, .bed_target = NAN, .chamber_temp = NAN, .progress = -1,
};
static esp_mqtt_client_handle_t s_client = NULL;

static char   s_report_topic[80]  = {0};   // device/<serial>/report
static char   s_request_topic[80] = {0};   // device/<serial>/request
static char  *s_rx = NULL;                 // RX_CAP reassembly buffer
static size_t s_rx_len = 0;
static bool   s_in_report = false;         // current inbound msg is on the report topic

// ---------- NVS ----------

static esp_err_t nvs_load(dc_bambu_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host = unconfigured

    sz = sizeof(out->serial);
    nvs_get_str(h, KEY_SER, out->serial, &sz);
    sz = sizeof(out->code);
    nvs_get_str(h, KEY_CODE, out->code, &sz);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_bambu_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_SER, cfg->serial);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CODE, cfg->code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- report parsing ----------

// Pull the float value of a JSON key via a targeted scan (no full JSON parse — the
// C3 can't afford a parse tree over a 15 KB payload). `key` includes the quotes,
// e.g. "\"bed_temper\"".
static bool find_float(const char *s, const char *key, float *out)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    char *end;
    float v = strtof(q + 1, &end);
    if (end == q + 1) return false;   // no number parsed
    *out = v;
    return true;
}

// Same targeted scan, but for the fan speeds, which Bambu emits as QUOTED
// integers ("big_fan2_speed":"15"). find_float() strtof's straight from the
// colon and bails on the opening quote, so it can never read these.
// Bambu is inconsistent about quoting numbers: "spd_lvl": 2 but
// "nozzle_diameter": "0.4" and "mc_print_error_code": "0". find_float() strtof's
// straight from the colon and bails on the opening quote, so quoted numerics
// silently read as absent. This tolerates either form.
static bool find_num(const char *s, const char *key, float *out)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '"') q++;
    char *end;
    float v = strtof(q, &end);
    if (end == q) return false;
    *out = v;
    return true;
}

static bool find_quoted_int(const char *s, const char *key, int *out)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '"') q++;
    if (*q < '0' || *q > '9') return false;
    *out = (int)strtol(q, NULL, 10);
    return true;
}

// find_string() + active_filament() (tri-state) live in dc_bambu_parse.h so they
// can be host-unit-tested (tests/dc_bambu_host_test.c).

static dc_bambu_fans_t s_fans = { -1, -1, -1, -1 };
static char s_gcode_err[64] = "";

// Find a float inside a named object rather than at top level. The H2
// generation moved chamber temperature out of a flat "chamber_temper" and into
// device.ctc.info.temp, so on a P2S the flat key is simply absent and chamber
// temp read as nothing. Scan forward from the object key rather than parsing.
static bool find_float_in(const char *s, const char *obj, const char *key, float *out)
{
    const char *o = strstr(s, obj);
    if (!o) return false;
    const char *q = strstr(o, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    char *end;
    float v = strtof(q + 1, &end);
    if (end == q + 1) return false;
    *out = v;
    return true;
}

// Display-only detail. NAN / -1 mean "never reported", so a consumer can tell
// an absent field from a real zero.
static dc_bambu_detail_t s_detail = {
    .nozzle_temp = NAN, .nozzle_target = NAN, .bed_target = NAN, .chamber_temp = NAN,
    .layer = -1, .total_layers = -1, .remaining_min = -1, .wifi_dbm = 1,
    .speed_level = -1, .nozzle_diameter = NAN, .error_code = -1,
};

static void parse_report(const char *json)
{
    float bed, bedtgt, cham, percent;
    bool got_bed  = find_float(json, "\"bed_temper\"", &bed);
    // bed_target_temper is the commanded setpoint AUTO triggers on. Match the
    // "_target_temper" so it can't be confused with "bed_temper" (strstr finds the
    // first hit; searching the more specific key avoids the prefix collision).
    bool got_tgt  = find_float(json, "bed_target_temper", &bedtgt);
    bool got_cham = find_float(json, "\"chamber_temper\"", &cham);
    bool got_percent = find_float(json, "\"mc_percent\"", &percent);
    // NOTE(phase 2b): H2/newer moved chamber temp to a packed device.ctc.info.temp
    // field; only the legacy flat chamber_temper is read here. Bed follow (the
    // goal) works on all models via bed_temper/bed_target_temper.
    char fila[16];
    dc_fila_result_t fr = dc_bambu_active_filament(json, fila, sizeof fila);
    char gs[16];
    bool got_gs = dc_bambu_find_string(json, "\"gcode_state\"", gs, sizeof gs);  // print state

    int fpart, faux, fcham, fhb;
    bool got_fpart = find_quoted_int(json, "\"cooling_fan_speed\"",   &fpart);
    bool got_faux  = find_quoted_int(json, "\"big_fan1_speed\"",      &faux);
    bool got_fcham = find_quoted_int(json, "\"big_fan2_speed\"",      &fcham);
    bool got_fhb   = find_quoted_int(json, "\"heatbreak_fan_speed\"", &fhb);

    float df; char sbuf[64];
    dc_bambu_detail_t nd = { .nozzle_temp = NAN, .nozzle_target = NAN, .bed_target = NAN,
                             .chamber_temp = NAN, .layer = -1, .total_layers = -1,
                             .remaining_min = -1, .wifi_dbm = 1, .speed_level = -1,
                             .nozzle_diameter = NAN, .error_code = -1 };
    bool nd_any = false;
    #define SCAN_F(key, field) do { if (find_num(json, key, &df)) { nd.field = df; nd_any = true; } } while (0)
    #define SCAN_I(key, field) do { if (find_num(json, key, &df)) { nd.field = (int)df; nd_any = true; } } while (0)
    SCAN_F("\"nozzle_temper\"",       nozzle_temp);
    SCAN_F("nozzle_target_temper",    nozzle_target);
    SCAN_F("bed_target_temper",       bed_target);
    SCAN_I("\"layer_num\"",           layer);
    SCAN_I("\"total_layer_num\"",     total_layers);
    SCAN_I("\"mc_remaining_time\"",   remaining_min);
    SCAN_I("\"spd_lvl\"",             speed_level);
    SCAN_F("\"nozzle_diameter\"",     nozzle_diameter);
    SCAN_I("\"mc_print_error_code\"", error_code);
    #undef SCAN_F
    #undef SCAN_I
    if (got_cham) { nd.chamber_temp = cham; nd_any = true; }
    else if (find_float_in(json, "\"ctc\"", "\"temp\"", &df)) { nd.chamber_temp = df; nd_any = true; }
    // wifi_signal arrives as a string like "-40dBm"; strtol stops at the unit.
    if (dc_bambu_find_string(json, "\"wifi_signal\"", sbuf, sizeof sbuf)) {
        nd.wifi_dbm = (int)strtol(sbuf, NULL, 10); nd_any = true; }
    // Read straight into the destination so the field's own size bounds the
    // copy; going via a wider scratch buffer just invites a truncation warning.
    if (dc_bambu_find_string(json, "\"nozzle_type\"",  nd.nozzle_type, sizeof nd.nozzle_type)) nd_any = true;
    if (dc_bambu_find_string(json, "\"subtask_name\"", nd.job_name,    sizeof nd.job_name))    nd_any = true;

    // gcode_line acknowledgement. Arrives on the same report topic as status:
    //   {"print":{"command":"gcode_line","result":"failed",
    //             "reason":"mqtt message verify failed","sequence_id":"802"}}
    // Only look when the payload actually mentions gcode_line, so a normal
    // push_status can never be mistaken for an ack.
    char ack_res[24], ack_why[64];
    bool is_ack = strstr(json, "\"gcode_line\"") != NULL
               && dc_bambu_find_string(json, "\"result\"", ack_res, sizeof ack_res);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (is_ack) {
        if (strcmp(ack_res, "success") == 0) {
            s_gcode_err[0] = '\0';
        } else {
            if (!dc_bambu_find_string(json, "\"reason\"", ack_why, sizeof ack_why) || !ack_why[0])
                snprintf(ack_why, sizeof ack_why, "%s", ack_res);
            snprintf(s_gcode_err, sizeof s_gcode_err, "%s", ack_why);
            ESP_LOGW(TAG, "printer rejected gcode: %s", s_gcode_err);
        }
    }
    if (nd_any) {
        // Deltas omit most keys, so merge field by field instead of assigning
        // the struct wholesale — otherwise every partial report would blank
        // everything it happened not to mention.
        if (!isnan(nd.nozzle_temp))     s_detail.nozzle_temp     = nd.nozzle_temp;
        if (!isnan(nd.nozzle_target))   s_detail.nozzle_target   = nd.nozzle_target;
        if (!isnan(nd.bed_target))      s_detail.bed_target      = nd.bed_target;
        if (!isnan(nd.chamber_temp))    s_detail.chamber_temp    = nd.chamber_temp;
        if (!isnan(nd.nozzle_diameter)) s_detail.nozzle_diameter = nd.nozzle_diameter;
        if (nd.layer         >= 0) s_detail.layer         = nd.layer;
        if (nd.total_layers  >= 0) s_detail.total_layers  = nd.total_layers;
        if (nd.remaining_min >= 0) s_detail.remaining_min = nd.remaining_min;
        if (nd.speed_level   >= 0) s_detail.speed_level   = nd.speed_level;
        if (nd.error_code    >= 0) s_detail.error_code    = nd.error_code;
        if (nd.wifi_dbm      <= 0) s_detail.wifi_dbm      = nd.wifi_dbm;
        if (nd.nozzle_type[0]) snprintf(s_detail.nozzle_type, sizeof s_detail.nozzle_type, "%s", nd.nozzle_type);
        if (nd.job_name[0])    snprintf(s_detail.job_name,    sizeof s_detail.job_name,    "%s", nd.job_name);
    }
    if (got_fpart) s_fans.part      = fpart;
    if (got_faux)  s_fans.aux       = faux;
    if (got_fcham) s_fans.chamber   = fcham;
    if (got_fhb)   s_fans.heatbreak = fhb;
    if (got_bed) {
        s_status.bed_temp  = bed;
        s_status.state     = DC_BAMBU_SUBSCRIBED;   // we have live data now
        s_status.connected = true;
    }
    if (got_tgt)  s_status.bed_target = bedtgt;
    if (got_cham) s_status.chamber_temp = cham;
    if (got_percent) s_status.progress = percent < 0 ? 0 : percent > 100 ? 1 : percent / 100.0f;
    if (got_gs) {
        s_status.printing = dc_bambu_gcode_active(gs);   // keep prior if a delta omits it
        s_status.error    = (strcmp(gs, "FAILED") == 0); // Bambu's print-failed state
        switch (dc_bambu_gcode_phase(gs)) {
        case DC_BAMBU_GCODE_IDLE:        s_status.print_state = DC_BAMBU_PRINT_IDLE; break;
        case DC_BAMBU_GCODE_DOWNLOADING: s_status.print_state = DC_BAMBU_PRINT_DOWNLOADING; break;
        case DC_BAMBU_GCODE_PREPARING:   s_status.print_state = DC_BAMBU_PRINT_PREPARING; break;
        case DC_BAMBU_GCODE_PRINTING:    s_status.print_state = DC_BAMBU_PRINT_PRINTING; break;
        case DC_BAMBU_GCODE_PAUSED:      s_status.print_state = DC_BAMBU_PRINT_PAUSED; break;
        case DC_BAMBU_GCODE_COMPLETE:    s_status.print_state = DC_BAMBU_PRINT_COMPLETE; break;
        case DC_BAMBU_GCODE_ERROR:       s_status.print_state = DC_BAMBU_PRINT_ERROR; break;
        default:                          s_status.print_state = DC_BAMBU_PRINT_UNKNOWN; break;
        }
    }
    // Tri-state: PRESENT updates the filament; EMPTY (unload / print end / no spool)
    // CLEARS it so a stale zone is never applied; ABSENT (a delta that simply omits
    // the AMS/tray block) leaves the last known value untouched.
    bool fila_changed = false;
    if (fr == DC_FILA_PRESENT && strcmp(fila, s_status.filament) != 0) {
        snprintf(s_status.filament, sizeof s_status.filament, "%s", fila);
        fila_changed = true;
    } else if (fr == DC_FILA_EMPTY && s_status.filament[0]) {
        s_status.filament[0] = '\0';
        fila_changed = true;
    }
    xSemaphoreGive(s_lock);

    if (fila_changed) ESP_LOGI(TAG, "active filament: %s", fila[0] ? fila : "(none)");
    if (got_bed) ESP_LOGD(TAG, "bed=%.1f chamber=%.1f", bed, got_cham ? cham : NAN);
}

static bool topic_is_report(const char *topic, int len)
{
    return len > 0 && (size_t)len == strlen(s_report_topic)
        && strncmp(topic, s_report_topic, (size_t)len) == 0;
}

// ---------- mqtt events ----------

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected; subscribing %s", s_report_topic);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_CONNECTED;   // not SUBSCRIBED until first report
        s_status.last_failure[0] = '\0';
        xSemaphoreGive(s_lock);
        esp_mqtt_client_subscribe(e->client, s_report_topic, 0);
        esp_mqtt_client_publish(e->client, s_request_topic, PUSHALL, 0, 0, 0);
        break;

    case MQTT_EVENT_ERROR: {
        // Surface WHY a connection attempt failed, the way dc_wifi surfaces
        // last_failure. A wrong access code otherwise fails in silence.
        esp_mqtt_error_codes_t *er = e->error_handle;
        char msg[64] = "";
        if (er && er->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            switch (er->connect_return_code) {
            case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
            case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                snprintf(msg, sizeof msg, "access code rejected by the printer"); break;
            case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                snprintf(msg, sizeof msg, "printer MQTT service unavailable"); break;
            case MQTT_CONNECTION_REFUSE_PROTOCOL:
            case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                snprintf(msg, sizeof msg, "printer refused the MQTT session"); break;
            default:
                snprintf(msg, sizeof msg, "connection refused (code %d)", (int)er->connect_return_code); break;
            }
        } else if (er && er->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            snprintf(msg, sizeof msg, "no TCP/TLS connection to %.36s", s_cfg.host);
        }
        if (msg[0]) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snprintf(s_status.last_failure, sizeof s_status.last_failure, "%s", msg);
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "connect failed: %s", msg);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state     = DC_BAMBU_DISCONNECTED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        break;

    case MQTT_EVENT_DATA: {
        // Reassemble a possibly-fragmented payload. The topic is present only on
        // the first fragment (offset 0); track whether this message is the report.
        if (e->current_data_offset == 0) {
            s_in_report = topic_is_report(e->topic, e->topic_len);
            s_rx_len = 0;
        }
        if (!s_in_report || s_rx == NULL) break;
        size_t off = (size_t)e->current_data_offset;
        if (off < RX_CAP - 1) {
            size_t copy = (size_t)e->data_len;
            if (off + copy > RX_CAP - 1) copy = (RX_CAP - 1) - off;
            memcpy(s_rx + off, e->data, copy);
            s_rx_len = off + copy;
        }
        if (e->current_data_offset + e->data_len >= e->total_data_len) {
            s_rx[s_rx_len] = '\0';
            parse_report(s_rx);
        }
        break;
    }

    default:
        break;
    }
}

// ---------- lifecycle ----------

static void heal_task(void *arg);

/* Bring the MQTT client up from the saved config. Called with NO locks held:
 * client stop/start joins the event task, which itself takes s_lock, so
 * orchestrating while holding the lock would deadlock. */
static esp_err_t client_bringup(void)
{
    dc_bambu_config_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg = s_cfg;
    xSemaphoreGive(s_lock);

    if (cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Bambu config saved; idle");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_DISABLED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (cfg.serial[0] == '\0' || cfg.code[0] == '\0') {
        ESP_LOGW(TAG, "Bambu needs host + serial + access code; idle");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_DISABLED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    if (s_rx == NULL) {
        s_rx = malloc(RX_CAP);
        if (s_rx == NULL) return ESP_ERR_NO_MEM;
    }
    snprintf(s_report_topic,  sizeof s_report_topic,  "device/%s/report",  cfg.serial);
    snprintf(s_request_topic, sizeof s_request_topic, "device/%s/request", cfg.serial);

    char uri[96];
    snprintf(uri, sizeof uri, "mqtts://%s:8883", cfg.host);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        // Self-signed per-device cert (CN=serial) reached by IP: no CA to verify
        // against, so a read-only LAN client connects WITHOUT server-cert
        // verification. With no CA/bundle set, esp-tls falls through to VERIFY_NONE
        // only because CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled (see
        // sdkconfig.defaults) — otherwise it errors "No server verification option
        // set" and the connect fails. skip the CN check too since IP != serial.
        .broker.verification.skip_cert_common_name_check = true,
        .broker.verification.use_global_ca_store = false,
        .credentials.username = "bblp",
        .credentials.authentication.password = cfg.code,   // esp-mqtt duplicates config strings
        .buffer.size = MQTT_BUF,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mc);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_DISCONNECTED;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_DISCONNECTED;
        xSemaphoreGive(s_lock);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_client = client;
    s_status.state = DC_BAMBU_CONNECTING;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "connecting to %s (serial %s)", uri, cfg.serial);
    return ESP_OK;
}

/* Detach the running client and stop it with no locks held. */
static void client_teardown(dc_bambu_state_t idle_state)
{
    esp_mqtt_client_handle_t old;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    old = s_client;
    s_client = NULL;
    s_status.state = idle_state;
    s_status.connected = false;
    xSemaphoreGive(s_lock);
    if (old) {
        esp_mqtt_client_stop(old);
        esp_mqtt_client_destroy(old);
    }
}

esp_err_t dc_bambu_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    (void)nvs_load(&s_cfg);
    esp_err_t err = client_bringup();

    /* Watch for a printer whose DHCP lease moved it to a new address: after a
     * sustained outage, rediscover by serial over SSDP and rebind. */
    if (xTaskCreate(heal_task, "bb_heal", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "could not start the rebind watcher");
    return err;
}

esp_err_t dc_bambu_restart(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    client_teardown(DC_BAMBU_DISCONNECTED);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.last_failure[0] = '\0';
    xSemaphoreGive(s_lock);
    return client_bringup();
}

esp_err_t dc_bambu_set_config(const dc_bambu_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
        return dc_bambu_restart();   // apply immediately; no reboot needed
    }
    s_cfg = *cfg;
    return ESP_OK;                   // pre-start: dc_bambu_start() picks it up
}

/* Sustained-outage watcher. 90 s of DISCONNECTED/CONNECTING with a full config
 * triggers one SSDP scan; a discovery whose serial matches but whose host
 * differs rebinds the client to the printer's new address. */
static void heal_task(void *arg)
{
    (void)arg;
    int64_t down_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        dc_bambu_state_t st;
        char host[64], serial[32];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        st = s_status.state;
        snprintf(host, sizeof host, "%s", s_cfg.host);
        snprintf(serial, sizeof serial, "%s", s_cfg.serial);
        xSemaphoreGive(s_lock);

        if (st != DC_BAMBU_DISCONNECTED && st != DC_BAMBU_CONNECTING) { down_since = 0; continue; }
        if (serial[0] == '\0' || host[0] == '\0') { down_since = 0; continue; }

        int64_t now = esp_timer_get_time();
        if (down_since == 0) { down_since = now; continue; }
        if (now - down_since < 90LL * 1000000LL) continue;

        ESP_LOGW(TAG, "printer unreachable for 90 s; scanning the LAN for serial %s", serial);
        (void)dc_bambu_scan_start();
        vTaskDelay(pdMS_TO_TICKS(9000));
        dc_bambu_found_t found[DC_BAMBU_DISCOVER_MAX];
        int n = dc_bambu_discover_get(found, DC_BAMBU_DISCOVER_MAX);
        for (int i = 0; i < n; ++i) {
            if (strcmp(found[i].serial, serial) == 0 && strcmp(found[i].host, host) != 0) {
                ESP_LOGW(TAG, "printer %s moved %s -> %s; rebinding", serial, host, found[i].host);
                dc_bambu_config_t cfg;
                dc_bambu_get_config(&cfg);
                snprintf(cfg.host, sizeof cfg.host, "%s", found[i].host);
                (void)dc_bambu_set_config(&cfg);   // persists + reconnects
                break;
            }
        }
        down_since = now;   // rate-limit: at most one scan per outage window
    }
}

esp_err_t dc_bambu_get_config(dc_bambu_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_lock) {
        dc_bambu_config_t persisted;
        if (nvs_load(&persisted) == ESP_OK) {
            *out = persisted;
        } else {
            *out = s_cfg;
        }
        return ESP_OK;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_bambu_get_status(dc_bambu_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_bambu_get_fans(dc_bambu_fans_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_lock) { *out = (dc_bambu_fans_t){ -1, -1, -1, -1 }; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fans;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_bambu_get_detail(dc_bambu_detail_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_detail;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void dc_bambu_last_gcode_error(char *out, size_t len)
{
    if (!out || !len) return;
    if (!s_lock) { out[0] = '\0'; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, len, "%s", s_gcode_err);
    xSemaphoreGive(s_lock);
}

esp_err_t dc_bambu_send_gcode(const char *line)
{
    if (!line || !line[0] || strchr(line, '\n')) return ESP_ERR_INVALID_ARG;
    if (!s_client || !s_request_topic[0]) return ESP_ERR_INVALID_STATE;
    // The printer ignores a repeated sequence_id, so it has to move every call.
    static uint32_t seq = 1000;
    char payload[192];
    int n = snprintf(payload, sizeof payload,
        "{\"print\":{\"sequence_id\":\"%lu\",\"command\":\"gcode_line\",\"param\":\"%s\\n\"}}",
        (unsigned long)(++seq), line);
    if (n <= 0 || n >= (int)sizeof payload) return ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "gcode -> %s", line);
    // Clear the previous verdict so the UI can tell "no answer yet" from a
    // stale failure; the ack (or its absence) refreshes it within a second.
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); s_gcode_err[0] = '\0'; xSemaphoreGive(s_lock); }
    // QoS 0, same as the pushall above. At QoS 1 esp-mqtt blocks the calling task
    // until the PUBACK lands, and the caller here is the single httpd worker —
    // so one fan command stalled every other HTTP request on the device for up
    // to the network timeout. Fire-and-forget is correct: the printer's own
    // report stream is what confirms the change, not the broker ack.
    int id = esp_mqtt_client_publish(s_client, s_request_topic, payload, 0, 0, 0);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t dc_bambu_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_SER);
    nvs_erase_key(h, KEY_CODE);
    nvs_commit(h);
    nvs_close(h);
    if (s_lock) {
        client_teardown(DC_BAMBU_DISABLED);   // live unbind: drop the printer now
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_status.printing = false;
        s_status.error = false;
        s_status.print_state = DC_BAMBU_PRINT_UNKNOWN;
        s_status.progress = -1.0f;
        s_status.last_failure[0] = '\0';
        xSemaphoreGive(s_lock);
    } else {
        memset(&s_cfg, 0, sizeof(s_cfg));
    }
    return ESP_OK;
}

// ---------- filament chamber zones (issue #64, Bambu only) ----------

// Built-in filament types: display name + per-key NVS target override + a default
// (shown as the UI's "default N" hint). PLA/TPU off (a hot chamber hurts PLA);
// PETG/ABS-ASA/PA/PC want warmth for adhesion / reduced warping. ABS and ASA share
// one combined zone (identical chamber needs); to diverge, add a custom ABS or ASA
// profile — it overrides the combined default (see dc_bambu_zone_target).
static const char *const ZONE_NAMES[DC_BAMBU_ZONE_COUNT] = { "PLA", "PETG", "ABS/ASA", "PA", "PC", "TPU" };
static const char *const ZONE_KEYS [DC_BAMBU_ZONE_COUNT] = { "zone_pla", "zone_petg", "zone_abs", "zone_pa", "zone_pc", "zone_tpu" };
//                                                            PLA PETG ABS/ASA PA PC  TPU
static const uint8_t     ZONE_DEF  [DC_BAMBU_ZONE_COUNT] = {  0,  40,   55,    50, 60,  0 };

// Prefix(es) a Bambu filament report is matched against, per built-in. The combined
// zone matches BOTH "ABS…" and "ASA…"; the rest match their own name. NULL-terminated.
static const char *const ZONE_MATCH[DC_BAMBU_ZONE_COUNT][3] = {
    { "PLA",  NULL },
    { "PETG", NULL },
    { "ABS",  "ASA", NULL },   // combined ABS/ASA
    { "PA",   NULL },
    { "PC",   NULL },
    { "TPU",  NULL },
};

// User custom profiles — persisted together as one NVS blob "zone_custom".
typedef struct { char name[12]; uint8_t target_c; } custom_zone_t;

static uint8_t       s_zone_c[DC_BAMBU_ZONE_COUNT];   // built-in target cache
static custom_zone_t s_custom[DC_BAMBU_CUSTOM_MAX];   // custom profile cache
static int           s_custom_n = 0;
static bool          s_zones_loaded = false;

// Load built-in targets (NVS override, else default) + custom profiles into RAM.
static void zones_load(void)
{
    nvs_handle_t h;
    bool have = (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK);
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++) {
        uint8_t v = ZONE_DEF[i];
        if (have) nvs_get_u8(h, ZONE_KEYS[i], &v);   // leaves default if key absent
        s_zone_c[i] = v;
    }
    s_custom_n = 0;
    if (have) {
        size_t len = sizeof s_custom;
        if (nvs_get_blob(h, "zone_custom", s_custom, &len) == ESP_OK)
            s_custom_n = (int)(len / sizeof(custom_zone_t));
        nvs_close(h);
    }
    s_zones_loaded = true;
}

static esp_err_t customs_persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (s_custom_n > 0) err = nvs_set_blob(h, "zone_custom", s_custom, s_custom_n * sizeof(custom_zone_t));
    else                nvs_erase_key(h, "zone_custom");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint8_t dc_bambu_zone_target(const char *filament)
{
    if (!s_zones_loaded) zones_load();
    if (!filament || !filament[0]) return 0;
    // Longest-prefix match over customs + built-in aliases (a custom "PETG-CF" beats
    // "PETG"). Customs are listed FIRST so that on an EQUAL-length tie the custom
    // wins — this is what lets a custom "ABS"/"ASA" override the combined built-in
    // (the matcher keeps the first of equal length). Built-ins expand to their match
    // prefixes, so the combined zone contributes both "ABS" and "ASA".
    const char *names[DC_BAMBU_CUSTOM_MAX + DC_BAMBU_ZONE_COUNT + 2];
    uint8_t     temps[DC_BAMBU_CUSTOM_MAX + DC_BAMBU_ZONE_COUNT + 2];
    int n = 0;
    for (int i = 0; i < s_custom_n; i++) { names[n] = s_custom[i].name; temps[n] = s_custom[i].target_c; n++; }
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++)
        for (int a = 0; ZONE_MATCH[i][a]; a++) { names[n] = ZONE_MATCH[i][a]; temps[n] = s_zone_c[i]; n++; }
    int idx = dc_bambu_zone_match(filament, names, n);
    return idx < 0 ? 0 : temps[idx];
}

int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max)
{
    if (!s_zones_loaded) zones_load();
    int n = 0;
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT && n < max; i++, n++) {
        snprintf(out[n].name, sizeof out[n].name, "%s", ZONE_NAMES[i]);
        out[n].target_c  = s_zone_c[i];
        out[n].default_c = ZONE_DEF[i];
        out[n].custom    = false;
    }
    for (int i = 0; i < s_custom_n && n < max; i++, n++) {
        snprintf(out[n].name, sizeof out[n].name, "%.11s", s_custom[i].name);
        out[n].target_c  = s_custom[i].target_c;
        out[n].default_c = 0;
        out[n].custom    = true;
    }
    return n;
}

esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (target_c > 70) target_c = 70;   // settable ceiling; dc_policy enforces hard cutoffs
    if (!s_zones_loaded) zones_load();

    // Built-in: update its NVS key + cache.
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++) {
        if (strcasecmp(name, ZONE_NAMES[i]) == 0) {
            nvs_handle_t h;
            esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
            if (err != ESP_OK) return err;
            err = nvs_set_u8(h, ZONE_KEYS[i], target_c);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
            if (err == ESP_OK) s_zone_c[i] = target_c;
            return err;
        }
    }
    // Existing custom profile: update in place.
    for (int i = 0; i < s_custom_n; i++)
        if (strcasecmp(name, s_custom[i].name) == 0) {
            s_custom[i].target_c = target_c;
            return customs_persist();
        }
    // New custom profile: append if there's room and the name fits.
    if (s_custom_n >= DC_BAMBU_CUSTOM_MAX) return ESP_ERR_NO_MEM;
    if (strlen(name) >= sizeof s_custom[0].name) return ESP_ERR_INVALID_SIZE;
    memset(&s_custom[s_custom_n], 0, sizeof s_custom[0]);
    snprintf(s_custom[s_custom_n].name, sizeof s_custom[s_custom_n].name, "%.11s", name);
    s_custom[s_custom_n].target_c = target_c;
    s_custom_n++;
    return customs_persist();
}

esp_err_t dc_bambu_zone_remove(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (!s_zones_loaded) zones_load();
    for (int i = 0; i < s_custom_n; i++)
        if (strcasecmp(name, s_custom[i].name) == 0) {
            for (int j = i; j < s_custom_n - 1; j++) s_custom[j] = s_custom[j + 1];
            s_custom_n--;
            return customs_persist();
        }
    return ESP_ERR_NOT_FOUND;   // not a custom (built-ins can't be removed)
}
