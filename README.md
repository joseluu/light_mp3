# light_mp3

A lightweight native Win32 MP3 player written in C++. No frameworks, no runtimes —
just the Windows API. Starts in a fraction of the time of media players like VLC.

## Features

- Plays MP3 files using a built-in decoder (minimp3) — no system codecs required
- Displays embedded cover art (JPEG or PNG)
- Shows all ID3v2 metadata: title, artist, album, year, genre, BPM, track number
- Shows custom tags such as **MPM**, Energy, Key, and any other TXXX frames written by
  tagging tools (Rekordbox, Traktor, Mp3tag, etc.)
- Seek bar with live position/duration display
- Drag-and-drop a file onto the window to open it
- Accepts a file path as a command-line argument (usable as a default app association)

## Size & dependencies

| Item | Detail |
|---|---|
| Executable | ~320 KB |
| Runtime | None — links against OS DLLs already loaded on any Windows 10/11 system |
| Third-party libraries | None |

## Building

Requirements: **VS 2022 Build Tools** (C++ workload) and **Windows 10 SDK 10.0.22621.0**,
both available free from Microsoft. Run from the project root in **Git Bash**:

```bash
bash build.sh
```

Output: `build/light_mp3.exe`

The build script calls `rc.exe`, `cl.exe` and `link.exe` directly — no make, nmake or
cmake involved. See `CLAUDE.md` for a detailed explanation of the toolchain setup.

## Usage

```
light_mp3.exe [file.mp3]
```

- **Open button** — file picker dialog
- **Drag and drop** — drop an MP3 onto the window
- **Play / Pause / Stop** — playback controls
- **Seek bar** — click or drag to jump to any position

## Project structure

```
build.sh        build script (Git Bash)
res/
  app.manifest  DPI-aware, ComCtl32 v6 manifest
  app.rc        resource script
src/
  main.cpp      Win32 UI, GDI+ cover art rendering
  id3v2.cpp/h   ID3v2 tag parser (v2.2, v2.3, v2.4)
  player.cpp/h  minimp3 decode + waveOut playback
  minimp3.h     public-domain MP3 decoder (lieff/minimp3)
  minimp3_ex.h  minimp3 extended API
build/          compiled output (generated)
```

## Technical notes

Audio playback uses **minimp3** (public-domain header-only library) to decode MP3 to PCM
in-process, then streams it to the audio device via the Windows `waveOut` API.
No system codecs, optional Windows features, or third-party DLLs are needed.
Cover art is decoded with **GDI+** (`Gdiplus::Bitmap::FromStream`) directly from the
bytes stored in the ID3v2 APIC frame — no temporary files.
The ID3v2 parser is written from scratch and handles all four text encodings
(Latin-1, UTF-16 LE/BE, UTF-8) as well as synchsafe integers for both tag and frame
sizes.
