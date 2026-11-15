#!/bin/sh
# Installs and enables the crate-power-limit service system-wide. Decks
# runs it with pkexec from its system check, only after the user accepts.
#   install.sh               installs, enables and starts it
#   install.sh --uninstall   stops, disables and removes it; keeps the values
# The first install writes /etc/default/crate-power-limit with 45 W on AC and
# the firmware's current limit on battery; an existing file is kept.
#   DESTDIR, for tests: installs under it and touches no running service
#   CRATE_POWER_SYSFS (default /sys/class), for tests
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dest=${DESTDIR:-}
sysfs=${CRATE_POWER_SYSFS:-/sys/class}
bin="$dest/usr/local/bin/crate-power-limit"
unit="$dest/etc/systemd/system/crate-power-limit.service"
rule="$dest/etc/udev/rules.d/90-crate-power-limit.rules"
sleep_hook="$dest/usr/lib/systemd/system-sleep/crate-power-limit"
defaults="$dest/etc/default/crate-power-limit"

live() {
    [ -z "$dest" ]
}

if [ "${1:-}" = --uninstall ]; then
    if live; then
        systemctl disable --now crate-power-limit.service 2>/dev/null || true
    fi
    rm -f "$bin" "$unit" "$rule" "$sleep_hook"
    if live; then
        systemctl daemon-reload
        udevadm control --reload
    fi
    echo "crate-power-limit: removed ($defaults kept)"
    exit 0
fi

install -Dm755 "$here/crate-power-limit" "$bin"
install -Dm644 "$here/crate-power-limit.service" "$unit"
install -Dm644 "$here/90-crate-power-limit.rules" "$rule"
install -Dm755 "$here/crate-power-limit.sleep" "$sleep_hook"

if [ ! -e "$defaults" ]; then
    # On battery, the limit the firmware set before this service changes it
    battery_watts=20
    firmware="$sysfs/powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw"
    if [ -r "$firmware" ]; then
        microwatts=$(cat "$firmware")
        [ "$microwatts" -gt 0 ] 2>/dev/null && battery_watts=$((microwatts / 1000000))
    fi
    mkdir -p "$(dirname "$defaults")"
    cat > "$defaults" <<VALUES
# Sustained CPU package power limit (PL1) that crate-power-limit sets, in watts
CRATE_PL1_AC_WATTS=45
CRATE_PL1_BATTERY_WATTS=$battery_watts
VALUES
fi

if live; then
    systemctl daemon-reload
    udevadm control --reload
    systemctl enable --now crate-power-limit.service
fi
echo "crate-power-limit: installed"
