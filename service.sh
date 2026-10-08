#!/system/bin/sh
# ==========================================================================
#  Magisk late_start service
#
#  Starts the built-in web page once boot has settled, unless the file
#  `disable_http` exists in the module folder. It reads nothing and writes
#  nothing - the page reads on demand.
# ==========================================================================

MODDIR=${0%/*}

i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 120 ]; do
    sleep 5
    i=$((i + 1))
done

# let the charger driver / fuel gauge settle
sleep 20

[ -e "$MODDIR/disable_http" ] || \
    sh "$MODDIR/system/bin/batteryhealth" --serve >/dev/null 2>&1

exit 0
