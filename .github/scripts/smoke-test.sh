#!/usr/bin/env bash
# Checks an unpacked Linux release (tools/package.py) on a distro it was not built on. Run as root in
# a fresh container of that distro (.github/workflows/build.yml): it installs what a desktop already
# has (python3, the Vulkan loader, the X11/Wayland/audio libraries the release leaves to the host),
# then checks without a game dump or a GPU:
#   - the programs find every library (ldd) and start: a missing one or a too-new glibc stops them;
#   - the game, never set up, says so; deception-setup rejects a folder that is not a dump, and
#     with one that looks like a dump runs prepare/run.py through the system python3 to its end;
#   - a set-up folder (paths.ini, game/) starts the game from it, logging to <save>/log.
# Usage: smoke-test.sh <unpacked release folder>
set -euo pipefail
release=$(readlink -f -- "$1")
if command -v apt-get >/dev/null; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends python3 libvulkan1 \
        libx11-6 libx11-xcb1 libxcb1 libxcb-render0 libxcb-shm0 libxext6 libxfixes3 libxrender1 libxi6 libxcursor1 libxrandr2 \
        libwayland-client0 libwayland-egl1 libwayland-cursor0 libxcb-shape0 libxcb-xfixes0 libasound2t64 libpulse0 libudev1 \
        libfontconfig1 libfreetype6 zlib1g libdrm2 libgbm1 libegl1 libgl1 libstdc++6 >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q python3 vulkan-loader libX11 libX11-xcb libxcb libXext libXfixes libXrender libXi \
        libXcursor libXrandr libwayland-client libwayland-egl libwayland-cursor alsa-lib pulseaudio-libs \
        systemd-libs fontconfig freetype zlib-ng-compat libdrm mesa-libgbm libglvnd-egl libglvnd-glx \
        libstdc++ >/dev/null
elif command -v pacman >/dev/null; then
    pacman -Sy --noconfirm --needed python vulkan-icd-loader libx11 libxcb libxext libxfixes libxrender \
        libxi libxcursor libxrandr wayland alsa-lib libpulse systemd-libs fontconfig freetype2 zlib \
        libdrm mesa libglvnd gcc-libs >/dev/null
fi
grep -m1 PRETTY_NAME /etc/os-release
ldd --version | sed -n 1p  # head would end ldd with SIGPIPE (pipefail)
fail() { echo "FAIL: $*" >&2; exit 1; }
# Without a display, as in a container: started without a terminal, the game and the setup would
# otherwise show their messages in a box and wait for it to be closed.
unset DISPLAY WAYLAND_DISPLAY
cd "$release"

for exe in deception deception-setup; do
    [[ -x $exe ]] || fail "$exe is missing or not executable"
    if ldd "$exe" | grep 'not found'; then fail "$exe: libraries missing (above)"; fi
done
status=0
./deception --cpu-features || status=$?
[[ $status == 0 || $status == 20 ]] || fail "deception --cpu-features exited $status"
usage=$(./deception-setup --help 2>&1 || true)
grep -q 'usage: deception-setup' <<<"$usage" || fail "deception-setup --help has no usage: $usage"

work=$(mktemp -d)
# Not set up: no paths.ini. No terminal (as from a file manager): the message goes to stderr.
rm -f paths.ini
status=0
./deception </dev/null >"$work/unset.log" 2>&1 || status=$?
grep -q 'The game is not set up yet' "$work/unset.log" || { cat "$work/unset.log"; fail 'no "not set up" message'; }
[[ $status == 1 ]] || fail "deception without paths.ini exited $status"

# A folder that is not a dump, then one that has the files the setup looks for (empty, so the
# preparation itself fails, in python3).
mkdir -p "$work/empty" "$work/dump/sce_sys" "$work/dump/sce_module" "$work/dump/FILE_PS4"
status=0
./deception-setup --game "$work/empty" --save "$work/save" >"$work/setup-empty.log" 2>&1 || status=$?
grep -q "isn't a decrypted Deception dump" "$work/setup-empty.log" ||
    { cat "$work/setup-empty.log"; fail 'the setup took a folder without eboot.bin'; }
for f in eboot.bin sce_sys/param.sfo sce_module/libc.prx sce_module/libSceFios2.prx FILE_PS4/archive00.lnk; do
    : >"$work/dump/$f"
done
status=0
./deception-setup --game "$work/dump" --save "$work/save" >"$work/setup-dump.log" 2>&1 || status=$?
cat "$work/setup-dump.log"
grep -q 'Preparing the game from' "$work/setup-dump.log" || fail 'the setup did not start preparing'
grep -q 'Preparing the game failed (exit code' "$work/setup-dump.log" || fail 'the preparation did not run to its end'
grep -q '^  ' "$work/setup-dump.log" || fail "no output of prepare/run.py reached the setup's log"
[[ $status == 1 ]] || fail "deception-setup with an empty dump exited $status"
[[ ! -e paths.ini ]] || fail 'a failed setup wrote paths.ini'

# Set up by hand: the game starts from paths.ini and logs to <save>/log (no terminal). The boot file
# is not a real one, so it stops there.
mkdir -p game
printf 'not a boot file' >game/boot-linked.bin
: >game/content.bin
cp release.txt game/release.txt
printf '[paths]\ngame=%s\nsave=%s\n' "$work/dump" "$work/save" >paths.ini
status=0
./deception </dev/null >"$work/launch.out" 2>&1 || status=$?
log=$work/save/log/deception.log
[[ -f $log ]] || { cat "$work/launch.out"; fail "no $log"; }
cat "$log"
grep -q "Launch: game $work/dump" "$log" || fail 'the game did not start from paths.ini'
(( status != 0 && status < 128 )) || fail "deception exited $status on a bad boot file (0 or a signal)"
rm -rf game paths.ini

echo "PASS: $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2)"
