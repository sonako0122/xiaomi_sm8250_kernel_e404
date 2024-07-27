# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

properties() { '
kernel.string=NOVA Kernel by Project 113
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=
device.name2=
supported.versions=
'; }

is_apollo=0;
is_munch=0;
is_alioth=0;

block=/dev/block/bootdevice/by-name/boot;
ramdisk_compression=auto;

if [ $is_apollo == "1" ]; then
  is_slot_device=0;
elif [ $is_munch == "1" ]; then
  is_slot_device=1;
elif [ $is_alioth == "1" ]; then
  is_slot_device=1;
fi;

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
set_perm_recursive 0 0 750 750 $ramdisk/*;

## AnyKernel install
dump_boot;

# Begin Ramdisk Changes

# migrate from /overlay to /overlay.d to enable SAR Magisk
if [ -d $ramdisk/overlay ]; then
  rm -rf $ramdisk/overlay;
fi;

write_boot;
## end install

if [ $is_apollo == "0" ]; then 
  ## vendor_boot shell variables
  block=/dev/block/bootdevice/by-name/vendor_boot;
  is_slot_device=1;
  ramdisk_compression=auto;
  patch_vbmeta_flag=auto;

  # reset for vendor_boot patching
  reset_ak;

  # vendor_boot install
  dump_boot;

  write_boot;
  ## end vendor_boot install
fi;