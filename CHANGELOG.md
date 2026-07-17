# Changelog

Notable changes in this fork of [pixop/video-compare](https://github.com/pixop/video-compare).
Upstream releases keep their own date-stamped tags; this fork's own work is tagged `v1.x`.

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

| Where | Issue | Why it is still here |
|---|---|---|
| `video_compare.cpp:918` | A partial seek can call `av_seek_frame` while the demuxer thread is inside `av_read_frame` on the same `AVFormatContext`. Undefined behaviour. The full seek path has a handshake for this; the partial one does not. | Needs slow I/O (a network stream, a loaded machine) to hit the window. No way to reproduce on demand. |
| `video_compare.cpp:1269` | When the realign on resume cannot produce a frame, its failure is discarded and the picture stops with nothing said. | Requires a seek target past the end of the file, and the ordinary seek path usually gets there first. Could not be reproduced. |
| `video_compare.cpp:928` | With a time-shift multiplier other than 1, a partial seek aims at a position that leaves out the multiplier's own contribution. The full seek path includes it. | Needs `--time-shift` with a multiplier. Untested. |
| `video_compare.cpp:1142` | A forward step that seeks and then meets the end of the file reports "reached end of video" although the offset did move. | Cosmetic. |
| `video_compare.cpp:1262` | Stepping still feeds the frame-duration average samples that are not frame durations, so `delta_pts_` can be off. | The offset no longer depends on it as of v1.2. What is left of it reaches the sync tolerance and the durations stamped on frames. |
| `video_compare.cpp:1636` | Shift+D after a backward step is served from the frames set aside by that step. Believed right, and it behaves that way in use. | No log evidence either way — the debug print cannot tell it apart from ordinary playback. |

### Notes

- A partial seek always leaves single-decoder mode off until the next real seek.
  Correct, but a comparison of a file against itself decodes it twice until then.
- Stepping across a dropped frame reports the two frame durations it covered
  rather than one. The count follows the picture, and on such material a frame
  count and a time span are not the same thing.
