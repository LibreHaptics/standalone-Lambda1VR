/**
 * LibreHaptics (Loom Engine) Non-Blocking POSIX UDP Client Library
 * File: lh_udp_client.h (ABI v1.1 Android/Quest & Linux)
 *
 * Sends strict 64-byte little-endian LH_EventPacket datagrams (single or batched N*64B)
 * directly to LibreHaptics Player Lite (UdpEventReceiver, default 127.0.0.1:19800).
 *
 * Defensive modding guarantees:
 *  - Zero background threads
 *  - Zero heap allocations on hot path
 *  - Non-blocking UDP socket (MSG_DONTWAIT / O_NONBLOCK) so VR frame pacing is never stalled
 *  - Silent error isolation when Player Lite is closed or restarting
 */

#ifndef LH_UDP_CLIENT_H
#define LH_UDP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LH_UDP_DEFAULT_HOST      "127.0.0.1"
#define LH_UDP_DEFAULT_PORT      19800
#define LH_UDP_MAX_BATCH_PACKETS 8
#define LH_PACKET_SIZE_BYTES     64

#pragma pack(push, 8)

typedef enum {
    LH_PAYLOAD_PARAMETRIC = 0, // Tor A: Parametryczna synteza fali
    LH_PAYLOAD_PCM_STREAM = 1, // Tor B: Bezposredni strumien PCM
    LH_PAYLOAD_LEGACY     = 2  // Dyskretny wyzwalacz wzorca
} LH_PayloadType;

typedef enum {
    LH_FLAG_NONE           = 0,
    LH_FLAG_ACTIVE_BRAKING = 1 << 0, /* 0x0001 - Hamowanie przeciwfaza (VCM/LRA) */
    LH_FLAG_BYPASS_LIMITER = 1 << 1, /* 0x0002 - Obejscie duckingu (zdarzenie krytyczne) */
    LH_FLAG_PENETRATION    = 1 << 2, /* 0x0004 - Penetracja / pocisk */
    LH_FLAG_HEADSHOT       = 1 << 3, /* 0x0008 - Przekierowanie na strefe glowy */
    LH_FLAG_PREFER_EMS     = 1 << 4, /* 0x0010 - Jawna zgoda/preferencja na EMS */
    LH_FLAG_SUPPRESS_EMS   = 1 << 5  /* 0x0020 - Jawny zakaz EMS (wylacznie wibracja/kinetyka) */
} LH_Flags;

typedef enum {
    LH_BUS_CRITICAL_IMPACT = 0,
    LH_BUS_WEAPON_RECOIL   = 1,
    LH_BUS_ENVIRONMENT     = 2,
    LH_BUS_PLAYER_VITALS   = 3,
    LH_BUS_UI_FEEDBACK     = 4
} LH_BusID;

typedef enum {
    LH_ZONE_TORSO          = 0,
    LH_ZONE_HEAD           = 1,
    LH_ZONE_ARM_LEFT       = 2,
    LH_ZONE_ARM_RIGHT      = 3,
    LH_ZONE_HAND_LEFT      = 4,
    LH_ZONE_HAND_RIGHT     = 5,
    LH_ZONE_FEET           = 6
} LH_ZoneHint;

typedef enum {
    LH_WAVE_SINE           = 0,
    LH_WAVE_TRIANGLE       = 1,
    LH_WAVE_SQUARE         = 2,
    LH_WAVE_NOISE          = 3,
    LH_WAVE_IMPULSE        = 4
} LH_WaveShape;

// Strict 64-byte binary ABI matching LibreHaptics.Common.ABI.LH_EventPacket
typedef struct {
    uint64_t target_display_time_ns; // [0x00] OpenXR time (0 = immediate)
    uint32_t sequence_id;            // [0x08] Monotonic sequence counter
    uint8_t  bus_id;                 // [0x0C] LH_BusID
    uint8_t  payload_type;           // [0x0D] LH_PayloadType
    uint8_t  zone_hint;              // [0x0E] LH_ZoneHint
    uint8_t  reserved_byte;          // [0x0F]
    uint16_t flags;                  // [0x10] LH_Flags bitmask
    uint16_t reserved_flags;         // [0x12]
    float    entry_pos[3];           // [0x14] TCS (X: -L/+R, Y: -Waist/+Head, Z: -Back/+Front)
    float    spread_radius;          // [0x20] Spread radius in meters [0.01..2.50]

    union {
        struct {
            float   energy;           // [0x24] Normalized energy [0.0 .. 2.0]
            float   attack_sharpness; // [0x28] Transient sharpness [0.0 .. 1.0]
            float   base_freq_hz;     // [0x2C] Base frequency [5.0 .. 500.0 Hz]
            float   freq_glide_hz;    // [0x30] Frequency chirp delta (Hz)
            float   decay_ms;         // [0x34] Decay duration (ms)
            uint8_t wave_shape;       // [0x38] LH_WaveShape
            uint8_t reserved[7];      // [0x39]
        } synth;

        struct {
            float   exit_dir[3];      // [0x24]
            float   penetration_depth;// [0x30]
            float   exit_energy;      // [0x34]
            uint8_t reserved[8];      // [0x38]
        } trajectory;

        struct {
            uint32_t pcm_offset;      // [0x24]
            uint32_t sample_count;    // [0x28]
            uint16_t sample_rate;     // [0x2C]
            uint8_t  bit_depth;       // [0x2E]
            uint8_t  channels;        // [0x2F]
            uint8_t  reserved[16];    // [0x30]
        } pcm_stream;
    } payload;
} LH_EventPacket;

#pragma pack(pop)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(LH_EventPacket) == LH_PACKET_SIZE_BYTES, "LH_EventPacket must be exactly 64 bytes");
#elif defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(LH_EventPacket) == LH_PACKET_SIZE_BYTES, "LH_EventPacket must be exactly 64 bytes");
#else
typedef char lh_event_packet_size_check[(sizeof(LH_EventPacket) == LH_PACKET_SIZE_BYTES) ? 1 : -1];
#endif

typedef struct {
    char     host[64];
    uint16_t port;
    bool     enabled;
    bool     socket_ready;
    float    intensity_scale;
    uint64_t packets_sent;
    uint64_t send_errors;
} LH_UdpStats;

bool lh_udp_init(const char *host, uint16_t port);
bool lh_udp_configure(const char *host, uint16_t port);
void lh_udp_shutdown(void);

void lh_udp_set_enabled(bool enabled);
void lh_udp_set_intensity(float scale);

bool lh_udp_send(const LH_EventPacket *packet);
bool lh_udp_send_batch(const LH_EventPacket *packets, size_t count);

void lh_udp_get_stats(LH_UdpStats *out_stats);
void lh_udp_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* LH_UDP_CLIENT_H */

/* ============================================================================
 * IMPLEMENTATION (#define LH_UDP_IMPLEMENTATION in one translation unit)
 * ============================================================================ */
#ifdef LH_UDP_IMPLEMENTATION

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int                g_lh_udp_fd         = -1;
static struct sockaddr_in g_lh_udp_addr;
static bool               g_lh_udp_addr_valid = false;
static char               g_lh_udp_host[64]   = LH_UDP_DEFAULT_HOST;
static uint16_t           g_lh_udp_port       = LH_UDP_DEFAULT_PORT;
static bool               g_lh_udp_enabled    = true;
static float              g_lh_udp_intensity  = 1.0f;
static uint32_t           g_lh_udp_seq        = 0;
static uint64_t           g_lh_udp_sent       = 0;
static uint64_t           g_lh_udp_errors     = 0;

static bool lh_udp_open_socket_if_needed(void) {
    if (g_lh_udp_fd >= 0) {
        return true;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    g_lh_udp_fd = fd;
    return true;
}

bool lh_udp_configure(const char *host, uint16_t port) {
    const char *safe_host = (host && host[0] != '\0') ? host : LH_UDP_DEFAULT_HOST;
    uint16_t safe_port = (port > 0) ? port : LH_UDP_DEFAULT_PORT;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(safe_port);

    if (inet_pton(AF_INET, safe_host, &addr.sin_addr) != 1) {
        /* Fallback to loopback if user entered an invalid IP */
        safe_host = LH_UDP_DEFAULT_HOST;
        (void)inet_pton(AF_INET, safe_host, &addr.sin_addr);
    }

    snprintf(g_lh_udp_host, sizeof(g_lh_udp_host), "%s", safe_host);
    g_lh_udp_port = safe_port;
    g_lh_udp_addr = addr;
    g_lh_udp_addr_valid = true;

    return lh_udp_open_socket_if_needed();
}

bool lh_udp_init(const char *host, uint16_t port) {
    return lh_udp_configure(host, port);
}

void lh_udp_shutdown(void) {
    if (g_lh_udp_fd >= 0) {
        close(g_lh_udp_fd);
        g_lh_udp_fd = -1;
    }
    g_lh_udp_addr_valid = false;
}

void lh_udp_set_enabled(bool enabled) {
    g_lh_udp_enabled = enabled;
}

void lh_udp_set_intensity(float scale) {
    if (!isfinite(scale)) {
        scale = 1.0f;
    }
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 2.0f) scale = 2.0f;
    g_lh_udp_intensity = scale;
}

bool lh_udp_send_batch(const LH_EventPacket *packets, size_t count) {
    if (!g_lh_udp_enabled || !packets || count == 0 || g_lh_udp_intensity <= 0.001f) {
        return false;
    }

    if (!g_lh_udp_addr_valid) {
        if (!lh_udp_configure(g_lh_udp_host, g_lh_udp_port)) {
            g_lh_udp_errors++;
            return false;
        }
    } else if (g_lh_udp_fd < 0) {
        if (!lh_udp_open_socket_if_needed()) {
            g_lh_udp_errors++;
            return false;
        }
    }

    bool all_ok = true;
    size_t offset = 0;

    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > LH_UDP_MAX_BATCH_PACKETS) {
            chunk = LH_UDP_MAX_BATCH_PACKETS;
        }

        LH_EventPacket wire[LH_UDP_MAX_BATCH_PACKETS];
        for (size_t i = 0; i < chunk; i++) {
            wire[i] = packets[offset + i];
            if (wire[i].sequence_id == 0) {
                wire[i].sequence_id = __atomic_add_fetch(&g_lh_udp_seq, 1u, __ATOMIC_RELAXED);
            }
            if (wire[i].payload_type == (uint8_t)LH_PAYLOAD_PARAMETRIC) {
                float scaled = wire[i].payload.synth.energy * g_lh_udp_intensity;
                if (!isfinite(scaled) || scaled < 0.0f) scaled = 0.0f;
                if (scaled > 2.0f) scaled = 2.0f;
                wire[i].payload.synth.energy = scaled;
            }
        }

        ssize_t nbytes = (ssize_t)(chunk * LH_PACKET_SIZE_BYTES);
        ssize_t sent = sendto(
            g_lh_udp_fd,
            wire,
            (size_t)nbytes,
            MSG_DONTWAIT,
            (const struct sockaddr *)&g_lh_udp_addr,
            sizeof(g_lh_udp_addr)
        );

        if (sent == nbytes) {
            g_lh_udp_sent += (uint64_t)chunk;
        } else {
            g_lh_udp_errors++;
            all_ok = false;
        }

        offset += chunk;
    }

    return all_ok;
}

bool lh_udp_send(const LH_EventPacket *packet) {
    if (!packet) return false;
    return lh_udp_send_batch(packet, 1);
}

void lh_udp_get_stats(LH_UdpStats *out_stats) {
    if (!out_stats) return;
    snprintf(out_stats->host, sizeof(out_stats->host), "%s", g_lh_udp_host);
    out_stats->port = g_lh_udp_port;
    out_stats->enabled = g_lh_udp_enabled;
    out_stats->socket_ready = (g_lh_udp_fd >= 0 && g_lh_udp_addr_valid);
    out_stats->intensity_scale = g_lh_udp_intensity;
    out_stats->packets_sent = g_lh_udp_sent;
    out_stats->send_errors = g_lh_udp_errors;
}

void lh_udp_reset_stats(void) {
    g_lh_udp_sent = 0;
    g_lh_udp_errors = 0;
    g_lh_udp_seq = 0;
}

#endif /* LH_UDP_IMPLEMENTATION */
