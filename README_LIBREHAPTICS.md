# Lambda1VR + LibreHaptics (Half-Life 1 VR dla Meta Quest)

Natywna integracja **LibreHaptics (Loom.Lite)** dla portu [Team-Beef-Studios/Lambda1VR](https://github.com/Team-Beef-Studios/Lambda1VR) (Xash3D-FWGS + OpenXR, `arm64-v8a`), działająca bezpośrednio z **LibreHaptics Player Lite** (`org.librehaptics.player.quest`) przez lokalne UDP (`127.0.0.1:19800`) oraz mechanizm ochrony procesu `:loom_service` (`BIND_AUTO_CREATE | BIND_IMPORTANT`).

Mod działa na poziomie silnika **`libxash.so`**, dzięki czemu obsługuje automatycznie w trybie Singleplayer i Multiplayer:
- **Half-Life 1** (`valve`)
- **Half-Life: Blue Shift** (`bshift`)
- **Half-Life: Opposing Force** (`gearbox`)
- **Afraid of Monsters** (`aomdc`) oraz **They Hunger** (`hunger`)

---

## Instalacja na Meta Quest

Instaluje się dokładnie jak standardowa Lambda1VR (`com.drbeef.lambda1vr`):
1. Jeśli masz zainstalowaną oryginalną wersję Lambda1VR z SideQuest, odinstaluj ją najpierw (zapisy gry i pliki w `/sdcard/xash/` pozostają nienaruszone).
2. Zainstaluj zbudowany plik APK przez SideQuest lub ADB:
   ```bash
   ./install_to_quest.sh
   ```
   *(Opcjonalnie, jeśli jeszcze nie masz skopiowanych plików gry z oryginalnego Half-Life, możesz dodać flagę `--sync-assets`, aby przesłać `valve` + `valve_hd` z `~/.local/share/Steam/steamapps/common/Half-Life` lub ścieżki podanej w `$HL1_STEAM_DIR` do `/sdcard/xash/valve`.)*
3. **Uruchamianie na goglach:**
   - Uruchom **LibreHaptics Player Lite** i połącz kamizelkę (TrueGear / bHaptics / OWO).
   - Uruchom **Lambda1VR** — aplikacja automatycznie podepnie `:loom_service` (ochrona przed OOM) i zacznie wysyłać 64-bajtowe pakiety `LH_EventPacket` na `127.0.0.1:19800`.

---

## Komendy i zmienne konsoli Xash3D (`config.cfg`)

| Zmienna / Komenda | Domyślna wartość | Opis |
|---|---|---|
| `lh_enable` | `"1"` | Włącza / wyłącza wysyłanie telemetrii LibreHaptics |
| `lh_host` | `"127.0.0.1"` | Adres IP odbiornika `UdpEventReceiver` |
| `lh_port` | `"19800"` | Port UDP odbiornika `UdpEventReceiver` |
| `lh_intensity` | `"1.0"` | Globalny mnożnik siły (`0.0 .. 2.0`) |
| `lh_heartbeat` | `"1"` | Dynamiczne bicie serca (*lub-dub*) przy HP ≤ 30 |
| `lh_geiger` | `"1"` | Kliknięcia licznika Geigera przy promieniowaniu |
| `lh_footsteps` | `"0"` | Opcjonalne lekkie kroki na strefie stóp (domyślnie wyłączone) |
| `lh_debug` | `"0"` | Logowanie wysyłanych pakietów w konsoli / `logcat` |
| `lh_status` | *(komenda)* | Wyświetla status gniazda, licznik pakietów TX, tryb ręki, HP/HEV |
| `lh_test <efekt>` | *(komenda)* | Wysyła testowy impuls: `front`, `back`, `left`, `right`, `headshot`, `shock`, `explosion`, `crowbar`, `glock`, `mp5`, `shotgun`, `doublebarrel`, `magnum`, `gauss`, `egon`, `rpg`, `hev`, `geiger`, `heartbeat`, `longjump`, `death` |

---

## Kompilacja i testy

```bash
./build_lambda1vr_lh.sh          # buduje testy C99 + lambda1vr-release.apk
./build_lambda1vr_lh.sh Debug    # buduje wariant Debug
```

Gotowy plik APK:
`Projects/Android/build/outputs/apk/release/lambda1vr-release.apk`
