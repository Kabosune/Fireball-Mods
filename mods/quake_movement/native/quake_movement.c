/*
 * quake_movement.c — Native C11 Tier 2 Fireball-Mods plugin `quake_movement.fbplugin.dll`.
 *
 * Pure C11, zero third-party dependencies, compiled with `/std:c11 /W4 /WX`
 * strictly against Fireball-Core's public C11 SDK (`fireball_plugin.h`,
 * `fireball_menu.h`, `fireball_console.h`).
 *
 * Features:
 *   - Live-switchable on-foot movement presets: Quake VQ3, Quake CPMA, Half-Life,
 *     Apex Legends, Deadlock, and Custom (via decoupled `qm_physics.c` in Z-up meters).
 *   - Exports `fb_plugin_query`, `fb_plugin_init`, `fb_plugin_shutdown`, plus optional
 *     `fb_plugin_register_menu` and `fb_plugin_register_console` with graceful v1 fallback.
 *   - Guarded Tier 2 title memory access (`fb.player_pos`, `fb.player_vel`,
 *     `fb.player_flags`, `fb.camera_basis`) with safe no-op degradation when symbols are
 *     unresolved or when the player is dead, ragdolled, or inside a vehicle.
 *   - Conflict-free default bindings:
 *       toggle_movement : F6        / RB + A
 *       cycle_preset    : Ctrl + P  / RB + Y
 *       dash            : Shift + Q / RB + B
 *   - Console commands:
 *       qm toggle | on | off
 *       qm preset <vq3|cpma|halflife|apex|deadlock|custom>
 *       qm set <param> <value>
 *       qm status
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fireball_plugin.h"
#include "fireball_menu.h"
#include "fireball_console.h"
#include "qm_physics.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QM_MOD_ID    "community.quake_movement"
#define QM_PLUGIN_ID "quake_movement"
#define QM_VERSION   "0.1.0"

/* Default player flag bitmasks (overridable via mod.toml once live VIctory scan confirms) */
#define QM_FLAG_ON_GROUND_DEFAULT   0x00000001u
#define QM_FLAG_CROUCHED_DEFAULT    0x00000002u
#define QM_FLAG_IN_VEHICLE_DEFAULT  0x00000004u
#define QM_FLAG_RAGDOLL_DEFAULT     0x00000008u
#define QM_FLAG_DEAD_DEFAULT        0x00000010u

/* Default conflict-free Win32 virtual keys */
#define QM_KEY_TOGGLE_DEFAULT       0x75u /* F6 */
#define QM_KEY_CYCLE_PRESET_DEFAULT 0x50u /* 'P' (with Ctrl) */
#define QM_KEY_DASH_DEFAULT         0x51u /* 'Q' (with Shift) */

typedef enum qm_cam_layout {
    QM_CAM_MAT34       = 0, /* MAT34V: row 1 (+0x10, +0x14) is forward XY */
    QM_CAM_YAW_F32     = 1, /* Single float yaw (rad) at +0x00 */
    QM_CAM_FORWARD_V3  = 2  /* float[3] forward unit vector at +0x00 */
} qm_cam_layout_t;

static const char* const k_qm_preset_labels[QM_PRESET_COUNT] = {
    "Quake VQ3",
    "Quake CPMA",
    "Half-Life",
    "Apex Legends",
    "Deadlock",
    "Custom"
};

typedef struct qm_plugin_state {
    fb_core_services_v1_t core;
    int                   initialized;

    /* Immutable / config */
    char                  player_pos_symbol[128];
    char                  player_vel_symbol[128];
    char                  player_flags_symbol[128];
    char                  camera_basis_symbol[128];
    qm_cam_layout_t       camera_layout;

    uint32_t              flag_on_ground;
    uint32_t              flag_crouched;
    uint32_t              flag_in_vehicle;
    uint32_t              flag_ragdoll;
    uint32_t              flag_dead;

    uint32_t              key_toggle;
    uint32_t              key_cycle_preset;
    uint32_t              key_dash;

    /* Resolved Tier 2 title addresses */
    int                   has_tier2_mem;
    uint64_t              player_pos_va;
    uint64_t              player_vel_va;
    uint64_t              player_flags_va;
    uint64_t              camera_basis_va;

    /* Active movement configuration & state */
    int                   enabled;
    qm_preset_id_t        preset;
    qm_params_t           active_params;
    qm_params_t           custom_params;
    qm_state_t            phys;

    /* Live input state (updated by FB_EVENT_INPUT_EVENT + actions) */
    int                   key_w;
    int                   key_s;
    int                   key_a;
    int                   key_d;
    int                   key_jump;
    int                   key_crouch;
    int                   pending_jump_press;
    int                   pending_crouch_press;
    int                   pending_dash_press;

    /* Telemetry & safety counters */
    uint64_t              frame_count;
    uint64_t              vel_writes;
    uint64_t              guard_skips;
    uint32_t              last_flags;
    float                 last_cam_yaw;
    int                   last_blocked_guard;

    /* Optional Mod Menu & Console APIs */
    int                   menu_registered;
    fb_menu_api_v1_t      menu;
    int                   console_registered;
    fb_console_api_v1_t   console;

    /* Menu sync shadows (avoid redundant set_setting calls) */
    int                   sync_init;
    int                   synced_enabled;
    int32_t               synced_preset;
    int                   synced_auto_bhop;
} qm_plugin_state_t;

static qm_plugin_state_t g_qm;

/* ------------------------------------------------------------------------- */
/* Logging & string helpers                                                  */
/* ------------------------------------------------------------------------- */

static void qm_log(int level, const char* msg) {
    if (g_qm.core.log && msg) {
        g_qm.core.log(QM_MOD_ID, level, msg);
    }
}

static int qm_ci_eq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

/* ------------------------------------------------------------------------- */
/* Flat JSON config parsing helpers                                          */
/* ------------------------------------------------------------------------- */

static const char* qm_json_find(const char* json, const char* key) {
    char pat[128];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) {
        return NULL;
    }
    const char* p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p += (size_t)n;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != ':') {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static void qm_json_u32(const char* json, const char* key, uint32_t* io) {
    const char* v = qm_json_find(json, key);
    if (v && isdigit((unsigned char)*v)) {
        char* end = NULL;
        unsigned long long parsed = strtoull(v, &end, 0);
        if (end != v) {
            *io = (uint32_t)parsed;
        }
    }
}

static void qm_json_float(const char* json, const char* key, float* io) {
    const char* v = qm_json_find(json, key);
    if (v && (isdigit((unsigned char)*v) || *v == '-' || *v == '.')) {
        char* end = NULL;
        double parsed = strtod(v, &end);
        if (end != v && isfinite(parsed)) {
            *io = (float)parsed;
        }
    }
}

static void qm_json_bool(const char* json, const char* key, int* io) {
    const char* v = qm_json_find(json, key);
    if (v && strncmp(v, "true", 4) == 0) {
        *io = 1;
    } else if (v && strncmp(v, "false", 5) == 0) {
        *io = 0;
    }
}

static void qm_json_str(const char* json, const char* key, char* out, size_t cap) {
    const char* v = qm_json_find(json, key);
    if (!v || *v != '"' || cap < 2u) {
        return;
    }
    ++v;
    char tmp[256];
    size_t len = 0u;
    while (*v && *v != '"' && len + 1u < sizeof(tmp)) {
        if (*v == '\\' && v[1] != '\0') {
            ++v;
        }
        tmp[len++] = *v++;
    }
    if (*v != '"') {
        return;
    }
    tmp[len] = '\0';
    snprintf(out, cap, "%s", tmp);
}

/* ------------------------------------------------------------------------- */
/* Guarded Tier 2 Title Memory I/O                                           */
/* ------------------------------------------------------------------------- */

static int qm_read_u32(uint64_t va, uint32_t* out) {
    if (!g_qm.has_tier2_mem || va == 0ull || !g_qm.core.title_mem_read_u32) {
        return FB_ERR_UNSUPPORTED;
    }
    return g_qm.core.title_mem_read_u32(va, out);
}

static int qm_read_f32(uint64_t va, float* out) {
    uint32_t bits = 0u;
    int rc = qm_read_u32(va, &bits);
    if (rc != FB_OK) {
        return rc;
    }
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    if (!isfinite(f)) {
        return FB_ERR_MEM_FAULT;
    }
    *out = f;
    return FB_OK;
}

static int qm_write_f32(uint64_t va, float val) {
    if (!g_qm.has_tier2_mem || va == 0ull || !g_qm.core.title_mem_write_u32 || !isfinite(val)) {
        return FB_ERR_UNSUPPORTED;
    }
    uint32_t bits = 0u;
    memcpy(&bits, &val, sizeof(bits));
    return g_qm.core.title_mem_write_u32(va, bits);
}

static int qm_read_vec3(uint64_t va, float out[3]) {
    if (va == 0ull) {
        return FB_ERR_NOT_FOUND;
    }
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (qm_read_f32(va + 0ull, &x) != FB_OK ||
        qm_read_f32(va + 4ull, &y) != FB_OK ||
        qm_read_f32(va + 8ull, &z) != FB_OK) {
        return FB_ERR_MEM_FAULT;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return FB_OK;
}

static int qm_write_vec3(uint64_t va, const float v[3]) {
    if (va == 0ull || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) {
        return FB_ERR_INVALID_ARG;
    }
    if (qm_write_f32(va + 0ull, v[0]) != FB_OK ||
        qm_write_f32(va + 4ull, v[1]) != FB_OK ||
        qm_write_f32(va + 8ull, v[2]) != FB_OK) {
        return FB_ERR_MEM_FAULT;
    }
    return FB_OK;
}

/* Read camera yaw (rad) where 0 = +Y North, +CCW around +Z */
static int qm_read_camera_yaw(float* out_yaw) {
    if (g_qm.camera_basis_va == 0ull) {
        return 0;
    }
    if (g_qm.camera_layout == QM_CAM_YAW_F32) {
        float yaw = 0.0f;
        if (qm_read_f32(g_qm.camera_basis_va, &yaw) == FB_OK) {
            *out_yaw = yaw;
            return 1;
        }
        return 0;
    }
    uint64_t fwd_off = (g_qm.camera_layout == QM_CAM_MAT34) ? 0x10ull : 0x00ull;
    float fx = 0.0f, fy = 0.0f;
    if (qm_read_f32(g_qm.camera_basis_va + fwd_off + 0ull, &fx) != FB_OK ||
        qm_read_f32(g_qm.camera_basis_va + fwd_off + 4ull, &fy) != FB_OK) {
        return 0;
    }
    float len = hypotf(fx, fy);
    if (len < 1e-4f) {
        return 0;
    }
    /* forward = (-sin(yaw), cos(yaw)) => yaw = atan2(-fx, fy) */
    *out_yaw = atan2f(-fx / len, fy / len);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Menu synchronization helpers (`notify = 0` prevents feedback loops)       */
/* ------------------------------------------------------------------------- */

static void qm_sync_menu_bool(const char* sid, int val) {
    if (!g_qm.menu_registered || !g_qm.menu.set_setting) {
        return;
    }
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = (uint32_t)FB_SETTING_BOOL;
    v.v.bool_val = val ? 1 : 0;
    (void)g_qm.menu.set_setting(QM_MOD_ID, sid, &v, 0);
}

static void qm_sync_menu_enum(const char* sid, int32_t val) {
    if (!g_qm.menu_registered || !g_qm.menu.set_setting) {
        return;
    }
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = (uint32_t)FB_SETTING_ENUM;
    v.v.enum_val = val;
    (void)g_qm.menu.set_setting(QM_MOD_ID, sid, &v, 0);
}

static void qm_sync_menu_float(const char* sid, float val) {
    if (!g_qm.menu_registered || !g_qm.menu.set_setting) {
        return;
    }
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = (uint32_t)FB_SETTING_FLOAT;
    v.v.float_val = (double)val;
    (void)g_qm.menu.set_setting(QM_MOD_ID, sid, &v, 0);
}

static void qm_sync_custom_sliders_to_menu(void) {
    if (!g_qm.menu_registered) {
        return;
    }
    qm_sync_menu_float("custom_max_speed", g_qm.custom_params.max_speed);
    qm_sync_menu_float("custom_ground_accel", g_qm.custom_params.ground_accel);
    qm_sync_menu_float("custom_air_accel", g_qm.custom_params.air_accel);
    qm_sync_menu_float("custom_air_wish_cap", g_qm.custom_params.air_wish_cap);
    qm_sync_menu_float("custom_friction", g_qm.custom_params.friction);
    qm_sync_menu_float("custom_gravity", g_qm.custom_params.gravity);
    qm_sync_menu_float("custom_jump_speed", g_qm.custom_params.jump_speed);
    qm_sync_menu_float("custom_slide_boost", g_qm.custom_params.slide_boost);
    qm_sync_menu_float("custom_dash_impulse", g_qm.custom_params.dash_impulse);
}

static void qm_sync_core_menu_state(void) {
    if (!g_qm.menu_registered) {
        return;
    }
    if (!g_qm.sync_init || g_qm.synced_enabled != g_qm.enabled) {
        g_qm.synced_enabled = g_qm.enabled;
        qm_sync_menu_bool("enabled", g_qm.enabled);
    }
    if (!g_qm.sync_init || g_qm.synced_preset != (int32_t)g_qm.preset) {
        g_qm.synced_preset = (int32_t)g_qm.preset;
        qm_sync_menu_enum("preset", (int32_t)g_qm.preset);
    }
    if (!g_qm.sync_init || g_qm.synced_auto_bhop != g_qm.active_params.auto_bhop) {
        g_qm.synced_auto_bhop = g_qm.active_params.auto_bhop;
        qm_sync_menu_bool("auto_bhop", g_qm.active_params.auto_bhop);
    }
    g_qm.sync_init = 1;
}

/* ------------------------------------------------------------------------- */
/* State transitions                                                         */
/* ------------------------------------------------------------------------- */

static void qm_apply_preset(qm_preset_id_t preset, int log_change) {
    if ((int)preset < 0 || preset >= QM_PRESET_COUNT) {
        return;
    }
    g_qm.preset = preset;
    if (preset == QM_PRESET_CUSTOM) {
        g_qm.active_params = g_qm.custom_params;
    } else {
        qm_preset_defaults(preset, &g_qm.active_params);
    }
    /* Clamp stamina to new preset's max */
    if (g_qm.active_params.dash_max_stamina > 0) {
        float max_s = (float)g_qm.active_params.dash_max_stamina;
        if (g_qm.phys.stamina <= 0.0f || g_qm.phys.stamina > max_s) {
            g_qm.phys.stamina = max_s;
        }
    }
    qm_sync_core_menu_state();
    if (log_change) {
        char msg[160];
        snprintf(msg, sizeof(msg), "[quake_movement] Preset -> %s (max=%.1f m/s, air_accel=%.1f, bhop=%s)",
                 qm_preset_name(preset),
                 (double)g_qm.active_params.max_speed,
                 (double)g_qm.active_params.air_accel,
                 g_qm.active_params.auto_bhop ? "on" : "off");
        qm_log(FB_LOG_INFO, msg);
    }
}

static void qm_set_enabled(int enabled, int log_change) {
    int next = enabled ? 1 : 0;
    if (next == g_qm.enabled) {
        return;
    }
    g_qm.enabled = next;
    g_qm.phys.sliding = 0;
    g_qm.phys.dash_timer = 0.0f;
    g_qm.phys.dash_active_window = 0;
    g_qm.phys.coyote_timer = 0.0f;
    g_qm.phys.jump_buffer_timer = 0.0f;
    g_qm.phys.tap_strafe_timer = 0.0f;
    if (g_qm.active_params.dash_max_stamina > 0) {
        g_qm.phys.stamina = (float)g_qm.active_params.dash_max_stamina;
    }
    qm_sync_core_menu_state();
    if (log_change) {
        char msg[160];
        snprintf(msg, sizeof(msg), "[quake_movement] Movement %s (preset=%s)",
                 g_qm.enabled ? "ENABLED" : "DISABLED",
                 qm_preset_name(g_qm.preset));
        qm_log(FB_LOG_INFO, msg);
    }
}

static void qm_cycle_preset(void) {
    qm_preset_id_t next = (qm_preset_id_t)(((int)g_qm.preset + 1) % (int)QM_PRESET_COUNT);
    qm_apply_preset(next, 1);
}

static void qm_trigger_dash(void) {
    if (g_qm.enabled) {
        g_qm.pending_dash_press = 1;
    }
}

/* Set a named parameter on active_params (and custom_params when in Custom mode) */
static int qm_set_param_by_name(const char* name, float val) {
    if (!name || !isfinite(val)) {
        return 0;
    }
    qm_params_t* dst = &g_qm.active_params;
    qm_params_t* cust = (g_qm.preset == QM_PRESET_CUSTOM) ? &g_qm.custom_params : NULL;

    if (qm_ci_eq(name, "max_speed")) {
        float v = (val < 1.0f) ? 1.0f : ((val > 35.0f) ? 35.0f : val);
        dst->max_speed = v;
        if (cust) { cust->max_speed = v; qm_sync_menu_float("custom_max_speed", v); }
        return 1;
    }
    if (qm_ci_eq(name, "ground_accel")) {
        float v = (val < 0.5f) ? 0.5f : ((val > 60.0f) ? 60.0f : val);
        dst->ground_accel = v;
        if (cust) { cust->ground_accel = v; qm_sync_menu_float("custom_ground_accel", v); }
        return 1;
    }
    if (qm_ci_eq(name, "air_accel")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 250.0f) ? 250.0f : val);
        dst->air_accel = v;
        if (cust) { cust->air_accel = v; qm_sync_menu_float("custom_air_accel", v); }
        return 1;
    }
    if (qm_ci_eq(name, "air_wish_cap")) {
        float v = (val < 0.25f) ? 0.25f : ((val > 25.0f) ? 25.0f : val);
        dst->air_wish_cap = v;
        if (cust) { cust->air_wish_cap = v; qm_sync_menu_float("custom_air_wish_cap", v); }
        return 1;
    }
    if (qm_ci_eq(name, "air_control")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 30.0f) ? 30.0f : val);
        dst->air_control = v;
        if (cust) { cust->air_control = v; }
        return 1;
    }
    if (qm_ci_eq(name, "friction")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 25.0f) ? 25.0f : val);
        dst->friction = v;
        if (cust) { cust->friction = v; qm_sync_menu_float("custom_friction", v); }
        return 1;
    }
    if (qm_ci_eq(name, "stop_speed")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 15.0f) ? 15.0f : val);
        dst->stop_speed = v;
        if (cust) { cust->stop_speed = v; }
        return 1;
    }
    if (qm_ci_eq(name, "gravity")) {
        float v = (val < 1.0f) ? 1.0f : ((val > 50.0f) ? 50.0f : val);
        dst->gravity = v;
        if (cust) { cust->gravity = v; qm_sync_menu_float("custom_gravity", v); }
        return 1;
    }
    if (qm_ci_eq(name, "jump_speed")) {
        float v = (val < 1.0f) ? 1.0f : ((val > 25.0f) ? 25.0f : val);
        dst->jump_speed = v;
        if (cust) { cust->jump_speed = v; qm_sync_menu_float("custom_jump_speed", v); }
        return 1;
    }
    if (qm_ci_eq(name, "coyote_time")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 0.5f) ? 0.5f : val);
        dst->coyote_time_sec = v;
        if (cust) { cust->coyote_time_sec = v; }
        return 1;
    }
    if (qm_ci_eq(name, "jump_buffer")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 0.5f) ? 0.5f : val);
        dst->jump_buffer_sec = v;
        if (cust) { cust->jump_buffer_sec = v; }
        return 1;
    }
    if (qm_ci_eq(name, "auto_bhop")) {
        int b = (val != 0.0f) ? 1 : 0;
        dst->auto_bhop = b;
        if (cust) { cust->auto_bhop = b; }
        qm_sync_core_menu_state();
        return 1;
    }
    if (qm_ci_eq(name, "slide_enabled")) {
        int b = (val != 0.0f) ? 1 : 0;
        dst->slide_enabled = b;
        if (cust) { cust->slide_enabled = b; }
        return 1;
    }
    if (qm_ci_eq(name, "slide_boost")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 20.0f) ? 20.0f : val);
        dst->slide_boost = v;
        if (cust) { cust->slide_boost = v; qm_sync_menu_float("custom_slide_boost", v); }
        return 1;
    }
    if (qm_ci_eq(name, "slide_friction")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 10.0f) ? 10.0f : val);
        dst->slide_friction = v;
        if (cust) { cust->slide_friction = v; }
        return 1;
    }
    if (qm_ci_eq(name, "slide_slope_accel")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 40.0f) ? 40.0f : val);
        dst->slide_slope_accel = v;
        if (cust) { cust->slide_slope_accel = v; }
        return 1;
    }
    if (qm_ci_eq(name, "slide_jump_boost")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 15.0f) ? 15.0f : val);
        dst->slide_jump_boost = v;
        if (cust) { cust->slide_jump_boost = v; }
        return 1;
    }
    if (qm_ci_eq(name, "tap_strafe_enabled")) {
        int b = (val != 0.0f) ? 1 : 0;
        dst->tap_strafe_enabled = b;
        if (cust) { cust->tap_strafe_enabled = b; }
        return 1;
    }
    if (qm_ci_eq(name, "tap_strafe_window")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 1.5f) ? 1.5f : val);
        dst->tap_strafe_window_sec = v;
        if (cust) { cust->tap_strafe_window_sec = v; }
        return 1;
    }
    if (qm_ci_eq(name, "tap_strafe_strength")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 2.0f) ? 2.0f : val);
        dst->tap_strafe_strength = v;
        if (cust) { cust->tap_strafe_strength = v; }
        return 1;
    }
    if (qm_ci_eq(name, "dash_enabled")) {
        int b = (val != 0.0f) ? 1 : 0;
        dst->dash_enabled = b;
        if (cust) { cust->dash_enabled = b; }
        return 1;
    }
    if (qm_ci_eq(name, "dash_impulse")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 30.0f) ? 30.0f : val);
        dst->dash_impulse = v;
        if (cust) { cust->dash_impulse = v; qm_sync_menu_float("custom_dash_impulse", v); }
        return 1;
    }
    if (qm_ci_eq(name, "dash_recharge")) {
        float v = (val < 0.25f) ? 0.25f : ((val > 30.0f) ? 30.0f : val);
        dst->dash_recharge_sec = v;
        if (cust) { cust->dash_recharge_sec = v; }
        return 1;
    }
    if (qm_ci_eq(name, "dash_jump_boost")) {
        float v = (val < 0.0f) ? 0.0f : ((val > 20.0f) ? 20.0f : val);
        dst->dash_jump_boost = v;
        if (cust) { cust->dash_jump_boost = v; }
        return 1;
    }
    if (qm_ci_eq(name, "wall_bounce_enabled")) {
        int b = (val != 0.0f) ? 1 : 0;
        dst->wall_bounce_enabled = b;
        if (cust) { cust->wall_bounce_enabled = b; }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Event Handling (FB_EVENT_INPUT_EVENT & FB_EVENT_FRAME_BEGIN)              */
/* ------------------------------------------------------------------------- */

static int qm_handle_input(const fb_input_event_v1_t* ev) {
    if (!ev) {
        return 0;
    }
    if (ev->device_kind == FB_DEV_KEYBOARD) {
        int pressed = (ev->action == FB_INPUT_PRESS);
        int held = (ev->action == FB_INPUT_PRESS || ev->action == FB_INPUT_REPEAT);
        uint32_t mods = ev->modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT);

        /* v1 fallback hotkeys when Mod Menu is not registered */
        if (!g_qm.menu_registered && pressed) {
            if (mods == FB_MOD_NONE && ev->code == g_qm.key_toggle) {
                qm_set_enabled(!g_qm.enabled, 1);
                return 1;
            }
            if (mods == FB_MOD_CTRL && ev->code == g_qm.key_cycle_preset) {
                qm_cycle_preset();
                return 1;
            }
            if (mods == FB_MOD_SHIFT && ev->code == g_qm.key_dash) {
                qm_trigger_dash();
                return g_qm.enabled ? 1 : 0;
            }
        }

        /* Track movement inputs passively (never consume WASD/Space/Ctrl) */
        switch (ev->code) {
        case 'W': g_qm.key_w = held; break;
        case 'S': g_qm.key_s = held; break;
        case 'A': g_qm.key_a = held; break;
        case 'D': g_qm.key_d = held; break;
        case VK_SPACE:
            if (pressed && !g_qm.key_jump) {
                g_qm.pending_jump_press = 1;
            }
            g_qm.key_jump = held;
            break;
        case VK_CONTROL:
        case VK_LCONTROL:
        case 'C':
            if (pressed && !g_qm.key_crouch) {
                g_qm.pending_crouch_press = 1;
            }
            g_qm.key_crouch = held;
            break;
        default:
            break;
        }
    }
    return 0;
}

static void qm_handle_frame(const fb_frame_event_v1_t* fe) {
    g_qm.frame_count = fe ? fe->frame_index : (g_qm.frame_count + 1ull);
    double dt = fe ? fe->dt_sec : (1.0 / 60.0);
    if (!isfinite(dt) || dt <= 0.0 || dt > 0.25) {
        dt = 1.0 / 60.0;
    }

    int jump_pressed = g_qm.pending_jump_press;
    int crouch_pressed = g_qm.pending_crouch_press;
    int dash_pressed = g_qm.pending_dash_press;
    g_qm.pending_jump_press = 0;
    g_qm.pending_crouch_press = 0;
    g_qm.pending_dash_press = 0;

    if (!g_qm.enabled) {
        g_qm.last_blocked_guard = 0;
        return;
    }

    /* Require resolved velocity + player_flags symbols before touching title memory */
    if (!g_qm.has_tier2_mem || g_qm.player_vel_va == 0ull || g_qm.player_flags_va == 0ull) {
        g_qm.last_blocked_guard = 0;
        return;
    }

    uint32_t flags = 0u;
    if (qm_read_u32(g_qm.player_flags_va, &flags) != FB_OK) {
        g_qm.guard_skips += 1ull;
        g_qm.last_blocked_guard = 1;
        return;
    }
    g_qm.last_flags = flags;

    /* Safety guard: NEVER override movement when player is dead, ragdolled, or in a vehicle */
    uint32_t unsafe_mask = g_qm.flag_dead | g_qm.flag_ragdoll | g_qm.flag_in_vehicle;
    if ((flags & unsafe_mask) != 0u) {
        g_qm.guard_skips += 1ull;
        g_qm.last_blocked_guard = 1;
        g_qm.phys.sliding = 0;
        g_qm.phys.dash_timer = 0.0f;
        g_qm.phys.dash_active_window = 0;
        return;
    }
    g_qm.last_blocked_guard = 0;

    float cur_vel[3] = { 0.0f, 0.0f, 0.0f };
    if (qm_read_vec3(g_qm.player_vel_va, cur_vel) != FB_OK) {
        g_qm.guard_skips += 1ull;
        return;
    }
    g_qm.phys.vel[0] = cur_vel[0];
    g_qm.phys.vel[1] = cur_vel[1];
    g_qm.phys.vel[2] = cur_vel[2];

    if (g_qm.player_pos_va != 0ull) {
        float cur_pos[3] = { 0.0f, 0.0f, 0.0f };
        if (qm_read_vec3(g_qm.player_pos_va, cur_pos) == FB_OK) {
            g_qm.phys.pos[0] = cur_pos[0];
            g_qm.phys.pos[1] = cur_pos[1];
            g_qm.phys.pos[2] = cur_pos[2];
        }
    }

    float cam_yaw = g_qm.last_cam_yaw;
    if (qm_read_camera_yaw(&cam_yaw)) {
        g_qm.last_cam_yaw = cam_yaw;
    }

    /* Assemble input (ignore movement keys while Mod Menu or Console overlay is open) */
    int ui_open = 0;
    if (g_qm.menu_registered && g_qm.menu.is_menu_open && g_qm.menu.is_menu_open()) {
        ui_open = 1;
    }
    if (g_qm.console_registered && g_qm.console.is_console_open && g_qm.console.is_console_open()) {
        ui_open = 1;
    }

    qm_input_t in;
    memset(&in, 0, sizeof(in));
    in.dt_sec = (float)dt;
    in.cam_yaw_rad = g_qm.last_cam_yaw;
    in.on_ground = ((flags & g_qm.flag_on_ground) != 0u) ? 1 : 0;
    in.slope_normal[0] = 0.0f;
    in.slope_normal[1] = 0.0f;
    in.slope_normal[2] = 1.0f;

    if (!ui_open) {
        in.wish_forward = (float)(g_qm.key_w ? 1 : 0) - (float)(g_qm.key_s ? 1 : 0);
        in.wish_right = (float)(g_qm.key_d ? 1 : 0) - (float)(g_qm.key_a ? 1 : 0);
        in.jump_held = g_qm.key_jump;
        in.jump_pressed = jump_pressed;
        in.crouch_held = g_qm.key_crouch || (((flags & g_qm.flag_crouched) != 0u) ? 1 : 0);
        in.crouch_pressed = crouch_pressed;
        in.dash_pressed = dash_pressed;
    }

    qm_step(&g_qm.active_params, &in, &g_qm.phys, &g_qm.phys);

    if (qm_write_vec3(g_qm.player_vel_va, g_qm.phys.vel) == FB_OK) {
        g_qm.vel_writes += 1ull;
    }
}

static int qm_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_qm.initialized) {
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_INPUT_EVENT) {
        return qm_handle_input((const fb_input_event_v1_t*)payload);
    }
    if (event_id == (uint32_t)FB_EVENT_FRAME_BEGIN) {
        qm_handle_frame((const fb_frame_event_v1_t*)payload);
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_SHUTDOWN) {
        fb_plugin_shutdown();
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Optional Fireball Mod Menu Integration (`fb_plugin_register_menu`)        */
/* ------------------------------------------------------------------------- */

static void qm_menu_action_cb(const char* action_id, int active, void* user_ctx) {
    (void)user_ctx;
    if (!action_id || !active) {
        return;
    }
    if (strcmp(action_id, "toggle_movement") == 0) {
        qm_set_enabled(!g_qm.enabled, 1);
    } else if (strcmp(action_id, "cycle_preset") == 0) {
        qm_cycle_preset();
    } else if (strcmp(action_id, "dash") == 0) {
        qm_trigger_dash();
    }
}

static void qm_menu_setting_cb(const char* setting_id, const fb_setting_value_t* val, void* user_ctx) {
    (void)user_ctx;
    if (!setting_id || !val) {
        return;
    }
    if (strcmp(setting_id, "enabled") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        qm_set_enabled(val->v.bool_val, 1);
        return;
    }
    if (strcmp(setting_id, "preset") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        if (val->v.enum_val >= 0 && val->v.enum_val < (int32_t)QM_PRESET_COUNT) {
            qm_apply_preset((qm_preset_id_t)val->v.enum_val, 1);
        }
        return;
    }
    if (strcmp(setting_id, "auto_bhop") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        int b = val->v.bool_val ? 1 : 0;
        g_qm.active_params.auto_bhop = b;
        g_qm.custom_params.auto_bhop = b;
        g_qm.synced_auto_bhop = b;
        return;
    }
    if (val->type == (uint32_t)FB_SETTING_FLOAT) {
        float fv = (float)val->v.float_val;
        if (strcmp(setting_id, "custom_max_speed") == 0) {
            g_qm.custom_params.max_speed = fv;
        } else if (strcmp(setting_id, "custom_ground_accel") == 0) {
            g_qm.custom_params.ground_accel = fv;
        } else if (strcmp(setting_id, "custom_air_accel") == 0) {
            g_qm.custom_params.air_accel = fv;
        } else if (strcmp(setting_id, "custom_air_wish_cap") == 0) {
            g_qm.custom_params.air_wish_cap = fv;
        } else if (strcmp(setting_id, "custom_friction") == 0) {
            g_qm.custom_params.friction = fv;
        } else if (strcmp(setting_id, "custom_gravity") == 0) {
            g_qm.custom_params.gravity = fv;
        } else if (strcmp(setting_id, "custom_jump_speed") == 0) {
            g_qm.custom_params.jump_speed = fv;
        } else if (strcmp(setting_id, "custom_slide_boost") == 0) {
            g_qm.custom_params.slide_boost = fv;
        } else if (strcmp(setting_id, "custom_dash_impulse") == 0) {
            g_qm.custom_params.dash_impulse = fv;
        }
        if (g_qm.preset == QM_PRESET_CUSTOM) {
            g_qm.active_params = g_qm.custom_params;
        }
    }
}

static void qm_menu_button_cb(const char* setting_id, void* user_ctx) {
    (void)user_ctx;
    if (!setting_id) {
        return;
    }
    if (strcmp(setting_id, "reset_peak_btn") == 0) {
        g_qm.phys.peak_speed = g_qm.phys.horiz_speed;
    } else if (strcmp(setting_id, "reset_custom_btn") == 0) {
        qm_preset_defaults(QM_PRESET_CUSTOM, &g_qm.custom_params);
        if (g_qm.preset == QM_PRESET_CUSTOM) {
            g_qm.active_params = g_qm.custom_params;
        }
        qm_sync_custom_sliders_to_menu();
        qm_sync_core_menu_state();
    }
}

static void qm_menu_status_cb(const char* setting_id, char* out_buf, size_t out_cap, void* user_ctx) {
    (void)user_ctx;
    if (!out_buf || out_cap == 0u) {
        return;
    }
    if (setting_id && strcmp(setting_id, "speed_readout") == 0) {
        snprintf(out_buf, out_cap, "Horiz: %.2f m/s | Vert: %+.2f m/s | Peak: %.2f m/s",
                 (double)g_qm.phys.horiz_speed,
                 (double)g_qm.phys.vel[2],
                 (double)g_qm.phys.peak_speed);
        return;
    }
    const char* sym_state = (g_qm.has_tier2_mem && g_qm.player_vel_va && g_qm.player_flags_va)
        ? (g_qm.last_blocked_guard ? "GUARDED" : "LIVE")
        : "STANDBY(no-syms)";
    snprintf(out_buf, out_cap, "%s | %s | %s | %.2f m/s | stam=%.1f/%d | %s",
             g_qm.enabled ? "ON" : "OFF",
             qm_preset_name(g_qm.preset),
             qm_mode_name(g_qm.phys.mode),
             (double)g_qm.phys.horiz_speed,
             (double)g_qm.phys.stamina,
             g_qm.active_params.dash_max_stamina,
             sym_state);
}

static void qm_reg_float_slider(
    const fb_menu_api_v1_t* menu,
    const char*             sid,
    const char*             label,
    const char*             desc,
    float                   def_val,
    double                  min_v,
    double                  max_v,
    double                  step_v,
    const char*             fmt)
{
    fb_setting_desc_v1_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.type = (uint32_t)FB_SETTING_FLOAT;
    s.mod_id = QM_MOD_ID;
    s.section = "Custom Tuning";
    s.setting_id = sid;
    s.label = label;
    s.description = desc;
    s.default_float = (double)def_val;
    s.float_min = min_v;
    s.float_max = max_v;
    s.float_step = step_v;
    s.float_fmt = fmt;
    s.on_change = qm_menu_setting_cb;
    (void)menu->register_setting(&s);
}

FB_API int fb_plugin_register_menu(const fb_menu_api_v1_t* menu) {
    if (!menu || !g_qm.initialized) {
        return FB_ERR_INVALID_ARG;
    }
    if (menu->abi_version != FB_MENU_ABI_VERSION || menu->struct_size < sizeof(fb_menu_api_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!menu->register_mod || !menu->register_action || !menu->register_setting) {
        return FB_ERR_INVALID_ARG;
    }

    g_qm.menu = *menu;
    g_qm.menu_registered = 1;

    (void)menu->register_mod(
        QM_MOD_ID,
        "Quake Movement",
        "Quake (VQ3/CPMA), Half-Life, Apex Legends, Deadlock & Custom on-foot movement physics.");

    /* Conflict-free actions */
    {
        fb_action_desc_v1_t a;
        memset(&a, 0, sizeof(a));
        a.struct_size = (uint32_t)sizeof(a);
        a.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
        a.mod_id = QM_MOD_ID;
        a.action_id = "toggle_movement";
        a.label = "Toggle Quake Movement";
        a.description = "Enable or disable custom on-foot movement physics.";
        a.consume_input = 1;
        a.default_key.code = g_qm.key_toggle;
        a.default_key.modifiers = FB_MOD_NONE;
        a.default_pad.code = FB_PAD_A;
        a.default_pad.modifiers = FB_PAD_RIGHT_SHOULDER; /* RB + A */
        a.callback = qm_menu_action_cb;
        (void)menu->register_action(&a);

        memset(&a, 0, sizeof(a));
        a.struct_size = (uint32_t)sizeof(a);
        a.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
        a.mod_id = QM_MOD_ID;
        a.action_id = "cycle_preset";
        a.label = "Cycle Movement Preset";
        a.description = "Switch between Quake VQ3, CPMA, Half-Life, Apex Legends, Deadlock, and Custom.";
        a.consume_input = 1;
        a.default_key.code = g_qm.key_cycle_preset;
        a.default_key.modifiers = FB_MOD_CTRL;
        a.default_pad.code = FB_PAD_Y;
        a.default_pad.modifiers = FB_PAD_RIGHT_SHOULDER; /* RB + Y */
        a.callback = qm_menu_action_cb;
        (void)menu->register_action(&a);

        memset(&a, 0, sizeof(a));
        a.struct_size = (uint32_t)sizeof(a);
        a.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
        a.mod_id = QM_MOD_ID;
        a.action_id = "dash";
        a.label = "Directional Dash";
        a.description = "Trigger directional ground/air dash (Deadlock / Custom presets).";
        a.consume_input = 1;
        a.default_key.code = g_qm.key_dash;
        a.default_key.modifiers = FB_MOD_SHIFT;
        a.default_pad.code = FB_PAD_B;
        a.default_pad.modifiers = FB_PAD_RIGHT_SHOULDER; /* RB + B */
        a.callback = qm_menu_action_cb;
        (void)menu->register_action(&a);
    }

    /* Status & Speed Readouts */
    {
        fb_setting_desc_v1_t s;
        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_STATUS;
        s.mod_id = QM_MOD_ID;
        s.section = "Live Telemetry";
        s.setting_id = "status";
        s.label = "State";
        s.description = "Live movement state, active preset, stamina pips, and Tier 2 symbol guard status.";
        s.on_status = qm_menu_status_cb;
        (void)menu->register_setting(&s);

        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_STATUS;
        s.mod_id = QM_MOD_ID;
        s.section = "Live Telemetry";
        s.setting_id = "speed_readout";
        s.label = "Speed";
        s.description = "Live horizontal speed, vertical velocity, and session peak speed (m/s).";
        s.on_status = qm_menu_status_cb;
        (void)menu->register_setting(&s);
    }

    /* General Controls */
    {
        fb_setting_desc_v1_t s;
        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_BOOL;
        s.mod_id = QM_MOD_ID;
        s.section = "General";
        s.setting_id = "enabled";
        s.label = "Enable Quake Movement";
        s.description = "Toggle custom on-foot movement physics override.";
        s.default_bool = g_qm.enabled;
        s.on_change = qm_menu_setting_cb;
        (void)menu->register_setting(&s);

        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_ENUM;
        s.mod_id = QM_MOD_ID;
        s.section = "General";
        s.setting_id = "preset";
        s.label = "Movement Preset";
        s.description = "Select physics preset (Quake VQ3, CPMA, Half-Life, Apex Legends, Deadlock, Custom).";
        s.default_enum = (int32_t)g_qm.preset;
        s.enum_labels = k_qm_preset_labels;
        s.enum_count = (uint32_t)QM_PRESET_COUNT;
        s.on_change = qm_menu_setting_cb;
        (void)menu->register_setting(&s);

        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_BOOL;
        s.mod_id = QM_MOD_ID;
        s.section = "General";
        s.setting_id = "auto_bhop";
        s.label = "Auto Bunnyhop";
        s.description = "Hold jump to auto-jump on landing without ground friction loss.";
        s.default_bool = g_qm.active_params.auto_bhop;
        s.on_change = qm_menu_setting_cb;
        (void)menu->register_setting(&s);

        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_BUTTON;
        s.mod_id = QM_MOD_ID;
        s.section = "General";
        s.setting_id = "reset_peak_btn";
        s.label = "Reset Peak Speed";
        s.description = "Clear recorded session peak horizontal speed.";
        s.on_button = qm_menu_button_cb;
        (void)menu->register_setting(&s);
    }

    /* Custom Preset Sliders */
    qm_reg_float_slider(menu, "custom_max_speed", "Max Ground Speed",
                        "Target ground run speed in Custom preset (m/s).",
                        g_qm.custom_params.max_speed, 3.0, 25.0, 0.5, "%.1f m/s");
    qm_reg_float_slider(menu, "custom_ground_accel", "Ground Accel",
                        "Ground PM_Accelerate factor in Custom preset.",
                        g_qm.custom_params.ground_accel, 1.0, 40.0, 0.5, "%.1f");
    qm_reg_float_slider(menu, "custom_air_accel", "Air Accel",
                        "Air PM_AirAccelerate factor in Custom preset.",
                        g_qm.custom_params.air_accel, 0.5, 180.0, 1.0, "%.1f");
    qm_reg_float_slider(menu, "custom_air_wish_cap", "Air Wish Cap",
                        "Air wishspeed cap in Custom preset (m/s).",
                        g_qm.custom_params.air_wish_cap, 0.5, 20.0, 0.25, "%.2f m/s");
    qm_reg_float_slider(menu, "custom_friction", "Ground Friction",
                        "Ground friction coefficient in Custom preset.",
                        g_qm.custom_params.friction, 0.5, 15.0, 0.5, "%.1f");
    qm_reg_float_slider(menu, "custom_gravity", "Gravity",
                        "Downward gravity magnitude in Custom preset (m/s^2).",
                        g_qm.custom_params.gravity, 4.0, 35.0, 0.5, "%.1f m/s^2");
    qm_reg_float_slider(menu, "custom_jump_speed", "Jump Speed",
                        "Upward jump launch velocity in Custom preset (m/s).",
                        g_qm.custom_params.jump_speed, 2.0, 15.0, 0.25, "%.2f m/s");
    qm_reg_float_slider(menu, "custom_slide_boost", "Slide Boost",
                        "Flat-ground crouch-slide boost in Custom preset (m/s).",
                        g_qm.custom_params.slide_boost, 0.0, 12.0, 0.5, "%.1f m/s");
    qm_reg_float_slider(menu, "custom_dash_impulse", "Dash Impulse",
                        "Directional dash speed in Custom preset (m/s).",
                        g_qm.custom_params.dash_impulse, 0.0, 25.0, 0.5, "%.1f m/s");

    {
        fb_setting_desc_v1_t s;
        memset(&s, 0, sizeof(s));
        s.struct_size = (uint32_t)sizeof(s);
        s.type = (uint32_t)FB_SETTING_BUTTON;
        s.mod_id = QM_MOD_ID;
        s.section = "Custom Tuning";
        s.setting_id = "reset_custom_btn";
        s.label = "Reset Custom Defaults";
        s.description = "Restore all Custom preset sliders to their default values.";
        s.on_button = qm_menu_button_cb;
        (void)menu->register_setting(&s);
    }

    /* Pull any persisted user settings from fireball_user_config.json */
    if (menu->get_setting) {
        fb_setting_value_t fv;
        if (menu->get_setting(QM_MOD_ID, "custom_max_speed", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.max_speed = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_ground_accel", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.ground_accel = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_air_accel", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.air_accel = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_air_wish_cap", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.air_wish_cap = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_friction", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.friction = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_gravity", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.gravity = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_jump_speed", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.jump_speed = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_slide_boost", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.slide_boost = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "custom_dash_impulse", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_FLOAT) {
            g_qm.custom_params.dash_impulse = (float)fv.v.float_val;
        }
        if (menu->get_setting(QM_MOD_ID, "preset", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_ENUM &&
            fv.v.enum_val >= 0 && fv.v.enum_val < (int32_t)QM_PRESET_COUNT) {
            qm_apply_preset((qm_preset_id_t)fv.v.enum_val, 0);
        }
        if (menu->get_setting(QM_MOD_ID, "auto_bhop", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_BOOL) {
            g_qm.active_params.auto_bhop = fv.v.bool_val ? 1 : 0;
        }
        if (menu->get_setting(QM_MOD_ID, "enabled", &fv) == FB_OK && fv.type == (uint32_t)FB_SETTING_BOOL) {
            g_qm.enabled = fv.v.bool_val ? 1 : 0;
        }
    }

    qm_sync_core_menu_state();
    return FB_OK;
}

/* ------------------------------------------------------------------------- */
/* Optional Fireball Console Integration (`fb_plugin_register_console`)      */
/* ------------------------------------------------------------------------- */

static void qm_con_print(const fb_console_writer_v1_t* out, int level, const char* text) {
    if (out && out->print) {
        out->print(out->user_ctx, level, text);
    }
}

static int qm_cmd_handler(
    int                           argc,
    const char* const*            argv,
    const fb_console_writer_v1_t* out,
    void*                         user_ctx)
{
    (void)user_ctx;
    if (argc < 2 || qm_ci_eq(argv[1], "status")) {
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "qm: %s | preset=%s (%s) | mode=%s | speed=%.2f m/s (vz=%+.2f, peak=%.2f) | "
                 "max=%.1f air_acc=%.1f cap=%.2f fric=%.1f grav=%.1f jump=%.2f bhop=%d | "
                 "stam=%.1f/%d | writes=%llu skips=%llu | syms=%s",
                 g_qm.enabled ? "ON" : "OFF",
                 qm_preset_name(g_qm.preset),
                 qm_preset_key(g_qm.preset),
                 qm_mode_name(g_qm.phys.mode),
                 (double)g_qm.phys.horiz_speed,
                 (double)g_qm.phys.vel[2],
                 (double)g_qm.phys.peak_speed,
                 (double)g_qm.active_params.max_speed,
                 (double)g_qm.active_params.air_accel,
                 (double)g_qm.active_params.air_wish_cap,
                 (double)g_qm.active_params.friction,
                 (double)g_qm.active_params.gravity,
                 (double)g_qm.active_params.jump_speed,
                 g_qm.active_params.auto_bhop,
                 (double)g_qm.phys.stamina,
                 g_qm.active_params.dash_max_stamina,
                 (unsigned long long)g_qm.vel_writes,
                 (unsigned long long)g_qm.guard_skips,
                 (g_qm.has_tier2_mem && g_qm.player_vel_va && g_qm.player_flags_va) ? "resolved" : "unresolved");
        qm_con_print(out, FB_LOG_INFO, buf);
        return FB_OK;
    }

    if (qm_ci_eq(argv[1], "toggle")) {
        qm_set_enabled(!g_qm.enabled, 1);
        char buf[128];
        snprintf(buf, sizeof(buf), "qm: movement %s (preset=%s)",
                 g_qm.enabled ? "ON" : "OFF", qm_preset_name(g_qm.preset));
        qm_con_print(out, FB_LOG_INFO, buf);
        return FB_OK;
    }

    if (qm_ci_eq(argv[1], "on") || qm_ci_eq(argv[1], "1")) {
        qm_set_enabled(1, 1);
        qm_con_print(out, FB_LOG_INFO, "qm: movement ON");
        return FB_OK;
    }

    if (qm_ci_eq(argv[1], "off") || qm_ci_eq(argv[1], "0")) {
        qm_set_enabled(0, 1);
        qm_con_print(out, FB_LOG_INFO, "qm: movement OFF");
        return FB_OK;
    }

    if (qm_ci_eq(argv[1], "preset")) {
        if (argc < 3) {
            qm_con_print(out, FB_LOG_WARN, "Usage: qm preset <vq3|cpma|halflife|apex|deadlock|custom>");
            return FB_ERR_INVALID_ARG;
        }
        qm_preset_id_t pid = QM_PRESET_QUAKE_VQ3;
        if (!qm_preset_from_name(argv[2], &pid)) {
            qm_con_print(out, FB_LOG_ERROR, "Unknown preset. Expected: vq3, cpma, halflife, apex, deadlock, custom");
            return FB_ERR_INVALID_ARG;
        }
        qm_apply_preset(pid, 1);
        char buf[128];
        snprintf(buf, sizeof(buf), "qm: preset set to %s", qm_preset_name(pid));
        qm_con_print(out, FB_LOG_INFO, buf);
        return FB_OK;
    }

    if (qm_ci_eq(argv[1], "set")) {
        if (argc < 4) {
            qm_con_print(out, FB_LOG_WARN, "Usage: qm set <param> <value>");
            return FB_ERR_INVALID_ARG;
        }
        char* end = NULL;
        double parsed = strtod(argv[3], &end);
        if (end == argv[3] || !isfinite(parsed)) {
            if (qm_ci_eq(argv[3], "true") || qm_ci_eq(argv[3], "on")) {
                parsed = 1.0;
            } else if (qm_ci_eq(argv[3], "false") || qm_ci_eq(argv[3], "off")) {
                parsed = 0.0;
            } else {
                qm_con_print(out, FB_LOG_ERROR, "Invalid numeric/boolean value for 'qm set'");
                return FB_ERR_INVALID_ARG;
            }
        }
        if (!qm_set_param_by_name(argv[2], (float)parsed)) {
            qm_con_print(out, FB_LOG_ERROR,
                         "Unknown parameter. Valid: max_speed, ground_accel, air_accel, air_wish_cap, "
                         "air_control, friction, stop_speed, gravity, jump_speed, coyote_time, jump_buffer, "
                         "auto_bhop, slide_enabled, slide_boost, slide_friction, slide_slope_accel, "
                         "slide_jump_boost, tap_strafe_enabled, tap_strafe_window, tap_strafe_strength, "
                         "dash_enabled, dash_impulse, dash_recharge, dash_jump_boost, wall_bounce_enabled");
            return FB_ERR_INVALID_ARG;
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "qm: set %s = %.3f", argv[2], parsed);
        qm_con_print(out, FB_LOG_INFO, buf);
        return FB_OK;
    }

    qm_con_print(out, FB_LOG_WARN,
                 "Usage: qm <toggle|on|off|preset <vq3|cpma|halflife|apex|deadlock|custom>|set <param> <value>|status>");
    return FB_ERR_INVALID_ARG;
}

FB_API int fb_plugin_register_console(const fb_console_api_v1_t* console) {
    if (!console || !g_qm.initialized) {
        return FB_ERR_INVALID_ARG;
    }
    if (console->abi_version != FB_CONSOLE_ABI_VERSION || console->struct_size < sizeof(fb_console_api_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!console->register_command) {
        return FB_ERR_INVALID_ARG;
    }

    g_qm.console = *console;
    g_qm.console_registered = 1;

    fb_command_desc_v1_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.struct_size = (uint32_t)sizeof(cmd);
    cmd.mod_id = QM_MOD_ID;
    cmd.name = "qm";
    cmd.usage = "qm <toggle|on|off|preset <vq3|cpma|halflife|apex|deadlock|custom>|set <param> <value>|status>";
    cmd.description = "Control Quake Movement presets, parameters, and live telemetry.";
    cmd.callback = qm_cmd_handler;
    return console->register_command(&cmd);
}

/* ------------------------------------------------------------------------- */
/* Plugin Lifecycle (`fb_plugin_query`, `fb_plugin_init`, `fb_plugin_shutdown`) */
/* ------------------------------------------------------------------------- */

FB_API int fb_plugin_query(fb_plugin_info_v1_t* out_info) {
    if (!out_info) {
        return FB_ERR_INVALID_ARG;
    }
    if (out_info->struct_size != 0u && out_info->struct_size < sizeof(fb_plugin_info_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(fb_plugin_info_v1_t);
    out_info->abi_version = FB_PLUGIN_ABI_VERSION;
    out_info->mod_id = QM_MOD_ID;
    out_info->plugin_id = QM_PLUGIN_ID;
    out_info->version = QM_VERSION;
    out_info->required_caps = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS;
    return FB_OK;
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core) {
        return FB_ERR_INVALID_ARG;
    }
    if (core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size < sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!core->register_event_listener) {
        return FB_ERR_INVALID_ARG;
    }

    memset(&g_qm, 0, sizeof(g_qm));
    g_qm.core = *core;

    /* Default symbol names & layouts */
    snprintf(g_qm.player_pos_symbol, sizeof(g_qm.player_pos_symbol), "fb.player_pos");
    snprintf(g_qm.player_vel_symbol, sizeof(g_qm.player_vel_symbol), "fb.player_vel");
    snprintf(g_qm.player_flags_symbol, sizeof(g_qm.player_flags_symbol), "fb.player_flags");
    snprintf(g_qm.camera_basis_symbol, sizeof(g_qm.camera_basis_symbol), "fb.camera_basis");
    g_qm.camera_layout = QM_CAM_MAT34;

    g_qm.flag_on_ground = QM_FLAG_ON_GROUND_DEFAULT;
    g_qm.flag_crouched = QM_FLAG_CROUCHED_DEFAULT;
    g_qm.flag_in_vehicle = QM_FLAG_IN_VEHICLE_DEFAULT;
    g_qm.flag_ragdoll = QM_FLAG_RAGDOLL_DEFAULT;
    g_qm.flag_dead = QM_FLAG_DEAD_DEFAULT;

    g_qm.key_toggle = QM_KEY_TOGGLE_DEFAULT;
    g_qm.key_cycle_preset = QM_KEY_CYCLE_PRESET_DEFAULT;
    g_qm.key_dash = QM_KEY_DASH_DEFAULT;

    g_qm.enabled = 0;
    g_qm.preset = QM_PRESET_QUAKE_VQ3;
    qm_preset_defaults(QM_PRESET_CUSTOM, &g_qm.custom_params);
    qm_preset_defaults(g_qm.preset, &g_qm.active_params);

    /* Optional config from mod.toml / bundle */
    if (core->config_get_json) {
        char cfg[4096];
        memset(cfg, 0, sizeof(cfg));
        if (core->config_get_json(QM_MOD_ID, cfg, sizeof(cfg)) == FB_OK && cfg[0] != '\0') {
            qm_json_bool(cfg, "start_enabled", &g_qm.enabled);

            char preset_str[64] = { 0 };
            qm_json_str(cfg, "default_preset", preset_str, sizeof(preset_str));
            if (preset_str[0]) {
                qm_preset_id_t pid = QM_PRESET_QUAKE_VQ3;
                if (qm_preset_from_name(preset_str, &pid)) {
                    g_qm.preset = pid;
                    qm_preset_defaults(pid, &g_qm.active_params);
                }
            }

            qm_json_bool(cfg, "auto_bhop", &g_qm.active_params.auto_bhop);
            qm_json_float(cfg, "custom_max_speed", &g_qm.custom_params.max_speed);
            qm_json_float(cfg, "custom_ground_accel", &g_qm.custom_params.ground_accel);
            qm_json_float(cfg, "custom_air_accel", &g_qm.custom_params.air_accel);
            qm_json_float(cfg, "custom_air_wish_cap", &g_qm.custom_params.air_wish_cap);
            qm_json_float(cfg, "custom_friction", &g_qm.custom_params.friction);
            qm_json_float(cfg, "custom_gravity", &g_qm.custom_params.gravity);
            qm_json_float(cfg, "custom_jump_speed", &g_qm.custom_params.jump_speed);
            qm_json_float(cfg, "custom_slide_boost", &g_qm.custom_params.slide_boost);
            qm_json_float(cfg, "custom_dash_impulse", &g_qm.custom_params.dash_impulse);
            if (g_qm.preset == QM_PRESET_CUSTOM) {
                g_qm.active_params = g_qm.custom_params;
            }

            qm_json_str(cfg, "player_pos_symbol", g_qm.player_pos_symbol, sizeof(g_qm.player_pos_symbol));
            qm_json_str(cfg, "player_vel_symbol", g_qm.player_vel_symbol, sizeof(g_qm.player_vel_symbol));
            qm_json_str(cfg, "player_flags_symbol", g_qm.player_flags_symbol, sizeof(g_qm.player_flags_symbol));
            qm_json_str(cfg, "camera_basis_symbol", g_qm.camera_basis_symbol, sizeof(g_qm.camera_basis_symbol));

            char cam_layout_str[64] = { 0 };
            qm_json_str(cfg, "camera_layout", cam_layout_str, sizeof(cam_layout_str));
            if (qm_ci_eq(cam_layout_str, "yaw_f32")) {
                g_qm.camera_layout = QM_CAM_YAW_F32;
            } else if (qm_ci_eq(cam_layout_str, "forward_vec3")) {
                g_qm.camera_layout = QM_CAM_FORWARD_V3;
            } else if (qm_ci_eq(cam_layout_str, "mat34")) {
                g_qm.camera_layout = QM_CAM_MAT34;
            }

            qm_json_u32(cfg, "flag_on_ground_mask", &g_qm.flag_on_ground);
            qm_json_u32(cfg, "flag_crouched_mask", &g_qm.flag_crouched);
            qm_json_u32(cfg, "flag_in_vehicle_mask", &g_qm.flag_in_vehicle);
            qm_json_u32(cfg, "flag_ragdoll_mask", &g_qm.flag_ragdoll);
            qm_json_u32(cfg, "flag_dead_mask", &g_qm.flag_dead);

            qm_json_u32(cfg, "key_toggle_movement", &g_qm.key_toggle);
            qm_json_u32(cfg, "key_cycle_preset", &g_qm.key_cycle_preset);
            qm_json_u32(cfg, "key_dash", &g_qm.key_dash);
        }
    }

    qm_state_init(&g_qm.phys, &g_qm.active_params);

    /* Resolve Tier 2 engine symbols (degrades cleanly to standby if absent) */
    if ((core->active_host_caps & (FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW)) ==
            (FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW) &&
        core->resolve_engine_symbol && core->title_mem_read_u32 && core->title_mem_write_u32) {
        g_qm.has_tier2_mem = 1;
        g_qm.player_pos_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_qm.player_pos_symbol);
        g_qm.player_vel_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_qm.player_vel_symbol);
        g_qm.player_flags_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_qm.player_flags_symbol);
        g_qm.camera_basis_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_qm.camera_basis_symbol);
    }

    if (core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, qm_on_event, NULL) != FB_OK ||
        core->register_event_listener((uint32_t)FB_EVENT_FRAME_BEGIN, qm_on_event, NULL) != FB_OK ||
        core->register_event_listener((uint32_t)FB_EVENT_SHUTDOWN, qm_on_event, NULL) != FB_OK) {
        return FB_ERR_INTERNAL;
    }

    g_qm.initialized = 1;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[quake_movement] Initialized v%s (preset=%s, enabled=%d, vel_va=0x%llX, flags_va=0x%llX, cam_va=0x%llX)",
             QM_VERSION,
             qm_preset_name(g_qm.preset),
             g_qm.enabled,
             (unsigned long long)g_qm.player_vel_va,
             (unsigned long long)g_qm.player_flags_va,
             (unsigned long long)g_qm.camera_basis_va);
    qm_log(FB_LOG_INFO, msg);
    return FB_OK;
}

FB_API void fb_plugin_shutdown(void) {
    if (!g_qm.initialized) {
        return;
    }
    g_qm.enabled = 0;
    g_qm.initialized = 0;
    g_qm.menu_registered = 0;
    g_qm.console_registered = 0;
    qm_log(FB_LOG_INFO, "[quake_movement] Shutdown complete.");
}
