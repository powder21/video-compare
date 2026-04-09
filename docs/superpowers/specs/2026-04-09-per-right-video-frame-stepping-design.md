# Per-Right-Video Frame Stepping

Replaces the existing global +/- time shift mechanism with independent, per-right-video frame stepping. Each right video maintains its own frame offset. Stepping is lightweight (no full seek) and playback after stepping is seamless.

## Background

The existing +/- mechanism (`shift_right_frames_` / `total_right_time_shifted`) has two problems:

1. **Global**: all right videos share one offset, so stepping affects every right video equally.
2. **Heavy**: every step triggers a full seek (drain all queues, re-seek all streams, reinit filters), causing a visible "positioning" artifact when playback resumes.

## Requirements

1. **Independence**: stepping only affects the currently active right video. Left video and other right videos do not move.
2. **Continuity**: after stepping, pressing play resumes both left and right from their current frame positions with zero additional seeking.
3. **Persistence**: each right video's offset survives Tab switching and is restored when switching back.

## Data Model

Replace the global offset with a per-video map, both declared as local variables in `VideoCompare::compare()`:

```cpp
// Remove:
int total_right_time_shifted = 0;

// Add:
std::map<int, int64_t> per_right_time_shifts;  // right_index -> accumulated PTS offset
```

In `Display`, replace:

```cpp
// Remove:
int shift_right_frames_{0};

// Add:
int step_right_frames_{0};  // transient delta, consumed each loop iteration
```

Getter changes accordingly: `get_shift_right_frames()` becomes `get_step_right_frames()`.

## Stepping Logic

### Forward Step (+)

1. Increment `per_right_time_shifts[active_right_index]` by one frame's PTS duration (`right_delta`).
2. Decode/pop the next frame for the current right video only. Left video and other right videos are not touched.
3. Display the new frame immediately.

### Backward Step (-)

1. Decrement `per_right_time_shifts[active_right_index]` by one frame's PTS duration.
2. If the target frame is in the buffer, use it. Otherwise, seek only the current right video backward.
3. Display the new frame immediately.

### Multi-Frame Steps (Ctrl/Alt modifiers)

Same logic, repeated N times (10 for Ctrl, 100 for Alt). The offset accumulates by `N * right_delta`.

### Key Principle

Stepping completes frame positioning synchronously. When the step finishes, the current right video is already showing the target frame. No deferred work remains for the play action.

## Playback with Offsets

When play is pressed after stepping:

1. No additional seek is triggered. The right video is already at the correct frame.
2. Left video resumes decoding from its current frame.
3. Each right video resumes with its own PTS offset applied during synchronization: the sync comparison uses `right_pts - per_right_time_shifts[index]` instead of the former global `static_right_time_shift`.
4. The frame difference is maintained permanently throughout playback.

## Config-Level Time Shift Interaction

The existing `time_shift_offset_av_time_` (set via command-line `--time-shift`) is a global baseline applied to all right videos. Per-video offsets are additive:

```
effective_shift[i] = time_shift_offset_av_time_ + per_right_time_shifts[i] * right_delta
```

Resetting via Ctrl+0 only clears `per_right_time_shifts[i]`; the command-line baseline remains.

## Seek Integration

When a user-initiated seek occurs (arrow keys, mouse click, etc.):

- Left video seeks to the target position.
- Each right video seeks to the target position plus its own `per_right_time_shifts[index]` offset.
- This replaces the former global `static_right_time_shift` computation.

## Keybindings

| Key | Action |
|-----|--------|
| `+` / `-` | Step current right video +/- 1 frame |
| `Ctrl + +/-` | Step current right video +/- 10 frames |
| `Alt + +/-` | Step current right video +/- 100 frames |
| `Ctrl + 0` | Reset current right video offset to 0 |

These replace the existing +/- / Ctrl+/- / Alt+/- bindings entirely.

## UI Feedback

Uses the existing `notify_user` mechanism:

- On step: `Right #1: +3 frames`
- On reset: `Right #1: offset reset`
- On Tab switch to a video with non-zero offset: `Right #2: +5 frames` (remind user of current offset)

## Scope Boundaries

**In scope:**
- Per-right-video frame offset storage and application
- Stepping forward/backward with buffer-first approach
- Offset persistence across Tab switches
- One-key reset (Ctrl+0)
- Notification display

**Out of scope:**
- Changing left video stepping behavior (Shift+A/D remain unchanged)
- Frame buffer navigation (A/D keys remain unchanged)
- Playback speed controls (J/L remain unchanged)
