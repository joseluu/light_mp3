# light_mp3 — Native Win32 C++ MP3 Player

## Environment

| Item | Value |
|---|---|
| Shell | Git Bash (MINGW64) |
| Compiler | MSVC cl.exe 14.43.34808 (VS 2022 Build Tools, x64) |
| Windows SDK | 10.0.22621.0 |
| Toolchain root | `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools` |
| SDK root | `C:/Program Files (x86)/Windows Kits/10` |

VS 2022 **Community** (`C:/Program Files/Microsoft Visual Studio/2022/Community`) has
**no C++ compiler installed** (only clang-format/clang-tidy).  The **BuildTools** edition
in `Program Files (x86)` has the full MSVC toolchain.

## Build

```bash
bash build.sh
```

Output: `build/light_mp3.exe`

### How build.sh works

No make/nmake/cmake is used. The script:
1. Calls `rc.exe` to compile `res/app.rc` → `build/app.res` (embeds the manifest).
2. Writes a **compile response file** (`build/compile.rsp`) and invokes `cl.exe /c`
   (compile only) to produce `.obj` files.
3. Writes a **link response file** (`build/link.rsp`) and invokes `link.exe` to
   produce the final executable.

Response files avoid Git Bash quoting issues with paths that contain spaces.
The `/link` separator does **not** work inside a cl.exe response file, so compile
and link are separate steps.

### Linker flags summary

| Lib | Purpose |
|---|---|
| user32 / gdi32 | Window, DC, basic drawing |
| gdiplus | Cover art decode + render (JPEG/PNG via IStream) |
| winmm | `waveOut*` PCM audio output |
| comctl32 | ListView (metadata), Trackbar (seek bar) |
| comdlg32 | `GetOpenFileNameW` file picker |
| ole32 | `CreateStreamOnHGlobal` (cover art IStream) |
| shell32 | `DragQueryFileW` (drag-and-drop) |

## Project Structure

```
light_mp3/
 build.sh          — direct cl.exe build script (Git Bash)
 CLAUDE.md         — this file
 res/
   app.manifest    — ComCtl32 v6 + DPI-aware manifest
   app.rc          — resource script (embeds manifest as RT_MANIFEST)
 src/
   main.cpp        — WinMain, window proc, UI layout, GDI+ cover art
   id3v2.cpp/.h    — ID3v2.2/2.3/2.4 tag parser (no third-party libs)
   player.cpp/.h   — minimp3 decode + waveOut playback
   minimp3.h       — public-domain single-header MP3 decoder (lieff/minimp3)
   minimp3_ex.h    — minimp3 extended API (file loading)
 build/            — generated artefacts (cl.rsp, *.obj, *.res, .exe)
```

## Key Design Decisions

### Audio: minimp3 + waveOut
MP3 decoding is done entirely in-process using **minimp3** (public-domain,
header-only, ~1800 lines of C).  The entire file is decoded to 16-bit PCM
on `Open()`, then streamed to the audio device via `waveOut*` with 4-buffer
rotation.  `WM_PLAYER_DONE` (WM_APP+1) is posted when all PCM has been played.

Previous approaches (MCI, DirectShow) both failed on this system:
- MCI error 277 — requires optional "Windows Media Player" Windows feature.
- DirectShow — no MP3 decoder filter registered.
Embedding the decoder eliminates all codec dependencies.

### Cover Art: GDI+
Raw JPEG/PNG bytes from the APIC frame are wrapped in an `IStream`
(`CreateStreamOnHGlobal`) and passed to `Gdiplus::Bitmap::FromStream`.
The bitmap is rendered inside a dedicated `CoverArtClass` child window
(isolates WM_PAINT, prevents flicker via WM_ERASEBKGND = 1).

### Metadata: custom ID3v2 parser (`id3v2.cpp`)
Handles ID3v2.2, v2.3 and v2.4.  Key features:
- Synchsafe integer decoding for tag-header size and v2.4 frame sizes.
- Four text encodings: Latin-1, UTF-16 (BOM), UTF-16BE, UTF-8.
- TXXX custom frames (e.g. `MPM`, `Energy`, `Key`) mapped into `customTags`.
- APIC attached-picture extraction for cover art.
- Genre string normalisation (strips `(N)` numeric prefix).
All discovered text frames land in `allTextFrames`; custom frames also in
`customTags`.  The UI displays them all in the ListView.

### UI Layout (fixed 740 × 370 client area)
```
 [Cover 200×200]  [ListView: Tag | Value — all ID3 fields]
 [filename]
 [Open] [|<<] [Play/Pause] [Stop] [>>|]   [time / length]
 [========== seek trackbar ===========]
```
The seek bar maps player position to 0–1000 (`TBM_SETRANGE`).
A 500 ms `WM_TIMER` refreshes position and time label.
Drag-and-drop (`WM_EX_ACCEPTFILES` + `WM_DROPFILES`) and
a command-line argument both trigger `OpenFile()`.

## Extending

- **Playlist**: add a second ListView or ListBox below the controls; store
  a `std::vector<std::wstring>` of paths; wire `|<<` / `>>|` buttons.
- **Volume**: `waveOutSetVolume(hWaveOut, vol)` with packed L/R 16-bit values.
- **ReplayGain / more TXXX tags**: already parsed — just add rows in `BuildTagRows`.
- **PyInstaller packaging** (previous discussion): replaced by the native .exe — no
  runtime or packaging needed; `build/light_mp3.exe` is self-contained on Windows 10+.
