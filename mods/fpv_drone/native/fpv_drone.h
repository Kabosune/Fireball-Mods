/*
 * fpv_drone.h — Internal interface shared across `fpv_drone.fbplugin.dll` translation units.
 */

#ifndef FIREBALL_FPV_DRONE_INTERNAL_H
#define FIREBALL_FPV_DRONE_INTERNAL_H

#include "libfpv_abi.h"

#include <stddef.h>
#include <stdint.h>

#define DRONE_MOD_ID      "community.fpv_drone"
#define DRONE_PLUGIN_ID   "fpv_drone"
#define DRONE_VERSION     "0.1.0"
#define DRONE_OSD_W       360u
#define DRONE_OSD_H       288u
#define DRONE_MAX_PRESETS 32u

typedef struct drone_osd_snapshot {
    int        valid;
    int        drone_active;
    uint32_t   view;
    uint32_t   osd_style;
    uint32_t   osd_revision;
    uint32_t   flags;
    float      speed_kmh;
    float      altitude;
    float      voltage;
    float      cell_voltage;
    uint32_t   cell_count;
    float      signal_strength;
    char       arming_disable_text[96];
    char       configurator_url[64];
    int        has_rgba;
    uint8_t    rgba[DRONE_OSD_W * DRONE_OSD_H * 4u];
} drone_osd_snapshot_t;

/* Audio worker (`fpv_drone_audio.c`) — stereo 44.1 kHz Win32 waveOut pulling `fpv_audio_read` */
typedef uint32_t (*drone_audio_read_fn)(float* out, uint32_t frames);

int  drone_audio_start(drone_audio_read_fn read_fn, uint32_t sample_rate, uint32_t channels);
void drone_audio_stop(void);
int  drone_audio_is_running(void);

/* Optional Win32 GDI live Betaflight OSD preview window (`fpv_drone_osd.c`) */
int  drone_osd_window_start(void);
void drone_osd_window_stop(void);
int  drone_osd_window_is_open(void);
void drone_osd_window_set_visible(int visible);
void drone_osd_window_push_snapshot(const drone_osd_snapshot_t* snap);

#endif /* FIREBALL_FPV_DRONE_INTERNAL_H */
