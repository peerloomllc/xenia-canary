#!/bin/bash
# Xenia Canary (native Linux fork) on a Steam Deck, as the target of the
# non-Steam shortcut.
#
# Starts the userspace guitar driver first when a CRKD guitar is plugged in:
# SteamOS's kernel driver binds that guitar and never completes the GIP
# start-up conversation, so it sends nothing (notes/81). The helper does the
# conversation itself and republishes the guitar as an input device carrying
# its own USB ids, which is what Xenia already knows to treat as a guitar.
HERE="$(dirname "$(readlink -f "$0")")"
XENIA="${XENIA:-/home/deck/xenia_canary_fix.AppImage}"
LOG="${LOG:-/home/deck/xenia-gaming.log}"

guitar_present() {
  local d
  for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor")" = "0351" ] || continue
    case "$(cat "$d/idProduct")" in 4161|1300) return 0 ;; esac
  done
  return 1
}

helper=""
if guitar_present && [ -x "$HERE/gip-guitar.py" ]; then
  "$HERE/gip-guitar.py" >> "${LOG%.log}-guitar.log" 2>&1 &
  helper=$!
  sleep 2
  # Hide the Deck's own pad while a guitar is plugged in, so the guitar is
  # player one. A slot goes to whichever controller claims it first and the
  # Deck's pad claims slot 0, which leaves a band game looking at a gamepad in
  # player one's seat and asking for the rest of the band (notes/81). Steam
  # already sets this variable, so add to it rather than replacing it.
  # Steam's own value ends with a comma, and appending after it leaves an
  # empty entry that SDL stops on, so trim it first.
  ignore="${SDL_GAMECONTROLLER_IGNORE_DEVICES%,}"
  export SDL_GAMECONTROLLER_IGNORE_DEVICES="${ignore:+$ignore,}0x28de/0x1205"
  # Stronger and not dependent on that list parsing: show the guitar only.
  export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT="0x0351/0x4161,0x0351/0x1300"
  # Steam also tells SDL to add its virtual pad whatever the lists say.
  export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=0
fi

# XENIA_LOG_LEVEL=3 turns on the per-slot guitar diagnostic, which logs what a
# title actually receives once a second.
"$XENIA" --apu=sdl ${XENIA_LOG_LEVEL:+--log_level=$XENIA_LOG_LEVEL} "$@" > "$LOG" 2>&1
status=$?

[ -n "$helper" ] && kill "$helper" 2>/dev/null
exit $status
