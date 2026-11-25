/**
 * Native C99 Unit & UDP Loopback Integration Test Suite for Lambda1VR LibreHaptics
 *
 * Verifies:
 *  1. Strict 64-byte LH_EventPacket ABI v1.1 & byte offsets (including PREFER_EMS / SUPPRESS_EMS)
 *  2. GoldSrc 3D (+Z up) -> pure TCS unit cylinder (ux, y, uz) projection across all directions & VR yaw angles
 *  3. Single directional 64-byte packet on lateral flank hits (engine handles flank spanning)
 *  4. 12-byte GoldSrc Damage UserMsg binary decoding & all DMG_* synthesis profiles + HEV armor + EMS flags
 *  5. All HL1 & OpFor weapon fire events (L/R handedness, 2H stabilisation, Gauss, Egon, RPG, SPAS-12 2x)
 *  6. Sound cues (physical VR crowbar flesh vs metal, wall chargers, Barnacle, Headcrab, Houndeye, footsteps)
 *  7. 3D explosions, ScreenShake, Geiger, HEV suit equip, Heartbeat lub-dub, Xen Long-Jump
 *  8. Real non-blocking UDP socket transmission to loopback receiver
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "VrLibreHaptics.h"

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define EXPECT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] %s (line %d): %s\n", __func__, __LINE__, (msg)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define EXPECT_NEAR(val, exp, tol, msg) do { \
    float _v = (float)(val), _e = (float)(exp), _t = (float)(tol); \
    if (fabsf(_v - _e) > _t) { \
        fprintf(stderr, "  [FAIL] %s (line %d): %s (got %.3f, expected %.3f +/- %.3f)\n", \
                __func__, __LINE__, (msg), _v, _e, _t); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

static int g_rx_fd = -1;
static uint16_t g_rx_port = 0;

static void drain_rx(void) {
    uint8_t tmp[1024];
    while (recv(g_rx_fd, tmp, sizeof(tmp), MSG_DONTWAIT) > 0) {}
}

static size_t recv_packets(LH_EventPacket *out, size_t max_pkts) {
    uint8_t buf[LH_PACKET_SIZE_BYTES * 16];
    ssize_t n = recv(g_rx_fd, buf, sizeof(buf), 0);
    if (n <= 0 || (n % LH_PACKET_SIZE_BYTES) != 0) {
        return 0;
    }
    size_t count = (size_t)n / LH_PACKET_SIZE_BYTES;
    if (count > max_pkts) count = max_pkts;
    memcpy(out, buf, count * LH_PACKET_SIZE_BYTES);
    return count;
}

static void setup_loopback_rx(void) {
    g_rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_fd < 0) {
        perror("socket");
        exit(1);
    }
    struct timeval tv = { 0, 250000 }; /* 250ms timeout */
    setsockopt(g_rx_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral port */

    if (bind(g_rx_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        exit(1);
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(g_rx_fd, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname");
        exit(1);
    }
    g_rx_port = ntohs(addr.sin_port);

    if (!lh_udp_init("127.0.0.1", g_rx_port)) {
        fprintf(stderr, "Failed to initialize lh_udp_init\n");
        exit(1);
    }
    lh_udp_set_enabled(true);
    lh_udp_set_intensity(1.0f);
    LH_L1VR_Init();
}

static void test_abi_layout(void) {
    EXPECT_TRUE(sizeof(LH_EventPacket) == 64, "LH_EventPacket size must be 64");
    EXPECT_TRUE(offsetof(LH_EventPacket, target_display_time_ns) == 0x00, "offset 0x00");
    EXPECT_TRUE(offsetof(LH_EventPacket, sequence_id) == 0x08, "offset 0x08");
    EXPECT_TRUE(offsetof(LH_EventPacket, bus_id) == 0x0C, "offset 0x0C");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload_type) == 0x0D, "offset 0x0D");
    EXPECT_TRUE(offsetof(LH_EventPacket, zone_hint) == 0x0E, "offset 0x0E");
    EXPECT_TRUE(offsetof(LH_EventPacket, reserved_byte) == 0x0F, "offset 0x0F");
    EXPECT_TRUE(offsetof(LH_EventPacket, flags) == 0x10, "offset 0x10");
    EXPECT_TRUE(offsetof(LH_EventPacket, reserved_flags) == 0x12, "offset 0x12");
    EXPECT_TRUE(offsetof(LH_EventPacket, entry_pos) == 0x14, "offset 0x14");
    EXPECT_TRUE(offsetof(LH_EventPacket, spread_radius) == 0x20, "offset 0x20");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.energy) == 0x24, "offset 0x24");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.attack_sharpness) == 0x28, "offset 0x28");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.base_freq_hz) == 0x2C, "offset 0x2C");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.freq_glide_hz) == 0x30, "offset 0x30");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.decay_ms) == 0x34, "offset 0x34");
    EXPECT_TRUE(offsetof(LH_EventPacket, payload.synth.wave_shape) == 0x38, "offset 0x38");
    EXPECT_TRUE(LH_FLAG_PREFER_EMS == 0x0010, "LH_FLAG_PREFER_EMS == 0x0010");
    EXPECT_TRUE(LH_FLAG_SUPPRESS_EMS == 0x0020, "LH_FLAG_SUPPRESS_EMS == 0x0020");
    g_tests_passed++;
}

static void test_tcs_projection(void) {
    /* Facing +X (yaw = 0 deg): front = +X, right = -Y, left = +Y, back = -X */
    LH_TcsHitPoint front = LH_L1VR_ProjectWorldDelta(100.0f, 0.0f, 0.0f, 0.0f, true);
    EXPECT_NEAR(front.x, 0.0f, 0.01f, "front X");
    EXPECT_NEAR(front.z, 1.0f, 0.01f, "front Z");
    EXPECT_TRUE(front.zone_hint == LH_ZONE_TORSO, "front torso");

    LH_TcsHitPoint back = LH_L1VR_ProjectWorldDelta(-100.0f, 0.0f, 0.0f, 0.0f, true);
    EXPECT_NEAR(back.x, 0.0f, 0.01f, "back X");
    EXPECT_NEAR(back.z, -1.0f, 0.01f, "back Z");

    /* Pure unit cylinder side hits: (+1, y, 0) and (-1, y, 0) without Z flattening */
    LH_TcsHitPoint right_side = LH_L1VR_ProjectWorldDelta(0.0f, -100.0f, 0.0f, 0.0f, true);
    EXPECT_NEAR(right_side.x, 1.0f, 0.01f, "right flank pure cylinder X=+1.0");
    EXPECT_NEAR(right_side.z, 0.0f, 0.01f, "right flank pure cylinder Z=0.0");

    LH_TcsHitPoint left_side = LH_L1VR_ProjectWorldDelta(0.0f, 100.0f, 0.0f, 0.0f, true);
    EXPECT_NEAR(left_side.x, -1.0f, 0.01f, "left flank pure cylinder X=-1.0");
    EXPECT_NEAR(left_side.z, 0.0f, 0.01f, "left flank pure cylinder Z=0.0");

    /* 45-degree front-right diagonal: (cos45, y, sin45) without snapping Z to +/-1 */
    LH_TcsHitPoint diag = LH_L1VR_ProjectWorldDelta(100.0f, -100.0f, 0.0f, 0.0f, true);
    EXPECT_NEAR(diag.x, 0.7071f, 0.01f, "45-deg front-right X");
    EXPECT_NEAR(diag.z, 0.7071f, 0.01f, "45-deg front-right Z");

    /* Facing +Y (yaw = 90 deg): front = +Y, right = +X */
    LH_TcsHitPoint yaw90_front = LH_L1VR_ProjectWorldDelta(0.0f, 100.0f, 0.0f, 90.0f, true);
    EXPECT_NEAR(yaw90_front.z, 1.0f, 0.01f, "yaw90 front Z");
    EXPECT_NEAR(yaw90_front.x, 0.0f, 0.01f, "yaw90 front X");

    LH_TcsHitPoint yaw90_right = LH_L1VR_ProjectWorldDelta(100.0f, 0.0f, 0.0f, 90.0f, true);
    EXPECT_NEAR(yaw90_right.x, 1.0f, 0.01f, "yaw90 right flank X=+1.0");
    EXPECT_NEAR(yaw90_right.z, 0.0f, 0.01f, "yaw90 right flank Z=0.0");

    /* Headshot & feet elevation */
    LH_TcsHitPoint head = LH_L1VR_ProjectWorldDelta(60.0f, 0.0f, 25.0f, 0.0f, true);
    EXPECT_TRUE(head.zone_hint == LH_ZONE_HEAD && head.is_headshot && head.y > 1.2f, "headshot");

    LH_TcsHitPoint feet = LH_L1VR_ProjectWorldDelta(40.0f, 0.0f, -30.0f, 0.0f, true);
    EXPECT_TRUE(feet.zone_hint == LH_ZONE_FEET && feet.y < -1.0f, "feet");

    g_tests_passed++;
}

static void test_damage_wire_and_types(void) {
    LH_L1VR_ResetState();
    drain_rx();

    /* 1. Encode 12-byte GoldSrc Damage message: Right flank bullet hit at (0, -80, 10) -> coord * 8 */
    uint8_t wire[12];
    wire[0] = 5;  /* armor */
    wire[1] = 20; /* health */
    uint32_t bits = (1u << 1); /* DMG_BULLET */
    wire[2] = (uint8_t)(bits & 0xFF);
    wire[3] = (uint8_t)((bits >> 8) & 0xFF);
    wire[4] = (uint8_t)((bits >> 16) & 0xFF);
    wire[5] = (uint8_t)((bits >> 24) & 0xFF);
    int16_t cx = 0 * 8;
    int16_t cy = -80 * 8;
    int16_t cz = 10 * 8;
    wire[6]  = (uint8_t)(cx & 0xFF); wire[7]  = (uint8_t)((cx >> 8) & 0xFF);
    wire[8]  = (uint8_t)(cy & 0xFF); wire[9]  = (uint8_t)((cy >> 8) & 0xFF);
    wire[10] = (uint8_t)(cz & 0xFF); wire[11] = (uint8_t)((cz >> 8) & 0xFF);

    LH_L1VR_OnUserMessage("Damage", 12, wire);

    LH_EventPacket rx[8];
    size_t n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1, "Lateral flank bullet must emit 1 clean directional packet (64B)");
    EXPECT_TRUE(rx[0].bus_id == LH_BUS_CRITICAL_IMPACT, "CriticalImpact bus");
    EXPECT_NEAR(rx[0].entry_pos[0], 1.0f, 0.02f, "Right flank pure cylinder X=+1.0");
    EXPECT_NEAR(rx[0].entry_pos[2], 0.0f, 0.02f, "Right flank pure cylinder Z=0.0");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PENETRATION) != 0, "Penetration flag");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Bullet damage explicitly sets PREFER_EMS");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_SUPPRESS_EMS) == 0, "Bullet damage does not set SUPPRESS_EMS");

    /* 2. Blast damage: pure vibro (SUPPRESS_EMS), low sub-bass freq, bypass limiter */
    const float origin[3] = { 0, 0, 0 };
    const float from_front[3] = { 90, 0, 10 };
    LH_L1VR_ProcessDamage(0, 45, (1u << 6), from_front, origin, 28.0f, 0.0f, 0);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1, "Blast single volumetric packet");
    EXPECT_TRUE(rx[0].payload.synth.base_freq_hz < 35.0f, "Sub-bass blast");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_BYPASS_LIMITER) != 0, "Bypass limiter on blast");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Blast explicitly sets SUPPRESS_EMS");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) == 0, "Blast does not set PREFER_EMS");

    /* 3. Shock damage: square wave + dual EMS arms, all PREFER_EMS */
    LH_L1VR_ProcessDamage(0, 25, (1u << 8), from_front, origin, 28.0f, 0.0f, 0);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 3, "Shock emits torso + left arm + right arm");
    EXPECT_TRUE(rx[0].payload.synth.wave_shape == LH_WAVE_SQUARE, "Shock square wave");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Shock torso sets PREFER_EMS");
    EXPECT_TRUE(rx[1].zone_hint == LH_ZONE_ARM_LEFT && rx[2].zone_hint == LH_ZONE_ARM_RIGHT, "Dual EMS arms");
    EXPECT_TRUE((rx[1].flags & LH_FLAG_PREFER_EMS) != 0 && (rx[2].flags & LH_FLAG_PREFER_EMS) != 0, "Shock arms set PREFER_EMS");

    /* 4. Fall damage: feet + lower waist, both SUPPRESS_EMS */
    LH_L1VR_ProcessDamage(0, 30, (1u << 5), origin, origin, 28.0f, 0.0f, 0);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2, "Fall emits feet + waist");
    EXPECT_TRUE(rx[0].zone_hint == LH_ZONE_FEET, "Feet zone");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0 && (rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Fall sets SUPPRESS_EMS");

    /* 5. Environmental damage (Burn, Drown, Radiation) -> SUPPRESS_EMS; Slash -> PREFER_EMS */
    LH_L1VR_ProcessDamage(0, 12, (1u << 3), from_front, origin, 28.0f, 0.0f, 0); /* DMG_BURN */
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Burn sets SUPPRESS_EMS");

    LH_L1VR_ProcessDamage(0, 10, (1u << 14), origin, origin, 28.0f, 0.0f, 0); /* DMG_DROWN */
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Drown sets SUPPRESS_EMS");

    LH_L1VR_ProcessDamage(0, 10, (1u << 18), from_front, origin, 28.0f, 0.0f, 0); /* DMG_RADIATION */
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Radiation sets SUPPRESS_EMS");

    LH_L1VR_ProcessDamage(0, 15, (1u << 2), from_front, origin, 28.0f, 0.0f, 0); /* DMG_SLASH */
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Slash sets PREFER_EMS");

    /* 6. Armor shatter accent */
    LH_L1VR_ProcessDamage(10, 5, (1u << 1), from_front, origin, 28.0f, 0.0f, 10);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2, "Armor shatter adds shatter accent packet");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Bullet penetrating shattered armor sets PREFER_EMS");
    EXPECT_TRUE((rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0, "HEV shatter accent sets SUPPRESS_EMS");

    g_tests_passed++;
}

static void test_weapons_and_handedness(void) {
    LH_L1VR_ResetState();
    drain_rx();

    LH_EventPacket rx[8];

    /* 1. Right-handed Glock 1H -> dominant arm (PREFER_EMS) + light shoulder (SUPPRESS_EMS) */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/glock1.sc", 0.0f, 0, 0, 0, false, false), "Glock fire");
    size_t n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2, "Glock right hand emits arm + light shoulder");
    EXPECT_TRUE(rx[0].zone_hint == LH_ZONE_ARM_RIGHT && rx[0].entry_pos[0] > 1.0f, "Right arm");
    EXPECT_TRUE(rx[0].bus_id == LH_BUS_WEAPON_RECOIL, "WeaponRecoil bus");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Glock arm sets PREFER_EMS");
    EXPECT_TRUE((rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Glock shoulder vest sets SUPPRESS_EMS");

    /* 2. Left-handed MP5 with 2H stabilisation -> Left arm + Left shoulder + Right off-hand! */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/mp5.sc", 0.0f, 0, 0, 0, true, true), "MP5 2H left");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 3, "Stabilised 2H weapon emits 3 packets");
    EXPECT_TRUE(rx[0].zone_hint == LH_ZONE_ARM_LEFT && rx[0].entry_pos[0] < -1.0f, "Left dominant arm");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "MP5 dominant arm sets PREFER_EMS");
    EXPECT_TRUE(rx[1].zone_hint == LH_ZONE_TORSO && rx[1].entry_pos[0] < 0.0f, "Left shoulder");
    EXPECT_TRUE((rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0, "MP5 shoulder sets SUPPRESS_EMS");
    EXPECT_TRUE(rx[2].zone_hint == LH_ZONE_ARM_RIGHT && rx[2].entry_pos[0] > 1.0f, "Right support arm");
    EXPECT_TRUE((rx[2].flags & LH_FLAG_PREFER_EMS) != 0, "MP5 support arm sets PREFER_EMS");

    /* 3. Double-barrel SPAS-12 heavier than single-barrel */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/shotgun1.sc", 0.0f, 0, 0, 0, false, false), "Shotgun 1x");
    size_t n1 = recv_packets(rx, 8);
    float e1 = rx[0].payload.synth.energy;
    EXPECT_TRUE(n1 >= 2 && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Shotgun 1x arm+shoulder PREFER_EMS");

    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/shotgun2.sc", 0.0f, 0, 0, 0, false, false), "Shotgun 2x");
    size_t n2 = recv_packets(rx, 8);
    float e2 = rx[0].payload.synth.energy;
    EXPECT_TRUE(n2 >= 2 && e2 > e1, "Double-barrel stronger than single");

    /* 4. RPG backblast (3 packets: arm PREFER_EMS, front shoulder & rear backblast SUPPRESS_EMS) */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/rpg.sc", 0.0f, 0, 0, 0, false, false), "RPG");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 3 && rx[2].entry_pos[2] < -0.5f, "RPG includes rear backblast");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "RPG firing arm PREFER_EMS");
    EXPECT_TRUE((rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0 && (rx[2].flags & LH_FLAG_SUPPRESS_EMS) != 0, "RPG blast SUPPRESS_EMS");

    /* 5. Gauss spin (SUPPRESS_EMS) + charged shot (PREFER_EMS) + cleanup ignored */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/gaussspin.sc", 0.0f, 220, 0, 0, false, false), "Gauss spin");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].payload.synth.base_freq_hz > 180.0f && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Gauss spin SUPPRESS_EMS");
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/gauss.sc", 180.0f, 0, 0, 0, false, true), "Gauss charged");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 3 && rx[0].payload.synth.wave_shape == LH_WAVE_SQUARE && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Gauss charged PREFER_EMS");
    EXPECT_TRUE(!LH_L1VR_ProcessWeaponEvent("events/gauss.sc", 0.0f, 0, 0, 1, false, false), "Gauss holster ignored");

    /* 6. Egon fire -> PREFER_EMS */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/egon_fire.sc", 0.0f, 0, 0, 0, false, false), "Egon fire");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2 && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Egon arm sets PREFER_EMS");
    LH_L1VR_ProcessWeaponEvent("events/egon_stop.sc", 0.0f, 0, 0, 0, false, false);

    /* 7. Opposing Force M249 */
    EXPECT_TRUE(LH_L1VR_ProcessWeaponEvent("events/m249.sc", 0.0f, 0, 0, 0, false, true), "OpFor M249");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 3 && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "M249 2H batch PREFER_EMS");

    g_tests_passed++;
}

static void test_sounds_explosions_and_frame_systems(void) {
    LH_L1VR_ResetState();
    drain_rx();

    const float origin[3] = { 0, 0, 0 };
    LH_EventPacket rx[8];

    /* 1. Physical VR Crowbar flesh vs metal (PREFER_EMS on arm) */
    EXPECT_TRUE(LH_L1VR_ProcessSound("weapons/cbar_hitbod2.wav", origin, true, origin, 28.0f, 0.0f, false, false, false, 1.0f), "Crowbar flesh");
    size_t n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2 && rx[0].payload.synth.base_freq_hz < 120.0f && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Crowbar flesh PREFER_EMS");

    EXPECT_TRUE(LH_L1VR_ProcessSound("weapons/cbar_hit1.wav", origin, true, origin, 28.0f, 0.0f, false, false, false, 1.0f), "Crowbar metal");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2 && rx[0].payload.synth.base_freq_hz > 180.0f && (rx[0].flags & LH_FLAG_PREFER_EMS) != 0, "Crowbar metal PREFER_EMS");

    /* 2. Footsteps ignored when disabled, emitted with SUPPRESS_EMS when enabled */
    EXPECT_TRUE(!LH_L1VR_ProcessSound("player/pl_step1.wav", origin, true, origin, 28.0f, 0.0f, false, false, false, 1.0f), "Footstep disabled");
    EXPECT_TRUE(LH_L1VR_ProcessSound("player/pl_step1.wav", origin, true, origin, 28.0f, 0.0f, false, false, true, 1.0f), "Footstep enabled");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].zone_hint == LH_ZONE_FEET && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Footstep SUPPRESS_EMS");

    /* 3. Nearby 3D explosion (SUPPRESS_EMS) vs far explosion */
    const float near_exp[3] = { 180.0f, 0.0f, 10.0f };
    const float far_exp[3]  = { 1500.0f, 0.0f, 10.0f };
    EXPECT_TRUE(!LH_L1VR_ProcessExplosion(far_exp, 3.0f, origin, 28.0f, 0.0f), "Far explosion culled");
    EXPECT_TRUE(LH_L1VR_ProcessExplosion(near_exp, 3.0f, origin, 28.0f, 0.0f), "Near explosion emitted");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].bus_id == LH_BUS_ENVIRONMENT && rx[0].entry_pos[2] > 0.3f && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Forward 3D blast SUPPRESS_EMS");

    /* 4. Low-HP lub-dub Heartbeat + Xen Long-Jump + Geiger + menu pause (all SUPPRESS_EMS) */
    const float vel_still[3] = { 0, 0, 0 };
    LH_L1VR_ProcessFrameState(1000, true, false, 15, 0, true, vel_still, false, false, true);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].bus_id == LH_BUS_PLAYER_VITALS && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Heartbeat lub SUPPRESS_EMS");

    LH_L1VR_ProcessFrameState(1140, true, false, 15, 0, true, vel_still, false, false, true);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].bus_id == LH_BUS_PLAYER_VITALS && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Heartbeat dub SUPPRESS_EMS");

    /* Xen Long-Jump velocity spike while airborne */
    const float vel_lj[3] = { 560.0f, 0.0f, 260.0f };
    LH_L1VR_ProcessFrameState(1200, true, false, 80, 0, false, vel_lj, false, false, true);
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].entry_pos[2] < -0.8f && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Xen Long-Jump SUPPRESS_EMS");

    /* Geiger & HEV Suit -> SUPPRESS_EMS */
    EXPECT_TRUE(LH_L1VR_TriggerTestEffect("geiger", false, false), "Geiger test");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0, "Geiger sets SUPPRESS_EMS");

    EXPECT_TRUE(LH_L1VR_TriggerTestEffect("hev", false, false), "HEV test");
    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2 && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0 && (rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0, "HEV suit sets SUPPRESS_EMS");

    /* Pause in menu -> zero packets */
    LH_L1VR_ProcessFrameState(2500, false, false, 10, 0, true, vel_still, false, false, true);
    LH_UdpStats st;
    lh_udp_get_stats(&st);
    EXPECT_TRUE(st.packets_sent > 15 && st.send_errors == 0, "All loopback UDP packets delivered with zero errors");

    g_tests_passed++;
}

static void test_all_lh_test_commands(void) {
    LH_L1VR_ResetState();
    drain_rx();

    const char *effects[] = {
        "front", "back", "left", "right", "headshot", "shock", "explosion",
        "crowbar", "glock", "mp5", "shotgun", "doublebarrel", "magnum",
        "gauss", "egon", "rpg", "hev", "geiger", "heartbeat", "longjump", "death"
    };
    size_t count = sizeof(effects) / sizeof(effects[0]);
    for (size_t i = 0; i < count; i++) {
        EXPECT_TRUE(LH_L1VR_TriggerTestEffect(effects[i], false, true), effects[i]);
        LH_EventPacket rx[8];
        size_t n = recv_packets(rx, 8);
        EXPECT_TRUE(n >= 1, effects[i]);
        EXPECT_TRUE(rx[0].sequence_id > 0, "Monotonic sequence_id assigned");
    }
    EXPECT_TRUE(!LH_L1VR_TriggerTestEffect("nonexistent_effect", false, false), "Unknown effect returns false");

    g_tests_passed++;
}

static void test_multiplayer_sound_and_charger_distance_filters(void) {
    LH_L1VR_ResetState();
    drain_rx();

    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float far_charger[3] = { 300.0f, 0.0f, 0.0f };
    const float near_charger[3] = { 40.0f, 0.0f, 0.0f };
    LH_EventPacket rx[8];

    LH_UdpStats before;
    lh_udp_get_stats(&before);

    /* 1. Remote player sounds (is_local_player = false) must be rejected with zero UDP packets */
    EXPECT_TRUE(!LH_L1VR_ProcessSound("weapons/cbar_hitbod1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player crowbar flesh hit ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("weapons/cbar_hit1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player crowbar metal hit ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("weapons/cbar_miss1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player crowbar miss ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("weapons/pwrench_hit1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player pipe wrench hit ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("weapons/knife_hit_flesh1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player knife hit ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("items/flashlight1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player flashlight ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("player/pl_jumpland2.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player jump landing ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("player/pl_ladder1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player ladder step ignored");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("player/pl_step1.wav", origin, false, origin, 28.0f, 0.0f, false, false, true, 1.0f),
                "Remote player footstep ignored");

    LH_UdpStats after_remote;
    lh_udp_get_stats(&after_remote);
    EXPECT_TRUE(after_remote.packets_sent == before.packets_sent, "No UDP packets sent for remote player sounds");

    /* Confirm non-blocking socket truly has zero queued datagrams */
    uint8_t probe[LH_PACKET_SIZE_BYTES];
    EXPECT_TRUE(recv(g_rx_fd, probe, sizeof(probe), MSG_DONTWAIT) < 0, "UDP RX queue empty after remote player sounds");

    /* 2. Wall charger distance check: 300.0f rejected, 40.0f accepted (even when is_local_player = false, e.g. func_recharge entity) */
    EXPECT_TRUE(!LH_L1VR_ProcessSound("items/suitcharge1.wav", far_charger, false, origin, 28.0f, 0.0f, false, false, false, 1.0f),
                "Distant HEV charger (300u) rejected");
    EXPECT_TRUE(!LH_L1VR_ProcessSound("items/medcharge4.wav", far_charger, false, origin, 28.0f, 0.0f, false, false, false, 1.0f),
                "Distant Health charger (300u) rejected");

    LH_UdpStats after_far;
    lh_udp_get_stats(&after_far);
    EXPECT_TRUE(after_far.packets_sent == before.packets_sent, "No UDP packets sent for distant chargers");

    EXPECT_TRUE(LH_L1VR_ProcessSound("items/suitcharge1.wav", near_charger, false, origin, 28.0f, 0.0f, false, false, false, 1.0f),
                "Nearby HEV charger (40u) accepted");
    size_t n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].bus_id == LH_BUS_PLAYER_VITALS && (rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0,
                "Nearby HEV charger emits 1 PlayerVitals packet");

    g_tests_passed++;
}

static void test_item_pickup_bounded_buffer(void) {
    LH_L1VR_ResetState();
    drain_rx();

    LH_EventPacket rx[8];

    /* 1. Exact 9-byte buffer with NO trailing NUL byte */
    const char suit_no_nul[9] = { 'i', 't', 'e', 'm', '_', 's', 'u', 'i', 't' };
    LH_L1VR_OnUserMessage("ItemPickup", (int)sizeof(suit_no_nul), suit_no_nul);

    size_t n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 2, "Un-terminated 9-byte 'item_suit' detected as HEV suit equip (2 packets)");
    EXPECT_TRUE(rx[0].bus_id == LH_BUS_PLAYER_VITALS && rx[1].bus_id == LH_BUS_PLAYER_VITALS,
                "HEV suit packets on PlayerVitals bus");
    EXPECT_TRUE((rx[0].flags & LH_FLAG_SUPPRESS_EMS) != 0 && (rx[1].flags & LH_FLAG_SUPPRESS_EMS) != 0,
                "HEV suit packets suppress EMS");

    /* 2. Buffer where 'item_suit' only appears PAST size -> must NOT read beyond size */
    const char bounded_probe[] = "ammo_9mm_item_suit";
    LH_L1VR_OnUserMessage("ItemPickup", 9, bounded_probe);

    n = recv_packets(rx, 8);
    EXPECT_TRUE(n == 1 && rx[0].bus_id == LH_BUS_UI_FEEDBACK,
                "Bounded ItemPickup does not read past size into trailing 'item_suit'");

    g_tests_passed++;
}

int main(void) {
    printf("=== Lambda1VR LibreHaptics Native C99 & UDP Loopback Test Suite ===\n");
    setup_loopback_rx();
    printf("Bound test UDP receiver on 127.0.0.1:%u\n", (unsigned)g_rx_port);

    test_abi_layout();
    test_tcs_projection();
    test_damage_wire_and_types();
    test_weapons_and_handedness();
    test_sounds_explosions_and_frame_systems();
    test_all_lh_test_commands();
    test_multiplayer_sound_and_charger_distance_filters();
    test_item_pickup_bounded_buffer();

    LH_L1VR_Shutdown();
    if (g_rx_fd >= 0) close(g_rx_fd);

    printf("Results: %d passed, %d failed\n", g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
