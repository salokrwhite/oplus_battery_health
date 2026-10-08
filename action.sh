#!/system/bin/sh

MODDIR=${0%/*}

sh "$MODDIR/system/bin/batteryhealth" --action >/dev/null 2>&1

exit 0
