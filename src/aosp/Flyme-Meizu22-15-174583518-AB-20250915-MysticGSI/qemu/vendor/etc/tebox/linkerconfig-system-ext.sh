#!/system/bin/sh
# This GSI has no system_ext partition: /system_ext is a symlink to
# /system/system_ext. linkerconfig permits /system_ext/${LIB} in each
# namespace, but the linker checks a library's real path, so APEX libraries
# that are symlinks into /system_ext (Flyme's btservices links
# vendor.qti.hardware.btconfigstore@*.so there) are "not accessible" and
# Bluetooth crash-loops. Permit the real path next to every such entry.
# Runs after init.rc's perform_apex_config, before zygote starts.
f=/linkerconfig/ld.config.txt
[ -L /system_ext ] || exit 0
mark='# tebox: real /system_ext path permitted'
grep -qF "$mark" "$f" && exit 0
sed -i -e '/permitted\.paths += \/system_ext\/\${LIB}$/{p;s|/system_ext/|/system/system_ext/|}' "$f"
echo "$mark" >> "$f"
