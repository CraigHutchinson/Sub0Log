#!/bin/sh
# Builds lifecycle.apk with nothing but the Android SDK/NDK command-line
# tools -- no Gradle, no Java sources (NativeActivity, hasCode=false).
#
#   build_apk.sh <repo-root> <out-dir> [abi]
#
# Needs ANDROID_HOME (build-tools, a platform) and an NDK
# (ANDROID_NDK_LATEST_HOME or ANDROID_NDK_HOME), keytool, zip, cmake, ninja.
# GitHub's ubuntu runners have all of them preinstalled.
set -eu
abi=${3:-x86_64}
# Absolute paths: the zip step below runs from inside the staging directory.
root=$(cd "$1" && pwd)
mkdir -p "$2" && out=$(cd "$2" && pwd)

ndk=${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:?need an NDK}}
build_tools=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
platform=$(ls -d "$ANDROID_HOME"/platforms/android-* | sort -V | tail -1)
echo "ndk: $ndk"; echo "build-tools: $build_tools"; echo "platform: $platform"

cmake -S "$root/tests/android" -B "$out/native" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
cmake --build "$out/native"

rm -rf "$out/stage" && mkdir -p "$out/stage/lib/$abi"
cp "$out/native/libsub0log_lifecycle.so" "$out/stage/lib/$abi/"

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
