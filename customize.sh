#!/system/bin/sh
# Runs during Magisk module installation (flash time).
# Sets up directory structure and runs a full first-time apply.

MODDIR="$MODPATH"
YAY_DATA="/data/adb/yay"
CFG_DIR="$YAY_DATA/config"
CACHE_DIR="$YAY_DATA/cache"

ui_print "- yay: setting up data directories..."
mkdir -p "$CFG_DIR" "$CACHE_DIR"
chmod 700 "$YAY_DATA" "$CFG_DIR" "$CACHE_DIR"

# Copy default configs only if not already present (preserve user edits on update)
for f in rules.json game_config.json io_config.json dns_config.json; do
    if [ ! -f "$CFG_DIR/$f" ]; then
        if [ -f "$MODDIR/config/$f" ]; then
            cp "$MODDIR/config/$f" "$CFG_DIR/$f"
            ui_print "  installed default $f"
        fi
    else
        ui_print "  keeping existing $f"
    fi
done

# Keep the watcher setting across this update. Older versions stored the flag
# inside the module directory, which the update replaces; the previous
# install is still on disk at this point, so carry the flag over.
if [ -f /data/adb/modules/yay/enable_watch ]; then
    touch "$YAY_DATA/enable_watch"
    ui_print "  watcher: kept enabled (flag migrated)"
fi

# Mark binaries as executable
chmod 755 "$MODDIR/bin/yay_apply"
chmod 755 "$MODDIR/bin/yay_watch"
chmod 755 "$MODDIR/bin/yay_inspect"
chmod 755 "$MODDIR/bin/yay_dns"

# ─── SystemUI MediaMetadata NPE Fix (Zygisk component) ──────────────────────
# Fingerprint-locked to one exact firmware build (see media_fix/module.cpp's
# isSupportedBuild(), which re-checks this at runtime on every load — this
# install-time check is purely to avoid shipping a dead .so on devices where
# it can never activate anyway). Unlike the rest of yay, this fix targets one
# specific TranSystemUI defect on one specific build, so it cannot be made
# device-agnostic the way rules/appops/game-mode/io-scheduler are.
#
# Critically: a firmware mismatch here must NOT abort the whole install.
# Only the Zygisk .so is skipped; every other yay feature still installs and
# works normally on any device.
MEDIA_FIX_SDK=31
MEDIA_FIX_FP='Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys'
MEDIA_FIX_SYSTEMUI_VERSION_CODE=202309220
MEDIA_FIX_SYSTEMUI_VERSION_NAME='12.0.2.187'

if [ -f "$MODDIR/zygisk/arm64-v8a.so" ]; then
    CURRENT_SDK="$(getprop ro.build.version.sdk)"
    CURRENT_FP="$(getprop ro.build.fingerprint)"
    CURRENT_ABIS="$(getprop ro.product.cpu.abilist)"
    PACKAGE_INFO="$(dumpsys package com.android.systemui 2>/dev/null)"
    CURRENT_SYSTEMUI_VERSION_CODE="$(printf '%s\n' "$PACKAGE_INFO" | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' | head -n 1)"
    CURRENT_SYSTEMUI_VERSION_NAME="$(printf '%s\n' "$PACKAGE_INFO" | sed -n 's/.*versionName=\([^[:space:]]*\).*/\1/p' | head -n 1)"

    MEDIA_FIX_SUPPORTED=1
    [ "$CURRENT_SDK" = "$MEDIA_FIX_SDK" ] || MEDIA_FIX_SUPPORTED=0
    [ "$CURRENT_FP" = "$MEDIA_FIX_FP" ] || MEDIA_FIX_SUPPORTED=0
    printf '%s' "$CURRENT_ABIS" | grep -q 'arm64-v8a' || MEDIA_FIX_SUPPORTED=0
    [ "$CURRENT_SYSTEMUI_VERSION_CODE" = "$MEDIA_FIX_SYSTEMUI_VERSION_CODE" ] || MEDIA_FIX_SUPPORTED=0
    [ "$CURRENT_SYSTEMUI_VERSION_NAME" = "$MEDIA_FIX_SYSTEMUI_VERSION_NAME" ] || MEDIA_FIX_SUPPORTED=0

    if [ "$MEDIA_FIX_SUPPORTED" = "1" ]; then
        ui_print "- yay: SystemUI MediaMetadata fix — firmware match, enabling"
        chmod 644 "$MODDIR/zygisk/arm64-v8a.so"
        ui_print "  Enable Zygisk (built-in, Zygisk Next, or ReZygisk) and reboot"
    else
        ui_print "- yay: SystemUI MediaMetadata fix — firmware mismatch, skipping"
        ui_print "  (targets Infinix X6815B, firmware 231020V486 only — safe no-op here)"
        rm -f "$MODDIR/zygisk/arm64-v8a.so"
        rmdir "$MODDIR/zygisk" 2>/dev/null
    fi
fi

# Auto Private DNS is opt-in: it needs your own NextDNS endpoint, which no
# default can supply. Nothing runs until it is enabled in the WebUI
# (Settings -> Auto Private DNS) or "enabled" is set in dns_config.json.
ui_print "- yay: Auto Private DNS is off by default"
ui_print "  Enable it in the WebUI (Settings) and enter your NextDNS host"

ui_print "- yay: running full first-time apply..."
"$MODDIR/bin/yay_apply" --full

ui_print "- yay: done."
