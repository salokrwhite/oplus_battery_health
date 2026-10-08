#!/system/bin/sh
# Magisk uninstall hook

MODDIR=${0%/*}

# stop the built-in web server if it is running
sh "$MODDIR/system/bin/batteryhealth" --serve-stop >/dev/null 2>&1
rm -f /data/local/tmp/oppo_battery_health_httpd.pid

# leftovers from earlier versions (the module no longer writes result files)
rm -f /data/local/tmp/battery_health.txt
rm -f /data/adb/modules/oppo_battery_health/battery_health.txt
rm -rf /data/adb/oppo_battery_health

exit 0
