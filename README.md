<p align="center">
  <img src="docs/icon.png" width="128" alt="Afterglow icon">
</p>

<h1 align="center">Afterglow</h1>

<p align="center">
  A two-deck DJ app that comes with its own music, so you can start mixing right away.
</p>

<p align="center">
  <a href="https://github.com/groverburger/afterglow/releases/latest"><img src="https://img.shields.io/github/v/release/groverburger/afterglow?label=download&color=f08a4b" alt="Latest release"></a>
  <a href="https://github.com/groverburger/afterglow/actions/workflows/build.yml"><img src="https://github.com/groverburger/afterglow/actions/workflows/build.yml/badge.svg" alt="Build status"></a>
  <img src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-8a5cf6" alt="Windows, macOS and Linux">
</p>

<p align="center">
  <img src="docs/screenshot.png" alt="Afterglow mixing two house tracks, with the radial visualizer running">
</p>

## Download

Get the latest build from [Releases](https://github.com/groverburger/afterglow/releases/latest):

- **Windows 10/11:** unzip and run `Afterglow.exe`.
- **macOS 11+:** open the DMG and drag Afterglow into Applications.
- **Linux:** [build from source](#building-from-source).

The builds aren't code-signed yet. On Windows, click **More info → Run
anyway**. On macOS, right-click the app and choose **Open**.

## Features

- **Built-in music.** A generated library of house, techno and drum & bass.
- **Two decks and a mixer.** Sync, hot cues, beat loops, 3-band EQ, filter and echo.
  Everything stays on the beat.
- **Scratching.** Drag a waveform to scratch it like vinyl.
- **One-click transitions.** Sixteen built in, plus an editor to make your own.
- **Auto DJ.** Mixes through your library on its own.
- **Your music.** Shows your Music folder automatically. Plays WAV, MP3 and
  FLAC, plus M4A on macOS and Windows.
- **Clips.** Cut any part of a track into a reusable clip.
- **Set recording.** Record a mix, replay it live, or export it as a WAV.
- **Visuals.** Six audio-reactive visualizers, fullscreen with **F**.

Shortcuts: `Q`/`P` play deck A/B, `T` mix, `F` party mode, `V` next visual, `H` help.

## Building from source

You need CMake 3.20+ and a C++17 compiler. All other dependencies are in the repo.

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
```

On Linux, first install
`libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev libasound2-dev`.
Run the tests with `ctest --test-dir build -C Release`.

Publishing a GitHub release automatically attaches Windows and macOS builds.

## Acknowledgements

Built with [sokol](https://github.com/floooh/sokol),
[Dear ImGui](https://github.com/ocornut/imgui),
[dr_libs](https://github.com/mackron/dr_libs) and the
[Roboto](https://fonts.google.com/specimen/Roboto) typeface.
