# Flatpak

A Flatpak of this fork, for immutable systems (SteamOS, Bazzite, the ROG Ally)
where nothing installs to the system folders and an AppImage is awkward.

## Installing

Download `xenia_canary_<version>.flatpak` from a release and:

```sh
flatpak install --user xenia_canary_<version>.flatpak
flatpak run io.github.peerloomllc.XeniaCanary
```

Updating means installing the next bundle over it.

### What it downloads

The emulator itself is about **38 MB**. The first install also pulls the
runtime it sits on: roughly **1.3 GB** to download, **2.8 GB** installed.
Measured on a Steam Deck that had no other Flatpaks on it:

| | Installed |
| --- | --- |
| Xenia Canary | 303 MB |
| GNOME runtime | 1.1 GB |
| Graphics drivers | 457 MB |
| Video codecs | 43 MB |

Most of that is **shared with every other Flatpak**, so it is paid once
rather than per app: anyone who already has one installed pays far less, and
updates to Xenia after the first install are tens of megabytes. `flatpak
install` prints these sizes and asks before downloading anything.

The AppImage is 49 MB because it carries only itself and borrows the rest
from the system, which is also why a system update can break it. On an
immutable system that is the problem the Flatpak solves.

## Building it yourself

```sh
flatpak install flathub org.gnome.Platform//49 org.gnome.Sdk//49 org.freedesktop.Sdk.Extension.llvm22//25.08
flatpak-builder --user --force-clean --repo=fp-repo fp-build packaging/flatpak/io.github.peerloomllc.XeniaCanary.yaml
flatpak build-bundle fp-repo xenia_canary.flatpak io.github.peerloomllc.XeniaCanary master
```

About 12 minutes of compiling on a fast machine.

## Why it is built the way it is

- **The GNOME runtime, not freedesktop.** The build hard-requires GTK 3
  (`pkg_check_modules(GTK3 REQUIRED gtk+-x11-3.0)`) and freedesktop does not
  carry it.
- **It builds libunwind, SDL2 and the shader tools itself.** No runtime ships
  them, and this tree builds SDL2 from source only on Windows.
- **The submodules it skips** are the three the Linux build never compiles:
  DirectXShaderCompiler (an LLVM fork, gigabytes on its own), DirectX-Headers
  and xbyak_aarch64.

## Permissions, and why

| Permission | Why |
| --- | --- |
| `--filesystem=host` | games, content and save states live wherever the user keeps them, and a second internal drive is the normal place for 100 GB of discs |
| `--filesystem=/run/media`, `/media` | an SD card on a handheld |
| `--device=all` | GPU, controllers, and raw USB for a guitar |
| `--share=network` | the Patches tab downloads from the community patch repository |
| `--socket=x11`, `--share=ipc` | the build forces `GDK_BACKEND=x11`, which is XWayland under gamescope |
| `--socket=pulseaudio` | audio |
| `--filesystem=xdg-run/gamescope-0` | a Steam Deck's gaming mode |

`--filesystem=home` was tried first and is not enough: games on a second drive
are invisible to the sandbox and the library comes up empty.

## What this does not do

On a Steam Deck the Flatpak is the front door, not the whole job. Adding the
entry to Steam, its artwork, and the udev rule a USB guitar needs are all
outside any sandbox.
