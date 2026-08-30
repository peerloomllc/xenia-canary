<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Canary - Xbox 360 Emulator</h1>

Xenia Canary is an experimental fork of the Xenia emulator. For more information, see the
[Xenia Canary wiki](https://github.com/xenia-canary/xenia-canary/wiki).

Come chat with us about **emulator-related topics** on [Discord](https://discord.gg/Q9mxZf9).
For developer chat join `#dev` but stay on topic. Lurking is not only fine, but encouraged!
Please check the [FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page before asking questions.
We've got jobs/lives/etc, so don't expect instant answers.

Discussing illegal activities will get you banned.

## This fork: native Linux work (peerloomllc)

This is [xenia-canary](https://github.com/xenia-canary/xenia-canary) at
commit `9d08d64b5` plus fixes and features for the **native Linux build**
(the Vulkan renderer, SDL audio, GTK UI), developed and tested on Fedora
with an NVIDIA GPU, mainly with Lost Odyssey, Eternal Sonata and Blue
Dragon. It is not affiliated with the Xenia project. The pieces that are
general fixes are offered upstream as separate pull requests; everything
else lives here. Branch: `linux-native-work`.

**Fixes**

* Guest threads sometimes never started on Linux (a resume lost between
  the thread publishing "started" and setting its own suspend count):
  Lost Odyssey hung at "Loading" about half the time. Upstream PR
  [#1187](https://github.com/xenia-canary/xenia-canary/pull/1187).
* Kernel timers with an absolute due time in the past never fired
  (Eternal Sonata was silent).
* The Linux mutex made a `gettid` syscall on every lock (39,000/s on the
  GPU thread); the ring buffer read pointer was published to the guest
  once per burst instead of as it advanced. Both cost frame rate.
* Any ImGui dialog crashed the native build when fontconfig picked a CFF
  CJK font.
* A title opened in the first seconds after the window appeared
  deadlocked the UI thread.

**Features**

* Save states: F8 saves, F10 loads, PageUp/PageDown pick one of nine
  slots per title (per disc for multi-disc titles), with a thumbnail and
  a slot table. LZ4-compressed files; the game pauses for a few hundred
  milliseconds. Guest memory, threads, kernel objects, the guest clock,
  GPU registers and EDRAM, XMA decoder state, the media player, mounted
  DLC and the signed-in profile are in the file. Single-player only;
  nothing online is saved.
* Pause (F7), fast-forward and slow-motion (numpad + - *) with
  time-stretched audio (SoundTouch), mute (Delete), an FPS overlay, all
  reassignable in-app.
* A GTK Preferences window (Settings menu): Graphics (output, colour
  filters, accuracy, performance), Audio, Input, Folders (games, content,
  save states), Patches (per-game patch toggles, lookup and download from
  the community patch repository), Profiles, Console.
* A game library dashboard when no title runs: scanned games folder,
  icons, list or grid view, launch.
* Menus: File, Emulation, Settings, Tools, Help; Reset Game and Close
  Game; the advanced GPU options in a dialog; per-title content listing.
* Diagnostic flags for guest-side investigation (`--stack_dump_interval_seconds`,
  `--watch_guest_pointer`, `--find_guest_refs`, `--find_guest_pattern`,
  `--poke_guest_memory`, `--trace_event_handles`, `--stats_log_seconds`,
  `--log_wait_reg_mem`); a save-state hang leaves the stalled thread's
  stack in the log.

**Building on Fedora (44)**

```sh
sudo dnf install clang cmake ninja-build python3 gtk3-devel lz4-devel sdl2-compat-devel \
    vulkan-loader-devel spirv-tools glslang libunwind-devel alsa-lib-devel libX11-devel
git clone --recursive -b linux-native-work https://github.com/peerloomllc/xenia-canary.git
cd xenia-canary
export CC=/usr/bin/clang CXX=/usr/bin/clang++
./xb build --config=release
build/bin/Linux/Release/xenia_canary --gpu=vulkan --apu=sdl
```

Other distributions: the same libraries under their own names; see
[docs/building.md](docs/building.md) for the Ubuntu package list. Releases
carry a binary built on Fedora 44 (glibc 2.43); it needs the GTK 3, SDL2,
Vulkan loader, lz4, libunwind and ALSA runtime libraries.

**Support development**: https://peerloomllc.com/about/

---

## Status

Buildbot | Status | Releases
-------- | ------ | --------
Canary (🪟, 🐧) | [![CI](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental)](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/xenia-canary/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/xenia-canary/xenia-canary/releases/latest) ◦ [All](https://github.com/xenia-canary/xenia-canary/releases) ◦ [Old](https://github.com/xenia-canary/xenia-canary-releases/releases)

### Experimental Netplay

Buildbot | Status | Releases
-------- | ------ | --------
Windows | [![Codacy Badge](https://app.codacy.com/project/badge/Grade/d814c4b6aa444dcc9c1631e0224b2739)](https://app.codacy.com/gh/AdrianCassar/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/AdrianCassar/xenia-canary/releases/latest)

## Quickstart

See the [Quickstart](https://github.com/xenia-canary/xenia-canary/wiki/Quickstart) page.

## FAQ

See the [frequently asked questions](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page.

## Game Compatibility

See the [Game compatibility list](https://github.com/xenia-canary/game-compatibility/issues)
for currently tracked games, and feel free to contribute your own updates,
screenshots, and information there following the [existing conventions](https://github.com/xenia-canary/game-compatibility/blob/canary/README.md).

## Building

See [building.md](docs/building.md) for setup and information about the
`xb` script. When writing code, check the [style guide](docs/style_guide.md)
and be sure to run clang-format!

## Contributors Wanted!

Have some spare time, know advanced C++, and want to write an emulator?
Contribute! There's a ton of work that needs to be done, a lot of which
is wide open greenfield fun.

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome (please!), but in addition to
that there are some major work areas still untouched:

* Help work through [missing functionality/bugs in games](https://github.com/xenia-canary/xenia-canary/labels/compat)
* Reduce the size of Xenia's [huge log files](https://github.com/xenia-canary/xenia-canary/issues/1526)
* Skilled with Linux? A strong contributor is needed to [help with porting](https://github.com/xenia-canary/xenia-canary/labels/platform-linux)

See more projects [good for contributors](https://github.com/xenia-canary/xenia-canary/labels/good%20first%20issue). It's a good idea to ask on Discord and check the issues page before beginning work on
something.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic
of emulation of modern devices and operating systems. **It is not for enabling
illegal activity**. All information is obtained via reverse engineering of
legally purchased devices and games and information made public on the internet
(you'd be surprised what's indexed on Google...).
