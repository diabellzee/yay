#!/system/bin/sh
# Runs when the module is removed. Put back everything the optional daemons
# changed so removing yay leaves the device the way it was.
#
# Deliberately does NOT touch Private DNS: PingPimp's uninstall forced
# private_dns_mode=off, which silently overrides whatever the user had set.
# yay_dns only ever writes the two Settings keys and leaves them as-is.

MODDIR=${0%/*}

# Network engine: stop it and undo every knob / rule / setting from the
# persisted snapshot (sysctls reset at reboot anyway; iptables chains and
# Settings keys would otherwise outlive the module).
[ -x "$MODDIR/bin/yay_net" ] && "$MODDIR/bin/yay_net" --restore >/dev/null 2>&1
[ -x "$MODDIR/bin/yay_dns" ] && "$MODDIR/bin/yay_dns" --stop >/dev/null 2>&1
pkill -x yay_watch >/dev/null 2>&1

exit 0
