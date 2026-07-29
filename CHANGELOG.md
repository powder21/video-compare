# Changelog

Notable changes in this fork of [pixop/video-compare](https://github.com/pixop/video-compare).
Upstream releases keep their own date-stamped tags; this fork's own work is tagged `v1.x`.

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
| `video_compare.cpp:955` (in `partial_seek_right_video`) | A partial seek can call `av_seek_frame` while the demuxer thread is inside `av_read_frame` on the same `AVFormatContext`. Undefined behaviour. The full seek path has a handshake for this; the partial one does not. | Needs slow I/O (a network stream, a loaded machine) to hit the window. No way to reproduce on demand. |
| `video_compare.cpp:1362` (the pause-to-play realign) | When the realign on resume cannot produce a frame, its failure is discarded and the picture stops with nothing said. | Requires a seek target past the end of the file, and the ordinary seek path usually gets there first. Could not be reproduced. |
| `video_compare.cpp:965` (in `partial_seek_right_video`) | With a time-shift multiplier other than 1, a partial seek aims at a position that leaves out the multiplier's own contribution. The full seek path includes it. | Needs `--time-shift` with a multiplier. Untested. |
| `video_compare.cpp:1226` (forward stepping) | A forward step that seeks and then meets the end of the file reports "reached end of video" although the offset did move. | Cosmetic. |
| `video_compare.cpp:1740` (in `update_frame_timing`) | Stepping still feeds the frame-duration average samples that are not frame durations, so `delta_pts_` can be off. | The offset no longer depends on it as of v1.2. What is left of it reaches the sync tolerance and the durations stamped on frames. |
| `video_compare.cpp:1634` (in `pop_frame`) | Shift+D after a backward step is served from the frames set aside by that step. Believed right, and it behaves that way in use. | No log evidence either way — the debug print cannot tell it apart from ordinary playback. |

### Notes

- A partial seek always leaves single-decoder mode off until the next real seek.
  Correct, but a comparison of a file against itself decodes it twice until then.
- Stepping across a dropped frame reports the two frame durations it covered
  rather than one. The count follows the picture, and on such material a frame
  count and a time span are not the same thing.
