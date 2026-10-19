#!/system/bin/sh
# MIUI writes screen_off_timeout=60000 and clears stay_on_while_plugged_in
# again after boot_completed. A cast then freezes once the panel dozes.
# Re-apply for a minute, same idea as ColorOS re-checking its wallpaper.
hz=$(getprop ro.boot.tebox.refresh_hz)
[ -n "$hz" ] || hz=120
# A dozing lock screen reports the panel OFF. scrcpy then sends POWER and the
# mirror stays black. Keep the panel awake and skip the lock screen.
locksettings set-disabled true
cmd settings put secure screensaver_enabled 0
cmd settings put secure doze_enabled 0
cmd settings put secure doze_always_on 0
input keyevent KEYCODE_WAKEUP
i=0
while [ "$i" -lt 12 ]; do
    cmd settings put global stay_on_while_plugged_in 3
    cmd settings put system screen_off_timeout 2147483647
    cmd settings put system screen_brightness_mode 0
    cmd settings put system screen_brightness 255
    cmd settings put system peak_refresh_rate "$hz"
    cmd settings put system min_refresh_rate "$hz"
    i=$((i + 1))
    sleep 5
done
