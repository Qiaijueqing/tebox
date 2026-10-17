#!/system/bin/sh
# Flyme's TeleService (com.android.phone, targetSdk 35) registers
# com.meizu.networkdiagnostic's receiver without RECEIVER_EXPORTED /
# NOT_EXPORTED, which DYNAMIC_RECEIVER_EXPLICIT_EXPORT_REQUIRED (161145287)
# turns into a SecurityException: phone, and com.qti.phone after it,
# crash-loop. The change is checked per uid; on a Meizu device other
# android.uid.phone packages from the vendor partitions presumably target an
# older SDK and keep it off for the whole uid. Disable it for com.android.phone
# as `am compat disable` would; PlatformCompat reads the file at boot.
dir=/data/misc/appcompat
f=$dir/compat_framework_overrides.xml
change=161145287
entry='<change-overrides changeId="161145287"><validated><override-value packageName="com.android.phone" enabled="false"></override-value></validated><raw><raw-override-value packageName="com.android.phone" minVersionCode="-9223372036854775808" maxVersionCode="9223372036854775807" enabled="false"></raw-override-value></raw></change-overrides>'

mkdir -p "$dir"
chown system:system "$dir"
chmod 0700 "$dir"
if [ ! -s "$f" ]; then
  echo '<?xml version="1.0" encoding="utf-8"?>' > "$f.tmp"
  echo "<overrides>$entry</overrides>" >> "$f.tmp"
elif grep -q "changeId=\"$change\"" "$f"; then
  exit 0
else
  sed "s#</overrides>#$entry</overrides>#" "$f" > "$f.tmp"
  grep -q "changeId=\"$change\"" "$f.tmp" || { rm -f "$f.tmp"; exit 1; }
fi
chown system:system "$f.tmp"
chmod 0600 "$f.tmp"
mv "$f.tmp" "$f"
restorecon -R "$dir"
