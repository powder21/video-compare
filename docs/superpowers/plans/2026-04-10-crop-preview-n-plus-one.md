# Crop Preview with N+1 Images Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace immediate crop-save with an interactive concat preview, and support N right videos by saving N+1 images (N individual right cutouts + 1 concatenated left+all-rights image).

**Architecture:** When the user completes a selection rectangle in save mode (Shift+F), instead of writing PNGs immediately, Display creates a concatenated preview image (left crop + all right video crops side-by-side), renders it as an SDL texture centered in the window with independent pan/zoom. The user inspects the preview, then confirms (Enter) to save N+1 images or cancels (Escape). All right video frames are passed from `video_compare.cpp` to Display each refresh cycle via a lightweight pointer vector.

**Tech Stack:** C++14, SDL2 (textures, rendering, events), FFmpeg libavutil (AVFrame), existing PngSaver

---

### Task 1: Add all-right-frames and file-name plumbing

**Files:**
- Modify: `display.h` (add members and public method declarations)
- Modify: `display.cpp` (implement setters)
- Modify: `video_compare.cpp` (pass frames and file names to Display)

- [ ] **Step 1: Add member variables to display.h**

In the private section, after the `global_center_` member (line 265), add:

```cpp
  // All right video frames for crop preview (updated each refresh cycle, raw pointers owned by SideState)
  std::vector<const AVFrame*> all_right_frames_;
  // All right video file names (set once at startup)
  std::vector<std::string> all_right_file_names_;
```

- [ ] **Step 2: Add public method declarations to display.h**

In the public section, after `set_active_right_index(size_t index)` (line 522), add:

```cpp
  void set_all_right_frames(const std::vector<const AVFrame*>& frames);
  void set_all_right_file_names(const std::vector<std::string>& file_names);
```

- [ ] **Step 3: Implement setter methods in display.cpp**

Add at the end of display.cpp (before any closing guards), near the other setter implementations:

```cpp
void Display::set_all_right_frames(const std::vector<const AVFrame*>& frames) {
  all_right_frames_ = frames;
}

void Display::set_all_right_file_names(const std::vector<std::string>& file_names) {
  all_right_file_names_ = file_names;
}
```

- [ ] **Step 4: Pass all right file names at startup in video_compare.cpp**

In `VideoCompare::compare()`, after `display_->set_active_right_index(active_right_index_)` (around line 346), add:

```cpp
    {
      std::vector<std::string> all_right_names;
      for (size_t i = 0; i < right_video_info_.size(); i++) {
        all_right_names.push_back(right_video_info_[Side::Right(i)].file_name);
      }
      display_->set_all_right_file_names(all_right_names);
    }
```

- [ ] **Step 5: Pass all right frames before each display refresh in video_compare.cpp**

In the main loop, immediately before the `display_->possibly_refresh(left_display_frame, right_display_frame, ...)` call (around line 1724), add:

```cpp
            // Provide all right video frames for crop preview
            {
              std::vector<const AVFrame*> all_right_frames;
              for (size_t i = 0; i < right_video_info_.size(); i++) {
                const Side side = Side::Right(i);
                const auto it = side_states.find(side);
                if (it != side_states.end() && frame_offset >= 0 &&
                    frame_offset < static_cast<int>(it->second.frames_.size()) &&
                    it->second.frames_[frame_offset] != nullptr) {
                  all_right_frames.push_back(it->second.frames_[frame_offset].get());
                } else {
                  all_right_frames.push_back(nullptr);
                }
              }
              display_->set_all_right_frames(all_right_frames);
            }
```

- [ ] **Step 6: Build**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Successful build, no errors.

- [ ] **Step 7: Commit**

```bash
git add display.h display.cpp video_compare.cpp
git commit -m "feat: add all-right-frames and file-name plumbing for crop preview"
```

---

### Task 2: Add crop preview state model and lifecycle

**Files:**
- Modify: `display.h` (add preview state types, members, method declarations)
- Modify: `display.cpp` (implement lifecycle methods)

- [ ] **Step 1: Add crop preview types and members to display.h**

After the `all_right_file_names_` member added in Task 1, add:

```cpp
  // Crop preview state
  enum class CropPreviewMode { Inactive, Active };
  CropPreviewMode crop_preview_mode_{CropPreviewMode::Inactive};

  // Preview texture and dimensions
  SDL_Texture* crop_preview_texture_{nullptr};
  int crop_preview_width_{0};
  int crop_preview_height_{0};

  // Preview zoom/pan state (in window coordinates)
  // preview_scale_: window pixels per preview-image pixel
  // preview_offset_: top-left corner of the rendered preview in window coordinates
  float preview_scale_{1.0F};
  Vector2D preview_offset_{0.0F, 0.0F};

  // Stored cropped frames for deferred saving
  struct CropPreviewData {
    std::vector<AVFrame*> right_cutouts;   // N individual right cutouts
    AVFrame* concatenated{nullptr};         // left + all rights side-by-side

    void free_all() {
      for (auto& f : right_cutouts) {
        if (f) {
          av_frame_free(&f);
        }
      }
      right_cutouts.clear();
      if (concatenated) {
        av_frame_free(&concatenated);
      }
    }
  };
  CropPreviewData crop_preview_data_;
```

- [ ] **Step 2: Add private method declarations to display.h**

In the private method section (near `possibly_save_selected_area` around line 420), add:

```cpp
  void enter_crop_preview(const AVFrame* left_frame);
  void render_crop_preview();
  void exit_crop_preview(bool save);
  void save_crop_preview_images();
  void destroy_crop_preview();
```

- [ ] **Step 3: Implement destroy_crop_preview in display.cpp**

```cpp
void Display::destroy_crop_preview() {
  if (crop_preview_texture_) {
    SDL_DestroyTexture(crop_preview_texture_);
    crop_preview_texture_ = nullptr;
  }
  crop_preview_data_.free_all();
  crop_preview_width_ = 0;
  crop_preview_height_ = 0;
}
```

- [ ] **Step 4: Add destroy_crop_preview call to Display destructor**

In the Display destructor (`Display::~Display()`), add `destroy_crop_preview();` before the existing cleanup code to ensure preview resources are freed.

- [ ] **Step 5: Build**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Successful build, no errors.

- [ ] **Step 6: Commit**

```bash
git add display.h display.cpp
git commit -m "feat: add crop preview state model and lifecycle"
```

---

### Task 3: Create crop preview from selection

Replaces the immediate-save path with preview creation. When `save_selected_area_` is true and the user completes a selection, we create cropped AVFrames for all videos and build an SDL texture from the concatenated result.

**Files:**
- Modify: `display.cpp` (implement `enter_crop_preview`, modify `possibly_refresh` entry point)

**Key context:**
- The existing `save_selected_area` (display.cpp:2189-2251) creates frames via `av_frame_alloc` + `av_frame_get_buffer` and copies pixel data row-by-row with `memcpy`.
- Pixel size: `use_10_bpc_ ? 3 * sizeof(uint16_t) : 3` bytes per pixel.
- `all_right_frames_` contains raw pointers to each right video's current frame (may include nullptr for unavailable videos).
- The `get_left_selection_rect()` method returns the selection rectangle in video coordinates.

- [ ] **Step 1: Implement enter_crop_preview**

```cpp
void Display::enter_crop_preview(const AVFrame* left_frame) {
  const SDL_Rect selection_rect = get_left_selection_rect();

  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Selection rectangle is empty. Please make a valid selection." << std::endl;
    return;
  }

  // Destroy any previous preview
  destroy_crop_preview();

  const int pixel_size = use_10_bpc_ ? 3 * sizeof(uint16_t) : 3;

  // Helper to create an AVFrame with allocated buffer
  auto create_frame = [&](const int width, const int height, const AVFrame* source_frame) -> AVFrame* {
    AVFrame* frame = av_frame_alloc();
    frame->format = source_frame->format;
    frame->width = width;
    frame->height = height;
    frame->colorspace = source_frame->colorspace;
    frame->color_range = source_frame->color_range;
    av_frame_get_buffer(frame, 0);
    return frame;
  };

  // Helper to copy a crop region from source to destination frame
  auto copy_crop_region = [&](AVFrame* dst, const AVFrame* src, const SDL_Rect& rect) {
    for (int y = 0; y < rect.h; y++) {
      const int src_y = rect.y + y;
      memcpy(dst->data[0] + y * dst->linesize[0],
             src->data[0] + src_y * src->linesize[0] + rect.x * pixel_size,
             rect.w * pixel_size);
    }
  };

  // Collect valid right frames
  std::vector<const AVFrame*> valid_right_frames;
  for (const auto* frame : all_right_frames_) {
    if (frame != nullptr) {
      valid_right_frames.push_back(frame);
    }
  }

  if (valid_right_frames.empty()) {
    std::cerr << "No right video frames available for crop preview." << std::endl;
    return;
  }

  // Create individual right cutouts
  for (const auto* right_frame : valid_right_frames) {
    AVFrame* cutout = create_frame(selection_rect.w, selection_rect.h, right_frame);
    copy_crop_region(cutout, right_frame, selection_rect);
    crop_preview_data_.right_cutouts.push_back(cutout);
  }

  // Create concatenated frame: [left | right0 | right1 | ... | rightN-1]
  const int num_panels = 1 + static_cast<int>(valid_right_frames.size());
  const int concat_width = selection_rect.w * num_panels;
  crop_preview_data_.concatenated = create_frame(concat_width, selection_rect.h, left_frame);

  // Copy left panel
  for (int y = 0; y < selection_rect.h; y++) {
    const int src_y = selection_rect.y + y;
    memcpy(crop_preview_data_.concatenated->data[0] + y * crop_preview_data_.concatenated->linesize[0],
           left_frame->data[0] + src_y * left_frame->linesize[0] + selection_rect.x * pixel_size,
           selection_rect.w * pixel_size);
  }

  // Copy right panels
  for (size_t i = 0; i < valid_right_frames.size(); i++) {
    const int x_offset = static_cast<int>(i + 1) * selection_rect.w;
    for (int y = 0; y < selection_rect.h; y++) {
      const int src_y = selection_rect.y + y;
      memcpy(crop_preview_data_.concatenated->data[0] + y * crop_preview_data_.concatenated->linesize[0] + x_offset * pixel_size,
             valid_right_frames[i]->data[0] + src_y * valid_right_frames[i]->linesize[0] + selection_rect.x * pixel_size,
             selection_rect.w * pixel_size);
    }
  }

  // Create SDL texture from concatenated frame
  crop_preview_width_ = concat_width;
  crop_preview_height_ = selection_rect.h;

  if (use_10_bpc_) {
    // For 10-bit: create ARGB2101010 texture and convert from RGB48LE
    crop_preview_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB2101010,
                                              SDL_TEXTUREACCESS_STATIC, concat_width, selection_rect.h);
    if (!crop_preview_texture_) {
      std::cerr << "Failed to create 10-bit preview texture: " << SDL_GetError() << std::endl;
      destroy_crop_preview();
      return;
    }

    // Convert RGB48LE -> ARGB2101010
    const size_t row_bytes = concat_width * sizeof(uint32_t);
    std::vector<uint32_t> packed_buffer(concat_width * selection_rect.h);
    const AVFrame* concat = crop_preview_data_.concatenated;

    for (int y = 0; y < selection_rect.h; y++) {
      const uint16_t* src = reinterpret_cast<const uint16_t*>(concat->data[0] + y * concat->linesize[0]);
      uint32_t* dst = packed_buffer.data() + y * concat_width;
      for (int x = 0; x < concat_width; x++) {
        const uint16_t r = src[x * 3] >> 6;
        const uint16_t g = src[x * 3 + 1] >> 6;
        const uint16_t b = src[x * 3 + 2] >> 6;
        dst[x] = (3u << 30) | (static_cast<uint32_t>(r) << 20) | (static_cast<uint32_t>(g) << 10) | static_cast<uint32_t>(b);
      }
    }
    SDL_UpdateTexture(crop_preview_texture_, nullptr, packed_buffer.data(), row_bytes);
  } else {
    // For 8-bit: create RGB24 texture and upload directly
    crop_preview_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24,
                                              SDL_TEXTUREACCESS_STATIC, concat_width, selection_rect.h);
    if (!crop_preview_texture_) {
      std::cerr << "Failed to create preview texture: " << SDL_GetError() << std::endl;
      destroy_crop_preview();
      return;
    }
    SDL_UpdateTexture(crop_preview_texture_, nullptr,
                      crop_preview_data_.concatenated->data[0],
                      crop_preview_data_.concatenated->linesize[0]);
  }

  // Initialize preview zoom/pan to fit the entire image in the window
  const float scale_x = static_cast<float>(window_width_) / static_cast<float>(concat_width);
  const float scale_y = static_cast<float>(window_height_) / static_cast<float>(selection_rect.h);
  preview_scale_ = std::min(scale_x, scale_y) * 0.9F;  // 90% of window for margin
  preview_offset_ = Vector2D(
      (static_cast<float>(window_width_) - concat_width * preview_scale_) / 2.0F,
      (static_cast<float>(window_height_) - selection_rect.h * preview_scale_) / 2.0F);

  crop_preview_mode_ = CropPreviewMode::Active;

  // Pause playback while previewing
  play_ = false;
}
```

- [ ] **Step 2: Modify possibly_refresh to enter preview mode instead of saving**

In `possibly_refresh`, replace the existing save_selected_area block (around line 2749-2751):

```cpp
  // BEFORE (remove):
  if (save_selected_area_) {
    possibly_save_selected_area(left_frame, right_frame);
  }

  // AFTER (replace with):
  if (save_selected_area_ && selection_state_ == SelectionState::Completed) {
    enter_crop_preview(left_frame);
    selection_state_ = SelectionState::None;
    save_selected_area_ = false;
  }
```

- [ ] **Step 3: Add preview-mode short-circuit at the top of possibly_refresh**

At the very beginning of `possibly_refresh`, after the function signature, add:

```cpp
  // If crop preview is active, render preview and skip normal video rendering
  if (crop_preview_mode_ == CropPreviewMode::Active) {
    render_crop_preview();
    return true;
  }
```

- [ ] **Step 4: Stub out render_crop_preview (filled in Task 4)**

```cpp
void Display::render_crop_preview() {
  // Will be implemented in Task 4
  SDL_SetRenderDrawColor(renderer_, 32, 32, 32, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);
}
```

- [ ] **Step 5: Build and quick test**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Successful build. Running the app and pressing Shift+F, drawing a selection, and releasing should show a dark screen (stub preview) instead of immediately saving files.

- [ ] **Step 6: Commit**

```bash
git add display.h display.cpp
git commit -m "feat: create crop preview from selection instead of immediate save"
```

---

### Task 4: Render crop preview overlay

Implements the preview rendering: dark background, centered preview texture with zoom/pan applied, and instruction text overlay.

**Files:**
- Modify: `display.cpp` (implement `render_crop_preview`)

**Key context:**
- `preview_scale_` is window-pixels-per-image-pixel.
- `preview_offset_` is top-left of rendered image in window coordinates.
- SDL renderer works in drawable coordinates; multiply window coords by `drawable_to_window_*_factor_` to get drawable coords.
- Existing `render_text_with_fallback` creates an SDL_Surface from text. Pattern: create surface -> create texture -> render -> destroy.
- `BACKGROUND_COLOR` is the existing dark background constant.

- [ ] **Step 1: Implement render_crop_preview**

Replace the stub from Task 3:

```cpp
void Display::render_crop_preview() {
  // Dark background
  SDL_SetRenderDrawColor(renderer_, 24, 24, 24, 255);
  SDL_RenderClear(renderer_);

  // Render the preview texture with current zoom/pan
  if (crop_preview_texture_) {
    const float dw_factor = drawable_to_window_width_factor_;
    const float dh_factor = drawable_to_window_height_factor_;

    const SDL_FRect dst = {
        preview_offset_.x() * dw_factor,
        preview_offset_.y() * dh_factor,
        static_cast<float>(crop_preview_width_) * preview_scale_ * dw_factor,
        static_cast<float>(crop_preview_height_) * preview_scale_ * dh_factor};

    SDL_RenderCopyF(renderer_, crop_preview_texture_, nullptr, &dst);

    // Draw thin vertical separator lines between panels
    const int num_panels = 1 + static_cast<int>(crop_preview_data_.right_cutouts.size());
    if (num_panels > 1) {
      SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 128);
      SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
      const float panel_width = static_cast<float>(crop_preview_width_) / static_cast<float>(num_panels);
      for (int i = 1; i < num_panels; i++) {
        const float line_x = (preview_offset_.x() + panel_width * static_cast<float>(i) * preview_scale_) * dw_factor;
        SDL_RenderDrawLineF(renderer_, line_x, dst.y, line_x, dst.y + dst.h);
      }
      SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }
  }

  // Render instruction text at bottom center
  const std::string instructions = "Enter: Save  |  Esc: Cancel  |  Right-drag: Pan  |  Scroll: Zoom";
  SDL_Surface* text_surface = render_text_with_fallback(instructions);
  if (text_surface) {
    SDL_Texture* text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    if (text_texture) {
      const int text_w = text_surface->w;
      const int text_h = text_surface->h;
      const int padding = 10;

      // Background bar at bottom
      SDL_Rect bg_rect = {0, drawable_height_ - text_h - padding * 2, drawable_width_, text_h + padding * 2};
      SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
      SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
      SDL_RenderFillRect(renderer_, &bg_rect);
      SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

      // Center the text
      SDL_Rect text_rect = {(drawable_width_ - text_w) / 2, drawable_height_ - text_h - padding, text_w, text_h};
      SDL_RenderCopy(renderer_, text_texture, nullptr, &text_rect);

      SDL_DestroyTexture(text_texture);
    }
    SDL_FreeSurface(text_surface);
  }

  SDL_RenderPresent(renderer_);
}
```

- [ ] **Step 2: Build and test**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds. Running the app, pressing Shift+F, drawing a selection, and releasing should show the crop preview centered on a dark background with instruction text at the bottom. The preview shows the concatenated left+right crop panels with separator lines.

- [ ] **Step 3: Commit**

```bash
git add display.cpp
git commit -m "feat: render crop preview overlay with instructions"
```

---

### Task 5: Preview interaction — pan, zoom, confirm, cancel

Intercepts SDL events when crop preview is active. Implements right-click drag for panning, scroll wheel for cursor-centered zoom (affine transform preserving cursor pixel position), Enter to confirm save, Escape to cancel.

**Files:**
- Modify: `display.cpp` (modify `handle_event`, implement `exit_crop_preview`)

**Key context:**
- `handle_event` (display.cpp:2989) stores the event in `event_` and dispatches via `switch(event_.type)`.
- Existing video zoom uses `ZOOM_STEP_SIZE` constant and `wheel_sensitivity_` for scroll speed.
- Mouse coordinates `mouse_x_`, `mouse_y_` are in window coordinates.
- `event_.motion.xrel`, `event_.motion.yrel` are relative motion in window coordinates.

**Cursor-centered zoom formula:**
Given current offset `(ox, oy)`, scale `s`, mouse at `(mx, my)`, and new scale `s'`:
- The image pixel under the cursor: `ix = (mx - ox) / s`
- After zoom, same pixel must remain under cursor: `mx = ox' + ix * s'`
- Solving: `ox' = mx - (mx - ox) * (s' / s)`
- In vector form: `new_offset = offset + (mouse - offset) * (1 - ratio)` where `ratio = s' / s`

- [ ] **Step 1: Add preview event interception at the top of handle_event**

In `handle_event`, after `event_ = event;` and `input_received_ = true;` (line 2990-2991), add the preview event handler block:

```cpp
  // Intercept events when crop preview is active
  if (crop_preview_mode_ == CropPreviewMode::Active) {
    switch (event_.type) {
      case SDL_MOUSEWHEEL:
        if (event_.wheel.y != 0) {
          // Cursor-centered zoom (affine transform: cursor pixel stays fixed)
          float delta_zoom = wheel_sensitivity_ * event_.wheel.y * (event_.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
          if (delta_zoom > 0) {
            delta_zoom /= 2.0F;
          }

          const float new_scale = preview_scale_ * compute_zoom_factor(-delta_zoom);

          if (new_scale >= 0.01F && new_scale <= 1000.0F) {
            const float ratio = new_scale / preview_scale_;
            const float mx = static_cast<float>(mouse_x_);
            const float my = static_cast<float>(mouse_y_);

            // Affine transform: new_offset = offset + (mouse - offset) * (1 - ratio)
            // This preserves the image pixel under the cursor
            const float new_ox = preview_offset_.x() + (mx - preview_offset_.x()) * (1.0F - ratio);
            const float new_oy = preview_offset_.y() + (my - preview_offset_.y()) * (1.0F - ratio);

            preview_offset_ = Vector2D(new_ox, new_oy);
            preview_scale_ = new_scale;
          }
        }
        break;

      case SDL_MOUSEMOTION:
        SDL_GetMouseState(&mouse_x_, &mouse_y_);

        if (event_.motion.state & SDL_BUTTON_RMASK) {
          // Right-click drag: pan the preview
          const float dx = static_cast<float>(event_.motion.xrel);
          const float dy = static_cast<float>(event_.motion.yrel);
          preview_offset_ = preview_offset_ + Vector2D(dx, dy);
        }
        break;

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        // Update cursor for right-click pan indicator
        if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) {
          SDL_SetCursor(pan_mode_cursor_);
        } else {
          SDL_SetCursor(normal_mode_cursor_);
        }
        break;

      case SDL_KEYDOWN:
        switch (event_.key.keysym.sym) {
          case SDLK_RETURN:
          case SDLK_KP_ENTER:
            exit_crop_preview(true);   // Confirm: save images
            break;
          case SDLK_ESCAPE:
            exit_crop_preview(false);  // Cancel: discard
            break;
          case SDLK_r:
            // Reset preview zoom/pan to fit-in-window
            {
              const float scale_x = static_cast<float>(window_width_) / static_cast<float>(crop_preview_width_);
              const float scale_y = static_cast<float>(window_height_) / static_cast<float>(crop_preview_height_);
              preview_scale_ = std::min(scale_x, scale_y) * 0.9F;
              preview_offset_ = Vector2D(
                  (static_cast<float>(window_width_) - crop_preview_width_ * preview_scale_) / 2.0F,
                  (static_cast<float>(window_height_) - crop_preview_height_ * preview_scale_) / 2.0F);
            }
            break;
          default:
            break;
        }
        break;

      case SDL_WINDOWEVENT:
        // Handle window resize during preview
        if (event_.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event_.window.event == SDL_WINDOWEVENT_RESIZED) {
          handle_window_resize();
        }
        // Handle quit via window close
        if (event_.window.event == SDL_WINDOWEVENT_CLOSE) {
          quit_ = true;
        }
        break;

      case SDL_QUIT:
        quit_ = true;
        break;

      default:
        break;
    }
    return;  // Block all other events during preview
  }
```

- [ ] **Step 2: Implement exit_crop_preview**

```cpp
void Display::exit_crop_preview(const bool save) {
  if (crop_preview_mode_ != CropPreviewMode::Active) {
    return;
  }

  if (save) {
    save_crop_preview_images();
  } else {
    set_pending_message("Crop preview cancelled");
    std::cout << "Crop preview cancelled" << std::endl;
  }

  destroy_crop_preview();
  crop_preview_mode_ = CropPreviewMode::Inactive;

  // Restore normal cursor
  SDL_SetCursor(normal_mode_cursor_);
}
```

- [ ] **Step 3: Stub save_crop_preview_images (filled in Task 6)**

```cpp
void Display::save_crop_preview_images() {
  // Will be implemented in Task 6
  set_pending_message("Saving...");
}
```

- [ ] **Step 4: Build and test**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds. Running the app:
1. Shift+F, draw selection, release → shows crop preview
2. Right-click drag → pans the preview image
3. Scroll wheel → zooms centered on cursor (image pixel under cursor stays fixed)
4. Escape → returns to normal video display
5. Enter → triggers save (currently just a stub message)

- [ ] **Step 5: Commit**

```bash
git add display.cpp
git commit -m "feat: preview interaction with cursor-centered zoom and pan"
```

---

### Task 6: Save N+1 images on confirmation

Implements `save_crop_preview_images`: saves N individual right cutout PNGs and 1 concatenated PNG. Uses thread-parallel saving via the existing `write_png` utility and `PngSaver`.

**Files:**
- Modify: `display.cpp` (implement `save_crop_preview_images`)

**Key context:**
- `write_png(const AVFrame* frame, const std::string& filename, std::atomic_bool& error_occurred)` is a free function in display.cpp (line 1299) that wraps `PngSaver::save`.
- `get_file_stem` (display.cpp:137) extracts the stem from a file path.
- `strip_ffmpeg_patterns` cleans FFmpeg URL patterns from stems.
- `saved_selected_image_number_` is the auto-incrementing counter for cutout filenames.
- File naming: `{stem}_cutout_{number:04d}.png` for individuals, `{left_stem}_cutout_concat_{number:04d}.png` for the concatenation.

- [ ] **Step 1: Implement save_crop_preview_images**

Replace the stub from Task 5:

```cpp
void Display::save_crop_preview_images() {
  std::atomic_bool error_occurred(false);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) {
    return write_png(frame, filename, error_occurred);
  };

  // Compute file stems
  const std::string left_stem = strip_ffmpeg_patterns(get_file_stem(left_file_name_));
  std::vector<std::string> right_stems;
  for (const auto& fn : all_right_file_names_) {
    right_stems.push_back(strip_ffmpeg_patterns(get_file_stem(fn)));
  }

  // Build individual right cutout filenames
  // Map from valid_right_frames index to the original right video index
  std::vector<size_t> valid_indices;
  for (size_t i = 0; i < all_right_frames_.size(); i++) {
    if (all_right_frames_[i] != nullptr) {
      valid_indices.push_back(i);
    }
  }

  const int num = saved_selected_image_number_;
  std::vector<std::string> right_filenames;
  for (size_t vi = 0; vi < valid_indices.size(); vi++) {
    const size_t orig_idx = valid_indices[vi];
    const std::string& stem = (orig_idx < right_stems.size()) ? right_stems[orig_idx] : "unknown";

    // If there's only one right video, don't add an index suffix
    if (valid_indices.size() == 1) {
      right_filenames.push_back(string_sprintf("%s_cutout_%04d.png", stem.c_str(), num));
    } else {
      right_filenames.push_back(string_sprintf("%s_right%zu_cutout_%04d.png", stem.c_str(), orig_idx + 1, num));
    }
  }

  const std::string concat_filename = string_sprintf("%s_cutout_concat_%04d.png", left_stem.c_str(), num);

  // Save all images in parallel using threads
  std::vector<std::thread> save_threads;

  // Save individual right cutouts
  for (size_t i = 0; i < crop_preview_data_.right_cutouts.size(); i++) {
    if (i < right_filenames.size()) {
      save_threads.emplace_back(save_frame, crop_preview_data_.right_cutouts[i], right_filenames[i]);
    }
  }

  // Save concatenated image
  save_threads.emplace_back(save_frame, crop_preview_data_.concatenated, concat_filename);

  // Wait for all saves to complete
  for (auto& t : save_threads) {
    t.join();
  }

  if (!error_occurred) {
    // Build notification message
    std::string saved_files;
    for (size_t i = 0; i < right_filenames.size(); i++) {
      if (i > 0) {
        saved_files += ", ";
      }
      saved_files += right_filenames[i];
    }
    saved_files += ", " + concat_filename;

    const std::string message = string_sprintf("Saved %zu images: %s", right_filenames.size() + 1, saved_files.c_str());
    set_pending_message(message);
    std::cout << message << std::endl;

    saved_selected_image_number_++;
  }
}
```

- [ ] **Step 2: Build and test**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds. Full workflow test:
1. Load video-compare with 1+ right videos
2. Shift+F, draw selection, release → preview appears
3. Pan and zoom the preview to inspect
4. Press Enter → N+1 PNG files are saved in the current directory
5. Verify: N individual right cutout PNGs + 1 concatenated PNG (left + all rights)
6. File naming includes stems and index numbers

- [ ] **Step 3: Commit**

```bash
git add display.cpp
git commit -m "feat: save N+1 images (N right cutouts + 1 concat) on preview confirm"
```

---

### Task 7: Edge cases, cleanup, and polish

Handle edge cases and improve the user experience.

**Files:**
- Modify: `display.cpp` (various locations)

- [ ] **Step 1: Handle window resize during preview**

In `render_crop_preview`, the preview should re-center when the window is resized. The current implementation already handles this because `preview_offset_` is in window coordinates and `drawable_to_window_*_factor_` is updated by `handle_window_resize`. However, verify that `handle_window_resize` is called during preview mode (already added in Task 5's SDL_WINDOWEVENT handler).

No code change needed — verify behavior.

- [ ] **Step 2: Show panel labels on the preview**

Add panel labels (L, R1, R2, ...) below the preview image to identify which panel is which. In `render_crop_preview`, after rendering the preview texture and separator lines:

```cpp
  // Render panel labels below the preview
  {
    const float dw_factor = drawable_to_window_width_factor_;
    const float dh_factor = drawable_to_window_height_factor_;
    const int num_panels_for_labels = 1 + static_cast<int>(crop_preview_data_.right_cutouts.size());
    const float panel_pixel_width = static_cast<float>(crop_preview_width_) / static_cast<float>(num_panels_for_labels);

    auto render_panel_label = [&](const int panel_index, const std::string& label) {
      SDL_Surface* label_surface = render_text_with_fallback(label);
      if (!label_surface) {
        return;
      }
      SDL_Texture* label_texture = SDL_CreateTextureFromSurface(renderer_, label_surface);
      if (label_texture) {
        const float panel_center_x = (preview_offset_.x() + (static_cast<float>(panel_index) + 0.5F) * panel_pixel_width * preview_scale_) * dw_factor;
        const float label_y = (preview_offset_.y() + static_cast<float>(crop_preview_height_) * preview_scale_) * dh_factor + 4.0F;
        const int lw = label_surface->w;
        const int lh = label_surface->h;
        SDL_Rect label_rect = {static_cast<int>(panel_center_x) - lw / 2, static_cast<int>(label_y), lw, lh};
        SDL_RenderCopy(renderer_, label_texture, nullptr, &label_rect);
        SDL_DestroyTexture(label_texture);
      }
      SDL_FreeSurface(label_surface);
    };

    render_panel_label(0, "L");
    for (int i = 0; i < num_panels_for_labels - 1; i++) {
      render_panel_label(i + 1, string_sprintf("R%d", i + 1));
    }
  }
```

Insert this block in `render_crop_preview` after the separator lines block and before the instructions text block.

- [ ] **Step 3: Ensure Shift+F toggle works during preview**

If the user presses Shift+F while a preview is active, it should cancel the preview (same as Escape). In the preview event handler's `SDL_KEYDOWN` section (Task 5), add a case:

```cpp
          case SDLK_f:
            if (event_.key.keysym.mod & KMOD_SHIFT) {
              exit_crop_preview(false);  // Cancel
            }
            break;
```

- [ ] **Step 4: Clean up the old possibly_save_selected_area code path**

The `possibly_save_selected_area` and `save_selected_area` methods are no longer called from `possibly_refresh` (they were replaced by the preview flow in Task 3). However, keep them as internal utilities in case they're needed for future fallback. No deletion needed.

- [ ] **Step 5: Build and full test**

```bash
make -j$(nproc) 2>&1 | tail -20
```

Run a comprehensive test:
1. Single right video: Shift+F, draw selection → preview shows [L|R1]. Enter → saves 2 files.
2. Multiple right videos (Tab to switch): Shift+F, draw selection → preview shows [L|R1|R2|...]. Enter → saves N+1 files.
3. Zoom with scroll wheel: cursor position stays fixed (affine invariant). Zoom in on a detail, zoom out.
4. Pan with right-click drag: smooth pan in all directions.
5. Press R to reset zoom/pan to fit-in-window.
6. Press Escape to cancel without saving.
7. Press Enter to save: verify correct file names and image content.
8. Verify panel labels (L, R1, R2) appear below the preview.
9. Test with 10-bit video if available.

- [ ] **Step 6: Commit**

```bash
git add display.h display.cpp
git commit -m "feat: crop preview polish — panel labels, edge cases, Shift+F cancel"
```
