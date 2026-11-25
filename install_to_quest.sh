#!/usr/bin/env bash
# =============================================================================
# Quest Sideloading Helper for Lambda1VR + LibreHaptics
#
# Installs the built APK like standard Lambda1VR (com.drbeef.lambda1vr).
# Optionally syncs Half-Life 1 assets (+ HD pack by default) from Steam install
# when invoked with --sync-assets.
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APK_RELEASE="${ROOT_DIR}/Projects/Android/build/outputs/apk/release/lambda1vr-release.apk"
APK_DEBUG="${ROOT_DIR}/Projects/Android/build/outputs/apk/debug/lambda1vr-debug.apk"
HL1_STEAM_DIR="${HL1_STEAM_DIR:-${HOME}/.local/share/Steam/steamapps/common/Half-Life}"

SYNC_ASSETS=false
USE_HD=true
APK_PATH=""

for arg in "$@"; do
    case "$arg" in
        --sync-assets) SYNC_ASSETS=true ;;
        --no-hd)       USE_HD=false ;;
        --debug)       APK_PATH="${APK_DEBUG}" ;;
        --release)     APK_PATH="${APK_RELEASE}" ;;
        -h|--help)
            echo "Usage: $0 [--release|--debug] [--sync-assets] [--no-hd]"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${APK_PATH}" ]]; then
    if [[ -f "${APK_RELEASE}" ]]; then
        APK_PATH="${APK_RELEASE}"
    elif [[ -f "${APK_DEBUG}" ]]; then
        APK_PATH="${APK_DEBUG}"
    else
        echo "ERROR: No built APK found. Run ./build_lambda1vr_lh.sh first." >&2
        exit 1
    fi
fi

echo "Checking connected ADB device..."
if ! adb get-state >/dev/null 2>&1; then
    echo "ERROR: No Meta Quest detected over ADB. Connect USB and accept RSA prompt." >&2
    exit 1
fi

echo "Installing ${APK_PATH} ..."
if ! adb install -r "${APK_PATH}"; then
    echo ""
    echo "NOTE: If stock Lambda1VR from SideQuest is installed with a different signing key,"
    echo "uninstall it first (your saves in /sdcard/xash/valve/save are kept):"
    echo "  adb uninstall com.drbeef.lambda1vr && adb install \"${APK_PATH}\""
    exit 1
fi

if [[ "${SYNC_ASSETS}" == "true" ]]; then
    if [[ ! -d "${HL1_STEAM_DIR}/valve" ]]; then
        echo "ERROR: Half-Life 'valve' directory not found at ${HL1_STEAM_DIR}/valve" >&2
        exit 1
    fi
    echo "Ensuring /sdcard/xash/valve exists on Quest..."
    adb shell mkdir -p /sdcard/xash/valve
    echo "Pushing Half-Life 'valve' assets (this may take a few minutes)..."
    adb push "${HL1_STEAM_DIR}/valve/." /sdcard/xash/valve/
    if [[ "${USE_HD}" == "true" && -d "${HL1_STEAM_DIR}/valve_hd" ]]; then
        echo "Applying Half-Life HD pack ('valve_hd')..."
        adb push "${HL1_STEAM_DIR}/valve_hd/." /sdcard/xash/valve/
    fi
fi

echo "[OK] Done! Start LibreHaptics Player Lite first, then launch Lambda1VR on your Quest."
