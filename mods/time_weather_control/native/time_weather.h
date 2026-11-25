/*
 * time_weather.h — shared state between the frame/input side (time_weather.c)
 * and the optional legacy Win32 control window (time_weather_ui.c).
 *
 * Threading: the window thread and the input/menu callbacks only queue commands
 * (tw_cmd_*); every title write happens in FB_EVENT_FRAME_BEGIN.
 */

#ifndef TIME_WEATHER_H
#define TIME_WEATHER_H

#include <stdint.h>

#define TW_MAX_WEATHERS 256
#define TW_NAME_MAX     48

typedef struct tw_weather_entry {
    uint32_t index;
    uint32_t joaat_lower;
    uint32_t joaat;
    char     name[TW_NAME_MAX];
} tw_weather_entry_t;

/* Status snapshot published by the frame thread for the menu & optional window. */
typedef struct tw_status {
    int      clock_ok;
    double   tod_sec;        /* current game time of day, seconds */
    int      paused;
    double   scale;          /* 1.0 = the game's own rate */
    int      weather_ok;
    int32_t  weather_cur;    /* list index of the next/current weather slot, -1 unknown */
    int32_t  weather_target; /* list index the plugin holds, -1 none */
    int      weather_locked;
    double   blend_progress; /* 0..1 while blending, else 1 */
    uint64_t writes;
    uint64_t reasserts;
} tw_status_t;

/* Commands (any thread). */
void tw_cmd_set_tod(double tod_sec);
void tw_cmd_add_hours(int hours);
void tw_cmd_toggle_pause(void);
void tw_cmd_set_scale(double scale);
void tw_cmd_set_weather(int32_t list_index, int blend);
void tw_cmd_set_lock(int locked);
void tw_cmd_toggle_lock(void);

/* Queries (any thread). */
void     tw_get_status(tw_status_t* out);
uint32_t tw_weather_count(void);
const tw_weather_entry_t* tw_weather_at(uint32_t i);
double   tw_ms_per_game_minute(void);

/* Optional legacy F8 window (time_weather_ui.c). */
void tw_ui_toggle(void);
void tw_ui_shutdown(void);

#endif /* TIME_WEATHER_H */
