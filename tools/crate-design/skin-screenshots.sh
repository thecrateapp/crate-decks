#!/usr/bin/env bash
# Renders the CrateDecks skin to PNG for the side-by-side comparison with the
# approved GU1 mockups (plan V2), with OpenGL, so that the waveforms show.
#
#   tools/crate-design/skin-screenshots.sh <output dir> [<tracks dir>] [<build dir>]
#
# It runs the CrateSkinScreenshots harness of mixxx-test inside a headless
# Mutter (a Wayland compositor with a virtual monitor and no output), which
# renders with Mesa's EGL on a render node: nothing opens on the desktop and
# nothing needs installing beyond Mutter. The compositor, the test and a
# private D-Bus without service activation live in a fresh runtime
# directory, so neither portals nor the keyring of the desktop session are
# reached. Without Mutter, use the offscreen harness, which has no OpenGL and
# leaves the waveforms empty:
#
#   CRATE_SKIN_SCREENSHOTS=<dir> build/mixxx-test --gtest_filter='CrateSkinScreenshots.*'
#
# The tracks directory is only read: never commit audio, screenshots or
# mockups. CRATE_SKIN_RUNTIME_DIR picks the runtime directory (its path must
# leave room for a Unix socket name, about 100 characters).
set -euo pipefail

if [[ $# -lt 1 ]]; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

out=$(realpath -m "$1")
tracks=${2:+$(realpath "$2")}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(realpath "${3:-$here/../../build}")

if ! command -v mutter >/dev/null; then
    echo "mutter is not installed: use the offscreen harness (see the header)" >&2
    exit 1
fi
if [[ ! -x "$build/mixxx-test" ]]; then
    echo "$build/mixxx-test not found: build it first" >&2
    exit 1
fi

runtime=${CRATE_SKIN_RUNTIME_DIR:-}
cleanup_runtime=0
if [[ -z "$runtime" ]]; then
    runtime=$(mktemp -d /tmp/crate-skin.XXXXXX)
    cleanup_runtime=1
fi
mkdir -p "$runtime" "$out"
chmod 700 "$runtime"

# A session bus that starts no services: a call to a portal or to the
# keyring fails at once instead of waiting for an activation
cat > "$runtime/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

cat > "$runtime/run.sh" <<'EOF'
#!/usr/bin/env bash
set -u
mutter --headless --wayland --no-x11 --virtual-monitor 1920x1080 \
    --wayland-display wl > "$XDG_RUNTIME_DIR/mutter.log" 2>&1 &
compositor=$!
for _ in $(seq 100); do
    [[ -S "$XDG_RUNTIME_DIR/wl" ]] && break
    sleep 0.1
done
if [[ ! -S "$XDG_RUNTIME_DIR/wl" ]]; then
    echo "headless mutter did not start:" >&2
    cat "$XDG_RUNTIME_DIR/mutter.log" >&2
    kill "$compositor" 2>/dev/null
    exit 1
fi
WAYLAND_DISPLAY=wl QT_QPA_PLATFORM=wayland \
    "$CRATE_SKIN_TEST" --gtest_filter='CrateSkinScreenshots.*'
status=$?
kill "$compositor" 2>/dev/null
wait "$compositor" 2>/dev/null
exit "$status"
EOF
chmod +x "$runtime/run.sh"

status=0
env -u DISPLAY -u WAYLAND_DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$runtime" \
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
    CRATE_SKIN_TEST="$build/mixxx-test" \
    CRATE_SKIN_SCREENSHOTS="$out" \
    CRATE_SKIN_TRACKS="$tracks" \
    dbus-run-session --config-file="$runtime/bus.conf" -- "$runtime/run.sh" || status=$?

if [[ $cleanup_runtime == 1 ]]; then
    rm -rf "$runtime"
fi
exit "$status"
