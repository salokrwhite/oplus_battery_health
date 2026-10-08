#!/system/bin/sh
# ==========================================================================
#  Magisk / KernelSU / APatch "Action" button
#
#  Starts the built-in page and opens it in the browser. Nothing else: the page
#  stays idle and only reads when you press 读取电池健康, so neither the button
#  nor the page load ever waits for a reading.
# ==========================================================================

MODDIR=${0%/*}

sh "$MODDIR/system/bin/batteryhealth" --action >/dev/null 2>&1

exit 0
