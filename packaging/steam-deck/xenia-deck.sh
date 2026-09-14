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
if [ -x "$HERE/gip-guitar.py" ]; then
  # Start it whether or not a guitar is plugged in: it waits for one and
  # reattaches, so a guitar connected after the emulator is already up works
  # without restarting anything. Checking once here meant plugging in late
  # did nothing for the whole session.
  "$HERE/gip-guitar.py" >> "${LOG%.log}-guitar.log" 2>&1 &
  helper=$!
fi

if guitar_present; then
  sleep 2
  # The Deck's own pad used to be hidden from the emulator entirely while a
  # guitar was attached, so the guitar would be player one: a slot goes to
  # whichever controller claims it first and the Deck's pad claims slot 0,
  # which left a band game looking at a gamepad in player one's seat and
  # asking for the rest of the band (notes/81). The cost was that the Deck's
  # own buttons could not work the emulator's menus or game library at all.
  #
  # --ui_only_controllers below does the same job without that cost: the
  # Deck's pad drives the menus and the library, is given the last slot, and
  # is never reported to a title, so the guitar is still player one.
  # Steam also tells SDL to add its virtual pad whatever the lists say, and
  # that one is a duplicate of the Deck's own controls.
  export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=0
fi

# The guide button opens the emulator's menus everywhere else, but on a Deck
# Steam takes it for its own overlay and the emulator never sees it. View and
# Menu pressed together is free, works whether or not Steam Input is on, and
# cannot collide with what a title reads, since a title sees nothing at all
# while those menus are up.
#
# XENIA_LOG_LEVEL=3 turns on the per-slot guitar diagnostic, which logs what a
# title actually receives once a second.
"$XENIA" --apu=sdl --gamepad_ui_button=back+start \
  --ui_only_controllers="steam deck,steam controller,steam virtual gamepad" \
  ${XENIA_LOG_LEVEL:+--log_level=$XENIA_LOG_LEVEL} "$@" > "$LOG" 2>&1
status=$?

[ -n "$helper" ] && kill "$helper" 2>/dev/null
exit $status
