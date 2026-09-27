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
| `src/Visuals.*` | FFT analysis and visualizers |
| `src/Widgets.*` | Knobs, faders, meters, waveform drawing |
| `src/UIDecks.cpp`, `src/UIPanels.cpp`, `src/main.cpp` | UI and app logic |
| `test/engine_test.cpp` | Headless engine, transition, landing and BPM tests |

`AFTERGLOW_TOUR=1 build/Afterglow.app/Contents/MacOS/Afterglow` runs an
automated tour through every screen and then quits. It's a quick smoke test.

Third-party licenses: sokol (zlib), Dear ImGui (MIT), dr_libs (public domain
/ MIT-0), Roboto (Apache 2.0, `assets/Roboto-LICENSE.txt`).
