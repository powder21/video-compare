# Per-Right-Video Frame Stepping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the global +/- time shift mechanism with independent per-right-video frame stepping that is lightweight (no full seek for forward steps) and supports seamless playback.

**Architecture:** Forward stepping pops frames directly from the current right video's converted frame queue (no seek). Backward stepping uses a per-side partial seek (queue cascade + demuxer seek for only the affected right video). Per-video PTS offsets are applied in the sync logic during playback and in seek position calculations during global seeks.

**Tech Stack:** C++14, FFmpeg, SDL2

---

### Task 1: Display Layer — Replace shift_right_frames_ with step_right_frames_

**Files:**
- Modify: `display.h:223` (member variable)
- Modify: `display.h:500` (getter declaration)
- Modify: `display.cpp:2978` (begin_input_frame reset)
- Modify: `display.cpp:3479-3498` (key handlers for +/-)
- Modify: `display.cpp:3621-3622` (getter implementation)

- [ ] **Step 1: Rename member variable in display.h**

In `display.h`, line 223, rename:

```cpp
// Old:
int shift_right_frames_{0};

// New:
int step_right_frames_{0};
```

In `display.h`, line 500, rename getter declaration:

```cpp
// Old:
int get_shift_right_frames() const;

// New:
int get_step_right_frames() const;
```

- [ ] **Step 2: Update begin_input_frame reset in display.cpp**

In `display.cpp`, line 2978, rename:

```cpp
// Old:
shift_right_frames_ = 0;

// New:
step_right_frames_ = 0;
```

- [ ] **Step 3: Update key handlers in display.cpp**

In `display.cpp`, lines 3479-3498, rename all `shift_right_frames_` to `step_right_frames_`:

```cpp
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
        case SDLK_EQUALS:  // for tenkeyless keyboards
          if (is_alt_down) {
            step_right_frames_ += 100;
          } else if (is_ctrl_down) {
            step_right_frames_ += 10;
          } else {
            step_right_frames_++;
          }
          break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
          if (is_alt_down) {
            step_right_frames_ -= 100;
          } else if (is_ctrl_down) {
            step_right_frames_ -= 10;
          } else {
            step_right_frames_--;
          }
          break;
```

- [ ] **Step 4: Update getter implementation in display.cpp**

In `display.cpp`, lines 3621-3622, rename:

```cpp
// Old:
int Display::get_shift_right_frames() const {
  return shift_right_frames_;
}

// New:
int Display::get_step_right_frames() const {
  return step_right_frames_;
}
```

- [ ] **Step 5: Add Ctrl+0 reset support to Display**

In `display.h`, add a new member variable after `step_right_frames_` (around line 224):

```cpp
  bool reset_right_offset_{false};
```

Add getter declaration in the public section (after `get_step_right_frames`):

```cpp
  bool get_reset_right_offset() const;
```

In `display.cpp`, in `begin_input_frame()`, add reset after `step_right_frames_ = 0;`:

```cpp
  reset_right_offset_ = false;
```

In `display.cpp`, in the `SDLK_0` / `SDLK_KP_0` case (lines 3263-3266), add a Ctrl check BEFORE the existing subtraction_mode toggle:

```cpp
        case SDLK_0:
        case SDLK_KP_0:
          if (is_ctrl_down) {
            reset_right_offset_ = true;
          } else {
            subtraction_mode_ = !subtraction_mode_;
          }
          break;
```

Add getter implementation after `get_step_right_frames()`:

```cpp
bool Display::get_reset_right_offset() const {
  return reset_right_offset_;
}
```

- [ ] **Step 6: Build to verify compilation**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -20`
Expected: Build succeeds (linker will fail on `get_shift_right_frames` references in video_compare.cpp — that's expected and fixed in the next task)

- [ ] **Step 7: Commit**

```bash
git add display.h display.cpp
git commit -m "refactor(display): rename shift_right_frames to step_right_frames, add Ctrl+0 reset"
```

---

### Task 2: VideoCompare — Per-Video Time Shift Data Model

**Files:**
- Modify: `video_compare.cpp:872-873` (local variables in compare())
- Modify: `video_compare.cpp:1008` (getter call)
- Modify: `video_compare.cpp:1011-1018` (time shift computation)

- [ ] **Step 1: Replace global time shift with per-video map**

In `video_compare.cpp`, in `compare()`, replace lines 872-873:

```cpp
// Old:
    int64_t static_right_time_shift = time_shift_offset_av_time_;
    int total_right_time_shifted = 0;

// New:
    std::map<size_t, int64_t> per_right_time_shifts;  // right_index -> accumulated frame count offset
```

- [ ] **Step 2: Add helper lambda after per_right_time_shifts**

Add this right after the `per_right_time_shifts` declaration (before `int forward_navigate_frames = 0;` at line 875):

```cpp
    // Compute the effective PTS time shift for a given right video index.
    auto compute_static_right_time_shift = [&](const size_t right_index, const int64_t right_delta_pts) -> int64_t {
      return time_shift_offset_av_time_ + per_right_time_shifts[right_index] * right_delta_pts;
    };
```

- [ ] **Step 3: Rename getter call**

In `video_compare.cpp`, line 1008, rename:

```cpp
// Old:
      const int shift_right_frames = display_->get_shift_right_frames();

// New:
      const int step_right_frames = display_->get_step_right_frames();
```

- [ ] **Step 4: Build to verify compilation**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Build will fail because `static_right_time_shift`, `total_right_time_shifted`, and `shift_right_frames` are still referenced. That's expected — we fix these references in the following tasks.

- [ ] **Step 5: Commit (WIP)**

```bash
git add video_compare.cpp
git commit -m "wip: replace global time shift with per-video map (references not yet updated)"
```

---

### Task 3: VideoCompare — Forward Stepping Logic

**Files:**
- Modify: `video_compare.cpp` — new forward stepping block in compare(), between the existing frame_navigation_delta handling (line ~992) and the seek block (line ~1010)

- [ ] **Step 1: Add forward stepping block**

After the `frame_navigation_delta` handling (after line 1001 `seek_from_start = false; }`) and before the `skip_update` and `handle_pending_crop_request` lines, add:

```cpp
      // --- Per-right-video forward stepping (+ key) ---
      if (step_right_frames > 0 && !display_->get_play()) {
        int frames_stepped = 0;

        for (int i = 0; i < step_right_frames; i++) {
          AVFrameUniquePtr stepped_frame{nullptr, avframe_deleter};

          if (converted_frame_queues_[active_right]->pop(stepped_frame)) {
            // Update the right video's state with the new frame
            right_ptr->frame_ = std::move(stepped_frame);
            right_ptr->decoded_picture_number_++;

            // Accumulate per-video offset
            per_right_time_shifts[active_right_index_]++;
            frames_stepped++;
          } else {
            // Queue empty — cannot step further forward
            break;
          }
        }

        if (frames_stepped > 0 && right_ptr->frame_ != nullptr) {
          // Compute effective time shift for the active right video
          const int64_t static_shift = compute_static_right_time_shift(active_right_index_, right_delta);
          right_ptr->effective_time_shift_ = static_shift + calculate_dynamic_time_shift(time_shift_.multiplier, right_ptr->frame_->pts, true);

          // Update frame timing for the stepped right video
          const int64_t new_pts = right_ptr->frame_->pts - right_ptr->effective_time_shift_;
          right_ptr->delta_pts_ = (right_ptr->pts_ != 0) ? (new_pts - right_ptr->pts_) : right_ptr->delta_pts_;
          right_ptr->pts_ = new_pts;

          // Store in the right video's frame buffer
          if (right_ptr->frames_.size() >= frame_buffer_size_) {
            right_ptr->frames_.pop_back();
          }
          right_ptr->frames_.push_front(std::move(right_ptr->frame_));

          // Reset frame offset so display shows the newly stepped frame
          frame_offset = 0;
        }

        // Notify user
        const int64_t total_offset = per_right_time_shifts[active_right_index_];
        display_->notify_user(string_sprintf("Right #%zu: %s%lld frames",
          active_right_index_ + 1,
          total_offset > 0 ? "+" : "",
          static_cast<long long>(total_offset)));

        skip_update = true;
      }
```

- [ ] **Step 2: Build to verify (expect remaining errors from other references)**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`

- [ ] **Step 3: Commit (WIP)**

```bash
git add video_compare.cpp
git commit -m "wip: add forward stepping logic (pop from right queue)"
```

---

### Task 4: VideoCompare — Backward Stepping with Partial Seek

**Files:**
- Modify: `video_compare.cpp` — new backward stepping block, right after the forward stepping block from Task 3

- [ ] **Step 1: Add backward stepping block**

Immediately after the forward stepping block (after its closing `}`), add:

```cpp
      // --- Per-right-video backward stepping (- key): partial seek ---
      if (step_right_frames < 0 && !display_->get_play()) {
        // Accumulate per-video offset
        per_right_time_shifts[active_right_index_] += step_right_frames;  // step_right_frames is negative

        // Compute target seek position for the right video
        const int64_t effective_shift = compute_static_right_time_shift(active_right_index_, right_delta);
        const float right_target_position = left.pts_ * AV_TIME_TO_SEC + right_ptr->start_time_ + effective_shift * AV_TIME_TO_SEC;

        // --- Partial seek: only affect the current right video's pipeline ---

        // 1. Stop the packet queue to halt demuxing for this side
        packet_queues_[active_right]->stop();

        // 2. Wait for the pipeline cascade to complete (all downstream queues stop)
        while (!converted_frame_queues_[active_right]->is_stopped()) {
          decoded_frame_queues_[active_right]->empty();
          filtered_frame_queues_[active_right]->empty();
          converted_frame_queues_[active_right]->empty();
          sleep_for_ms(SLEEP_PERIOD_MS);
        }

        // 3. Final empty of all queues for this side
        packet_queues_[active_right]->empty();
        decoded_frame_queues_[active_right]->empty();
        filtered_frame_queues_[active_right]->empty();
        converted_frame_queues_[active_right]->empty();

        // 4. Flush decoder to clear cached reference frames
        video_decoders_[active_right]->flush();

        // 5. Reinit filter graph for this side
        video_filterers_[active_right]->reinit();

        // 6. Seek the demuxer to the target position
        demuxers_[active_right]->seek(right_target_position, true);

        // 7. Restart all queues for this side
        packet_queues_[active_right]->restart();
        decoded_frame_queues_[active_right]->restart();
        filtered_frame_queues_[active_right]->restart();
        converted_frame_queues_[active_right]->restart();

        // 8. Pop the first frame from the restarted pipeline
        converted_frame_queues_[active_right]->pop(right_ptr->frame_);

        if (right_ptr->frame_ != nullptr) {
          right_ptr->pts_ = right_ptr->frame_->pts;
          right_ptr->effective_time_shift_ = effective_shift + calculate_dynamic_time_shift(time_shift_.multiplier, right_ptr->frame_->pts, true);
          right_ptr->pts_ -= right_ptr->effective_time_shift_;
          right_ptr->previous_decoded_picture_number_ = -1;
          right_ptr->decoded_picture_number_ = 1;

          // Clear and re-seed the frame buffer
          right_ptr->frames_.clear();
          right_ptr->frames_.push_front(std::move(right_ptr->frame_));

          // Reset frame offset so display shows the newly stepped frame
          frame_offset = 0;
        }

        // Notify user
        const int64_t total_offset = per_right_time_shifts[active_right_index_];
        display_->notify_user(string_sprintf("Right #%zu: %s%lld frames",
          active_right_index_ + 1,
          total_offset > 0 ? "+" : "",
          static_cast<long long>(total_offset)));

        skip_update = true;
      }
```

- [ ] **Step 2: Build to verify**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`

- [ ] **Step 3: Commit (WIP)**

```bash
git add video_compare.cpp
git commit -m "wip: add backward stepping with partial seek"
```

---

### Task 5: VideoCompare — Update Global Seek to Use Per-Video Offsets

**Files:**
- Modify: `video_compare.cpp:1011-1018` (seek trigger condition and time shift computation)
- Modify: `video_compare.cpp:1101` (backward seek flag)
- Modify: `video_compare.cpp:1136` (right seek position)
- Modify: `video_compare.cpp:1219-1224` (rounding)
- Modify: `video_compare.cpp:1232` (effective_time_shift assignment in pop_and_reset)

- [ ] **Step 1: Remove step_right_frames from the global seek trigger**

The global seek block (line 1011) currently triggers on `shift_right_frames != 0`. Since stepping is now handled separately (Tasks 3 & 4), remove it from the global seek condition.

Replace lines 1011-1018:

```cpp
// Old:
      if ((seek_relative != 0.0F) || (shift_right_frames != 0) || force_seek_current_position) {
        // update total right time shifted
        if (shift_right_frames != 0) {
          total_right_time_shifted += shift_right_frames;
        }

        // compute effective time shift
        static_right_time_shift = time_shift_offset_av_time_ + total_right_time_shifted * right_delta;

// New:
      if ((seek_relative != 0.0F) || force_seek_current_position) {
```

The per-video time shifts are already accumulated in Tasks 3 & 4. The global seek block no longer needs to handle stepping.

- [ ] **Step 2: Update backward flag at line 1101**

Replace:

```cpp
// Old:
        const bool backward = (seek_relative < 0.0F) || (shift_right_frames != 0) || (force_seek_current_position && all_media_are_multi_frame());

// New:
        const bool backward = (seek_relative < 0.0F) || (force_seek_current_position && all_media_are_multi_frame());
```

- [ ] **Step 3: Update right seek position computation at line 1136**

Replace the static_right_time_shift reference with per-video computation:

```cpp
// Old:
            next_right_position += static_right_time_shift * AV_TIME_TO_SEC;

// New:
            const int64_t per_video_static_shift = compute_static_right_time_shift(side.right_index(), normalized_delta(right_state.delta_pts_));
            next_right_position += per_video_static_shift * AV_TIME_TO_SEC;
```

- [ ] **Step 4: Update time shift rounding at lines 1219-1224**

Replace the global rounding with per-video rounding. The rounding is applied when setting `effective_time_shift_` for each right video after seek. Replace:

```cpp
// Old:
        // round away from zero to nearest 2 ms
        if (static_right_time_shift > 0) {
          static_right_time_shift = ((static_right_time_shift / 1000) + 2) * 1000;
        } else if (static_right_time_shift < 0) {
          static_right_time_shift = ((static_right_time_shift / 1000) - 2) * 1000;
        }

        // Reset all right videos after seek
        for (auto& pair : side_states) {
          const Side& side = pair.first;
          if (side.is_right()) {
            SideState& right_state = pair.second;

            right_state.effective_time_shift_ = static_right_time_shift;
            pop_and_reset(right_state, &right_state.effective_time_shift_);
          }
        }

// New:
        // Reset all right videos after seek with per-video time shifts
        for (auto& pair : side_states) {
          const Side& side = pair.first;
          if (side.is_right()) {
            SideState& right_state = pair.second;

            int64_t per_video_shift = compute_static_right_time_shift(side.right_index(), normalized_delta(right_state.delta_pts_));

            // round away from zero to nearest 2 ms
            if (per_video_shift > 0) {
              per_video_shift = ((per_video_shift / 1000) + 2) * 1000;
            } else if (per_video_shift < 0) {
              per_video_shift = ((per_video_shift / 1000) - 2) * 1000;
            }

            right_state.effective_time_shift_ = per_video_shift;
            pop_and_reset(right_state, &right_state.effective_time_shift_);
          }
        }
```

- [ ] **Step 5: Update effective_time_shift during frame popping**

In the `store_frames` block (around line 1308-1313), the effective_time_shift is computed using the old global variable. Replace:

```cpp
// Old (line 1311):
              side_state.effective_time_shift_ = static_right_time_shift + calculate_dynamic_time_shift(time_shift_.multiplier, side_state.frame_->pts, true);

// New:
              side_state.effective_time_shift_ = compute_static_right_time_shift(side.right_index(), normalized_delta(side_state.delta_pts_)) + calculate_dynamic_time_shift(time_shift_.multiplier, side_state.frame_->pts, true);
```

Note: `side` is available from the loop: `for (auto& pair : side_states) { ... const Side& side = pair.first; ... }`. Verify the variable name matches the enclosing loop (around line 1308).

- [ ] **Step 6: Remove stale static_right_time_shift references**

Search for any remaining references to `static_right_time_shift` in video_compare.cpp and replace them. Key locations:

- Line ~1253 (debug print, inside `#ifdef _DEBUG`): replace `static_right_time_shift` with `right_ptr->effective_time_shift_` (already per-video).
- Line ~1317 (play_frame_delay computation): this uses `right_ptr->effective_time_shift_` which is already per-video. No change needed.

- [ ] **Step 7: Build to verify compilation**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Build succeeds with no errors.

- [ ] **Step 8: Commit**

```bash
git add video_compare.cpp
git commit -m "feat: update global seek to use per-video time shift offsets"
```

---

### Task 6: VideoCompare — Ctrl+0 Reset Handling

**Files:**
- Modify: `video_compare.cpp` — in compare(), after the Tab-switch block (around line 971) and before the frame_navigation_delta handling

- [ ] **Step 1: Add reset handling**

After the Tab-switch block (after `scope_update_state_.reset(); }`) and before `const int format_conversion_sws_flags = ...`, add:

```cpp
      // Handle Ctrl+0 reset of current right video's offset
      if (display_->get_reset_right_offset()) {
        if (per_right_time_shifts[active_right_index_] != 0) {
          per_right_time_shifts[active_right_index_] = 0;

          // Trigger a partial seek to re-sync the right video with left's position
          const int64_t effective_shift = compute_static_right_time_shift(active_right_index_, right_delta);
          const float right_target_position = left.pts_ * AV_TIME_TO_SEC + right_ptr->start_time_ + effective_shift * AV_TIME_TO_SEC;

          // Partial seek (same cascade as backward stepping)
          packet_queues_[active_right]->stop();

          while (!converted_frame_queues_[active_right]->is_stopped()) {
            decoded_frame_queues_[active_right]->empty();
            filtered_frame_queues_[active_right]->empty();
            converted_frame_queues_[active_right]->empty();
            sleep_for_ms(SLEEP_PERIOD_MS);
          }

          packet_queues_[active_right]->empty();
          decoded_frame_queues_[active_right]->empty();
          filtered_frame_queues_[active_right]->empty();
          converted_frame_queues_[active_right]->empty();

          video_decoders_[active_right]->flush();
          video_filterers_[active_right]->reinit();
          demuxers_[active_right]->seek(right_target_position, true);

          packet_queues_[active_right]->restart();
          decoded_frame_queues_[active_right]->restart();
          filtered_frame_queues_[active_right]->restart();
          converted_frame_queues_[active_right]->restart();

          converted_frame_queues_[active_right]->pop(right_ptr->frame_);

          if (right_ptr->frame_ != nullptr) {
            right_ptr->pts_ = right_ptr->frame_->pts;
            right_ptr->effective_time_shift_ = effective_shift + calculate_dynamic_time_shift(time_shift_.multiplier, right_ptr->frame_->pts, true);
            right_ptr->pts_ -= right_ptr->effective_time_shift_;
            right_ptr->previous_decoded_picture_number_ = -1;
            right_ptr->decoded_picture_number_ = 1;

            right_ptr->frames_.clear();
            right_ptr->frames_.push_front(std::move(right_ptr->frame_));

            // Reset frame offset so display shows the reset frame
            frame_offset = 0;
          }

          display_->notify_user(string_sprintf("Right #%zu: offset reset", active_right_index_ + 1));
        }
      }
```

- [ ] **Step 2: Build to verify**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add video_compare.cpp
git commit -m "feat: add Ctrl+0 reset for per-right-video frame offset"
```

---

### Task 7: UI Notification on Tab Switch

**Files:**
- Modify: `video_compare.cpp` — in the Tab-switch block (around line 963-971)

- [ ] **Step 1: Add offset notification on Tab switch**

In the Tab-switch block, after `scope_update_state_.reset();` (line 970), add:

```cpp
        // Show current offset when switching to a right video
        const int64_t switched_offset = per_right_time_shifts[active_right_index_];
        if (switched_offset != 0) {
          display_->notify_user(string_sprintf("Right #%zu: %s%lld frames",
            active_right_index_ + 1,
            switched_offset > 0 ? "+" : "",
            static_cast<long long>(switched_offset)));
        }
```

- [ ] **Step 2: Build to verify**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add video_compare.cpp
git commit -m "feat: show frame offset notification on Tab switch"
```

---

### Task 8: Update Controls Documentation

**Files:**
- Modify: `controls.cpp:37-38` (existing +/- description)
- Modify: `controls.cpp:68` (existing Ctrl+/- description)
- Add: Ctrl+0 entry

- [ ] **Step 1: Update control descriptions**

In `controls.cpp`, replace the existing +/- and Ctrl+/- descriptions:

```cpp
// Old (lines 37-38):
      {"+", "Time-shift right video 1 frame forward"},
      {"-", "Time-shift right video 1 frame backward"}}},

// New:
      {"+", "Step current right video 1 frame forward"},
      {"-", "Step current right video 1 frame backward"},
      {"Ctrl+0", "Reset current right video frame offset"}}},
```

Update the Advanced section Ctrl+/- entry (line 68):

```cpp
// Old:
      {"Ctrl + +/-", "Time-shift right video by 10 frames"},
      {"Alt + +/-", "Time-shift right video by 100 frames"}}},

// New:
      {"Ctrl + +/-", "Step current right video by 10 frames"},
      {"Alt + +/-", "Step current right video by 100 frames"}}},
```

- [ ] **Step 2: Build to verify**

Run: `cd /Users/larry/Experiments/code/video-compare && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add controls.cpp
git commit -m "docs: update control descriptions for per-right-video stepping"
```

---

### Task 9: Full Build and Manual Verification

- [ ] **Step 1: Clean build**

Run: `cd /Users/larry/Experiments/code/video-compare && make clean && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -30`
Expected: Full clean build succeeds with no errors and no new warnings.

- [ ] **Step 2: Verify no stale references**

Run: `grep -rn 'shift_right_frames\|total_right_time_shifted\|get_shift_right_frames\|static_right_time_shift' *.cpp *.h`
Expected: No matches (all old names should be replaced).

- [ ] **Step 3: Commit final state**

If any fixup was needed, commit it:

```bash
git add -A
git commit -m "fix: clean up any remaining stale references from time-shift refactor"
```
