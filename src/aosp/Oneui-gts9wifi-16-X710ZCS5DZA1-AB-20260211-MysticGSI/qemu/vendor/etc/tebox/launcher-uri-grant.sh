#!/system/bin/sh
# OneUI SystemUI reads content://com.sec.android.app.launcher.settings
# (KshDataUtils.isDexDisplay, on every ACTION_CLOSE_SYSTEM_DIALOGS / Home) but
# this GSI's SystemUI shared uid does not request the launcher's READ_SETTINGS,
# so the call throws SecurityException and kills SystemUI. A persisted URI
# grant on that authority satisfies ContentProviderHelper's
# checkAuthorityGrants() without touching system.img.
f=/data/system/urigrants.xml
auth=com.sec.android.app.launcher.settings
grant='<uri-grant sourceUserId="0" targetUserId="0" sourcePkg="com.sec.android.app.launcher" targetPkg="com.android.systemui" uri="content://'$auth'" prefix="true" modeFlags="1" createdTime="0" />'

if [ ! -s "$f" ]; then
  echo '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>' > "$f.tmp"
  echo "<uri-grants>$grant</uri-grants>" >> "$f.tmp"
elif grep -q "$auth" "$f"; then
  exit 0
else
  # system_server normally writes binary XML; append the grant as text.
  { abx2xml "$f" - 2>/dev/null || cat "$f"; } | sed "s#</uri-grants>#$grant</uri-grants>#" > "$f.tmp"
  grep -q "$auth" "$f.tmp" || { rm -f "$f.tmp"; exit 1; }
fi
chown system:system "$f.tmp"
chmod 0600 "$f.tmp"
mv "$f.tmp" "$f"
restorecon "$f"
