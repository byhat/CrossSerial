#!/usr/bin/env bash
#
# Builds libcrossserial.so for one or more Android ABIs and lays them out as
# jniLibs/<abi>/. Each ABI gets its own CMake build directory (with the
# embedded classes.dex), then the .so is copied to <OutDir>/jniLibs/<abi>/ and
# the public headers to <OutDir>/include/cs/.
#
# Usage:
#   scripts/build-android.sh                          # all 4 ABIs
#   scripts/build-android.sh arm64-v8a armeabi-v7a    # a subset
#   MIN_API=android-21 OUT_DIR=dist/android scripts/build-android.sh
set -euo pipefail

ABIS=("arm64-v8a" "armeabi-v7a" "x86" "x86_64")
MIN_API="${MIN_API:-android-21}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
OUT_DIR="${OUT_DIR:-dist/android}"
CLEAN="${CLEAN:-0}"

# Extra ABIs from the command line.
if [ "$#" -gt 0 ]; then ABIS=("$@"); fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Discover SDK / NDK / JDK.
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
if [ ! -d "$SDK_ROOT" ]; then echo "SDK not found: $SDK_ROOT (set ANDROID_SDK_ROOT)"; exit 1; fi

NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}"
if [ -z "$NDK_ROOT" ] || [ ! -d "$NDK_ROOT" ]; then
    NDK_BASE="$SDK_ROOT/ndk"
    if [ -d "$NDK_BASE" ]; then
        NDK_ROOT="$(ls -d "$NDK_BASE"/*/ 2>/dev/null | sort -V | tail -n1)"
    fi
fi
NDK_ROOT="${NDK_ROOT%/}"
TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then echo "Android toolchain not found: $TOOLCHAIN"; exit 1; fi

if ! command -v javac >/dev/null 2>&1; then echo "javac not found (need a JDK)"; exit 1; fi

# Generator: prefer Ninja, fall back to Unix Makefiles.
if command -v ninja >/dev/null 2>&1; then GENERATOR="Ninja"; else GENERATOR="Unix Makefiles"; fi

export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_NDK="$NDK_ROOT"
[ -n "${JAVA_HOME:-}" ] && export JAVA_HOME

echo "Repo      : $REPO_ROOT"
echo "SDK       : $SDK_ROOT"
echo "NDK       : $NDK_ROOT"
echo "Generator : $GENERATOR"
echo "Output    : $OUT_DIR"
echo "ABIs      : ${ABIS[*]}"
echo

BUILD_ROOT="$REPO_ROOT/build-android"
JNI_OUT="$OUT_DIR/jniLibs"
INC_OUT="$OUT_DIR/include"

[ "$CLEAN" = "1" ] && [ -d "$BUILD_ROOT" ] && rm -rf "$BUILD_ROOT"

mkdir -p "$JNI_OUT" "$INC_OUT"

FAILED=()
for abi in "${ABIS[@]}"; do
    echo "=== $abi ==="
    abi_build="$BUILD_ROOT/$abi"
    cmake -S "$REPO_ROOT" -B "$abi_build" -G "$GENERATOR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="$MIN_API" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DANDROID_SDK_ROOT="$SDK_ROOT" \
        -DCROSSSERIAL_BUILD_EXAMPLES=OFF \
        -DCROSSSERIAL_BUILD_TESTS=OFF
    cmake --build "$abi_build" -j
    if [ ! -f "$abi_build/libcrossserial.so" ]; then
        echo "  libcrossserial.so not produced for $abi"; FAILED+=("$abi"); continue
    fi
    mkdir -p "$JNI_OUT/$abi"
    cp -f "$abi_build/libcrossserial.so" "$JNI_OUT/$abi/libcrossserial.so"
    echo "  -> $JNI_OUT/$abi/libcrossserial.so"
done

# Headers (once).
rm -rf "$INC_OUT/cs"
mkdir -p "$INC_OUT/cs"
cp -f "$REPO_ROOT"/include/cs/* "$INC_OUT/cs/"

echo
echo "Done. Output layout:"
echo "  $OUT_DIR/jniLibs/<abi>/libcrossserial.so   (dex embedded)"
echo "  $OUT_DIR/include/cs/*.h,*.hpp"
if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "Failed ABIs: ${FAILED[*]}"; exit 1
fi
