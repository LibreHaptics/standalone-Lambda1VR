#!/usr/bin/env bash
# =============================================================================
# Build & Verification Script for Lambda1VR + LibreHaptics (Meta Quest arm64-v8a)
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="${ROOT_DIR}/Projects/Android"
BUILD_TYPE="${1:-Release}" # Release or Debug

export JAVA_HOME="${JAVA_HOME:-${HOME}/Android/Jdk17}"
export ANDROID_HOME="${ANDROID_HOME:-${HOME}/Android/Sdk}"
export ANDROID_SDK_ROOT="${ANDROID_HOME}"

echo "=== [1/4] Compiling & running native C99 unit + UDP loopback tests ==="
TEST_BIN="${ROOT_DIR}/build_test_lh_lambda1vr"
gcc -std=c99 -Wall -Wextra -Werror -O2 -DLH_STANDALONE_TEST \
    -I"${ROOT_DIR}/Projects/Android/jni/src/Lambda1VR" \
    "${ROOT_DIR}/test_lh_lambda1vr.c" \
    "${ROOT_DIR}/Projects/Android/jni/src/Lambda1VR/VrLibreHaptics.c" \
    -lm -o "${TEST_BIN}"
"${TEST_BIN}"
rm -f "${TEST_BIN}"

echo ""
echo "=== [2/4] Checking OpenXR loader prebuilts ==="
if [[ ! -f "${ANDROID_DIR}/libs/arm64-v8a/libopenxr_loader_meta.so" ]] || \
   [[ ! -f "${ROOT_DIR}/Projects/AndroidPrebuilt/jni/libopenxr_loader.so" ]]; then
    echo "ERROR: Missing libopenxr_loader_meta.so or Projects/AndroidPrebuilt/jni/libopenxr_loader.so!" >&2
    exit 1
fi
ls -lh "${ANDROID_DIR}/libs/arm64-v8a/libopenxr_loader_meta.so" "${ROOT_DIR}/Projects/AndroidPrebuilt/jni/libopenxr_loader.so"

echo ""
echo "=== [3/4] Building Lambda1VR APK (${BUILD_TYPE}, arm64-v8a) ==="
# Note: GNU Make (ndk-build) does not support spaces in canonical parent paths ("Modowanie gier"),
# and Gradle resolves symlinks via getCanonicalPath(). Mirror to a space-free cache directory
# (keeping incremental build/.cxx outputs) and copy the resulting APK back into ROOT_DIR.
BUILD_ROOT="${ROOT_DIR}"
MIRROR_DIR=""
if [[ "${ROOT_DIR}" == *" "* ]]; then
    MIRROR_DIR="${HOME}/.cache/lambda1vr_lh_build"
    mkdir -p "${MIRROR_DIR}"
    rsync -a --delete \
        --exclude='.git' \
        --exclude='Projects/Android/build' \
        --exclude='Projects/Android/.cxx' \
        --exclude='Projects/Android/.gradle' \
        "${ROOT_DIR}/" "${MIRROR_DIR}/"
    BUILD_ROOT="${MIRROR_DIR}"
fi

pushd "${BUILD_ROOT}/Projects/Android" >/dev/null
"${JAVA_HOME}/bin/java" -Xmx2048m \
    "-Dorg.gradle.appname=gradlew" \
    -classpath "${BUILD_ROOT}/Projects/Android/gradle/wrapper/gradle-wrapper.jar" \
    org.gradle.wrapper.GradleWrapperMain \
    "assemble${BUILD_TYPE}" --no-daemon "${@:2}"
popd >/dev/null

APK_LOWER="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
BUILT_APK="${BUILD_ROOT}/Projects/Android/build/outputs/apk/${APK_LOWER}/lambda1vr-${APK_LOWER}.apk"
APK_PATH="${ANDROID_DIR}/build/outputs/apk/${APK_LOWER}/lambda1vr-${APK_LOWER}.apk"

if [[ ! -f "${BUILT_APK}" ]]; then
    echo "ERROR: Expected APK not found at ${BUILT_APK}" >&2
    find "${BUILD_ROOT}/Projects/Android/build/outputs/apk" -name "*.apk" 2>/dev/null || true
    exit 1
fi

if [[ -n "${MIRROR_DIR}" ]]; then
    mkdir -p "$(dirname "${APK_PATH}")"
    cp -f "${BUILT_APK}" "${APK_PATH}"
fi

echo ""
echo "=== [4/4] Verifying APK contents, signature & LibreHaptics symbols in libxash.so ==="
ls -lh "${APK_PATH}"
ASSET_COUNT="$(unzip -l "${APK_PATH}" | grep -c ' assets/' || true)"
echo "Bundled assets count: ${ASSET_COUNT}"
unzip -l "${APK_PATH}" | grep -E "lib/arm64-v8a/.*\.so$"

PATH="${JAVA_HOME}/bin:${PATH}" "${ANDROID_HOME}/build-tools/34.0.0/apksigner" verify --verbose "${APK_PATH}"

TMP_SO_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_SO_DIR}"' EXIT
unzip -q -j "${APK_PATH}" "lib/arm64-v8a/libxash.so" -d "${TMP_SO_DIR}"

NM_BIN="${ANDROID_HOME}/ndk/25.1.8937393/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
echo "Checking exported/defined LH_L1VR_* symbols in libxash.so:"
"${NM_BIN}" -D "${TMP_SO_DIR}/libxash.so" 2>/dev/null | grep -E "LH_L1VR_|lh_udp_" || true
"${NM_BIN}" "${TMP_SO_DIR}/libxash.so" 2>/dev/null | grep -E "LH_L1VR_|lh_udp_" | head -n 20 || true

echo ""
echo "[OK] Lambda1VR + LibreHaptics APK ready at:"
echo "     ${APK_PATH}"
