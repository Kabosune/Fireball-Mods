/*
 * libfpv_abi.h — Pure C11 Runtime ABI & Dynamic Loader Header for hammerill/libfpv (ABI v1 / v1.1.3).
 *
 * Dual-licensed under MIT OR Apache-2.0 (matching Fireball-Mods).
 * Struct layouts, enum constants, and C ABI signatures match the MIT-licensed public
 * header `include/libfpv.h` from https://github.com/hammerill/libfpv (LIBFPV_ABI_VERSION = 1).
 *
 * Zero build-time dependency on libfpv: `fpv_drone.fbplugin.dll` loads `libfpv.dll`
 * (monolithic or split-process `libfpv.dll` + `libfpv-server.exe`) at runtime via
 * `LoadLibraryW` + `GetProcAddress` and validates `fpv_abi_version() == LIBFPV_ABI_VERSION`.
 */

#ifndef FIREBALL_FPV_DRONE_LIBFPV_ABI_H
#define FIREBALL_FPV_DRONE_LIBFPV_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBFPV_ABI_VERSION 1u

typedef enum fpv_result {
    FPV_OK                        =   0,
    FPV_ERROR_INVALID_ARGUMENT    =  -1,
    FPV_ERROR_NOT_INITIALIZED     =  -2,
    FPV_ERROR_ALREADY_INITIALIZED =  -3,
    FPV_ERROR_DRONE_EXISTS        =  -4,
    FPV_ERROR_NOT_FOUND           =  -5,
    FPV_ERROR_IO                  =  -6,
    FPV_ERROR_BUSY                =  -7,
    FPV_ERROR_ABI_MISMATCH        =  -8,
    FPV_ERROR_PARSE               =  -9,
    FPV_ERROR_TIMEOUT             = -10,
    FPV_ERROR_INTERNAL            = -11
} fpv_result;

typedef enum fpv_axis {
    FPV_AXIS_POS_X = 0,
    FPV_AXIS_NEG_X = 1,
    FPV_AXIS_POS_Y = 2,
    FPV_AXIS_NEG_Y = 3,
    FPV_AXIS_POS_Z = 4,
    FPV_AXIS_NEG_Z = 5
} fpv_axis;

typedef struct fpv_coord_system {
    fpv_axis right;
    fpv_axis up;
    fpv_axis forward;
    float    units_per_meter;
} fpv_coord_system;

typedef enum fpv_coords_preset_id {
    FPV_COORDS_Y_UP_RIGHT_HANDED = 0,
    FPV_COORDS_Y_UP_LEFT_HANDED  = 1,
    FPV_COORDS_Z_UP_LEFT_HANDED  = 2,
    FPV_COORDS_Z_UP_RIGHT_HANDED = 3
} fpv_coords_preset_id;

typedef enum fpv_log_level {
    FPV_LOG_DEBUG = 0,
    FPV_LOG_INFO  = 1,
    FPV_LOG_WARN  = 2,
    FPV_LOG_ERROR = 3
} fpv_log_level;

typedef void (*fpv_log_fn)(void* user, fpv_log_level level, const char* message);

typedef enum fpv_timing_mode {
    FPV_TIMING_THREAD = 0,
    FPV_TIMING_MANUAL = 1
} fpv_timing_mode;

enum fpv_init_flags {
    FPV_INIT_NO_DEVICE_INPUT    = 1u << 0,
    FPV_INIT_NO_CONFIGURATOR    = 1u << 1,
    FPV_INIT_NO_AUTO_PAUSE      = 1u << 2,
    FPV_INIT_NO_LENS_DISTORTION = 1u << 3,
    FPV_INIT_VIDEO_FEED         = 1u << 4
};

typedef struct fpv_global_config {
    uint32_t         struct_size;
    const char*      data_dir;
    fpv_coord_system coords;
    fpv_log_fn       log;
    void*            log_user;
    fpv_log_level    log_level;
    int              configurator_port;
    fpv_timing_mode  timing;
    float            sim_rate_hz;
    float            auto_pause_seconds;
    uint32_t         flags;
    const char*      host_name;
    const char*      host_build;
    const char*      host_url;
} fpv_global_config;

enum fpv_material_flags {
    FPV_MATERIAL_KILLS_PROPS          = 1u << 0,
    FPV_MATERIAL_NO_COLLISION         = 1u << 1,
    FPV_MATERIAL_TRANSPARENT_TO_VIDEO = 1u << 2
};

typedef struct fpv_material {
    float    restitution;
    float    friction;
    uint32_t flags;
} fpv_material;

typedef struct fpv_mesh {
    const float*    positions;
    uint32_t        vertex_count;
    uint32_t        vertex_stride;
    const uint32_t* indices;
    uint32_t        triangle_count;
    const uint8_t*  materials;
    uint8_t         material;
} fpv_mesh;

typedef struct fpv_transform {
    float position[3];
    float rotation[4];
    float scale[3];
} fpv_transform;

typedef struct fpv_raycast_hit {
    float    distance;
    float    normal[3];
    uint8_t  material;
    uint32_t object_id;
} fpv_raycast_hit;

typedef int (*fpv_raycast_fn)(
    void*            user,
    const float      origin[3],
    const float      direction[3],
    float            max_distance,
    fpv_raycast_hit* out_hit);

#define FPV_RAYCAST_HIT_POSE 1u

typedef struct fpv_raycast_hit_ex {
    uint32_t        struct_size;
    fpv_raycast_hit hit;
    uint32_t        flags;
    float           object_position[3];
    float           object_rotation[4];
} fpv_raycast_hit_ex;

typedef int (*fpv_raycast_ex_fn)(
    void*               user,
    const float         origin[3],
    const float         direction[3],
    float               max_distance,
    fpv_raycast_hit_ex* out_hit);

typedef struct fpv_drone fpv_drone;

typedef struct fpv_drone_desc {
    uint32_t    struct_size;
    const char* preset;
    float       position[3];
    float       rotation[4];
    float       pilot_position[3];
} fpv_drone_desc;

typedef struct fpv_camera {
    float position[3];
    float rotation[4];
    float forward[3];
    float up[3];
    float right[3];
    float fov_y_degrees;
    float aspect_hint;
    float near_plane;
    float distortion_k1;
    float distortion_k2;
    float distortion_zoom;
} fpv_camera;

enum fpv_motor_flags {
    FPV_MOTOR_BURNED_OUT  = 1u << 0,
    FPV_MOTOR_DAMAGED     = 1u << 1,
    FPV_MOTOR_PROP_BROKEN = 1u << 2
};

typedef struct fpv_motor_state {
    float    rpm;
    float    thrust_newton;
    float    current_amp;
    float    temperature_c;
    float    command;
    float    prop_damage;
    float    position[3];
    float    spin_direction;
    uint32_t flags;
} fpv_motor_state;

typedef struct fpv_battery_state {
    float    voltage;
    float    cell_voltage;
    float    current_amp;
    float    mah_drawn;
    float    capacity_mah;
    uint32_t cell_count;
} fpv_battery_state;

enum fpv_drone_flags {
    FPV_DRONE_ARMED            = 1u << 0,
    FPV_DRONE_FAILSAFE         = 1u << 1,
    FPV_DRONE_CRASHED          = 1u << 2,
    FPV_DRONE_BEEPER           = 1u << 3,
    FPV_DRONE_PAUSED           = 1u << 4,
    FPV_DRONE_ON_GROUND        = 1u << 5,
    FPV_DRONE_MODE_ANGLE       = 1u << 6,
    FPV_DRONE_MODE_HORIZON     = 1u << 7,
    FPV_DRONE_TURTLE           = 1u << 8,
    FPV_DRONE_OSD_DISABLED     = 1u << 9,
    FPV_DRONE_RADIO_ACTIVE     = 1u << 10,
    FPV_DRONE_HEARD_FROM_PILOT = 1u << 12
};

typedef struct fpv_impact {
    float    position[3];
    float    normal[3];
    float    speed;
    float    impulse;
    uint8_t  material;
    uint32_t object_id;
    int32_t  motor_index;
} fpv_impact;

#define FPV_IMPACT_CARGO     (-2)
#define FPV_MAX_IMPACTS      16
#define FPV_OSD_ROWS         16
#define FPV_OSD_COLS         30
#define FPV_CHANNEL_COUNT    10
#define FPV_VIDEO_MAX_LINES  320

typedef struct fpv_video_state {
    float    noise;
    float    breakup;
    float    signal_strength;
    uint32_t is_digital;
} fpv_video_state;

enum fpv_video_feed_flags {
    FPV_VIDEO_FEED_NOISE         = 1u << 0,
    FPV_VIDEO_FEED_RESOLUTION    = 1u << 1,
    FPV_VIDEO_FEED_OSD_IN_SIGNAL = 1u << 2
};

typedef enum fpv_video_system {
    FPV_VIDEO_NTSC    = 0,
    FPV_VIDEO_PAL     = 1,
    FPV_VIDEO_DIGITAL = 2
} fpv_video_system;

typedef struct fpv_video_feed {
    uint32_t flags;
    uint32_t system;
    float    picture_aspect;
    uint32_t raster_width;
    uint32_t raster_height;
    float    luma_cycles;
    float    chroma_cycles;
    float    subcarrier_cycles;
    float    sharpening;
    uint32_t field;
    uint32_t field_parity;
    float    roll;
    float    seed;
    float    interference[4];
    float    block_errors;
    float    softness;
    uint32_t frozen;
    float    rssi_dbm;
    float    cnr_db;
    uint32_t osd_style;
    uint32_t line_count;
    uint8_t  lines[FPV_VIDEO_MAX_LINES * 8];
    float    chroma_boost;
    float    chroma_tint[2];
    float    hue_sweep;
    float    display_aspect;
    uint32_t osd_rows;
} fpv_video_feed;

typedef struct fpv_frame {
    uint32_t          struct_size;
    double            sim_time;
    uint32_t          flags;
    uint32_t          arming_disable_flags;
    char              arming_disable_text[96];

    float             position[3];
    float             rotation[4];
    float             velocity[3];
    float             angular_velocity[3];
    float             speed_kmh;
    float             altitude;

    fpv_camera        camera;
    fpv_camera        chase_camera;
    fpv_camera        los_camera;

    fpv_motor_state   motors[4];
    fpv_battery_state battery;
    fpv_video_state   video;

    uint32_t          impact_count;
    fpv_impact        impacts[FPV_MAX_IMPACTS];

    uint8_t           osd[FPV_OSD_ROWS * FPV_OSD_COLS];
    uint32_t          osd_revision;

    float             channels[FPV_CHANNEL_COUNT];
    float             wind[3];

    uint32_t          portal_passes;
    int32_t           last_portal;

    uint32_t          cargo_flags;
    float             cargo_position[3];
    float             cargo_velocity[3];

    fpv_video_feed    video_feed;
} fpv_frame;

typedef enum fpv_view {
    FPV_VIEW_FPV   = 0,
    FPV_VIEW_CHASE = 1,
    FPV_VIEW_LOS   = 2,
    FPV_VIEW_PILOT = 3
} fpv_view;

enum fpv_sim_options {
    FPV_SIM_NO_BATTERY_DRAIN = 1u << 0,
    FPV_SIM_NO_DAMAGE        = 1u << 1
};

enum fpv_pilot_features {
    FPV_PILOT_STAND_IN = 1u << 0,
    FPV_PILOT_POSE     = 1u << 1
};

typedef struct fpv_pilot_config {
    uint32_t struct_size;
    uint32_t features;
    float    stand_in_distance;
    float    stand_in_return_distance;
} fpv_pilot_config;

enum fpv_wind_flags {
    FPV_WIND_NO_SHELTER = 1u << 0,
    FPV_WIND_ENCLOSED   = 1u << 1
};

typedef struct fpv_wind {
    uint32_t struct_size;
    float    velocity[3];
    float    reference_height;
    float    turbulence;
    uint32_t flags;
} fpv_wind;

typedef enum fpv_input_source {
    FPV_INPUT_SOURCE_NONE     = 0,
    FPV_INPUT_SOURCE_RADIO    = 1,
    FPV_INPUT_SOURCE_GAMEPAD  = 2,
    FPV_INPUT_SOURCE_KEYBOARD = 3,
    FPV_INPUT_SOURCE_HOST     = 4
} fpv_input_source;

typedef struct fpv_input_status {
    uint32_t         struct_size;
    fpv_input_source source;
    char             device_name[128];
    int              needs_setup;
    int              device_count;
} fpv_input_status;

typedef enum fpv_key {
    FPV_KEY_NONE      = 0,
    FPV_KEY_TAB       = 9,
    FPV_KEY_ENTER     = 13,
    FPV_KEY_ESCAPE    = 27,
    FPV_KEY_SPACE     = 32,
    FPV_KEY_LEFT      = 256,
    FPV_KEY_RIGHT,
    FPV_KEY_UP,
    FPV_KEY_DOWN,
    FPV_KEY_PAGE_UP,
    FPV_KEY_PAGE_DOWN,
    FPV_KEY_HOME,
    FPV_KEY_END,
    FPV_KEY_INSERT,
    FPV_KEY_DELETE,
    FPV_KEY_BACKSPACE,
    FPV_KEY_LSHIFT,
    FPV_KEY_RSHIFT,
    FPV_KEY_LCTRL,
    FPV_KEY_RCTRL,
    FPV_KEY_LALT,
    FPV_KEY_RALT,
    FPV_KEY_F1,
    FPV_KEY_F2,
    FPV_KEY_F3,
    FPV_KEY_F4,
    FPV_KEY_F5,
    FPV_KEY_F6,
    FPV_KEY_F7,
    FPV_KEY_F8,
    FPV_KEY_F9,
    FPV_KEY_F10,
    FPV_KEY_F11,
    FPV_KEY_F12
} fpv_key;

typedef enum fpv_control_kind {
    FPV_CONTROL_INSTANT = 0,
    FPV_CONTROL_SWITCH  = 1,
    FPV_CONTROL_HOLD    = 2
} fpv_control_kind;

enum fpv_gamepad_button {
    FPV_GAMEPAD_NONE = -1
};

typedef struct fpv_control_desc {
    uint32_t    struct_size;
    uint32_t    id;
    const char* name;
    const char* label;
    uint32_t    kind;
    int32_t     key;
    int32_t     gamepad_button;
} fpv_control_desc;

typedef struct fpv_draw_vertex {
    float    pos[2];
    float    uv[2];
    uint32_t color;
} fpv_draw_vertex;

typedef struct fpv_draw_cmd {
    float    clip_rect[4];
    uint64_t texture_id;
    uint32_t index_offset;
    uint32_t index_count;
    uint32_t vertex_offset;
} fpv_draw_cmd;

typedef struct fpv_draw_list {
    const fpv_draw_vertex* vertices;
    uint32_t               vertex_count;
    const uint32_t*        indices;
    uint32_t               index_count;
    const fpv_draw_cmd*    commands;
    uint32_t               command_count;
} fpv_draw_list;

typedef struct fpv_draw_data {
    float                display_size[2];
    uint32_t             list_count;
    const fpv_draw_list* lists;
    uint32_t             total_vertex_count;
    uint32_t             total_index_count;
} fpv_draw_data;

#define FPV_TEXTURE_OVERLAY_FONT 1u
#define FPV_TEXTURE_OSD_FONT     2u

typedef struct fpv_texture {
    uint32_t       width;
    uint32_t       height;
    const uint8_t* rgba;
    uint32_t       revision;
} fpv_texture;

typedef enum fpv_osd_style {
    FPV_OSD_STYLE_ANALOG = 0,
    FPV_OSD_STYLE_HD     = 1
} fpv_osd_style;

typedef struct fpv_configurator_info {
    uint32_t struct_size;
    int      enabled;
    int      port;
    int      client_count;
    char     url[64];
} fpv_configurator_info;

/* Dynamic function pointer table resolved from `libfpv.dll` via GetProcAddress */
typedef struct libfpv_api {
    /* Required core symbols */
    uint32_t    (*abi_version)(void);
    const char* (*version_string)(void);
    const char* (*result_string)(fpv_result result);
    const char* (*last_error)(void);
    void        (*free_mem)(void* ptr);

    void        (*global_config_defaults)(fpv_global_config* config);
    fpv_result  (*shared_data_dir)(char* out, uint32_t out_size);
    fpv_result  (*global_init)(const fpv_global_config* config);
    void        (*global_terminate)(void);
    int         (*is_initialized)(void);

    void        (*drone_desc_defaults)(fpv_drone_desc* desc);
    fpv_result  (*drone_create)(const fpv_drone_desc* desc, fpv_drone** out_drone);
    fpv_result  (*drone_delete)(fpv_drone* drone);
    fpv_result  (*drone_reset)(fpv_drone* drone, const float position[3], const float rotation[4]);
    fpv_result  (*drone_repair)(fpv_drone* drone);
    fpv_result  (*drone_tick)(fpv_drone* drone, float frame_dt, fpv_frame* frame);

    /* Optional / extended symbols (gracefully checked for NULL before calling) */
    fpv_result  (*material_set)(uint8_t material_id, const fpv_material* material);
    void        (*world_set_raycast_callback)(fpv_raycast_fn callback, void* user);
    void        (*world_set_raycast_callback_ex)(fpv_raycast_ex_fn callback, void* user);

    fpv_result  (*drone_set_pilot_position)(fpv_drone* drone, const float position[3]);
    fpv_result  (*drone_set_preset)(fpv_drone* drone, const char* preset);
    fpv_result  (*gps_set_home)(const float position[3]);
    void        (*set_view)(fpv_view view);

    void        (*sim_set_options)(uint32_t options);
    uint32_t    (*sim_get_options)(void);

    fpv_result  (*pilot_set_support)(uint32_t features, float stand_in_distance, float stand_in_return_distance);
    fpv_result  (*pilot_get_config)(fpv_pilot_config* config);

    void        (*wind_defaults)(fpv_wind* wind);
    fpv_result  (*wind_set)(const fpv_wind* wind);

    void        (*set_paused)(int paused);
    int         (*is_paused)(void);
    void        (*set_time_scale)(float scale);
    fpv_result  (*step)(float seconds);

    fpv_result  (*input_get_status)(fpv_input_status* status);
    void        (*input_set_channels)(const float* channels, uint32_t count);
    void        (*input_clear_channels)(void);
    void        (*input_key)(int key, int down);

    fpv_result  (*controls_set)(const fpv_control_desc* controls, uint32_t count);
    int         (*control_poll)(uint32_t id, uint32_t* presses);

    void        (*overlay_set_open)(int open);
    int         (*overlay_is_open)(void);
    int         (*overlay_wants_input)(void);
    void        (*overlay_set_toggle_key)(int key);
    void        (*overlay_set_display)(float width, float height, float ui_scale);
    void        (*overlay_set_action)(uint32_t id, const char* label);
    uint32_t    (*overlay_poll_action)(void);
    const fpv_draw_data* (*overlay_render)(float frame_dt);

    const fpv_draw_data* (*osd_draw_data)(const fpv_frame* frame, float width, float height, fpv_osd_style style);
    fpv_result  (*osd_render_rgba)(const fpv_frame* frame, uint8_t* rgba, uint32_t width, uint32_t height,
                                   uint32_t stride_bytes, fpv_osd_style style);

    fpv_result  (*audio_configure)(uint32_t sample_rate, uint32_t channels);
    uint32_t    (*audio_read)(float* out, uint32_t frames);
    void        (*audio_set_volume)(float volume);
    void        (*audio_set_beep_volume)(float volume);
    void        (*audio_set_listener)(const float position[3], const float rotation[4]);

    uint32_t    (*preset_count)(void);
    const char* (*preset_name)(uint32_t index);
    fpv_result  (*configurator_get_info)(fpv_configurator_info* info);
} libfpv_api_t;

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_FPV_DRONE_LIBFPV_ABI_H */
