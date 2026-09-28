# Afterglow

A beginner-proof two-deck DJ app in C++17 with Dear ImGui and sokol. It
comes with its own library of procedurally generated house, techno and
drum & bass, so it works out of the box.

## Build & run

You need CMake 3.20+ and a C++17 compiler. Everything else is vendored in
`third_party/` (sokol, Dear ImGui, dr_libs; see `third_party/VERSIONS.txt`),
and the Roboto UI font is embedded at build time.

### macOS

```sh
brew install cmake        # once
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
open build/Afterglow.app
```

You can drag `build/Afterglow.app` into `/Applications` like any other app.

### Windows (Visual Studio 2022)

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
.\build\Release\Afterglow.exe
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install cmake g++ libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev libasound2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/Afterglow
```

Run the tests with `ctest --test-dir build -C Release`. CI
(`.github/workflows/build.yml`) builds and tests all three platforms.

Clips, custom transitions and the music drop folder live in a per-user data
folder:

- macOS: `~/Library/Application Support/Afterglow`
- Windows: `%APPDATA%\Afterglow`
- Linux: `~/.local/share/afterglow`

**File → Open music folder** opens it.

## How the build is set up

The whole build is one `CMakeLists.txt` at the repo root. There's no package
manager, no git submodules and no downloads at build time. Everything the app
needs is in the repository, so a fresh clone plus CMake and a compiler is
enough. The layout follows the sibling project `chartroom-cpp`: CMake,
`MACOSX_BUNDLE` for a double-clickable Mac app, an embedded UI font, and a
three-OS GitHub Actions matrix. The project started with a plain Makefile and
moved to CMake so it could build an app bundle and run on Windows and Linux.

### Targets

| Target | Kind | What it is |
|---|---|---|
| `imgui` | static lib | Dear ImGui core (`imgui*.cpp`; the demo window file is left out) |
| `sokol` | static lib | sokol app/gfx/glue/log/audio/time plus `sokol_imgui`, compiled as one translation unit |
| `dr_libs` | static lib | WAV/MP3/FLAC decoders (and the WAV writer), one C translation unit |
| `afterglow_core` | static lib | Everything without a UI: engine, DSP, transitions, sets, song generator, analysis, library |
| `afterglow` | app | The GUI: `Afterglow.app` on macOS, `Afterglow.exe` on Windows, `Afterglow` on Linux |
| `afterglow_set` | CLI tool | Records the scripted demo set and renders sets to WAV |
| `afterglow_tests` | test | Headless tests, registered with CTest as `engine` |

```
afterglow ──► afterglow_core ──► dr_libs
    │
    ├──► sokol ──► imgui
    └──► imgui
afterglow_set, afterglow_tests ──► afterglow_core   (no window, GPU or audio device)
```

`afterglow_core` is kept free of any UI or platform code on purpose. The tests
and the set tool link only against it, so they run in CI on machines with no
display or sound card.

### Vendored dependencies (`third_party/`)

- sokol, Dear ImGui and dr_libs were shallow-cloned from GitHub, their `.git`
  folders were removed, and they're committed as plain source. The exact
  commits are in `third_party/VERSIONS.txt`.
- The vendored code is unmodified. To update a library, replace its folder
  with a newer checkout and update the hash in `VERSIONS.txt`.
- sokol changes its API often (the 2026 releases replaced `sg_update_image`
  with `sg_write_image_transient`, for example). Check sokol's `CHANGELOG.md`
  when updating.
- These header-only libraries each need one file that compiles the
  implementation:
  - `src/platform/sokol_impl.cpp` defines `SOKOL_IMPL`, picks the graphics
    backend, and includes the sokol headers after `imgui.h`.
  - `src/platform/sokol_impl.mm` just includes that `.cpp`. On macOS, sokol
    has to be compiled as Objective-C++ (with ARC, `-fobjc-arc`) to talk to
    Cocoa and Metal, so CMake enables the `OBJCXX` language on Apple and
    builds the `.mm` instead.
  - `src/platform/dr_impl.c` defines the `DR_*_IMPLEMENTATION` macros.
- Third-party code is compiled without our warning flags. Our own targets use
  `-Wall -Wextra` (or `/W4` on MSVC) and build with zero warnings.

### Per-platform backends

| | Graphics | Audio | Linked against |
|---|---|---|---|
| macOS | Metal | CoreAudio (AudioToolbox) | Cocoa, QuartzCore, Metal, MetalKit, AudioToolbox frameworks |
| Windows | Direct3D 11 | WASAPI | kernel32, user32, shell32, gdi32, ole32, d3d11, dxgi |
| Linux | OpenGL 4.x core | ALSA | X11, Xi, Xcursor, GL, asound (found with `find_package`) |

- The backend is chosen by `#if` in `sokol_impl.cpp` (`SOKOL_METAL`,
  `SOKOL_D3D11`, `SOKOL_GLCORE`). The app code is identical on every platform.
- On Windows, the app is a `WIN32` (GUI) executable and sokol_app supplies
  `WinMain`. `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS` are defined for MSVC.
- Only the macOS build has been run so far. The Windows and Linux settings
  have been written to be correct, but they will first actually compile in CI
  on the first push to GitHub.

### Generated files

At configure time, CMake turns two assets into C++ headers under
`build/generated/`:

- `font_roboto.h` comes from `assets/Roboto-Regular.ttf`. The UI font is
  compiled into the binary, so the app doesn't depend on system fonts or on
  where it's launched from.
- `demo_set.h` comes from `assets/sets/Sunset to Jungle.set`. On first run the
  app writes it into the user's sets folder.

Both use `file(READ ... HEX)` to produce a byte array. Both files are listed in
`CMAKE_CONFIGURE_DEPENDS`, so editing either one automatically re-runs the
configure step on the next build. To regenerate the demo set after changing the
set script or the song generator:

```sh
./build/afterglow_set compose "assets/sets/Sunset to Jungle.set"
```

### The macOS app bundle

- `add_executable(afterglow WIN32 MACOSX_BUNDLE ...)` makes CMake produce
  `build/Afterglow.app`.
- The Info.plist comes from `cmake/Info.plist.in`, a custom template rather
  than CMake's default. It sets `NSHighResolutionCapable` (for a sharp Retina
  UI), the icon, the bundle id `com.afterglow.dj`, the music app category and
  macOS 11 as the minimum.
- `assets/Afterglow.icns` is copied into `Contents/Resources`. The icon is
  drawn procedurally by `tools/make_icon.py` (standard-library Python). To
  rebuild it:

  ```sh
  python3 tools/make_icon.py icon1024.png
  mkdir Afterglow.iconset
  for s in 16 32 128 256 512; do
    sips -z $s $s icon1024.png --out Afterglow.iconset/icon_${s}x${s}.png
    sips -z $((s*2)) $((s*2)) icon1024.png --out Afterglow.iconset/icon_${s}x${s}@2x.png
  done
  iconutil -c icns Afterglow.iconset -o assets/Afterglow.icns
  ```

- The bundle isn't code-signed. That's fine for an app you built yourself, but
  sharing it with other Macs would need signing and notarization.
- An app launched from Finder starts with `/` as its working directory. That's
  why the app never uses relative paths: user files go in the per-user data
  folder listed above, and the font and demo set are embedded.

### Build type, tests and CI

- If you don't pick a build type, it defaults to **Release**. Pass
  `-DCMAKE_BUILD_TYPE=Debug` for a debug build.
- `include(CTest)` adds the `BUILD_TESTING` option (on by default).
  `ctest --test-dir build -C Release` runs the test executable.
- The test covers:
  - every stock transition, sync and phase alignment, landing mode
  - BPM detection
  - transition file round trips
  - a recorded session replaying bit-identically
- `.github/workflows/build.yml` runs configure → build → ctest on
  `macos-14`, `windows-2022` and `ubuntu-24.04`, installing the X11/GL/ALSA
  dev packages on Linux. It uploads the built app for each OS as an artifact.
- `AFTERGLOW_TOUR=1` runs the GUI through every screen and quits (see the end
  of this README). This isn't part of CI because CI machines have no display.

### Editor support

`compile_flags.txt` gives clangd (and other clang-based tools) the include
paths, including `build/generated`, so the generated headers resolve. It also
passes `-xc++` so `.h` files are parsed as C++.

## What's inside

**Music.** The stock library is synthesized on first use:

- House: synth, deep, tech, disco, bass, progressive, and UK garage
- Techno: techno and acid techno
- Drum & bass: liquid, neurofunk, jump-up, jungle, and classic DnB
- Also trance and lo-fi

Every track has a drums-only intro and outro for easy mixing. To add your own
music, drop WAV/MP3/FLAC files onto the window or into the music folder.
Imported files are auto-analyzed for tempo, beat grid and loudness.

**Decks.** Each deck has play/cue, sync, a tempo slider with nudge, 4 hot cues,
beat loops (1/2 to 16 beats), and power-down/backspin effects. You also get
frequency-colored scrolling waveforms and an overview (click it to seek).

**Mixer.** Per channel: 3-band EQ with kills, a filter knob (low-pass to
high-pass), a tempo-synced ping-pong echo, a trim control and a fader. The
crossfader has three curves, and the master output has an always-on limiter.

**Clipping.**
- In the **Clip Editor** tab, drag over any track to select a region. Snap to
  beat or bar is on by default. You can preview the region, then save it as a
  clip or loop it on a deck.
- Clips get click-free fades, keep their beat grid, and are saved as WAV files.
- Any active deck loop can also be saved as a clip with one click.

**Transitions.**
- There are 9 stock transitions: Smooth Crossfade, Bass Swap, Filter Sweep,
  Echo Out, Backspin, Power Down, Quick Cut, Long EQ Blend and Wash Out.
- **MIX** waits for the next bar, beat-matches the incoming track, then
  automates the mixer. While it runs, the automated controls turn yellow and
  are locked.
- **Start vs Landing.** In **Start** mode, the incoming track plays from its
  playhead when the blend begins. In **Landing** mode, it *arrives* at its
  playhead when the blend ends. For example, put the playhead on the drop,
  pick an 8-bar blend, and MIX rewinds the track 8 bars so the drop hits right
  as the old track disappears. A yellow zone on the incoming waveform shows
  where the blend will begin.
- In the **Transition Editor** tab, duplicate a stock transition or start a new
  one. You can draw keyframe curves for 13 parameters, set when the incoming
  deck starts, and add a backspin or power-down. Your transitions auto-save.
- **Auto DJ** plays through the library by itself. It supports library order,
  shuffle, or best tempo/key match.

**Sets: record, replay live, export.**
- In the **Sets** tab, press **Record a set** and mix. Every action is
  captured against the audio sample clock: loads, play/cue, loops, hot cues,
  transitions, and every knob, fader and crossfader move.
- **Play live** replays a set through the real decks and mixer. Tracks load,
  knobs turn and transitions fire on their own. The engine is deterministic,
  so a replay is sample-identical to the original performance.
- Touch any control during a replay to take over the mix.
- **Export audio** renders a set to WAV in `~/Music/Afterglow` in the
  background.
- A demo set, *Sunset to Jungle* (25 minutes, 15 tracks, house to DnB), comes
  preinstalled. Start the app straight into it with:
  `open build/Afterglow.app --args --play-set "Sunset to Jungle"`
- Set files are plain text (`.set`). Custom transitions are embedded, so a set
  still plays after you edit or delete them.

**Visuals.** Six audio-reactive modes: Spectrum, Radial Burst, Spectrogram,
Vectorscope, Tunnel and Oscilloscope. Party mode (F) shows them fullscreen
with a now-playing overlay.

## Beginner-proofing

- SYNC and quantize are on by default. Play, cues, loops and transitions all
  land on the beat.
- Every track is loudness-normalized, and a master limiter keeps the output
  from clipping.
- You're asked to confirm before loading over a deck that's on air. Ejecting
  a live deck is disabled.
- Every knob or fader resets with a double-click. **Reset FX** is the panic
  button.
- Tempos too far apart to beat-match fall back to an unsynced blend. DnB can
  mix with half-time tracks.
- There are tooltips on every control, a quick-start guide (H), and a Match
  column in the Library that tells you which tracks fit.

Keys: `Q`/`P` play or pause deck A/B, `T` mix, `F` party mode, `V` next
visual, `H` help.

## Code map

| File | Role |
|---|---|
| `src/Engine.*` | Audio thread: decks, mixer, sync, loops, transition automation |
| `src/DSP.h` | EQ biquads, SVF filter, echo, limiter, interpolation |
| `src/Transitions.*` | Transition model, stock presets, save/load |
| `src/SynthGen.*` | Procedural song generator |
| `src/TrackIO.cpp` | Decoding, waveform analysis, BPM/beat-grid detection, WAV export |
| `src/Library.*` | Track library with background loading |
| `src/SetFile.*` | Set recording format, offline set rendering |
| `src/UISets.cpp` | Sets tab: record, live replay, export |
| `tools/afterglow_set.cpp` | Scripted demo set, set compose/render CLI |
| `src/Visuals.*` | FFT analysis and visualizers |
| `src/Widgets.*` | Knobs, faders, meters, waveform drawing |
| `src/UIDecks.cpp`, `src/UIPanels.cpp`, `src/main.cpp` | UI and app logic |
| `test/engine_test.cpp` | Headless engine, transition, landing, record/replay and BPM tests |

**Set tool.** `build/afterglow_set` is a command-line companion:

```sh
afterglow_set compose out.set [out.wav] [tracklist.txt]   # perform the scripted demo set and record it
afterglow_set render in.set out.wav [tracklist.txt]       # render any recorded set to WAV
```

`compose` renders its WAV by replaying the saved file, then checks that the
replay is bit-identical to the live performance. The demo set's script is at
the top of `tools/afterglow_set.cpp`. Regenerate the shipped copy with
`afterglow_set compose "assets/sets/Sunset to Jungle.set"`.

`AFTERGLOW_TOUR=1 build/Afterglow.app/Contents/MacOS/Afterglow` runs an
automated tour through every screen and then quits. It's a quick smoke test.

Third-party licenses: sokol (zlib), Dear ImGui (MIT), dr_libs (public domain
/ MIT-0), Roboto (Apache 2.0, `assets/Roboto-LICENSE.txt`).
