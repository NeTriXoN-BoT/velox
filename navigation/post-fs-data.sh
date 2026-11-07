#!/system/bin/sh
# Mount the optional layout only with the exact launcher it was built against.
MODDIR=${0%/*}
STOCK=/system_ext/priv-app/NexusLauncherRelease/NexusLauncherRelease.apk
EXPECTED=acb879bd26853c8a4a61cdb620a05f44c584a679b037b604987ff30ce3b98d0f
ACTIVE="$MODDIR/system/product/overlay/VeloxNavOrder.apk"
COMPATIBLE=true
ACTUAL=$(sha256sum "$STOCK" 2>/dev/null)
ACTUAL=${ACTUAL%% *}
[ "$ACTUAL" = "$EXPECTED" ] || COMPATIBLE=false
for UPDATED in /data/app/*/com.google.android.apps.nexuslauncher-*/base.apk; do
    [ -f "$UPDATED" ] && COMPATIBLE=false
done
if [ "$COMPATIBLE" = true ]; then
    cp "$MODDIR/VeloxNavOrder.apk" "$ACTIVE"
    chmod 644 "$ACTIVE"
    chcon u:object_r:system_file:s0 "$ACTIVE"
else
    # This file belongs only to this module; leave every other overlay untouched.
    rm -f "$ACTIVE"
fi
