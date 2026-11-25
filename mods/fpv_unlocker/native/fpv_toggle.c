/*
 * fpv_toggle.c — Native C11 Tier 2 Fireball-Mods Plugin (`fpv_toggle.fbplugin.dll`).
 *
 * Pure C11, zero third-party dependencies, compiled with `/std:c11 /W4 /WX`
 * strictly against Fireball-Core's public C11 SDK (`fireball_plugin.h`, `fireball_menu.h`).
 *
 * Features:
 *   - `toggle_fpv` (FB_TRIGGER_PRESS, default `V` / `LB+R3`): immediately flips
 *     camGameplayDirector view_mode `0 (ThirdPerson) <-> 2 (FirstPerson)` via guarded
 *     `title_mem_read_u32` / `title_mem_write_u32`.
 *   - `peek_fpv` (FB_TRIGGER_HOLD, default `Alt+V` / `LB+L3`): holds FirstPerson
 *     (hold-guarded while held) and restores prior view_mode & latch state on release.
 *   - Hold-guard on `FB_EVENT_FRAME_BEGIN`: while latched ON (with `hold_guard_enabled`)
 *     or peeking, re-asserts FirstPerson if the game resets view_mode; per-frame
 *     re-assert log spam is suppressed unless `verbose_reassert_log` is enabled.
 *   - `start_in_fpv` (persisted): automatically latches FirstPerson on the first valid frame.
 *   - `restore_on_shutdown` (persisted, default true): restores ThirdPerson on shutdown
 *     if latched/peeking.
 *   - `view_mode_preset` (ThirdPerson / Cinematic / FirstPerson) & `toggle_now` button
 *     kept bidirectionally synced via `menu->set_setting(..., notify=0)`.
 *   - Full graceful v1 fallback when loaded without `fb_plugin_register_menu`.
 */

#include "fireball_plugin.h"
#include "fireball_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPV_MOD_ID       "community.fpv_unlocker"
#define FPV_PLUGIN_ID    "fpv_toggle"
#define FPV_VERSION      "0.2.0"

typedef enum fpv_preset_idx {
    FPV_PRESET_THIRD_PERSON = 0,
    FPV_PRESET_CINEMATIC    = 1,
    FPV_PRESET_FIRST_PERSON = 2,
    FPV_PRESET_COUNT        = 3
} fpv_preset_idx_t;

static const char* const k_fpv_preset_labels[FPV_PRESET_COUNT] = {
    "ThirdPerson",
    "Cinematic",
    "FirstPerson"
};

typedef struct fpv_state {
    fb_core_services_v1_t core;
    int                   initialized;

    /* Config / persisted settings */
    uint32_t              hotkey_vkey;
    uint32_t              third_person_mode;
    uint32_t              cinematic_mode;
    uint32_t              first_person_mode;
    char                  director_symbol[128];
    uint64_t              view_mode_field_offset;
    int                   hold_guard_enabled;
    int                   start_in_fpv;
    int                   restore_on_shutdown;
    int                   verbose_reassert_log;

    /* Resolved title addresses */
    uint64_t              director_va;
    uint64_t              view_mode_va;

    /* Runtime camera state */
    int                   fpv_latched;
    int                   peek_active;
    int                   peek_prev_latched;
    uint32_t              peek_prev_mode;
    int                   startup_applied;
    int                   has_last_read;
    uint32_t              last_read_mode;
    uint64_t              toggle_count;
    uint64_t              reassert_count;
    uint64_t              frames_seen;

    /* Optional Mod Menu API */
    int                   menu_registered;
    fb_menu_api_v1_t      menu;
} fpv_state_t;

static fpv_state_t g_fpv;

static void fpv_log(int level, const char* msg) {
    if (g_fpv.core.log) {
        g_fpv.core.log(FPV_MOD_ID, level, msg);
    }
}

/* ------------------------------------------------------------------------- */
/* Minimal, safe JSON field helpers for flat `[plugins.config]` objects      */
/* ------------------------------------------------------------------------- */

static const char* fpv_json_find_value(const char* json, const char* key) {
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

static int fpv_json_read_u64(const char* json, const char* key, uint64_t* out_val) {
    const char* v = fpv_json_find_value(json, key);
    if (!v || !isdigit((unsigned char)*v)) {
        return 0;
    }
    char* end = NULL;
    unsigned long long parsed = strtoull(v, &end, 0);
    if (end == v) {
        return 0;
    }
    *out_val = (uint64_t)parsed;
    return 1;
}

static int fpv_json_read_bool(const char* json, const char* key, int* out_val) {
    const char* v = fpv_json_find_value(json, key);
    if (!v) {
        return 0;
    }
    if (strncmp(v, "true", 4) == 0) {
        *out_val = 1;
        return 1;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out_val = 0;
        return 1;
    }
    return 0;
}

static int fpv_json_read_str(const char* json, const char* key, char* out_buf, size_t out_cap) {
    const char* v = fpv_json_find_value(json, key);
    if (!v || *v != '"' || !out_buf || out_cap < 2u) {
        return 0;
    }
    ++v;
    size_t len = 0u;
    while (*v && *v != '"' && len + 1u < out_cap) {
        if (*v == '\\' && v[1] != '\0') {
            ++v;
        }
        out_buf[len++] = *v++;
    }
    if (*v != '"') {
        return 0;
    }
    out_buf[len] = '\0';
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Helpers: mode formatting, preset sync, camera operations                  */
/* ------------------------------------------------------------------------- */

static void fpv_format_mode_name(uint32_t mode, char* buf, size_t cap) {
    if (mode == g_fpv.third_person_mode) {
        snprintf(buf, cap, "ThirdPerson");
    } else if (mode == g_fpv.cinematic_mode) {
        snprintf(buf, cap, "Cinematic");
    } else if (mode == g_fpv.first_person_mode) {
        snprintf(buf, cap, "FirstPerson");
    } else {
        snprintf(buf, cap, "raw=%u", mode);
    }
}

static int32_t fpv_mode_to_preset(uint32_t mode) {
    if (mode == g_fpv.first_person_mode) {
        return (int32_t)FPV_PRESET_FIRST_PERSON;
    }
    if (mode == g_fpv.cinematic_mode) {
        return (int32_t)FPV_PRESET_CINEMATIC;
    }
    return (int32_t)FPV_PRESET_THIRD_PERSON;
}

static uint32_t fpv_preset_to_mode(int32_t preset) {
    if (preset == (int32_t)FPV_PRESET_FIRST_PERSON) {
        return g_fpv.first_person_mode;
    }
    if (preset == (int32_t)FPV_PRESET_CINEMATIC) {
        return g_fpv.cinematic_mode;
    }
    return g_fpv.third_person_mode;
}

static void fpv_sync_preset_to_menu(uint32_t mode) {
    if (!g_fpv.menu_registered || !g_fpv.menu.set_setting) {
        return;
    }
    fb_setting_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = (uint32_t)FB_SETTING_ENUM;
    val.v.enum_val = fpv_mode_to_preset(mode);
    (void)g_fpv.menu.set_setting(FPV_MOD_ID, "view_mode_preset", &val, 0);
}

static int fpv_apply_preset(int32_t preset, int sync_menu) {
    uint32_t cur_mode = 0u;
    int rc = g_fpv.core.title_mem_read_u32(g_fpv.view_mode_va, &cur_mode);
    if (rc != FB_OK) {
        char msg[256];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_toggle] Guarded read failed on 0x%016llX (rc=%d)",
            (unsigned long long)g_fpv.view_mode_va,
            rc);
        fpv_log(FB_LOG_WARN, msg);
        return 0;
    }

    uint32_t target_mode = fpv_preset_to_mode(preset);
    int wrc = g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, target_mode);
    if (wrc != FB_OK) {
        char msg[256];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_toggle] Guarded write failed on 0x%016llX (rc=%d)",
            (unsigned long long)g_fpv.view_mode_va,
            wrc);
        fpv_log(FB_LOG_WARN, msg);
        return 0;
    }

    g_fpv.peek_active = 0;
    g_fpv.fpv_latched = (target_mode == g_fpv.first_person_mode) ? 1 : 0;
    g_fpv.startup_applied = 1;
    g_fpv.last_read_mode = target_mode;
    g_fpv.has_last_read = 1;
    g_fpv.toggle_count += 1ull;

    if (sync_menu) {
        fpv_sync_preset_to_menu(target_mode);
    }

    {
        char msg[256];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_toggle] Switched view_mode: %u -> %u (latched=%s)",
            cur_mode,
            target_mode,
            g_fpv.fpv_latched ? "true" : "false");
        fpv_log(FB_LOG_INFO, msg);
    }
    return 1;
}

static int fpv_perform_toggle(void) {
    int next_latched = g_fpv.fpv_latched ? 0 : 1;
    int32_t target_preset = next_latched ? (int32_t)FPV_PRESET_FIRST_PERSON : (int32_t)FPV_PRESET_THIRD_PERSON;
    return fpv_apply_preset(target_preset, 1);
}

static void fpv_handle_peek(int active) {
    if (active) {
        if (g_fpv.peek_active) {
            return;
        }
        uint32_t cur_mode = g_fpv.fpv_latched ? g_fpv.first_person_mode : g_fpv.third_person_mode;
        int rc = g_fpv.core.title_mem_read_u32(g_fpv.view_mode_va, &cur_mode);
        if (rc == FB_OK) {
            g_fpv.last_read_mode = cur_mode;
            g_fpv.has_last_read = 1;
        }
        g_fpv.peek_prev_mode = cur_mode;
        g_fpv.peek_prev_latched = g_fpv.fpv_latched;

        int wrc = g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, g_fpv.first_person_mode);
        if (wrc != FB_OK) {
            char msg[256];
            snprintf(
                msg,
                sizeof(msg),
                "[fpv_toggle] Peek write failed on 0x%016llX (rc=%d)",
                (unsigned long long)g_fpv.view_mode_va,
                wrc);
            fpv_log(FB_LOG_WARN, msg);
            return;
        }

        g_fpv.peek_active = 1;
        g_fpv.last_read_mode = g_fpv.first_person_mode;
        g_fpv.has_last_read = 1;
        fpv_sync_preset_to_menu(g_fpv.first_person_mode);
        fpv_log(FB_LOG_INFO, "[fpv_toggle] Peek ON -> forced FirstPerson");
    } else {
        if (!g_fpv.peek_active) {
            return;
        }
        g_fpv.peek_active = 0;
        g_fpv.fpv_latched = g_fpv.peek_prev_latched;
        uint32_t restore_mode = g_fpv.fpv_latched ? g_fpv.first_person_mode : g_fpv.peek_prev_mode;
        int wrc = g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, restore_mode);
        if (wrc == FB_OK) {
            g_fpv.last_read_mode = restore_mode;
            g_fpv.has_last_read = 1;
            fpv_sync_preset_to_menu(restore_mode);
        }
        {
            char msg[256];
            snprintf(
                msg,
                sizeof(msg),
                "[fpv_toggle] Peek OFF -> restored view_mode=%u (latched=%s)",
                restore_mode,
                g_fpv.fpv_latched ? "true" : "false");
            fpv_log(FB_LOG_INFO, msg);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Event bus & v1 fallback input handling                                    */
/* ------------------------------------------------------------------------- */

static int fpv_handle_input(const fb_input_event_v1_t* ev) {
    if (g_fpv.menu_registered) {
        return 0; /* Handled centrally by Fireball-Core Action Binding Manager */
    }
    if (!ev || ev->struct_size < sizeof(fb_input_event_v1_t)) {
        return 0;
    }
    if (ev->device_kind != FB_DEV_KEYBOARD) {
        return 0;
    }
    if (ev->code != g_fpv.hotkey_vkey) {
        return 0;
    }
    uint32_t mods = ev->modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT);
    if (mods == FB_MOD_NONE && ev->action == (uint32_t)FB_INPUT_PRESS) {
        return fpv_perform_toggle();
    }
    if (mods == FB_MOD_ALT && ev->action == (uint32_t)FB_INPUT_PRESS) {
        fpv_handle_peek(1);
        return 1;
    }
    if (g_fpv.peek_active && ev->action == (uint32_t)FB_INPUT_RELEASE) {
        fpv_handle_peek(0);
        return 1;
    }
    return 0;
}

static void fpv_handle_frame_begin(const fb_frame_event_v1_t* frame) {
    g_fpv.frames_seen += 1ull;

    uint32_t cur_mode = 0u;
    int rc = g_fpv.core.title_mem_read_u32(g_fpv.view_mode_va, &cur_mode);
    if (rc != FB_OK) {
        return;
    }

    g_fpv.last_read_mode = cur_mode;
    g_fpv.has_last_read = 1;

    /* Auto-latch FirstPerson on the first valid frame when `start_in_fpv` is enabled */
    if (!g_fpv.startup_applied) {
        g_fpv.startup_applied = 1;
        if (g_fpv.start_in_fpv && !g_fpv.fpv_latched && !g_fpv.peek_active) {
            int wrc = g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, g_fpv.first_person_mode);
            if (wrc == FB_OK) {
                g_fpv.fpv_latched = 1;
                g_fpv.last_read_mode = g_fpv.first_person_mode;
                fpv_sync_preset_to_menu(g_fpv.first_person_mode);
                fpv_log(FB_LOG_INFO, "[fpv_toggle] start_in_fpv latched FirstPerson on first valid frame");
            }
            return;
        }
    }

    int enforce_fpv = g_fpv.peek_active || (g_fpv.fpv_latched && g_fpv.hold_guard_enabled);
    if (enforce_fpv && cur_mode != g_fpv.first_person_mode) {
        int wrc = g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, g_fpv.first_person_mode);
        if (wrc == FB_OK) {
            g_fpv.reassert_count += 1ull;
            g_fpv.last_read_mode = g_fpv.first_person_mode;
            if (g_fpv.verbose_reassert_log) {
                char msg[256];
                snprintf(
                    msg,
                    sizeof(msg),
                    "[fpv_toggle] Frame #%llu: hold-guard intercepted context reset (%u -> re-asserted %u)",
                    (unsigned long long)(frame ? frame->frame_index : g_fpv.frames_seen),
                    cur_mode,
                    g_fpv.first_person_mode);
                fpv_log(FB_LOG_INFO, msg);
            }
        }
    }
}

static int fpv_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_fpv.initialized) {
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_INPUT_EVENT) {
        return fpv_handle_input((const fb_input_event_v1_t*)payload);
    }
    if (event_id == (uint32_t)FB_EVENT_FRAME_BEGIN) {
        fpv_handle_frame_begin((const fb_frame_event_v1_t*)payload);
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_SHUTDOWN) {
        fb_plugin_shutdown();
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Mod Menu callbacks (`include/fireball_menu.h`)                            */
/* ------------------------------------------------------------------------- */

static void fpv_on_action(const char* action_id, int active, void* user_ctx) {
    (void)user_ctx;
    if (!g_fpv.initialized || !action_id) {
        return;
    }
    if (strcmp(action_id, "toggle_fpv") == 0) {
        if (active) {
            (void)fpv_perform_toggle();
        }
    } else if (strcmp(action_id, "peek_fpv") == 0) {
        fpv_handle_peek(active ? 1 : 0);
    }
}

static void fpv_on_setting_change(const char* setting_id, const fb_setting_value_t* val, void* user_ctx) {
    (void)user_ctx;
    if (!setting_id || !val) {
        return;
    }
    if (strcmp(setting_id, "hold_guard_enabled") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_fpv.hold_guard_enabled = val->v.bool_val ? 1 : 0;
    } else if (strcmp(setting_id, "start_in_fpv") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_fpv.start_in_fpv = val->v.bool_val ? 1 : 0;
    } else if (strcmp(setting_id, "restore_on_shutdown") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_fpv.restore_on_shutdown = val->v.bool_val ? 1 : 0;
    } else if (strcmp(setting_id, "verbose_reassert_log") == 0 && val->type == (uint32_t)FB_SETTING_BOOL) {
        g_fpv.verbose_reassert_log = val->v.bool_val ? 1 : 0;
    } else if (strcmp(setting_id, "view_mode_preset") == 0 && val->type == (uint32_t)FB_SETTING_ENUM) {
        /* Apply immediately only for live menu edits (never let stale config hijack boot before start_in_fpv) */
        if (g_fpv.initialized && g_fpv.menu_registered) {
            (void)fpv_apply_preset(val->v.enum_val, 0);
        }
    }
}

static void fpv_on_button(const char* setting_id, void* user_ctx) {
    (void)user_ctx;
    if (g_fpv.initialized && setting_id && strcmp(setting_id, "toggle_now") == 0) {
        (void)fpv_perform_toggle();
    }
}

static void fpv_on_status(const char* setting_id, char* out_buf, size_t out_cap, void* user_ctx) {
    (void)setting_id;
    (void)user_ctx;
    if (!out_buf || out_cap == 0u) {
        return;
    }
    const char* latch_str = g_fpv.peek_active
                                ? "FirstPerson (peek)"
                                : (g_fpv.fpv_latched ? "FirstPerson (latched)" : "Unlatched");
    char mode_str[32];
    if (g_fpv.has_last_read) {
        fpv_format_mode_name(g_fpv.last_read_mode, mode_str, sizeof(mode_str));
    } else {
        snprintf(mode_str, sizeof(mode_str), "unread");
    }
    snprintf(
        out_buf,
        out_cap,
        "%s | mode=%s | toggles=%llu | re-asserts=%llu",
        latch_str,
        mode_str,
        (unsigned long long)g_fpv.toggle_count,
        (unsigned long long)g_fpv.reassert_count);
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
    out_info->mod_id = FPV_MOD_ID;
    out_info->plugin_id = FPV_PLUGIN_ID;
    out_info->version = FPV_VERSION;
    out_info->required_caps =
        FB_CAP_FRAME_LIFECYCLE |
        FB_CAP_INPUT_EVENTS |
        FB_CAP_RAGE_SYMBOL_REG |
        FB_CAP_TITLE_MEM_RW;
    return FB_OK;
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core) {
        return FB_ERR_INVALID_ARG;
    }
    if (core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size != sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!core->register_event_listener || !core->resolve_engine_symbol ||
        !core->title_mem_read_u32 || !core->title_mem_write_u32)
    {
        return FB_ERR_UNSUPPORTED;
    }

    memset(&g_fpv, 0, sizeof(g_fpv));
    g_fpv.core = *core;
    g_fpv.hotkey_vkey = 0x56u; /* Win32 VK_V ('V') */
    g_fpv.third_person_mode = 0u;
    g_fpv.cinematic_mode = 1u;
    g_fpv.first_person_mode = 2u;
    snprintf(g_fpv.director_symbol, sizeof(g_fpv.director_symbol), "%s", "rage::camGameplayDirector::s_Instance");
    g_fpv.view_mode_field_offset = 0ull;
    g_fpv.hold_guard_enabled = 1;
    g_fpv.start_in_fpv = 0;
    g_fpv.restore_on_shutdown = 1;
    g_fpv.verbose_reassert_log = 0;

    if (core->config_get_json) {
        char cfg_json[2048];
        memset(cfg_json, 0, sizeof(cfg_json));
        if (core->config_get_json(FPV_MOD_ID, cfg_json, sizeof(cfg_json)) == FB_OK) {
            uint64_t tmp_u64 = 0ull;
            int tmp_bool = 0;
            if (fpv_json_read_u64(cfg_json, "hotkey_vkey", &tmp_u64)) {
                g_fpv.hotkey_vkey = (uint32_t)tmp_u64;
            }
            if (fpv_json_read_u64(cfg_json, "third_person_mode", &tmp_u64)) {
                g_fpv.third_person_mode = (uint32_t)tmp_u64;
            }
            if (fpv_json_read_u64(cfg_json, "cinematic_mode", &tmp_u64)) {
                g_fpv.cinematic_mode = (uint32_t)tmp_u64;
            }
            if (fpv_json_read_u64(cfg_json, "first_person_mode", &tmp_u64)) {
                g_fpv.first_person_mode = (uint32_t)tmp_u64;
            }
            if (fpv_json_read_u64(cfg_json, "view_mode_field_offset", &tmp_u64)) {
                g_fpv.view_mode_field_offset = tmp_u64;
            }
            if (fpv_json_read_bool(cfg_json, "hold_guard_enabled", &tmp_bool)) {
                g_fpv.hold_guard_enabled = tmp_bool;
            }
            if (fpv_json_read_bool(cfg_json, "start_in_fpv", &tmp_bool)) {
                g_fpv.start_in_fpv = tmp_bool;
            }
            if (fpv_json_read_bool(cfg_json, "restore_on_shutdown", &tmp_bool)) {
                g_fpv.restore_on_shutdown = tmp_bool;
            }
            if (fpv_json_read_bool(cfg_json, "verbose_reassert_log", &tmp_bool)) {
                g_fpv.verbose_reassert_log = tmp_bool;
            }
            fpv_json_read_str(cfg_json, "director_symbol", g_fpv.director_symbol, sizeof(g_fpv.director_symbol));
        }
    }

    void* sym_ptr = core->resolve_engine_symbol(g_fpv.director_symbol);
    g_fpv.director_va = (uint64_t)(uintptr_t)sym_ptr;
    if (g_fpv.director_va == 0ull) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[fpv_toggle] Symbol '%s' unavailable; plugin standing by.", g_fpv.director_symbol);
        fpv_log(FB_LOG_WARN, msg);
        return FB_ERR_NOT_FOUND;
    }

    g_fpv.view_mode_va = g_fpv.director_va + g_fpv.view_mode_field_offset;

    int rc1 = core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, fpv_on_event, &g_fpv);
    int rc2 = core->register_event_listener((uint32_t)FB_EVENT_FRAME_BEGIN, fpv_on_event, &g_fpv);
    int rc3 = core->register_event_listener((uint32_t)FB_EVENT_SHUTDOWN, fpv_on_event, &g_fpv);
    if (rc1 != FB_OK || rc2 != FB_OK || rc3 != FB_OK) {
        return FB_ERR_INTERNAL;
    }

    g_fpv.initialized = 1;
    {
        char msg[384];
        snprintf(
            msg,
            sizeof(msg),
            "[fpv_toggle] Initialized v%s: symbol=%s @ 0x%016llX, view_mode_va=0x%016llX, hotkey=0x%02X, hold_guard=%s, start_in_fpv=%s",
            FPV_VERSION,
            g_fpv.director_symbol,
            (unsigned long long)g_fpv.director_va,
            (unsigned long long)g_fpv.view_mode_va,
            g_fpv.hotkey_vkey,
            g_fpv.hold_guard_enabled ? "true" : "false",
            g_fpv.start_in_fpv ? "true" : "false");
        fpv_log(FB_LOG_INFO, msg);
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

    g_fpv.menu = *menu;
    menu->register_mod(FPV_MOD_ID, "FPV Unlocker", "On-foot First-Person View toggle, hold-to-peek, and hold-guard");

    fb_setting_desc_v1_t st;

    /* Live Status */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.type = (uint32_t)FB_SETTING_STATUS;
    st.setting_id = "status";
    st.label = "FPV Camera";
    st.description = "Latched state, last-read view_mode, toggle count, and hold-guard re-assert count";
    st.on_status = fpv_on_status;
    menu->register_setting(&st);

    /* Camera Section Settings */
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "hold_guard_enabled";
    st.label = "Hold-Guard Context Resets";
    st.description = "Re-assert FirstPerson view mode if the game resets it while latched ON";
    st.default_bool = g_fpv.hold_guard_enabled;
    st.on_change = fpv_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "start_in_fpv";
    st.label = "Start in First-Person";
    st.description = "Automatically latch FirstPerson on the first valid frame";
    st.default_bool = g_fpv.start_in_fpv;
    st.on_change = fpv_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "restore_on_shutdown";
    st.label = "Restore Third-Person on Shutdown";
    st.description = "Restore ThirdPerson view mode cleanly on plugin shutdown if latched";
    st.default_bool = g_fpv.restore_on_shutdown;
    st.on_change = fpv_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_BOOL;
    st.setting_id = "verbose_reassert_log";
    st.label = "Verbose Re-Assert Logging";
    st.description = "Log every hold-guard frame re-assertion (off by default to prevent log spam)";
    st.default_bool = g_fpv.verbose_reassert_log;
    st.on_change = fpv_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_ENUM;
    st.setting_id = "view_mode_preset";
    st.label = "Camera View Mode";
    st.description = "Directly switch between ThirdPerson, Cinematic, and FirstPerson";
    st.default_enum = (int32_t)FPV_PRESET_THIRD_PERSON;
    st.enum_labels = k_fpv_preset_labels;
    st.enum_count = (uint32_t)FPV_PRESET_COUNT;
    st.on_change = fpv_on_setting_change;
    menu->register_setting(&st);

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.mod_id = FPV_MOD_ID;
    st.section = "Camera";
    st.type = (uint32_t)FB_SETTING_BUTTON;
    st.setting_id = "toggle_now";
    st.label = "Toggle First-Person Now";
    st.description = "Immediately flip between ThirdPerson and FirstPerson";
    st.on_button = fpv_on_button;
    menu->register_setting(&st);

    /* Actions */
    fb_action_desc_v1_t act;
    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    act.mod_id = FPV_MOD_ID;
    act.action_id = "toggle_fpv";
    act.label = "Toggle First-Person";
    act.description = "Toggle on-foot FirstPerson view mode (0 <-> 2) immediately";
    act.consume_input = 1;
    act.default_key.code = g_fpv.hotkey_vkey;
    act.default_key.modifiers = FB_MOD_NONE;
    act.default_pad.code = FB_PAD_RIGHT_THUMB;
    act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + R3 */
    act.callback = fpv_on_action;
    menu->register_action(&act);

    memset(&act, 0, sizeof(act));
    act.struct_size = (uint32_t)sizeof(act);
    act.trigger_mode = (uint32_t)FB_TRIGGER_HOLD;
    act.mod_id = FPV_MOD_ID;
    act.action_id = "peek_fpv";
    act.label = "Peek First-Person (Hold)";
    act.description = "Hold to force FirstPerson (hold-guarded) and restore prior view mode on release";
    act.consume_input = 1;
    act.default_key.code = g_fpv.hotkey_vkey;
    act.default_key.modifiers = FB_MOD_ALT; /* Alt + V */
    act.default_pad.code = FB_PAD_LEFT_THUMB;
    act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + L3 */
    act.callback = fpv_on_action;
    menu->register_action(&act);

    g_fpv.menu_registered = 1;
    /* Keep view_mode_preset aligned with live state rather than stale persisted enum value */
    fpv_sync_preset_to_menu(g_fpv.fpv_latched ? g_fpv.first_person_mode : g_fpv.third_person_mode);
    return FB_OK;
}

FB_API void fb_plugin_shutdown(void) {
    if (g_fpv.initialized && (g_fpv.fpv_latched || g_fpv.peek_active)) {
        if (g_fpv.restore_on_shutdown && g_fpv.core.title_mem_write_u32) {
            uint32_t restore_mode = g_fpv.fpv_latched
                                        ? g_fpv.third_person_mode
                                        : (g_fpv.peek_prev_latched ? g_fpv.third_person_mode : g_fpv.peek_prev_mode);
            g_fpv.core.title_mem_write_u32(g_fpv.view_mode_va, restore_mode);
            {
                char msg[160];
                snprintf(msg, sizeof(msg), "[fpv_toggle] Shutdown restored view_mode=%u", restore_mode);
                fpv_log(FB_LOG_INFO, msg);
            }
        }
        g_fpv.fpv_latched = 0;
        g_fpv.peek_active = 0;
    }
    g_fpv.menu_registered = 0;
    g_fpv.initialized = 0;
}
