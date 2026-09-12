#!/usr/bin/env bash
# Set up this build of Xenia on a Steam Deck.
#
# Installing the emulator was never the awkward part. These are the five steps
# that were, none of which a sandboxed application can do for itself:
#
#   1. fetch the newest build
#   2. add it to Steam, so gaming mode can launch it
#   3. give that entry artwork, instead of a placeholder and a file name
#   4. let a USB guitar be reached at all (a udev rule, and loading xpad,
#      neither of which SteamOS does)
#   5. install the userspace guitar driver the kernel one cannot replace
#
# Run it from a checked out tree, or on its own:
#   curl -fsSL https://raw.githubusercontent.com/peerloomllc/xenia-canary/linux-native-work/packaging/steam-deck/deck-setup.sh | bash
set -euo pipefail

REPO="${XENIA_REPO:-peerloomllc/xenia-canary}"
BRANCH="${XENIA_BRANCH:-linux-native-work}"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH/packaging/steam-deck"
BIN="$HOME/.local/bin"
APP="$HOME/Applications"
NAME="Xenia Canary"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
die() { printf '\n\033[31mStopped: %s\033[0m\n' "$1" >&2; exit 1; }

# Files come from the checked out tree when there is one, and from the
# repository when this script was piped in on its own.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-/nonexistent}")" 2>/dev/null && pwd || true)"
fetch() { # fetch <name> <destination>
  if [ -n "$HERE" ] && [ -f "$HERE/$1" ]; then
    install -Dm644 "$HERE/$1" "$2"
  else
    curl -fsSL "$RAW/$1" -o "$2" || die "could not fetch $1"
  fi
}

# In a subshell: /etc/os-release sets NAME, and sourcing it here would rename
# the Steam entry after the operating system. It did exactly that once.
os_id="$(. /etc/os-release 2>/dev/null && printf '%s' "${ID:-}")"
if [ "$os_id" != "steamos" ]; then
  note "This is written for SteamOS; ${os_id:-this system} may differ."
fi
command -v curl >/dev/null || die "curl is needed"
command -v python3 >/dev/null || die "python3 is needed"

# ---------------------------------------------------------------- 1. the build
say "Fetching the newest build"
mkdir -p "$APP" "$BIN"
# The newest release that actually carries one, not simply the newest: a
# release whose build failed (a dependency host returning 504, say) has no
# assets at all, and stopping there would be an odd thing to do to someone
# who just wants it installed.
api="https://api.github.com/repos/$REPO/releases?per_page=10"
url="$(curl -fsSL "$api" | python3 -c '
import json,sys
for r in json.load(sys.stdin):
    if r.get("draft"):
        continue
    for a in r.get("assets", []):
        if a["name"].endswith(".AppImage"):
            print(a["browser_download_url"]); sys.exit()
')"
[ -n "$url" ] || die "no release of $REPO carries an AppImage"
note "$(basename "$url")"
curl -fL --progress-bar "$url" -o "$APP/xenia_canary.AppImage"
chmod +x "$APP/xenia_canary.AppImage"

# ------------------------------------------------------- 5. the guitar driver
# Before the Steam entry, because the launcher refers to it.
say "Installing the launcher and the guitar driver"
fetch gip-guitar.py "$BIN/gip-guitar.py"
fetch xenia-deck.sh "$BIN/xenia-deck.sh"
chmod +x "$BIN/gip-guitar.py" "$BIN/xenia-deck.sh"
sed -i "s|^XENIA=.*|XENIA=\"\${XENIA:-$APP/xenia_canary.AppImage}\"|" "$BIN/xenia-deck.sh"
note "$BIN/xenia-deck.sh"

# ------------------------------------------------------------ 4. the host side
say "Letting a USB guitar be reached"
if [ "$(id -u)" -eq 0 ]; then
  sudo() { "$@"; }
fi
if sudo -n true 2>/dev/null || [ -t 0 ]; then
  tmp="$(mktemp -d)"
  fetch 70-crkd-guitar-xpad.rules "$tmp/70-crkd-guitar-xpad.rules"
  fetch 70-crkd-guitar.hwdb "$tmp/70-crkd-guitar.hwdb"
  # SteamOS keeps / read only but /etc takes changes, and they survive an
  # update. xpad is never loaded there, so nothing binds a guitar at all.
  sudo install -Dm644 "$tmp/70-crkd-guitar-xpad.rules" /etc/udev/rules.d/70-crkd-guitar-xpad.rules
  sudo install -Dm644 "$tmp/70-crkd-guitar.hwdb" /etc/udev/hwdb.d/70-crkd-guitar.hwdb
  echo xpad | sudo tee /etc/modules-load.d/xpad.conf >/dev/null
  sudo systemd-hwdb update || true
  sudo udevadm control --reload || true
  sudo modprobe xpad 2>/dev/null || true
  rm -rf "$tmp"
  note "udev rules installed, xpad set to load at boot"
else
  note "Skipped: no password prompt available. Re-run this in a terminal to"
  note "set up the guitar, or play without one."
fi

# ----------------------------------------------------------- 2 and 3. Steam
say "Adding it to Steam, with artwork"
if pgrep -x steam >/dev/null 2>&1; then
  note "Steam is running, and it rewrites its shortcut list when it exits."
  note "Close Steam, then run:"
  note "  $BIN/xenia-deck-steam-entry"
  cat > "$BIN/xenia-deck-steam-entry" <<EOF
#!/bin/sh
exec python3 "$BIN/steam-shortcut.py" --name "$NAME" --exe "$BIN/xenia-deck.sh" --art "$BIN/xenia-art"
EOF
  chmod +x "$BIN/xenia-deck-steam-entry"
  STEAM_LATER=1
fi
mkdir -p "$BIN/xenia-art"
for f in xeniap.png xenia.png xenia_hero.png xenia_logo.png; do
  fetch "$f" "$BIN/xenia-art/$f"
done
fetch steam-shortcut.py "$BIN/steam-shortcut.py"
chmod +x "$BIN/steam-shortcut.py"
if [ -z "${STEAM_LATER:-}" ]; then
  python3 "$BIN/steam-shortcut.py" --name "$NAME" --exe "$BIN/xenia-deck.sh" \
    --art "$BIN/xenia-art"
fi

say "Done"
note "In gaming mode you will find \"$NAME\" in your library."
if [ -n "${STEAM_LATER:-}" ]; then
  note "Close Steam and run $BIN/xenia-deck-steam-entry to add the entry."
fi
cat <<'EOF'

    Two things worth knowing:

    * Point it at your games with File > Game Library folder, the first time.
    * With a guitar plugged in, turn Steam Input OFF for this entry
      (gear > Properties > Controller). Steam otherwise replaces the guitar
      with an ordinary gamepad and the whammy and tilt stop working. The
      back paddles stop sending keys while it is off, so save states move
      back to a keyboard.
EOF
