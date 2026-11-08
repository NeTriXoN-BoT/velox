#!/system/bin/sh
# AVD-only adaptation of Magisk v31.1 scripts/avd_patch.sh.
# Copyright John Wu and Magisk contributors; GPL-3.0-only.
# Modified for Velox, 2026-11-08: bounded pinned-APK extraction and stock-only input.
# Source: https://github.com/topjohnwu/Magisk/blob/v31.1/scripts/avd_patch.sh
# Runs only in Velox's fresh private guest directory, using its verified APK.
set -eu
cd "${0%/*}"
[ "$(getprop ro.product.cpu.abi)" = x86_64 ]
[ "$(getprop ro.build.version.sdk)" = 35 ]
for tool in magisk magiskinit magiskboot init-ld; do
    /system/bin/unzip -p Magisk.zip "lib/x86_64/lib${tool}.so" > "$tool"
    chmod 755 "$tool"
done
./magiskboot decompress ramdisk.img ramdisk.cpio
cp ramdisk.cpio ramdisk.cpio.orig
# Refuse an already modified input. The host always supplies stock SDK data.
./magiskboot cpio ramdisk.cpio test
export KEEPVERITY=true KEEPFORCEENCRYPT=true
printf 'KEEPVERITY=true\nKEEPFORCEENCRYPT=true\n' > config
PREINITDEVICE=$(./magisk --preinit-device)
printf 'PREINITDEVICE=%s\n' "$PREINITDEVICE" >> config
./magiskboot compress=xz magisk magisk.xz
./magiskboot compress=xz init-ld init-ld.xz
./magiskboot cpio ramdisk.cpio \
    'add 0750 init magiskinit' \
    'mkdir 0750 overlay.d' \
    'mkdir 0750 overlay.d/sbin' \
    'add 0644 overlay.d/sbin/magisk.xz magisk.xz' \
    'add 0644 overlay.d/sbin/init-ld.xz init-ld.xz' \
    'patch' \
    'backup ramdisk.cpio.orig' \
    'mkdir 000 .backup' \
    'add 000 .backup/.magisk config'
./magiskboot compress=gzip ramdisk.cpio ramdiskpatched4AVD.img
