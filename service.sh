#!/system/bin/sh
MODDIR=${0%/*}

# Wait for full boot
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 3
done

# One-shot: runs, applies, exits. No daemon.
"$MODDIR/bin/yay_apply" --boot

# Optional: start inotify watcher only if user explicitly enables it
# via sentinel file. Watcher sleeps in poll() with no timeout — zero wakeups.
#
# The sentinel lives in /data/adb/yay so it survives module updates. Older
# versions kept it inside the module dir, which an update wipes; carry such
# a leftover over once.
if [ -f "$MODDIR/enable_watch" ]; then
    touch /data/adb/yay/enable_watch && rm -f "$MODDIR/enable_watch"
fi
if [ -f /data/adb/yay/enable_watch ]; then
    "$MODDIR/bin/yay_watch" &
fi

# Auto Private DNS: Wi-Fi -> NextDNS, mobile data -> Cloudflare.
# Always safe to call: it exits immediately (0 cost) unless
# /data/adb/yay/config/dns_config.json has "enabled": true, and it
# detaches itself, so there is no `&` / subshell to manage here.
# Started AFTER yay_apply on purpose: yay_apply --boot rotates run.log, and
# the daemon must open the fresh file rather than the rotated one.
"$MODDIR/bin/yay_dns" --start >/dev/null 2>&1

# Network engine (PingPimp port): profiles, per-app boost/jail, Smart Auto.
# Same contract as yay_dns: exits immediately unless net_config.json has
# "enabled": true, detaches itself, event-driven afterwards.
"$MODDIR/bin/yay_net" --start >/dev/null 2>&1

# EEM (Extended Environment Monitor) undervolt offset.
# Backgrounded + delayed well past boot_completed.
# Not persisted anywhere by us — this only pokes a runtime kernel node.
# So a reboot (or module removal) alone reverts it to stock.
#(
#    sleep 60
#    [ -w /proc/eem/EEM_DET_B/eem_offset ]   && echo "-5" > /proc/eem/EEM_DET_B/eem_offset   2>/dev/null
#    [ -w /proc/eem/EEM_DET_L/eem_offset ]   && echo "-5" > /proc/eem/EEM_DET_L/eem_offset   2>/dev/null
#) &
#
exit 0
