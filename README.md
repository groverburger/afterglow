# Dummy DJ Set

A beginner-proof two-deck DJ tool in C++17 with Dear ImGui and sokol. It
ships with its own procedurally generated music, so it works out of the box.

## Build & run

```sh
make        # builds ./build/dummydj (macOS: Metal + CoreAudio; Linux: GL + ALSA)
make run
```

The only requirement is a C++17 compiler. All dependencies are vendored in
`third_party/` (sokol, Dear ImGui, dr_libs); see `third_party/VERSIONS.txt`.

## What's inside

**Music.** Eight original tracks are synthesized on first use, one per style:
synth house, techno, deep house, drum & bass, trance, lo-fi, acid techno and
progressive house. Each has a drums-only intro and outro so it is easy to mix.
To add your own music, drop WAV/MP3/FLAC files onto the window, or put them
in `./music` to load at startup. Imported files are auto-analyzed for tempo,
beat grid and loudness.

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
- Clips get click-free fades and keep their beat grid. They are saved as WAV
  files in `user_data/clips` and reload at startup.
- Any active deck loop can also be saved as a clip with one click.

**Transitions.**
- There are 9 stock transitions: Smooth Crossfade, Bass Swap, Filter Sweep,
  Echo Out, Backspin, Power Down, Quick Cut, Long EQ Blend and Wash Out.
- **MIX** waits for the next bar, beat-matches the incoming track, then
  automates the mixer. While it runs, the automated controls turn yellow and
  are locked.
- In the **Transition Editor** tab, duplicate a stock transition or start a new
  one. You can draw keyframe curves for 13 parameters (crossfader, volume, EQ,
  filter and echo for both the outgoing and incoming decks). You can also set
  when the incoming deck starts and add a backspin or power-down. Your
  transitions auto-save to `user_data/transitions.txt`.
- **Auto DJ** plays through the library by itself. It supports library order,
  shuffle, or best tempo/key match.

**Visuals.** Six audio-reactive modes: Spectrum, Radial Burst, Spectrogram,
Vectorscope, Tunnel and Oscilloscope. Party mode (F) shows them fullscreen
with a now-playing overlay.

## Dummy-proofing

- SYNC and quantize are on by default. Play, cues, loops and transitions all
  land on the beat.
- Every track is loudness-normalized, and a master limiter keeps the output
  from clipping.
- You're asked to confirm before loading over a deck that's on air. Ejecting
  a live deck is disabled.
- Every knob or fader resets with a double-click. **Reset FX** is the panic
  button.
- Tempos too far apart to beat-match fall back to an unsynced blend instead of
  sounding awful. DnB can mix with half-time tracks.
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

`DUMMYDJ_TOUR=1 ./build/dummydj` runs an automated tour through every screen
and then quits. It's a quick smoke test.
