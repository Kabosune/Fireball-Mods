/*
 * fpv_drone.c — Native C11 Tier 0/2 Fireball-Mods Plugin (`fpv_drone.fbplugin.dll`).
 *
 * Implements a real Betaflight-in-the-loop FPV quadcopter for GTA VI (Project Americas / RAGE 9)
 * by dynamically loading `hammerill/libfpv` (`libfpv.dll`, ABI v1, monolithic or split-process
 * `libfpv.dll` + `libfpv-server.exe`) at runtime via `LoadLibraryW` + `GetProcAddress`.
 *
 * Pure C11, zero build-time third-party dependencies, compiled with `/std:c11 /W4 /WX`
 * strictly against Fireball-Core's public C11 SDK (`fireball_plugin.h`, `fireball_menu.h`).
 *
 * Key architecture points:
 *   - RAGE 9 Z-up coordinate system: { +X right (East), +Z up, +Y forward (North), 1.0 unit/m }.
 *   - Uses libfpv's shared config folder (`fpv_shared_data_dir()`) by default so Betaflight
 *     EEPROM, rates, presets, and radio mappings carry across games.
 *   - Supports Betaflight Configurator over WebSocket (`ws://localhost:5761`).
 *   - Registers raycast callback (`fpv_world_set_raycast_callback_ex` with fallback) including
 *     a configurable ground-plane fallback until a RAGE 9 physics line-probe hook is wired.
 *   - 50 ms post-spawn settle guard (per libfpv INTEGRATION.md) prevents `(0,0,0)` single-frame
 *     camera teleports right after `fpv_drone_create()`.
 *   - Writes camera transform (`mat34_fov`, `pos_quat_fov`, or RAGE `pos_euler_fov`), camera
 *     override flag, streaming focus, and drone pose through guarded `title_mem_write_u32`
 *     whenever Tier 2 symbols are published by the host.
 *   - Stereo Win32 `waveOut` motor/beeper audio (`fpv_drone_audio.c`) and opt-in Win32 GDI live
 *     Betaflight MAX7456 OSD preview window (`fpv_drone_osd.c`).
 *   - Full Fireball Mod Menu integration with bidirectional sync (`notify=0`) and graceful v1
 *     fallback when loaded without `fb_plugin_register_menu`.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fireball_plugin.h"
#include "fireball_menu.h"
#include "fpv_drone.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRONE_PI 3.14159265358979323846

typedef enum drone_cam_layout {
    DRONE_CAM_MAT34_FOV     = 0,
    DRONE_CAM_POS_QUAT_FOV  = 1,
    DRONE_CAM_POS_EULER_FOV = 2
} drone_cam_layout_t;

enum drone_host_action_id {
    DRONE_HOST_ACT_MOVE_PILOT = 1u,
    DRONE_HOST_ACT_RESPAWN    = 2u,
    DRONE_HOST_ACT_CYCLE_VIEW = 3u,
    DRONE_HOST_ACT_RECALL     = 4u
};

enum drone_ctrl_id {
    DRONE_CTRL_CYCLE_VIEW = 1u,
    DRONE_CTRL_RESPAWN    = 2u,
    DRONE_CTRL_RECALL     = 3u,
    DRONE_CTRL_MOVE_PILOT = 4u
};

static const char* const k_view_labels[4] = {
    "FPV",
    "Chase",
    "Line of Sight",
    "Pilot"
};

static const char* const k_osd_style_labels[2] = {
    "Analog (MAX7456)",
    "HD (Crisp)"
};

static const char* const k_scale_labels[5] = {
    "1.0x (Real time)",
    "0.5x (Slow-mo)",
    "0.25x",
    "0.1x",
    "2.0x (Fast)"
};

static const float k_scale_vals[5] = {
    1.0f, 0.5f, 0.25f, 0.1f, 2.0f
};

static const char* const k_default_preset_labels[1] = {
    "Default (libfpv not loaded)"
};

typedef struct drone_state {
    fb_core_services_v1_t core;
    int                   initialized;
    volatile LONG         log_active;
    SRWLOCK               log_lock;
    int                   log_lock_init;
    FILE*                 log_file;

    /* Config (populated during init) */
    char                  dll_path_cfg[520];
    char                  data_dir_cfg[520];
    char                  preset_cfg[128];
    char                  log_path[520];
    int                   enable_configurator;
    int                   configurator_port;
    int                   enable_device_input;
    int                   enable_lens_distortion;
    int                   enable_video_feed;
    float                 sim_rate_hz;
    float                 auto_pause_seconds;
    int                   no_battery_drain;
    int                   no_damage;
    int                   audio_enabled;
    float                 audio_volume;
    float                 audio_beep_volume;
    int                   osd_window_enabled;
    uint32_t              osd_style;
    float                 spawn_forward_m;
    float                 spawn_height_m;
    int                   use_ground_fallback;
    float                 ground_fallback_z;
    float                 ambient_wind[3];
    float                 ambient_wind_turb;

    /* Title memory symbol names & resolved addresses */
    char                  camera_symbol[128];
    drone_cam_layout_t    camera_layout;
    char                  camera_override_symbol[128];
    char                  pilot_pos_symbol[128];
    char                  focus_pos_symbol[128];
    char                  drone_pose_symbol[128];
    char                  wind_symbol[128];

    int                   has_tier2_mem;
    uint64_t              camera_va;
    uint64_t              camera_override_va;
    uint64_t              pilot_pos_va;
    uint64_t              focus_pos_va;
    uint64_t              drone_pose_va;
    uint64_t              wind_va;

    /* Hotkey VKs (for v1 fallback & Menu default bindings) */
    uint32_t              key_toggle_drone;
    uint32_t              key_cycle_view;
    uint32_t              key_respawn;
    uint32_t              key_pilot_view;
    uint32_t              key_overlay;

    /* Dynamically loaded libfpv state */
    HMODULE               libfpv_mod;
    libfpv_api_t          api;
    int                   libfpv_loaded;
    int                   libfpv_ready;
    char                  libfpv_version[64];
    char                  libfpv_error[256];
    char                  resolved_data_dir[520];
    char                  configurator_url[64];

    /* Preset list copied from libfpv */
    char                  preset_names[DRONE_MAX_PRESETS][64];
    const char*           preset_ptrs[DRONE_MAX_PRESETS];
    uint32_t              preset_count;
    int32_t               preset_index;

    /* Active flight state */
    fpv_drone*            drone;
    int                   drone_active;
    uint32_t              view;
    uint32_t              prev_non_pilot_view;
    int                   sim_paused;
    int32_t               sim_scale_idx;
    int                   overlay_open;
    int                   settle_pending;
    double                settle_start_sim_time;
    uint64_t              frames_since_spawn;
    uint64_t              frames_seen;
    uint64_t              camera_writes;
    uint64_t              raycast_calls;

    /* Last pilot & drone telemetry */
    float                 pilot_pos[3];
    float                 pilot_heading_rad;
    int                   has_pilot_pos;
    int                   has_last_frame;
    fpv_frame             last_frame;
    fpv_input_status      last_input_status;
    uint32_t              last_osd_revision;
    int                   camera_override_latched;

    /* Optional Mod Menu API */
    int                   menu_registered;
    fb_menu_api_v1_t      menu;
} drone_state_t;

static drone_state_t g_drone;

/* Anchor symbol used by GetModuleHandleExW to locate this plugin's directory */
static void drone_module_anchor(void) {}

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void drone_log(int level, const char* msg) {
    if (!msg) {
        return;
    }
    if (g_drone.core.log) {
        g_drone.core.log(DRONE_MOD_ID, level, msg);
    }
    if (g_drone.log_lock_init) {
        AcquireSRWLockExclusive(&g_drone.log_lock);
        if (g_drone.log_file) {
            fprintf(g_drone.log_file, "[L%d] %s\n", level, msg);
            fflush(g_drone.log_file);
        }
        ReleaseSRWLockExclusive(&g_drone.log_lock);
    }
}

static void drone_libfpv_log_cb(void* user, fpv_log_level level, const char* message) {
    (void)user;
    if (!InterlockedCompareExchange(&g_drone.log_active, 0, 0) || !message) {
        return;
    }
    int fb_lvl = FB_LOG_INFO;
    if (level == FPV_LOG_DEBUG) {
        fb_lvl = FB_LOG_DEBUG;
    } else if (level == FPV_LOG_WARN) {
        fb_lvl = FB_LOG_WARN;
    } else if (level == FPV_LOG_ERROR) {
        fb_lvl = FB_LOG_ERROR;
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "[libfpv] %s", message);
    drone_log(fb_lvl, buf);
}

/* ------------------------------------------------------------------------- */
/* Flat JSON config parsing helpers                                          */
/* ------------------------------------------------------------------------- */

static const char* drone_json_find(const char* json, const char* key) {
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

static void drone_json_u32(const char* json, const char* key, uint32_t* io) {
    const char* v = drone_json_find(json, key);
    if (v && isdigit((unsigned char)*v)) {
        char* end = NULL;
        unsigned long long parsed = strtoull(v, &end, 0);
        if (end != v) {
            *io = (uint32_t)parsed;
        }
    }
}

static void drone_json_int(const char* json, const char* key, int* io) {
    const char* v = drone_json_find(json, key);
    if (v && (isdigit((unsigned char)*v) || *v == '-')) {
        char* end = NULL;
        long parsed = strtol(v, &end, 0);
        if (end != v) {
            *io = (int)parsed;
        }
    }
}

static void drone_json_float(const char* json, const char* key, float* io) {
    const char* v = drone_json_find(json, key);
    if (v && (isdigit((unsigned char)*v) || *v == '-' || *v == '.')) {
        char* end = NULL;
        double parsed = strtod(v, &end);
        if (end != v) {
            *io = (float)parsed;
        }
    }
}

static void drone_json_bool(const char* json, const char* key, int* io) {
    const char* v = drone_json_find(json, key);
    if (v && strncmp(v, "true", 4) == 0) {
        *io = 1;
    } else if (v && strncmp(v, "false", 5) == 0) {
        *io = 0;
    }
}

static void drone_json_str(const char* json, const char* key, char* out, size_t cap) {
    const char* v = drone_json_find(json, key);
    if (!v || *v != '"' || cap < 2u) {
        return;
    }
    ++v;
    char tmp[520];
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
/* Guarded title memory float / u32 helpers                                  */
/* ------------------------------------------------------------------------- */

static uint32_t drone_f32_to_bits(float f) {
    uint32_t bits = 0u;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float drone_bits_to_f32(uint32_t bits) {
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static int drone_mem_read_f32(uint64_t va, float* out_f) {
    if (!g_drone.has_tier2_mem || va == 0ull || !g_drone.core.title_mem_read_u32) {
        return 0;
    }
    uint32_t bits = 0u;
    if (g_drone.core.title_mem_read_u32(va, &bits) != FB_OK) {
        return 0;
    }
    float f = drone_bits_to_f32(bits);
    if (!isfinite(f)) {
        return 0;
    }
    *out_f = f;
    return 1;
}

static int drone_mem_write_f32(uint64_t va, float f) {
    if (!g_drone.has_tier2_mem || va == 0ull || !g_drone.core.title_mem_write_u32) {
        return 0;
    }
    return g_drone.core.title_mem_write_u32(va, drone_f32_to_bits(f)) == FB_OK ? 1 : 0;
}

static void drone_set_camera_override(int active) {
    if (!g_drone.has_tier2_mem || g_drone.camera_override_va == 0ull || !g_drone.core.title_mem_write_u32) {
        g_drone.camera_override_latched = active ? 1 : 0;
        return;
    }
    uint32_t val = active ? 1u : 0u;
    if (g_drone.core.title_mem_write_u32(g_drone.camera_override_va, val) == FB_OK) {
        g_drone.camera_override_latched = active ? 1 : 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Menu synchronization helpers                                              */
/* ------------------------------------------------------------------------- */

static void drone_sync_menu_bool(const char* setting_id, int value) {
    if (!g_drone.menu_registered || !g_drone.menu.set_setting) {
        return;
    }
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = (uint32_t)FB_SETTING_BOOL;
    v.v.bool_val = value ? 1 : 0;
    (void)g_drone.menu.set_setting(DRONE_MOD_ID, setting_id, &v, 0);
}

static void drone_sync_menu_enum(const char* setting_id, int32_t value) {
    if (!g_drone.menu_registered || !g_drone.menu.set_setting) {
        return;
    }
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = (uint32_t)FB_SETTING_ENUM;
    v.v.enum_val = value;
    (void)g_drone.menu.set_setting(DRONE_MOD_ID, setting_id, &v, 0);
}

/* ------------------------------------------------------------------------- */
/* Raycast callback (ground-plane fallback + clean hook point for RAGE 9)    */
/* ------------------------------------------------------------------------- */

static int drone_raycast_common(
    const float      origin[3],
    const float      direction[3],
    float            max_distance,
    fpv_raycast_hit* out_hit)
{
    g_drone.raycast_calls += 1ull;
    if (!g_drone.use_ground_fallback || !origin || !direction || !out_hit || max_distance <= 0.0f) {
        return 0;
    }
    float dz = direction[2];
    if (dz >= -1e-5f) {
        return 0;
    }
    float t = (g_drone.ground_fallback_z - origin[2]) / dz;
    if (t < 0.0f || t > max_distance) {
        return 0;
    }
    out_hit->distance = t;
    out_hit->normal[0] = 0.0f;
    out_hit->normal[1] = 0.0f;
    out_hit->normal[2] = 1.0f;
    out_hit->material = 1u; /* Rough ground material slot (friction 0.6) */
    out_hit->object_id = 0u;
    return 1;
}

static int drone_raycast_cb(
    void*            user,
    const float      origin[3],
    const float      direction[3],
    float            max_distance,
    fpv_raycast_hit* out_hit)
{
    (void)user;
    return drone_raycast_common(origin, direction, max_distance, out_hit);
}

static int drone_raycast_ex_cb(
    void*               user,
    const float         origin[3],
    const float         direction[3],
    float               max_distance,
    fpv_raycast_hit_ex* out_hit)
{
    (void)user;
    if (!out_hit || out_hit->struct_size < (uint32_t)(offsetof(fpv_raycast_hit_ex, hit) + sizeof(fpv_raycast_hit))) {
        return 0;
    }
    if (!drone_raycast_common(origin, direction, max_distance, &out_hit->hit)) {
        return 0;
    }
    if (out_hit->struct_size >= (uint32_t)(offsetof(fpv_raycast_hit_ex, flags) + sizeof(uint32_t))) {
        out_hit->flags = 0u;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Dynamic loader for `libfpv.dll` (zero build-time dependency)              */
/* ------------------------------------------------------------------------- */

static int drone_get_plugin_dir_w(wchar_t* out, size_t cap) {
    HMODULE self = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(const void*)&drone_module_anchor,
            &self) || !self)
    {
        return 0;
    }
    DWORD len = GetModuleFileNameW(self, out, (DWORD)cap);
    if (len == 0u || len >= (DWORD)cap) {
        return 0;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) {
        slash = wcsrchr(out, L'/');
    }
    if (!slash) {
        return 0;
    }
    *slash = L'\0';
    return 1;
}

static HMODULE drone_load_libfpv_module(void) {
    /* 1. Explicit configured path */
    if (g_drone.dll_path_cfg[0] != '\0') {
        wchar_t wpath[520];
        if (MultiByteToWideChar(CP_UTF8, 0, g_drone.dll_path_cfg, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) > 0) {
            HMODULE m = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (m) {
                return m;
            }
        }
    }

    /* 2. Environment override LIBFPV_DLL */
    {
        wchar_t env_path[520];
        DWORD n = GetEnvironmentVariableW(L"LIBFPV_DLL", env_path, (DWORD)(sizeof(env_path) / sizeof(env_path[0])));
        if (n > 0u && n < (DWORD)(sizeof(env_path) / sizeof(env_path[0]))) {
            HMODULE m = LoadLibraryExW(env_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (m) {
                return m;
            }
        }
    }

    /* 3. Next to fpv_drone.fbplugin.dll and ./libfpv/ subfolder */
    wchar_t dir[520];
    if (drone_get_plugin_dir_w(dir, sizeof(dir) / sizeof(dir[0]))) {
        wchar_t cand[640];
        swprintf(cand, sizeof(cand) / sizeof(cand[0]), L"%ls\\libfpv.dll", dir);
        HMODULE m = LoadLibraryExW(cand, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) {
            return m;
        }
        swprintf(cand, sizeof(cand) / sizeof(cand[0]), L"%ls\\libfpv\\libfpv.dll", dir);
        m = LoadLibraryExW(cand, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) {
            return m;
        }
    }

    /* 4. Standard Windows DLL search (game folder / PATH) */
    return LoadLibraryW(L"libfpv.dll");
}

#define DRONE_RESOLVE(mod, field, sym)                                         \
    do {                                                                       \
        FARPROC fp_ = GetProcAddress((mod), (sym));                            \
        memcpy(&g_drone.api.field, &fp_, sizeof(g_drone.api.field));           \
    } while (0)

static int drone_resolve_libfpv_exports(HMODULE mod) {
    memset(&g_drone.api, 0, sizeof(g_drone.api));
    DRONE_RESOLVE(mod, abi_version, "fpv_abi_version");
    DRONE_RESOLVE(mod, version_string, "fpv_version_string");
    DRONE_RESOLVE(mod, result_string, "fpv_result_string");
    DRONE_RESOLVE(mod, last_error, "fpv_last_error");
    DRONE_RESOLVE(mod, free_mem, "fpv_free");

    DRONE_RESOLVE(mod, global_config_defaults, "fpv_global_config_defaults");
    DRONE_RESOLVE(mod, shared_data_dir, "fpv_shared_data_dir");
    DRONE_RESOLVE(mod, global_init, "fpv_global_init");
    DRONE_RESOLVE(mod, global_terminate, "fpv_global_terminate");
    DRONE_RESOLVE(mod, is_initialized, "fpv_is_initialized");

    DRONE_RESOLVE(mod, drone_desc_defaults, "fpv_drone_desc_defaults");
    DRONE_RESOLVE(mod, drone_create, "fpv_drone_create");
    DRONE_RESOLVE(mod, drone_delete, "fpv_drone_delete");
    DRONE_RESOLVE(mod, drone_reset, "fpv_drone_reset");
    DRONE_RESOLVE(mod, drone_repair, "fpv_drone_repair");
    DRONE_RESOLVE(mod, drone_tick, "fpv_drone_tick");

    DRONE_RESOLVE(mod, material_set, "fpv_material_set");
    DRONE_RESOLVE(mod, world_set_raycast_callback, "fpv_world_set_raycast_callback");
    DRONE_RESOLVE(mod, world_set_raycast_callback_ex, "fpv_world_set_raycast_callback_ex");

    DRONE_RESOLVE(mod, drone_set_pilot_position, "fpv_drone_set_pilot_position");
    DRONE_RESOLVE(mod, drone_set_preset, "fpv_drone_set_preset");
    DRONE_RESOLVE(mod, gps_set_home, "fpv_gps_set_home");
    DRONE_RESOLVE(mod, set_view, "fpv_set_view");

    DRONE_RESOLVE(mod, sim_set_options, "fpv_sim_set_options");
    DRONE_RESOLVE(mod, sim_get_options, "fpv_sim_get_options");

    DRONE_RESOLVE(mod, pilot_set_support, "fpv_pilot_set_support");
    DRONE_RESOLVE(mod, pilot_get_config, "fpv_pilot_get_config");

    DRONE_RESOLVE(mod, wind_defaults, "fpv_wind_defaults");
    DRONE_RESOLVE(mod, wind_set, "fpv_wind_set");

    DRONE_RESOLVE(mod, set_paused, "fpv_set_paused");
    DRONE_RESOLVE(mod, is_paused, "fpv_is_paused");
    DRONE_RESOLVE(mod, set_time_scale, "fpv_set_time_scale");
    DRONE_RESOLVE(mod, step, "fpv_step");

    DRONE_RESOLVE(mod, input_get_status, "fpv_input_get_status");
    DRONE_RESOLVE(mod, input_set_channels, "fpv_input_set_channels");
    DRONE_RESOLVE(mod, input_clear_channels, "fpv_input_clear_channels");
    DRONE_RESOLVE(mod, input_key, "fpv_input_key");

    DRONE_RESOLVE(mod, controls_set, "fpv_controls_set");
    DRONE_RESOLVE(mod, control_poll, "fpv_control_poll");

    DRONE_RESOLVE(mod, overlay_set_open, "fpv_overlay_set_open");
    DRONE_RESOLVE(mod, overlay_is_open, "fpv_overlay_is_open");
    DRONE_RESOLVE(mod, overlay_wants_input, "fpv_overlay_wants_input");
    DRONE_RESOLVE(mod, overlay_set_toggle_key, "fpv_overlay_set_toggle_key");
    DRONE_RESOLVE(mod, overlay_set_display, "fpv_overlay_set_display");
    DRONE_RESOLVE(mod, overlay_set_action, "fpv_overlay_set_action");
    DRONE_RESOLVE(mod, overlay_poll_action, "fpv_overlay_poll_action");
    DRONE_RESOLVE(mod, overlay_render, "fpv_overlay_render");

    DRONE_RESOLVE(mod, osd_draw_data, "fpv_osd_draw_data");
    DRONE_RESOLVE(mod, osd_render_rgba, "fpv_osd_render_rgba");

    DRONE_RESOLVE(mod, audio_configure, "fpv_audio_configure");
    DRONE_RESOLVE(mod, audio_read, "fpv_audio_read");
    DRONE_RESOLVE(mod, audio_set_volume, "fpv_audio_set_volume");
    DRONE_RESOLVE(mod, audio_set_beep_volume, "fpv_audio_set_beep_volume");
    DRONE_RESOLVE(mod, audio_set_listener, "fpv_audio_set_listener");

    DRONE_RESOLVE(mod, preset_count, "fpv_preset_count");
    DRONE_RESOLVE(mod, preset_name, "fpv_preset_name");
    DRONE_RESOLVE(mod, configurator_get_info, "fpv_configurator_get_info");

    return g_drone.api.abi_version &&
           g_drone.api.global_init &&
           g_drone.api.global_terminate &&
           g_drone.api.drone_create &&
           g_drone.api.drone_delete &&
           g_drone.api.drone_reset &&
           g_drone.api.drone_repair &&
           g_drone.api.drone_tick;
}

#undef DRONE_RESOLVE

static void drone_apply_sim_options(void) {
    if (!g_drone.libfpv_ready || !g_drone.api.sim_set_options) {
        return;
    }
    uint32_t opts = 0u;
    if (g_drone.no_battery_drain) {
        opts |= (uint32_t)FPV_SIM_NO_BATTERY_DRAIN;
    }
    if (g_drone.no_damage) {
        opts |= (uint32_t)FPV_SIM_NO_DAMAGE;
    }
    g_drone.api.sim_set_options(opts);
}

static void drone_apply_audio_settings(void) {
    if (!g_drone.libfpv_ready) {
        return;
    }
    if (g_drone.api.audio_set_volume) {
        g_drone.api.audio_set_volume(g_drone.audio_volume);
    }
    if (g_drone.api.audio_set_beep_volume) {
        g_drone.api.audio_set_beep_volume(g_drone.audio_beep_volume);
    }
    if (g_drone.drone_active && g_drone.audio_enabled && g_drone.api.audio_read) {
        if (!drone_audio_is_running()) {
            if (g_drone.api.audio_configure) {
                (void)g_drone.api.audio_configure(44100u, 2u);
            }
            (void)drone_audio_start(g_drone.api.audio_read, 44100u, 2u);
        }
    } else {
        drone_audio_stop();
    }
}

static void drone_refresh_presets(void) {
    g_drone.preset_count = 0u;
    if (!g_drone.libfpv_ready || !g_drone.api.preset_count || !g_drone.api.preset_name) {
        return;
    }
    uint32_t n = g_drone.api.preset_count();
    if (n > DRONE_MAX_PRESETS) {
        n = DRONE_MAX_PRESETS;
    }
    for (uint32_t i = 0u; i < n; ++i) {
        const char* name = g_drone.api.preset_name(i);
        if (!name || !name[0]) {
            continue;
        }
        snprintf(g_drone.preset_names[g_drone.preset_count], sizeof(g_drone.preset_names[0]), "%s", name);
        g_drone.preset_ptrs[g_drone.preset_count] = g_drone.preset_names[g_drone.preset_count];
        if (g_drone.preset_cfg[0] != '\0' && strcmp(g_drone.preset_cfg, name) == 0) {
            g_drone.preset_index = (int32_t)g_drone.preset_count;
        }
        g_drone.preset_count += 1u;
    }
}

static int drone_init_libfpv(void) {
    if (g_drone.libfpv_ready) {
        return 1;
    }
    g_drone.libfpv_error[0] = '\0';

    if (!g_drone.libfpv_loaded) {
        HMODULE mod = drone_load_libfpv_module();
        if (!mod) {
            snprintf(
                g_drone.libfpv_error,
                sizeof(g_drone.libfpv_error),
                "libfpv.dll not found (place x64 libfpv.dll next to fpv_drone.fbplugin.dll or set libfpv_dll_path)");
            drone_log(FB_LOG_WARN, "[fpv_drone] libfpv.dll not found; standing by until installed.");
            return 0;
        }
        if (!drone_resolve_libfpv_exports(mod)) {
            FreeLibrary(mod);
            snprintf(g_drone.libfpv_error, sizeof(g_drone.libfpv_error), "libfpv.dll missing required C ABI exports");
            drone_log(FB_LOG_ERROR, "[fpv_drone] libfpv.dll missing required exports.");
            return 0;
        }
        uint32_t abi = g_drone.api.abi_version();
        if (abi != LIBFPV_ABI_VERSION) {
            FreeLibrary(mod);
            snprintf(
                g_drone.libfpv_error,
                sizeof(g_drone.libfpv_error),
                "libfpv ABI mismatch (got %u, expected %u)",
                (unsigned)abi,
                (unsigned)LIBFPV_ABI_VERSION);
            drone_log(FB_LOG_ERROR, g_drone.libfpv_error);
            return 0;
        }
        g_drone.libfpv_mod = mod;
        g_drone.libfpv_loaded = 1;
        const char* ver = g_drone.api.version_string ? g_drone.api.version_string() : "1.x";
        snprintf(g_drone.libfpv_version, sizeof(g_drone.libfpv_version), "%s", ver ? ver : "1.x");
    }

    /* Resolve writable data_dir: explicit config -> libfpv shared folder -> <plugin_dir>/fpv-data */
    if (g_drone.data_dir_cfg[0] != '\0') {
        snprintf(g_drone.resolved_data_dir, sizeof(g_drone.resolved_data_dir), "%s", g_drone.data_dir_cfg);
    } else if (g_drone.api.shared_data_dir &&
               g_drone.api.shared_data_dir(g_drone.resolved_data_dir, (uint32_t)sizeof(g_drone.resolved_data_dir)) == FPV_OK)
    {
        /* Using shared %APPDATA%\libfpv per INTEGRATION.md */
    } else {
        wchar_t dir[520];
        if (drone_get_plugin_dir_w(dir, sizeof(dir) / sizeof(dir[0]))) {
            char utf8_dir[480];
            if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, utf8_dir, (int)sizeof(utf8_dir), NULL, NULL) > 0) {
                snprintf(g_drone.resolved_data_dir, sizeof(g_drone.resolved_data_dir), "%s\\fpv-data", utf8_dir);
            }
        }
        if (g_drone.resolved_data_dir[0] == '\0') {
            snprintf(g_drone.resolved_data_dir, sizeof(g_drone.resolved_data_dir), "fpv-data");
        }
    }

    fpv_global_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (g_drone.api.global_config_defaults) {
        g_drone.api.global_config_defaults(&cfg);
    }
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.data_dir = g_drone.resolved_data_dir;
    /* RAGE 9 / GTA VI world convention: +X East (right), +Z Up, +Y North (forward), 1.0 unit/meter */
    cfg.coords.right = FPV_AXIS_POS_X;
    cfg.coords.up = FPV_AXIS_POS_Z;
    cfg.coords.forward = FPV_AXIS_POS_Y;
    cfg.coords.units_per_meter = 1.0f;
    cfg.log = drone_libfpv_log_cb;
    cfg.log_user = &g_drone;
    cfg.log_level = FPV_LOG_INFO;
    cfg.configurator_port = g_drone.configurator_port;
    cfg.timing = FPV_TIMING_THREAD;
    cfg.sim_rate_hz = g_drone.sim_rate_hz;
    cfg.auto_pause_seconds = g_drone.auto_pause_seconds;
    cfg.flags = 0u;
    if (!g_drone.enable_device_input) {
        cfg.flags |= (uint32_t)FPV_INIT_NO_DEVICE_INPUT;
    }
    if (!g_drone.enable_configurator) {
        cfg.flags |= (uint32_t)FPV_INIT_NO_CONFIGURATOR;
    }
    if (!g_drone.enable_lens_distortion) {
        cfg.flags |= (uint32_t)FPV_INIT_NO_LENS_DISTORTION;
    }
    if (g_drone.enable_video_feed) {
        cfg.flags |= (uint32_t)FPV_INIT_VIDEO_FEED;
    }
    cfg.host_name = "Fireball-Mods GTA VI FPV Drone";
    cfg.host_build = DRONE_VERSION;
    cfg.host_url = "https://github.com/Kabosune/Fireball-Mods";

    InterlockedExchange(&g_drone.log_active, 1);
    fpv_result res = g_drone.api.global_init(&cfg);
    if (res != FPV_OK && res != FPV_ERROR_ALREADY_INITIALIZED) {
        const char* detail = g_drone.api.last_error ? g_drone.api.last_error() : "";
        snprintf(
            g_drone.libfpv_error,
            sizeof(g_drone.libfpv_error),
            "fpv_global_init failed (rc=%d): %s",
            (int)res,
            detail ? detail : "");
        drone_log(FB_LOG_ERROR, g_drone.libfpv_error);
        return 0;
    }

    g_drone.libfpv_ready = 1;

    /* Material slot 1 = rough ground (concrete/asphalt/grass friction ~0.6 per INTEGRATION.md) */
    if (g_drone.api.material_set) {
        fpv_material ground;
        ground.restitution = 0.2f;
        ground.friction = 0.6f;
        ground.flags = 0u;
        (void)g_drone.api.material_set(1u, &ground);
    }

    /* Register raycast callback (prefer extended callback with moving-object pose support) */
    if (g_drone.api.world_set_raycast_callback_ex) {
        g_drone.api.world_set_raycast_callback_ex(drone_raycast_ex_cb, &g_drone);
    } else if (g_drone.api.world_set_raycast_callback) {
        g_drone.api.world_set_raycast_callback(drone_raycast_cb, &g_drone);
    }

    /* Match libfpv's overlay toggle key to our non-conflicting F9 binding */
    if (g_drone.api.overlay_set_toggle_key) {
        g_drone.api.overlay_set_toggle_key(FPV_KEY_F9);
    }

    /* Register GTA VI custom controls on the pilot's radio switches/buttons */
    if (g_drone.api.controls_set) {
        fpv_control_desc ctrls[4];
        memset(ctrls, 0, sizeof(ctrls));
        ctrls[0].struct_size = (uint32_t)sizeof(fpv_control_desc);
        ctrls[0].id = DRONE_CTRL_CYCLE_VIEW;
        ctrls[0].name = "gta6.cycle_view";
        ctrls[0].label = "Cycle Camera View";
        ctrls[0].kind = (uint32_t)FPV_CONTROL_INSTANT;
        ctrls[0].key = 0;
        ctrls[0].gamepad_button = FPV_GAMEPAD_NONE;

        ctrls[1].struct_size = (uint32_t)sizeof(fpv_control_desc);
        ctrls[1].id = DRONE_CTRL_RESPAWN;
        ctrls[1].name = "gta6.respawn";
        ctrls[1].label = "Repair & Respawn";
        ctrls[1].kind = (uint32_t)FPV_CONTROL_INSTANT;
        ctrls[1].key = 0;
        ctrls[1].gamepad_button = FPV_GAMEPAD_NONE;

        ctrls[2].struct_size = (uint32_t)sizeof(fpv_control_desc);
        ctrls[2].id = DRONE_CTRL_RECALL;
        ctrls[2].name = "gta6.recall";
        ctrls[2].label = "Recall Drone";
        ctrls[2].kind = (uint32_t)FPV_CONTROL_INSTANT;
        ctrls[2].key = 0;
        ctrls[2].gamepad_button = FPV_GAMEPAD_NONE;

        ctrls[3].struct_size = (uint32_t)sizeof(fpv_control_desc);
        ctrls[3].id = DRONE_CTRL_MOVE_PILOT;
        ctrls[3].name = "gta6.move_pilot";
        ctrls[3].label = "Move Pilot to Drone";
        ctrls[3].kind = (uint32_t)FPV_CONTROL_INSTANT;
        ctrls[3].key = 'Y';
        ctrls[3].gamepad_button = FPV_GAMEPAD_NONE;

        (void)g_drone.api.controls_set(ctrls, 4u);
    }

    /* Register host action buttons inside libfpv's Fly overlay tab */
    if (g_drone.api.overlay_set_action) {
        g_drone.api.overlay_set_action(DRONE_HOST_ACT_MOVE_PILOT, "Move Pilot to Drone (Y)");
        g_drone.api.overlay_set_action(DRONE_HOST_ACT_RESPAWN, "Repair & Respawn (Ctrl+R)");
        g_drone.api.overlay_set_action(DRONE_HOST_ACT_CYCLE_VIEW, "Cycle View (Shift+V)");
        g_drone.api.overlay_set_action(DRONE_HOST_ACT_RECALL, "Recall Drone (F7)");
    }

    /* Query Betaflight Configurator WebSocket URL */
    g_drone.configurator_url[0] = '\0';
    if (g_drone.api.configurator_get_info) {
        fpv_configurator_info ci;
        memset(&ci, 0, sizeof(ci));
        ci.struct_size = (uint32_t)sizeof(ci);
        if (g_drone.api.configurator_get_info(&ci) == FPV_OK && ci.enabled && ci.url[0]) {
            snprintf(g_drone.configurator_url, sizeof(g_drone.configurator_url), "%s", ci.url);
        }
    }

    drone_apply_sim_options();
    drone_refresh_presets();

    {
        char msg[384];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_drone] libfpv v%s initialized (data_dir='%s', presets=%u, configurator='%s')",
            g_drone.libfpv_version,
            g_drone.resolved_data_dir,
            (unsigned)g_drone.preset_count,
            g_drone.configurator_url[0] ? g_drone.configurator_url : "disabled");
        drone_log(FB_LOG_INFO, msg);
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Spawn / Recall / Respawn / View / Pilot Sync                              */
/* ------------------------------------------------------------------------- */

static void drone_read_pilot_from_title(void) {
    if (!g_drone.has_tier2_mem || g_drone.pilot_pos_va == 0ull) {
        return;
    }
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (drone_mem_read_f32(g_drone.pilot_pos_va + 0ull, &px) &&
        drone_mem_read_f32(g_drone.pilot_pos_va + 4ull, &py) &&
        drone_mem_read_f32(g_drone.pilot_pos_va + 8ull, &pz))
    {
        g_drone.pilot_pos[0] = px;
        g_drone.pilot_pos[1] = py;
        g_drone.pilot_pos[2] = pz;
        g_drone.has_pilot_pos = 1;
        float h = 0.0f;
        if (drone_mem_read_f32(g_drone.pilot_pos_va + 12ull, &h)) {
            g_drone.pilot_heading_rad = h;
        }
    }
}

static void drone_compute_spawn_pose(float out_pos[3], float out_rot[4]) {
    drone_read_pilot_from_title();
    float h = g_drone.has_pilot_pos ? g_drone.pilot_heading_rad : 0.0f;
    /* RAGE heading 0 = +Y (North), CCW positive: forward = (-sin(h), cos(h), 0) */
    float fx = -sinf(h);
    float fy = cosf(h);
    float base_x = g_drone.has_pilot_pos ? g_drone.pilot_pos[0] : 0.0f;
    float base_y = g_drone.has_pilot_pos ? g_drone.pilot_pos[1] : 0.0f;
    float base_z = g_drone.has_pilot_pos ? g_drone.pilot_pos[2] : g_drone.ground_fallback_z;

    out_pos[0] = base_x + fx * g_drone.spawn_forward_m;
    out_pos[1] = base_y + fy * g_drone.spawn_forward_m;
    out_pos[2] = base_z + g_drone.spawn_height_m;

    /* Z-axis quaternion for heading `h` */
    float half = 0.5f * h;
    out_rot[0] = 0.0f;
    out_rot[1] = 0.0f;
    out_rot[2] = sinf(half);
    out_rot[3] = cosf(half);
}

static void drone_set_active_view(uint32_t next_view, int sync_menu) {
    if (next_view > (uint32_t)FPV_VIEW_PILOT) {
        next_view = (uint32_t)FPV_VIEW_FPV;
    }
    g_drone.view = next_view;
    if (next_view != (uint32_t)FPV_VIEW_PILOT) {
        g_drone.prev_non_pilot_view = next_view;
    }
    if (g_drone.libfpv_ready && g_drone.api.set_view) {
        g_drone.api.set_view((fpv_view)next_view);
    }
    if (next_view == (uint32_t)FPV_VIEW_PILOT || !g_drone.drone_active) {
        drone_set_camera_override(0);
    }
    if (sync_menu) {
        drone_sync_menu_enum("camera_view", (int32_t)next_view);
    }
}

static int drone_launch(int sync_menu) {
    if (g_drone.drone_active && g_drone.drone) {
        return 1;
    }
    if (!drone_init_libfpv()) {
        if (sync_menu) {
            drone_sync_menu_bool("drone_active", 0);
        }
        return 0;
    }

    float spawn_pos[3];
    float spawn_rot[4];
    drone_compute_spawn_pose(spawn_pos, spawn_rot);

    fpv_drone_desc desc;
    memset(&desc, 0, sizeof(desc));
    if (g_drone.api.drone_desc_defaults) {
        g_drone.api.drone_desc_defaults(&desc);
    }
    desc.struct_size = (uint32_t)sizeof(desc);
    if (g_drone.preset_count > 0u && g_drone.preset_index >= 0 && (uint32_t)g_drone.preset_index < g_drone.preset_count) {
        desc.preset = g_drone.preset_names[g_drone.preset_index];
    } else if (g_drone.preset_cfg[0] != '\0') {
        desc.preset = g_drone.preset_cfg;
    }
    memcpy(desc.position, spawn_pos, sizeof(spawn_pos));
    memcpy(desc.rotation, spawn_rot, sizeof(spawn_rot));
    if (g_drone.has_pilot_pos) {
        memcpy(desc.pilot_position, g_drone.pilot_pos, sizeof(g_drone.pilot_pos));
    } else {
        memcpy(desc.pilot_position, spawn_pos, sizeof(spawn_pos));
    }

    fpv_drone* d = NULL;
    fpv_result res = g_drone.api.drone_create(&desc, &d);
    if (res != FPV_OK || !d) {
        const char* detail = g_drone.api.last_error ? g_drone.api.last_error() : "";
        char msg[320];
        snprintf(msg, sizeof(msg), "[fpv_drone] fpv_drone_create failed (rc=%d): %s", (int)res, detail ? detail : "");
        drone_log(FB_LOG_ERROR, msg);
        if (sync_menu) {
            drone_sync_menu_bool("drone_active", 0);
        }
        return 0;
    }

    g_drone.drone = d;
    g_drone.drone_active = 1;
    g_drone.settle_pending = 1;
    g_drone.settle_start_sim_time = -1.0;
    g_drone.frames_since_spawn = 0ull;
    g_drone.has_last_frame = 0;

    if (g_drone.api.gps_set_home) {
        (void)g_drone.api.gps_set_home(desc.pilot_position);
    }
    if (g_drone.view == (uint32_t)FPV_VIEW_PILOT) {
        drone_set_active_view(g_drone.prev_non_pilot_view, sync_menu);
    } else {
        drone_set_active_view(g_drone.view, sync_menu);
    }
    if (g_drone.api.set_paused) {
        g_drone.api.set_paused(g_drone.sim_paused);
    }
    if (g_drone.api.set_time_scale && g_drone.sim_scale_idx >= 0 && g_drone.sim_scale_idx < 5) {
        g_drone.api.set_time_scale(k_scale_vals[g_drone.sim_scale_idx]);
    }
    drone_apply_audio_settings();

    if (sync_menu) {
        drone_sync_menu_bool("drone_active", 1);
    }

    {
        char msg[256];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_drone] Launched drone at (%.2f, %.2f, %.2f) view=%s",
            (double)spawn_pos[0],
            (double)spawn_pos[1],
            (double)spawn_pos[2],
            k_view_labels[g_drone.view]);
        drone_log(FB_LOG_INFO, msg);
    }
    return 1;
}

static void drone_recall(int sync_menu) {
    drone_audio_stop();
    drone_set_camera_override(0);

    /* Restore streaming focus to pilot if available */
    if (g_drone.has_tier2_mem && g_drone.focus_pos_va != 0ull && g_drone.has_pilot_pos) {
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0ull, g_drone.pilot_pos[0]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 4ull, g_drone.pilot_pos[1]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 8ull, g_drone.pilot_pos[2]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 12ull, 0.0f);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 16ull, 0.0f);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 20ull, 0.0f);
    }

    if (g_drone.drone && g_drone.api.drone_delete) {
        (void)g_drone.api.drone_delete(g_drone.drone);
    }
    g_drone.drone = NULL;
    g_drone.drone_active = 0;
    g_drone.settle_pending = 0;
    g_drone.has_last_frame = 0;

    if (sync_menu) {
        drone_sync_menu_bool("drone_active", 0);
    }
    drone_log(FB_LOG_INFO, "[fpv_drone] Drone recalled.");
}

static void drone_respawn(void) {
    if (!g_drone.drone_active || !g_drone.drone) {
        return;
    }
    float spawn_pos[3];
    float spawn_rot[4];
    drone_compute_spawn_pose(spawn_pos, spawn_rot);
    /* Repair BEFORE reset per libfpv.h so 0.25s post-repair immunity covers water/hazards */
    if (g_drone.api.drone_repair) {
        (void)g_drone.api.drone_repair(g_drone.drone);
    }
    if (g_drone.api.drone_reset) {
        (void)g_drone.api.drone_reset(g_drone.drone, spawn_pos, spawn_rot);
    }
    drone_log(FB_LOG_INFO, "[fpv_drone] Drone repaired & respawned.");
}

static void drone_move_pilot_to_drone(void) {
    if (!g_drone.drone_active || !g_drone.has_last_frame) {
        return;
    }
    float dst[3];
    dst[0] = g_drone.last_frame.position[0];
    dst[1] = g_drone.last_frame.position[1];
    dst[2] = g_drone.last_frame.position[2] + 0.5f;
    if (g_drone.has_tier2_mem && g_drone.pilot_pos_va != 0ull) {
        (void)drone_mem_write_f32(g_drone.pilot_pos_va + 0ull, dst[0]);
        (void)drone_mem_write_f32(g_drone.pilot_pos_va + 4ull, dst[1]);
        (void)drone_mem_write_f32(g_drone.pilot_pos_va + 8ull, dst[2]);
        g_drone.pilot_pos[0] = dst[0];
        g_drone.pilot_pos[1] = dst[1];
        g_drone.pilot_pos[2] = dst[2];
        g_drone.has_pilot_pos = 1;
        if (g_drone.api.drone_set_pilot_position && g_drone.drone) {
            (void)g_drone.api.drone_set_pilot_position(g_drone.drone, dst);
        }
        drone_log(FB_LOG_INFO, "[fpv_drone] Moved pilot position to drone and recalling drone.");
    }
    drone_recall(1);
}

static void drone_toggle_overlay(int sync_menu) {
    if (!g_drone.libfpv_ready && !drone_init_libfpv()) {
        if (sync_menu) {
            drone_sync_menu_bool("overlay_open", 0);
        }
        return;
    }
    int next = g_drone.overlay_open ? 0 : 1;
    g_drone.overlay_open = next;
    if (g_drone.api.overlay_set_open) {
        g_drone.api.overlay_set_open(next);
    }
    if (sync_menu) {
        drone_sync_menu_bool("overlay_open", next);
    }
}

/* ------------------------------------------------------------------------- */
/* Title memory writers (camera matrix/quat/Euler, focus, drone pose)        */
/* ------------------------------------------------------------------------- */

static void drone_decompose_rage_euler_deg(const fpv_camera* cam, float* out_pitch, float* out_roll, float* out_yaw) {
    /*
     * RAGE (RDR2 / GTA) Z-up camera Euler order: Rz(yaw) * Rx(pitch) * Ry(roll)
     * with heading 0 = +Y (North), counter-clockwise positive.
     * Handles zenith/nadir singularity by folding into yaw (roll = 0).
     */
    float fz = cam->forward[2];
    if (fz > 1.0f) {
        fz = 1.0f;
    } else if (fz < -1.0f) {
        fz = -1.0f;
    }
    float pitch_rad = asinf(fz);
    float yaw_rad = 0.0f;
    float roll_rad = 0.0f;
    if (fabsf(fz) < 0.9995f) {
        yaw_rad = atan2f(-cam->forward[0], cam->forward[1]);
        roll_rad = atan2f(-cam->right[2], cam->up[2]);
    } else {
        yaw_rad = atan2f(cam->right[1], cam->right[0]);
        roll_rad = 0.0f;
    }
    const float rad2deg = (float)(180.0 / DRONE_PI);
    *out_pitch = pitch_rad * rad2deg;
    *out_roll = roll_rad * rad2deg;
    *out_yaw = yaw_rad * rad2deg;
}

static void drone_write_camera_to_title(const fpv_camera* cam) {
    if (!g_drone.has_tier2_mem || g_drone.camera_va == 0ull || !cam) {
        return;
    }
    uint64_t va = g_drone.camera_va;
    int ok = 1;
    if (g_drone.camera_layout == DRONE_CAM_MAT34_FOV) {
        ok &= drone_mem_write_f32(va + 0x00ull, cam->right[0]);
        ok &= drone_mem_write_f32(va + 0x04ull, cam->right[1]);
        ok &= drone_mem_write_f32(va + 0x08ull, cam->right[2]);
        ok &= drone_mem_write_f32(va + 0x0Cull, cam->forward[0]);
        ok &= drone_mem_write_f32(va + 0x10ull, cam->forward[1]);
        ok &= drone_mem_write_f32(va + 0x14ull, cam->forward[2]);
        ok &= drone_mem_write_f32(va + 0x18ull, cam->up[0]);
        ok &= drone_mem_write_f32(va + 0x1Cull, cam->up[1]);
        ok &= drone_mem_write_f32(va + 0x20ull, cam->up[2]);
        ok &= drone_mem_write_f32(va + 0x24ull, cam->position[0]);
        ok &= drone_mem_write_f32(va + 0x28ull, cam->position[1]);
        ok &= drone_mem_write_f32(va + 0x2Cull, cam->position[2]);
        ok &= drone_mem_write_f32(va + 0x30ull, cam->fov_y_degrees);
        ok &= drone_mem_write_f32(va + 0x34ull, cam->near_plane);
    } else if (g_drone.camera_layout == DRONE_CAM_POS_QUAT_FOV) {
        ok &= drone_mem_write_f32(va + 0x00ull, cam->position[0]);
        ok &= drone_mem_write_f32(va + 0x04ull, cam->position[1]);
        ok &= drone_mem_write_f32(va + 0x08ull, cam->position[2]);
        ok &= drone_mem_write_f32(va + 0x0Cull, cam->rotation[0]);
        ok &= drone_mem_write_f32(va + 0x10ull, cam->rotation[1]);
        ok &= drone_mem_write_f32(va + 0x14ull, cam->rotation[2]);
        ok &= drone_mem_write_f32(va + 0x18ull, cam->rotation[3]);
        ok &= drone_mem_write_f32(va + 0x1Cull, cam->fov_y_degrees);
        ok &= drone_mem_write_f32(va + 0x20ull, cam->near_plane);
    } else {
        float pitch = 0.0f, roll = 0.0f, yaw = 0.0f;
        drone_decompose_rage_euler_deg(cam, &pitch, &roll, &yaw);
        ok &= drone_mem_write_f32(va + 0x00ull, cam->position[0]);
        ok &= drone_mem_write_f32(va + 0x04ull, cam->position[1]);
        ok &= drone_mem_write_f32(va + 0x08ull, cam->position[2]);
        ok &= drone_mem_write_f32(va + 0x0Cull, pitch);
        ok &= drone_mem_write_f32(va + 0x10ull, roll);
        ok &= drone_mem_write_f32(va + 0x14ull, yaw);
        ok &= drone_mem_write_f32(va + 0x18ull, cam->fov_y_degrees);
        ok &= drone_mem_write_f32(va + 0x1Cull, cam->near_plane);
    }
    if (ok) {
        g_drone.camera_writes += 1ull;
    }
}

static void drone_write_pose_and_focus(const fpv_frame* frame) {
    if (!g_drone.has_tier2_mem || !frame) {
        return;
    }
    if (g_drone.focus_pos_va != 0ull) {
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x00ull, frame->position[0]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x04ull, frame->position[1]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x08ull, frame->position[2]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x0Cull, frame->velocity[0]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x10ull, frame->velocity[1]);
        (void)drone_mem_write_f32(g_drone.focus_pos_va + 0x14ull, frame->velocity[2]);
    }
    if (g_drone.drone_pose_va != 0ull) {
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x00ull, frame->position[0]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x04ull, frame->position[1]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x08ull, frame->position[2]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x0Cull, frame->rotation[0]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x10ull, frame->rotation[1]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x14ull, frame->rotation[2]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x18ull, frame->rotation[3]);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x1Cull, frame->motors[0].rpm);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x20ull, frame->motors[1].rpm);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x24ull, frame->motors[2].rpm);
        (void)drone_mem_write_f32(g_drone.drone_pose_va + 0x28ull, frame->motors[3].rpm);
        if (g_drone.core.title_mem_write_u32) {
            (void)g_drone.core.title_mem_write_u32(g_drone.drone_pose_va + 0x2Cull, frame->flags);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Keyboard VK -> physical US QWERTY `fpv_key` translation                   */
/* ------------------------------------------------------------------------- */

static int drone_vk_to_fpv_key(uint32_t vk) {
    /* Non-layout special keys */
    switch (vk) {
    case VK_TAB:     return FPV_KEY_TAB;
    case VK_RETURN:  return FPV_KEY_ENTER;
    case VK_ESCAPE:  return FPV_KEY_ESCAPE;
    case VK_SPACE:   return FPV_KEY_SPACE;
    case VK_LEFT:    return FPV_KEY_LEFT;
    case VK_RIGHT:   return FPV_KEY_RIGHT;
    case VK_UP:      return FPV_KEY_UP;
    case VK_DOWN:    return FPV_KEY_DOWN;
    case VK_PRIOR:   return FPV_KEY_PAGE_UP;
    case VK_NEXT:    return FPV_KEY_PAGE_DOWN;
    case VK_HOME:    return FPV_KEY_HOME;
    case VK_END:     return FPV_KEY_END;
    case VK_INSERT:  return FPV_KEY_INSERT;
    case VK_DELETE:  return FPV_KEY_DELETE;
    case VK_BACK:    return FPV_KEY_BACKSPACE;
    case VK_LSHIFT:  return FPV_KEY_LSHIFT;
    case VK_RSHIFT:  return FPV_KEY_RSHIFT;
    case VK_SHIFT:   return FPV_KEY_LSHIFT;
    case VK_LCONTROL:return FPV_KEY_LCTRL;
    case VK_RCONTROL:return FPV_KEY_RCTRL;
    case VK_CONTROL: return FPV_KEY_LCTRL;
    case VK_LMENU:   return FPV_KEY_LALT;
    case VK_RMENU:   return FPV_KEY_RALT;
    case VK_MENU:    return FPV_KEY_LALT;
    case VK_F1:      return FPV_KEY_F1;
    case VK_F2:      return FPV_KEY_F2;
    case VK_F3:      return FPV_KEY_F3;
    case VK_F4:      return FPV_KEY_F4;
    case VK_F5:      return FPV_KEY_F5;
    case VK_F6:      return FPV_KEY_F6;
    case VK_F7:      return FPV_KEY_F7;
    case VK_F8:      return FPV_KEY_F8;
    case VK_F9:      return FPV_KEY_F9;
    case VK_F10:     return FPV_KEY_F10;
    case VK_F11:     return FPV_KEY_F11;
    case VK_F12:     return FPV_KEY_F12;
    default: break;
    }

    /* Map letters/digits by physical keyboard scancode (per libfpv INTEGRATION.md) */
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (sc) {
    case 0x02: return '1';
    case 0x03: return '2';
    case 0x04: return '3';
    case 0x05: return '4';
    case 0x06: return '5';
    case 0x07: return '6';
    case 0x08: return '7';
    case 0x09: return '8';
    case 0x0A: return '9';
    case 0x0B: return '0';
    case 0x10: return 'Q';
    case 0x11: return 'W';
    case 0x12: return 'E';
    case 0x13: return 'R';
    case 0x14: return 'T';
    case 0x15: return 'Y';
    case 0x16: return 'U';
    case 0x17: return 'I';
    case 0x18: return 'O';
    case 0x19: return 'P';
    case 0x1E: return 'A';
    case 0x1F: return 'S';
    case 0x20: return 'D';
    case 0x21: return 'F';
    case 0x22: return 'G';
    case 0x23: return 'H';
    case 0x24: return 'J';
    case 0x25: return 'K';
    case 0x26: return 'L';
    case 0x2C: return 'Z';
    case 0x2D: return 'X';
    case 0x2E: return 'C';
    case 0x2F: return 'V';
    case 0x30: return 'B';
    case 0x31: return 'N';
    case 0x32: return 'M';
    default: break;
    }

    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return (int)vk;
    }
    return FPV_KEY_NONE;
}

/* ------------------------------------------------------------------------- */
/* Event bus handlers (`FB_EVENT_INPUT_EVENT`, `FB_EVENT_FRAME_BEGIN`, ...)  */
/* ------------------------------------------------------------------------- */

static int drone_handle_input(const fb_input_event_v1_t* ev) {
    if (!ev || ev->struct_size < sizeof(fb_input_event_v1_t)) {
        return 0;
    }
    if (ev->device_kind != FB_DEV_KEYBOARD) {
        return 0;
    }

    uint32_t mods = ev->modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT);

    /* Graceful v1 fallback when Mod Menu Action Binding Manager is absent */
    if (!g_drone.menu_registered && ev->action == (uint32_t)FB_INPUT_PRESS) {
        if (mods == FB_MOD_NONE && ev->code == g_drone.key_toggle_drone) {
            if (g_drone.drone_active) {
                drone_recall(0);
            } else {
                (void)drone_launch(0);
            }
            return 1;
        }
        if (mods == FB_MOD_SHIFT && ev->code == g_drone.key_cycle_view && g_drone.drone_active) {
            drone_set_active_view((g_drone.view + 1u) % 4u, 0);
            return 1;
        }
        if (mods == FB_MOD_CTRL && ev->code == g_drone.key_respawn && g_drone.drone_active) {
            drone_respawn();
            return 1;
        }
        if (mods == FB_MOD_CTRL && ev->code == g_drone.key_pilot_view && g_drone.drone_active) {
            uint32_t next = (g_drone.view == (uint32_t)FPV_VIEW_PILOT)
                                ? g_drone.prev_non_pilot_view
                                : (uint32_t)FPV_VIEW_PILOT;
            drone_set_active_view(next, 0);
            return 1;
        }
        if (mods == FB_MOD_NONE && ev->code == g_drone.key_overlay) {
            drone_toggle_overlay(0);
            return 1;
        }
    }

    /* Forward physical keys to libfpv when overlay is open or drone is active */
    if (g_drone.libfpv_ready && g_drone.api.input_key && (g_drone.drone_active || g_drone.overlay_open)) {
        if (g_drone.menu_registered && g_drone.menu.is_menu_open && g_drone.menu.is_menu_open()) {
            return 0; /* Do not steal keys while Fireball Mod Menu itself is open */
        }
        int fk = drone_vk_to_fpv_key(ev->code);
        if (fk != FPV_KEY_NONE && (ev->action == (uint32_t)FB_INPUT_PRESS || ev->action == (uint32_t)FB_INPUT_RELEASE)) {
            g_drone.api.input_key(fk, ev->action == (uint32_t)FB_INPUT_PRESS ? 1 : 0);
            int wants = g_drone.api.overlay_wants_input ? g_drone.api.overlay_wants_input() : 0;
            if (wants) {
                return 1;
            }
            /* While flying in drone camera views, consume flight sticks (WASD, arrows, Enter, T) */
            if (g_drone.drone_active && g_drone.view != (uint32_t)FPV_VIEW_PILOT) {
                if (fk == 'W' || fk == 'A' || fk == 'S' || fk == 'D' || fk == 'T' ||
                    fk == FPV_KEY_UP || fk == FPV_KEY_DOWN || fk == FPV_KEY_LEFT || fk == FPV_KEY_RIGHT ||
                    fk == FPV_KEY_ENTER)
                {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void drone_update_wind(void) {
    if (!g_drone.libfpv_ready || !g_drone.api.wind_set) {
        return;
    }
    float wx = g_drone.ambient_wind[0];
    float wy = g_drone.ambient_wind[1];
    float wz = g_drone.ambient_wind[2];
    if (g_drone.has_tier2_mem && g_drone.wind_va != 0ull) {
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        if (drone_mem_read_f32(g_drone.wind_va + 0ull, &rx) &&
            drone_mem_read_f32(g_drone.wind_va + 4ull, &ry) &&
            drone_mem_read_f32(g_drone.wind_va + 8ull, &rz))
        {
            wx = rx;
            wy = ry;
            wz = rz;
        }
    }
    fpv_wind w;
    memset(&w, 0, sizeof(w));
    if (g_drone.api.wind_defaults) {
        g_drone.api.wind_defaults(&w);
    }
    w.struct_size = (uint32_t)sizeof(w);
    w.velocity[0] = wx;
    w.velocity[1] = wy;
    w.velocity[2] = wz;
    w.turbulence = g_drone.ambient_wind_turb;
    (void)g_drone.api.wind_set(&w);
}

static void drone_handle_frame_begin(const fb_frame_event_v1_t* fe) {
    g_drone.frames_seen += 1ull;
    float dt = fe ? (float)fe->dt_sec : (1.0f / 60.0f);
    if (!(dt > 0.0f && dt <= 0.5f)) {
        dt = 1.0f / 60.0f;
    }

    if (!g_drone.libfpv_ready) {
        return;
    }

    /* Keep overlay open flag synchronized if libfpv closed it via Esc */
    if (g_drone.api.overlay_is_open) {
        int cur_open = g_drone.api.overlay_is_open() ? 1 : 0;
        if (cur_open != g_drone.overlay_open) {
            g_drone.overlay_open = cur_open;
            drone_sync_menu_bool("overlay_open", cur_open);
        }
    }

    /* Always call overlay_render every frame (collision x-ray draws even when closed) */
    if (g_drone.api.overlay_render) {
        (void)g_drone.api.overlay_render(dt);
    }

    /* Poll libfpv overlay host action buttons */
    if (g_drone.api.overlay_poll_action) {
        uint32_t act_id = 0u;
        while ((act_id = g_drone.api.overlay_poll_action()) != 0u) {
            if (act_id == DRONE_HOST_ACT_MOVE_PILOT) {
                drone_move_pilot_to_drone();
            } else if (act_id == DRONE_HOST_ACT_RESPAWN) {
                drone_respawn();
            } else if (act_id == DRONE_HOST_ACT_CYCLE_VIEW && g_drone.drone_active) {
                drone_set_active_view((g_drone.view + 1u) % 4u, 1);
            } else if (act_id == DRONE_HOST_ACT_RECALL && g_drone.drone_active) {
                drone_recall(1);
            }
        }
    }

    /* Poll custom radio controls */
    if (g_drone.api.control_poll && g_drone.drone_active) {
        uint32_t presses = 0u;
        (void)g_drone.api.control_poll(DRONE_CTRL_CYCLE_VIEW, &presses);
        if (presses > 0u) {
            drone_set_active_view((g_drone.view + 1u) % 4u, 1);
        }
        presses = 0u;
        (void)g_drone.api.control_poll(DRONE_CTRL_RESPAWN, &presses);
        if (presses > 0u) {
            drone_respawn();
        }
        presses = 0u;
        (void)g_drone.api.control_poll(DRONE_CTRL_MOVE_PILOT, &presses);
        if (presses > 0u) {
            drone_move_pilot_to_drone();
        }
        presses = 0u;
        (void)g_drone.api.control_poll(DRONE_CTRL_RECALL, &presses);
        if (presses > 0u) {
            drone_recall(1);
        }
    }

    if (!g_drone.drone_active || !g_drone.drone) {
        return;
    }

    /* Sync pilot feet position and world wind into libfpv before ticking */
    drone_read_pilot_from_title();
    if (g_drone.has_pilot_pos && g_drone.api.drone_set_pilot_position) {
        (void)g_drone.api.drone_set_pilot_position(g_drone.drone, g_drone.pilot_pos);
    }
    drone_update_wind();

    fpv_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = (uint32_t)sizeof(frame);
    if (g_drone.api.drone_tick(g_drone.drone, dt, &frame) != FPV_OK) {
        return;
    }

    g_drone.frames_since_spawn += 1ull;
    g_drone.last_frame = frame;
    g_drone.has_last_frame = 1;

    if (g_drone.api.input_get_status) {
        memset(&g_drone.last_input_status, 0, sizeof(g_drone.last_input_status));
        g_drone.last_input_status.struct_size = (uint32_t)sizeof(g_drone.last_input_status);
        (void)g_drone.api.input_get_status(&g_drone.last_input_status);
    }

    /* 50 ms post-spawn settle guard (per libfpv INTEGRATION.md) */
    if (g_drone.settle_pending) {
        if (g_drone.settle_start_sim_time < 0.0) {
            g_drone.settle_start_sim_time = frame.sim_time;
        }
        if ((frame.sim_time - g_drone.settle_start_sim_time) < 0.05) {
            return;
        }
        g_drone.settle_pending = 0;
    }

    /* Write active drone camera to guarded title memory */
    if (g_drone.view != (uint32_t)FPV_VIEW_PILOT) {
        const fpv_camera* cam = &frame.camera;
        if (g_drone.view == (uint32_t)FPV_VIEW_CHASE) {
            cam = &frame.chase_camera;
        } else if (g_drone.view == (uint32_t)FPV_VIEW_LOS) {
            cam = &frame.los_camera;
        }
        drone_set_camera_override(1);
        drone_write_camera_to_title(cam);
    } else {
        drone_set_camera_override(0);
    }

    drone_write_pose_and_focus(&frame);

    /* Push live Betaflight MAX7456 OSD snapshot to optional Win32 preview window */
    if (drone_osd_window_is_open()) {
        static drone_osd_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        snap.valid = 1;
        snap.drone_active = g_drone.drone_active;
        snap.view = g_drone.view;
        snap.osd_style = g_drone.osd_style;
        snap.osd_revision = frame.osd_revision;
        snap.flags = frame.flags;
        snap.speed_kmh = frame.speed_kmh;
        snap.altitude = frame.altitude;
        snap.voltage = frame.battery.voltage;
        snap.cell_voltage = frame.battery.cell_voltage;
        snap.cell_count = frame.battery.cell_count;
        snap.signal_strength = frame.video.signal_strength;
        snprintf(snap.arming_disable_text, sizeof(snap.arming_disable_text), "%s", frame.arming_disable_text);
        snprintf(snap.configurator_url, sizeof(snap.configurator_url), "%s", g_drone.configurator_url);
        if (g_drone.api.osd_render_rgba && !(frame.flags & FPV_DRONE_OSD_DISABLED)) {
            if (g_drone.api.osd_render_rgba(
                    &frame,
                    snap.rgba,
                    DRONE_OSD_W,
                    DRONE_OSD_H,
                    DRONE_OSD_W * 4u,
                    (fpv_osd_style)g_drone.osd_style) == FPV_OK)
            {
                snap.has_rgba = 1;
            }
        }
        drone_osd_window_push_snapshot(&snap);
    }
}

static int drone_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_drone.initialized) {
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_INPUT_EVENT) {
        return drone_handle_input((const fb_input_event_v1_t*)payload);
    }
    if (event_id == (uint32_t)FB_EVENT_FRAME_BEGIN) {
        drone_handle_frame_begin((const fb_frame_event_v1_t*)payload);
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_SHUTDOWN) {
        fb_plugin_shutdown();
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Fireball Mod Menu callbacks (`include/fireball_menu.h`)                   */
/* ------------------------------------------------------------------------- */

static void drone_on_action(const char* action_id, int active, void* user_ctx) {
    (void)user_ctx;
    if (!g_drone.initialized || !active || !action_id) {
        return;
    }
    if (strcmp(action_id, "toggle_drone") == 0) {
        if (g_drone.drone_active) {
            drone_recall(1);
        } else {
            (void)drone_launch(1);
        }
    } else if (strcmp(action_id, "cycle_view") == 0) {
        if (g_drone.drone_active) {
            drone_set_active_view((g_drone.view + 1u) % 4u, 1);
        }
    } else if (strcmp(action_id, "respawn_drone") == 0) {
        drone_respawn();
    } else if (strcmp(action_id, "toggle_pilot_view") == 0) {
        if (g_drone.drone_active) {
            uint32_t next = (g_drone.view == (uint32_t)FPV_VIEW_PILOT)
                                ? g_drone.prev_non_pilot_view
                                : (uint32_t)FPV_VIEW_PILOT;
            drone_set_active_view(next, 1);
        }
    } else if (strcmp(action_id, "toggle_overlay") == 0) {
        drone_toggle_overlay(1);
    }
}

static void drone_on_setting_change(const char* setting_id, const fb_setting_value_t* val, void* user_ctx) {
    (void)user_ctx;
    if (!setting_id || !val) {
        return;
    }
    if (strcmp(setting_id, "drone_active") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        /* Apply only for live menu edits (never auto-launch drone on boot from stale config) */
        if (g_drone.initialized && g_drone.menu_registered) {
            if (val->v.bool_val) {
                (void)drone_launch(0);
            } else {
                drone_recall(0);
            }
        }
    } else if (strcmp(setting_id, "camera_view") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        if (g_drone.initialized && g_drone.menu_registered) {
            drone_set_active_view((uint32_t)val->v.enum_val, 0);
        }
    } else if (strcmp(setting_id, "preset_index") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        if (val->v.enum_val >= 0 && (uint32_t)val->v.enum_val < g_drone.preset_count) {
            g_drone.preset_index = val->v.enum_val;
            if (g_drone.drone_active && g_drone.drone && g_drone.api.drone_set_preset) {
                (void)g_drone.api.drone_set_preset(g_drone.drone, g_drone.preset_names[g_drone.preset_index]);
            }
        }
    } else if (strcmp(setting_id, "overlay_open") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        if (g_drone.initialized && g_drone.menu_registered) {
            int want = val->v.bool_val ? 1 : 0;
            if (want != g_drone.overlay_open) {
                drone_toggle_overlay(0);
            }
        }
    } else if (strcmp(setting_id, "no_battery_drain") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_drone.no_battery_drain = val->v.bool_val ? 1 : 0;
        drone_apply_sim_options();
    } else if (strcmp(setting_id, "no_damage") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_drone.no_damage = val->v.bool_val ? 1 : 0;
        drone_apply_sim_options();
    } else if (strcmp(setting_id, "sim_paused") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        if (g_drone.initialized && g_drone.menu_registered) {
            g_drone.sim_paused = val->v.bool_val ? 1 : 0;
            if (g_drone.libfpv_ready && g_drone.api.set_paused) {
                g_drone.api.set_paused(g_drone.sim_paused);
            }
        }
    } else if (strcmp(setting_id, "sim_time_scale") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        if (val->v.enum_val >= 0 && val->v.enum_val < 5) {
            g_drone.sim_scale_idx = val->v.enum_val;
            if (g_drone.libfpv_ready && g_drone.api.set_time_scale) {
                g_drone.api.set_time_scale(k_scale_vals[g_drone.sim_scale_idx]);
            }
        }
    } else if (strcmp(setting_id, "audio_enabled") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_drone.audio_enabled = val->v.bool_val ? 1 : 0;
        drone_apply_audio_settings();
    } else if (strcmp(setting_id, "audio_volume") == 0 && val->type == (uint32_t)FB_SETTING_FLOAT) {
        g_drone.audio_volume = (float)val->v.float_val;
        drone_apply_audio_settings();
    } else if (strcmp(setting_id, "audio_beep_volume") == 0 && val->type == (uint32_t)FB_SETTING_FLOAT) {
        g_drone.audio_beep_volume = (float)val->v.float_val;
        drone_apply_audio_settings();
    } else if (strcmp(setting_id, "osd_style") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        g_drone.osd_style = (val->v.enum_val == 1) ? (uint32_t)FPV_OSD_STYLE_HD : (uint32_t)FPV_OSD_STYLE_ANALOG;
    } else if (strcmp(setting_id, "osd_window_enabled") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_drone.osd_window_enabled = val->v.bool_val ? 1 : 0;
        drone_osd_window_set_visible(g_drone.osd_window_enabled);
    }
}

static void drone_on_button(const char* setting_id, void* user_ctx) {
    (void)user_ctx;
    if (!g_drone.initialized || !setting_id) {
        return;
    }
    if (strcmp(setting_id, "respawn_btn") == 0) {
        drone_respawn();
    } else if (strcmp(setting_id, "move_pilot_btn") == 0) {
        drone_move_pilot_to_drone();
    } else if (strcmp(setting_id, "reload_libfpv_btn") == 0) {
        (void)drone_init_libfpv();
    }
}

static const char* drone_input_source_name(fpv_input_source src) {
    switch (src) {
    case FPV_INPUT_SOURCE_RADIO:    return "Radio";
    case FPV_INPUT_SOURCE_GAMEPAD:  return "Gamepad";
    case FPV_INPUT_SOURCE_KEYBOARD: return "Keyboard";
    case FPV_INPUT_SOURCE_HOST:     return "Host";
    default:                        return "None";
    }
}

static void drone_on_status(const char* setting_id, char* out_buf, size_t out_cap, void* user_ctx) {
    (void)user_ctx;
    if (!out_buf || out_cap == 0u || !setting_id) {
        return;
    }
    if (strcmp(setting_id, "drone_status") == 0) {
        if (!g_drone.libfpv_ready) {
            snprintf(
                out_buf,
                out_cap,
                "libfpv unavailable | %s",
                g_drone.libfpv_error[0] ? g_drone.libfpv_error : "place libfpv.dll next to plugin");
            return;
        }
        if (!g_drone.drone_active) {
            snprintf(
                out_buf,
                out_cap,
                "Standby | libfpv v%s | BF=%s",
                g_drone.libfpv_version,
                g_drone.configurator_url[0] ? g_drone.configurator_url : "off");
            return;
        }
        const char* arm_str = "DISARMED";
        if (g_drone.has_last_frame) {
            if (g_drone.last_frame.flags & FPV_DRONE_CRASHED) {
                arm_str = "CRASHED";
            } else if (g_drone.last_frame.flags & FPV_DRONE_ARMED) {
                arm_str = "ARMED";
            }
        }
        snprintf(
            out_buf,
            out_cap,
            "%s | view=%s | %.1f km/h | %.2fV | cam_writes=%llu",
            arm_str,
            k_view_labels[g_drone.view],
            g_drone.has_last_frame ? (double)g_drone.last_frame.speed_kmh : 0.0,
            g_drone.has_last_frame ? (double)g_drone.last_frame.battery.voltage : 0.0,
            (unsigned long long)g_drone.camera_writes);
        return;
    }

    if (strcmp(setting_id, "link_status") == 0) {
        const char* preset = (g_drone.preset_count > 0u && g_drone.preset_index >= 0 &&
                              (uint32_t)g_drone.preset_index < g_drone.preset_count)
                                 ? g_drone.preset_names[g_drone.preset_index]
                                 : "Default";
        const char* arm_msg = (g_drone.has_last_frame && g_drone.last_frame.arming_disable_text[0])
                                  ? g_drone.last_frame.arming_disable_text
                                  : "READY";
        snprintf(
            out_buf,
            out_cap,
            "Preset=%s | Input=%s | Arm=%s | Rays=%llu",
            preset,
            drone_input_source_name(g_drone.last_input_status.source),
            arm_msg,
            (unsigned long long)g_drone.raycast_calls);
    }
}

/* ------------------------------------------------------------------------- */
/* Plugin ABI entry points                                                   */
/* ------------------------------------------------------------------------- */

FB_API int fb_plugin_query(fb_plugin_info_v1_t* out_info) {
    if (!out_info) {
        return FB_ERR_INVALID_ARG;
    }
    out_info->struct_size = (uint32_t)sizeof(fb_plugin_info_v1_t);
    out_info->abi_version = FB_PLUGIN_ABI_VERSION;
    out_info->mod_id = DRONE_MOD_ID;
    out_info->plugin_id = DRONE_PLUGIN_ID;
    out_info->version = DRONE_VERSION;
    out_info->required_caps = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS;
    return FB_OK;
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core) {
        return FB_ERR_INVALID_ARG;
    }
    if (core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size != sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!core->register_event_listener) {
        return FB_ERR_UNSUPPORTED;
    }

    memset(&g_drone, 0, sizeof(g_drone));
    g_drone.core = *core;
    InitializeSRWLock(&g_drone.log_lock);
    g_drone.log_lock_init = 1;

    /* Defaults matching mod.toml */
    g_drone.enable_configurator = 1;
    g_drone.configurator_port = 5761;
    g_drone.enable_device_input = 1;
    g_drone.enable_lens_distortion = 0;
    g_drone.enable_video_feed = 0;
    g_drone.sim_rate_hz = 1000.0f;
    g_drone.auto_pause_seconds = 0.5f;
    g_drone.no_battery_drain = 0;
    g_drone.no_damage = 0;
    g_drone.audio_enabled = 1;
    g_drone.audio_volume = 1.0f;
    g_drone.audio_beep_volume = 1.0f;
    g_drone.osd_window_enabled = 0;
    g_drone.osd_style = (uint32_t)FPV_OSD_STYLE_ANALOG;
    g_drone.spawn_forward_m = 2.0f;
    g_drone.spawn_height_m = 0.25f;
    g_drone.use_ground_fallback = 1;
    g_drone.ground_fallback_z = 0.0f;
    g_drone.ambient_wind_turb = -1.0f;

    snprintf(g_drone.camera_symbol, sizeof(g_drone.camera_symbol), "%s", "fb.drone_camera");
    g_drone.camera_layout = DRONE_CAM_MAT34_FOV;
    snprintf(g_drone.camera_override_symbol, sizeof(g_drone.camera_override_symbol), "%s", "fb.drone_camera_override");
    snprintf(g_drone.pilot_pos_symbol, sizeof(g_drone.pilot_pos_symbol), "%s", "fb.pilot_pos");
    snprintf(g_drone.focus_pos_symbol, sizeof(g_drone.focus_pos_symbol), "%s", "fb.streaming_focus");
    snprintf(g_drone.drone_pose_symbol, sizeof(g_drone.drone_pose_symbol), "%s", "fb.drone_pose");
    snprintf(g_drone.wind_symbol, sizeof(g_drone.wind_symbol), "%s", "fb.wind");

    g_drone.key_toggle_drone = 0x76u; /* F7 */
    g_drone.key_cycle_view = 0x56u;   /* Shift + V */
    g_drone.key_respawn = 0x52u;      /* Ctrl + R */
    g_drone.key_pilot_view = 0x46u;   /* Ctrl + F */
    g_drone.key_overlay = 0x78u;      /* F9 */
    g_drone.view = (uint32_t)FPV_VIEW_FPV;
    g_drone.prev_non_pilot_view = (uint32_t)FPV_VIEW_FPV;

    if (core->config_get_json) {
        char cfg_json[4096];
        memset(cfg_json, 0, sizeof(cfg_json));
        if (core->config_get_json(DRONE_MOD_ID, cfg_json, sizeof(cfg_json)) == FB_OK) {
            drone_json_str(cfg_json, "libfpv_dll_path", g_drone.dll_path_cfg, sizeof(g_drone.dll_path_cfg));
            drone_json_str(cfg_json, "data_dir", g_drone.data_dir_cfg, sizeof(g_drone.data_dir_cfg));
            drone_json_str(cfg_json, "preset", g_drone.preset_cfg, sizeof(g_drone.preset_cfg));
            drone_json_str(cfg_json, "log_path", g_drone.log_path, sizeof(g_drone.log_path));
            drone_json_bool(cfg_json, "enable_configurator", &g_drone.enable_configurator);
            drone_json_int(cfg_json, "configurator_port", &g_drone.configurator_port);
            drone_json_bool(cfg_json, "enable_device_input", &g_drone.enable_device_input);
            drone_json_bool(cfg_json, "enable_lens_distortion", &g_drone.enable_lens_distortion);
            drone_json_bool(cfg_json, "enable_video_feed", &g_drone.enable_video_feed);
            drone_json_float(cfg_json, "sim_rate_hz", &g_drone.sim_rate_hz);
            drone_json_float(cfg_json, "auto_pause_seconds", &g_drone.auto_pause_seconds);
            drone_json_bool(cfg_json, "no_battery_drain", &g_drone.no_battery_drain);
            drone_json_bool(cfg_json, "no_damage", &g_drone.no_damage);
            drone_json_bool(cfg_json, "audio_enabled", &g_drone.audio_enabled);
            drone_json_float(cfg_json, "audio_volume", &g_drone.audio_volume);
            drone_json_float(cfg_json, "audio_beep_volume", &g_drone.audio_beep_volume);
            drone_json_bool(cfg_json, "osd_window_enabled", &g_drone.osd_window_enabled);
            {
                char style_s[32];
                style_s[0] = '\0';
                drone_json_str(cfg_json, "osd_style", style_s, sizeof(style_s));
                if (strcmp(style_s, "hd") == 0 || strcmp(style_s, "HD") == 0) {
                    g_drone.osd_style = (uint32_t)FPV_OSD_STYLE_HD;
                }
            }
            drone_json_float(cfg_json, "spawn_forward_m", &g_drone.spawn_forward_m);
            drone_json_float(cfg_json, "spawn_height_m", &g_drone.spawn_height_m);
            drone_json_bool(cfg_json, "use_ground_fallback", &g_drone.use_ground_fallback);
            drone_json_float(cfg_json, "ground_fallback_z", &g_drone.ground_fallback_z);
            drone_json_float(cfg_json, "ambient_wind_x", &g_drone.ambient_wind[0]);
            drone_json_float(cfg_json, "ambient_wind_y", &g_drone.ambient_wind[1]);
            drone_json_float(cfg_json, "ambient_wind_z", &g_drone.ambient_wind[2]);
            drone_json_float(cfg_json, "ambient_wind_turbulence", &g_drone.ambient_wind_turb);

            drone_json_str(cfg_json, "camera_symbol", g_drone.camera_symbol, sizeof(g_drone.camera_symbol));
            {
                char layout_s[32];
                layout_s[0] = '\0';
                drone_json_str(cfg_json, "camera_layout", layout_s, sizeof(layout_s));
                if (strcmp(layout_s, "pos_quat_fov") == 0) {
                    g_drone.camera_layout = DRONE_CAM_POS_QUAT_FOV;
                } else if (strcmp(layout_s, "pos_euler_fov") == 0) {
                    g_drone.camera_layout = DRONE_CAM_POS_EULER_FOV;
                } else if (strcmp(layout_s, "mat34_fov") == 0) {
                    g_drone.camera_layout = DRONE_CAM_MAT34_FOV;
                }
            }
            drone_json_str(cfg_json, "camera_override_symbol", g_drone.camera_override_symbol, sizeof(g_drone.camera_override_symbol));
            drone_json_str(cfg_json, "pilot_pos_symbol", g_drone.pilot_pos_symbol, sizeof(g_drone.pilot_pos_symbol));
            drone_json_str(cfg_json, "focus_pos_symbol", g_drone.focus_pos_symbol, sizeof(g_drone.focus_pos_symbol));
            drone_json_str(cfg_json, "drone_pose_symbol", g_drone.drone_pose_symbol, sizeof(g_drone.drone_pose_symbol));
            drone_json_str(cfg_json, "wind_symbol", g_drone.wind_symbol, sizeof(g_drone.wind_symbol));

            drone_json_u32(cfg_json, "key_toggle_drone", &g_drone.key_toggle_drone);
            drone_json_u32(cfg_json, "key_cycle_view", &g_drone.key_cycle_view);
            drone_json_u32(cfg_json, "key_respawn", &g_drone.key_respawn);
            drone_json_u32(cfg_json, "key_pilot_view", &g_drone.key_pilot_view);
            drone_json_u32(cfg_json, "key_overlay", &g_drone.key_overlay);
        }
    }

    if (g_drone.log_path[0] != '\0') {
        g_drone.log_file = fopen(g_drone.log_path, "a");
    }

    /* Optional Tier 2 symbol resolution */
    if ((core->active_host_caps & (FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW)) ==
            (FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW) &&
        core->resolve_engine_symbol && core->title_mem_read_u32 && core->title_mem_write_u32)
    {
        g_drone.has_tier2_mem = 1;
        g_drone.camera_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.camera_symbol);
        g_drone.camera_override_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.camera_override_symbol);
        g_drone.pilot_pos_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.pilot_pos_symbol);
        g_drone.focus_pos_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.focus_pos_symbol);
        g_drone.drone_pose_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.drone_pose_symbol);
        g_drone.wind_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_drone.wind_symbol);
    }

    /* Try loading libfpv.dll eagerly (graceful standby if missing) */
    (void)drone_init_libfpv();

    if (g_drone.osd_window_enabled) {
        (void)drone_osd_window_start();
    }

    int rc1 = core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, drone_on_event, &g_drone);
    int rc2 = core->register_event_listener((uint32_t)FB_EVENT_FRAME_BEGIN, drone_on_event, &g_drone);
    int rc3 = core->register_event_listener((uint32_t)FB_EVENT_SHUTDOWN, drone_on_event, &g_drone);
    if (rc1 != FB_OK || rc2 != FB_OK || rc3 != FB_OK) {
        fb_plugin_shutdown();
        return FB_ERR_INTERNAL;
    }

    g_drone.initialized = 1;
    {
        char msg[320];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_drone] Initialized v%s (libfpv=%s, tier2_cam=0x%016llX)",
            DRONE_VERSION,
            g_drone.libfpv_ready ? g_drone.libfpv_version : "standby",
            (unsigned long long)g_drone.camera_va);
        drone_log(FB_LOG_INFO, msg);
    }
    return FB_OK;
}

FB_API int fb_plugin_register_menu(const fb_menu_api_v1_t* menu) {
    if (!menu || menu->abi_version != FB_MENU_ABI_VERSION || menu->struct_size < sizeof(fb_menu_api_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!menu->register_mod || !menu->register_action || !menu->register_setting) {
        return FB_ERR_UNSUPPORTED;
    }

    g_drone.menu = *menu;
    menu->register_mod(
        DRONE_MOD_ID,
        "FPV Drone (libfpv)",
        "Betaflight-in-the-loop FPV quadcopter powered by hammerill/libfpv");

    fb_setting_desc_v1_t st;

    /* Live Status Rows */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.type = (uint32_t)FB_SETTING_STATUS;
    st.setting_id = "drone_status";
    st.label = "Drone";
    st.description = "libfpv runtime state, armed status, active view, speed, battery, and camera writes";
    st.on_status = drone_on_status;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.type = (uint32_t)FB_SETTING_STATUS;
    st.setting_id = "link_status";
    st.label = "Betaflight / Link";
    st.description = "Selected quad preset, input source (Radio/Gamepad/Keyboard), arming flag, and raycast probes";
    st.on_status = drone_on_status;
    menu->register_setting(&st);

    /* Flight Section */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "drone_active";
    st.label = "Drone Active";
    st.description = "Launch or recall the FPV quadcopter";
    st.default_bool = 0;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_ENUM;
    st.setting_id = "camera_view";
    st.label = "Camera View";
    st.description = "Switch between FPV goggles, Chase, Line of Sight, and Pilot character camera";
    st.default_enum = (int32_t)g_drone.view;
    st.enum_labels = k_view_labels;
    st.enum_count = 4u;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_ENUM;
    st.setting_id = "preset_index";
    st.label = "Quad Preset";
    st.description = "Physical frame, motor, prop, battery, and camera preset loaded from libfpv";
    st.default_enum = g_drone.preset_index;
    st.enum_labels = g_drone.preset_count > 0u ? g_drone.preset_ptrs : k_default_preset_labels;
    st.enum_count = g_drone.preset_count > 0u ? g_drone.preset_count : 1u;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_BUTTON;
    st.setting_id = "respawn_btn";
    st.label = "Repair & Respawn Drone";
    st.description = "Fresh battery, undamaged props/motors, and reset in front of pilot";
    st.on_button = drone_on_button;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_BUTTON;
    st.setting_id = "move_pilot_btn";
    st.label = "Move Pilot to Drone";
    st.description = "Move the player character to the drone's location and end the flight";
    st.on_button = drone_on_button;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Flight";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "overlay_open";
    st.label = "libfpv Settings Overlay";
    st.description = "Toggle libfpv's built-in Dear ImGui overlay (radio wizard, rates, Betaflight CLI)";
    st.default_bool = 0;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    /* Simulation Section */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Simulation";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "no_battery_drain";
    st.label = "Unlimited Battery";
    st.description = "Disable battery capacity drain (voltage sag under load still simulated)";
    st.default_bool = g_drone.no_battery_drain;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Simulation";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "no_damage";
    st.label = "Invincible Props & Motors";
    st.description = "Disable prop breakage, motor burnouts, and VTX crash damage";
    st.default_bool = g_drone.no_damage;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Simulation";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "sim_paused";
    st.label = "Pause Drone Physics";
    st.description = "Pause libfpv's 1 kHz Betaflight simulation thread";
    st.default_bool = 0;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Simulation";
    st.type = (uint32_t)FB_SETTING_ENUM;
    st.setting_id = "sim_time_scale";
    st.label = "Physics Time Scale";
    st.description = "Slow-motion or fast-forward multiplier for drone physics";
    st.default_enum = 0;
    st.enum_labels = k_scale_labels;
    st.enum_count = 5u;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    /* Audio & OSD Section */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Audio & OSD";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "audio_enabled";
    st.label = "Motor & Beeper Audio";
    st.description = "Play stereo synthesized motor/beeper audio via Windows waveOut";
    st.default_bool = g_drone.audio_enabled;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Audio & OSD";
    st.type = (uint32_t)FB_SETTING_FLOAT;
    st.setting_id = "audio_volume";
    st.label = "Motor Volume";
    st.description = "Volume multiplier for synthesized quadcopter motors (0.0 - 2.0)";
    st.default_float = (double)g_drone.audio_volume;
    st.float_min = 0.0;
    st.float_max = 2.0;
    st.float_step = 0.1;
    st.float_fmt = "%.1f";
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Audio & OSD";
    st.type = (uint32_t)FB_SETTING_FLOAT;
    st.setting_id = "audio_beep_volume";
    st.label = "Beeper Volume";
    st.description = "Volume multiplier for Betaflight arming/low-battery beeps (0.0 - 2.0)";
    st.default_float = (double)g_drone.audio_beep_volume;
    st.float_min = 0.0;
    st.float_max = 2.0;
    st.float_step = 0.1;
    st.float_fmt = "%.1f";
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Audio & OSD";
    st.type = (uint32_t)FB_SETTING_ENUM;
    st.setting_id = "osd_style";
    st.label = "Betaflight OSD Style";
    st.description = "Select Analog MAX7456 or crisp HD OSD rendering";
    st.default_enum = (int32_t)g_drone.osd_style;
    st.enum_labels = k_osd_style_labels;
    st.enum_count = 2u;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Audio & OSD";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "osd_window_enabled";
    st.label = "Live Betaflight OSD Window";
    st.description = "Opt-in standalone Win32 GDI preview window rendering MAX7456 OSD & telemetry";
    st.default_bool = g_drone.osd_window_enabled;
    st.on_change = drone_on_setting_change;
    menu->register_setting(&st);

    /* Runtime Section */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = DRONE_MOD_ID;
    st.section = "Runtime";
    st.type = (uint32_t)FB_SETTING_BUTTON;
    st.setting_id = "reload_libfpv_btn";
    st.label = "Load / Retry libfpv.dll";
    st.description = "Attempt to load libfpv.dll if it was added after startup";
    st.on_button = drone_on_button;
    menu->register_setting(&st);

    /* Conflict-free Default Keyboard + Gamepad Actions */
    fb_action_desc_v1_t act;

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = DRONE_MOD_ID;
    act.action_id = "toggle_drone";
    act.label = "Launch / Recall FPV Drone";
    act.description = "Spawn or recall the libfpv Betaflight quadcopter";
    act.consume_input = 1;
    act.default_key.code = g_drone.key_toggle_drone;
    act.default_key.modifiers = FB_MOD_NONE; /* F7 */
    act.default_pad.code = FB_PAD_X;
    act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + X */
    act.callback = drone_on_action;
    menu->register_action(&act);

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = DRONE_MOD_ID;
    act.action_id = "cycle_view";
    act.label = "Cycle Drone Camera View";
    act.description = "Cycle FPV -> Chase -> Line of Sight -> Pilot camera views";
    act.consume_input = 1;
    act.default_key.code = g_drone.key_cycle_view;
    act.default_key.modifiers = FB_MOD_SHIFT; /* Shift + V */
    act.default_pad.code = FB_PAD_B;
    act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + B */
    act.callback = drone_on_action;
    menu->register_action(&act);

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = DRONE_MOD_ID;
    act.action_id = "respawn_drone";
    act.label = "Repair & Respawn Drone";
    act.description = "Repair battery/props/motors and reset drone in front of pilot";
    act.consume_input = 1;
    act.default_key.code = g_drone.key_respawn;
    act.default_key.modifiers = FB_MOD_CTRL; /* Ctrl + R */
    act.default_pad.code = FB_PAD_A;
    act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + A */
    act.callback = drone_on_action;
    menu->register_action(&act);

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = DRONE_MOD_ID;
    act.action_id = "toggle_pilot_view";
    act.label = "Toggle Pilot View";
    act.description = "Return camera to pilot character while leaving the drone flying in the world";
    act.consume_input = 1;
    act.default_key.code = g_drone.key_pilot_view;
    act.default_key.modifiers = FB_MOD_CTRL; /* Ctrl + F */
    act.default_pad.code = FB_PAD_BACK;
    act.default_pad.modifiers = FB_PAD_RIGHT_SHOULDER; /* RB + View */
    act.callback = drone_on_action;
    menu->register_action(&act);

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = DRONE_MOD_ID;
    act.action_id = "toggle_overlay";
    act.label = "Toggle libfpv Overlay";
    act.description = "Open/close libfpv's Dear ImGui settings, radio wizard, and Betaflight CLI";
    act.consume_input = 1;
    act.default_key.code = g_drone.key_overlay;
    act.default_key.modifiers = FB_MOD_NONE; /* F9 */
    act.default_pad.code = FB_PAD_START;
    act.default_pad.modifiers = FB_PAD_RIGHT_SHOULDER; /* RB + Start */
    act.callback = drone_on_action;
    menu->register_action(&act);

    g_drone.menu_registered = 1;
    /* Keep runtime toggles aligned with live state rather than stale persisted values */
    drone_sync_menu_bool("drone_active", g_drone.drone_active);
    drone_sync_menu_bool("overlay_open", g_drone.overlay_open);
    drone_sync_menu_bool("sim_paused", g_drone.sim_paused);
    drone_sync_menu_enum("camera_view", (int32_t)g_drone.view);
    return FB_OK;
}

FB_API void fb_plugin_shutdown(void) {
    drone_audio_stop();
    drone_osd_window_stop();

    if (g_drone.drone_active) {
        drone_recall(0);
    } else {
        drone_set_camera_override(0);
    }

    InterlockedExchange(&g_drone.log_active, 0);
    if (g_drone.libfpv_ready && g_drone.api.global_terminate) {
        g_drone.api.global_terminate();
    }
    g_drone.libfpv_ready = 0;

    if (g_drone.libfpv_mod) {
        FreeLibrary(g_drone.libfpv_mod);
        g_drone.libfpv_mod = NULL;
    }
    g_drone.libfpv_loaded = 0;

    if (g_drone.log_lock_init) {
        AcquireSRWLockExclusive(&g_drone.log_lock);
        if (g_drone.log_file) {
            fclose(g_drone.log_file);
            g_drone.log_file = NULL;
        }
        ReleaseSRWLockExclusive(&g_drone.log_lock);
    }

    g_drone.menu_registered = 0;
    g_drone.initialized = 0;
}
