SKIPUNZIP=0
[ "$(getprop ro.board.platform)" = "mt6785" ] || abort "! so mt6785"
set_perm_recursive $MODPATH/system/vendor/lib64 0 0 0755 0644 u:object_r:same_process_hal_file:s0
set_perm_recursive $MODPATH/system/vendor/etc 0 0 0755 0644 u:object_r:vendor_configs_file:s0
set_perm_recursive $MODPATH/system/bin 0 2000 0755 0755
