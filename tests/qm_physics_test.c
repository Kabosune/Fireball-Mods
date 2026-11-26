/*
 * qm_physics_test.c — Standalone pure-C11 unit tests for `qm_physics.h/.c`.
 *
 * Verifies every preset's physics invariants in RAGE 9 Z-up meters:
 *   - Ground friction deceleration to rest
 *   - Quake VQ3 air-strafe speed gain above max_speed
 *   - Half-Life low-cap / high-accel air-strafe speed gain & crouch-jump lift
 *   - Quake CPMA tight air-control turning (pure W and pure A/D) vs VQ3
 *   - Auto-bhop speed retention, coyote time, and jump input buffering
 *   - Apex Legends crouch-slide boost, cooldown, downhill slope acceleration,
 *     slide-jump burst, and post-jump tap-strafe air-lurch redirection
 *   - Deadlock directional ground/air dash, stamina pips, recharge timer,
 *     timed dash-jump momentum burst, and downhill crouch-slide
 *   - Optional wall-bounce surface normal probe hook
 *   - Preset name parsing & NaN/invalid-dt safety
 */

#include "qm_physics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define QM_PI 3.14159265358979323846f

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            fprintf(stderr, "  [FAIL] line %d: %s\n", __LINE__, #cond);        \
        }                                                                      \
    } while (0)

static qm_input_t make_flat_input(float dt) {
    qm_input_t in;
    memset(&in, 0, sizeof(in));
    in.dt_sec = dt;
    in.on_ground = 1;
    in.slope_normal[2] = 1.0f;
    return in;
}

static void test_ground_friction_stop(void) {
    printf("[1] Ground friction stop & acceleration...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_QUAKE_VQ3, &p);
    qm_state_init(&st, &p);

    /* Accelerate forward on flat ground */
    qm_input_t in = make_flat_input(1.0f / 60.0f);
    in.wish_forward = 1.0f;
    for (int i = 0; i < 90; ++i) {
        qm_step(&p, &in, &st, &st);
    }
    CHECK(fabsf(st.vel[1] - p.max_speed) < 0.25f);
    CHECK(fabsf(st.vel[0]) < 1e-4f);
    CHECK(st.mode == QM_MODE_GROUND);

    /* Release input -> friction brings player cleanly to complete stop */
    in.wish_forward = 0.0f;
    float prev_spd = st.horiz_speed;
    for (int i = 0; i < 120; ++i) {
        qm_step(&p, &in, &st, &st);
        CHECK(st.horiz_speed <= prev_spd + 1e-5f);
        prev_spd = st.horiz_speed;
    }
    CHECK(st.horiz_speed < 1e-4f);
}

static void test_vq3_air_strafe_speed_gain(void) {
    printf("[2] Quake VQ3 air-strafe speed gain...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_QUAKE_VQ3, &p);
    qm_state_init(&st, &p);

    /* Launch airborne at max_speed along +Y (yaw = 0) */
    st.vel[0] = 0.0f;
    st.vel[1] = p.max_speed;
    st.vel[2] = p.jump_speed;

    /* Hold W+D while gradually turning camera right (-yaw) at optimal strafe angle */
    const float dt = 1.0f / 125.0f;
    float yaw = 0.0f;
    for (int i = 0; i < 80; ++i) {
        qm_input_t in = make_flat_input(dt);
        in.on_ground = 0;
        in.wish_forward = 1.0f;
        in.wish_right = 1.0f;
        /* Track current velocity heading so wishdir stays near optimal ~45 deg right */
        float vel_yaw = atan2f(-st.vel[0], st.vel[1]);
        yaw = vel_yaw - 0.12f;
        in.cam_yaw_rad = yaw;
        qm_step(&p, &in, &st, &st);
    }
    printf("    VQ3 air-strafe speed: %.2f -> %.2f m/s\n", (double)p.max_speed, (double)st.horiz_speed);
    CHECK(st.horiz_speed > p.max_speed + 1.5f);
    CHECK(st.mode == QM_MODE_AIR);
}

static void test_half_life_air_strafe_and_crouch_jump(void) {
    printf("[3] Half-Life low-cap air-strafe speed gain & crouch-jump...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_HALF_LIFE, &p);
    qm_state_init(&st, &p);
    CHECK(p.air_wish_cap < 1.2f && p.air_accel >= 100.0f);

    /* Crouch-jump lifts higher than standard jump */
    qm_state_t s_norm = st, s_crouch = st;
    qm_input_t j_norm = make_flat_input(1.0f / 60.0f);
    j_norm.jump_pressed = 1;
    j_norm.jump_held = 1;
    qm_input_t j_crouch = j_norm;
    j_crouch.crouch_held = 1;
    qm_step(&p, &j_norm, &s_norm, &s_norm);
    qm_step(&p, &j_crouch, &s_crouch, &s_crouch);
    CHECK(s_crouch.vel[2] > s_norm.vel[2] + 0.4f);

    /* Classic GoldSrc/Source pure-D right air-strafe with smooth mouse turn */
    st.vel[0] = 0.0f;
    st.vel[1] = p.max_speed;
    st.vel[2] = p.jump_speed;
    const float dt = 1.0f / 100.0f;
    for (int i = 0; i < 70; ++i) {
        qm_input_t in = make_flat_input(dt);
        in.on_ground = 0;
        in.wish_forward = 0.0f;
        in.wish_right = 1.0f;
        /* Keep pure-D wishdir inside optimal GoldSrc/Source air_wish_cap cone (dot < 0.95) */
        float vel_yaw = atan2f(-st.vel[0], st.vel[1]);
        in.cam_yaw_rad = vel_yaw + 0.05f;
        qm_step(&p, &in, &st, &st);
    }
    printf("    Half-Life air-strafe speed: %.2f -> %.2f m/s\n", (double)p.max_speed, (double)st.horiz_speed);
    CHECK(st.horiz_speed > p.max_speed + 1.5f);
}

static void test_cpma_air_turning(void) {
    printf("[4] Quake CPMA tight air-control turning vs VQ3...\n");
    qm_params_t p_vq3, p_cpma;
    qm_state_t s_vq3, s_cpma;
    qm_preset_defaults(QM_PRESET_QUAKE_VQ3, &p_vq3);
    qm_preset_defaults(QM_PRESET_QUAKE_CPMA, &p_cpma);
    qm_state_init(&s_vq3, &p_vq3);
    qm_state_init(&s_cpma, &p_cpma);

    /* Start both moving North (+Y) at 10 m/s in midair, then turn camera 45 deg right and hold W */
    s_vq3.vel[1] = 10.0f;
    s_vq3.vel[2] = 6.0f;
    s_cpma.vel[1] = 10.0f;
    s_cpma.vel[2] = 6.0f;

    const float dt = 1.0f / 100.0f;
    for (int i = 0; i < 40; ++i) {
        qm_input_t in = make_flat_input(dt);
        in.on_ground = 0;
        in.wish_forward = 1.0f;
        in.cam_yaw_rad = -0.785398f; /* 45 deg right (+X, +Y) */
        qm_step(&p_vq3, &in, &s_vq3, &s_vq3);
        qm_step(&p_cpma, &in, &s_cpma, &s_cpma);
    }

    float vq3_angle = atan2f(s_vq3.vel[0], s_vq3.vel[1]);
    float cpma_angle = atan2f(s_cpma.vel[0], s_cpma.vel[1]);
    printf("    Turn angle: VQ3=%.2f rad, CPMA=%.2f rad (CPMA speed=%.2f m/s)\n",
           (double)vq3_angle, (double)cpma_angle, (double)s_cpma.horiz_speed);
    CHECK(cpma_angle > vq3_angle + 0.25f);
    CHECK(s_cpma.horiz_speed >= 9.9f);

    /* Also verify pure A/D strafe turning in CPMA */
    qm_state_init(&s_cpma, &p_cpma);
    s_cpma.vel[1] = 10.0f;
    s_cpma.vel[2] = 6.0f;
    for (int i = 0; i < 40; ++i) {
        qm_input_t in = make_flat_input(dt);
        in.on_ground = 0;
        in.wish_right = 1.0f;
        float vel_yaw = atan2f(-s_cpma.vel[0], s_cpma.vel[1]);
        in.cam_yaw_rad = vel_yaw + 0.05f;
        qm_step(&p_cpma, &in, &s_cpma, &s_cpma);
    }
    CHECK(s_cpma.vel[0] > 2.5f);
    CHECK(s_cpma.horiz_speed > 10.5f);
}

static void test_auto_bhop_coyote_and_buffer(void) {
    printf("[5] Auto-bhop speed retention, coyote time & jump buffer...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_QUAKE_VQ3, &p);
    qm_state_init(&st, &p);

    /* Auto-bhop ON: land at 14.0 m/s with jump_held=1 -> re-jumps with zero friction loss */
    p.auto_bhop = 1;
    st.vel[1] = 14.0f;
    st.vel[2] = -2.0f;
    qm_input_t in = make_flat_input(1.0f / 60.0f);
    in.on_ground = 1;
    in.jump_held = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 1);
    CHECK(st.horiz_speed >= 13.99f);
    CHECK(st.vel[2] > 5.0f);

    /* Auto-bhop OFF: land at 14.0 m/s with jump_held=1 (not freshly pressed) -> friction slows player */
    p.auto_bhop = 0;
    qm_state_init(&st, &p);
    st.vel[1] = 14.0f;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 0);
    CHECK(st.horiz_speed < 13.5f);

    /* Coyote time: step off ground for 0.04s (< 0.08s), then press jump -> launches */
    qm_state_init(&st, &p);
    in = make_flat_input(1.0f / 60.0f);
    in.on_ground = 1;
    qm_step(&p, &in, &st, &st); /* primes coyote_timer */
    in.on_ground = 0;
    qm_step(&p, &in, &st, &st);
    qm_step(&p, &in, &st, &st);
    in.jump_pressed = 1;
    in.jump_held = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 1);
    CHECK(st.vel[2] > 5.0f);

    /* Jump input buffer: press jump 3 frames before touching ground -> fires on landing */
    qm_state_init(&st, &p);
    in = make_flat_input(1.0f / 60.0f);
    in.on_ground = 0;
    in.jump_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 0);
    in.jump_pressed = 0;
    qm_step(&p, &in, &st, &st);
    in.on_ground = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 1);
}

static void test_apex_slide_slope_and_tap_strafe(void) {
    printf("[6] Apex Legends slide boost, cooldown, downhill slope & tap-strafe...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_APEX_LEGENDS, &p);
    qm_state_init(&st, &p);

    /* Flat-ground sprint-to-crouch slide boost */
    st.vel[1] = 7.2f;
    qm_input_t in = make_flat_input(1.0f / 60.0f);
    in.crouch_held = 1;
    in.crouch_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.slide_started == 1);
    CHECK(st.sliding == 1);
    CHECK(st.mode == QM_MODE_SLIDE);
    CHECK(st.horiz_speed > 10.0f); /* 7.2 + 3.8 minus 1 tick slide_friction */

    /* Immediate un-crouch & re-crouch inside slide_cooldown must NOT re-boost */
    in.crouch_held = 0;
    in.crouch_pressed = 0;
    qm_step(&p, &in, &st, &st);
    CHECK(st.sliding == 0);
    st.vel[1] = 7.0f;
    in.crouch_held = 1;
    in.crouch_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.sliding == 1);
    CHECK(st.horiz_speed <= 7.05f); /* Cooldown prevented boost spam */

    /* Downhill slope surfing: slope tilts downhill toward +Y (25 deg) */
    const float slope_rad = 25.0f * (QM_PI / 180.0f);
    in.crouch_held = 1;
    in.crouch_pressed = 0;
    in.slope_normal[0] = 0.0f;
    in.slope_normal[1] = sinf(slope_rad);
    in.slope_normal[2] = cosf(slope_rad);
    float speed_before_slope = st.horiz_speed;
    for (int i = 0; i < 60; ++i) {
        qm_step(&p, &in, &st, &st);
    }
    printf("    Apex downhill slide speed: %.2f -> %.2f m/s\n", (double)speed_before_slope, (double)st.horiz_speed);
    CHECK(st.horiz_speed > speed_before_slope + 2.5f);

    /* Slide-jump burst */
    float speed_before_sj = st.horiz_speed;
    in.jump_pressed = 1;
    in.jump_held = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.slide_jumped == 1);
    CHECK(st.jumped == 1);
    CHECK(st.horiz_speed > speed_before_sj + 1.0f);

    /* Tap-strafe air-lurch: immediately after jumping North (+Y) at 10 m/s, turn 90 deg right (+X) and tap W */
    qm_state_init(&st, &p);
    st.vel[1] = 10.0f;
    in = make_flat_input(1.0f / 60.0f);
    in.jump_pressed = 1;
    in.jump_held = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.jumped == 1);
    CHECK(st.tap_strafe_timer > 0.3f);

    int saw_tap_strafe = 0;
    for (int i = 0; i < 15; ++i) {
        in = make_flat_input(1.0f / 60.0f);
        in.on_ground = 0;
        in.wish_forward = 1.0f;
        in.cam_yaw_rad = -QM_PI * 0.5f; /* Facing +X East */
        qm_step(&p, &in, &st, &st);
        if (st.tap_strafed) {
            saw_tap_strafe = 1;
        }
    }
    printf("    Apex tap-strafe redirected vel=(%.2f, %.2f) speed=%.2f m/s\n",
           (double)st.vel[0], (double)st.vel[1], (double)st.horiz_speed);
    CHECK(saw_tap_strafe == 1);
    CHECK(st.vel[0] > 7.5f);
    CHECK(st.horiz_speed >= 9.5f);
}

static void test_deadlock_dash_stamina_and_dash_jump(void) {
    printf("[7] Deadlock directional dash, stamina recharge, dash-jump & downhill slide...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_DEADLOCK, &p);
    qm_state_init(&st, &p);
    CHECK(fabsf(st.stamina - 3.0f) < 1e-4f);

    /* Directional ground dash to the right (+X) */
    qm_input_t in = make_flat_input(1.0f / 60.0f);
    in.wish_right = 1.0f;
    in.dash_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.dashed == 1);
    CHECK(st.mode == QM_MODE_DASH);
    CHECK(st.vel[0] > 12.5f);
    CHECK(st.stamina < 2.05f && st.stamina > 1.95f);

    /* Timed dash-jump burst after ~0.15s */
    in.dash_pressed = 0;
    for (int i = 0; i < 8; ++i) {
        qm_step(&p, &in, &st, &st);
    }
    float spd_before_dj = st.horiz_speed;
    in.jump_pressed = 1;
    in.jump_held = 1;
    qm_step(&p, &in, &st, &st);
    printf("    Deadlock dash-jump: %.2f -> %.2f m/s (vz=%.2f)\n",
           (double)spd_before_dj, (double)st.horiz_speed, (double)st.vel[2]);
    CHECK(st.dash_jumped == 1);
    CHECK(st.jumped == 1);
    CHECK(st.horiz_speed > spd_before_dj + 3.5f);
    CHECK(st.vel[2] > p.jump_speed + 0.5f);

    /* Air-dash cancels downward fall & spends another pip */
    st.vel[0] = 0.0f;
    st.vel[1] = 2.0f;
    st.vel[2] = -6.0f;
    st.dash_timer = 0.0f;
    in = make_flat_input(1.0f / 60.0f);
    in.on_ground = 0;
    in.wish_forward = 1.0f;
    in.dash_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.dashed == 1);
    CHECK(st.vel[1] > 13.0f);
    CHECK(st.vel[2] > -0.5f); /* Fall velocity zeroed before reduced-gravity tick */

    /* Spend remaining stamina, confirm 0 stamina blocks dash, then recharge 1 full pip */
    st.stamina = 0.2f;
    st.dash_timer = 0.0f;
    qm_step(&p, &in, &st, &st);
    CHECK(st.dashed == 0);

    const float dt = 0.05f;
    in.dt_sec = dt;
    in.dash_pressed = 0;
    in.on_ground = 1;
    in.wish_forward = 0.0f;
    int ticks = (int)(p.dash_recharge_sec / dt) + 2;
    for (int i = 0; i < ticks; ++i) {
        qm_step(&p, &in, &st, &st);
    }
    CHECK(st.stamina >= 1.15f);
    in.dash_pressed = 1;
    qm_step(&p, &in, &st, &st);
    CHECK(st.dashed == 1);
}

static int mock_east_wall_probe(
    const float             pos[3],
    const float             probe_dir[3],
    float                   max_dist_m,
    qm_wall_probe_result_t* out_hit,
    void*                   user_ctx)
{
    (void)pos;
    (void)max_dist_m;
    (void)user_ctx;
    if (probe_dir[0] > 0.5f) {
        out_hit->hit = 1;
        out_hit->normal[0] = -1.0f; /* Wall faces West (-X) */
        out_hit->normal[1] = 0.0f;
        out_hit->normal[2] = 0.0f;
        out_hit->distance_m = 0.45f;
        return 1;
    }
    return 0;
}

static void test_wall_bounce_hook_and_parsing(void) {
    printf("[8] Wall-bounce surface probe hook & preset name parsing...\n");
    qm_params_t p;
    qm_state_t st;
    qm_preset_defaults(QM_PRESET_APEX_LEGENDS, &p);
    qm_state_init(&st, &p);

    st.vel[0] = 6.0f;
    st.vel[1] = 2.0f;
    st.vel[2] = -1.0f;
    qm_input_t in = make_flat_input(1.0f / 60.0f);
    in.on_ground = 0;
    in.jump_pressed = 1;
    in.wall_probe = mock_east_wall_probe;
    qm_step(&p, &in, &st, &st);
    CHECK(st.wall_bounced == 1);
    CHECK(st.vel[0] < -5.5f);
    CHECK(st.vel[2] > 5.5f);

    /* Preset parsing */
    qm_preset_id_t pid = QM_PRESET_CUSTOM;
    CHECK(qm_preset_from_name("vq3", &pid) && pid == QM_PRESET_QUAKE_VQ3);
    CHECK(qm_preset_from_name("CPMA", &pid) && pid == QM_PRESET_QUAKE_CPMA);
    CHECK(qm_preset_from_name("halflife", &pid) && pid == QM_PRESET_HALF_LIFE);
    CHECK(qm_preset_from_name("apex", &pid) && pid == QM_PRESET_APEX_LEGENDS);
    CHECK(qm_preset_from_name("deadlock", &pid) && pid == QM_PRESET_DEADLOCK);
    CHECK(qm_preset_from_name("custom", &pid) && pid == QM_PRESET_CUSTOM);
    CHECK(!qm_preset_from_name("nonexistent", &pid));

    /* Invalid / zero dt safety */
    in.dt_sec = 0.0f;
    qm_step(&p, &in, &st, &st);
    CHECK(isfinite(st.vel[0]) && isfinite(st.vel[1]) && isfinite(st.vel[2]));
}

int main(void) {
    test_ground_friction_stop();
    test_vq3_air_strafe_speed_gain();
    test_half_life_air_strafe_and_crouch_jump();
    test_cpma_air_turning();
    test_auto_bhop_coyote_and_buffer();
    test_apex_slide_slope_and_tap_strafe();
    test_deadlock_dash_stamina_and_dash_jump();
    test_wall_bounce_hook_and_parsing();

    printf("\nqm_physics_test: %s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
