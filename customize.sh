#!/system/bin/sh
# Magisk installer hook

SKIPMOUNT=false
PROPFILE=false
POSTFSDATA=false
LATESTARTSERVICE=true

ui_print " "
ui_print "  OPPO Battery Health (SOH)"
ui_print "  reads battery health and capacity state"
ui_print "  KernelSU / APatch users get a WebUI to read it on demand"
ui_print " "

# default: every file 0644, every directory 0755
set_perm_recursive "$MODPATH" 0 0 0755 0644

# scripts must be executable
set_perm_recursive "$MODPATH/system" 0 0 0755 0755
set_perm "$MODPATH/system/bin/batteryhealth" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

# WebUI assets
set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644
set_perm_recursive "$MODPATH/webroot/cgi-bin" 0 0 0755 0755
set_perm "$MODPATH/webroot/cgi-bin/read" 0 0 0755

# DEX used by app_process to call the charger HAL
set_perm_recursive "$MODPATH/bin" 0 0 0755 0644

ui_print "- installed."
ui_print "  Magisk            : tap ACTION on the module card to open the page"
ui_print "  KernelSU / APatch : open the module -> WebUI"
ui_print "  terminal          : su -c batteryhealth"
