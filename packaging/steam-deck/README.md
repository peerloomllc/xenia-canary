# Steam Deck

Installing the emulator was never the awkward part on a Deck. These are the
five steps that were, and `deck-setup.sh` does all of them:

1. **Fetch the newest build** (the AppImage from the latest release).
2. **Add it to Steam**, so gaming mode can launch it.
3. **Give that entry artwork**, instead of a placeholder and a file name.
4. **Let a USB guitar be reached at all.** SteamOS never loads `xpad`, and
   this guitar's id is in no driver's table.
5. **Install the userspace guitar driver**, because the kernel one binds this
   guitar and then never completes the start-up conversation it needs.

## Running it, without typing anything

A Deck has no keyboard, and the on-screen one makes any typed command
unpleasant. So the script is started from a file instead:

1. On the releases page, download **`Install-Xenia-on-Steam-Deck.desktop`**.
2. In the file manager, right-click it, choose **Properties**, then
   **Permissions**, and tick **Is executable**.
3. Double-click it. A terminal opens, the script runs, and it waits for you
   at the end.

Step 2 is there because a file that arrives from the internet is never
allowed to run on its own, which is a rule worth keeping.

From a terminal, the same thing is:

```sh
curl -fsSL https://raw.githubusercontent.com/peerloomllc/xenia-canary/linux-native-work/packaging/steam-deck/deck-setup.sh | bash
```

Close Steam first: it rewrites its shortcut list when it exits, and would undo
the entry. If Steam is running the script does everything else and leaves
`~/.local/bin/xenia-deck-steam-entry` to run once it is closed.

Your existing `shortcuts.vdf` is copied to `shortcuts.vdf.before-xenia` before
anything is written, and an entry for the same launcher is replaced rather
than duplicated.

## After it runs

- Point it at your games with **File > Game Library folder** the first time.
- **With a guitar, turn Steam Input off** for the entry (gear, Properties,
  Controller). Steam otherwise replaces the guitar with an ordinary gamepad,
  and the whammy and tilt stop working. The back paddles stop sending keys
  while it is off, so save states move back to a keyboard.

## What is in here

| File | What |
| --- | --- |
| `Install-Xenia-on-Steam-Deck.desktop` | what a user downloads and double-clicks; it runs the script below in a terminal |
| `deck-setup.sh` | the five steps |
| `steam-shortcut.py` | writes Steam's binary shortcut list and names the artwork by the app id Steam derives from the command and the name |
| `gip-guitar.py` | the userspace guitar driver |
| `xenia-deck.sh` | the launcher the Steam entry points at: starts the guitar driver when one is plugged in, and hides the Deck's own pad so the guitar is player one |
| `70-crkd-guitar-xpad.rules`, `70-crkd-guitar.hwdb` | the host rules the guitar needs |
| `xenia*.png` | the library artwork |

See `notes/81` and `notes/82` in the xenia-linux notes for why each of these
is the way it is.
