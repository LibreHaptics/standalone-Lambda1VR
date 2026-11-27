/**
 * LibreHaptics (Loom Engine) Spatial Telemetry & Synthesis Bridge for Lambda1VR
 * File: VrLibreHaptics.c (ABI v1.1)
 *
 * Conforms to LibreHaptics ABI v1.1 spatial & modality contract:
 *   - Torso hits: pure normalized unit cylinder (ux, y, uz); lateral flank spanning is handled by Loom engines
 *   - Arm actuators (TrueGear v2.1 / OWO): (+/-1.40, 0.35, 0.25), LH_ZONE_ARM_LEFT / ARM_RIGHT
 *   - Explicit EMS modality flags: LH_FLAG_PREFER_EMS for sharp combat/weapon kicks,
 *     LH_FLAG_SUPPRESS_EMS for blasts, environmental damage, vitals, footsteps, HEV & UI/backpack cues
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define LH_UDP_IMPLEMENTATION
#include "VrLibreHaptics.h"

#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef LH_STANDALONE_TEST
#include <common/common.h>
#include <common/cvardef.h>
#include <common/xash3d_types.h>
#include <common/const.h>
#include <common/event_args.h>
#include <client/client.h>
#include "VrCommon.h"
#include "VrCvars.h"
#endif

/* ============================================================================
 * Constants & GoldSrc Damage Flags (from hlsdk-xash3d/dlls/cbase.h)
 * ============================================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LH_DMG_GENERIC      0u
#define LH_DMG_CRUSH        (1u << 0)
#define LH_DMG_BULLET       (1u << 1)
#define LH_DMG_SLASH        (1u << 2)
#define LH_DMG_BURN         (1u << 3)
#define LH_DMG_VEHICLE      (1u << 4)
#define LH_DMG_FALL         (1u << 5)
#define LH_DMG_BLAST        (1u << 6)
#define LH_DMG_CLUB         (1u << 7)
#define LH_DMG_SHOCK        (1u << 8)
#define LH_DMG_SONIC        (1u << 9)
#define LH_DMG_ENERGYBEAM   (1u << 10)
#define LH_DMG_DROWN        (1u << 14)
#define LH_DMG_PARALYZE     (1u << 15)
#define LH_DMG_NERVEGAS     (1u << 16)
#define LH_DMG_POISON       (1u << 17)
#define LH_DMG_RADIATION    (1u << 18)
#define LH_DMG_DROWNRECOVER (1u << 19)
#define LH_DMG_ACID         (1u << 20)
#define LH_DMG_SLOWBURN     (1u << 21)
#define LH_DMG_SLOWFREEZE   (1u << 22)
#define LH_DMG_MORTAR       (1u << 23)

/* Calibrated TCS anchor points matching LibreHaptics.HAL.DeviceTopologies */
#define LH_TCS_ARM_X            1.40f
#define LH_TCS_ARM_Y            0.35f
#define LH_TCS_ARM_Z            0.25f
#define LH_TCS_SHOULDER_X       0.68f
#define LH_TCS_SHOULDER_Y       0.60f
#define LH_TCS_SHOULDER_FRONT_Z 0.92f
#define LH_TCS_SHOULDER_BACK_Z  -0.92f
#define LH_TCS_HEAD_Y           1.35f
#define LH_TCS_FEET_Y           -1.25f

/* HL1 Weapon IDs (from hlsdk-xash3d/dlls/weapons.h) */
#define LH_HL_WEAPON_CROWBAR      1
#define LH_HL_WEAPON_HANDGRENADE 12
#define LH_HL_WEAPON_SATCHEL     14

/* ============================================================================
 * Runtime State (zero background threads, rate-limited safely)
 * ============================================================================ */

typedef struct {
    bool     initialized;
    bool     debug_logging;

    int      last_health;
    int      last_battery;
    int      cur_weapon_id;
    int      cur_weapon_clip;
    int      train_speed;
    bool     was_stabilised;
    bool     was_on_ground;
    bool     egon_firing;
    float    prev_horiz_speed;

    uint64_t last_damage_ms;
    uint64_t last_explosion_ms;
    uint64_t last_shake_ms;
    uint64_t last_geiger_ms;
    uint64_t last_charger_ms;
    uint64_t last_gaussspin_ms;
    uint64_t last_egon_pulse_ms;
    uint64_t egon_start_ms;
    uint64_t last_heartbeat_ms;
    uint64_t next_heartbeat_dub_ms;
    float    pending_dub_energy;
    uint64_t last_train_ms;
    uint64_t last_step_ms;
    uint64_t last_longjump_ms;
    uint64_t last_underwater_ms;
    uint64_t last_backpack_hover_ms;
    uint64_t last_reload_ms;
} LH_RuntimeState;

static LH_RuntimeState g_lh = { 0 };

#ifndef LH_STANDALONE_TEST
static convar_t *lh_enable    = NULL;
static convar_t *lh_host      = NULL;
static convar_t *lh_port      = NULL;
static convar_t *lh_intensity = NULL;
static convar_t *lh_heartbeat = NULL;
static convar_t *lh_geiger    = NULL;
static convar_t *lh_footsteps = NULL;
static convar_t *lh_debug     = NULL;

static char     g_lh_synced_host[64] = "";
static int      g_lh_synced_port     = 0;
#endif

/* ============================================================================
 * Utility Helpers
 * ============================================================================ */

static inline float lh_clampf(float v, float lo, float hi) {
    if (!isfinite(v)) return lo;
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uint64_t lh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int lh_strcasecmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int lh_strncasecmp(const char *a, const char *b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}

#define strcasecmp  lh_strcasecmp
#define strncasecmp lh_strncasecmp

static bool lh_str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static LH_EventPacket lh_make_packet(
    uint8_t bus_id,
    uint8_t zone_hint,
    uint16_t flags,
    float x, float y, float z,
    float spread,
    float energy,
    float sharpness,
    float base_freq_hz,
    float freq_glide_hz,
    float decay_ms,
    uint8_t wave_shape
) {
    LH_EventPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.target_display_time_ns = 0;
    pkt.sequence_id            = 0;
    pkt.bus_id                 = bus_id;
    pkt.payload_type           = (uint8_t)LH_PAYLOAD_PARAMETRIC;
    pkt.zone_hint              = zone_hint;
    pkt.flags                  = flags;
    pkt.entry_pos[0]           = lh_clampf(x, -2.0f, 2.0f);
    pkt.entry_pos[1]           = lh_clampf(y, -2.0f, 2.0f);
    pkt.entry_pos[2]           = lh_clampf(z, -2.0f, 2.0f);
    pkt.spread_radius          = lh_clampf(spread, 0.01f, 2.50f);

    pkt.payload.synth.energy           = lh_clampf(energy, 0.0f, 1.8f);
    pkt.payload.synth.attack_sharpness = lh_clampf(sharpness, 0.0f, 1.0f);
    pkt.payload.synth.base_freq_hz     = lh_clampf(base_freq_hz, 5.0f, 500.0f);
    pkt.payload.synth.freq_glide_hz    = lh_clampf(freq_glide_hz, -300.0f, 300.0f);
    pkt.payload.synth.decay_ms         = lh_clampf(decay_ms, 10.0f, 1500.0f);
    pkt.payload.synth.wave_shape       = wave_shape;
    return pkt;
}

static void lh_log_debug(const char *tag, const LH_EventPacket *pkt) {
    if (!g_lh.debug_logging || !pkt) return;
#ifndef LH_STANDALONE_TEST
    MsgDev(D_INFO, "[LH] %s hash=0x%08X bus=%u zone=%u pos=(%.2f,%.2f,%.2f) spread=%.2f E=%.2f sharp=%.2f f=%.0fHz d=%.0fms\n",
           tag, (unsigned)pkt->payload.synth.effect_hash, pkt->bus_id, pkt->zone_hint,
           pkt->entry_pos[0], pkt->entry_pos[1], pkt->entry_pos[2],
           pkt->spread_radius, pkt->payload.synth.energy,
           pkt->payload.synth.attack_sharpness, pkt->payload.synth.base_freq_hz,
           pkt->payload.synth.decay_ms);
#else
    (void)tag;
#endif
}

static void lh_emit_one(const char *tag, const LH_EventPacket *pkt) {
    if (!pkt) return;
    LH_EventPacket stamped = *pkt;
    if (stamped.payload_type == (uint8_t)LH_PAYLOAD_PARAMETRIC &&
        stamped.payload.synth.effect_hash == 0u &&
        tag && tag[0]) {
        stamped.payload.synth.effect_hash = lh_fnv1a_hash(tag);
    }
    lh_log_debug(tag, &stamped);
    (void)lh_udp_send(&stamped);
}

static void lh_emit_batch(const char *tag, const LH_EventPacket *pkts, size_t count) {
    if (!pkts || count == 0) return;
    uint32_t tag_hash = (tag && tag[0]) ? lh_fnv1a_hash(tag) : 0u;
    if (tag_hash == 0u) {
        lh_log_debug(tag, &pkts[0]);
        (void)lh_udp_send_batch(pkts, count);
        return;
    }

    size_t offset = 0;
    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > LH_UDP_MAX_BATCH_PACKETS) {
            chunk = LH_UDP_MAX_BATCH_PACKETS;
        }
        LH_EventPacket stamped[LH_UDP_MAX_BATCH_PACKETS];
        for (size_t i = 0; i < chunk; ++i) {
            stamped[i] = pkts[offset + i];
            if (stamped[i].payload_type == (uint8_t)LH_PAYLOAD_PARAMETRIC &&
                stamped[i].payload.synth.effect_hash == 0u) {
                stamped[i].payload.synth.effect_hash = tag_hash;
            }
        }
        if (offset == 0) {
            lh_log_debug(tag, &stamped[0]);
        }
        (void)lh_udp_send_batch(stamped, chunk);
        offset += chunk;
    }
}

/* ============================================================================
 * Spatial Projection: GoldSrc 3D (+Z Up) -> LibreHaptics TCS
 * ============================================================================ */

LH_TcsHitPoint LH_L1VR_ProjectWorldDelta(
    float dx,
    float dy,
    float dz,
    float torso_yaw_deg,
    bool has_exact_height
) {
    LH_TcsHitPoint out;
    memset(&out, 0, sizeof(out));

    float yaw_rad = torso_yaw_deg * (float)(M_PI / 180.0);
    float cos_y = cosf(yaw_rad);
    float sin_y = sinf(yaw_rad);

    /* GoldSrc horizontal basis (verified against hlsdk-xash3d CalcDamageDirection):
     *   forward = (cosYaw,  sinYaw, 0)
     *   right   = (sinYaw, -cosYaw, 0)
     *   up      = (0,       0,      1)
     */
    float local_forward = dx * cos_y + dy * sin_y;
    float local_right   = dx * sin_y - dy * cos_y;
    float horiz_len     = sqrtf(local_right * local_right + local_forward * local_forward);

    float ux = 0.0f;
    float uz = 1.0f;
    if (horiz_len > 0.01f) {
        ux = local_right / horiz_len;
        uz = local_forward / horiz_len;
    }

    float tcs_y = 0.15f;
    uint8_t zone = (uint8_t)LH_ZONE_TORSO;
    bool headshot = false;

    /* Half-Life player standing height is 72 units, eye ~+28, chest ~+10 above origin */
    if (has_exact_height && horiz_len < 160.0f) {
        if (dz > 18.0f) {
            tcs_y = LH_TCS_HEAD_Y;
            zone = (uint8_t)LH_ZONE_HEAD;
            headshot = true;
        } else if (dz < -24.0f) {
            tcs_y = LH_TCS_FEET_Y;
            zone = (uint8_t)LH_ZONE_FEET;
        } else {
            tcs_y = lh_clampf(dz / 20.0f, -0.82f, 0.82f);
        }
    } else {
        float total_dist = sqrtf(horiz_len * horiz_len + dz * dz);
        if (total_dist > 0.01f) {
            float sin_elev = dz / total_dist;
            if (sin_elev > 0.76f) {
                tcs_y = LH_TCS_HEAD_Y;
                zone = (uint8_t)LH_ZONE_HEAD;
                headshot = true;
            } else if (sin_elev < -0.76f) {
                tcs_y = LH_TCS_FEET_Y;
                zone = (uint8_t)LH_ZONE_FEET;
            } else {
                tcs_y = lh_clampf(sin_elev * 1.15f, -0.80f, 0.80f);
            }
        }
    }

    /* Pure normalized unit torso cylinder (ux, y, uz) — flank & vest-plane mapping belong to Loom engines */
    float tcs_x = lh_clampf(ux, -1.0f, 1.0f);
    float tcs_z = lh_clampf(uz, -1.0f, 1.0f);

    if (zone == (uint8_t)LH_ZONE_HEAD) {
        tcs_x = lh_clampf(ux * 0.35f, -0.40f, 0.40f);
        tcs_z = lh_clampf(uz * 0.80f, -0.80f, 0.80f);
    } else if (zone == (uint8_t)LH_ZONE_FEET) {
        tcs_x = lh_clampf(ux * 0.30f, -0.35f, 0.35f);
        tcs_z = lh_clampf(uz * 0.40f, -0.45f, 0.45f);
    }

    out.x = tcs_x;
    out.y = tcs_y;
    out.z = tcs_z;
    out.zone_hint = zone;
    out.is_headshot = headshot;
    return out;
}

/* ============================================================================
 * Weapon Recoil Helper (Dominant Arm + Optional 2H Stabilised Off-Hand/Shoulder)
 * ============================================================================ */

static void lh_emit_weapon_recoil(
    const char *tag,
    bool is_left_handed,
    bool is_stabilised,
    float energy,
    float sharpness,
    float freq_hz,
    float glide_hz,
    float decay_ms,
    uint8_t wave_shape,
    float shoulder_ratio,
    float offhand_ratio,
    bool prefer_ems
) {
    bool right_hand = !is_left_handed;
    float dom_arm_x = right_hand ? LH_TCS_ARM_X : -LH_TCS_ARM_X;
    float off_arm_x = right_hand ? -LH_TCS_ARM_X : LH_TCS_ARM_X;
    float shoulder_x = right_hand ? LH_TCS_SHOULDER_X : -LH_TCS_SHOULDER_X;

    uint8_t dom_zone = right_hand ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT;
    uint8_t off_zone = right_hand ? (uint8_t)LH_ZONE_ARM_LEFT  : (uint8_t)LH_ZONE_ARM_RIGHT;
    uint16_t arm_flags = (uint16_t)(LH_FLAG_ACTIVE_BRAKING | (prefer_ems ? LH_FLAG_PREFER_EMS : LH_FLAG_SUPPRESS_EMS));
    uint16_t shoulder_flags = (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_SUPPRESS_EMS);

    LH_EventPacket batch[3];
    size_t count = 0;

    /* 1. Dominant arm / EMS cuff kick */
    batch[count++] = lh_make_packet(
        (uint8_t)LH_BUS_WEAPON_RECOIL,
        dom_zone,
        arm_flags,
        dom_arm_x, LH_TCS_ARM_Y, LH_TCS_ARM_Z,
        0.18f,
        energy,
        sharpness,
        freq_hz,
        glide_hz,
        decay_ms,
        wave_shape
    );

    /* 2. Shoulder / upper chest stock kick (vibro vest feedback, suppresses EMS duplication) */
    float eff_shoulder = shoulder_ratio;
    if (is_stabilised && shoulder_ratio > 0.05f) {
        eff_shoulder = lh_clampf(eff_shoulder + 0.14f, 0.60f, 1.15f);
    }
    if (eff_shoulder > 0.05f) {
        float torso_decay_ms = decay_ms * 1.35f;
        if (torso_decay_ms < 58.0f) {
            torso_decay_ms = 58.0f;
        }
        /* Heavy double-barrel / grenade launcher blasts wrap subtly to upper back via wide spread */
        float torso_spread = (eff_shoulder >= 0.98f) ? 1.05f : 0.36f;
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_WEAPON_RECOIL,
            (uint8_t)LH_ZONE_TORSO,
            shoulder_flags,
            shoulder_x, LH_TCS_SHOULDER_Y, LH_TCS_SHOULDER_FRONT_Z,
            torso_spread,
            energy * eff_shoulder,
            lh_clampf(sharpness * 0.85f, 0.30f, 0.90f),
            lh_clampf(freq_hz * 0.82f, 25.0f, 220.0f),
            glide_hz * 0.6f,
            torso_decay_ms,
            wave_shape
        );
    }

    /* 3. Off-hand grip kick when two-handed weapon stabilisation is engaged */
    if (is_stabilised && offhand_ratio > 0.05f) {
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_WEAPON_RECOIL,
            off_zone,
            arm_flags,
            off_arm_x, LH_TCS_ARM_Y, LH_TCS_ARM_Z,
            0.18f,
            energy * offhand_ratio,
            lh_clampf(sharpness * 0.90f, 0.40f, 0.92f),
            freq_hz * 1.05f,
            glide_hz * 0.8f,
            decay_ms * 0.85f,
            wave_shape
        );
    }

    lh_emit_batch(tag, batch, count);
}

/* ============================================================================
 * Damage Processing (GoldSrc 12B Damage Message)
 * ============================================================================ */

bool LH_L1VR_ProcessDamage(
    int armor_dmg,
    int health_dmg,
    uint32_t bits_damage,
    const float vec_from[3],
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg,
    int prev_armor_value
) {
    int total_dmg = armor_dmg + health_dmg;
    if (total_dmg <= 0) {
        return false;
    }

    g_lh.last_damage_ms = lh_now_ms();

    /* Compute 3D hit point in TCS */
    LH_TcsHitPoint hit;
    bool has_vec_from = (vec_from != NULL) &&
                        (fabsf(vec_from[0]) > 0.01f || fabsf(vec_from[1]) > 0.01f || fabsf(vec_from[2]) > 0.01f);

    if (has_vec_from && player_origin != NULL) {
        float chest_z = player_origin[2] + ((view_ofs_z > 4.0f) ? (view_ofs_z * 0.40f) : 10.0f);
        float dx = vec_from[0] - player_origin[0];
        float dy = vec_from[1] - player_origin[1];
        float dz = vec_from[2] - chest_z;
        hit = LH_L1VR_ProjectWorldDelta(dx, dy, dz, torso_yaw_deg, true);
    } else {
        hit.x = 0.0f;
        hit.y = 0.20f;
        hit.z = 1.0f;
        hit.zone_hint = (uint8_t)LH_ZONE_TORSO;
        hit.is_headshot = false;
    }

    /* Synthesize physical parameters from GoldSrc DMG_* bitmask */
    float energy    = lh_clampf((float)total_dmg / 32.0f, 0.30f, 1.35f);
    float sharpness = 0.86f;
    float base_freq = 150.0f;
    float glide     = -75.0f;
    float decay_ms  = 65.0f;
    float spread    = 0.16f;
    uint8_t wave    = (uint8_t)LH_WAVE_SINE;
    uint16_t flags  = (uint16_t)LH_FLAG_ACTIVE_BRAKING;
    bool prefer_ems = false;
    bool emit_fall_waist = false;
    bool emit_dual_shock_arms = false;

    if (hit.is_headshot) {
        flags |= (uint16_t)LH_FLAG_HEADSHOT;
    }

    if (bits_damage & LH_DMG_FALL) {
        /* Fall impact enters through feet and radiates up lower waist (pure kinetic, suppress EMS) */
        hit.x = 0.0f;
        hit.y = LH_TCS_FEET_Y;
        hit.z = 0.0f;
        hit.zone_hint = (uint8_t)LH_ZONE_FEET;
        energy    = lh_clampf((float)total_dmg / 26.0f, 0.42f, 1.25f);
        sharpness = 0.55f;
        base_freq = 44.0f;
        glide     = -14.0f;
        decay_ms  = 175.0f;
        spread    = 0.65f;
        prefer_ems = false;
        emit_fall_waist = true;
    } else if (bits_damage & (LH_DMG_BLAST | LH_DMG_MORTAR)) {
        /* Explosive shockwave: deep visceral sub-bass engulfing the vest (suppress EMS) */
        energy    = lh_clampf((float)total_dmg / 25.0f, 0.50f, 1.45f);
        sharpness = 0.38f;
        base_freq = 28.0f;
        glide     = -10.0f;
        decay_ms  = 380.0f;
        spread    = 1.05f;
        wave      = (uint8_t)LH_WAVE_NOISE;
        flags    |= (uint16_t)LH_FLAG_BYPASS_LIMITER;
        prefer_ems = false;
    } else if (bits_damage & (LH_DMG_SHOCK | LH_DMG_ENERGYBEAM)) {
        /* Vortigaunt lightning / Resonance Cascade arcs / laser tripmine -> explicit EMS square wave */
        energy    = lh_clampf((float)total_dmg / 28.0f, 0.48f, 1.35f);
        sharpness = 0.98f;
        base_freq = 265.0f;
        glide     = 35.0f;
        decay_ms  = 120.0f;
        spread    = 0.65f;
        wave      = (uint8_t)LH_WAVE_SQUARE;
        prefer_ems = true;
        emit_dual_shock_arms = true;
    } else if (bits_damage & LH_DMG_SONIC) {
        /* Houndeye concentric sonic shockwave */
        energy    = lh_clampf((float)total_dmg / 24.0f, 0.48f, 1.30f);
        sharpness = 0.45f;
        base_freq = 48.0f;
        glide     = -18.0f;
        decay_ms  = 260.0f;
        spread    = 0.90f;
        wave      = (uint8_t)LH_WAVE_TRIANGLE;
        prefer_ems = false;
    } else if (bits_damage & LH_DMG_BULLET) {
        /* Ballistic penetration hit (HECU MP5, turrets, snipers, assassins) */
        energy    = lh_clampf((float)total_dmg / 24.0f, 0.38f, 1.35f);
        sharpness = 0.95f;
        base_freq = 185.0f;
        glide     = -110.0f;
        decay_ms  = 50.0f;
        spread    = 0.08f;
        flags    |= (uint16_t)LH_FLAG_PENETRATION;
        prefer_ems = true;
    } else if (bits_damage & (LH_DMG_SLASH | LH_DMG_CLUB)) {
        /* Melee claw/bite/swipe (Headcrab, Zombie, Alien Grunt, Bullsquid) */
        energy    = lh_clampf((float)total_dmg / 26.0f, 0.42f, 1.30f);
        sharpness = 0.92f;
        base_freq = (bits_damage & LH_DMG_SLASH) ? 220.0f : 95.0f;
        glide     = -65.0f;
        decay_ms  = 78.0f;
        spread    = 0.22f;
        prefer_ems = true;
    } else if (bits_damage & (LH_DMG_BURN | LH_DMG_SLOWBURN)) {
        /* Fire / hot steam / lava */
        energy    = lh_clampf((float)total_dmg / 22.0f, 0.30f, 1.00f);
        sharpness = 0.25f;
        base_freq = 110.0f;
        glide     = 20.0f;
        decay_ms  = 185.0f;
        spread    = 0.72f;
        wave      = (uint8_t)LH_WAVE_NOISE;
        prefer_ems = false;
    } else if (bits_damage & (LH_DMG_ACID | LH_DMG_POISON | LH_DMG_NERVEGAS | LH_DMG_RADIATION)) {
        /* Bullsquid acid / toxic waste / radiation burn */
        energy    = lh_clampf((float)total_dmg / 22.0f, 0.28f, 0.95f);
        sharpness = 0.28f;
        base_freq = 135.0f;
        glide     = 25.0f;
        decay_ms  = 165.0f;
        spread    = 0.58f;
        wave      = (uint8_t)LH_WAVE_NOISE;
        prefer_ems = false;
    } else if (bits_damage & LH_DMG_DROWN) {
        /* Drowning chest pressure */
        hit.x = 0.0f;
        hit.y = 0.30f;
        hit.z = 0.85f;
        hit.zone_hint = (uint8_t)LH_ZONE_TORSO;
        energy    = 0.55f;
        sharpness = 0.18f;
        base_freq = 38.0f;
        glide     = -8.0f;
        decay_ms  = 240.0f;
        spread    = 0.80f;
        prefer_ems = false;
    } else if (bits_damage & (LH_DMG_CRUSH | LH_DMG_VEHICLE)) {
        /* Heavy crushing / train / door slam */
        energy    = lh_clampf((float)total_dmg / 26.0f, 0.55f, 1.40f);
        sharpness = 0.62f;
        base_freq = 52.0f;
        glide     = -15.0f;
        decay_ms  = 190.0f;
        spread    = 0.75f;
        prefer_ems = false;
    } else if (bits_damage & LH_DMG_SLOWFREEZE) {
        /* Freezer room cold shiver */
        energy    = lh_clampf((float)total_dmg / 20.0f, 0.26f, 0.80f);
        sharpness = 0.22f;
        base_freq = 155.0f;
        glide     = -20.0f;
        decay_ms  = 150.0f;
        spread    = 0.75f;
        wave      = (uint8_t)LH_WAVE_TRIANGLE;
        prefer_ems = false;
    }

    /* HEV Suit armor absorption modulation */
    bool full_armor_absorb = (armor_dmg > 0 && health_dmg == 0);
    bool armor_shattered   = (armor_dmg > 0 && prev_armor_value > 0 && prev_armor_value <= armor_dmg);

    if (full_armor_absorb) {
        spread    = lh_clampf(spread * 2.0f, 0.24f, 0.85f);
        sharpness = lh_clampf(sharpness * 0.72f, 0.30f, 0.78f);
        base_freq = lh_clampf(base_freq * 1.20f, 65.0f, 320.0f);
        prefer_ems = false;
    }

    flags |= (uint16_t)(prefer_ems ? LH_FLAG_PREFER_EMS : LH_FLAG_SUPPRESS_EMS);

    LH_EventPacket batch[4];
    size_t count = 0;

    /* Emit 1 clean directional packet; lateral flank spanning belongs to Loom engine */
    batch[count++] = lh_make_packet(
        (uint8_t)LH_BUS_CRITICAL_IMPACT, hit.zone_hint, flags,
        hit.x, hit.y, hit.z,
        spread,
        energy, sharpness, base_freq, glide, decay_ms, wave
    );

    if (emit_fall_waist && count < 4) {
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, -0.75f, 0.0f, 0.70f,
            energy * 0.72f, 0.45f, 52.0f, -15.0f, 140.0f, (uint8_t)LH_WAVE_SINE
        );
    }

    if (emit_dual_shock_arms && count + 2 <= 4) {
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_ARM_LEFT,
            (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_PREFER_EMS),
            -LH_TCS_ARM_X, LH_TCS_ARM_Y, LH_TCS_ARM_Z, 0.25f,
            energy * 0.75f, 0.96f, 250.0f, 30.0f, 90.0f, (uint8_t)LH_WAVE_SQUARE
        );
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_ARM_RIGHT,
            (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_PREFER_EMS),
            LH_TCS_ARM_X, LH_TCS_ARM_Y, LH_TCS_ARM_Z, 0.25f,
            energy * 0.75f, 0.96f, 250.0f, 30.0f, 90.0f, (uint8_t)LH_WAVE_SQUARE
        );
    }

    if (armor_shattered && count < 4) {
        batch[count++] = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_TORSO,
            (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_SUPPRESS_EMS),
            0.0f, 0.25f, 1.0f, 0.75f,
            0.78f, 0.88f, 280.0f, -160.0f, 130.0f, (uint8_t)LH_WAVE_TRIANGLE
        );
    }

    lh_emit_batch("Damage", batch, count);
    return true;
}

/* ============================================================================
 * Weapon Events Processing (CL_FireEvent -> events/<name>.sc)
 * ============================================================================ */

bool LH_L1VR_ProcessWeaponEvent(
    const char *event_name,
    float fparam1,
    int iparam1,
    int bparam1,
    int bparam2,
    bool is_left_handed,
    bool is_stabilised
) {
    if (!event_name || event_name[0] == '\0') return false;

    uint64_t now = lh_now_ms();

    /* Strip leading "events/" if present */
    const char *ev = event_name;
    if (strncasecmp(ev, "events/", 7) == 0) {
        ev += 7;
    }

    /* 1. 9mm Glock (primary / burst) */
    if (strcasecmp(ev, "glock1.sc") == 0 || strcasecmp(ev, "glock2.sc") == 0) {
        bool fast = (strcasecmp(ev, "glock2.sc") == 0);
        lh_emit_weapon_recoil("Glock", is_left_handed, is_stabilised,
            fast ? 0.64f : 0.58f, 0.88f, 155.0f, -55.0f, 42.0f,
            (uint8_t)LH_WAVE_SINE, 0.58f, 0.45f, true);
        return true;
    }

    /* 2. .357 Magnum Python */
    if (strcasecmp(ev, "python.sc") == 0) {
        lh_emit_weapon_recoil("Python357", is_left_handed, is_stabilised,
            1.02f, 0.96f, 84.0f, -40.0f, 75.0f,
            (uint8_t)LH_WAVE_SINE, 0.85f, 0.68f, true);
        return true;
    }

    /* 3. MP5 SMG (primary) */
    if (strcasecmp(ev, "mp5.sc") == 0) {
        lh_emit_weapon_recoil("MP5", is_left_handed, is_stabilised,
            0.62f, 0.86f, 135.0f, -45.0f, 40.0f,
            (uint8_t)LH_WAVE_SINE, 0.72f, 0.55f, true);
        return true;
    }

    /* 4. MP5 M203 Underbarrel Grenade Launcher */
    if (strcasecmp(ev, "mp52.sc") == 0) {
        lh_emit_weapon_recoil("MP5_M203", is_left_handed, is_stabilised,
            0.98f, 0.72f, 52.0f, -20.0f, 115.0f,
            (uint8_t)LH_WAVE_SINE, 0.92f, 0.72f, true);
        return true;
    }

    /* 5. SPAS-12 Shotgun (single vs devastating double-barrel!) */
    if (strcasecmp(ev, "shotgun1.sc") == 0) {
        lh_emit_weapon_recoil("Shotgun1x", is_left_handed, is_stabilised,
            0.96f, 0.92f, 68.0f, -30.0f, 82.0f,
            (uint8_t)LH_WAVE_SINE, 0.90f, 0.70f, true);
        return true;
    }
    if (strcasecmp(ev, "shotgun2.sc") == 0) {
        lh_emit_weapon_recoil("Shotgun2x", is_left_handed, is_stabilised,
            1.28f, 0.96f, 46.0f, -20.0f, 120.0f,
            (uint8_t)LH_WAVE_SINE, 1.05f, 0.85f, true);
        return true;
    }

    /* 6. Crossbow (bolt / scoped sniper bolt) */
    if (strcasecmp(ev, "crossbow1.sc") == 0 || strcasecmp(ev, "crossbow2.sc") == 0) {
        lh_emit_weapon_recoil("Crossbow", is_left_handed, is_stabilised,
            0.72f, 0.88f, 190.0f, -95.0f, 58.0f,
            (uint8_t)LH_WAVE_TRIANGLE, 0.65f, 0.55f, true);
        return true;
    }

    /* 7. RPG Rocket Launcher (massive backblast & shoulder punch) */
    if (strcasecmp(ev, "rpg.sc") == 0) {
        bool right_hand = !is_left_handed;
        float dom_arm_x = right_hand ? LH_TCS_ARM_X : -LH_TCS_ARM_X;
        float sh_x = right_hand ? LH_TCS_SHOULDER_X : -LH_TCS_SHOULDER_X;
        LH_EventPacket batch[3];
        batch[0] = lh_make_packet(
            (uint8_t)LH_BUS_WEAPON_RECOIL,
            right_hand ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT,
            (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_PREFER_EMS),
            dom_arm_x, LH_TCS_ARM_Y, LH_TCS_ARM_Z, 0.25f,
            1.10f, 0.84f, 42.0f, -15.0f, 150.0f, (uint8_t)LH_WAVE_NOISE
        );
        batch[1] = lh_make_packet(
            (uint8_t)LH_BUS_WEAPON_RECOIL, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            sh_x, LH_TCS_SHOULDER_Y, LH_TCS_SHOULDER_FRONT_Z, 0.55f,
            1.15f, 0.52f, 34.0f, -12.0f, 210.0f, (uint8_t)LH_WAVE_SINE
        );
        batch[2] = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            sh_x * 0.85f, 0.45f, LH_TCS_SHOULDER_BACK_Z, 0.65f,
            0.70f, 0.35f, 30.0f, -10.0f, 240.0f, (uint8_t)LH_WAVE_NOISE
        );
        lh_emit_batch("RPG", batch, 3);
        return true;
    }

    /* 8. Tau Cannon (Gauss) — progressive charge hum & overcharged discharge */
    if (strcasecmp(ev, "gaussspin.sc") == 0) {
        if (now - g_lh.last_gaussspin_ms < 70ULL) {
            return false;
        }
        g_lh.last_gaussspin_ms = now;
        /* iparam1 carries pitch [110..250] proportional to charge level */
        float charge = lh_clampf(((float)iparam1 - 100.0f) / 150.0f, 0.15f, 1.0f);
        bool right_hand = !is_left_handed;
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_WEAPON_RECOIL,
            right_hand ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT,
            (uint16_t)LH_FLAG_SUPPRESS_EMS,
            right_hand ? LH_TCS_ARM_X : -LH_TCS_ARM_X, LH_TCS_ARM_Y, LH_TCS_ARM_Z,
            0.22f,
            0.20f + 0.55f * charge,
            0.35f + 0.30f * charge,
            85.0f + 165.0f * charge,
            25.0f,
            68.0f,
            (uint8_t)LH_WAVE_TRIANGLE
        );
        lh_emit_one("GaussSpin", &pkt);
        return true;
    }
    if (strcasecmp(ev, "gauss.sc") == 0) {
        /* bparam2 != 0 indicates stop/holster cleanup event, not a shot */
        if (bparam2 != 0) return false;
        bool primary = (bparam1 != 0);
        float charge_power = primary ? 0.65f : lh_clampf(fparam1 / 160.0f, 0.70f, 1.35f);
        lh_emit_weapon_recoil(
            primary ? "GaussPrimary" : "GaussCharged",
            is_left_handed, is_stabilised,
            charge_power,
            0.96f,
            primary ? 175.0f : 90.0f,
            -60.0f,
            primary ? 65.0f : 135.0f,
            (uint8_t)LH_WAVE_SQUARE,
            primary ? 0.68f : 1.00f,
            0.75f,
            true
        );
        return true;
    }

    /* 9. Gluon Gun (Egon) — continuous sustained energy beam */
    if (strcasecmp(ev, "egon_fire.sc") == 0) {
        g_lh.egon_firing = true;
        g_lh.egon_start_ms = now;
        g_lh.last_egon_pulse_ms = now;
        lh_emit_weapon_recoil("EgonStart", is_left_handed, is_stabilised,
            0.76f, 0.82f, 165.0f, 30.0f, 80.0f,
            (uint8_t)LH_WAVE_SQUARE, 0.66f, 0.55f, true);
        return true;
    }
    if (strcasecmp(ev, "egon_stop.sc") == 0) {
        g_lh.egon_firing = false;
        return false;
    }

    /* 10. Alien Hivehand (Hornet Gun) */
    if (strcasecmp(ev, "firehornet.sc") == 0) {
        lh_emit_weapon_recoil("HornetGun", is_left_handed, is_stabilised,
            0.46f, 0.55f, 95.0f, 25.0f, 55.0f,
            (uint8_t)LH_WAVE_TRIANGLE, 0.15f, 0.25f, false);
        return true;
    }

    /* 11. Tripmine placement & Snark throw */
    if (strcasecmp(ev, "tripfire.sc") == 0) {
        lh_emit_weapon_recoil("TripminePlant", is_left_handed, false,
            0.40f, 0.85f, 185.0f, -40.0f, 35.0f,
            (uint8_t)LH_WAVE_IMPULSE, 0.0f, 0.0f, false);
        return true;
    }
    if (strcasecmp(ev, "snarkfire.sc") == 0) {
        lh_emit_weapon_recoil("SnarkThrow", is_left_handed, false,
            0.35f, 0.50f, 130.0f, 40.0f, 40.0f,
            (uint8_t)LH_WAVE_TRIANGLE, 0.0f, 0.0f, false);
        return true;
    }

    /* 12. Crowbar trigger fallback (MP) */
    if (strcasecmp(ev, "crowbar.sc") == 0) {
        lh_emit_weapon_recoil("CrowbarSwing", is_left_handed, false,
            0.32f, 0.40f, 75.0f, -20.0f, 45.0f,
            (uint8_t)LH_WAVE_SINE, 0.15f, 0.0f, false);
        return true;
    }

    /* 13. Opposing Force weapons (automatic out-of-the-box support) */
    if (strcasecmp(ev, "eagle.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_DesertEagle", is_left_handed, is_stabilised,
            0.94f, 0.94f, 95.0f, -45.0f, 65.0f,
            (uint8_t)LH_WAVE_SINE, 0.78f, 0.60f, true);
        return true;
    }
    if (strcasecmp(ev, "m249.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_M249", is_left_handed, is_stabilised,
            0.82f, 0.90f, 92.0f, -35.0f, 48.0f,
            (uint8_t)LH_WAVE_SINE, 0.82f, 0.72f, true);
        return true;
    }
    if (strcasecmp(ev, "sniper.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_M40A1", is_left_handed, is_stabilised,
            1.12f, 0.96f, 62.0f, -30.0f, 95.0f,
            (uint8_t)LH_WAVE_SINE, 0.95f, 0.75f, true);
        return true;
    }
    if (strcasecmp(ev, "spore.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_SporeLauncher", is_left_handed, is_stabilised,
            0.78f, 0.60f, 64.0f, -20.0f, 95.0f,
            (uint8_t)LH_WAVE_TRIANGLE, 0.68f, 0.55f, false);
        return true;
    }
    if (strcasecmp(ev, "shock.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_ShockRoach", is_left_handed, is_stabilised,
            0.55f, 0.90f, 225.0f, 35.0f, 45.0f,
            (uint8_t)LH_WAVE_SQUARE, 0.20f, 0.35f, true);
        return true;
    }
    if (strcasecmp(ev, "displacer.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_Displacer", is_left_handed, is_stabilised,
            1.15f, 0.95f, 110.0f, 75.0f, 180.0f,
            (uint8_t)LH_WAVE_SQUARE, 0.85f, 0.85f, true);
        return true;
    }
    if (strcasecmp(ev, "pipewrench.sc") == 0 || strcasecmp(ev, "knife.sc") == 0) {
        lh_emit_weapon_recoil("OpFor_Melee", is_left_handed, false,
            0.38f, 0.45f, 80.0f, -20.0f, 45.0f,
            (uint8_t)LH_WAVE_SINE, 0.18f, 0.0f, false);
        return true;
    }

    return false;
}

/* ============================================================================
 * 3D Nearby Explosions (TE_EXPLOSION / TE_EXPLOSION2 / TE_TAREXPLOSION)
 * ============================================================================ */

bool LH_L1VR_ProcessExplosion(
    const float explosion_pos[3],
    float scale,
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg
) {
    if (!explosion_pos || !player_origin) return false;

    uint64_t now = lh_now_ms();
    if (now - g_lh.last_explosion_ms < 85ULL) {
        return false;
    }

    float chest_z = player_origin[2] + ((view_ofs_z > 4.0f) ? (view_ofs_z * 0.40f) : 10.0f);
    float dx = explosion_pos[0] - player_origin[0];
    float dy = explosion_pos[1] - player_origin[1];
    float dz = explosion_pos[2] - chest_z;

    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    const float max_radius = 650.0f;
    if (dist > max_radius) {
        return false;
    }

    g_lh.last_explosion_ms = now;

    /* Smooth quadratic distance attenuation */
    float norm_dist = dist / max_radius;
    float falloff = (1.0f - norm_dist) * (1.0f - norm_dist);
    float scale_factor = lh_clampf(scale > 0.1f ? (scale / 3.0f) : 1.0f, 0.65f, 1.35f);
    float energy = lh_clampf(falloff * scale_factor * 1.10f, 0.15f, 1.15f);

    /* Core directional centroid so wide H-VBAP washes over the whole vest with strongest blast-facing side */
    float yaw_rad = torso_yaw_deg * (float)(M_PI / 180.0);
    float cos_y = cosf(yaw_rad);
    float sin_y = sinf(yaw_rad);
    float local_forward = dx * cos_y + dy * sin_y;
    float local_right   = dx * sin_y - dy * cos_y;

    float cx = 0.0f, cy = 0.0f, cz = 0.35f;
    if (dist > 1.0f) {
        cx = lh_clampf((local_right / dist) * 0.55f, -0.65f, 0.65f);
        cy = lh_clampf((dz / dist) * 0.45f, -0.55f, 0.55f);
        cz = lh_clampf((local_forward / dist) * 0.70f, -0.75f, 0.75f);
    }

    LH_EventPacket pkt = lh_make_packet(
        (uint8_t)LH_BUS_ENVIRONMENT,
        (uint8_t)LH_ZONE_TORSO,
        (uint16_t)LH_FLAG_SUPPRESS_EMS,
        cx, cy, cz,
        1.15f,
        energy,
        0.34f, /* Pure visceral sub-bass, suppress EMS */
        28.0f,
        -10.0f,
        340.0f,
        (uint8_t)LH_WAVE_NOISE
    );
    lh_emit_one("Explosion3D", &pkt);
    return true;
}

/* ============================================================================
 * Sound-Driven World & VR Melee Cues (S_StartSound)
 * ============================================================================ */

bool LH_L1VR_ProcessSound(
    const char *sound_name,
    const float sound_pos[3],
    bool is_local_player,
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg,
    bool is_left_handed,
    bool is_stabilised,
    bool footsteps_enabled,
    float fvol
) {
    if (!sound_name || sound_name[0] == '\0') return false;

    /* Fast prefix check to reject 95% of unrelated world sounds in O(1) */
    char c0 = sound_name[0];
    if (c0 == '*' || c0 == '!') {
        sound_name++;
        c0 = sound_name[0];
    }

    uint64_t now = lh_now_ms();

    /* --- WEAPONS: VR Physical Crowbar / Pipe Wrench / Knife / Reloads --- */
    if (strncasecmp(sound_name, "weapons/", 8) == 0) {
        const char *w = sound_name + 8;

        /* Crowbar hit flesh (meaty impact!) - local player only */
        if (is_local_player &&
            (strncasecmp(w, "cbar_hitbod", 11) == 0 ||
             strncasecmp(w, "pwrench_hitbod", 14) == 0 ||
             strncasecmp(w, "knife_hit_flesh", 15) == 0)) {
            lh_emit_weapon_recoil("MeleeHitFlesh", is_left_handed, false,
                0.90f, 0.92f, 95.0f, -35.0f, 75.0f,
                (uint8_t)LH_WAVE_SINE, 0.62f, 0.0f, true);
            return true;
        }

        /* Crowbar hit wall/crate/metal (sharp metallic clang!) - local player only */
        if (is_local_player &&
            (strncasecmp(w, "cbar_hit", 8) == 0 ||
             strncasecmp(w, "pwrench_hit", 11) == 0 ||
             strncasecmp(w, "knife_hit_wall", 14) == 0)) {
            lh_emit_weapon_recoil("MeleeHitMetal", is_left_handed, false,
                0.78f, 0.95f, 215.0f, -70.0f, 60.0f,
                (uint8_t)LH_WAVE_TRIANGLE, 0.52f, 0.0f, true);
            return true;
        }

        /* Crowbar swing through air - local player only */
        if (is_local_player &&
            (strncasecmp(w, "cbar_miss", 9) == 0 ||
             strncasecmp(w, "pwrench_miss", 12) == 0)) {
            lh_emit_weapon_recoil("MeleeMiss", is_left_handed, false,
                0.22f, 0.25f, 65.0f, -15.0f, 40.0f,
                (uint8_t)LH_WAVE_SINE, 0.0f, 0.0f, false);
            return true;
        }

        /* Reload clicks & bolt actions (local player only) */
        if (is_local_player &&
            (strncasecmp(w, "reload", 6) == 0 ||
             strncasecmp(w, "357_reload", 10) == 0 ||
             strncasecmp(w, "xbow_reload", 11) == 0 ||
             strncasecmp(w, "scock", 5) == 0 ||
             strncasecmp(w, "357_cock", 8) == 0 ||
             lh_str_contains_ci(w, "clipin") ||
             lh_str_contains_ci(w, "clipout"))) {
            if (now - g_lh.last_reload_ms < 120ULL) return false;
            g_lh.last_reload_ms = now;
            lh_emit_weapon_recoil("ReloadClick", is_left_handed, is_stabilised,
                0.34f, 0.82f, 165.0f, -35.0f, 28.0f,
                (uint8_t)LH_WAVE_IMPULSE, 0.0f, 0.45f, false);
            return true;
        }

        /* Hand grenade pin pull */
        if (is_local_player && strncasecmp(w, "gren_cock", 9) == 0) {
            lh_emit_weapon_recoil("GrenadePin", is_left_handed, false,
                0.35f, 0.85f, 195.0f, 20.0f, 25.0f,
                (uint8_t)LH_WAVE_IMPULSE, 0.0f, 0.0f, false);
            return true;
        }

        /* Displacer self-teleport (OpFor) */
        if (lh_str_contains_ci(w, "displacer_teleport_player")) {
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.2f, 0.0f, 1.3f,
                0.85f, 0.65f, 95.0f, 140.0f, 280.0f, (uint8_t)LH_WAVE_TRIANGLE
            );
            lh_emit_one("DisplacerTeleport", &pkt);
            return true;
        }
        return false;
    }

    /* --- ITEMS: Wall Chargers (HEV / Health), Pickups, Flashlight --- */
    if (strncasecmp(sound_name, "items/", 6) == 0) {
        const char *it = sound_name + 6;

        /* Wall HEV Suit Charger hum (emitted by wall entity func_recharge -> gate by 3D distance) */
        if (strncasecmp(it, "suitcharge1", 11) == 0 || strncasecmp(it, "suitchargeok", 12) == 0) {
            if (sound_pos != NULL && player_origin != NULL) {
                float dx = sound_pos[0] - player_origin[0];
                float dy = sound_pos[1] - player_origin[1];
                float dz = sound_pos[2] - player_origin[2];
                if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f) return false;
            }
            if (now - g_lh.last_charger_ms < 180ULL) return false;
            g_lh.last_charger_ms = now;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.25f, 0.90f, 0.85f,
                0.42f, 0.35f, 125.0f, 55.0f, 150.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("HevCharger", &pkt);
            return true;
        }

        /* Wall Health Station / Medkit (emitted by func_healthcharger or item -> gate by 3D distance) */
        if (strncasecmp(it, "medcharge", 9) == 0 ||
            strncasecmp(it, "medshot4", 8) == 0 ||
            strncasecmp(it, "smallmedkit", 11) == 0) {
            if (sound_pos != NULL && player_origin != NULL) {
                float dx = sound_pos[0] - player_origin[0];
                float dy = sound_pos[1] - player_origin[1];
                float dz = sound_pos[2] - player_origin[2];
                if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f) return false;
            }
            if (now - g_lh.last_charger_ms < 180ULL) return false;
            g_lh.last_charger_ms = now;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.30f, 0.95f, 0.70f,
                0.44f, 0.22f, 75.0f, 35.0f, 175.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("HealthCharger", &pkt);
            return true;
        }

        /* Reload clip sounds */
        if (is_local_player &&
            (strncasecmp(it, "clipinsert", 10) == 0 || strncasecmp(it, "cliprelease", 11) == 0)) {
            if (now - g_lh.last_reload_ms < 100ULL) return false;
            g_lh.last_reload_ms = now;
            lh_emit_weapon_recoil("ClipClick", is_left_handed, is_stabilised,
                0.30f, 0.80f, 170.0f, -30.0f, 24.0f,
                (uint8_t)LH_WAVE_IMPULSE, 0.0f, 0.45f, false);
            return true;
        }

        /* Flashlight toggle subtle tactile click on upper shoulder button (off-hand side) - local player only */
        if (is_local_player && strncasecmp(it, "flashlight", 10) == 0) {
            bool off_right = is_left_handed;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_UI_FEEDBACK,
                (uint8_t)LH_ZONE_TORSO,
                (uint16_t)(LH_FLAG_ACTIVE_BRAKING | LH_FLAG_SUPPRESS_EMS),
                off_right ? 0.72f : -0.72f, 0.78f, 0.96f,
                0.20f,
                0.18f, 0.82f, 175.0f, -40.0f, 28.0f, (uint8_t)LH_WAVE_IMPULSE
            );
            lh_emit_one("Flashlight", &pkt);
            return true;
        }

        return false;
    }

    /* --- PLAYER: Landing, Ladders, Swimming, Optional Footsteps (local player only) --- */
    if (strncasecmp(sound_name, "player/", 7) == 0) {
        if (!is_local_player) return false;
        const char *pl = sound_name + 7;

        /* Soft jump landing (when not triggering fall damage) */
        if (strncasecmp(pl, "pl_jumpland", 11) == 0) {
            if (now - g_lh.last_damage_ms < 120ULL) return false;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_FEET, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, LH_TCS_FEET_Y, 0.0f, 0.45f,
                0.34f, 0.40f, 52.0f, -12.0f, 75.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("JumpLand", &pkt);
            return true;
        }

        /* Ladder rungs (subtle alternating shoulder/arm climb feedback) */
        if (strncasecmp(pl, "pl_ladder", 9) == 0) {
            if (now - g_lh.last_step_ms < 220ULL) return false;
            g_lh.last_step_ms = now;
            static bool alt_hand = false;
            alt_hand = !alt_hand;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_ENVIRONMENT,
                alt_hand ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT,
                (uint16_t)LH_FLAG_SUPPRESS_EMS,
                alt_hand ? LH_TCS_ARM_X : -LH_TCS_ARM_X, LH_TCS_ARM_Y, LH_TCS_ARM_Z,
                0.20f,
                0.24f, 0.55f, 115.0f, -25.0f, 35.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("LadderStep", &pkt);
            return true;
        }

        /* Optional footsteps (opt-in via lh_footsteps 1) */
        if (footsteps_enabled &&
            (strncasecmp(pl, "pl_step", 7) == 0 ||
             strncasecmp(pl, "pl_metal", 8) == 0 ||
             strncasecmp(pl, "pl_grate", 8) == 0 ||
             strncasecmp(pl, "pl_tile", 7) == 0 ||
             strncasecmp(pl, "pl_dirt", 7) == 0 ||
             strncasecmp(pl, "pl_duct", 7) == 0)) {
            if (now - g_lh.last_step_ms < 260ULL) return false;
            g_lh.last_step_ms = now;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_FEET, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, LH_TCS_FEET_Y, 0.0f, 0.30f,
                lh_clampf(0.16f * (fvol > 0.1f ? fvol : 0.7f), 0.10f, 0.22f),
                0.35f, 65.0f, -15.0f, 32.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("Footstep", &pkt);
            return true;
        }

        return false;
    }

    /* --- ICONIC HL1 ENEMIES & RESONANCE CASCADE --- */

    /* Barnacle ceiling grab & chew (upper chest / neck constriction) */
    if (strncasecmp(sound_name, "barnacle/bcl_bite", 17) == 0 ||
        strncasecmp(sound_name, "barnacle/bcl_chew", 17) == 0) {
        if (sound_pos && player_origin) {
            float dx = sound_pos[0] - player_origin[0];
            float dy = sound_pos[1] - player_origin[1];
            float dz = sound_pos[2] - player_origin[2];
            if (dx * dx + dy * dy + dz * dz > 140.0f * 140.0f) return false;
        }
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_HEAD,
            (uint16_t)(LH_FLAG_HEADSHOT | LH_FLAG_ACTIVE_BRAKING | LH_FLAG_PREFER_EMS),
            0.0f, 1.15f, 0.45f, 0.45f,
            0.78f, 0.85f, 72.0f, -20.0f, 145.0f, (uint8_t)LH_WAVE_TRIANGLE
        );
        lh_emit_one("BarnacleBite", &pkt);
        return true;
    }

    /* Headcrab face latch/bite */
    if (strncasecmp(sound_name, "headcrab/hc_headbite", 20) == 0) {
        if (sound_pos && player_origin) {
            float dx = sound_pos[0] - player_origin[0];
            float dy = sound_pos[1] - player_origin[1];
            float dz = sound_pos[2] - player_origin[2];
            if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f) return false;
        }
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_HEAD,
            (uint16_t)(LH_FLAG_HEADSHOT | LH_FLAG_ACTIVE_BRAKING | LH_FLAG_PREFER_EMS),
            0.0f, LH_TCS_HEAD_Y, 0.90f, 0.25f,
            0.82f, 0.94f, 210.0f, -85.0f, 80.0f, (uint8_t)LH_WAVE_SINE
        );
        lh_emit_one("HeadcrabBite", &pkt);
        return true;
    }

    /* Houndeye sonic blast (directional ambient wave even if outside damage radius) */
    if (strncasecmp(sound_name, "houndeye/he_blast", 17) == 0 && sound_pos && player_origin) {
        float chest_z = player_origin[2] + ((view_ofs_z > 4.0f) ? (view_ofs_z * 0.40f) : 10.0f);
        float dx = sound_pos[0] - player_origin[0];
        float dy = sound_pos[1] - player_origin[1];
        float dz = sound_pos[2] - chest_z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > 480.0f) return false;
        LH_TcsHitPoint hit = LH_L1VR_ProjectWorldDelta(dx, dy, dz, torso_yaw_deg, false);
        float falloff = 1.0f - (dist / 480.0f);
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            hit.x * 0.7f, hit.y * 0.6f, hit.z * 0.8f, 0.90f,
            lh_clampf(0.68f * falloff, 0.16f, 0.75f),
            0.42f, 52.0f, -18.0f, 220.0f, (uint8_t)LH_WAVE_TRIANGLE
        );
        lh_emit_one("HoundeyeBlast", &pkt);
        return true;
    }

    /* Resonance Cascade / Xen Teleport surge */
    if (strncasecmp(sound_name, "ambience/port_suckin", 20) == 0 ||
        strncasecmp(sound_name, "debris/beamstart", 16) == 0) {
        if (sound_pos && player_origin) {
            float dx = sound_pos[0] - player_origin[0];
            float dy = sound_pos[1] - player_origin[1];
            float dz = sound_pos[2] - player_origin[2];
            if (dx * dx + dy * dy + dz * dz > 550.0f * 550.0f) return false;
        }
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, 0.15f, 0.20f, 1.25f,
            0.72f, 0.58f, 65.0f, 120.0f, 290.0f, (uint8_t)LH_WAVE_TRIANGLE
        );
        lh_emit_one("XenPortalSurge", &pkt);
        return true;
    }

    return false;
}

/* ============================================================================
 * Frame-Ticked Continuous Systems (Heartbeat, Egon Sustain, Long-Jump, Tram)
 * ============================================================================ */

void LH_L1VR_ProcessFrameState(
    uint64_t now_ms,
    bool in_game,
    bool is_dead,
    int health,
    int water_level,
    bool on_ground,
    const float velocity[3],
    bool is_left_handed,
    bool is_stabilised,
    bool heartbeat_enabled
) {
    if (!in_game || is_dead) {
        g_lh.egon_firing = false;
        g_lh.next_heartbeat_dub_ms = 0;
        g_lh.was_stabilised = is_stabilised;
        g_lh.was_on_ground = on_ground;
        return;
    }

    /* 1. Two-handed weapon stabilisation lock-in click (off-hand arm) */
    if (is_stabilised && !g_lh.was_stabilised) {
        bool off_right = is_left_handed;
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_UI_FEEDBACK,
            off_right ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT,
            (uint16_t)LH_FLAG_SUPPRESS_EMS,
            off_right ? LH_TCS_ARM_X : -LH_TCS_ARM_X, LH_TCS_ARM_Y, LH_TCS_ARM_Z,
            0.16f,
            0.24f, 0.70f, 165.0f, -30.0f, 22.0f, (uint8_t)LH_WAVE_IMPULSE
        );
        lh_emit_one("StabiliseLock", &pkt);
    }
    g_lh.was_stabilised = is_stabilised;

    /* 2. Gluon Gun (Egon) sustained plasma stream pulses (safe auto-timeout at 6s without refresh) */
    if (g_lh.egon_firing) {
        if (now_ms - g_lh.egon_start_ms > 6000ULL) {
            g_lh.egon_firing = false;
        } else if (now_ms - g_lh.last_egon_pulse_ms >= 68ULL) {
            g_lh.last_egon_pulse_ms = now_ms;
            lh_emit_weapon_recoil("EgonSustain", is_left_handed, is_stabilised,
                0.60f, 0.74f, 155.0f, 20.0f, 65.0f,
                (uint8_t)LH_WAVE_SQUARE, 0.56f, 0.48f, true);
        }
    }

    /* 3. Dynamic Low-HP Heartbeat (lub-dub on upper-left chest R0+R1, HP 1..30, 72..140 BPM) */
    if (heartbeat_enabled && health > 0 && health <= 30) {
        if (g_lh.next_heartbeat_dub_ms != 0 && now_ms >= g_lh.next_heartbeat_dub_ms) {
            /* Second beat ("dub") */
            LH_EventPacket dub = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                -0.30f, 0.65f, 0.95f, 0.24f,
                g_lh.pending_dub_energy, 0.26f, 44.0f, -8.0f, 110.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("HeartbeatDub", &dub);
            g_lh.next_heartbeat_dub_ms = 0;
        } else {
            /* Interval: 5 HP -> 430 ms (~140 BPM), 30 HP -> 830 ms (~72 BPM) */
            int clamped_hp = (health < 5) ? 5 : health;
            uint64_t interval_ms = (uint64_t)(430 + ((clamped_hp - 5) * 400) / 25);
            if (now_ms - g_lh.last_heartbeat_ms >= interval_ms) {
                g_lh.last_heartbeat_ms = now_ms;
                float urgency = lh_clampf((31.0f - (float)health) / 26.0f, 0.25f, 1.0f);
                float lub_e = 0.28f + 0.42f * urgency;
                g_lh.pending_dub_energy = lub_e * 0.72f;
                g_lh.next_heartbeat_dub_ms = now_ms + (uint64_t)(135 - (int)(35.0f * urgency));

                LH_EventPacket lub = lh_make_packet(
                    (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                    -0.30f, 0.65f, 0.95f, 0.26f,
                    lub_e, 0.30f, 54.0f, -10.0f, 130.0f, (uint8_t)LH_WAVE_SINE
                );
                lh_emit_one("HeartbeatLub", &lub);
            }
        }
    } else {
        g_lh.next_heartbeat_dub_ms = 0;
    }

    /* 4. Xen Long-Jump Module detection: sudden airborne horizontal burst (>430 u/s) */
    if (velocity != NULL) {
        float hspeed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
        if (!on_ground && velocity[2] > 140.0f &&
            hspeed > 430.0f && (hspeed - g_lh.prev_horiz_speed) > 170.0f &&
            (now_ms - g_lh.last_longjump_ms) > 650ULL) {
            g_lh.last_longjump_ms = now_ms;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, -0.45f, -1.0f, 0.65f,
                0.85f, 0.62f, 58.0f, -20.0f, 220.0f, (uint8_t)LH_WAVE_NOISE
            );
            lh_emit_one("XenLongJump", &pkt);
        }
        g_lh.prev_horiz_speed = hspeed;
    }
    g_lh.was_on_ground = on_ground;

    /* 5. Black Mesa Transit System tram / rail cart subtle floor rumble */
    if (g_lh.train_speed > 0 && (now_ms - g_lh.last_train_ms) >= 320ULL) {
        g_lh.last_train_ms = now_ms;
        float speed_norm = lh_clampf((float)g_lh.train_speed / 4.0f, 0.25f, 1.0f);
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, -0.60f, 0.0f, 0.90f,
            0.14f + 0.14f * speed_norm, 0.15f, 34.0f, 0.0f, 260.0f, (uint8_t)LH_WAVE_SINE
        );
        lh_emit_one("TramRumble", &pkt);
    }

    /* 6. Deep underwater pressure pulse (waterlevel == 3) */
    if (water_level >= 3 && (now_ms - g_lh.last_underwater_ms) >= 1400ULL) {
        g_lh.last_underwater_ms = now_ms;
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, 0.10f, 0.0f, 1.10f,
            0.16f, 0.12f, 32.0f, -5.0f, 320.0f, (uint8_t)LH_WAVE_SINE
        );
        lh_emit_one("UnderwaterPressure", &pkt);
    }
}

/* ============================================================================
 * UserMessage Wire Decoder (shared helper for engine & tests)
 * ============================================================================ */

static uint8_t lh_read_u8(const uint8_t *p, int size, int *off) {
    if (*off + 1 > size) return 0;
    uint8_t v = p[*off];
    (*off) += 1;
    return v;
}

static int16_t lh_read_i16(const uint8_t *p, int size, int *off) {
    if (*off + 2 > size) return 0;
    uint16_t lo = p[*off];
    uint16_t hi = p[*off + 1];
    (*off) += 2;
    return (int16_t)(lo | (hi << 8));
}

static uint32_t lh_read_u32(const uint8_t *p, int size, int *off) {
    if (*off + 4 > size) return 0;
    uint32_t v = ((uint32_t)p[*off]) |
                 ((uint32_t)p[*off + 1] << 8) |
                 ((uint32_t)p[*off + 2] << 16) |
                 ((uint32_t)p[*off + 3] << 24);
    (*off) += 4;
    return v;
}

static void lh_handle_user_message_internal(
    const char *msg_name,
    int size,
    const void *buf,
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg,
    bool is_left_handed,
    bool geiger_enabled
) {
    if (!msg_name || !buf || size <= 0) return;

    const uint8_t *p = (const uint8_t *)buf;
    int off = 0;
    uint64_t now = lh_now_ms();

    /* 1. Damage (12 bytes: u8 armor, u8 health, u32 bits, i16*3 coords / 8.0) */
    if (strcmp(msg_name, "Damage") == 0 && size >= 12) {
        int armor_dmg  = (int)lh_read_u8(p, size, &off);
        int health_dmg = (int)lh_read_u8(p, size, &off);
        uint32_t bits  = lh_read_u32(p, size, &off);
        float vec_from[3];
        vec_from[0] = (float)lh_read_i16(p, size, &off) * 0.125f;
        vec_from[1] = (float)lh_read_i16(p, size, &off) * 0.125f;
        vec_from[2] = (float)lh_read_i16(p, size, &off) * 0.125f;

        (void)LH_L1VR_ProcessDamage(
            armor_dmg, health_dmg, bits, vec_from,
            player_origin, view_ofs_z, torso_yaw_deg, g_lh.last_battery
        );
        return;
    }

    /* 2. Health (1 byte: new HP) -> healing pulse & dramatic death flatline */
    if (strcmp(msg_name, "Health") == 0 && size >= 1) {
        int new_hp = (int)lh_read_u8(p, size, &off);
        if (g_lh.last_health > 0 && new_hp == 0) {
            /* Player died */
            g_lh.egon_firing = false;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_TORSO,
                (uint16_t)(LH_FLAG_BYPASS_LIMITER | LH_FLAG_SUPPRESS_EMS),
                0.0f, 0.0f, 0.0f, 1.35f,
                1.20f, 0.45f, 30.0f, -12.0f, 520.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("PlayerDeath", &pkt);
        } else if (g_lh.last_health > 0 && new_hp > g_lh.last_health && (now - g_lh.last_charger_ms) > 160ULL) {
            /* Healed via medkit or health station */
            g_lh.last_charger_ms = now;
            int delta = new_hp - g_lh.last_health;
            float e = lh_clampf(0.28f + (float)delta * 0.025f, 0.28f, 0.65f);
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.25f, 0.95f, 0.70f,
                e, 0.22f, 76.0f, 30.0f, 160.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("HealPulse", &pkt);
        }
        g_lh.last_health = new_hp;
        return;
    }

    /* 3. Battery (2 bytes: i16 armor) -> HEV suit charge / battery pickup */
    if (strcmp(msg_name, "Battery") == 0 && size >= 2) {
        int new_bat = (int)lh_read_i16(p, size, &off);
        if (g_lh.last_battery >= 0 && new_bat > g_lh.last_battery && (now - g_lh.last_charger_ms) > 160ULL) {
            g_lh.last_charger_ms = now;
            int delta = new_bat - g_lh.last_battery;
            float e = lh_clampf(0.32f + (float)delta * 0.022f, 0.32f, 0.72f);
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.25f, 0.90f, 0.85f,
                e, 0.38f, 130.0f, 55.0f, 155.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("HevBatteryCharge", &pkt);
        }
        g_lh.last_battery = new_bat;
        return;
    }

    /* 4. Geiger (1 byte: range >> 2) -> authentic radiation counter clicks */
    if (strcmp(msg_name, "Geiger") == 0 && size >= 1) {
        if (!geiger_enabled) return;
        int range = (int)lh_read_u8(p, size, &off) << 2;
        if (range > 0 && range < 600) {
            /* Closer to radiation -> faster ticks (110ms..420ms) */
            uint64_t cooldown = (uint64_t)(110 + (range * 310) / 600);
            if (now - g_lh.last_geiger_ms >= cooldown) {
                g_lh.last_geiger_ms = now;
                float prox = lh_clampf(1.0f - ((float)range / 600.0f), 0.20f, 1.0f);
                bool right_hand = !is_left_handed;
                LH_EventPacket pkt = lh_make_packet(
                    (uint8_t)LH_BUS_UI_FEEDBACK,
                    right_hand ? (uint8_t)LH_ZONE_ARM_RIGHT : (uint8_t)LH_ZONE_ARM_LEFT,
                    (uint16_t)LH_FLAG_SUPPRESS_EMS,
                    right_hand ? 0.55f : -0.55f, 0.30f, 0.90f,
                    0.22f,
                    0.18f + 0.30f * prox,
                    0.75f,
                    220.0f + 80.0f * prox,
                    -40.0f,
                    22.0f,
                    (uint8_t)LH_WAVE_IMPULSE
                );
                lh_emit_one("GeigerClick", &pkt);
            }
        }
        return;
    }

    /* 5. ScreenShake (6 bytes: i16 amp 4.12, i16 dur 4.12, i16 freq 8.8) */
    if (strcmp(msg_name, "ScreenShake") == 0 && size >= 6) {
        if (now - g_lh.last_shake_ms < 95ULL) return;
        float amp  = (float)lh_read_i16(p, size, &off) / 4096.0f;
        float dur  = (float)lh_read_i16(p, size, &off) / 4096.0f;
        float freq = (float)lh_read_i16(p, size, &off) / 256.0f;
        if (amp <= 0.1f || dur <= 0.02f) return;

        g_lh.last_shake_ms = now;
        float energy = lh_clampf(amp / 12.0f, 0.22f, 1.05f);
        float decay_ms = lh_clampf(dur * 600.0f, 120.0f, 650.0f);
        float base_hz = lh_clampf(24.0f + freq * 0.4f, 24.0f, 55.0f);

        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, -0.10f, 0.0f, 1.25f,
            energy, 0.30f, base_hz, -8.0f, decay_ms, (uint8_t)LH_WAVE_NOISE
        );
        lh_emit_one("ScreenShake", &pkt);
        return;
    }

    /* 6. CurWeapon (3 bytes: u8 state, u8 id, u8 clip) -> detects grenade/satchel throws & weapon switches */
    if (strcmp(msg_name, "CurWeapon") == 0 && size >= 3) {
        int state = (int)lh_read_u8(p, size, &off);
        int id    = (int)lh_read_u8(p, size, &off);
        int clip  = (int)lh_read_u8(p, size, &off);
        if (state == 1) {
            if (g_lh.cur_weapon_id != 0 && id != g_lh.cur_weapon_id) {
                /* Weapon changed -> stop any Egon sustain & send subtle holster click */
                g_lh.egon_firing = false;
                bool right_hand = !is_left_handed;
                LH_EventPacket pkt = lh_make_packet(
                    (uint8_t)LH_BUS_UI_FEEDBACK, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                    right_hand ? 0.55f : -0.55f, -0.45f, 0.85f, 0.18f,
                    0.26f, 0.65f, 140.0f, -20.0f, 26.0f, (uint8_t)LH_WAVE_IMPULSE
                );
                lh_emit_one("WeaponSwitch", &pkt);
            } else if (id == g_lh.cur_weapon_id && clip >= 0 && g_lh.cur_weapon_clip >= 0 && clip < g_lh.cur_weapon_clip) {
                /* Weapons that don't fire .sc events (Hand Grenade / Satchel) */
                if (id == LH_HL_WEAPON_HANDGRENADE || id == LH_HL_WEAPON_SATCHEL) {
                    lh_emit_weapon_recoil("ThrowExplosive", is_left_handed, false,
                        0.45f, 0.65f, 110.0f, -25.0f, 48.0f,
                        (uint8_t)LH_WAVE_SINE, 0.15f, 0.0f, false);
                }
            }
            g_lh.cur_weapon_id = id;
            g_lh.cur_weapon_clip = clip;
        }
        return;
    }

    /* 7. ItemPickup / WeapPickup / AmmoPickup */
    if (strcmp(msg_name, "ItemPickup") == 0 ||
        strcmp(msg_name, "WeapPickup") == 0 ||
        strcmp(msg_name, "AmmoPickup") == 0) {
        bool is_hev_suit = false;
        if (strcmp(msg_name, "ItemPickup") == 0 && size >= 9) {
            char item_buf[64];
            size_t copy_len = (size < 63) ? (size_t)size : 63u;
            memcpy(item_buf, buf, copy_len);
            item_buf[copy_len] = '\0';
            is_hev_suit = lh_str_contains_ci(item_buf, "item_suit");
        }
        if (is_hev_suit) {
            /* Iconic HEV Mark IV Suit equip wave across full chest & back! */
            LH_EventPacket batch[2];
            batch[0] = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.30f, 1.0f, 0.95f,
                0.78f, 0.45f, 95.0f, 95.0f, 380.0f, (uint8_t)LH_WAVE_SINE
            );
            batch[1] = lh_make_packet(
                (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                0.0f, 0.25f, -1.0f, 0.95f,
                0.68f, 0.45f, 85.0f, 85.0f, 360.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_batch("HevSuitEquip", batch, 2);
        } else {
            bool right_hand = !is_left_handed;
            LH_EventPacket pkt = lh_make_packet(
                (uint8_t)LH_BUS_UI_FEEDBACK, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
                right_hand ? 0.50f : -0.50f, -0.40f, 0.90f, 0.20f,
                0.32f, 0.60f, 135.0f, 20.0f, 38.0f, (uint8_t)LH_WAVE_SINE
            );
            lh_emit_one("ItemPickup", &pkt);
        }
        return;
    }

    /* 8. Train (1 byte: tram/rail speed 0..4) */
    if (strcmp(msg_name, "Train") == 0 && size >= 1) {
        g_lh.train_speed = (int)lh_read_u8(p, size, &off);
        return;
    }
}

/* ============================================================================
 * Interactive Test Effects (lh_test <effect>)
 * ============================================================================ */

bool LH_L1VR_TriggerTestEffect(const char *effect_name, bool is_left_handed, bool is_stabilised) {
    if (!effect_name || effect_name[0] == '\0') return false;

    const float origin[3] = { 0.0f, 0.0f, 0.0f };

    if (strcasecmp(effect_name, "front") == 0) {
        const float from[3] = { 100.0f, 0.0f, 10.0f };
        return LH_L1VR_ProcessDamage(0, 22, LH_DMG_BULLET, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "back") == 0) {
        const float from[3] = { -100.0f, 0.0f, 10.0f };
        return LH_L1VR_ProcessDamage(0, 22, LH_DMG_BULLET, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "left") == 0) {
        const float from[3] = { 0.0f, 100.0f, 10.0f };
        return LH_L1VR_ProcessDamage(0, 22, LH_DMG_BULLET, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "right") == 0) {
        const float from[3] = { 0.0f, -100.0f, 10.0f };
        return LH_L1VR_ProcessDamage(0, 22, LH_DMG_BULLET, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "headshot") == 0) {
        const float from[3] = { 60.0f, 0.0f, 36.0f };
        return LH_L1VR_ProcessDamage(0, 35, LH_DMG_BULLET, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "shock") == 0) {
        const float from[3] = { 80.0f, 0.0f, 12.0f };
        return LH_L1VR_ProcessDamage(0, 28, LH_DMG_SHOCK, from, origin, 28.0f, 0.0f, 0);
    }
    if (strcasecmp(effect_name, "explosion") == 0) {
        const float exp_pos[3] = { 120.0f, 40.0f, 10.0f };
        g_lh.last_explosion_ms = 0;
        return LH_L1VR_ProcessExplosion(exp_pos, 3.0f, origin, 28.0f, 0.0f);
    }
    if (strcasecmp(effect_name, "crowbar") == 0) {
        return LH_L1VR_ProcessSound("weapons/cbar_hitbod1.wav", origin, true, origin, 28.0f, 0.0f,
                                    is_left_handed, is_stabilised, false, 1.0f);
    }
    if (strcasecmp(effect_name, "glock") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/glock1.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "mp5") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/mp5.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "shotgun") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/shotgun1.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "doublebarrel") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/shotgun2.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "magnum") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/python.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "gauss") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/gauss.sc", 180.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "egon") == 0) {
        bool ok = LH_L1VR_ProcessWeaponEvent("events/egon_fire.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
        g_lh.egon_firing = false;
        return ok;
    }
    if (strcasecmp(effect_name, "rpg") == 0) {
        return LH_L1VR_ProcessWeaponEvent("events/rpg.sc", 0.0f, 0, 0, 0, is_left_handed, is_stabilised);
    }
    if (strcasecmp(effect_name, "hev") == 0) {
        const char suit_msg[] = "item_suit";
        lh_handle_user_message_internal("ItemPickup", (int)sizeof(suit_msg), suit_msg, origin, 28.0f, 0.0f, is_left_handed, true);
        return true;
    }
    if (strcasecmp(effect_name, "geiger") == 0) {
        uint8_t msg[1] = { 20 };
        g_lh.last_geiger_ms = 0;
        lh_handle_user_message_internal("Geiger", 1, msg, origin, 28.0f, 0.0f, is_left_handed, true);
        return true;
    }
    if (strcasecmp(effect_name, "heartbeat") == 0) {
        LH_EventPacket lub = lh_make_packet(
            (uint8_t)LH_BUS_PLAYER_VITALS, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            -0.30f, 0.65f, 0.95f, 0.26f,
            0.65f, 0.30f, 54.0f, -10.0f, 130.0f, (uint8_t)LH_WAVE_SINE
        );
        lh_emit_one("TestHeartbeat", &lub);
        return true;
    }
    if (strcasecmp(effect_name, "longjump") == 0) {
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
            0.0f, -0.45f, -1.0f, 0.65f,
            0.85f, 0.62f, 58.0f, -20.0f, 220.0f, (uint8_t)LH_WAVE_NOISE
        );
        lh_emit_one("TestLongJump", &pkt);
        return true;
    }
    if (strcasecmp(effect_name, "death") == 0) {
        LH_EventPacket pkt = lh_make_packet(
            (uint8_t)LH_BUS_CRITICAL_IMPACT, (uint8_t)LH_ZONE_TORSO,
            (uint16_t)(LH_FLAG_BYPASS_LIMITER | LH_FLAG_SUPPRESS_EMS),
            0.0f, 0.0f, 0.0f, 1.35f,
            1.20f, 0.45f, 30.0f, -12.0f, 520.0f, (uint8_t)LH_WAVE_SINE
        );
        lh_emit_one("TestDeath", &pkt);
        return true;
    }

    return false;
}

void LH_L1VR_ResetState(void) {
    bool was_init = g_lh.initialized;
    bool was_dbg = g_lh.debug_logging;
    memset(&g_lh, 0, sizeof(g_lh));
    g_lh.initialized = was_init;
    g_lh.debug_logging = was_dbg;
    g_lh.last_health = 100;
    g_lh.last_battery = -1;
}

/* ============================================================================
 * Xash3D-FWGS / Lambda1VR Engine Glue (excluded when LH_STANDALONE_TEST is set)
 * ============================================================================ */

#ifndef LH_STANDALONE_TEST

static bool lh_is_left_handed_engine(void) {
    return (vr_control_scheme != NULL && vr_control_scheme->integer >= 10);
}

static bool lh_is_stabilised_engine(void) {
    return (vr_weapon_stabilised != NULL && vr_weapon_stabilised->integer != 0);
}

static float lh_get_torso_yaw_deg(void) {
    /* cl.refdef.cl_viewangles[YAW] tracks the player's VR heading in GoldSrc world degrees */
    return cl.refdef.cl_viewangles[YAW];
}

static void lh_sync_cvars_if_needed(void) {
    if (!g_lh.initialized) return;

    if (lh_enable) {
        lh_udp_set_enabled(lh_enable->integer != 0);
    }
    if (lh_intensity) {
        lh_udp_set_intensity(lh_intensity->value);
    }
    if (lh_debug) {
        g_lh.debug_logging = (lh_debug->integer != 0);
    }

    const char *want_host = (lh_host && lh_host->string && lh_host->string[0]) ? lh_host->string : LH_UDP_DEFAULT_HOST;
    int want_port = (lh_port && lh_port->integer > 0 && lh_port->integer <= 65535) ? lh_port->integer : LH_UDP_DEFAULT_PORT;

    if (want_port != g_lh_synced_port || strcmp(want_host, g_lh_synced_host) != 0) {
        if (lh_udp_configure(want_host, (uint16_t)want_port)) {
            snprintf(g_lh_synced_host, sizeof(g_lh_synced_host), "%s", want_host);
            g_lh_synced_port = want_port;
        }
    }
}

static void LH_Status_f(void) {
    lh_sync_cvars_if_needed();
    LH_UdpStats st;
    lh_udp_get_stats(&st);
    Msg("LibreHaptics (Lambda1VR) Status:\n");
    Msg("  Enabled:   %s | Socket: %s\n", st.enabled ? "YES" : "NO", st.socket_ready ? "READY" : "CLOSED");
    Msg("  Endpoint:  %s:%u (UDP 64B LH_EventPacket)\n", st.host, (unsigned)st.port);
    Msg("  Intensity: %.2f | Hand: %s | 2H Stabilised: %s\n",
        st.intensity_scale,
        lh_is_left_handed_engine() ? "LEFT" : "RIGHT",
        lh_is_stabilised_engine() ? "YES" : "NO");
    Msg("  Packets:   TX=%llu | Errors=%llu | HP=%d | HEV=%d\n",
        (unsigned long long)st.packets_sent,
        (unsigned long long)st.send_errors,
        g_lh.last_health,
        g_lh.last_battery < 0 ? 0 : g_lh.last_battery);
}

static void LH_Test_f(void) {
    lh_sync_cvars_if_needed();
    if (Cmd_Argc() < 2) {
        Msg("Usage: lh_test <front|back|left|right|headshot|shock|explosion|crowbar|glock|mp5|shotgun|doublebarrel|magnum|gauss|egon|rpg|hev|geiger|heartbeat|longjump|death>\n");
        return;
    }
    const char *eff = Cmd_Argv(1);
    if (LH_L1VR_TriggerTestEffect(eff, lh_is_left_handed_engine(), lh_is_stabilised_engine())) {
        Msg("[LibreHaptics] Triggered test effect: %s\n", eff);
    } else {
        Msg("[LibreHaptics] Unknown test effect: %s\n", eff);
    }
}

void LH_L1VR_Init(void) {
    if (g_lh.initialized) return;

    LH_L1VR_ResetState();

    lh_enable    = Cvar_Get("lh_enable",    "1",         CVAR_ARCHIVE, "Enable LibreHaptics UDP telemetry");
    lh_host      = Cvar_Get("lh_host",      "127.0.0.1", CVAR_ARCHIVE, "LibreHaptics Player Lite UDP host");
    lh_port      = Cvar_Get("lh_port",      "19800",     CVAR_ARCHIVE, "LibreHaptics Player Lite UDP port");
    lh_intensity = Cvar_Get("lh_intensity", "1.0",       CVAR_ARCHIVE, "LibreHaptics global intensity multiplier (0.0..2.0)");
    lh_heartbeat = Cvar_Get("lh_heartbeat", "1",         CVAR_ARCHIVE, "Enable low-HP dynamic heartbeat haptics");
    lh_geiger    = Cvar_Get("lh_geiger",    "1",         CVAR_ARCHIVE, "Enable Geiger counter radiation ticks");
    lh_footsteps = Cvar_Get("lh_footsteps", "0",         CVAR_ARCHIVE, "Enable subtle footstep haptics on feet zone");
    lh_debug     = Cvar_Get("lh_debug",     "0",         CVAR_ARCHIVE, "Log outgoing LibreHaptics UDP packets");

    Cmd_AddCommand("lh_status", LH_Status_f, "Show LibreHaptics telemetry status");
    Cmd_AddCommand("lh_test",   LH_Test_f,   "Send a test LibreHaptics haptic pulse");

    g_lh.initialized = true;
    lh_sync_cvars_if_needed();

    ALOGV("[LibreHaptics] Initialized UDP bridge -> %s:%d", g_lh_synced_host, g_lh_synced_port);
}

void LH_L1VR_Shutdown(void) {
    if (!g_lh.initialized) return;
    g_lh.egon_firing = false;
    g_lh.initialized = false;
    lh_udp_shutdown();
}

void LH_L1VR_OnFrame(void) {
    if (!g_lh.initialized) return;

    lh_sync_cvars_if_needed();

    bool in_game = (cls.state == ca_active) && !VR_UseScreenLayer();
    bool dead    = isPlayerDead();
    int hp       = (int)cl.frame.client.health;
    int wlevel   = cl.frame.client.waterlevel;
    bool on_gnd  = (cl.frame.client.flags & FL_ONGROUND) != 0;
    bool hb_on   = (lh_heartbeat == NULL || lh_heartbeat->integer != 0);

    LH_L1VR_ProcessFrameState(
        lh_now_ms(),
        in_game,
        dead,
        hp,
        wlevel,
        on_gnd,
        cl.frame.client.velocity,
        lh_is_left_handed_engine(),
        lh_is_stabilised_engine(),
        hb_on
    );
}

void LH_L1VR_OnUserMessage(const char *msg_name, int size, const void *buf) {
    if (!g_lh.initialized || !msg_name || !buf || size <= 0) return;

    bool geiger_on = (lh_geiger == NULL || lh_geiger->integer != 0);
    lh_handle_user_message_internal(
        msg_name,
        size,
        buf,
        cl.frame.client.origin,
        cl.frame.client.view_ofs[2],
        lh_get_torso_yaw_deg(),
        lh_is_left_handed_engine(),
        geiger_on
    );
}

void LH_L1VR_OnClientEvent(const char *event_name, const struct event_args_s *args) {
    if (!g_lh.initialized || !event_name || !args) return;

    /* Only emit weapon recoil for the local VR player */
    if (args->entindex != (cl.playernum + 1)) {
        return;
    }

    (void)LH_L1VR_ProcessWeaponEvent(
        event_name,
        args->fparam1,
        args->iparam1,
        args->bparam1,
        args->bparam2,
        lh_is_left_handed_engine(),
        lh_is_stabilised_engine()
    );
}

void LH_L1VR_OnExplosion(const float pos[3], float scale) {
    if (!g_lh.initialized || !pos) return;
    if (cls.state != ca_active) return;

    (void)LH_L1VR_ProcessExplosion(
        pos,
        scale,
        cl.frame.client.origin,
        cl.frame.client.view_ofs[2],
        lh_get_torso_yaw_deg()
    );
}

void LH_L1VR_OnTeleport(const float pos[3]) {
    if (!g_lh.initialized || !pos) return;
    if (cls.state != ca_active) return;

    float dx = pos[0] - cl.frame.client.origin[0];
    float dy = pos[1] - cl.frame.client.origin[1];
    float dz = pos[2] - cl.frame.client.origin[2];
    if (dx * dx + dy * dy + dz * dz > 260.0f * 260.0f) return;

    LH_EventPacket pkt = lh_make_packet(
        (uint8_t)LH_BUS_ENVIRONMENT, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
        0.0f, 0.20f, 0.0f, 1.15f,
        0.62f, 0.55f, 90.0f, 125.0f, 220.0f, (uint8_t)LH_WAVE_TRIANGLE
    );
    lh_emit_one("Teleport", &pkt);
}

void LH_L1VR_OnSound(const char *sound_name, const float pos[3], int entnum, float fvol) {
    if (!g_lh.initialized || !sound_name) return;
    if (cls.state != ca_active) return;

    bool is_local = (entnum == (cl.playernum + 1)) || (entnum == 0);
    bool footsteps_on = (lh_footsteps != NULL && lh_footsteps->integer != 0);

    (void)LH_L1VR_ProcessSound(
        sound_name,
        pos,
        is_local,
        cl.frame.client.origin,
        cl.frame.client.view_ofs[2],
        lh_get_torso_yaw_deg(),
        lh_is_left_handed_engine(),
        lh_is_stabilised_engine(),
        footsteps_on,
        fvol
    );
}

void LH_L1VR_OnBackpackHover(bool is_off_hand) {
    if (!g_lh.initialized) return;
    uint64_t now = lh_now_ms();
    if (now - g_lh.last_backpack_hover_ms < 250ULL) return;
    g_lh.last_backpack_hover_ms = now;

    bool left = lh_is_left_handed_engine();
    bool on_right_shoulder = is_off_hand ? left : !left;
    LH_EventPacket pkt = lh_make_packet(
        (uint8_t)LH_BUS_UI_FEEDBACK, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
        on_right_shoulder ? 0.55f : -0.55f, 0.65f, LH_TCS_SHOULDER_BACK_Z,
        0.22f,
        0.28f, 0.55f, 145.0f, -20.0f, 35.0f, (uint8_t)LH_WAVE_SINE
    );
    lh_emit_one("BackpackHover", &pkt);
}

void LH_L1VR_OnBackpackAction(bool draw_weapon) {
    if (!g_lh.initialized) return;
    bool right_shoulder = !lh_is_left_handed_engine();
    LH_EventPacket pkt = lh_make_packet(
        (uint8_t)LH_BUS_UI_FEEDBACK, (uint8_t)LH_ZONE_TORSO, (uint16_t)LH_FLAG_SUPPRESS_EMS,
        right_shoulder ? 0.55f : -0.55f, 0.60f, LH_TCS_SHOULDER_BACK_Z,
        0.28f,
        draw_weapon ? 0.52f : 0.36f,
        0.72f,
        draw_weapon ? 150.0f : 115.0f,
        draw_weapon ? 35.0f : -35.0f,
        55.0f,
        (uint8_t)LH_WAVE_TRIANGLE
    );
    lh_emit_one(draw_weapon ? "BackpackDraw" : "BackpackHolster", &pkt);
}

#else /* LH_STANDALONE_TEST */

void LH_L1VR_Init(void) {
    LH_L1VR_ResetState();
    g_lh.initialized = true;
}

void LH_L1VR_Shutdown(void) {
    g_lh.initialized = false;
    lh_udp_shutdown();
}

void LH_L1VR_OnFrame(void) {}
void LH_L1VR_OnUserMessage(const char *msg_name, int size, const void *buf) {
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    lh_handle_user_message_internal(msg_name, size, buf, origin, 28.0f, 0.0f, false, true);
}
void LH_L1VR_OnClientEvent(const char *event_name, const struct event_args_s *args) {
    (void)event_name; (void)args;
}
void LH_L1VR_OnExplosion(const float pos[3], float scale) { (void)pos; (void)scale; }
void LH_L1VR_OnTeleport(const float pos[3]) { (void)pos; }
void LH_L1VR_OnSound(const char *sound_name, const float pos[3], int entnum, float fvol) {
    (void)sound_name; (void)pos; (void)entnum; (void)fvol;
}
void LH_L1VR_OnBackpackHover(bool is_off_hand) { (void)is_off_hand; }
void LH_L1VR_OnBackpackAction(bool draw_weapon) { (void)draw_weapon; }

#endif /* LH_STANDALONE_TEST */
