#!/usr/bin/env bash
#
# Builds and installs this fork.  Run it from the repository root.
#
# Exists mainly for macOS: Homebrew's sdl2 is an alias for sdl2-compat these
# days, and a machine that still carries the sdl2 it had before that move can
# link the program against one and SDL2_ttf against the other.  Two SDL2s end up
# in one process, and nothing says so -- asking for the version still works.
# This checks for it and puts it right.

set -euo pipefail

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf '\n\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f makefile ] && [ -f video_compare.cpp ] || die "Run this from the video-compare repository root."

JOBS=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

case "$(uname -s)" in
  Darwin)
    command -v brew >/dev/null || die "Homebrew is required: https://brew.sh"
    say "Checking dependencies"
    # Only reach for what is missing.  Whatever is already there may well have
    # come from another tap, and brew refuses to install over that -- but it
    # works just as well, so leave it alone.
    for formula in ffmpeg sdl2 sdl2_ttf; do
      if brew list --formula --versions "$formula" >/dev/null 2>&1; then
        note "$(brew list --formula --versions "$formula") -- already installed"
      else
        note "Installing $formula"
        brew install "$formula"
      fi
    done
    ;;
  Linux)
    say "Installing dependencies"
    if command -v apt >/dev/null; then
      sudo apt update
      sudo apt install -y build-essential libavformat-dev libavcodec-dev libavfilter-dev \
                          libavutil-dev libswscale-dev libswresample-dev libsdl2-dev libsdl2-ttf-dev
    elif command -v dnf >/dev/null; then
      sudo dnf install -y make gcc-c++ ffmpeg-devel SDL2-devel SDL2_ttf-devel
    else
      note "Unknown package manager -- install FFmpeg, SDL2 and SDL2_ttf development"
      note "packages yourself, then run this again."
      die "Cannot install dependencies automatically."
    fi
    ;;
  *)
    die "$(uname -s) is not handled here. On Windows, build in an MSYS2 MINGW64 shell."
    ;;
esac

# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------

say "Building"
make -j"$JOBS"

# --------------------------------------------------------------------------
# On macOS, make sure only one SDL2 will be loaded
# --------------------------------------------------------------------------

if [ "$(uname -s)" = "Darwin" ]; then
  say "Checking which SDL2 is in play"

  prefix=$(brew --prefix)
  pattern="$prefix/opt/sdl2[^/]*/lib/libSDL2-2\.0\.0\.dylib"
  ttf_dylib="$(brew --prefix sdl2_ttf)/lib/libSDL2_ttf-2.0.0.dylib"

  app_sdl2=$(otool -L video-compare | grep -oE "$pattern" | head -1 || true)
  ttf_sdl2=$(otool -L "$ttf_dylib" | grep -oE "$pattern" | head -1 || true)

  if [ -z "$app_sdl2" ] || [ -z "$ttf_sdl2" ]; then
    note "Could not tell -- skipping this check."
    note "  program:   ${app_sdl2:-not found}"
    note "  SDL2_ttf:  ${ttf_sdl2:-not found}"
  elif [ "$app_sdl2" = "$ttf_sdl2" ]; then
    note "Both use $(basename "$(dirname "$(dirname "$app_sdl2")")") -- good."
  else
    # SDL2_ttf decides: it is prebuilt and cannot be pointed elsewhere, so the
    # program is the one that has to move.
    want=$(basename "$(dirname "$(dirname "$ttf_sdl2")")")
    have=$(basename "$(dirname "$(dirname "$app_sdl2")")")

    note "The program links $have, SDL2_ttf links $want."
    note "Both would load; linking against $want instead."

    brew unlink sdl2 >/dev/null 2>&1 || true
    brew unlink sdl2-compat >/dev/null 2>&1 || true
    brew link --overwrite "$want" >/dev/null || die "Could not link $want."

    say "Rebuilding against $want"
    make -B -j"$JOBS"

    app_sdl2=$(otool -L video-compare | grep -oE "$pattern" | head -1 || true)
    [ "$app_sdl2" = "$ttf_sdl2" ] || die "Still mismatched: program has $app_sdl2, SDL2_ttf has $ttf_sdl2"
    note "Both use $want now."
  fi
fi

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------

say "Installing"
make install

installed=$(command -v video-compare || true)
if [ -n "$installed" ]; then
  note "$installed"
  note "$("$installed" --version)"
else
  note "Installed, but it is not on your PATH."
fi

say "Done"
note "Compare two videos:  video-compare a.mp4 b.mp4"
note "On a Retina display, add -d and press 4 for a pixel-for-pixel view."
note "See README.zh-CN.md for the rest."
