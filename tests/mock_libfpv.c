/*
 * mock_libfpv.c — Test-only pure C11 mock `libfpv` DLL implementing `LIBFPV_ABI_VERSION = 1`.
 *
 * Used strictly by `fireball_mods_integration_test` (CTest) to verify dynamic `LoadLibraryW`
 * loading, ABI validation, RAGE 9 coordinate setup, raycast callback invocation, 50 ms settle
 * guard, multi-view camera output, preset/sim options, audio pull, OSD RGBA rendering, and
 * keyboard/overlay/control forwarding in `fpv_drone.fbplugin.dll` without requiring a live
 * Betaflight SimITL binary during CI.
 */

#include "libfpv_abi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define MOCK_FPV_API __declspec(dllexport)
#else
#  define MOCK_FPV_API __attribute__((visibility("default")))
#endif

struct fpv_drone {
    float    pos[3];
    float    rot[4];
    float    vel[3];
    float    pilot_pos[3];
    int      armed;
    int      repaired_count;
    int      reset_count;
    char     preset[64];
};

static const char* const k_mock_presets[3] = {
    "Freestyler 5",
    "Whoop 65",
    "LongRange 7"
};

static int                g_inited = 0;
static fpv_global_config  g_cfg;
static fpv_drone*         g_active_drone = NULL;
static fpv_raycast_fn     g_ray_fn = NULL;
static fpv_raycast_ex_fn  g_ray_ex_fn = NULL;
static void*              g_ray_user = NULL;
static fpv_view           g_view = FPV_VIEW_FPV;
static uint32_t           g_sim_opts = 0u;
static int                g_paused = 0;
static float              g_time_scale = 1.0f;
static double             g_sim_time = 0.0;
static int                g_overlay_open = 0;
static int                g_toggle_key = FPV_KEY_F8;
static int                g_key_w_down = 0;
static float              g_audio_vol = 1.0f;
static float              g_beep_vol = 1.0f;
static float              g_wind[3] = {0.0f, 0.0f, 0.0f};
static uint32_t           g_pending_action = 0u;
static char               g_last_err[128] = "";

MOCK_FPV_API uint32_t fpv_abi_version(void) {
    return LIBFPV_ABI_VERSION;
}

MOCK_FPV_API const char* fpv_version_string(void) {
    return "1.1.3-mock";
}

MOCK_FPV_API const char* fpv_result_string(fpv_result result) {
    return result == FPV_OK ? "FPV_OK" : "FPV_ERROR";
}

MOCK_FPV_API const char* fpv_last_error(void) {
    return g_last_err;
}

MOCK_FPV_API void fpv_free(void* ptr) {
    free(ptr);
}

MOCK_FPV_API void fpv_global_config_defaults(fpv_global_config* config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->coords.right = FPV_AXIS_POS_X;
    config->coords.up = FPV_AXIS_POS_Y;
    config->coords.forward = FPV_AXIS_NEG_Z;
    config->coords.units_per_meter = 1.0f;
    config->configurator_port = 5761;
    config->sim_rate_hz = 1000.0f;
    config->auto_pause_seconds = 0.5f;
}

MOCK_FPV_API fpv_result fpv_shared_data_dir(char* out, uint32_t out_size) {
    if (!out || out_size < 16u) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    snprintf(out, out_size, "mock_libfpv_data");
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_global_init(const fpv_global_config* config) {
    if (!config || config->struct_size < sizeof(uint32_t)) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    if (g_inited) {
        return FPV_ERROR_ALREADY_INITIALIZED;
    }
    g_cfg = *config;
    g_inited = 1;
    g_sim_time = 0.0;
    g_paused = 0;
    g_time_scale = 1.0f;
    g_overlay_open = 0;
    g_key_w_down = 0;
    g_pending_action = 0u;
    if (g_cfg.log) {
        g_cfg.log(g_cfg.log_user, FPV_LOG_INFO, "mock libfpv initialized");
    }
    return FPV_OK;
}

MOCK_FPV_API void fpv_global_terminate(void) {
    if (g_active_drone) {
        free(g_active_drone);
        g_active_drone = NULL;
    }
    g_inited = 0;
    g_ray_fn = NULL;
    g_ray_ex_fn = NULL;
    g_ray_user = NULL;
}

MOCK_FPV_API int fpv_is_initialized(void) {
    return g_inited;
}

MOCK_FPV_API fpv_result fpv_material_set(uint8_t material_id, const fpv_material* material) {
    (void)material_id;
    (void)material;
    return g_inited ? FPV_OK : FPV_ERROR_NOT_INITIALIZED;
}

MOCK_FPV_API void fpv_world_set_raycast_callback(fpv_raycast_fn callback, void* user) {
    g_ray_fn = callback;
    g_ray_ex_fn = NULL;
    g_ray_user = user;
}

MOCK_FPV_API void fpv_world_set_raycast_callback_ex(fpv_raycast_ex_fn callback, void* user) {
    g_ray_ex_fn = callback;
    g_ray_fn = NULL;
    g_ray_user = user;
}

MOCK_FPV_API void fpv_drone_desc_defaults(fpv_drone_desc* desc) {
    if (!desc) {
        return;
    }
    memset(desc, 0, sizeof(*desc));
    desc->struct_size = (uint32_t)sizeof(*desc);
    desc->rotation[3] = 1.0f;
}

MOCK_FPV_API fpv_result fpv_drone_create(const fpv_drone_desc* desc, fpv_drone** out_drone) {
    if (!g_inited) {
        return FPV_ERROR_NOT_INITIALIZED;
    }
    if (!out_drone) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    if (g_active_drone) {
        return FPV_ERROR_DRONE_EXISTS;
    }
    fpv_drone* d = (fpv_drone*)calloc(1, sizeof(fpv_drone));
    if (!d) {
        return FPV_ERROR_INTERNAL;
    }
    d->rot[3] = 1.0f;
    snprintf(d->preset, sizeof(d->preset), "%s", k_mock_presets[0]);
    if (desc) {
        memcpy(d->pos, desc->position, sizeof(d->pos));
        if (desc->rotation[0] != 0.0f || desc->rotation[1] != 0.0f ||
            desc->rotation[2] != 0.0f || desc->rotation[3] != 0.0f)
        {
            memcpy(d->rot, desc->rotation, sizeof(d->rot));
        }
        memcpy(d->pilot_pos, desc->pilot_position, sizeof(d->pilot_pos));
        if (desc->preset && desc->preset[0]) {
            snprintf(d->preset, sizeof(d->preset), "%s", desc->preset);
        }
    }
    g_active_drone = d;
    *out_drone = d;
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_delete(fpv_drone* drone) {
    if (!drone || drone != g_active_drone) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    free(drone);
    g_active_drone = NULL;
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_reset(fpv_drone* drone, const float position[3], const float rotation[4]) {
    if (!drone || drone != g_active_drone || !position) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    memcpy(drone->pos, position, sizeof(drone->pos));
    if (rotation) {
        memcpy(drone->rot, rotation, sizeof(drone->rot));
    }
    drone->vel[0] = drone->vel[1] = drone->vel[2] = 0.0f;
    drone->reset_count += 1;
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_repair(fpv_drone* drone) {
    if (!drone || drone != g_active_drone) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    drone->repaired_count += 1;
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_set_pilot_position(fpv_drone* drone, const float position[3]) {
    if (!drone || !position) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    memcpy(drone->pilot_pos, position, sizeof(drone->pilot_pos));
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_set_preset(fpv_drone* drone, const char* preset) {
    if (!drone || !preset) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    snprintf(drone->preset, sizeof(drone->preset), "%s", preset);
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_gps_set_home(const float position[3]) {
    (void)position;
    return g_inited ? FPV_OK : FPV_ERROR_NOT_INITIALIZED;
}

MOCK_FPV_API void fpv_set_view(fpv_view view) {
    g_view = view;
}

MOCK_FPV_API void fpv_sim_set_options(uint32_t options) {
    g_sim_opts = options;
}

MOCK_FPV_API uint32_t fpv_sim_get_options(void) {
    return g_sim_opts;
}

MOCK_FPV_API void fpv_wind_defaults(fpv_wind* wind) {
    if (!wind) {
        return;
    }
    memset(wind, 0, sizeof(*wind));
    wind->struct_size = (uint32_t)sizeof(*wind);
    wind->turbulence = -1.0f;
}

MOCK_FPV_API fpv_result fpv_wind_set(const fpv_wind* wind) {
    if (!g_inited) {
        return FPV_ERROR_NOT_INITIALIZED;
    }
    if (wind) {
        memcpy(g_wind, wind->velocity, sizeof(g_wind));
    } else {
        g_wind[0] = g_wind[1] = g_wind[2] = 0.0f;
    }
    return FPV_OK;
}

MOCK_FPV_API void fpv_set_paused(int paused) {
    g_paused = paused ? 1 : 0;
}

MOCK_FPV_API int fpv_is_paused(void) {
    return g_paused;
}

MOCK_FPV_API void fpv_set_time_scale(float scale) {
    g_time_scale = scale;
}

MOCK_FPV_API fpv_result fpv_input_get_status(fpv_input_status* status) {
    if (!status) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    status->source = FPV_INPUT_SOURCE_KEYBOARD;
    snprintf(status->device_name, sizeof(status->device_name), "Keyboard");
    status->needs_setup = 0;
    status->device_count = 0;
    return FPV_OK;
}

MOCK_FPV_API void fpv_input_key(int key, int down) {
    if (key == 'W') {
        g_key_w_down = down ? 1 : 0;
    } else if (key == FPV_KEY_ENTER && down && g_active_drone) {
        g_active_drone->armed = !g_active_drone->armed;
    } else if (key == g_toggle_key && down) {
        g_overlay_open = !g_overlay_open;
    }
}

MOCK_FPV_API fpv_result fpv_controls_set(const fpv_control_desc* controls, uint32_t count) {
    (void)controls;
    (void)count;
    return FPV_OK;
}

MOCK_FPV_API int fpv_control_poll(uint32_t id, uint32_t* presses) {
    (void)id;
    if (presses) {
        *presses = 0u;
    }
    return 0;
}

MOCK_FPV_API void fpv_overlay_set_open(int open) {
    g_overlay_open = open ? 1 : 0;
}

MOCK_FPV_API int fpv_overlay_is_open(void) {
    return g_overlay_open;
}

MOCK_FPV_API int fpv_overlay_wants_input(void) {
    return g_overlay_open;
}

MOCK_FPV_API void fpv_overlay_set_toggle_key(int key) {
    g_toggle_key = key;
}

MOCK_FPV_API void fpv_overlay_set_action(uint32_t id, const char* label) {
    (void)id;
    (void)label;
}

MOCK_FPV_API uint32_t fpv_overlay_poll_action(void) {
    uint32_t a = g_pending_action;
    g_pending_action = 0u;
    return a;
}

MOCK_FPV_API const fpv_draw_data* fpv_overlay_render(float frame_dt) {
    (void)frame_dt;
    return NULL;
}

MOCK_FPV_API fpv_result fpv_osd_render_rgba(
    const fpv_frame* frame,
    uint8_t*         rgba,
    uint32_t         width,
    uint32_t         height,
    uint32_t         stride_bytes,
    fpv_osd_style    style)
{
    (void)frame;
    (void)style;
    if (!rgba || width == 0u || height == 0u || stride_bytes < width * 4u) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    memset(rgba, 0x80, (size_t)stride_bytes * (size_t)height);
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_audio_configure(uint32_t sample_rate, uint32_t channels) {
    (void)sample_rate;
    (void)channels;
    return FPV_OK;
}

MOCK_FPV_API uint32_t fpv_audio_read(float* out, uint32_t frames) {
    if (out) {
        memset(out, 0, (size_t)frames * 2u * sizeof(float));
    }
    return frames;
}

MOCK_FPV_API void fpv_audio_set_volume(float volume) {
    g_audio_vol = volume;
}

MOCK_FPV_API void fpv_audio_set_beep_volume(float volume) {
    g_beep_vol = volume;
}

MOCK_FPV_API uint32_t fpv_preset_count(void) {
    return 3u;
}

MOCK_FPV_API const char* fpv_preset_name(uint32_t index) {
    return index < 3u ? k_mock_presets[index] : NULL;
}

MOCK_FPV_API fpv_result fpv_configurator_get_info(fpv_configurator_info* info) {
    if (!info) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }
    info->enabled = (g_cfg.flags & FPV_INIT_NO_CONFIGURATOR) == 0u;
    info->port = g_cfg.configurator_port ? g_cfg.configurator_port : 5761;
    info->client_count = 0;
    snprintf(info->url, sizeof(info->url), "ws://localhost:%d", info->port);
    return FPV_OK;
}

MOCK_FPV_API fpv_result fpv_drone_tick(fpv_drone* drone, float frame_dt, fpv_frame* frame) {
    if (!g_inited || !drone || drone != g_active_drone || !frame) {
        return FPV_ERROR_INVALID_ARGUMENT;
    }

    float dt = g_paused ? 0.0f : (frame_dt * g_time_scale);
    g_sim_time += (double)dt;

    /* Probe ground via host raycast callback */
    float alt = -1.0f;
    float down[3] = {0.0f, 0.0f, -1.0f};
    if (g_ray_ex_fn) {
        fpv_raycast_hit_ex hit;
        memset(&hit, 0, sizeof(hit));
        hit.struct_size = (uint32_t)sizeof(hit);
        if (g_ray_ex_fn(g_ray_user, drone->pos, down, 50.0f, &hit)) {
            alt = hit.hit.distance;
        }
    } else if (g_ray_fn) {
        fpv_raycast_hit hit;
        memset(&hit, 0, sizeof(hit));
        if (g_ray_fn(g_ray_user, drone->pos, down, 50.0f, &hit)) {
            alt = hit.distance;
        }
    }

    if (drone->armed && g_key_w_down && !g_paused) {
        drone->vel[2] = 5.0f;
        drone->pos[2] += drone->vel[2] * dt;
    } else {
        drone->vel[2] = 0.0f;
    }

    memset(frame, 0, sizeof(*frame));
    frame->struct_size = (uint32_t)sizeof(*frame);
    frame->sim_time = g_sim_time;
    frame->flags = drone->armed ? (uint32_t)FPV_DRONE_ARMED : 0u;
    if (g_paused) {
        frame->flags |= (uint32_t)FPV_DRONE_PAUSED;
    }
    if (!drone->armed) {
        snprintf(frame->arming_disable_text, sizeof(frame->arming_disable_text), "READY");
    }

    memcpy(frame->position, drone->pos, sizeof(frame->position));
    memcpy(frame->rotation, drone->rot, sizeof(frame->rotation));
    memcpy(frame->velocity, drone->vel, sizeof(frame->velocity));
    frame->speed_kmh = sqrtf(drone->vel[0] * drone->vel[0] + drone->vel[1] * drone->vel[1] + drone->vel[2] * drone->vel[2]) * 3.6f;
    frame->altitude = alt;

    /* Distinct FPV, Chase, and LOS cameras so tests can verify view switching cleanly */
    frame->camera.position[0] = drone->pos[0];
    frame->camera.position[1] = drone->pos[1];
    frame->camera.position[2] = drone->pos[2] + 0.05f;
    frame->camera.right[0] = 1.0f;
    frame->camera.forward[1] = 1.0f;
    frame->camera.up[2] = 1.0f;
    frame->camera.rotation[3] = 1.0f;
    frame->camera.fov_y_degrees = 105.0f;
    frame->camera.near_plane = 0.05f;

    frame->chase_camera = frame->camera;
    frame->chase_camera.position[1] = drone->pos[1] - 2.0f;
    frame->chase_camera.position[2] = drone->pos[2] + 0.8f;
    frame->chase_camera.fov_y_degrees = 75.0f;

    frame->los_camera = frame->camera;
    frame->los_camera.position[0] = drone->pilot_pos[0];
    frame->los_camera.position[1] = drone->pilot_pos[1];
    frame->los_camera.position[2] = drone->pilot_pos[2] + 1.6f;
    frame->los_camera.fov_y_degrees = 60.0f;

    for (int i = 0; i < 4; ++i) {
        frame->motors[i].rpm = drone->armed ? 14000.0f : 0.0f;
    }
    frame->battery.voltage = 16.4f;
    frame->battery.cell_voltage = 4.1f;
    frame->battery.cell_count = 4u;
    frame->video.signal_strength = 0.98f;
    memcpy(frame->wind, g_wind, sizeof(g_wind));
    return FPV_OK;
}
