/**
 * LibreHaptics (Loom Engine) Spatial Telemetry & Synthesis Bridge for Lambda1VR
 * File: VrLibreHaptics.h
 *
 * Engine-level Xash3D-FWGS + OpenXR integration supporting:
 *   - Half-Life 1 (valve), Blue Shift (bshift), Opposing Force (gearbox),
 *     Afraid of Monsters (aomdc), They Hunger (hunger), and custom GoldSrc mods.
 *   - Sends 64-byte LH_EventPacket frames via non-blocking UDP (127.0.0.1:19800)
 *     to LibreHaptics Player Lite (:loom_service).
 */

#ifndef VR_LIBREHAPTICS_H
#define VR_LIBREHAPTICS_H

#include <stdint.h>
#include <stdbool.h>
#include "lh_udp_client.h"

#ifdef __cplusplus
extern "C" {
#endif

struct event_args_s;

/* ============================================================================
 * Spatial Projection Types (GoldSrc 3D World -> LibreHaptics TCS)
 * ============================================================================ */

typedef struct {
    float   x;
    float   y;
    float   z;
    uint8_t zone_hint;
    bool    is_headshot;
} LH_TcsHitPoint;

/**
 * Projects a GoldSrc world-space delta (sourcePos - playerChestPos, +Z up)
 * onto the LibreHaptics Torso Coordinate System (TCS: -X Left / +X Right,
 * -Y Waist / +Y Head, -Z Back / +Z Front) using horizontal torso yaw only.
 */
LH_TcsHitPoint LH_L1VR_ProjectWorldDelta(
    float dx,
    float dy,
    float dz,
    float torso_yaw_deg,
    bool has_exact_height
);

/* ============================================================================
 * Engine Lifecycle & Hooks (called from libxash.so)
 * ============================================================================ */

void LH_L1VR_Init(void);
void LH_L1VR_Shutdown(void);
void LH_L1VR_OnFrame(void);

void LH_L1VR_OnUserMessage(const char *msg_name, int size, const void *buf);
void LH_L1VR_OnClientEvent(const char *event_name, const struct event_args_s *args);
void LH_L1VR_OnExplosion(const float pos[3], float scale);
void LH_L1VR_OnTeleport(const float pos[3]);
void LH_L1VR_OnSound(const char *sound_name, const float pos[3], int entnum, float fvol);
void LH_L1VR_OnBackpackHover(bool is_off_hand);
void LH_L1VR_OnBackpackAction(bool draw_weapon);

/* ============================================================================
 * Pure Testable Entry Points (shared by engine hooks and host unit tests)
 * ============================================================================ */

void LH_L1VR_ResetState(void);

bool LH_L1VR_ProcessDamage(
    int armor_dmg,
    int health_dmg,
    uint32_t bits_damage,
    const float vec_from[3],
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg,
    int prev_armor_value
);

bool LH_L1VR_ProcessWeaponEvent(
    const char *event_name,
    float fparam1,
    int iparam1,
    int bparam1,
    int bparam2,
    bool is_left_handed,
    bool is_stabilised
);

bool LH_L1VR_ProcessExplosion(
    const float explosion_pos[3],
    float scale,
    const float player_origin[3],
    float view_ofs_z,
    float torso_yaw_deg
);

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
);

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
);

bool LH_L1VR_TriggerTestEffect(const char *effect_name, bool is_left_handed, bool is_stabilised);

#ifdef __cplusplus
}
#endif

#endif /* VR_LIBREHAPTICS_H */
