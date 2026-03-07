#!/usr/bin/env bash
# Build script for light_mp3.
# Run from project root under Git Bash. No make/nmake/cmake required.
set -euo pipefail

MSVC="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.43.34808"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDK_VER="10.0.22621.0"

CL="$MSVC/bin/Hostx64/x64/cl.exe"
LINK="$MSVC/bin/Hostx64/x64/link.exe"
RC="$SDK/bin/$SDK_VER/x64/rc.exe"

mkdir -p build

# ---------------------------------------------------------------------------
# Step 1 — Resources
# MSYS_NO_PATHCONV=1 prevents Git Bash from mangling /fo and /I flags.
# ---------------------------------------------------------------------------
echo "[1/3] Compiling resources..."
MSYS_NO_PATHCONV=1 "$RC" /nologo \
  /I"$SDK/Include/$SDK_VER/um" \
  /I"$SDK/Include/$SDK_VER/shared" \
  /fo build/app.res res/app.rc

# ---------------------------------------------------------------------------
# Step 2 — Compile C++ to object files (/c = compile only, no link)
# The response file avoids shell-quoting issues with paths containing spaces.
# ---------------------------------------------------------------------------
echo "[2/3] Compiling C++..."
cat > build/compile.rsp << RSP
/nologo
/std:c++17
/EHsc
/W3
/O2
/DUNICODE
/D_UNICODE
/c
/I"$MSVC/include"
/I"$SDK/Include/$SDK_VER/ucrt"
/I"$SDK/Include/$SDK_VER/um"
/I"$SDK/Include/$SDK_VER/shared"
/Fo"build/"
src/main.cpp
src/id3v2.cpp
src/player.cpp
RSP

"$CL" "@build/compile.rsp"

# ---------------------------------------------------------------------------
# Step 3 — Link
# ---------------------------------------------------------------------------
echo "[3/3] Linking..."
cat > build/link.rsp << RSP
/nologo
/SUBSYSTEM:WINDOWS
/OUT:"build/light_mp3.exe"
/LIBPATH:"$MSVC/lib/x64"
/LIBPATH:"$SDK/Lib/$SDK_VER/ucrt/x64"
/LIBPATH:"$SDK/Lib/$SDK_VER/um/x64"
build/main.obj
build/id3v2.obj
build/player.obj
build/app.res
user32.lib
gdi32.lib
gdiplus.lib
winmm.lib
comctl32.lib
comdlg32.lib
ole32.lib
shell32.lib
RSP

"$LINK" "@build/link.rsp"
echo "Done -> build/light_mp3.exe"
