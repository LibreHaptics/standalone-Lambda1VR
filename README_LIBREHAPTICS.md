# Lambda1VR + LibreHaptics (Half-Life 1 VR for Meta Quest)

Native **LibreHaptics (Loom.Lite)** integration for [Team-Beef-Studios/Lambda1VR](https://github.com/Team-Beef-Studios/Lambda1VR) (Xash3D-FWGS + OpenXR, `arm64-v8a`), communicating directly with **LibreHaptics Player Lite** (`org.librehaptics.player.quest`) over local UDP (`127.0.0.1:19800`) with `:loom_service` OOM protection (`BIND_AUTO_CREATE | BIND_IMPORTANT`).

Because the hooks operate at the **`libxash.so`** engine level, haptics work automatically in Singleplayer and Multiplayer across:
- **Half-Life 1** (`valve`)
- **Half-Life: Blue Shift** (`bshift`)
- **Half-Life: Opposing Force** (`gearbox`)
- **Afraid of Monsters** (`aomdc`) and **They Hunger** (`hunger`)

---

## Installation on Meta Quest

### Option A — 1-Click via LibreHaptics Player Lite (Recommended)
1. Open **LibreHaptics Player Lite** on your Meta Quest and refresh the game catalog (`↻ GitHub`).
2. Select **Lambda1VR (Half-Life VR)** and tap **Install**.
   *(If you already have stock Lambda1VR from SideQuest installed, uninstall it first due to a different signing key — your game assets and saves in `/sdcard/xash/` are preserved).*

### Option B — Manual Sideload (SideQuest / ADB)
1. Uninstall stock Lambda1VR if installed from SideQuest (saves and assets in `/sdcard/xash/` remain untouched).
2. Download the release APK from **GitHub Releases** (or build it locally) and install via SideQuest or ADB:
   ```bash
   ./install_to_quest.sh
   ```
   *(Optional: pass `--sync-assets` to push `valve` + `valve_hd` from `~/.local/share/Steam/steamapps/common/Half-Life` or `$HL1_STEAM_DIR` to `/sdcard/xash/valve`.)*

### Running on Headset
1. Launch **LibreHaptics Player Lite** and connect your haptic vest (TrueGear / bHaptics / OWO).
2. Launch **Lambda1VR** — it automatically binds `:loom_service` (OOM protection) and streams 64-byte `LH_EventPacket` frames to `127.0.0.1:19800`.

---

## Xash3D Console Variables & Commands (`config.cfg`)

| Cvar / Command | Default | Description |
|---|---|---|
| `lh_enable` | `"1"` | Enables / disables LibreHaptics UDP telemetry |
| `lh_host` | `"127.0.0.1"` | `UdpEventReceiver` target IP address |
| `lh_port` | `"19800"` | `UdpEventReceiver` target UDP port |
| `lh_intensity` | `"1.0"` | Global intensity multiplier (`0.0 .. 2.0`) |
| `lh_heartbeat` | `"1"` | Dynamic low-HP heartbeat (*lub-dub*) when HP ≤ 30 |
| `lh_geiger` | `"1"` | Radiation Geiger counter clicks |
| `lh_footsteps` | `"0"` | Optional subtle footsteps on the feet zone (off by default) |
| `lh_debug` | `"0"` | Log outgoing packets to console / `logcat` |
| `lh_status` | *(command)* | Prints socket state, TX packet count, handedness, HP/HEV |
| `lh_test <effect>` | *(command)* | Triggers a test pulse: `front`, `back`, `left`, `right`, `headshot`, `shock`, `explosion`, `crowbar`, `glock`, `mp5`, `shotgun`, `doublebarrel`, `magnum`, `gauss`, `egon`, `rpg`, `hev`, `geiger`, `heartbeat`, `longjump`, `death` |

---

## Custom `.lhfx` Overrides & Muting

Every outgoing parametric `LH_EventPacket` from Lambda1VR automatically stamps a deterministic 32-bit FNV-1a (lowercase ASCII) **`effect_hash`** at byte offset `0x3C`.

- **Zero setup required:** If no matching `.lhfx` asset is installed in **LibreHaptics Player Lite**, the engine plays the built-in procedural haptics as a fallback.
- **Customize any effect:** Create an `.lhfx` asset in **LibreHaptics Creator** whose `metadata.id` matches an effect tag (case-insensitive) — e.g. `Flashlight`, `Shotgun2x`, `Glock`, `Python357`, `MP5`, `RPG`, `CrowbarSwing`, `MeleeHitFlesh`, `MeleeHitMetal`, `HevCharger`, `HealthCharger`, `HevSuitEquip`, `GeigerClick`, `HeartbeatLub`, `HeartbeatDub`, `Explosion3D`, `Damage`, `XenLongJump` — and add it to your game pack or user overrides.
  - Leave `spatial.default_position` as `null` (with `mode: "tcs_point"`) to inherit live 3D hit coordinates from the game.
  - Multi-packet batches sharing the same `effect_hash` (such as `Shotgun2x` or `HevSuitEquip`) are automatically deduplicated in `UdpEventReceiver` so the `.lhfx` override triggers once.
- **Mute an unwanted effect:** Provide a matching `.lhfx` asset with all layers disabled (`"enabled": false`) or muted (`"is_muted": true`).
- **Discovering exact tags:** Enable `lh_debug 1` in the console to log every outgoing effect tag and its `hash=0x...` to `logcat`.

---

## Building & Tests

```bash
./build_lambda1vr_lh.sh          # runs C99 unit tests + builds lambda1vr-release.apk
./build_lambda1vr_lh.sh Debug    # builds Debug variant
```

Output APK:
`Projects/Android/build/outputs/apk/release/lambda1vr-release.apk`
