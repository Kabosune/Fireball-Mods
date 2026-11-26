/*
 * fpv_drone_audio.c — Pure C11 Win32 `waveOut` stereo audio worker for `libfpv`.
 *
 * Pulls interleaved float32 samples from `fpv_audio_read()` into 4 x 512-frame
 * 16-bit PCM stereo buffers at 44.1 kHz, matching `libfpv/docs/INTEGRATION.md#audio`.
 * Zero third-party dependencies (standard Windows `winmm.lib` only).
 * Always cleanly stopped and joined before `fpv_drone_delete()` / `fpv_global_terminate()`.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include "fpv_drone.h"

#include <stdlib.h>
#include <string.h>

#define DRONE_AUDIO_BUFFERS 4
#define DRONE_AUDIO_FRAMES  512u

typedef struct drone_audio_state {
    volatile LONG       running;
    volatile LONG       stop_requested;
    HANDLE              thread;
    HANDLE              event;
    HWAVEOUT            hwo;
    drone_audio_read_fn read_fn;
    uint32_t            sample_rate;
    uint32_t            channels;
} drone_audio_state_t;

static drone_audio_state_t g_aud;

static int16_t drone_f32_to_i16(float x) {
    if (x > 1.0f) {
        x = 1.0f;
    } else if (x < -1.0f) {
        x = -1.0f;
    }
    return (int16_t)(x * 32767.0f);
}

static void drone_fill_pcm(int16_t* dst, uint32_t frames, uint32_t channels) {
    float fbuf[DRONE_AUDIO_FRAMES * 2u];
    memset(fbuf, 0, sizeof(fbuf));
    uint32_t got = 0u;
    if (g_aud.read_fn) {
        got = g_aud.read_fn(fbuf, frames);
    }
    if (got > frames) {
        got = frames;
    }
    uint32_t total_samples = frames * channels;
    uint32_t filled_samples = got * channels;
    for (uint32_t i = 0u; i < filled_samples; ++i) {
        dst[i] = drone_f32_to_i16(fbuf[i]);
    }
    for (uint32_t i = filled_samples; i < total_samples; ++i) {
        dst[i] = 0;
    }
}

static DWORD WINAPI drone_audio_thread(LPVOID param) {
    (void)param;
    uint32_t ch = (g_aud.channels == 1u) ? 1u : 2u;
    uint32_t buf_bytes = DRONE_AUDIO_FRAMES * ch * (uint32_t)sizeof(int16_t);

    WAVEHDR hdrs[DRONE_AUDIO_BUFFERS];
    int16_t pcm[DRONE_AUDIO_BUFFERS][DRONE_AUDIO_FRAMES * 2u];
    memset(hdrs, 0, sizeof(hdrs));
    memset(pcm, 0, sizeof(pcm));

    for (int i = 0; i < DRONE_AUDIO_BUFFERS; ++i) {
        hdrs[i].lpData = (LPSTR)pcm[i];
        hdrs[i].dwBufferLength = buf_bytes;
        if (waveOutPrepareHeader(g_aud.hwo, &hdrs[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            drone_fill_pcm(pcm[i], DRONE_AUDIO_FRAMES, ch);
            (void)waveOutWrite(g_aud.hwo, &hdrs[i], sizeof(WAVEHDR));
        }
    }

    while (!InterlockedCompareExchange(&g_aud.stop_requested, 0, 0)) {
        (void)WaitForSingleObject(g_aud.event, 50);
        if (InterlockedCompareExchange(&g_aud.stop_requested, 0, 0)) {
            break;
        }
        for (int i = 0; i < DRONE_AUDIO_BUFFERS; ++i) {
            if ((hdrs[i].dwFlags & WHDR_DONE) != 0u) {
                drone_fill_pcm(pcm[i], DRONE_AUDIO_FRAMES, ch);
                (void)waveOutWrite(g_aud.hwo, &hdrs[i], sizeof(WAVEHDR));
            }
        }
    }

    (void)waveOutReset(g_aud.hwo);
    for (int i = 0; i < DRONE_AUDIO_BUFFERS; ++i) {
        if ((hdrs[i].dwFlags & WHDR_PREPARED) != 0u) {
            (void)waveOutUnprepareHeader(g_aud.hwo, &hdrs[i], sizeof(WAVEHDR));
        }
    }
    return 0;
}

int drone_audio_start(drone_audio_read_fn read_fn, uint32_t sample_rate, uint32_t channels) {
    if (!read_fn) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_aud.running, 0, 0)) {
        return 1;
    }

    /* Respect headless test environments where audio devices may be absent or undesired */
    wchar_t headless[16];
    if (GetEnvironmentVariableW(L"FIREBALL_HEADLESS", headless, (DWORD)(sizeof(headless) / sizeof(headless[0]))) > 0u &&
        headless[0] == L'1')
    {
        return 0;
    }

    uint32_t ch = (channels == 1u) ? 1u : 2u;
    uint32_t sr = (sample_rate >= 8000u && sample_rate <= 96000u) ? sample_rate : 44100u;

    g_aud.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_aud.event) {
        return 0;
    }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)ch;
    wfx.nSamplesPerSec = sr;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mm = waveOutOpen(
        &g_aud.hwo,
        WAVE_MAPPER,
        &wfx,
        (DWORD_PTR)g_aud.event,
        0,
        CALLBACK_EVENT);
    if (mm != MMSYSERR_NOERROR) {
        CloseHandle(g_aud.event);
        g_aud.event = NULL;
        g_aud.hwo = NULL;
        return 0;
    }

    g_aud.read_fn = read_fn;
    g_aud.sample_rate = sr;
    g_aud.channels = ch;
    InterlockedExchange(&g_aud.stop_requested, 0);

    g_aud.thread = CreateThread(NULL, 0, drone_audio_thread, NULL, 0, NULL);
    if (!g_aud.thread) {
        (void)waveOutClose(g_aud.hwo);
        g_aud.hwo = NULL;
        CloseHandle(g_aud.event);
        g_aud.event = NULL;
        return 0;
    }

    InterlockedExchange(&g_aud.running, 1);
    return 1;
}

void drone_audio_stop(void) {
    if (!InterlockedCompareExchange(&g_aud.running, 0, 0)) {
        return;
    }
    InterlockedExchange(&g_aud.stop_requested, 1);
    if (g_aud.event) {
        SetEvent(g_aud.event);
    }
    if (g_aud.thread) {
        (void)WaitForSingleObject(g_aud.thread, 2000);
        CloseHandle(g_aud.thread);
        g_aud.thread = NULL;
    }
    if (g_aud.hwo) {
        (void)waveOutClose(g_aud.hwo);
        g_aud.hwo = NULL;
    }
    if (g_aud.event) {
        CloseHandle(g_aud.event);
        g_aud.event = NULL;
    }
    g_aud.read_fn = NULL;
    InterlockedExchange(&g_aud.running, 0);
}

int drone_audio_is_running(void) {
    return InterlockedCompareExchange(&g_aud.running, 0, 0) != 0;
}
