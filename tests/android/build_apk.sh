#!/bin/sh
# Builds lifecycle.apk with nothing but the Android SDK/NDK command-line
# tools -- no Gradle, no Java sources (NativeActivity, hasCode=false).
#
#   build_apk.sh <repo-root> <out-dir> [abi...]
#
# Default: all four Android ABIs, packed into one universal APK. 32-bit ARM
# and x86 are there on purpose: they are where a compiler may lower atomics
# to __atomic_* calls in libatomic (the root CMakeLists.txt probes for that
# and links it when needed), and each .so is checked below for any such
# symbol left unresolved -- which would link fine and fail only at load
# time, on a device.
#
# Needs ANDROID_HOME (build-tools, a platform) and an NDK
# (ANDROID_NDK_LATEST_HOME or ANDROID_NDK_HOME), keytool, zip, cmake, ninja.
# GitHub's ubuntu runners have all of them preinstalled.
set -eu
# Absolute paths: the zip step below runs from inside the staging directory.
root=$(cd "$1" && pwd)
mkdir -p "$2" && out=$(cd "$2" && pwd)
shift 2
abis=${*:-armeabi-v7a arm64-v8a x86 x86_64}

ndk=${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:?need an NDK}}
build_tools=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
platform=$(ls -d "$ANDROID_HOME"/platforms/android-* | sort -V | tail -1)
echo "ndk: $ndk"; echo "build-tools: $build_tools"; echo "platform: $platform"

nm_tool=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-nm | head -1)
rm -rf "$out/stage"
for abi in $abis; do
    echo "== $abi"
    cmake -S "$root/tests/android" -B "$out/native-$abi" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
    grep -E 'SUB0LOG_ATOMICS_LINK_(WITHOUT|WITH)_LIBATOMIC' "$out/native-$abi/CMakeCache.txt" || true
    cmake --build "$out/native-$abi"
    lib="$out/native-$abi/libsub0log_lifecycle.so"
    if "$nm_tool" -u "$lib" | grep -E '__atomic_|__sync_'; then
        echo "FAILED: $abi: unresolved atomic library calls in $lib" >&2
        exit 1
    fi
    mkdir -p "$out/stage/lib/$abi"
    cp "$lib" "$out/stage/lib/$abi/"
done

# --debug-mode makes it debuggable, which is what lets the driver read
# app-private storage with `run-as`.
"$build_tools/aapt2" link -o "$out/unsigned.apk" --manifest "$root/tests/android/AndroidManifest.xml" \
    -I "$platform/android.jar" --debug-mode
(cd "$out/stage" && zip -q -r "$out/unsigned.apk" lib)
"$build_tools/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"

rm -f "$out/debug.keystore"
keytool -genkeypair -keystore "$out/debug.keystore" -storepass android -keypass android \
    -alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Sub0Log debug" >/dev/null 2>&1
"$build_tools/apksigner" sign --ks "$out/debug.keystore" --ks-pass pass:android \
    --out "$out/lifecycle.apk" "$out/aligned.apk"
ls -l "$out/lifecycle.apk"
