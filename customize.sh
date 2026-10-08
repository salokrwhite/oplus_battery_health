#!/system/bin/sh

SKIPMOUNT=false
PROPFILE=false
POSTFSDATA=false
LATESTARTSERVICE=true

ui_print " "
ui_print "  OPPO Battery Health (SOH)"
ui_print "  reads battery health and capacity state"
ui_print "  KernelSU / APatch users get a WebUI to read it on demand"
ui_print " "

set_perm_recursive "$MODPATH" 0 0 0755 0644

set_perm_recursive "$MODPATH/system" 0 0 0755 0755
set_perm "$MODPATH/system/bin/batteryhealth" 0 0 0755
set_perm "$MODPATH/system/bin/battcore.arm64" 0 0 0755
set_perm "$MODPATH/system/bin/battcore.arm" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644

set_perm_recursive "$MODPATH/bin" 0 0 0755 0644

ui_print "- installed."
ui_print "  Magisk            : tap ACTION on the module card to open the page"
ui_print "  KernelSU / APatch : open the module -> WebUI"
ui_print "  terminal          : su -c batteryhealth"
