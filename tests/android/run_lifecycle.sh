#!/bin/sh
# Drives lifecycle.apk through pause, resume and termination on a running
# emulator/device, pulls what it wrote, and checks it (check_lifecycle.sh).
#
#   run_lifecycle.sh <lifecycle.apk> <sub0log-cat> <work-dir>
set -eu
apk=$1; cat_tool=$2; work=$3
pkg=io.sub0log.lifecycle
activity=$pkg/android.app.NativeActivity
here=$(dirname "$0")

adb wait-for-device
adb uninstall "$pkg" >/dev/null 2>&1 || true
adb install -r "$apk"

# Launched the way the home screen launches it (MAIN/LAUNCHER, new task),
# so a relaunch resumes the existing task instead of stacking a new
# activity instance on it.
launch() {
    adb shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
        -f 0x10200000 -n "$activity"
}

# Run 1: launch, background (HOME: pause + stop), foreground again (resume),
# then terminate the way the system does -- SIGKILL, no callback.
launch
sleep 4
adb shell input keyevent KEYCODE_HOME
sleep 4
launch
sleep 4
adb shell am force-stop "$pkg"
sleep 2

# Run 2: a fresh process after the kill, killed again while in front.
launch
sleep 4
adb shell am force-stop "$pkg"
sleep 2

rm -rf "$work" && mkdir -p "$work"
for f in $(adb shell run-as "$pkg" ls files | tr -d '\r'); do
    adb exec-out run-as "$pkg" cat "files/$f" > "$work/$f"
done
ls -l "$work"
adb logcat -d -s sub0log:I | tail -5 || true

sh "$here/check_lifecycle.sh" "$work" "$cat_tool"
