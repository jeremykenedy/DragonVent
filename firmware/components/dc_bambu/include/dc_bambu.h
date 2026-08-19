#pragma once
// Bambu Lab LAN MQTT client. Connects to the printer's on-device broker in LAN
// mode (mqtts://<host>:8883, username "bblp", password = LAN access code; the
// self-signed cert has CN=serial while we connect by IP, so cert verification is
// relaxed — LAN read-only), subscribes device/<serial>/report, publishes one
// "pushall" on connect (required on P1/A1 which send deltas), and scans each
// report for bed_temper / chamber_temper. The cached bed temperature feeds the
// AUTO seam exactly as Moonraker does. Read-only: we never send control commands
// to the printer. UNTESTED against real hardware — for community validation (see
// plans/control-source-bambu-ha.md).
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DC_BAMBU_DISABLED,      // no config saved / source not selected
    DC_BAMBU_DISCONNECTED,  // config present, not currently connected
    DC_BAMBU_CONNECTING,
    DC_BAMBU_CONNECTED,     // MQTT+TLS session up, subscribe in flight
    DC_BAMBU_SUBSCRIBED,    // receiving report updates
} dc_bambu_state_t;

typedef enum {
    DC_BAMBU_PRINT_UNKNOWN,
    DC_BAMBU_PRINT_IDLE,
    DC_BAMBU_PRINT_DOWNLOADING,
    DC_BAMBU_PRINT_PREPARING,
    DC_BAMBU_PRINT_PRINTING,
    DC_BAMBU_PRINT_PAUSED,
    DC_BAMBU_PRINT_COMPLETE,
    DC_BAMBU_PRINT_ERROR,
} dc_bambu_print_state_t;

typedef struct {
    char host[64];    // printer IP/hostname; empty string = unconfigured
    char serial[32];  // printer serial (embedded in the MQTT topic path)
    char code[32];    // LAN access code (MQTT password)
} dc_bambu_config_t;

typedef struct {
    dc_bambu_state_t state;
    bool  connected;      // convenience: state == DC_BAMBU_SUBSCRIBED
    float bed_temp;       // bed_temper (°C); NaN until first report
    float bed_target;     // bed_target_temper (°C, the setpoint AUTO triggers on)
    float chamber_temp;   // chamber_temper (°C); NaN if the model has no sensor
    char  filament[16];   // active filament type from AMS / ext spool (e.g. "PETG");
                          // "" if unknown. Feeds filament-based chamber zones.
    bool  printing;       // gcode_state is PREPARE/RUNNING/PAUSE (a print is active);
                          // gates when a filament zone is applied.
    bool  error;          // gcode_state is FAILED (print failed / errored)
    float progress;       // mc_percent / 100, or -1 until the printer reports it
    dc_bambu_print_state_t print_state; // normalized MQTT gcode_state phase
} dc_bambu_status_t;

esp_err_t dc_bambu_start(void);

// Overwrite saved config (NVS). Safe before dc_bambu_start(); the running client
// (once implemented) will reconnect with the new settings.
esp_err_t dc_bambu_set_config(const dc_bambu_config_t *cfg);

// Returns persisted config even when dc_bambu_start() has not been called.
esp_err_t dc_bambu_get_config(dc_bambu_config_t *out);
esp_err_t dc_bambu_get_status(dc_bambu_status_t *out);

// ---- Printer fans -------------------------------------------------------
// Bambu reports fan speeds as quoted 0..15 strings in the status report.
// -1 means "not seen in any report yet".
typedef struct {
    int part;        // cooling_fan_speed   (part cooling, M106 P1)
    int aux;         // big_fan1_speed      (auxiliary,    M106 P2)
    int chamber;     // big_fan2_speed      (chamber/exhaust, M106 P3)
    int heatbreak;   // heatbreak_fan_speed (not settable)
} dc_bambu_fans_t;

// Latest fan speeds seen in the report stream.
esp_err_t dc_bambu_get_fans(dc_bambu_fans_t *out);

// ---- Extra report detail, for display only ------------------------------
// Everything here is read straight out of the status report and never acted
// on. Floats are NAN and ints are -1 when the field has not been seen, so a
// consumer can tell "not reported" from a real zero.
typedef struct {
    float nozzle_temp, nozzle_target;
    float bed_target;
    float chamber_temp;        // flat chamber_temper, else device.ctc.info.temp
    int   layer, total_layers;
    int   remaining_min;
    int   wifi_dbm;
    int   speed_level;         // 1 silent .. 4 ludicrous
    float nozzle_diameter;
    char  nozzle_type[12];
    char  job_name[64];
    int   error_code;
} dc_bambu_detail_t;

esp_err_t dc_bambu_get_detail(dc_bambu_detail_t *out);

// Send one gcode line to the printer's request topic.
//
// NOTE: this is the ONLY control path in the Dragon firmware that writes to a
// printer. Everything else on the Bambu source is read-only, and the family's
// stated contract is that it stays that way. It exists so DragonVent's Fans
// screen can drive the chamber exhaust; callers must treat it as a deliberate
// exception, not a general remote-control API. `line` must not contain a
// newline; one is appended.
esp_err_t dc_bambu_send_gcode(const char *line);

// Reason string from the printer's last gcode_line acknowledgement, or "" if
// the last one succeeded / none has been sent. Bambu firmware rejects
// third-party gcode with result "failed" + reason "mqtt message verify failed";
// without surfacing that, a fan control reports success while the printer
// silently discards every command.
void dc_bambu_last_gcode_error(char *out, size_t len);

// Wipe saved Bambu config (factory reset).
esp_err_t dc_bambu_clear_config(void);

// --- LAN discovery (SSDP), on demand only -----------------------------------
// Bambu printers announce over SSDP on the LAN: multicast 239.255.255.250, UDP
// ports 1990 AND 2021 (NOT the standard 1900). Real printers emit unsolicited
// NOTIFY (~5 s); some emulators only answer an M-SEARCH — so a scan does both
// (joins the group to hear NOTIFY, and probes with M-SEARCH). The serial comes
// from the USN header, the IP from the datagram source (Location is a bare-IP
// cross-check), model/name from Dev*.bambu.com headers.
//
// Discovery is USER-INITIATED, never continuous: there is no background listener
// and no socket held open. A scan is a short-lived task (a few seconds) that the
// UI kicks off only when the operator is configuring a Bambu printer. It fills in
// host+serial for the setup form; it never auto-connects.
#define DC_BAMBU_DISCOVER_MAX 8

typedef struct {
    char     host[64];    // printer IP (datagram source; Location fallback)
    char     serial[32];  // USN
    char     model[24];   // DevModel.bambu.com code (e.g. "BL-P001", "C12")
    char     name[32];    // DevName.bambu.com friendly name
    uint32_t age_s;       // seconds since last seen (fresh = small)
} dc_bambu_found_t;

// Kick off a one-shot scan (spawns a short-lived task that opens a socket, probes
// for a few seconds, records what it finds, then closes the socket and exits).
// Idempotent while a scan is running. Returns ESP_OK if started or already active.
esp_err_t dc_bambu_scan_start(void);

// True while a scan task is in progress (the UI polls until it clears).
bool dc_bambu_scanning(void);

// Snapshot printers found by the most recent scan(s), freshest first, de-duped by
// serial. Returns the count written (<= max).
int dc_bambu_discover_get(dc_bambu_found_t *out, int max);

// --- Filament chamber zones (Bambu only, issue #64) -------------------------
// Maps the active filament type to a chamber target so a Bambu print gets a warm
// chamber (e.g. PETG -> 40 C) without any bed-threshold AUTO. Klipper doesn't use
// this — it drives the chamber via M141/M191. A zone target of 0 = "no zone" (off).
//
// There are 6 BUILT-IN filament types (fixed defaults, editable target) — PLA, PETG,
// the combined ABS/ASA, PA, PC, TPU — plus up to DC_BAMBU_CUSTOM_MAX USER profiles the
// operator can add/remove for filaments not in the built-in set (PCTG, PVA, ...), or to
// override a built-in for a specific type (e.g. a custom "ASA" beats the ABS/ASA zone).
// get_all returns the built-ins first, then customs.
#define DC_BAMBU_ZONE_COUNT  6                                        // built-in types
#define DC_BAMBU_CUSTOM_MAX  8                                        // user profiles
#define DC_BAMBU_ZONE_MAX    (DC_BAMBU_ZONE_COUNT + DC_BAMBU_CUSTOM_MAX)

typedef struct {
    char    name[12];    // filament type ("PETG", or a custom name like "PCTG")
    uint8_t target_c;    // chamber target (°C); 0 = no zone / off
    uint8_t default_c;   // built-in default (for the UI's "default N" hint); 0 for customs
    bool    custom;      // true = a user-added profile (removable)
} dc_bambu_zone_t;

// Resolve the chamber target for a filament type string. Longest case-insensitive
// prefix match over built-ins + customs (so a custom "PETG-CF" beats "PETG"). 0 = none.
uint8_t dc_bambu_zone_target(const char *filament);

// Fill `out` (capacity `max`) with built-in zones then custom profiles; returns the
// number written (<= DC_BAMBU_ZONE_MAX).
int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max);

// Set/update a zone by name (case-insensitive). A built-in name updates its target;
// an unknown name ADDS a custom profile (up to DC_BAMBU_CUSTOM_MAX). Persists.
esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c);

// Remove a custom profile by name (built-ins cannot be removed). Persists.
esp_err_t dc_bambu_zone_remove(const char *name);
