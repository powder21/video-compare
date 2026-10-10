# Changelog

Notable changes in this fork of [pixop/video-compare](https://github.com/pixop/video-compare).
Upstream releases keep their own date-stamped tags; this fork's own work is tagged `v1.x`.

## v1.6 — 2026-10-10

Caught up with upstream (pixop/video-compare to `20261004`, FFmpeg 9.0.2).

What upstream brings, as far as this fork is concerned:

- **Chinese file names stop showing up as boxes.** Upstream bundles a CJK
  fallback font (Sarasa Mono SC) and takes a font file of your own.
- **Files whose container starts before their video stream line up** (upstream
  #151) — squarely in the code stepping and sync live in.
- **Crops can be copied across** (`Ctrl+Shift+L/R`), **cleared on one side**
  (`Ctrl+L/R`) and **undone** (`Backspace`, which used to clear them all).
- `W` fits the window to the picture, `-U` scales the UI, the aspect ratio may
  change frame by frame, and `--conversion-fit native` places samples 1:1.
- Unit and integration tests run in CI.

What it took on this side:

- Sources moved to `src/` and vendored files to `third_party/`; tinyfiledialogs
  went along. `install.sh` looked for `video_compare.cpp` at the root and would
  have refused to run.
- Upstream moved working out seek targets into a function of its own, which
  knows one shift for every right video. Each right video now hands it its own
  step offset; without that, a stepped video lost its offset on the next seek.
- The v1.4 fix re-applied to the converter upstream rewrote: the default stretch
  path still converts on every core, the new padded native placements stay on
  one. A destination that is not refcounted falls back to one core rather than
  having swscale swap its buffer out. Measured again: in the same 1.8 s, a 4K
  59.94 fps pair played to 1.6 s threaded and 0.85 s on one core.
- `Shift+X` prints the frame number shown on each side (`frame_left=`,
  `frame_right=`), and a new `right-step` integration test asserts on it: step
  each right video on its own, keep both offsets through a global seek, clear
  them with Ctrl+0 — once with separate files and once in single-decoder mode.
  It fails when the step offset is taken out of the seek, so it does catch that.
- The macOS build stays on Apple Silicon; upstream moved theirs to an Intel
  runner.

## v1.6.1 — 2026-10-10

One thing v1.6 broke and two that were always there.

- **With `-d` the split ran ahead of the cursor.** Upstream now reads the mouse
  position from each event, and SDL reports those in drawable pixels, because
  the renderer's logical size is the drawable size, while everything else here
  works in window points. On a 2x display under `-d` the split sat at twice the
  cursor's distance from the left edge, click-to-seek went twice as far, and
  crop selections, Shift+F's included, started in the wrong place. The crop
  preview's right-drag pan had the same mismatch on its relative motion since
  v1.0, moving twice as fast as the cursor; it is fixed alongside.
- **Chinese in on-screen messages came out as mojibake.** The "Saved ...
  image" message carries file names and directories, and messages were drawn
  as Latin-1. They are UTF-8 now, and borrow the embedded CJK font when the
  font in use, which is picked from the filenames, lacks a glyph, so a Chinese
  save directory beside Latin filenames shows too.
- **No input method popup on every shortcut.** SDL2 starts text input by
  default, which hands each key press to the input method first; with Chinese
  input selected, every shortcut letter opened a candidate window. Nothing here
  takes typed text, so it is off.
- `-V`, the usage line and the verbose dump read
  `v1.6.1/by larry (upstream 20261004-osaka)`, the fork's release beside the
  upstream one it is based on.

## v1.5 — 2026-08-06

The crop preview can be put at a zoom you can name.

- **It only zoomed by wheel, against nothing.** The preview `Shift+F` opens had
  the wheel and `R`, and `R` fits the window at nine tenths — so the one thing
  the preview is for, looking at pixels, could not be set to 1:1 except by
  hunting for it. `4` now puts one source pixel on one physical display pixel
  and `6` puts it on one window point. The two differ by the backing scale
  factor, so on a Retina display `4` is half of `6`, and on a 1x display they
  are the same key twice. `4` means what it means in the main view; the main
  view's `6` is different, a return to the fit-to-window x1. `R` still fits, and the hint line now calls it Fit rather than Reset,
  which is what it always did.
- **The zoom factor is shown**, top left, so the wheel has something to land on.
  It is deliberately not on the hint line: that line is centred and a narrow
  window clips it from both ends, taking the one part that changes with it.

## v1.4 — 2026-08-05

4K plays at the speed it says it does.

- **A 4K 59.94 fps comparison ran at about half speed.** Frames are converted to
  RGB on their way to the screen, and that conversion ran on one core. With the
  high-quality flags used for aligning input resolutions a 3840x2160 frame costs
  about 31 ms, against the 16.7 ms a 59.94 fps source allows, so playback
  settled wherever the converter could keep up — around 31 fps, which at nominal
  speed is slow motion. 1080p needs 10.4 ms and fitted, which is why only 4K
  showed it. libswscale will spread the work across cores, but only through
  `sws_scale_frame()`; `sws_scale()` ignores the thread count it is handed. Each
  video now gets an equal share of the machine. Measured on a 10-core M4: the 4K
  pair 31.2 -> 59 fps, a mixed 1080p/4K pair 25.3 -> 51, 1080p unchanged at
  60.0. The picture is not affected — byte-identical to the single-threaded
  result at every thread count, including the upscale, downscale and odd-width
  paths, where a slice boundary could have left a seam.

## v1.3 — 2026-07-29

Two things worth looking at, and a fork you can actually install.

- **Which frame each side is on.** The HUD carries the frame number beside the
  timestamp it already showed, counted from zero the way FFmpeg does. Each side
  is numbered within its own file, so the difference between the two is the
  offset stepping applied. Divided by the container's frame rate as a rational
  rather than by a frame duration in whole microseconds — 59.94 fps is 16683.33
  us a frame, and rounding that away costs four frames across an hour. Constant
  frame rates only. `N` turns it off.
- **Blink comparison.** `O` shows one side at a time in the full frame, arrows
  flip between them, `O` again puts the view back. A difference that survives
  being looked for side by side rarely survives being blinked. Only the arrows
  are taken over; play, stepping and zoom carry on.
- **`install.sh`.** Homebrew's `sdl2` is an alias for `sdl2-compat` now, and a
  machine still carrying the older one can link the program against one and
  SDL2_ttf against the other — two SDL2s in a single process, with nothing to
  show for it since asking for the version works either way. The script finds
  that and relinks.
- **The macOS artifact carries its libraries**, as the Windows one already did.
  It was a bare executable wired to the build machine's Homebrew prefix, and
  downloading it produced no error and no output. Still arm64, and still no
  older than the macOS that built it.
- **A Chinese guide** ([README.zh-CN.md](README.zh-CN.md)) covering usage,
  releases and what is known to be wrong, plus a section on what a Retina
  display setting does to the picture — measured, since a scaled mode resamples
  the frame before it reaches the eye and the tool exists to judge encodes.

## v1.2 — 2026-07-17

Stepping now keeps the videos where the reported offset says they are.

- Stepping more than about eight frames could make the two sides take turns
  advancing, each freezing while the other caught up. The offset was recomputed
  on every frame from a rolling average of frame durations, and that average is
  measured from the very timestamps the offset shifts, so it fed itself. It is
  now fixed in time the moment you step, taken from the real distance between
  the frames stepped over.
- Stepping backward and then forward again skipped frames and left the videos a
  frame apart while reporting no offset. Frames rewound past are now kept and
  handed back on the way forward, so `+` and `-` undo each other exactly.
- Stepping back past the start of the video no longer claims frames that are not
  there, which used to drag the left video forward when playback resumed.
- The reported offset now always matches the picture. It is derived from the one
  value the program actually uses, so it can no longer say `-1 frames` with the
  videos level.
- Seeking during playback could freeze the window when comparing a file against
  itself several times over and one of the right videos was stepped.
- Each right video is now paced against its own frame rate rather than the
  active one's, which matters when they differ.

## v1.1 — 2026-07-16

Stepping no longer freezes the window, and Windows builds again.

- Stepping a right video, then resuming playback, could freeze the window with
  no way out but killing the process. So could stepping forward past the frames
  held in memory. Both only happened when comparing a file against itself.
- The MinGW build had been pinned to an FFmpeg version the dependency script no
  longer downloads, so every Windows build failed. Fixed, along with two
  link-time gaps that would have surfaced next.

## v1.0 — 2026-04-13

Two features on top of upstream.

**Per-right-video frame stepping.** Step the active right video against the left
one with `+` and `-` — hold Ctrl for 10 frames, Alt for 100. The accumulated
offset is shown on screen and reset with Ctrl+0. Stepping backward replays
frames already in memory where it can and seeks where it cannot. Resuming
playback puts the stepped video back in step with what is on screen.

**Crop preview.** `Shift+F` selects a region and opens a preview instead of
saving straight away. Zoom around the cursor, pan, then Enter to save the
concatenation, or Shift+Enter to save the left video, each right video, and the
concatenation. A native directory picker asks where they go, and filenames are
prefixed with a timestamp.

---

## Known issues

Found by review, reproduced or reasoned through, and deliberately left alone.
Each is either unlikely in practice or cannot be verified by running the program,
and this fork's rule has been not to change what cannot be checked.

Line numbers drift; the name beside each is what to search for.

| Where | Issue | Why it is still here |
|---|---|---|
| `src/video_compare.cpp:1143` (in `partial_seek_right_video`) | A partial seek can call `av_seek_frame` while the demuxer thread is inside `av_read_frame` on the same `AVFormatContext`. Undefined behaviour. The full seek path has a handshake for this; the partial one does not. | Needs slow I/O (a network stream, a loaded machine) to hit the window. No way to reproduce on demand. |
| `src/video_compare.cpp:1549` (the pause-to-play realign) | When the realign on resume cannot produce a frame, its failure is discarded and the picture stops with nothing said. | Requires a seek target past the end of the file, and the ordinary seek path usually gets there first. Could not be reproduced. |
| `src/video_compare.cpp:1153` (in `partial_seek_right_video`) | With a time-shift multiplier other than 1, a partial seek aims at a position that leaves out the multiplier's own contribution. The full seek path includes it. | Needs `--time-shift` with a multiplier. Untested. |
| `src/video_compare.cpp:1413` (forward stepping) | A forward step that seeks and then meets the end of the file reports "reached end of video" although the offset did move. | Cosmetic. |
| `src/video_compare.cpp:1898` (in `update_frame_timing`) | Stepping still feeds the frame-duration average samples that are not frame durations, so `delta_pts_` can be off. | The offset no longer depends on it as of v1.2. What is left of it reaches the sync tolerance and the durations stamped on frames. |
| `src/video_compare.cpp:1792` (in `pop_frame`) | Shift+D after a backward step is served from the frames set aside by that step. Believed right, and it behaves that way in use. | No log evidence either way — the debug print cannot tell it apart from ordinary playback. |

### Notes

- A partial seek always leaves single-decoder mode off until the next real seek.
  Correct, but a comparison of a file against itself decodes it twice until then.
- Stepping across a dropped frame reports the two frame durations it covered
  rather than one. The count follows the picture, and on such material a frame
  count and a time span are not the same thing.
- Frames reach the screen as RGB24, which Metal cannot hold, so SDL converts
  each one to ARGB8888 on the CPU first — about 3 ms per 4K frame, now the
  largest single cost on the main thread. Comparing two 4K videos of unequal
  resolution still falls a little short of 59.94 fps for the same reason the
  conversion is expensive at all: the smaller side is scaled up to the larger
  before either is drawn.
