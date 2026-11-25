/*
 * mods_integration_test.c — Native C11 CTest harness for Fireball-Mods.
 *
 * Compiles strictly against Fireball-Core's public C11 SDK (`fireball_host.h`,
 * `fireball_plugin.h`, `fireball_menu.h`) with `/std:c11 /W4 /WX` — never includes
 * any `src/core/` internal headers.
 *
 * Coverage:
 *   1. Standalone v1 fallback (WITHOUT calling `fb_plugin_register_menu`):
 *      - `fpv_toggle.fbplugin.dll` (V toggle, hold-guard, silent re-assert by default,
 *        Alt+V peek & restore, start_in_fpv, restore_on_shutdown true/false).
 *      - `time_weather.fbplugin.dll` (+1h/-1h, midnight wrap, pause/resume, weather
 *        prev/next instant & blend, Ctrl+\ weather lock toggle, CSV write/verify log).
 *   2. Mock `fb_menu_api_v1_t` registration, bidirectional sync & ZERO binding conflicts:
 *      - Verifies zero keyboard and zero gamepad chord conflicts across `fireball.core`,
 *        `community.fpv_unlocker`, and `community.time_weather_control`.
 *      - Verifies `fpv_toggle` live status, `view_mode_preset` sync (`notify=0`),
 *        `toggle_now`, `peek_fpv` hold/restore, `verbose_reassert_log`.
 *      - Verifies `time_weather` bidirectional sync (`clock_paused`, `time_scale`,
 *        `weather_preset`, `weather_locked`), Hour/Minute + Apply Time Now, quick presets
 *        (Dawn/Noon/Sunset/Midnight), `weather_apply_mode` Instant vs Blend,
 *        `weather_lock_on_set`, `toggle_weather_lock`, opt-in legacy F8 window off by default.
 *   3. End-to-end inside `fireball_core.dll` (`FIREBALL_HEADLESS=1`):
 *      - `fpv_toggle`: `FB_TRIGGER_HOLD` peek (keyboard Alt+V & gamepad LB+L3),
 *        `start_in_fpv` persistence across re-attach, `restore_on_shutdown` true & false.
 *      - `time_weather`: no boot-time clock overwrite even with persisted settings,
 *        headless menu quick presets & Hour/Minute + Apply Time Now, `toggle_weather_lock`
 *        (Ctrl+\ & LB+Y), `Instant`/`Blend` hotkey mode, bidirectional sync.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fireball_host.h"
#include "fireball_plugin.h"
#include "fireball_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            fprintf(stderr, "  [FAIL] line %d: %s\n", __LINE__, #cond);        \
        }                                                                      \
    } while (0)

typedef int (*fb_plugin_query_fn)(fb_plugin_info_v1_t*);
typedef int (*fb_plugin_init_fn)(const fb_core_services_v1_t*);
typedef void (*fb_plugin_shutdown_fn)(void);
typedef int (*fb_plugin_register_menu_fn)(const fb_menu_api_v1_t*);
typedef int (*fb_attach_host_fn)(const fb_host_api_v1_t*, fb_modcore_callbacks_v1_t*);

typedef struct loaded_plugin {
    HMODULE                    mod;
    fb_plugin_query_fn         query;
    fb_plugin_init_fn          init;
    fb_plugin_shutdown_fn      shutdown;
    fb_plugin_register_menu_fn reg_menu;
} loaded_plugin_t;

static int load_plugin_dll(const wchar_t* path, loaded_plugin_t* out) {
    memset(out, 0, sizeof(*out));
    out->mod = LoadLibraryW(path);
    if (!out->mod) {
        fprintf(stderr, "  LoadLibraryW('%ls') failed (err=%lu)\n", path, (unsigned long)GetLastError());
        return 0;
    }
    out->query = (fb_plugin_query_fn)(void*)GetProcAddress(out->mod, "fb_plugin_query");
    out->init = (fb_plugin_init_fn)(void*)GetProcAddress(out->mod, "fb_plugin_init");
    out->shutdown = (fb_plugin_shutdown_fn)(void*)GetProcAddress(out->mod, "fb_plugin_shutdown");
    out->reg_menu = (fb_plugin_register_menu_fn)(void*)GetProcAddress(out->mod, "fb_plugin_register_menu");
    return out->query && out->init && out->shutdown;
}

/* ------------------------------------------------------------------------- */
/* Shared mock title memory & helpers                                        */
/* ------------------------------------------------------------------------- */

#define MOCK_MEM_SIZE 4096u
#define OFF_CAM       0x000u
#define OFF_CLOCK     0x100u
#define OFF_WEATHER   0x200u

#define H_W0 0xF5A87B65u
#define H_W1 0x614A1F91u
#define H_W2 0x5974E8E5u

static uint8_t       g_mem[MOCK_MEM_SIZE];
static char          g_cfg_fpv[4096];
static char          g_cfg_tw[4096];
static fb_event_fn_t g_listeners[8][4];
static void*         g_listener_ctx[8][4];
static int           g_listener_count[8];
static uint64_t      g_frame_idx = 0;
static int           g_reassert_log_hits = 0;

static uint32_t peek_u32(uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, g_mem + off, 4);
    return v;
}

static void poke_u32(uint32_t off, uint32_t v) {
    memcpy(g_mem + off, &v, 4);
}

static float peek_f32(uint32_t off) {
    uint32_t bits = peek_u32(off);
    float f = 0.0f;
    memcpy(&f, &bits, 4);
    return f;
}

static fb_input_event_v1_t make_ev(uint32_t dev, uint32_t code, uint32_t action, uint32_t mods, uint64_t ts_us) {
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = dev;
    ev.code = code;
    ev.action = action;
    ev.modifiers = mods;
    ev.timestamp_us = ts_us;
    return ev;
}

static void mock_log(const char* mod_id, int level, const char* msg) {
    (void)mod_id;
    (void)level;
    if (msg && strstr(msg, "hold-guard intercepted context reset")) {
        g_reassert_log_hits += 1;
    }
}

static int mock_config_get_json(const char* mod_id, char* buf, size_t cap) {
    const char* src = NULL;
    if (mod_id && strcmp(mod_id, "community.fpv_unlocker") == 0 && g_cfg_fpv[0]) {
        src = g_cfg_fpv;
    } else if (mod_id && strcmp(mod_id, "community.time_weather_control") == 0 && g_cfg_tw[0]) {
        src = g_cfg_tw;
    }
    if (!src) {
        return FB_ERR_NOT_FOUND;
    }
    size_t len = strlen(src);
    if (len + 1u > cap) {
        return FB_ERR_INVALID_ARG;
    }
    memcpy(buf, src, len + 1u);
    return FB_OK;
}

static int mock_register_listener(uint32_t event_id, fb_event_fn_t fn, void* ctx) {
    if (event_id >= 8u || g_listener_count[event_id] >= 4) {
        return FB_ERR_INVALID_ARG;
    }
    int slot = g_listener_count[event_id]++;
    g_listeners[event_id][slot] = fn;
    g_listener_ctx[event_id][slot] = ctx;
    return FB_OK;
}

static void mock_reset_bus(void) {
    memset(g_listeners, 0, sizeof(g_listeners));
    memset(g_listener_ctx, 0, sizeof(g_listener_ctx));
    memset(g_listener_count, 0, sizeof(g_listener_count));
    g_frame_idx = 0;
    g_reassert_log_hits = 0;
}

static void mock_emit_frame(double dt_sec) {
    fb_frame_event_v1_t fe;
    fe.frame_index = ++g_frame_idx;
    fe.dt_sec = dt_sec;
    for (int i = 0; i < g_listener_count[FB_EVENT_FRAME_BEGIN]; ++i) {
        g_listeners[FB_EVENT_FRAME_BEGIN][i](FB_EVENT_FRAME_BEGIN, &fe, g_listener_ctx[FB_EVENT_FRAME_BEGIN][i]);
    }
}

static int mock_emit_input(const fb_input_event_v1_t* ev) {
    int consumed = 0;
    for (int i = 0; i < g_listener_count[FB_EVENT_INPUT_EVENT]; ++i) {
        if (g_listeners[FB_EVENT_INPUT_EVENT][i](FB_EVENT_INPUT_EVENT, ev, g_listener_ctx[FB_EVENT_INPUT_EVENT][i])) {
            consumed = 1;
        }
    }
    return consumed;
}

static void* mock_resolve_symbol(const char* sym) {
    if (!sym) {
        return NULL;
    }
    if (strcmp(sym, "rage::camGameplayDirector::s_Instance") == 0) {
        return g_mem + OFF_CAM;
    }
    if (strcmp(sym, "fb.clock") == 0) {
        return g_mem + OFF_CLOCK;
    }
    if (strcmp(sym, "fb.weather") == 0) {
        return g_mem + OFF_WEATHER;
    }
    return NULL;
}

static int mock_read_u32(uint64_t va, uint32_t* out) {
    uint64_t base = (uint64_t)(uintptr_t)g_mem;
    if (va < base || va + 4ull > base + MOCK_MEM_SIZE || (va & 3ull) != 0ull) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy(out, (const void*)(uintptr_t)va, 4);
    return FB_OK;
}

static int mock_write_u32(uint64_t va, uint32_t val) {
    uint64_t base = (uint64_t)(uintptr_t)g_mem;
    if (va < base || va + 4ull > base + MOCK_MEM_SIZE || (va & 3ull) != 0ull) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy((void*)(uintptr_t)va, &val, 4);
    return FB_OK;
}

static int mock_poll_key(uint32_t dev, uint32_t code) {
    (void)dev;
    (void)code;
    return 0;
}

static fb_core_services_v1_t make_mock_services(void) {
    fb_core_services_v1_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.abi_version = FB_PLUGIN_ABI_VERSION;
    s.active_host_caps = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    s.log = mock_log;
    s.config_get_json = mock_config_get_json;
    s.register_event_listener = mock_register_listener;
    s.poll_key_state = mock_poll_key;
    s.resolve_engine_symbol = mock_resolve_symbol;
    s.title_mem_read_u32 = mock_read_u32;
    s.title_mem_write_u32 = mock_write_u32;
    return s;
}

static void json_escape_path(char* out, size_t cap, const char* path) {
    size_t j = 0;
    for (size_t i = 0; path[i] && j + 2 < cap; ++i) {
        if (path[i] == '\\') {
            out[j++] = '\\';
        }
        out[j++] = path[i];
    }
    out[j] = '\0';
}

static int file_contains_str(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* ------------------------------------------------------------------------- */
/* 1. Standalone v1 Fallback Tests (NO fb_plugin_register_menu)              */
/* ------------------------------------------------------------------------- */

static void test_standalone_v1(const wchar_t* fpv_dll, const wchar_t* tw_dll, const char* tmp_dir) {
    printf("[1] Testing standalone v1 fallback (without fb_plugin_register_menu)...\n");

    /* --- 1a. fpv_toggle v1 --- */
    loaded_plugin_t fpv;
    CHECK(load_plugin_dll(fpv_dll, &fpv));
    if (fpv.mod) {
        fb_plugin_info_v1_t info;
        memset(&info, 0, sizeof(info));
        CHECK(fpv.query(&info) == FB_OK);
        CHECK(info.abi_version == FB_PLUGIN_ABI_VERSION);
        CHECK(strcmp(info.mod_id, "community.fpv_unlocker") == 0);
        CHECK(strcmp(info.plugin_id, "fpv_toggle") == 0);

        memset(g_mem, 0, sizeof(g_mem));
        mock_reset_bus();
        g_cfg_fpv[0] = '\0';
        fb_core_services_v1_t svc = make_mock_services();
        CHECK(fpv.init(&svc) == FB_OK);

        /* V press flips 0 -> 2, hold-guard re-asserts quietly by default */
        fb_input_event_v1_t ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, FB_MOD_NONE, 10);
        CHECK(mock_emit_input(&ev) == 1);
        CHECK(peek_u32(OFF_CAM) == 2u);
        poke_u32(OFF_CAM, 0u);
        mock_emit_frame(1.0 / 60.0);
        CHECK(peek_u32(OFF_CAM) == 2u);
        CHECK(g_reassert_log_hits == 0); /* verbose_reassert_log defaults to false */

        /* V press flips 2 -> 0; game Cinematic (1) preserved */
        CHECK(mock_emit_input(&ev) == 1);
        CHECK(peek_u32(OFF_CAM) == 0u);
        poke_u32(OFF_CAM, 1u);
        mock_emit_frame(1.0 / 60.0);
        CHECK(peek_u32(OFF_CAM) == 1u);

        /* Alt+V hold-peek forces 2, hold-guards while held, and restores Cinematic (1) on release */
        ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, FB_MOD_ALT, 20);
        CHECK(mock_emit_input(&ev) == 1);
        CHECK(peek_u32(OFF_CAM) == 2u);
        poke_u32(OFF_CAM, 0u);
        mock_emit_frame(1.0 / 60.0);
        CHECK(peek_u32(OFF_CAM) == 2u);
        ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_RELEASE, FB_MOD_ALT, 30);
        CHECK(mock_emit_input(&ev) == 1);
        CHECK(peek_u32(OFF_CAM) == 1u);

        /* Shutdown restores ThirdPerson when latched and restore_on_shutdown=true */
        ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, FB_MOD_NONE, 40);
        CHECK(mock_emit_input(&ev) == 1);
        CHECK(peek_u32(OFF_CAM) == 2u);
        fpv.shutdown();
        CHECK(peek_u32(OFF_CAM) == 0u);

        /* start_in_fpv=true & restore_on_shutdown=false via config */
        memset(g_mem, 0, sizeof(g_mem));
        mock_reset_bus();
        snprintf(g_cfg_fpv, sizeof(g_cfg_fpv),
                 "{\"start_in_fpv\":true,\"restore_on_shutdown\":false,\"verbose_reassert_log\":true}");
        CHECK(fpv.init(&svc) == FB_OK);
        CHECK(peek_u32(OFF_CAM) == 0u);
        mock_emit_frame(1.0 / 60.0);
        CHECK(peek_u32(OFF_CAM) == 2u); /* auto-latched on first valid frame */
        poke_u32(OFF_CAM, 0u);
        mock_emit_frame(1.0 / 60.0);
        CHECK(peek_u32(OFF_CAM) == 2u);
        CHECK(g_reassert_log_hits == 1); /* verbose logging enabled */
        fpv.shutdown();
        CHECK(peek_u32(OFF_CAM) == 2u); /* restore_on_shutdown=false left view_mode untouched */

        FreeLibrary(fpv.mod);
    }

    /* --- 1b. time_weather v1 --- */
    loaded_plugin_t tw;
    CHECK(load_plugin_dll(tw_dll, &tw));
    if (tw.mod) {
        fb_plugin_info_v1_t info;
        memset(&info, 0, sizeof(info));
        CHECK(tw.query(&info) == FB_OK);
        CHECK(info.abi_version == FB_PLUGIN_ABI_VERSION);
        CHECK(strcmp(info.mod_id, "community.time_weather_control") == 0);
        CHECK(strcmp(info.plugin_id, "time_weather") == 0);

        char list_path[300], log_path[300], jlist[600], jlog[600];
        snprintf(list_path, sizeof(list_path), "%s\\v1_weathers.txt", tmp_dir);
        snprintf(log_path, sizeof(log_path), "%s\\v1_tw.csv", tmp_dir);
        DeleteFileA(log_path);
        FILE* f = fopen(list_path, "w");
        CHECK(f != NULL);
        if (f) {
            fprintf(f, "# index joaat_lower joaat name\n0 0x%08X 0x1 CLEAR\n1 0x%08X 0x2 SUNNY\n2 0x%08X 0x3 STORM\n",
                    H_W0, H_W1, H_W2);
            fclose(f);
        }
        json_escape_path(jlist, sizeof(jlist), list_path);
        json_escape_path(jlog, sizeof(jlog), log_path);
        snprintf(g_cfg_tw, sizeof(g_cfg_tw),
                 "{\"clock_layout\":\"h,m,s\",\"weather_kind\":\"hash\",\"weather_list_path\":\"%s\","
                 "\"log_path\":\"%s\",\"verify_frames\":2,\"weather_blend_ms\":100,\"ui_enabled\":false}",
                 jlist, jlog);

        memset(g_mem, 0, sizeof(g_mem));
        mock_reset_bus();
        poke_u32(OFF_CLOCK, 10u);
        poke_u32(OFF_WEATHER, H_W0);
        poke_u32(OFF_WEATHER + 4u, H_W0);
        fb_core_services_v1_t svc = make_mock_services();
        CHECK(tw.init(&svc) == FB_OK);
        mock_emit_frame(1.0 / 30.0);

        /* ] (+1h), [ (-1h), \ (pause) */
        fb_input_event_v1_t ev = make_ev(FB_DEV_KEYBOARD, 0xDDu, FB_INPUT_PRESS, FB_MOD_NONE, 1);
        CHECK(mock_emit_input(&ev) == 1);
        mock_emit_frame(1.0 / 30.0);
        CHECK(peek_u32(OFF_CLOCK) == 11u);

        ev = make_ev(FB_DEV_KEYBOARD, 0xDCu, FB_INPUT_PRESS, FB_MOD_NONE, 2);
        CHECK(mock_emit_input(&ev) == 1);
        mock_emit_frame(1.0 / 30.0);
        poke_u32(OFF_CLOCK + 4u, 22u);
        mock_emit_frame(1.0 / 30.0);
        CHECK(peek_u32(OFF_CLOCK) == 11u && peek_u32(OFF_CLOCK + 4u) == 0u);

        /* ' (weather_next) locks weather; Ctrl+\ unlocks it so game cycle is free */
        ev = make_ev(FB_DEV_KEYBOARD, 0xDEu, FB_INPUT_PRESS, FB_MOD_NONE, 3);
        CHECK(mock_emit_input(&ev) == 1);
        mock_emit_frame(1.0 / 30.0);
        CHECK(peek_u32(OFF_WEATHER + 4u) == H_W1);
        poke_u32(OFF_WEATHER + 4u, H_W2);
        mock_emit_frame(1.0 / 30.0);
        CHECK(peek_u32(OFF_WEATHER + 4u) == H_W1); /* locked */

        ev = make_ev(FB_DEV_KEYBOARD, 0xDCu, FB_INPUT_PRESS, FB_MOD_CTRL, 4);
        CHECK(mock_emit_input(&ev) == 1);
        mock_emit_frame(1.0 / 30.0);
        poke_u32(OFF_WEATHER + 4u, H_W2);
        mock_emit_frame(1.0 / 30.0);
        CHECK(peek_u32(OFF_WEATHER + 4u) == H_W2); /* unlocked! */

        for (int i = 0; i < 4; ++i) {
            mock_emit_frame(1.0 / 30.0);
        }
        tw.shutdown();
        CHECK(file_contains_str(log_path, ",set,clock.h,"));
        CHECK(file_contains_str(log_path, ",verify,clock.h,"));
        CHECK(file_contains_str(log_path, ",weather_set,weather.next,"));
        FreeLibrary(tw.mod);
    }
}

/* ------------------------------------------------------------------------- */
/* 2. Mock Menu API: Zero Conflicts & Bidirectional Sync                     */
/* ------------------------------------------------------------------------- */

#define MAX_MOCK_SETTINGS 64
#define MAX_MOCK_ACTIONS  32

typedef struct mock_setting_slot {
    char                   mod_id[64];
    char                   section[64];
    char                   setting_id[64];
    uint32_t               type;
    fb_setting_value_t     val;
    fb_setting_change_fn_t on_change;
    fb_button_fn_t         on_button;
    fb_status_query_fn_t   on_status;
    void*                  user_ctx;
    int                    sync_calls;
} mock_setting_slot_t;

typedef struct mock_action_slot {
    char               mod_id[64];
    char               action_id[64];
    uint32_t           trigger_mode;
    int                consume_input;
    fb_binding_chord_t key;
    fb_binding_chord_t pad;
    fb_action_fn_t     callback;
    void*              user_ctx;
} mock_action_slot_t;

static mock_setting_slot_t g_mset[MAX_MOCK_SETTINGS];
static int                 g_mset_count = 0;
static mock_action_slot_t  g_mact[MAX_MOCK_ACTIONS];
static int                 g_mact_count = 0;

static mock_setting_slot_t* mmenu_find_setting(const char* mod_id, const char* sid) {
    for (int i = 0; i < g_mset_count; ++i) {
        if (strcmp(g_mset[i].mod_id, mod_id) == 0 && strcmp(g_mset[i].setting_id, sid) == 0) {
            return &g_mset[i];
        }
    }
    return NULL;
}

static mock_action_slot_t* mmenu_find_action(const char* mod_id, const char* aid) {
    for (int i = 0; i < g_mact_count; ++i) {
        if (strcmp(g_mact[i].mod_id, mod_id) == 0 && strcmp(g_mact[i].action_id, aid) == 0) {
            return &g_mact[i];
        }
    }
    return NULL;
}

static int mmenu_register_mod(const char* mod_id, const char* name, const char* desc) {
    (void)mod_id;
    (void)name;
    (void)desc;
    return FB_OK;
}

static int mmenu_register_setting(const fb_setting_desc_v1_t* d) {
    if (!d || g_mset_count >= MAX_MOCK_SETTINGS) {
        return FB_ERR_INTERNAL;
    }
    mock_setting_slot_t* s = &g_mset[g_mset_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->mod_id, sizeof(s->mod_id), "%s", d->mod_id ? d->mod_id : "");
    snprintf(s->section, sizeof(s->section), "%s", d->section ? d->section : "");
    snprintf(s->setting_id, sizeof(s->setting_id), "%s", d->setting_id ? d->setting_id : "");
    s->type = d->type;
    s->val.type = d->type;
    if (d->type == FB_SETTING_BOOL) {
        s->val.v.bool_val = d->default_bool ? 1 : 0;
    } else if (d->type == FB_SETTING_INT) {
        s->val.v.int_val = d->default_int;
    } else if (d->type == FB_SETTING_FLOAT) {
        s->val.v.float_val = d->default_float;
    } else if (d->type == FB_SETTING_ENUM) {
        s->val.v.enum_val = d->default_enum;
    }
    s->on_change = d->on_change;
    s->on_button = d->on_button;
    s->on_status = d->on_status;
    s->user_ctx = d->user_ctx;
    return FB_OK;
}

static int mmenu_register_action(const fb_action_desc_v1_t* d) {
    if (!d || g_mact_count >= MAX_MOCK_ACTIONS) {
        return FB_ERR_INTERNAL;
    }
    mock_action_slot_t* a = &g_mact[g_mact_count++];
    memset(a, 0, sizeof(*a));
    snprintf(a->mod_id, sizeof(a->mod_id), "%s", d->mod_id ? d->mod_id : "");
    snprintf(a->action_id, sizeof(a->action_id), "%s", d->action_id ? d->action_id : "");
    a->trigger_mode = d->trigger_mode;
    a->consume_input = d->consume_input;
    a->key = d->default_key;
    a->pad = d->default_pad;
    a->callback = d->callback;
    a->user_ctx = d->user_ctx;
    return FB_OK;
}

static int mmenu_get_setting(const char* mod_id, const char* sid, fb_setting_value_t* out_val) {
    mock_setting_slot_t* s = mmenu_find_setting(mod_id, sid);
    if (!s || !out_val) {
        return FB_ERR_NOT_FOUND;
    }
    *out_val = s->val;
    return FB_OK;
}

static int mmenu_set_setting(const char* mod_id, const char* sid, const fb_setting_value_t* val, int notify) {
    mock_setting_slot_t* s = mmenu_find_setting(mod_id, sid);
    if (!s || !val) {
        return FB_ERR_NOT_FOUND;
    }
    s->val = *val;
    if (!notify) {
        s->sync_calls += 1;
    }
    if (notify && s->on_change) {
        s->on_change(s->setting_id, &s->val, s->user_ctx);
    }
    return FB_OK;
}

static int mmenu_save_config(void) { return FB_OK; }
static int mmenu_is_open(void) { return 0; }

static void mmenu_user_set_bool(const char* mod_id, const char* sid, int b) {
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = FB_SETTING_BOOL;
    v.v.bool_val = b ? 1 : 0;
    CHECK(mmenu_set_setting(mod_id, sid, &v, 1) == FB_OK);
}

static void mmenu_user_set_int(const char* mod_id, const char* sid, int32_t x) {
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = FB_SETTING_INT;
    v.v.int_val = x;
    CHECK(mmenu_set_setting(mod_id, sid, &v, 1) == FB_OK);
}

static void mmenu_user_set_float(const char* mod_id, const char* sid, double x) {
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = FB_SETTING_FLOAT;
    v.v.float_val = x;
    CHECK(mmenu_set_setting(mod_id, sid, &v, 1) == FB_OK);
}

static void mmenu_user_set_enum(const char* mod_id, const char* sid, int32_t e) {
    fb_setting_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = FB_SETTING_ENUM;
    v.v.enum_val = e;
    CHECK(mmenu_set_setting(mod_id, sid, &v, 1) == FB_OK);
}

static void mmenu_user_click(const char* mod_id, const char* sid) {
    mock_setting_slot_t* s = mmenu_find_setting(mod_id, sid);
    CHECK(s != NULL && s->on_button != NULL);
    if (s && s->on_button) {
        s->on_button(s->setting_id, s->user_ctx);
    }
}

static void mmenu_fire_action(const char* mod_id, const char* aid, int active) {
    mock_action_slot_t* a = mmenu_find_action(mod_id, aid);
    CHECK(a != NULL && a->callback != NULL);
    if (a && a->callback) {
        a->callback(a->action_id, active, a->user_ctx);
    }
}

static void test_mock_menu_sync_and_conflicts(const wchar_t* fpv_dll, const wchar_t* tw_dll, const char* tmp_dir) {
    printf("[2] Testing Menu API registration, bidirectional sync & zero default binding conflicts...\n");

    g_mset_count = 0;
    g_mact_count = 0;
    memset(g_mem, 0, sizeof(g_mem));
    mock_reset_bus();

    /* Seed fireball.core's default Toggle Mod Menu action (F1 / LB + View) */
    fb_action_desc_v1_t core_act;
    memset(&core_act, 0, sizeof(core_act));
    core_act.struct_size = (uint32_t)sizeof(core_act);
    core_act.trigger_mode = FB_TRIGGER_PRESS;
    core_act.mod_id = "fireball.core";
    core_act.action_id = "toggle_menu";
    core_act.consume_input = 1;
    core_act.default_key.code = 0x70u; /* F1 */
    core_act.default_key.modifiers = FB_MOD_NONE;
    core_act.default_pad.code = FB_PAD_BACK;
    core_act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER;
    CHECK(mmenu_register_action(&core_act) == FB_OK);

    /* Weather list for time_weather */
    char list_path[300], jlist[600];
    snprintf(list_path, sizeof(list_path), "%s\\mock_weathers.txt", tmp_dir);
    FILE* f = fopen(list_path, "w");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "# index joaat_lower joaat name\n0 0x%08X 0x1 CLEAR\n1 0x%08X 0x2 SUNNY\n2 0x%08X 0x3 STORM\n",
                H_W0, H_W1, H_W2);
        fclose(f);
    }
    json_escape_path(jlist, sizeof(jlist), list_path);
    g_cfg_fpv[0] = '\0';
    snprintf(g_cfg_tw, sizeof(g_cfg_tw),
             "{\"clock_layout\":\"h,m,s\",\"weather_kind\":\"hash\",\"weather_list_path\":\"%s\",\"weather_blend_ms\":100}",
             jlist);

    poke_u32(OFF_CAM, 0u);
    poke_u32(OFF_CLOCK, 14u);
    poke_u32(OFF_CLOCK + 4u, 22u);
    poke_u32(OFF_WEATHER, H_W0);
    poke_u32(OFF_WEATHER + 4u, H_W0);

    loaded_plugin_t fpv, tw;
    CHECK(load_plugin_dll(fpv_dll, &fpv));
    CHECK(load_plugin_dll(tw_dll, &tw));
    if (!fpv.mod || !tw.mod) {
        return;
    }

    fb_core_services_v1_t svc = make_mock_services();
    fb_menu_api_v1_t menu;
    memset(&menu, 0, sizeof(menu));
    menu.struct_size = (uint32_t)sizeof(menu);
    menu.abi_version = FB_MENU_ABI_VERSION;
    menu.register_mod = mmenu_register_mod;
    menu.register_action = mmenu_register_action;
    menu.register_setting = mmenu_register_setting;
    menu.get_setting = mmenu_get_setting;
    menu.set_setting = mmenu_set_setting;
    menu.save_user_config = mmenu_save_config;
    menu.is_menu_open = mmenu_is_open;

    CHECK(fpv.init(&svc) == FB_OK);
    CHECK(fpv.reg_menu != NULL && fpv.reg_menu(&menu) == FB_OK);
    CHECK(tw.init(&svc) == FB_OK);
    CHECK(tw.reg_menu != NULL && tw.reg_menu(&menu) == FB_OK);

    /* Verify ZERO default keyboard or gamepad binding conflicts across all 3 mods */
    CHECK(g_mact_count == 1 + 2 + 6); /* core(1) + fpv(2) + tw(6); opt-in toggle_window is OFF by default */
    CHECK(mmenu_find_action("community.time_weather_control", "toggle_window") == NULL);
    for (int i = 0; i < g_mact_count; ++i) {
        for (int j = i + 1; j < g_mact_count; ++j) {
            if (g_mact[i].key.code != 0u && g_mact[j].key.code != 0u) {
                int kdup = (g_mact[i].key.code == g_mact[j].key.code &&
                            g_mact[i].key.modifiers == g_mact[j].key.modifiers);
                CHECK(!kdup);
            }
            if (g_mact[i].pad.code != 0u && g_mact[j].pad.code != 0u) {
                int pdup = (g_mact[i].pad.code == g_mact[j].pad.code &&
                            g_mact[i].pad.modifiers == g_mact[j].pad.modifiers);
                CHECK(!pdup);
            }
        }
    }

    /* --- FPV Menu Sync & Status --- */
    mmenu_fire_action("community.fpv_unlocker", "toggle_fpv", 1);
    CHECK(peek_u32(OFF_CAM) == 2u);
    fb_setting_value_t fv;
    CHECK(mmenu_get_setting("community.fpv_unlocker", "view_mode_preset", &fv) == FB_OK);
    CHECK(fv.v.enum_val == 2); /* Synced to FirstPerson */

    mmenu_user_click("community.fpv_unlocker", "toggle_now");
    CHECK(peek_u32(OFF_CAM) == 0u);
    CHECK(mmenu_get_setting("community.fpv_unlocker", "view_mode_preset", &fv) == FB_OK);
    CHECK(fv.v.enum_val == 0); /* Synced to ThirdPerson */

    mmenu_user_set_enum("community.fpv_unlocker", "view_mode_preset", 1); /* Cinematic */
    CHECK(peek_u32(OFF_CAM) == 1u);

    /* Peek from Cinematic -> FirstPerson, hold-guarded, restore Cinematic on release */
    mmenu_fire_action("community.fpv_unlocker", "peek_fpv", 1);
    CHECK(peek_u32(OFF_CAM) == 2u);
    poke_u32(OFF_CAM, 0u);
    mock_emit_frame(1.0 / 60.0);
    CHECK(peek_u32(OFF_CAM) == 2u);
    mmenu_fire_action("community.fpv_unlocker", "peek_fpv", 0);
    CHECK(peek_u32(OFF_CAM) == 1u);
    CHECK(mmenu_get_setting("community.fpv_unlocker", "view_mode_preset", &fv) == FB_OK);
    CHECK(fv.v.enum_val == 1); /* Restored Cinematic in menu */

    /* Status string contains latch state, mode name, toggles & re-asserts */
    mock_setting_slot_t* fpv_st = mmenu_find_setting("community.fpv_unlocker", "status");
    CHECK(fpv_st != NULL && fpv_st->on_status != NULL);
    if (fpv_st && fpv_st->on_status) {
        char buf[256];
        fpv_st->on_status("status", buf, sizeof(buf), fpv_st->user_ctx);
        CHECK(strstr(buf, "Cinematic") != NULL);
        CHECK(strstr(buf, "toggles=") != NULL);
        CHECK(strstr(buf, "re-asserts=1") != NULL);
        poke_u32(OFF_CAM, 7u);
        mock_emit_frame(1.0 / 60.0);
        fpv_st->on_status("status", buf, sizeof(buf), fpv_st->user_ctx);
        CHECK(strstr(buf, "raw=7") != NULL);
    }

    /* --- Time & Weather Bidirectional Sync, Hour/Min, Quick Presets, Apply Mode --- */
    /* Hour/Minute staged edits do NOT overwrite clock until Apply Time Now */
    mmenu_user_set_int("community.time_weather_control", "clock_hour", 7);
    mmenu_user_set_int("community.time_weather_control", "clock_minute", 35);
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 14u && peek_u32(OFF_CLOCK + 4u) == 22u);

    mmenu_user_click("community.time_weather_control", "apply_time_btn");
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 7u && peek_u32(OFF_CLOCK + 4u) == 35u);

    /* Quick Presets */
    mmenu_user_click("community.time_weather_control", "preset_dawn_btn");
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 6u && peek_u32(OFF_CLOCK + 4u) == 0u);
    mmenu_user_click("community.time_weather_control", "preset_noon_btn");
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 12u && peek_u32(OFF_CLOCK + 4u) == 0u);
    mmenu_user_click("community.time_weather_control", "preset_sunset_btn");
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 18u && peek_u32(OFF_CLOCK + 4u) == 0u);
    mmenu_user_click("community.time_weather_control", "preset_midnight_btn");
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 0u && peek_u32(OFF_CLOCK + 4u) == 0u);

    /* Hotkey toggle_pause syncs clock_paused to Mod Menu */
    mmenu_fire_action("community.time_weather_control", "toggle_pause", 1);
    mock_emit_frame(1.0 / 30.0);
    CHECK(mmenu_get_setting("community.time_weather_control", "clock_paused", &fv) == FB_OK && fv.v.bool_val == 1);
    mmenu_fire_action("community.time_weather_control", "toggle_pause", 1);
    mock_emit_frame(1.0 / 30.0);
    CHECK(mmenu_get_setting("community.time_weather_control", "clock_paused", &fv) == FB_OK && fv.v.bool_val == 0);

    /* Hotkey weather_next (Instant) syncs weather_preset & weather_locked */
    mmenu_fire_action("community.time_weather_control", "weather_next", 1);
    mock_emit_frame(1.0 / 30.0);
    CHECK(peek_u32(OFF_WEATHER) == H_W1 && peek_u32(OFF_WEATHER + 4u) == H_W1);
    CHECK(mmenu_get_setting("community.time_weather_control", "weather_preset", &fv) == FB_OK && fv.v.enum_val == 1);
    CHECK(mmenu_get_setting("community.time_weather_control", "weather_locked", &fv) == FB_OK && fv.v.bool_val == 1);

    /* toggle_weather_lock syncs weather_locked -> 0 */
    mmenu_fire_action("community.time_weather_control", "toggle_weather_lock", 1);
    mock_emit_frame(1.0 / 30.0);
    CHECK(mmenu_get_setting("community.time_weather_control", "weather_locked", &fv) == FB_OK && fv.v.bool_val == 0);

    /* Switch weather_apply_mode to Blend (1) and weather_lock_on_set to false */
    mmenu_user_set_enum("community.time_weather_control", "weather_apply_mode", 1);
    mmenu_user_set_float("community.time_weather_control", "weather_blend_ms", 100.0);
    mmenu_user_set_bool("community.time_weather_control", "weather_lock_on_set", 0);
    mmenu_fire_action("community.time_weather_control", "weather_next", 1);
    mock_emit_frame(0.05);
    CHECK(peek_u32(OFF_WEATHER) == H_W1 && peek_u32(OFF_WEATHER + 4u) == H_W2);
    CHECK(fabsf(peek_f32(OFF_WEATHER + 8u) - 0.0f) < 1e-4f);
    CHECK(mmenu_get_setting("community.time_weather_control", "weather_preset", &fv) == FB_OK && fv.v.enum_val == 2);
    CHECK(mmenu_get_setting("community.time_weather_control", "weather_locked", &fv) == FB_OK && fv.v.bool_val == 0);
    mock_emit_frame(0.05); /* 50ms / 100ms -> 0.5 */
    CHECK(fabsf(peek_f32(OFF_WEATHER + 8u) - 0.5f) < 1e-3f);
    mock_emit_frame(0.06); /* >= 100ms -> blend done */
    CHECK(peek_u32(OFF_WEATHER) == H_W2 && peek_u32(OFF_WEATHER + 4u) == H_W2);

    fpv.shutdown();
    tw.shutdown();
    FreeLibrary(fpv.mod);
    FreeLibrary(tw.mod);
}

/* ------------------------------------------------------------------------- */
/* 3. End-to-End Integration inside fireball_core.dll (FIREBALL_HEADLESS=1)  */
/* ------------------------------------------------------------------------- */

static uint64_t fnv1a_64_str(const char* s) {
    uint64_t h = 0xCBF29CE484222325ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x00000100000001B3ull;
    }
    return h;
}

static void put_u32_le(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put_u64_le(uint8_t* p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* Minimal valid FBIN v1 writer with 1 plugin_config entry so time_weather gets weather_list_path */
static int write_single_cfg_bundle(const wchar_t* path, const char* mod_id, const char* cfg_json) {
    uint32_t id_len = (uint32_t)strlen(mod_id);
    uint32_t js_len = (uint32_t)strlen(cfg_json);
    uint32_t hdr_sz = 64u;
    uint32_t cfg_off = hdr_sz;
    uint32_t str_off = cfg_off + 32u;
    uint32_t id_off = str_off;
    uint32_t js_off = id_off + id_len + 1u;
    uint32_t total = (js_off + js_len + 1u + 15u) & ~15u;

    uint8_t* buf = (uint8_t*)calloc(1, total);
    if (!buf) {
        return 0;
    }

    /* Header (64 bytes): <IIIIIIIIQQQQ */
    put_u32_le(buf + 0, 0x4E494246u); /* "FBIN" */
    put_u32_le(buf + 4, 1u);          /* version 1 */
    put_u32_le(buf + 8, 0u);          /* flags */
    put_u32_le(buf + 12, hdr_sz);
    put_u32_le(buf + 16, 0u);         /* dstorage_count */
    put_u32_le(buf + 20, 0u);         /* overlay_count */
    put_u32_le(buf + 24, 1u);         /* plugin_config_count */
    put_u32_le(buf + 28, 0u);         /* reserved */
    put_u64_le(buf + 32, hdr_sz);     /* dstorage_table_off */
    put_u64_le(buf + 40, hdr_sz);     /* overlay_table_off */
    put_u64_le(buf + 48, cfg_off);    /* plugin_cfg_table_off */
    put_u64_le(buf + 56, total);      /* total_size */

    /* Plugin cfg entry (32 bytes): <QIIIIQ */
    put_u64_le(buf + cfg_off + 0, fnv1a_64_str(mod_id));
    put_u32_le(buf + cfg_off + 8, id_off);
    put_u32_le(buf + cfg_off + 12, id_len);
    put_u32_le(buf + cfg_off + 16, js_off);
    put_u32_le(buf + cfg_off + 20, js_len);
    put_u64_le(buf + cfg_off + 24, 0ull);

    memcpy(buf + id_off, mod_id, id_len + 1u);
    memcpy(buf + js_off, cfg_json, js_len + 1u);

    FILE* f = _wfopen(path, L"wb");
    if (!f) {
        free(buf);
        return 0;
    }
    size_t wrote = fwrite(buf, 1, total, f);
    fclose(f);
    free(buf);
    return wrote == total;
}

static void e2e_log(int level, const char* sub, const char* msg) {
    (void)level;
    (void)sub;
    (void)msg;
}

static int copy_into_dir(const wchar_t* src, const wchar_t* dir, const wchar_t* name) {
    wchar_t dst[MAX_PATH * 2];
    swprintf(dst, sizeof(dst) / sizeof(dst[0]), L"%ls\\%ls", dir, name);
    return CopyFileW(src, dst, FALSE) ? 1 : 0;
}

static void press_and_release(fb_modcore_callbacks_v1_t* cb, uint32_t dev, uint32_t code, uint32_t mods, uint64_t* ts) {
    fb_input_event_v1_t ev = make_ev(dev, code, FB_INPUT_PRESS, mods, (*ts) += 10);
    (void)cb->on_input_event(&ev);
    ev = make_ev(dev, code, FB_INPUT_RELEASE, mods, (*ts) += 10);
    (void)cb->on_input_event(&ev);
}

static void test_e2e_fireball_core(
    const wchar_t* core_dll,
    const wchar_t* fpv_dll,
    const wchar_t* tw_dll,
    const wchar_t* tmp_wdir,
    const char*    tmp_adir)
{
    printf("[3] Testing end-to-end inside fireball_core.dll (FIREBALL_HEADLESS=1)...\n");
    SetEnvironmentVariableW(L"FIREBALL_HEADLESS", L"1");

    wchar_t mods_dir[MAX_PATH * 2];
    swprintf(mods_dir, sizeof(mods_dir) / sizeof(mods_dir[0]), L"%ls\\e2e_mods", tmp_wdir);
    CreateDirectoryW(mods_dir, NULL);

    wchar_t cfg_file[MAX_PATH * 2];
    swprintf(cfg_file, sizeof(cfg_file) / sizeof(cfg_file[0]), L"%ls\\fireball_user_config.json", mods_dir);
    DeleteFileW(cfg_file);

    CHECK(copy_into_dir(fpv_dll, mods_dir, L"fpv_toggle.fbplugin.dll"));
    CHECK(copy_into_dir(tw_dll, mods_dir, L"time_weather.fbplugin.dll"));

    /* Write weather list + minimal fireball_bundle.fbin supplying time_weather config */
    char list_path[300], jlist[600], tw_cfg_json[1024];
    snprintf(list_path, sizeof(list_path), "%s\\e2e_weathers.txt", tmp_adir);
    FILE* wf = fopen(list_path, "w");
    CHECK(wf != NULL);
    if (wf) {
        fprintf(wf, "# index joaat_lower joaat name\n0 0x%08X 0x1 CLEAR\n1 0x%08X 0x2 SUNNY\n2 0x%08X 0x3 STORM\n",
                H_W0, H_W1, H_W2);
        fclose(wf);
    }
    json_escape_path(jlist, sizeof(jlist), list_path);
    snprintf(tw_cfg_json, sizeof(tw_cfg_json),
             "{\"clock_layout\":\"h,m,s\",\"weather_kind\":\"hash\",\"weather_list_path\":\"%s\",\"weather_blend_ms\":100}",
             jlist);

    wchar_t bundle_path[MAX_PATH * 2];
    swprintf(bundle_path, sizeof(bundle_path) / sizeof(bundle_path[0]), L"%ls\\fireball_bundle.fbin", mods_dir);
    CHECK(write_single_cfg_bundle(bundle_path, "community.time_weather_control", tw_cfg_json));

    /* Pre-seed fireball_user_config.json with persisted clock_hour/minute + stale clock_paused/time_scale
     * AND weather_apply_mode=1 (Blend) to prove NO boot-time clock overwrite while preferences persist! */
    {
        FILE* cf = _wfopen(cfg_file, L"wb");
        CHECK(cf != NULL);
        if (cf) {
            fprintf(cf,
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"settings\": {\n"
                    "    \"community.time_weather_control\": {\n"
                    "      \"clock_hour\": 3,\n"
                    "      \"clock_minute\": 45,\n"
                    "      \"clock_paused\": true,\n"
                    "      \"time_scale\": 4,\n"
                    "      \"weather_apply_mode\": 1,\n"
                    "      \"weather_blend_ms\": 100.0\n"
                    "    }\n"
                    "  },\n"
                    "  \"bindings\": {}\n"
                    "}\n");
            fclose(cf);
        }
    }

    HMODULE core = LoadLibraryW(core_dll);
    CHECK(core != NULL);
    if (!core) {
        return;
    }
    fb_attach_host_fn attach_fn = (fb_attach_host_fn)(void*)GetProcAddress(core, "fb_attach_host");
    CHECK(attach_fn != NULL);
    if (!attach_fn) {
        FreeLibrary(core);
        return;
    }

    memset(g_mem, 0, sizeof(g_mem));
    poke_u32(OFF_CAM, 0u);
    poke_u32(OFF_CLOCK, 14u);
    poke_u32(OFF_CLOCK + 4u, 22u);
    poke_u32(OFF_CLOCK + 8u, 10u);
    poke_u32(OFF_WEATHER, H_W0);
    poke_u32(OFF_WEATHER + 4u, H_W0);

    fb_host_api_v1_t api;
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = FB_HOST_ABI_VERSION;
    api.host_name = "FireballMods-E2E";
    api.host_version = "0.2.0";
    api.game_root_dir = L"D:\\Games\\GTA6";
    api.mods_root_dir = mods_dir;
    api.capabilities = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    api.log = e2e_log;
    api.resolve_symbol = mock_resolve_symbol;
    api.title_mem_read_u32 = mock_read_u32;
    api.title_mem_write_u32 = mock_write_u32;

    fb_modcore_callbacks_v1_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = (uint32_t)sizeof(cb);
    CHECK(attach_fn(&api, &cb) == FB_OK);

    /* 3a. No boot-time clock overwrite (clock remains 14:22:10 even after frames!) */
    cb.on_frame_begin(1u, 1.0 / 30.0);
    cb.on_frame_begin(2u, 1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 14u && peek_u32(OFF_CLOCK + 4u) == 22u && peek_u32(OFF_CLOCK + 8u) == 10u);

    uint64_t ts = 1000;

    /* 3b. fpv_toggle FB_TRIGGER_HOLD peek via keyboard Alt+V (preserves Cinematic=1) */
    poke_u32(OFF_CAM, 1u);
    fb_input_event_v1_t ev = make_ev(FB_DEV_KEYBOARD, 0x12u /* VK_MENU */, FB_INPUT_PRESS, FB_MOD_ALT, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, FB_MOD_ALT, ts += 10);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(peek_u32(OFF_CAM) == 2u);
    poke_u32(OFF_CAM, 0u); /* context reset while peeking */
    cb.on_frame_begin(3u, 1.0 / 60.0);
    CHECK(peek_u32(OFF_CAM) == 2u); /* hold-guarded */
    ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_RELEASE, FB_MOD_ALT, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_KEYBOARD, 0x12u, FB_INPUT_RELEASE, FB_MOD_NONE, ts += 10);
    cb.on_input_event(&ev);
    CHECK(peek_u32(OFF_CAM) == 1u); /* restored Cinematic! */

    /* 3c. fpv_toggle FB_TRIGGER_HOLD peek via gamepad chord LB + L3 */
    poke_u32(OFF_CAM, 0u);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_PRESS, 0, ts += 10);
    CHECK(cb.on_input_event(&ev) == 0);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_THUMB, FB_INPUT_PRESS, 0, ts += 10);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(peek_u32(OFF_CAM) == 2u);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_THUMB, FB_INPUT_RELEASE, 0, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_RELEASE, 0, ts += 10);
    cb.on_input_event(&ev);
    CHECK(peek_u32(OFF_CAM) == 0u); /* restored ThirdPerson! */

    /* 3d. time_weather persisted weather_apply_mode=Blend respected by ' (weather_next) */
    press_and_release(&cb, FB_DEV_KEYBOARD, 0xDEu /* ' */, FB_MOD_NONE, &ts);
    cb.on_frame_begin(4u, 0.05);
    CHECK(peek_u32(OFF_WEATHER) == H_W0 && peek_u32(OFF_WEATHER + 4u) == H_W1);
    CHECK(fabsf(peek_f32(OFF_WEATHER + 8u) - 0.0f) < 1e-4f);
    cb.on_frame_begin(5u, 0.05);
    CHECK(fabsf(peek_f32(OFF_WEATHER + 8u) - 0.5f) < 1e-3f);
    cb.on_frame_begin(6u, 0.06);
    CHECK(peek_u32(OFF_WEATHER) == H_W1 && peek_u32(OFF_WEATHER + 4u) == H_W1);

    /* 3e. time_weather toggle_weather_lock via gamepad chord LB + Y (unlocks weather) */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_PRESS, 0, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_Y, FB_INPUT_PRESS, 0, ts += 10);
    CHECK(cb.on_input_event(&ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_Y, FB_INPUT_RELEASE, 0, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_RELEASE, 0, ts += 10);
    cb.on_input_event(&ev);
    cb.on_frame_begin(7u, 1.0 / 30.0);
    poke_u32(OFF_WEATHER + 4u, H_W2);
    cb.on_frame_begin(8u, 1.0 / 30.0);
    CHECK(peek_u32(OFF_WEATHER + 4u) == H_W2); /* unlocked! */

    /* Re-lock via keyboard Ctrl + \ */
    ev = make_ev(FB_DEV_KEYBOARD, 0x11u /* VK_CONTROL */, FB_INPUT_PRESS, FB_MOD_CTRL, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_KEYBOARD, 0xDCu /* \ */, FB_INPUT_PRESS, FB_MOD_CTRL, ts += 10);
    CHECK(cb.on_input_event(&ev) == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 0xDCu, FB_INPUT_RELEASE, FB_MOD_CTRL, ts += 10);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_KEYBOARD, 0x11u, FB_INPUT_RELEASE, FB_MOD_NONE, ts += 10);
    cb.on_input_event(&ev);
    cb.on_frame_begin(9u, 1.0 / 30.0);
    poke_u32(OFF_WEATHER + 4u, H_W0);
    cb.on_frame_begin(10u, 1.0 / 30.0);
    CHECK(peek_u32(OFF_WEATHER + 4u) == H_W1); /* locked back to H_W1! */

    /* 3f. Headless Mod Menu navigation:
     * Open menu (F1), RB -> Tab 1 (FPV Unlocker):
     *   Selectable rows on Tab 1:
     *     0: hold_guard_enabled
     *     1: start_in_fpv  -> press A to enable & persist!
     *     2: restore_on_shutdown
     * RB -> Tab 2 (Time & Weather):
     *   Selectable rows on Tab 2:
     *     0: clock_paused
     *     1: time_scale
     *     2: clock_hour (persisted as 3)
     *     3: clock_minute (persisted as 45)
     *     4: apply_time_btn -> press A -> sets clock to 03:45:00!
     *     5: preset_dawn_btn -> press A -> sets clock to 06:00:00!
     */
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts);
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, 0, &ts); /* Tab 1: FPV Unlocker */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, 0, &ts);      /* row 1: start_in_fpv */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_A, 0, &ts);              /* toggle start_in_fpv -> true (saved!) */

    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, 0, &ts); /* Tab 2: Time & Weather */
    for (int i = 0; i < 4; ++i) {
        press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, 0, &ts);
    }
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_A, 0, &ts);              /* click apply_time_btn (03:45) */
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts); /* close menu */
    cb.on_frame_begin(11u, 1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 3u && peek_u32(OFF_CLOCK + 4u) == 45u);

    /* Quick preset Dawn (06:00) via headless menu */
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts);
    /* Menu remembers Tab 2 and row 4; DPad Down -> row 5 (preset_dawn_btn) */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, 0, &ts);
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_A, 0, &ts);
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts);
    cb.on_frame_begin(12u, 1.0 / 30.0);
    CHECK(peek_u32(OFF_CLOCK) == 6u && peek_u32(OFF_CLOCK + 4u) == 0u);

    /* 3g. Shutdown & Re-attach: verify start_in_fpv persisted across re-attach & restore_on_shutdown works! */
    cb.on_shutdown();

    poke_u32(OFF_CAM, 0u);
    CHECK(attach_fn(&api, &cb) == FB_OK);
    CHECK(peek_u32(OFF_CAM) == 0u);
    cb.on_frame_begin(1u, 1.0 / 60.0);
    CHECK(peek_u32(OFF_CAM) == 2u); /* Persisted start_in_fpv latched FirstPerson on first valid frame! */

    /* With default restore_on_shutdown=true, shutdown restores ThirdPerson (0) */
    cb.on_shutdown();
    CHECK(peek_u32(OFF_CAM) == 0u);

    /* Re-attach, disable restore_on_shutdown in menu, latch FPV, shutdown -> stays 2! */
    CHECK(attach_fn(&api, &cb) == FB_OK);
    cb.on_frame_begin(1u, 1.0 / 60.0);
    CHECK(peek_u32(OFF_CAM) == 2u);
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts);
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, 0, &ts); /* Tab 1: FPV Unlocker */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, 0, &ts);      /* row 1: start_in_fpv */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, 0, &ts);      /* row 2: restore_on_shutdown */
    press_and_release(&cb, FB_DEV_GAMEPAD, FB_PAD_A, 0, &ts);              /* toggle restore_on_shutdown -> false */
    press_and_release(&cb, FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_MOD_NONE, &ts);
    cb.on_shutdown();
    CHECK(peek_u32(OFF_CAM) == 2u); /* restore_on_shutdown=false preserved FirstPerson! */

    FreeLibrary(core);
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* core_dll = NULL;
    const wchar_t* fpv_dll = NULL;
    const wchar_t* tw_dll = NULL;

    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--core") == 0) {
            core_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--fpv") == 0) {
            fpv_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--time-weather") == 0) {
            tw_dll = argv[++i];
        }
    }

    if (!fpv_dll || !tw_dll) {
        fprintf(stderr, "Usage: fireball_mods_integration_test --fpv <dll> --time-weather <dll> [--core <dll>]\n");
        return 2;
    }

    wchar_t tmp_w[MAX_PATH];
    char    tmp_a[MAX_PATH];
    DWORD wn = GetTempPathW((DWORD)(sizeof(tmp_w) / sizeof(tmp_w[0])), tmp_w);
    DWORD an = GetTempPathA((DWORD)sizeof(tmp_a), tmp_a);
    if (wn == 0 || wn >= MAX_PATH || an == 0 || an >= MAX_PATH) {
        return 2;
    }

    wchar_t work_w[MAX_PATH * 2];
    char    work_a[MAX_PATH * 2];
    swprintf(work_w, sizeof(work_w) / sizeof(work_w[0]), L"%lsfireball_mods_itest", tmp_w);
    snprintf(work_a, sizeof(work_a), "%sfireball_mods_itest", tmp_a);
    CreateDirectoryW(work_w, NULL);

    test_standalone_v1(fpv_dll, tw_dll, work_a);
    test_mock_menu_sync_and_conflicts(fpv_dll, tw_dll, work_a);
    if (core_dll) {
        test_e2e_fireball_core(core_dll, fpv_dll, tw_dll, work_w, work_a);
    } else {
        printf("[3] Skipped fireball_core.dll E2E (--core not provided)\n");
    }

    printf("\nFireball-Mods integration tests: %s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
