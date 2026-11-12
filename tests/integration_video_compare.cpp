#define SDL_MAIN_HANDLED
#include "string_utils.h"
#include "video_compare.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits.h>
#include <memory>
#include <mutex>
#include <functional>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <stdlib.h>
#endif
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

enum class Scenario { Baseline, EventInjection, Seek, StillSeek, SyncMismatch, MultiRightSync, FrameNavigation, FrameStepping, FrameSteppingVfr, SharedFrameStepping, BufferForwardOnly, BufferPingPong, SeekBurstForward, SeekBurstMixed, CropCopy, InteractiveCrop, DynamicSwap, RightStep, RightStepPlay, CropPreview, MixedRateRights, RightStepEdges, RefOverlay, RefStep };

std::atomic<int> events_pushed{0};
std::atomic<bool> watchdog_fired{false};

void reset_helper_counters() {
  events_pushed.store(0);
  watchdog_fired.store(false);
}

bool is_stress_scenario(const Scenario scenario) {
  return scenario == Scenario::SeekBurstForward || scenario == Scenario::SeekBurstMixed;
}

const char* scenario_name(const Scenario scenario) {
  switch (scenario) {
    case Scenario::Baseline:
      return "baseline";
    case Scenario::EventInjection:
      return "event-injection";
    case Scenario::Seek:
      return "seek";
    case Scenario::StillSeek:
      return "still-seek";
    case Scenario::SyncMismatch:
      return "sync-mismatch";
    case Scenario::MultiRightSync:
      return "multi-right-sync";
    case Scenario::FrameNavigation:
      return "frame-navigation";
    case Scenario::FrameStepping:
      return "frame-stepping";
    case Scenario::FrameSteppingVfr:
      return "frame-stepping-vfr";
    case Scenario::SharedFrameStepping:
      return "shared-frame-stepping";
    case Scenario::BufferForwardOnly:
      return "buffer-forward-only";
    case Scenario::BufferPingPong:
      return "buffer-pingpong";
    case Scenario::SeekBurstForward:
      return "seek-burst-forward";
    case Scenario::SeekBurstMixed:
      return "seek-burst-mixed";
    case Scenario::CropCopy:
      return "crop-copy";
    case Scenario::InteractiveCrop:
      return "interactive-crop";
    case Scenario::DynamicSwap:
      return "dynamic-swap";
    case Scenario::RightStep:
      return "right-step";
    case Scenario::RightStepPlay:
      return "right-step-play";
    case Scenario::CropPreview:
      return "crop-preview";
    case Scenario::MixedRateRights:
      return "mixed-rate-rights";
    case Scenario::RefOverlay:
      return "ref-overlay";
    case Scenario::RefStep:
      return "ref-step";
    case Scenario::RightStepEdges:
      return "right-step-edges";
  }
  return "unknown";
}

const char* stress_iteration_label() {
  const char* value = std::getenv("STRESS_ITERATION");
  return (value != nullptr && value[0] != '\0') ? value : "?";
}

int watchdog_ticks_for(const Scenario scenario) {
  // 100 ms per tick. Stress seeks serialize on drain/restart; 12 s is too tight.
  // crop-copy walks several crop/copy/undo/Shift+X rounds after the shared settle.
  if (scenario == Scenario::CropCopy || scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr || scenario == Scenario::MixedRateRights || scenario == Scenario::SharedFrameStepping) {
    return 400;
  }
  if (scenario == Scenario::InteractiveCrop || scenario == Scenario::RightStep || scenario == Scenario::RightStepPlay || scenario == Scenario::RightStepEdges || scenario == Scenario::RefStep) {
    return 250;
  }
  return is_stress_scenario(scenario) ? 250 : 120;
}

void sleep_ms(const int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct FrameState {
  bool playing{false};
  bool looping{false};
  int pending{0};
  size_t right_id{0};
  int64_t left_pts{INT64_MIN};
  int64_t right_pts{INT64_MIN};
  int64_t ref_pts{INT64_MIN};
};

// The application writes stdout on its main thread; the event script observes
// complete diagnostic lines through this buffer, never an unsynchronized stream.
class CapturedOutput : public std::streambuf {
 public:
  std::string str() {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_;
  }

  size_t generation() {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

  bool wait_for_frame(const size_t after, const std::chrono::steady_clock::time_point deadline, FrameState& frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!changed_.wait_until(lock, deadline, [&] { return generation_ > after; })) {
      return false;
    }
    frame = frame_;
    return true;
  }

 protected:
  std::streamsize xsputn(const char* data, const std::streamsize count) override {
    std::unique_lock<std::mutex> lock(mutex_);
    output_.append(data, static_cast<size_t>(count));
    line_.append(data, static_cast<size_t>(count));
    size_t end;
    while ((end = line_.find('\n')) != std::string::npos) {
      const std::string line = line_.substr(0, end);
      line_.erase(0, end + 1);
      int play = 0, pending = 0, loop = 0;
      size_t right_id = 0;
      long long left = 0, right = 0, ref = 0;
      if (std::sscanf(line.c_str(), "Frame state: play=%d pending=%d loop=%d right=%zu pts_left=%lld pts_right=%lld pts_ref=%lld",
          &play, &pending, &loop, &right_id, &left, &right, &ref) == 7) {
        // Optional harness-only delay reproduces slow CI responses without
        // changing playback, or assuming any fixed wait is long enough.
        const char* delay = std::getenv("VIDEO_COMPARE_TEST_QUERY_DELAY_MS");
        if (delay != nullptr) {
          lock.unlock();
          sleep_ms(std::max(0, std::min(1000, std::atoi(delay))));
          lock.lock();
        }
        frame_ = FrameState{play != 0, loop != 0, pending, right_id, left, right, ref};
        ++generation_;
        changed_.notify_all();
      }
    }
    return count;
  }

  int_type overflow(const int_type ch) override {
    if (!traits_type::eq_int_type(ch, traits_type::eof())) {
      const char byte = traits_type::to_char_type(ch);
      xsputn(&byte, 1);
    }
    return traits_type::not_eof(ch);
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string output_;
  std::string line_;
  FrameState frame_;
  size_t generation_{0};
};

int native_frame_index(const int64_t pts, const bool variable = false, const int fps = 25) {
  if (pts == INT64_MIN) {
    return -1;
  }
  const int ticks = static_cast<int>(std::llround(pts * (fps / 1000000.0)));
  if (variable) {
    return ticks % 4 <= 1 ? ticks / 4 * 2 + ticks % 4 : -1;
  }
  return ticks;
}

void require_readable_file(const char* path) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    throw std::runtime_error(std::string("fixture is not readable: ") + path);
  }
  std::fclose(file);
}

void prepare_dummy_software_sdl() {
  SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv("SDL_RENDER_DRIVER", "software", 1);
}

Uint32 first_sdl_window_id() {
  for (Uint32 id = 1; id < 32; ++id) {
    if (SDL_GetWindowFromID(id) != nullptr) {
      return id;
    }
  }
  return 0;
}

void push_quit() {
  SDL_Event event{};
  event.type = SDL_QUIT;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(SDL_QUIT) failed: %s\n", SDL_GetError());
  } else {
    events_pushed.fetch_add(1);
  }
}

void push_keydown(const SDL_Keycode key, const SDL_Scancode scancode, const Uint32 window_id, const SDL_Keymod mod = KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_KEYDOWN;
  event.key.type = SDL_KEYDOWN;
  event.key.timestamp = SDL_GetTicks();
  event.key.windowID = window_id;
  event.key.state = SDL_PRESSED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scancode;
  event.key.keysym.sym = key;
  event.key.keysym.mod = mod;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(KEYDOWN) failed: %s\n", SDL_GetError());
  } else {
    events_pushed.fetch_add(1);
  }
}

class FrameProbe {
 public:
  FrameProbe(CapturedOutput& output, std::vector<FrameState>& checkpoints, const bool variable)
      : output_(output), checkpoints_(checkpoints), variable_(variable) {}

  FrameState wait(const char* label, const std::function<bool(const FrameState&)>& ready, const bool record = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    FrameState state;
    while (std::chrono::steady_clock::now() < deadline) {
      const size_t after = output_.generation();
      push_keydown(SDLK_x, SDL_SCANCODE_X, 0, KMOD_SHIFT);
      if (!output_.wait_for_frame(after, deadline, state)) {
        break;
      }
      if (ready(state)) {
        if (record) {
          checkpoints_.push_back(state);
        }
        return state;
      }
      sleep_ms(10);
    }
    throw std::runtime_error(string_sprintf("%s timed out: play=%d pending=%d loop=%d right=%zu pts=%lld/%lld/%lld",
        label, state.playing, state.pending, state.looping, state.right_id,
        static_cast<long long>(state.left_pts), static_cast<long long>(state.right_pts), static_cast<long long>(state.ref_pts)));
  }

  FrameState at(const char* label, const int left, const int right, const int ref, const size_t right_id = 1, const int right_fps = 25) {
    return wait(label, [&](const FrameState& state) {
      return !state.playing && !state.looping && state.pending == 0 && state.right_id == right_id &&
          index(state.left_pts) == left && native_frame_index(state.right_pts, variable_, right_fps) == right &&
          (ref < 0 || index(state.ref_pts) == ref);
    });
  }

  int index(const int64_t pts) const { return native_frame_index(pts, variable_); }

 private:
  CapturedOutput& output_;
  std::vector<FrameState>& checkpoints_;
  const bool variable_;
};

void push_keyup(const SDL_Keycode key, const SDL_Scancode scancode, const Uint32 window_id) {
  SDL_Event event{};
  event.type = SDL_KEYUP;
  event.key.type = SDL_KEYUP;
  event.key.timestamp = SDL_GetTicks();
  event.key.windowID = window_id;
  event.key.state = SDL_RELEASED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scancode;
  event.key.keysym.sym = key;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(KEYUP) failed: %s\n", SDL_GetError());
  } else {
    events_pushed.fetch_add(1);
  }
}

SDL_Keymod clipboard_mod() {
#ifdef __APPLE__
  return static_cast<SDL_Keymod>(KMOD_GUI);
#else
  return static_cast<SDL_Keymod>(KMOD_CTRL);
#endif
}

void push_copy_timestamp() {
  push_keydown(SDLK_c, SDL_SCANCODE_C, 0, clipboard_mod());
}

void push_crop_gesture(const SDL_Keycode side_key, const SDL_Scancode side_scancode, const int x0, const int y0, const int x1, const int y1) {
  SDL_Event events[5]{};
  const Uint32 timestamp = SDL_GetTicks();

  events[0].type = SDL_KEYDOWN;
  events[0].key.type = SDL_KEYDOWN;
  events[0].key.timestamp = timestamp;
  events[0].key.windowID = 0;
  events[0].key.state = SDL_PRESSED;
  events[0].key.repeat = 0;
  events[0].key.keysym.scancode = side_scancode;
  events[0].key.keysym.sym = side_key;
  events[0].key.keysym.mod = KMOD_SHIFT;

  events[1].type = SDL_MOUSEMOTION;
  events[1].motion.type = SDL_MOUSEMOTION;
  events[1].motion.timestamp = timestamp;
  events[1].motion.windowID = 0;
  events[1].motion.x = x0;
  events[1].motion.y = y0;

  events[2].type = SDL_MOUSEBUTTONDOWN;
  events[2].button.type = SDL_MOUSEBUTTONDOWN;
  events[2].button.timestamp = timestamp;
  events[2].button.windowID = 0;
  events[2].button.button = SDL_BUTTON_LEFT;
  events[2].button.state = SDL_PRESSED;
  events[2].button.clicks = 1;
  events[2].button.x = x0;
  events[2].button.y = y0;

  events[3].type = SDL_MOUSEMOTION;
  events[3].motion.type = SDL_MOUSEMOTION;
  events[3].motion.timestamp = timestamp;
  events[3].motion.windowID = 0;
  events[3].motion.x = x1;
  events[3].motion.y = y1;

  events[4].type = SDL_MOUSEBUTTONUP;
  events[4].button.type = SDL_MOUSEBUTTONUP;
  events[4].button.timestamp = timestamp;
  events[4].button.windowID = 0;
  events[4].button.button = SDL_BUTTON_LEFT;
  events[4].button.state = SDL_RELEASED;
  events[4].button.clicks = 1;
  events[4].button.x = x1;
  events[4].button.y = y1;

  constexpr int kEventCount = sizeof(events) / sizeof(events[0]);
  const int added = SDL_PeepEvents(events, kEventCount, SDL_ADDEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
  if (added < 0) {
    std::fprintf(stderr, "SDL_PeepEvents(crop gesture) failed: %s\n", SDL_GetError());
    return;
  }

  events_pushed.fetch_add(added);
  if (added != kEventCount) {
    std::fprintf(stderr, "SDL_PeepEvents(crop gesture) added %d/%d events\n", added, kEventCount);
  }
}

void wait_for_filter_refresh() {
  // Crop/copy/clear/undo force a same-position seek and filter rebuild. Shift+X
  // reads Display metadata, which updates only after the new frames refresh.
  sleep_ms(700);
}

void dump_display_filters() {
  push_keydown(SDLK_x, SDL_SCANCODE_X, 0, KMOD_SHIFT);
  sleep_ms(200);
}

void select_right_video(const int index) {
  push_keydown(static_cast<SDL_Keycode>(SDLK_1 + index), static_cast<SDL_Scancode>(SDL_SCANCODE_1 + index), 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  sleep_ms(300);
}

void toggle_swap() {
  push_keydown(SDLK_s, SDL_SCANCODE_S, 0);
  sleep_ms(200);
}

void interactive_crop(const SDL_Keycode side_key, const SDL_Scancode side_scancode, const int x0, const int y0, const int x1, const int y1) {
  // Queue crop-mode entry and the complete drag as one transaction. This keeps
  // the synthetic gesture contiguous even if the platform pumps native mouse
  // events between main-loop iterations.
  push_crop_gesture(side_key, side_scancode, x0, y0, x1, y1);
  wait_for_filter_refresh();
}

void copy_visual_left_crop_to_others() {
  // Ctrl+Shift+L: visual-left crop -> right-side video(s). After Swap that is only the visual right.
  push_keydown(SDLK_l, SDL_SCANCODE_L, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  wait_for_filter_refresh();
}

void copy_visual_right_crop_to_left() {
  push_keydown(SDLK_r, SDL_SCANCODE_R, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  wait_for_filter_refresh();
}

void clear_visual_left_crop() {
  // Ctrl+L: clear the video currently displayed on the left.
  push_keydown(SDLK_l, SDL_SCANCODE_L, 0, KMOD_CTRL);
  wait_for_filter_refresh();
}

void clear_visual_right_crop() {
  // Ctrl+R: clear the video currently displayed on the right.
  push_keydown(SDLK_r, SDL_SCANCODE_R, 0, KMOD_CTRL);
  wait_for_filter_refresh();
}

void undo_last_crop() {
  push_keydown(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0);
  wait_for_filter_refresh();
}

std::string extract_quoted_field(const std::string& line, const std::string& key) {
  const std::string prefix = key + "=\"";
  const size_t start = line.find(prefix);
  if (start == std::string::npos) {
    return std::string();
  }
  std::string value;
  for (size_t i = start + prefix.size(); i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      value.push_back(line[i + 1]);
      ++i;
      continue;
    }
    if (line[i] == '"') {
      break;
    }
    value.push_back(line[i]);
  }
  return value;
}

std::string extract_unquoted_field(const std::string& line, const std::string& key) {
  const std::string token = " " + key + "=";
  const size_t start = line.find(token);
  if (start == std::string::npos) {
    return std::string();
  }
  const size_t value_start = start + token.size();
  const size_t value_end = line.find(' ', value_start);
  return line.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

struct FilterDump {
  std::string left;
  std::string right;
  std::string swapped;
  std::string right_id;
  std::string visual_left;
  std::string visual_right;
  std::string aspect;
  std::string window;
  std::string content;
  std::string ref_dar;
  std::string frame_left;
  std::string frame_right;
  std::string frame_ref;
};

std::vector<FilterDump> parse_display_filters(const std::string& output) {
  std::vector<FilterDump> dumps;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("Display state:") == std::string::npos) {
      continue;
    }
    FilterDump dump;
    dump.left = extract_quoted_field(line, "filters_left");
    dump.right = extract_quoted_field(line, "filters_right");
    dump.swapped = extract_unquoted_field(line, "swapped");
    dump.right_id = extract_unquoted_field(line, "right");
    dump.visual_left = extract_unquoted_field(line, "visual_left");
    dump.visual_right = extract_unquoted_field(line, "visual_right");
    dump.aspect = extract_unquoted_field(line, "aspect");
    dump.window = extract_unquoted_field(line, "window");
    dump.content = extract_unquoted_field(line, "content");
    dump.ref_dar = extract_unquoted_field(line, "ref_dar");
    dump.frame_left = extract_unquoted_field(line, "frame_left");
    dump.frame_right = extract_unquoted_field(line, "frame_right");
    dump.frame_ref = extract_unquoted_field(line, "frame_ref");
    dumps.push_back(dump);
  }
  return dumps;
}

CropState crop_state_from_token(const std::string& token) {
  CropState state;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  if (std::sscanf(token.c_str(), "crop=%d:%d:%d:%d", &width, &height, &x, &y) == 4) {
    state.enabled = true;
    state.rect = {x, y, width, height};
  }
  return state;
}

std::string mapped_crop_token(const std::string& source_token, const int source_w, const int source_h, const int dest_w, const int dest_h) {
  const CropState mapped = video_filter_state::map_crop_state(crop_state_from_token(source_token), source_w, source_h, dest_w, dest_h);
  return mapped.enabled ? video_filter_state::crop_filter(mapped.rect) : std::string();
}

struct WindowSize {
  int w{0};
  int h{0};
};

struct ContentViewport {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};

bool same_viewport(const ContentViewport& a, const ContentViewport& b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool parse_window_size(const std::string& text, WindowSize* size) {
  return std::sscanf(text.c_str(), "%dx%d", &size->w, &size->h) == 2 && size->w > 0 && size->h > 0;
}

bool parse_content_viewport(const std::string& text, ContentViewport* viewport) {
  return std::sscanf(text.c_str(), "%d,%d,%dx%d", &viewport->x, &viewport->y, &viewport->w, &viewport->h) == 4 && viewport->w > 0 && viewport->h > 0;
}

bool parse_dar(const std::string& text, int* num, int* den) {
  return std::sscanf(text.c_str(), "%d:%d", num, den) == 2 && *num > 0 && *den > 0;
}

bool dar_equals(const int num, const int den, const int want_num, const int want_den) {
  return den != 0 && want_den != 0 && num * want_den == den * want_num;
}

// Same rounding as Display::update_content_window_layout() for one frame aspect.
ContentViewport viewport_for_aspect(const WindowSize& window, const float content_aspect) {
  ContentViewport viewport;
  const float window_aspect = static_cast<float>(window.w) / static_cast<float>(window.h);
  if (window_aspect > content_aspect) {
    viewport.h = window.h;
    const int rounded = static_cast<int>(std::round(static_cast<float>(viewport.h) * content_aspect));
    viewport.w = rounded < 1 ? 1 : rounded;
    viewport.x = (window.w - viewport.w) / 2;
  } else {
    viewport.w = window.w;
    const int rounded = static_cast<int>(std::round(static_cast<float>(viewport.w) / content_aspect));
    viewport.h = rounded < 1 ? 1 : rounded;
    viewport.y = (window.h - viewport.h) / 2;
  }
  return viewport;
}

std::string crop_token(const std::string& filters) {
  const size_t start = filters.find("crop=");
  if (start == std::string::npos) {
    return std::string();
  }
  size_t end = start;
  while (end < filters.size() && filters[end] != ',' && filters[end] != '"') {
    ++end;
  }
  return filters.substr(start, end - start);
}

void print_filter_dumps(const std::vector<FilterDump>& dumps) {
  for (size_t i = 0; i < dumps.size(); ++i) {
    std::printf("crop-copy dump %zu: swapped=%s right=%s visual_left=%s visual_right=%s left=\"%s\" right_filters=\"%s\" crop_left=%s crop_right=%s\n", i, dumps[i].swapped.c_str(),
                dumps[i].right_id.c_str(), dumps[i].visual_left.c_str(), dumps[i].visual_right.c_str(), dumps[i].left.c_str(), dumps[i].right.c_str(), crop_token(dumps[i].left).c_str(),
                crop_token(dumps[i].right).empty() ? "(none)" : crop_token(dumps[i].right).c_str());
  }
}

bool parse_hhmmss(const std::string& text, double* seconds) {
  int hours = 0;
  int minutes = 0;
  int secs = 0;
  int millis = 0;
  if (std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &secs, &millis) != 4) {
    return false;
  }
  *seconds = static_cast<double>(hours * 3600 + minutes * 60 + secs) + static_cast<double>(millis) / 1000.0;
  return true;
}

bool file_is_non_empty(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string resolve_path(const std::string& path) {
#if defined(_WIN32)
  char resolved[_MAX_PATH];
  if (::_fullpath(resolved, path.c_str(), sizeof(resolved)) == nullptr) {
    throw std::runtime_error("cannot resolve path: " + path);
  }
#else
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr) {
    throw std::runtime_error("cannot resolve path: " + path);
  }
#endif
  return resolved;
}

// F writes PNGs into cwd. Isolate still-seek dumps so the repo root stays clean.
struct DumpDirectory {
  std::string previous;
  std::string path;

  explicit DumpDirectory(const char* prefix) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
      throw std::runtime_error("getcwd failed");
    }
    previous = cwd;

    const char* tmp = std::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = std::getenv("TEMP");
    }
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = "/tmp";
    }

    std::string tmpl = std::string(tmp) + "/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path = buf.data();
    if (::chdir(path.c_str()) != 0) {
      throw std::runtime_error("chdir to dump directory failed");
    }
  }

  ~DumpDirectory() {
    const char* names[] = {"screenshot_1_0001.png", "screenshot_2_0001.png", "screenshot_1_screenshot_2_osd_0001.png"};
    for (const char* name : names) {
      unlink((path + "/" + name).c_str());
    }
    if (::chdir(previous.c_str()) != 0) {
      std::fprintf(stderr, "warning: failed to restore working directory to %s\n", previous.c_str());
    }
    rmdir(path.c_str());
  }
};

void print_pts_sequence(const char* label, const std::vector<double>& times) {
  std::fprintf(stderr, "%s", label);
  for (size_t i = 0; i < times.size(); ++i) {
    std::fprintf(stderr, "%s%.3f", i == 0 ? "" : " ", times[i]);
  }
  std::fprintf(stderr, "\n");
}

std::vector<double> unique_pts(const std::vector<double>& times) {
  std::vector<double> out;
  for (const double t : times) {
    if (out.empty() || std::fabs(t - out.back()) > 0.015) {
      out.push_back(t);
    }
  }
  return out;
}

bool pts_near(const double a, const double b) {
  return std::fabs(a - b) <= 0.015;
}

void print_pts_stdout(const char* label, const std::vector<double>& times) {
  std::printf("%s", label);
  for (size_t i = 0; i < times.size(); ++i) {
    std::printf("%s%.3f", i == 0 ? "" : " ", times[i]);
  }
  std::printf("\n");
}

std::vector<double> copied_positions(const std::string& output) {
  std::vector<double> times;
  const std::string prefix = "Copied to clipboard: ";
  size_t pos = 0;
  while ((pos = output.find(prefix, pos)) != std::string::npos) {
    pos += prefix.size();
    const size_t end = output.find_first_of("\r\n", pos);
    const std::string token = output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    double seconds = 0.0;
    if (parse_hhmmss(token, &seconds)) {
      times.push_back(seconds);
    }
  }
  return times;
}

void push_relative_seek(const int direction) {
  if (direction > 0) {
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
  } else {
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
  }
}

void print_stress_failure(const Scenario scenario, const char* message, const std::vector<double>& times, const double expected_storm) {
  std::fprintf(stderr, "FAIL %s iteration=%s events_pushed=%d watchdog=%s", scenario_name(scenario), stress_iteration_label(), events_pushed.load(),
               watchdog_fired.load() ? "fired" : "idle");
  if (times.size() > 0) {
    std::fprintf(stderr, " T0=%.3f", times[0]);
  }
  if (times.size() > 1) {
    std::fprintf(stderr, " T1=%.3f storm=%.3f expected_storm=%.3f", times[1], times[1] - times[0], expected_storm);
  }
  if (times.size() > 2) {
    std::fprintf(stderr, " T2=%.3f T2-T1=%.3f", times[2], times[2] - times[1]);
  }
  if (!times.empty()) {
    std::fprintf(stderr, " last_pts=%.3f", times.back());
  }
  std::fprintf(stderr, " %s\n", message);
}

void run_event_script_body(const Scenario scenario, CapturedOutput& captured_out, std::vector<FrameState>& checkpoints) {
  // Construction already created Display/SDL. operator() starts workers
  // immediately after this thread is launched; wait for first frames.
  sleep_ms(1000);

  if (scenario == Scenario::EventInjection) {
    const Uint32 existing_window_id = first_sdl_window_id();
    // Display::handle_event does not filter KEYDOWN by windowID. ScopeManager
    // only consumes events with a non-zero id that matches an open scope.
    // windowID 0 is the case we need to prove is accepted.
    std::fprintf(stderr, "event-injection: first SDL window id=%u; pushing Tab with windowID=0\n", existing_window_id);
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(500);
  } else if (scenario == Scenario::Seek) {
    // Pause so the copied PTS is a stable pre-seek landing, not a moving playhead.
    // The seek transaction itself does not consult play_; pause only freezes fetch.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(300);
    // RIGHT/LEFT are relative ±1.0s (KMOD_NONE). begin_input_frame clears
    // seek_from_start, so these are not timeline-from-start seeks.
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(300);
  } else if (scenario == Scenario::StillSeek) {
    // JPEG/image2 becomes SingleFrame after the first unique PTS, then EOF.
    // The 1000 ms settle above is required: a JPEG input alone does not mean
    // single_frame is already true when the first event arrives.
    // RIGHT only: plan_seek rewrites seek_relative from left.delta_pts
    // before the backward decision, so LEFT (-1.0) becomes the same
    // forward still-seek when delta_pts > 0.
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(600);
    // Proof of life after skip_update + buffer restore. Do not quit
    // immediately after the seek. F dumps the presented stills.
    push_keydown(SDLK_f, SDL_SCANCODE_F, 0);
    sleep_ms(500);
  } else if (scenario == Scenario::SyncMismatch) {
    // Natural playback only. The shared 1000 ms settle plus this extra wait
    // give 25-vs-30 enough dual-pops for right to cross min_delta repeatedly.
    sleep_ms(800);
    push_copy_timestamp();
    sleep_ms(300);
  } else if (scenario == Scenario::MultiRightSync) {
    // Unpaused 30/25/25 playback. Tab is after the sync window so it does
    // not change active-right tolerance while the repeated-left-pop path runs.
    sleep_ms(1000);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(200);
  } else if (scenario == Scenario::RefOverlay) {
    // Tab must not land on the reference, and holding Q over playing video
    // must neither stall playback nor leave it stuck once released.
    sleep_ms(600);
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(200);
    push_keydown(SDLK_q, SDL_SCANCODE_Q, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(200);
    push_keyup(SDLK_q, SDL_SCANCODE_Q, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::FrameNavigation) {
    // Pause isolates one-frame movement from continuous fetch. SPACE
    // toggles play_ to false; Shift+D still fetches via
    // forward_navigate_frames; Shift+A rewinds decoded frames.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_copy_timestamp();
    sleep_ms(200);
    // Shift only — do not combine with the clipboard modifier.
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, KMOD_SHIFT);
    sleep_ms(500);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, KMOD_SHIFT);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::SharedFrameStepping || scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr) {
    const bool variable = scenario == Scenario::FrameSteppingVfr;
    FrameProbe probe(captured_out, checkpoints, variable);
    auto key = [](const SDL_Keycode code, const SDL_Scancode scancode, const SDL_Keymod mod = KMOD_NONE) {
      push_keydown(code, scancode, 0, mod);
    };
    // Wait for actual playback before pausing. Startup latency is platform-specific.
    probe.wait("initial playback", [&](const FrameState& state) {
      return state.playing && probe.index(state.left_pts) >= 12 && state.left_pts == state.right_pts && state.left_pts == state.ref_pts;
    }, false);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE);
    const auto initial_state = probe.wait("initial pause", [&](const FrameState& state) {
      return !state.playing && state.pending == 0 && probe.index(state.left_pts) >= 12 && state.left_pts == state.right_pts && state.left_pts == state.ref_pts;
    });
    const int initial = probe.index(initial_state.left_pts);
    if (scenario == Scenario::SharedFrameStepping) {
      key(SDLK_MINUS, SDL_SCANCODE_MINUS, KMOD_CTRL);
      probe.at("independent rewind", initial, initial - 10, initial);
      key(SDLK_PLUS, SDL_SCANCODE_EQUALS, KMOD_CTRL);
      probe.at("independent replay", initial, initial, initial);
      for (int i = 0; i < 20; ++i) {
        key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      }
      probe.at("shared decoder split", initial + 20, initial + 20, initial + 20);
      key(SDLK_TAB, SDL_SCANCODE_TAB);
      probe.at("inactive right pipeline", initial + 20, initial + 20, initial + 20, 2);
    } else {
      int current = initial;
      size_t active = 1;
      for (int direction : {-1, 1}) {
        for (int i = 0; i < 6; ++i) {
          key(direction < 0 ? SDLK_a : SDLK_d, direction < 0 ? SDL_SCANCODE_A : SDL_SCANCODE_D, KMOD_SHIFT);
          key(SDLK_TAB, SDL_SCANCODE_TAB);
          active = 3 - active;
          current += direction;
          probe.at("adjacent native frames", current, current, current, active);
        }
      }
      key(SDLK_a, SDL_SCANCODE_A);
      probe.at("browse previous frame", initial - 1, initial - 1, initial - 1);
      key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      probe.at("step from browsed frame", initial, initial, initial);
      key(SDLK_MINUS, SDL_SCANCODE_MINUS, KMOD_CTRL);
      probe.at("right ten-frame rewind", initial, initial - 10, initial);
      key(SDLK_PLUS, SDL_SCANCODE_EQUALS, KMOD_CTRL);
      probe.at("right ten-frame replay", initial, initial, initial);
      key(SDLK_q, SDL_SCANCODE_Q);
      key(SDLK_MINUS, SDL_SCANCODE_MINUS, KMOD_CTRL);
      push_keyup(SDLK_q, SDL_SCANCODE_Q, 0);
      probe.at("reference ten-frame rewind", initial, initial, initial - 10);
      key(SDLK_q, SDL_SCANCODE_Q);
      key(SDLK_PLUS, SDL_SCANCODE_EQUALS, KMOD_CTRL);
      push_keyup(SDLK_q, SDL_SCANCODE_Q, 0);
      probe.at("reference ten-frame replay", initial, initial, initial);
      for (int i = 0; i < 100; ++i) {
        key(SDLK_a, SDL_SCANCODE_A, KMOD_SHIFT);
      }
      probe.at("start clamp", 0, 0, 0);
      key(SDLK_a, SDL_SCANCODE_A, KMOD_SHIFT);
      probe.at("stable start clamp", 0, 0, 0);
      key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      probe.at("second frame", 1, 1, 1);
      key(SDLK_a, SDL_SCANCODE_A, KMOD_SHIFT);
      probe.at("first frame", 0, 0, 0);
      key(SDLK_SPACE, SDL_SCANCODE_SPACE);
      // Re-alignment may take longer than 250 ms; pause only after playback
      // has actually advanced, then observe one coherent paused snapshot.
      probe.wait("resumed playback", [&](const FrameState& state) {
        return state.playing && probe.index(state.left_pts) > 1 && state.left_pts == state.right_pts && state.left_pts == state.ref_pts;
      }, false);
      key(SDLK_SPACE, SDL_SCANCODE_SPACE);
      probe.wait("pause after resumed playback", [&](const FrameState& state) {
        return !state.playing && state.pending == 0 && probe.index(state.left_pts) > 1 && state.left_pts == state.right_pts && state.left_pts == state.ref_pts;
      });
      for (int i = 0; i < 100; ++i) {
        key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      }
      const int last = variable ? 49 : 99;
      // Wait for the batch counter to drain as well as for the final frame.
      probe.at("batch reaches EOF", last, last, last);
      key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      probe.at("stable EOF clamp", last, last, last);
      key(SDLK_a, SDL_SCANCODE_A, KMOD_SHIFT);
      probe.at("rewind after EOF", last - 1, last - 1, last - 1);
      key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
      probe.at("replay final frame", last, last, last);
    }
  } else if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    // Fill the 3-slot history, pause so offset 0 stays on the newest frame,
    // then slow to 1/16x so timer-driven steps are ~640 ms. J is the
    // ordinary playback-speed control (6 presses per octave).
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    for (int i = 0; i < 24; ++i) {
      push_keydown(SDLK_j, SDL_SCANCODE_J, 0);
    }
    sleep_ms(200);
    push_copy_timestamp();
    sleep_ms(200);
    if (scenario == Scenario::BufferForwardOnly) {
      push_keydown(SDLK_PERIOD, SDL_SCANCODE_PERIOD, 0);
    } else {
      push_keydown(SDLK_COMMA, SDL_SCANCODE_COMMA, 0);
    }
    // Period/comma pauses decode and starts timer-driven offset updates.
    // The first step is armed on that same iteration after refresh.
    // 1/16x makes each step ~640 ms; copy every 200 ms and collapse
    // consecutive duplicates so we recover the logical sequence.
    for (int i = 0; i < 18; ++i) {
      sleep_ms(200);
      push_copy_timestamp();
    }
    sleep_ms(200);
  } else if (scenario == Scenario::SeekBurstForward || scenario == Scenario::SeekBurstMixed) {
    // Pause so clipboard copies are a stable playhead and the post-storm
    // Shift+D health probe is a one-frame fetch, not continuous playback.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(300);

    if (scenario == Scenario::SeekBurstForward) {
      // Same-poll accumulation: five RIGHT with no delay.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
      }
      // Faster-than-service burst. Drain/seek is tens of ms; 10 ms spacing
      // lets later events queue while the first transaction is still running.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
        sleep_ms(10);
      }
      sleep_ms(1200);
    } else {
      // Move well into the 20 s clip before LEFT events so we stay off 0.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
      }
      sleep_ms(800);
      const int mixed[] = {1, 1, 1, 1, 1, -1, -1, -1, 1, 1, 1, 1, -1, -1};
      for (const int direction : mixed) {
        push_relative_seek(direction);
        sleep_ms(10);
      }
      sleep_ms(1500);
    }

    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, KMOD_SHIFT);
    sleep_ms(500);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::CropCopy) {
    // Window and generated clips are both 320x180. Distinct drags produce
    // distinct crop= tokens in the Shift+X filter dump. Do not hard-code the
    // mapped pixels; later assertions compare those captured tokens.
    const int left_x0 = 20, left_y0 = 20, left_x1 = 140, left_y1 = 100;
    const int right1_x0 = 50, right1_y0 = 40, right1_x1 = 170, right1_y1 = 120;
    const int right0_x0 = 30, right0_y0 = 50, right0_x1 = 150, right0_y1 = 130;
    const int right1_again_x0 = 70, right1_again_y0 = 20, right1_again_x1 = 190, right1_again_y1 = 90;

    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);

    // 1. Normal Ctrl+Shift+L: crop logical left, give R0 and R1 different crops,
    //    leave R2 uncropped, copy visual left to all rights, then undo.
    interactive_crop(SDLK_l, SDL_SCANCODE_L, left_x0, left_y0, left_x1, left_y1);
    dump_display_filters();
    toggle_swap();
    dump_display_filters();
    toggle_swap();

    select_right_video(1);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right1_x0, right1_y0, right1_x1, right1_y1);
    dump_display_filters();

    select_right_video(0);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right0_x0, right0_y0, right0_x1, right0_y1);
    dump_display_filters();

    copy_visual_left_crop_to_others();
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    undo_last_crop();
    select_right_video(0);
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    // 2. Swapped Ctrl+Shift+L: R1 on visual left is the source. Only the visual
    //    right (logical left) is a destination.
    select_right_video(1);
    toggle_swap();
    copy_visual_left_crop_to_others();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    undo_last_crop();
    select_right_video(0);
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();
    toggle_swap();

    // 3. Ctrl+Shift+R normal (current right -> logical left) then swapped
    //    (logical left -> current right).
    select_right_video(1);
    copy_visual_right_crop_to_left();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    dump_display_filters();

    select_right_video(0);
    toggle_swap();
    copy_visual_right_crop_to_left();
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    toggle_swap();

    // 4. Undo keeps the concrete side from the operation, not the later
    //    selected right. Crop R1, switch to R2, Backspace.
    select_right_video(1);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right1_again_x0, right1_again_y0, right1_again_x1, right1_again_y1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();

    // 5. Ctrl+L/R follow visual sides and only the displayed video.
    clear_visual_left_crop();
    dump_display_filters();
    undo_last_crop();
    clear_visual_right_crop();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    dump_display_filters();

    toggle_swap();
    clear_visual_left_crop();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    clear_visual_right_crop();
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    toggle_swap();
  } else if (scenario == Scenario::InteractiveCrop) {
    // Left dest is 160x90 after post-scale; right dest stays 320x180. Canvas
    // is 320x180. The same window drag must become crop=120:80:20:20 in
    // 320x180 crop-space, not dest-space crop=60:40:10:10.
    const int x0 = 20, y0 = 20, x1 = 140, y1 = 100;

    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);

    dump_display_filters();
    interactive_crop(SDLK_b, SDL_SCANCODE_B, x0, y0, x1, y1);
    dump_display_filters();
    interactive_crop(SDLK_l, SDL_SCANCODE_L, x0, y0, x1, y1);
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    interactive_crop(SDLK_r, SDL_SCANCODE_R, x0, y0, x1, y1);
    dump_display_filters();
    toggle_swap();
    interactive_crop(SDLK_l, SDL_SCANCODE_L, x0, y0, x1, y1);
    dump_display_filters();
  } else if (scenario == Scenario::DynamicSwap) {
    // 320x240 (4:3) left, 640x360 (16:9) right. Canvas/window stay 16:9;
    // Dynamic pillarboxes to the original left. Swap must not retarget either.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    dump_display_filters();
    toggle_swap();
    dump_display_filters();
  } else if (scenario == Scenario::RightStep) {
    // Paused throughout: +/- step only the active right video, by partial seeks
    // that leave the left and the other right where they are. Each step is
    // followed by a Shift+X dump of the presented frame numbers.
    auto step = [](const SDL_Keycode key, const SDL_Scancode scancode, const SDL_Keymod mod = KMOD_NONE) {
      push_keydown(key, scancode, 0, mod);
      sleep_ms(500);
    };
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    dump_display_filters();  // 0: right 1, level
    step(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    step(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    dump_display_filters();  // 1: right 1, +2
    step(SDLK_MINUS, SDL_SCANCODE_MINUS);
    dump_display_filters();  // 2: right 1, +1
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(300);
    step(SDLK_MINUS, SDL_SCANCODE_MINUS);
    step(SDLK_MINUS, SDL_SCANCODE_MINUS);
    dump_display_filters();  // 3: right 2, -2
    // A global seek must carry each right video's own offset along.
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(800);
    dump_display_filters();  // 4: right 2, -2 after the seek
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(400);
    dump_display_filters();  // 5: right 1, +1 after the seek
    step(SDLK_0, SDL_SCANCODE_0, KMOD_CTRL);
    dump_display_filters();  // 6: right 1, reset
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(300);
    step(SDLK_0, SDL_SCANCODE_0, KMOD_CTRL);
    dump_display_filters();  // 7: right 2, reset
  } else if (scenario == Scenario::RefStep) {
    // Paused throughout.  +/- and Ctrl+0 pressed while Q is held step the
    // reference and leave the right alone; released, they go back to the right.
    // Every dump is taken with Q up, so frame_left/right are left and right.
    auto key = [](const SDL_Keycode code, const SDL_Scancode scancode, const SDL_Keymod mod = KMOD_NONE) {
      push_keydown(code, scancode, 0, mod);
      sleep_ms(500);
    };
    auto hold_ref = []() {
      push_keydown(SDLK_q, SDL_SCANCODE_Q, 0);
      sleep_ms(200);
    };
    auto release_ref = []() {
      push_keyup(SDLK_q, SDL_SCANCODE_Q, 0);
      sleep_ms(300);
    };
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    dump_display_filters();  // 0: all level
    hold_ref();
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS, KMOD_CTRL);
    release_ref();
    dump_display_filters();  // 1: ref +11
    hold_ref();
    key(SDLK_MINUS, SDL_SCANCODE_MINUS);
    release_ref();
    dump_display_filters();  // 2: ref +10
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    dump_display_filters();  // 3: right +1, ref still +10
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(800);
    dump_display_filters();  // 4: both offsets kept through a global seek
    hold_ref();
    key(SDLK_0, SDL_SCANCODE_0, KMOD_CTRL);
    release_ref();
    dump_display_filters();  // 5: ref reset, right still +1
    key(SDLK_0, SDL_SCANCODE_0, KMOD_CTRL);
    dump_display_filters();  // 6: right reset
    // Shift+Q latches the reference on, Q released or not.  Its own release
    // must not drop the latch; a plain Q press and release does.
    auto toggle_ref = []() {
      push_keydown(SDLK_q, SDL_SCANCODE_Q, 0, KMOD_SHIFT);
      sleep_ms(100);
      push_keyup(SDLK_q, SDL_SCANCODE_Q, 0);
      sleep_ms(200);
    };
    toggle_ref();
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    toggle_ref();
    dump_display_filters();  // 7: ref +2 through the latch
    toggle_ref();
    hold_ref();
    release_ref();
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS);
    dump_display_filters();  // 8: Q released the latch, so + went to the right
  } else if (scenario == Scenario::RightStepPlay) {
    // The larger steps, the start-of-file clamp, and what becomes of the
    // offsets once playback resumes and a seek lands mid-play.
    auto key = [](const SDL_Keycode code, const SDL_Scancode scancode, const int wait_ms, const SDL_Keymod mod = KMOD_NONE) {
      push_keydown(code, scancode, 0, mod);
      sleep_ms(wait_ms);
    };
    key(SDLK_SPACE, SDL_SCANCODE_SPACE, 400);
    dump_display_filters();  // 0: right 1, level
    key(SDLK_MINUS, SDL_SCANCODE_MINUS, 500);
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS, 500);
    dump_display_filters();  // 1: right 1, - then + undo each other
    key(SDLK_PLUS, SDL_SCANCODE_EQUALS, 700, KMOD_CTRL);
    dump_display_filters();  // 2: right 1, +10
    key(SDLK_TAB, SDL_SCANCODE_TAB, 300);
    key(SDLK_MINUS, SDL_SCANCODE_MINUS, 900, KMOD_ALT);
    dump_display_filters();  // 3: right 2, -100 clamped at its first frame
    // Keep playback short: right 1 runs 10 frames ahead and must not reach the
    // end of the 100-frame fixture before the last dump.
    key(SDLK_TAB, SDL_SCANCODE_TAB, 300);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE, 500);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE, 400);
    dump_display_filters();  // 4: right 1 after playing, still +10
    key(SDLK_TAB, SDL_SCANCODE_TAB, 300);
    dump_display_filters();  // 5: right 2 after playing, still clamped offset
    key(SDLK_SPACE, SDL_SCANCODE_SPACE, 300);
    key(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 500);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE, 400);
    dump_display_filters();  // 6: right 2 after a seek during playback
    key(SDLK_TAB, SDL_SCANCODE_TAB, 300);
    dump_display_filters();  // 7: right 1 after a seek during playback
  } else if (scenario == Scenario::CropPreview) {
    // Shift+F selects the same way Shift+L/R/B do; window and clips are both
    // 320x180, so the selection should come out in the gesture's own pixels.
    // Enter/Shift+Enter would open the native folder picker, so only cancel.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    push_crop_gesture(SDLK_f, SDL_SCANCODE_F, 20, 20, 140, 100);
    sleep_ms(700);
    for (const SDL_Keycode code : {SDLK_4, SDLK_6, SDLK_r}) {
      push_keydown(code, SDL_GetScancodeFromKey(code), 0);
      sleep_ms(200);
    }
    push_keydown(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    sleep_ms(400);
    // dragged up and to the left
    push_crop_gesture(SDLK_f, SDL_SCANCODE_F, 300, 170, 200, 60);
    sleep_ms(700);
    push_keydown(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    sleep_ms(400);
  } else if (scenario == Scenario::MixedRateRights) {
    FrameProbe probe(captured_out, checkpoints, false);
    auto key = [](const SDL_Keycode code, const SDL_Scancode scancode, const SDL_Keymod mod = KMOD_NONE) {
      push_keydown(code, scancode, 0, mod);
    };
    probe.wait("mixed-rate startup", [](const FrameState& state) {
      return state.playing && native_frame_index(state.left_pts) >= 20;
    }, false);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE);
    probe.wait("mixed-rate pause", [](const FrameState& state) {
      return !state.playing && state.pending == 0 && state.right_id == 1 && state.left_pts == state.right_pts;
    });
    key(SDLK_TAB, SDL_SCANCODE_TAB);
    const auto baseline = probe.wait("30 fps right selected", [](const FrameState& state) {
      return !state.playing && state.pending == 0 && state.right_id == 2;
    });
    int left = native_frame_index(baseline.left_pts);
    int right = native_frame_index(baseline.right_pts, false, 30);
    for (int direction : {1, -1}) {
      for (int i = 0; i < 7; ++i) {
        key(direction > 0 ? SDLK_d : SDLK_a, direction > 0 ? SDL_SCANCODE_D : SDL_SCANCODE_A, KMOD_SHIFT);
        left += direction;
        right += direction;
        probe.at("mixed-rate native step", left, right, -1, 2, 30);
      }
    }
    key(SDLK_SPACE, SDL_SCANCODE_SPACE);
    auto synchronized = [&](const FrameState& state) {
      return state.pending == 0 && native_frame_index(state.left_pts) > left && std::llabs(state.left_pts - state.right_pts) <= 27000;
    };
    probe.wait("mixed-rate playback resynchronizes", [&](const FrameState& state) { return state.playing && synchronized(state); }, false);
    key(SDLK_SPACE, SDL_SCANCODE_SPACE);
    probe.wait("mixed-rate playback paused", [&](const FrameState& state) { return !state.playing && synchronized(state); });
    key(SDLK_SPACE, SDL_SCANCODE_SPACE);
    probe.wait("playing before Shift+D", [](const FrameState& state) { return state.playing; }, false);
    key(SDLK_d, SDL_SCANCODE_D, KMOD_SHIFT);
    const auto paused = probe.wait("Shift+D pauses playback", [](const FrameState& state) { return !state.playing && !state.looping && state.pending == 0; });
    probe.at("Shift+D remains paused", native_frame_index(paused.left_pts), native_frame_index(paused.right_pts, false, 30), -1, 2, 30);
    key(SDLK_PERIOD, SDL_SCANCODE_PERIOD);
    probe.wait("buffer loop started", [](const FrameState& state) { return state.looping; }, false);
    key(SDLK_a, SDL_SCANCODE_A, KMOD_SHIFT);
    const auto loop_paused = probe.wait("Shift+A exits buffer loop", [](const FrameState& state) { return !state.playing && !state.looping && state.pending == 0; });
    probe.at("Shift+A remains paused", native_frame_index(loop_paused.left_pts), native_frame_index(loop_paused.right_pts, false, 30), -1, 2, 30);
  } else if (scenario == Scenario::RightStepEdges) {
    // The directions right-step-play leaves out: Ctrl+- and Alt++, the latter
    // running into the end of the 100-frame fixture.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    dump_display_filters();  // 0: level
    push_keydown(SDLK_MINUS, SDL_SCANCODE_MINUS, 0, KMOD_CTRL);
    sleep_ms(900);
    dump_display_filters();  // 1: -10
    push_keydown(SDLK_PLUS, SDL_SCANCODE_EQUALS, 0, KMOD_ALT);
    sleep_ms(1500);
    dump_display_filters();  // 2: +100 from there, stopped at the last frame
    push_keydown(SDLK_0, SDL_SCANCODE_0, 0, KMOD_CTRL);
    sleep_ms(700);
    dump_display_filters();  // 3: level again
  }

}

void run_event_script(const Scenario scenario, std::atomic<bool>& finished, CapturedOutput& captured_out, std::vector<FrameState>& checkpoints, std::string& script_error) {
  const int watchdog_ticks = watchdog_ticks_for(scenario);
  std::thread watchdog([scenario, watchdog_ticks, &finished]() {
    for (int i = 0; i < watchdog_ticks && !finished.load(); ++i) {
      sleep_ms(100);
    }
    if (!finished.load()) {
      watchdog_fired.store(true);
      std::fprintf(stderr, "watchdog: pushing SDL_QUIT after %ds deadline (%s iteration=%s)\n", watchdog_ticks / 10, scenario_name(scenario),
                   stress_iteration_label());
      push_quit();
    }
  });

  try {
    run_event_script_body(scenario, captured_out, checkpoints);
  } catch (const std::exception& exception) {
    script_error = exception.what();
    std::fprintf(stderr, "FAIL event script: %s\n", script_error.c_str());
  }

  push_quit();
  finished.store(true);
  watchdog.join();
}

VideoCompareConfig make_config(const std::vector<std::string>& files) {
  VideoCompareConfig config;
  config.window_size = std::make_tuple(320, 180);
  config.left.file_name = files[0];

  for (size_t i = 1; i < files.size(); ++i) {
    InputVideo right;
    right.side = Side::Right(i - 1);
    right.side_description = "Right";
    right.file_name = files[i];
    config.right_videos.push_back(right);
  }

  return config;
}

int run_scenario(const Scenario scenario, const std::vector<std::string>& files) {
  std::vector<std::string> resolved = files;
  std::unique_ptr<DumpDirectory> dump_directory;
  if (scenario == Scenario::StillSeek) {
    for (std::string& file : resolved) {
      file = resolve_path(file);
    }
    dump_directory.reset(new DumpDirectory("vc-still-seek-"));
  }

  for (const std::string& file : resolved) {
    require_readable_file(file.c_str());
  }

  prepare_dummy_software_sdl();
  reset_helper_counters();

  const bool with_ref = scenario == Scenario::RefOverlay || scenario == Scenario::RefStep || scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr || scenario == Scenario::SharedFrameStepping;
  VideoCompareConfig config = make_config(with_ref ? std::vector<std::string>(resolved.begin(), resolved.end() - 1) : resolved);
  if (with_ref) {
    config.has_ref_video = true;
    config.ref_video.side = Side::Right(config.right_videos.size());
    config.ref_video.side_description = "Ref";
    config.ref_video.file_name = resolved.back();
  }
  if (scenario == Scenario::SyncMismatch || scenario == Scenario::MultiRightSync || scenario == Scenario::CropCopy || scenario == Scenario::InteractiveCrop || scenario == Scenario::CropPreview ||
      scenario == Scenario::MixedRateRights || scenario == Scenario::FrameSteppingVfr) {
    // Auto fps= would otherwise lift 25 fps to 30 and hide the sync path.
    // crop-copy also wants a clean Shift+X chain so crop= is the only extra filter.
    config.disable_auto_filters = true;
  }
  if (scenario == Scenario::FrameSteppingVfr) {
    // The default decoder deliberately regularizes timestamps between keyframes.
    // Preserve this fixture's irregular PTS to test real variable frame durations.
    av_dict_set(&config.left.decoder_options, "trust_dec_pts", "1", 0);
    for (auto& right : config.right_videos) {
      av_dict_set(&right.decoder_options, "trust_dec_pts", "1", 0);
    }
    av_dict_set(&config.ref_video.decoder_options, "trust_dec_pts", "1", 0);
  }
  if (scenario == Scenario::InteractiveCrop) {
    config.left.video_filters = "scale=160:90";
    config.right_videos[0].video_filters = "hflip";
  }
  if (scenario == Scenario::DynamicSwap) {
    // Keep pre-canvas sizes at 320x240 and 640x360. Dynamic letterboxes from the left DAR.
    config.disable_auto_filters = true;
    config.aspect_view_mode = Display::AspectViewMode::Dynamic;
  }
  if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong || scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr || scenario == Scenario::SharedFrameStepping) {
    // 3 slots: newest / middle / oldest. Smallest history that still has
    // an interior frame plus both endpoints for wrap and bounce.
    config.frame_buffer_size = 3;
  }

  const bool observe_frames = scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr || scenario == Scenario::SharedFrameStepping || scenario == Scenario::MixedRateRights;
  SDL_setenv("VIDEO_COMPARE_LOG_FRAME_STATE", observe_frames ? "1" : "0", 1);
  CapturedOutput captured_out;
  std::vector<FrameState> checkpoints;
  std::string script_error;
  std::streambuf* const old_out = std::cout.rdbuf(&captured_out);

  std::atomic<bool> finished{false};
  std::thread helper;
  int exit_code = EXIT_SUCCESS;

  try {
    VideoCompare video_compare(config);
    helper = std::thread(run_event_script, scenario, std::ref(finished), std::ref(captured_out), std::ref(checkpoints), std::ref(script_error));
    video_compare();
  } catch (const std::exception& exception) {
    finished.store(true);
    push_quit();
    std::cout.rdbuf(old_out);
    if (helper.joinable()) {
      helper.join();
    }
    std::fprintf(stderr, "FAIL VideoCompare threw: %s\n", exception.what());
    return EXIT_FAILURE;
  }

  std::cout.rdbuf(old_out);
  if (helper.joinable()) {
    helper.join();
  }

  const std::string output = captured_out.str();
  std::cout << output;
  if (!script_error.empty() || watchdog_fired.load()) {
    std::fprintf(stderr, "FAIL %s: %s\n", scenario_name(scenario), script_error.empty() ? "watchdog deadline reached" : script_error.c_str());
    return EXIT_FAILURE;
  }

  if (scenario == Scenario::EventInjection) {
    if (output.find("Active right video: 2/2") == std::string::npos) {
      std::fprintf(stderr, "FAIL injected Tab did not produce 'Active right video: 2/2'\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS injected Tab switched active right to 2/2\n");
    }
  } else if (scenario == Scenario::Seek) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 3) {
      std::fprintf(stderr, "FAIL seek expected 3 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      const double forward = times[1] - times[0];
      const double backward = times[1] - times[2];
      std::printf("seek timestamps: before=%.3f after+1s=%.3f after-1s=%.3f\n", times[0], times[1], times[2]);
      // Intra mpeg4 should land near ±1s. Allow decoder/PTS slack without
      // accepting a no-op (identical timestamps).
      if (forward < 0.40 || forward > 1.75) {
        std::fprintf(stderr, "FAIL forward seek delta %.3f not in [0.40, 1.75]\n", forward);
        exit_code = EXIT_FAILURE;
      } else if (backward < 0.40 || backward > 1.75) {
        std::fprintf(stderr, "FAIL backward seek delta %.3f not in [0.40, 1.75]\n", backward);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS keyboard seek moved presented PTS forward then backward\n");
      }
    }
  } else if (scenario == Scenario::StillSeek) {
    // A still seek often lands on the same PTS, so clipboard deltas are not
    // used as proof. F after the seek proves the post-seek stills were still
    // presentable (frames in the display buffer, refresh + PNG write succeeded).
    // It does not prove pixel-perfect equality or that PTS moved.
    const char* saved = "Saved screenshot_1_0001.png, screenshot_2_0001.png and screenshot_1_screenshot_2_osd_0001.png";
    const char* dumps[] = {"screenshot_1_0001.png", "screenshot_2_0001.png", "screenshot_1_screenshot_2_osd_0001.png"};
    if (output.find(saved) == std::string::npos) {
      std::fprintf(stderr, "FAIL still-seek did not save post-seek PNGs\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      bool all_present = true;
      for (const char* dump : dumps) {
        if (!file_is_non_empty(dump)) {
          std::fprintf(stderr, "FAIL still-seek dump missing or empty: %s\n", dump);
          all_present = false;
        }
      }
      if (all_present) {
        std::printf("PASS still-seek left presentable frames after skip_update (PNG dumps exist and are non-empty)\n");
      } else {
        exit_code = EXIT_FAILURE;
      }
    }
  } else if (scenario == Scenario::SyncMismatch) {
    // Clipboard is left PTS only. A mid-clip timestamp after unpaused 25/30
    // playback shows the loop kept presenting; it does not prove both sides'
    // PTS stayed inside min_delta. Filter strings go to av_log/stderr, not here.
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 1) {
      std::fprintf(stderr, "FAIL sync-mismatch expected a copied timestamp after playback\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times[0] < 0.40 || times[0] > 2.20) {
      std::fprintf(stderr, "FAIL sync-mismatch copied PTS %.3f not in [0.40, 2.20]\n", times[0]);
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS sync-mismatch presented left PTS %.3f after mismatched-rate playback\n", times[0]);
    }
  } else if (scenario == Scenario::RefOverlay) {
    const std::vector<double> times = copied_positions(output);
    if (output.find("Reference video (") == std::string::npos) {
      std::fprintf(stderr, "FAIL ref-overlay did not report the reference video\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (output.find("Active right video: 1/1") == std::string::npos) {
      std::fprintf(stderr, "FAIL ref-overlay Tab did not stay on 'Active right video: 1/1'\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times.size() < 2) {
      std::fprintf(stderr, "FAIL ref-overlay expected 2 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times[1] - times[0] < 0.20) {
      std::fprintf(stderr, "FAIL ref-overlay playback did not advance across Q release (%.3f -> %.3f)\n", times[0], times[1]);
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS ref-overlay kept the reference out of Tab and played through Q hold/release (%.3f -> %.3f)\n", times[0], times[1]);
    }
  } else if (scenario == Scenario::RefStep) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    struct Expected {
      const char* what;
      int right_offset;  // frame_right - frame_left
      int ref_offset;    // frame_ref - frame_left
    };
    const Expected expected[] = {{"level before stepping", 0, 0},        {"ref after Q held + and Ctrl++", 0, 11}, {"ref after Q held -", 0, 10}, {"right after + with Q up", 1, 10},
                                 {"both after a global seek", 1, 10}, {"ref after Q held Ctrl+0", 1, 0},       {"right after Ctrl+0", 0, 0},
                                 {"ref after ++ inside a Shift+Q latch", 0, 2}, {"right after Q ended the latch", 1, 2}};
    constexpr size_t kExpectedCount = sizeof(expected) / sizeof(expected[0]);

    auto fail_step = [&](const std::string& message) {
      std::fprintf(stderr, "FAIL ref-step: %s\n", message.c_str());
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: frame_left=%s frame_right=%s frame_ref=%s\n", i, dumps[i].frame_left.c_str(), dumps[i].frame_right.c_str(), dumps[i].frame_ref.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    if (dumps.size() < kExpectedCount) {
      fail_step("expected " + std::to_string(kExpectedCount) + " Shift+X dumps, got " + std::to_string(dumps.size()));
    } else {
      int left_before_seek = -1;
      for (size_t i = 0; i < kExpectedCount && exit_code == EXIT_SUCCESS; ++i) {
        int left = 0;
        int right = 0;
        int ref = 0;
        if (std::sscanf(dumps[i].frame_left.c_str(), "%d", &left) != 1 || std::sscanf(dumps[i].frame_right.c_str(), "%d", &right) != 1 || std::sscanf(dumps[i].frame_ref.c_str(), "%d", &ref) != 1) {
          fail_step(std::string("no frame numbers for ") + expected[i].what);
        } else if (right - left != expected[i].right_offset) {
          fail_step(std::string(expected[i].what) + ": expected right offset " + std::to_string(expected[i].right_offset) + ", got " + std::to_string(right - left));
        } else if (ref - left != expected[i].ref_offset) {
          fail_step(std::string(expected[i].what) + ": expected ref offset " + std::to_string(expected[i].ref_offset) + ", got " + std::to_string(ref - left));
        } else if (i < 4) {
          // stepping either video must never move the left
          if (left_before_seek >= 0 && left != left_before_seek) {
            fail_step(std::string("left moved while stepping (") + expected[i].what + ")");
          }
          left_before_seek = left;
        } else if (i == 4 && left <= left_before_seek) {
          fail_step("the global seek did not move the left forward");
        }
      }
      if (exit_code == EXIT_SUCCESS && output.find("Ref: +11 frames") == std::string::npos) {
        fail_step("no 'Ref: +11 frames' notice");
      }
      if (exit_code == EXIT_SUCCESS) {
        std::printf("PASS ref-step: Q held or a Shift+Q latch routes +/-, Ctrl++ and Ctrl+0 to the reference, the right keeps its own offset, both survive a seek\n");
      }
    }
  } else if (scenario == Scenario::MultiRightSync) {
    // Clipboard is left PTS only; Tab proves the second right is still live.
    // Same-iteration double left-pop is inferred from the frozen-pts sync loop
    // plus coverage, not asserted from this output.
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 1) {
      std::fprintf(stderr, "FAIL multi-right-sync expected a copied timestamp after playback\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times[0] < 0.40 || times[0] > 3.20) {
      std::fprintf(stderr, "FAIL multi-right-sync copied PTS %.3f not in [0.40, 3.20]\n", times[0]);
      exit_code = EXIT_FAILURE;
    } else if (output.find("Active right video: 2/2") == std::string::npos) {
      std::fprintf(stderr, "FAIL multi-right-sync Tab did not produce 'Active right video: 2/2'\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS multi-right-sync presented left PTS %.3f and switched active right to 2/2\n", times[0]);
    }
  } else if (scenario == Scenario::FrameNavigation) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 3) {
      std::fprintf(stderr, "FAIL frame-navigation expected 3 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double t1 = times[1];
      const double t2 = times[2];
      const double forward = t1 - t0;
      const double backward = t2 - t1;
      const double roundtrip = t2 - t0;
      std::printf("frame-navigation timestamps: T0=%.3f T1=%.3f T2=%.3f forward=%.3f backward=%.3f roundtrip=%.3f\n", t0, t1, t2, forward, backward, roundtrip);
      // 25 fps intra clips should move exactly one frame (~0.040 s).
      // Millisecond clipboard formatting can land on 0.039/0.041.
      if (!(t1 > t0)) {
        std::fprintf(stderr, "FAIL T1 (%.3f) is not greater than T0 (%.3f); forward=%.3f backward=%.3f\n", t1, t0, forward, backward);
        exit_code = EXIT_FAILURE;
      } else if (forward < 0.030 || forward > 0.050) {
        std::fprintf(stderr, "FAIL forward delta %.3f not in [0.030, 0.050]; T0=%.3f T1=%.3f T2=%.3f backward=%.3f\n", forward, t0, t1, t2, backward);
        exit_code = EXIT_FAILURE;
      } else if (!(t2 < t1)) {
        std::fprintf(stderr, "FAIL T2 (%.3f) is not less than T1 (%.3f); T0=%.3f forward=%.3f backward=%.3f\n", t2, t1, t0, forward, backward);
        exit_code = EXIT_FAILURE;
      } else if (backward < -0.050 || backward > -0.030) {
        std::fprintf(stderr, "FAIL backward delta %.3f not in [-0.050, -0.030]; T0=%.3f T1=%.3f T2=%.3f forward=%.3f\n", backward, t0, t1, t2, forward);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(roundtrip) > 0.015) {
        std::fprintf(stderr, "FAIL |T2-T0| %.3f exceeds 0.015; T0=%.3f T1=%.3f T2=%.3f forward=%.3f backward=%.3f\n", std::fabs(roundtrip), t0, t1, t2, forward, backward);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS Shift+D / Shift+A presented one frame forward then one frame backward\n");
      }
    }
  } else if (scenario == Scenario::SharedFrameStepping) {
    const auto& dumps = checkpoints;
    std::string failure;
    int initial = -1;
    if (dumps.size() != 5) {
      failure = "expected five frame dumps";
    }
    for (size_t i = 0; i < dumps.size() && failure.empty(); ++i) {
      int left = -1, right = -1, ref = -1;
      left = native_frame_index(dumps[i].left_pts);
      right = native_frame_index(dumps[i].right_pts);
      ref = native_frame_index(dumps[i].ref_pts);
      if (i == 0) {
        initial = left;
      }
      if (left != initial + (i >= 3 ? 20 : 0) || right != left - (i == 1 ? 10 : 0) || ref != left) {
        failure = "a dormant pipeline lost its position at dump " + std::to_string(i);
      }
    }
    if (!failure.empty()) {
      std::fprintf(stderr, "FAIL shared-frame-stepping: %s\ncaptured stdout:\n%s\n", failure.c_str(), output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS shared-frame-stepping: global steps remain adjacent after independent stepping splits the shared decoder\n");
    }
  } else if (scenario == Scenario::FrameStepping || scenario == Scenario::FrameSteppingVfr) {
    const bool variable = scenario == Scenario::FrameSteppingVfr;
    std::string failure;
    if (checkpoints.size() != 28) {
      failure = "expected 28 completed frame checkpoints";
    } else {
      const int initial = native_frame_index(checkpoints[0].left_pts, variable);
      const int last = variable ? 49 : 99;
      for (size_t i = 0; i < checkpoints.size() && failure.empty(); ++i) {
        const auto& state = checkpoints[i];
        const int left = native_frame_index(state.left_pts, variable);
        const int right = native_frame_index(state.right_pts, variable);
        const int ref = native_frame_index(state.ref_pts, variable);
        const int expected = i <= 6 ? initial - static_cast<int>(i)
            : i <= 12 ? initial - 6 + static_cast<int>(i - 6)
            : i == 13 ? initial - 1 : i <= 18 ? initial : i >= 24 ? last - (i == 26 ? 1 : 0) : i == 21 ? 1 : 0;
        if (state.playing || state.looping || state.pending != 0 || right != left - (i == 15 ? 10 : 0) || ref != left - (i == 17 ? 10 : 0)) {
          failure = "independent offsets or unfinished navigation at checkpoint " + std::to_string(i);
        } else if ((i != 23 && left != expected) || (i == 23 && left <= 1)) {
          failure = string_sprintf("checkpoint %zu: expected native frame %d, got %d", i, expected, left);
        }
      }
    }
    if (!failure.empty()) {
      std::fprintf(stderr, "FAIL %s: %s\ncaptured stdout:\n%s\n", scenario_name(scenario), failure.c_str(), output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS %s: 28 completed checkpoints, exact adjacent frames, offsets, first/last clamps, replay and playback resumption\n", scenario_name(scenario));
    }
  } else if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    const std::vector<double> times = copied_positions(output);
    const char* name = (scenario == Scenario::BufferForwardOnly) ? "ForwardOnly" : "PingPong";
    print_pts_stdout((std::string(name) + " observed: ").c_str(), times);
    if (times.size() < 8) {
      std::fprintf(stderr, "FAIL %s expected at least 8 copied timestamps, got %zu\n", name, times.size());
      print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
      exit_code = EXIT_FAILURE;
    } else {
      const double newest = times[0];
      const double middle = newest - 0.040;
      const double oldest = newest - 0.080;
      const std::vector<double> unique = unique_pts(std::vector<double>(times.begin() + 1, times.end()));
      print_pts_stdout((std::string(name) + " unique after start: ").c_str(), unique);

      bool lattice_ok = true;
      for (const double t : times) {
        if (!pts_near(t, newest) && !pts_near(t, middle) && !pts_near(t, oldest)) {
          lattice_ok = false;
        }
      }

      size_t start = 0;
      if (!unique.empty() && pts_near(unique[0], newest)) {
        start = 1;
      }

      if (!lattice_ok) {
        std::fprintf(stderr, "FAIL %s PTS left the 3-frame history lattice; newest=%.3f\n", name, newest);
        print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
        exit_code = EXIT_FAILURE;
      } else if (unique.size() <= start + 2) {
        std::fprintf(stderr, "FAIL %s did not traverse history; newest=%.3f unique=%zu\n", name, newest, unique.size());
        print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
        exit_code = EXIT_FAILURE;
      } else if (scenario == Scenario::BufferForwardOnly) {
        // Offset 0 wraps to last (oldest), then decrements toward newest.
        const double cycle[3] = {oldest, middle, newest};
        bool followed = true;
        for (size_t i = start, step = 0; i < unique.size(); ++i, ++step) {
          if (!pts_near(unique[i], cycle[step % 3])) {
            followed = false;
            break;
          }
        }
        if (!followed) {
          std::fprintf(stderr, "FAIL ForwardOnly unique sequence did not wrap oldest→middle→newest; newest=%.3f\n", newest);
          print_pts_sequence("ForwardOnly observed: ", times);
          exit_code = EXIT_FAILURE;
        } else {
          std::printf("PASS ForwardOnly wrapped from newest to oldest and replayed history\n");
        }
      } else {
        // From offset 0 / forward: flip, step to middle, continue to oldest,
        // bounce back through middle to newest. No stay-in-place endpoint.
        const double cycle[4] = {middle, oldest, middle, newest};
        bool followed = true;
        bool bounced = false;
        for (size_t i = start, step = 0; i < unique.size(); ++i, ++step) {
          if (!pts_near(unique[i], cycle[step % 4])) {
            followed = false;
            break;
          }
          if (step >= 2) {
            bounced = true;
          }
        }
        if (!followed || !bounced) {
          std::fprintf(stderr, "FAIL PingPong unique sequence did not bounce middle→oldest→middle→newest; newest=%.3f\n", newest);
          print_pts_sequence("PingPong observed: ", times);
          exit_code = EXIT_FAILURE;
        } else {
          std::printf("PASS PingPong reached the oldest frame and reversed through history\n");
        }
      }
    }
  } else if (is_stress_scenario(scenario)) {
    const std::vector<double> times = copied_positions(output);
    const double expected_storm = (scenario == Scenario::SeekBurstForward) ? 10.0 : 9.0;
    if (watchdog_fired.load()) {
      print_stress_failure(scenario, "in-process watchdog fired before helper finished", times, expected_storm);
      exit_code = EXIT_FAILURE;
    } else if (times.size() < 3) {
      print_stress_failure(scenario, "expected T0 T1 T2 copied timestamps", times, expected_storm);
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double t1 = times[1];
      const double t2 = times[2];
      const double storm = t1 - t0;
      const double step = t2 - t1;
      std::printf("%s timestamps: T0=%.3f T1=%.3f T2=%.3f storm=%.3f step=%.3f events=%d\n", scenario_name(scenario), t0, t1, t2, storm, step, events_pushed.load());
      if (t0 < 0.0 || t0 > 8.0) {
        print_stress_failure(scenario, "T0 is not a plausible paused start PTS", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (t1 < 0.0 || t1 > 19.50) {
        print_stress_failure(scenario, "T1 is outside the 20s fixture", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (!pts_near(storm, expected_storm)) {
        print_stress_failure(scenario, "storm delta does not match the sum of all seek commands", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (!(t2 > t1)) {
        print_stress_failure(scenario, "post-storm Shift+D did not advance PTS", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (step < 0.030 || step > 0.050) {
        print_stress_failure(scenario, "post-storm Shift+D was not one 25 fps frame", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS %s accounted for all seek commands and Shift+D advanced one frame\n", scenario_name(scenario));
      }
    }
  } else if (scenario == Scenario::CropCopy) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    print_filter_dumps(dumps);

    auto fail_crop = [&](const char* message) {
      std::fprintf(stderr, "FAIL crop-copy: %s\n", message);
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    // dump  0 left crop A (R0 selected, uncropped)
    // dump  1 after Swap only (same selection, logical filters unchanged)
    // dump  2 R1 crop B
    // dump  3 R0 crop C
    // dump  4 after Ctrl+Shift+L, R0
    // dump  5 after Ctrl+Shift+L, R1
    // dump  6 after Ctrl+Shift+L, R2
    // dump  7 after undo, R0
    // dump  8 after undo, R1
    // dump  9 after undo, R2
    // dump 10 swapped Ctrl+Shift+L, R1 (source); only visual right / logical left updated
    // dump 11 swapped Ctrl+Shift+L, R0 unchanged
    // dump 12 swapped Ctrl+Shift+L, R2 unchanged
    // dump 13 swapped undo, R0
    // dump 14 swapped undo, R1
    // dump 15 swapped undo, R2
    // dump 16 Ctrl+Shift+R normal, R1 (source)
    // dump 17 Ctrl+Shift+R normal, R0 (untouched)
    // dump 18 Ctrl+Shift+R undo, R1
    // dump 19 Ctrl+Shift+R swapped, R0 (dest)
    // dump 20 Ctrl+Shift+R swapped undo, R0
    // dump 21 R1 recrop D
    // dump 22 R2 after R1 recrop
    // dump 23 after undo while R2 selected
    // dump 24 R1 after that undo
    // dump 25 Ctrl+L, R1 (left cleared)
    // dump 26 Ctrl+R, R1 (R1 cleared)
    // dump 27 Ctrl+R, R0 unchanged
    // dump 28 undo Ctrl+R, R1 restored
    // dump 29 swapped Ctrl+L, R1 cleared; left unchanged
    // dump 30 swapped Ctrl+L, R0 unchanged
    // dump 31 swapped Ctrl+R, left cleared; R1 unchanged
    // dump 32 undo swapped Ctrl+R, left restored
    constexpr size_t kExpectedDumps = 33;
    if (dumps.size() < kExpectedDumps) {
      fail_crop("expected 33 Shift+X filter dumps");
    } else if (output.find("Copied left crop to all right videos") == std::string::npos) {
      fail_crop("normal/swapped Ctrl+Shift+L did not notify");
    } else if (output.find("Copied right crop to left") == std::string::npos) {
      fail_crop("Ctrl+Shift+R did not notify");
    } else if (output.find("Cleared left crop") == std::string::npos || output.find("Cleared right crop") == std::string::npos) {
      fail_crop("Ctrl+L/R did not notify");
    } else if (output.find("Active right video: 2/3") == std::string::npos || output.find("Active right video: 3/3") == std::string::npos) {
      fail_crop("did not select Right 1 and Right 2");
    } else {
      const std::string crop_a = crop_token(dumps[0].left);
      const std::string crop_b = crop_token(dumps[2].right);
      const std::string crop_c = crop_token(dumps[3].right);
      const std::string crop_d = crop_token(dumps[21].right);
      const std::string crop_a_640 = mapped_crop_token(crop_a, 320, 180, 640, 360);
      const std::string crop_a_160 = mapped_crop_token(crop_a, 320, 180, 160, 90);
      const std::string crop_b_320 = mapped_crop_token(crop_b, 320, 180, 320, 180);
      const std::string crop_b_640 = mapped_crop_token(crop_b, 320, 180, 640, 360);
      const std::string crop_b_160 = mapped_crop_token(crop_b, 320, 180, 160, 90);

      if (crop_a.empty() || crop_b.empty() || crop_c.empty() || crop_d.empty()) {
        fail_crop("interactive crops did not appear in the effective filter chain");
      } else if (crop_a == crop_b || crop_a == crop_c || crop_b == crop_c || crop_d == crop_b) {
        fail_crop("interactive crops were not distinct");
      } else if (dumps[0].swapped != "false" || dumps[0].right_id != "1") {
        fail_crop("initial Shift+X did not report swapped=false right=1");
      } else if (dumps[1].swapped != "true" || dumps[1].right_id != "1") {
        fail_crop("Swap did not report swapped=true with the same right=1");
      } else if (dumps[0].visual_left.empty() || dumps[0].visual_right.empty() || dumps[0].visual_left == dumps[0].visual_right) {
        fail_crop("pre-Swap visual sizes were missing or identical");
      } else if (dumps[1].visual_left != dumps[0].visual_right || dumps[1].visual_right != dumps[0].visual_left) {
        fail_crop("Swap did not exchange visual_left/visual_right");
      } else if (dumps[1].left != dumps[0].left || dumps[1].right != dumps[0].right) {
        fail_crop("Swap exchanged or changed logical filters_left/right");
      } else if (dumps[2].swapped != "false" || dumps[2].right_id != "2") {
        fail_crop("selecting Right 1 did not report right=2");
      } else if (crop_token(dumps[0].right) != "" || crop_token(dumps[2].left) != crop_a) {
        fail_crop("left crop leaked onto R0, or left changed while cropping R1");
      } else if (crop_token(dumps[4].left) != crop_a || crop_token(dumps[4].right) != crop_a_640) {
        fail_crop("normal Ctrl+Shift+L did not map left crop onto 640x360 R0, or changed left");
      } else if (crop_token(dumps[5].left) != crop_a || crop_token(dumps[5].right) != crop_a) {
        fail_crop("normal Ctrl+Shift+L did not copy left crop onto same-size R1");
      } else if (crop_token(dumps[6].left) != crop_a || crop_token(dumps[6].right) != crop_a_160) {
        fail_crop("normal Ctrl+Shift+L did not map left crop onto 160x90 R2");
      } else if (crop_token(dumps[7].left) != crop_a || crop_token(dumps[7].right) != crop_c) {
        fail_crop("undo Ctrl+Shift+L did not restore R0's previous crop, or changed left");
      } else if (crop_token(dumps[8].left) != crop_a || crop_token(dumps[8].right) != crop_b) {
        fail_crop("undo Ctrl+Shift+L did not restore R1's previous crop");
      } else if (crop_token(dumps[9].left) != crop_a || crop_token(dumps[9].right) != "") {
        fail_crop("undo Ctrl+Shift+L did not restore R2 to uncropped");
      } else if (crop_token(dumps[10].left) != crop_b_320 || crop_token(dumps[10].right) != crop_b) {
        fail_crop("swapped Ctrl+Shift+L did not copy R1 crop onto visual right (logical left), or changed the R1 source");
      } else if (crop_token(dumps[11].left) != crop_b_320 || crop_token(dumps[11].right) != crop_c) {
        fail_crop("swapped Ctrl+Shift+L changed R0, or lost the visual-right copy");
      } else if (crop_token(dumps[12].left) != crop_b_320 || crop_token(dumps[12].right) != "") {
        fail_crop("swapped Ctrl+Shift+L changed R2, or lost the visual-right copy");
      } else if (crop_token(dumps[13].left) != crop_a || crop_token(dumps[13].right) != crop_c) {
        fail_crop("undo swapped Ctrl+Shift+L did not restore left, or changed R0");
      } else if (crop_token(dumps[14].left) != crop_a || crop_token(dumps[14].right) != crop_b) {
        fail_crop("undo swapped Ctrl+Shift+L changed the R1 source");
      } else if (crop_token(dumps[15].left) != crop_a || crop_token(dumps[15].right) != "") {
        fail_crop("undo swapped Ctrl+Shift+L changed R2");
      } else if (crop_token(dumps[16].left) != crop_b_320 || crop_token(dumps[16].right) != crop_b) {
        fail_crop("Ctrl+Shift+R normal did not copy R1 crop onto logical left");
      } else if (crop_token(dumps[17].left) != crop_b_320 || crop_token(dumps[17].right) != crop_c) {
        fail_crop("Ctrl+Shift+R normal changed R0, or left was not the only destination");
      } else if (crop_token(dumps[18].left) != crop_a || crop_token(dumps[18].right) != crop_b) {
        fail_crop("undo Ctrl+Shift+R normal did not restore only logical left");
      } else if (crop_token(dumps[19].left) != crop_a || crop_token(dumps[19].right) != crop_a_640) {
        fail_crop("Ctrl+Shift+R swapped did not map logical left onto 640x360 R0");
      } else if (crop_token(dumps[20].left) != crop_a || crop_token(dumps[20].right) != crop_c) {
        fail_crop("undo Ctrl+Shift+R swapped did not restore only R0");
      } else if (crop_token(dumps[21].left) != crop_a || crop_token(dumps[21].right) != crop_d) {
        fail_crop("R1 recrop did not change only R1");
      } else if (crop_token(dumps[22].left) != crop_a || crop_token(dumps[22].right) != "") {
        fail_crop("R1 recrop changed R2");
      } else if (crop_token(dumps[23].left) != crop_a || crop_token(dumps[23].right) != "") {
        fail_crop("undo after selecting R2 changed R2");
      } else if (crop_token(dumps[24].left) != crop_a || crop_token(dumps[24].right) != crop_b) {
        fail_crop("undo after selecting R2 did not restore R1");
      } else if (crop_token(dumps[25].left) != "" || crop_token(dumps[25].right) != crop_b) {
        fail_crop("Ctrl+L did not clear only logical left");
      } else if (crop_token(dumps[26].left) != crop_a || crop_token(dumps[26].right) != "") {
        fail_crop("Ctrl+R did not clear only R1, or undo did not restore left");
      } else if (crop_token(dumps[27].left) != crop_a || crop_token(dumps[27].right) != crop_c) {
        fail_crop("Ctrl+R changed R0");
      } else if (crop_token(dumps[28].left) != crop_a || crop_token(dumps[28].right) != crop_b) {
        fail_crop("undo Ctrl+R did not restore R1");
      } else if (crop_token(dumps[29].left) != crop_a || crop_token(dumps[29].right) != "") {
        fail_crop("swapped Ctrl+L did not clear only R1");
      } else if (crop_token(dumps[30].left) != crop_a || crop_token(dumps[30].right) != crop_c) {
        fail_crop("swapped Ctrl+L changed R0");
      } else if (crop_token(dumps[31].left) != "" || crop_token(dumps[31].right) != crop_b) {
        fail_crop("swapped Ctrl+R did not clear only logical left, or undo did not restore R1");
      } else if (crop_token(dumps[32].left) != crop_a || crop_token(dumps[32].right) != crop_b) {
        fail_crop("undo swapped Ctrl+R did not restore left");
      } else {
        std::printf("PASS crop-copy wired normalized Ctrl+Shift+L/R and Ctrl+L/R (A=%s A640=%s A160=%s B=%s C=%s D=%s)\n", crop_a.c_str(), crop_a_640.c_str(), crop_a_160.c_str(), crop_b.c_str(), crop_c.c_str(), crop_d.c_str());
      }
    }
  } else if (scenario == Scenario::InteractiveCrop) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    print_filter_dumps(dumps);

    auto fail_crop = [&](const char* message) {
      std::fprintf(stderr, "FAIL interactive-crop: %s\n", message);
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    // dump 0 initial
    // dump 1 after refused Shift+B
    // dump 2 after Shift+L
    // dump 3 after Backspace
    // dump 4 after refused Shift+R
    // dump 5 after Swap + refused Shift+L
    constexpr size_t kExpectedDumps = 6;
    if (dumps.size() < kExpectedDumps) {
      fail_crop("expected 6 Shift+X filter dumps");
    } else if (output.find("Cannot apply crop: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("Shift+B did not notify that post-filters change spatial coordinates");
    } else if (output.find("Cannot crop right: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("Shift+R did not notify using the visual right side");
    } else if (output.find("Cannot crop left: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("swapped Shift+L did not notify using the visual left side");
    } else {
      const std::string left0 = crop_token(dumps[0].left);
      const std::string right0 = crop_token(dumps[0].right);
      const std::string left_b = crop_token(dumps[1].left);
      const std::string right_b = crop_token(dumps[1].right);
      const std::string left_l = crop_token(dumps[2].left);
      const std::string right_l = crop_token(dumps[2].right);
      const std::string left_undo = crop_token(dumps[3].left);
      const std::string right_undo = crop_token(dumps[3].right);
      const std::string left_r = crop_token(dumps[4].left);
      const std::string right_r = crop_token(dumps[4].right);
      const std::string left_swap = crop_token(dumps[5].left);
      const std::string right_swap = crop_token(dumps[5].right);

      if (left0 != "" || right0 != "") {
        fail_crop("initial filters already contained an application crop");
      } else if (left_b != "" || right_b != "") {
        fail_crop("Shift+B changed a crop state when one side is unsupported");
      } else if (dumps[1].left != dumps[0].left || dumps[1].right != dumps[0].right) {
        fail_crop("Shift+B changed filter chains");
      } else if (left_l != "crop=120:80:20:20") {
        fail_crop("Shift+L did not emit crop=120:80:20:20 in 320x180 crop-space");
      } else if (left_l == "crop=60:40:10:10") {
        fail_crop("Shift+L used dest-space 160x90 instead of crop-space");
      } else if (dumps[2].left.find("scale=160:90") == std::string::npos) {
        fail_crop("Shift+L lost the post-scale filter");
      } else if (right_l != "") {
        fail_crop("Shift+L changed the unsupported right crop");
      } else if (left_undo != "" || right_undo != "") {
        fail_crop("Backspace did not restore the pre-Shift+L crop state");
      } else if (left_r != "" || right_r != "") {
        fail_crop("Shift+R changed crop state on an unsupported right");
      } else if (left_swap != "" || right_swap != "") {
        fail_crop("swapped Shift+L changed crop state");
      } else {
        std::printf("PASS interactive-crop post-scale Shift+L and atomic Shift+B\n");
      }
    }
  } else if (scenario == Scenario::DynamicSwap) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    auto fail_dynamic = [&](const char* message) {
      std::fprintf(stderr, "FAIL dynamic-swap: %s\n", message);
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: aspect=%s swapped=%s window=%s content=%s ref_dar=%s visual_left=%s visual_right=%s\n", i, dumps[i].aspect.c_str(), dumps[i].swapped.c_str(), dumps[i].window.c_str(),
                     dumps[i].content.c_str(), dumps[i].ref_dar.c_str(), dumps[i].visual_left.c_str(), dumps[i].visual_right.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    if (dumps.size() < 2) {
      fail_dynamic("expected Shift+X dumps before and after Swap");
    } else {
      const FilterDump& before = dumps[0];
      const FilterDump& after = dumps[1];
      WindowSize window_before{};
      WindowSize window_after{};
      ContentViewport content_before{};
      ContentViewport content_after{};
      int dar_before_num = 0;
      int dar_before_den = 0;
      int dar_after_num = 0;
      int dar_after_den = 0;
      const bool window_ok = parse_window_size(before.window, &window_before) && parse_window_size(after.window, &window_after);
      const bool content_ok = parse_content_viewport(before.content, &content_before) && parse_content_viewport(after.content, &content_after);
      const bool dar_ok = parse_dar(before.ref_dar, &dar_before_num, &dar_before_den) && parse_dar(after.ref_dar, &dar_after_num, &dar_after_den);
      const ContentViewport pillar_43 = window_ok ? viewport_for_aspect(window_before, 4.0F / 3.0F) : ContentViewport{};

      if (before.aspect != "dynamic" || after.aspect != "dynamic") {
        fail_dynamic("Shift+X did not report aspect=dynamic");
      } else if (before.swapped != "false" || after.swapped != "true") {
        fail_dynamic("Swap did not report swapped=false then swapped=true");
      } else if (before.visual_left != "320x240" || before.visual_right != "640x360") {
        fail_dynamic("pre-Swap visual sizes were not 4:3 left (320x240) and 16:9 right (640x360)");
      } else if (after.visual_left != before.visual_right || after.visual_right != before.visual_left) {
        fail_dynamic("Swap did not exchange visual_left/visual_right");
      } else if (!dar_ok) {
        fail_dynamic("Shift+X did not report ref_dar");
      } else if (!dar_equals(dar_before_num, dar_before_den, 4, 3)) {
        fail_dynamic("Dynamic reference was not the original left 4:3 input");
      } else if (dar_equals(dar_after_num, dar_after_den, 16, 9)) {
        fail_dynamic("Dynamic followed the visually-left 16:9 input after Swap");
      } else if (!dar_equals(dar_after_num, dar_after_den, 4, 3)) {
        fail_dynamic("Swap changed the Dynamic reference away from the original left 4:3 input");
      } else if (!window_ok || window_before.w != window_after.w || window_before.h != window_after.h) {
        fail_dynamic("Swap changed the OS window size");
      } else if (!(window_before.w * 3 > window_before.h * 4)) {
        fail_dynamic("window is not wider than 4:3, so letterboxing would not show a 16:9 policy change");
      } else if (!content_ok || !same_viewport(content_before, content_after)) {
        fail_dynamic("Swap changed the letterbox viewport");
      } else if (!same_viewport(content_before, pillar_43)) {
        fail_dynamic("letterbox viewport is not the 4:3 pillarbox of the original left input");
      } else {
        std::printf("PASS dynamic-swap kept ref_dar=%s and content=%s after Swap (window=%s)\n", before.ref_dar.c_str(), before.content.c_str(), before.window.c_str());
      }
    }
  } else if (scenario == Scenario::RightStep) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    struct Expected {
      const char* what;
      const char* right_id;
      int offset;  // frame_right - frame_left
    };
    const Expected expected[] = {{"level before stepping", "1", 0},     {"right 1 after ++", "1", 2},      {"right 1 after ++-", "1", 1},  {"right 2 after --", "2", -2},
                                 {"right 2 after a global seek", "2", -2}, {"right 1 after a global seek", "1", 1}, {"right 1 after Ctrl+0", "1", 0}, {"right 2 after Ctrl+0", "2", 0}};
    constexpr size_t kExpectedCount = sizeof(expected) / sizeof(expected[0]);

    auto fail_step = [&](const std::string& message) {
      std::fprintf(stderr, "FAIL right-step: %s\n", message.c_str());
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: right=%s frame_left=%s frame_right=%s\n", i, dumps[i].right_id.c_str(), dumps[i].frame_left.c_str(), dumps[i].frame_right.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    if (dumps.size() < kExpectedCount) {
      fail_step("expected " + std::to_string(kExpectedCount) + " Shift+X dumps, got " + std::to_string(dumps.size()));
    } else {
      int left_before_seek = -1;
      for (size_t i = 0; i < kExpectedCount && exit_code == EXIT_SUCCESS; ++i) {
        const FilterDump& dump = dumps[i];
        int left = 0;
        int right = 0;
        if (std::sscanf(dump.frame_left.c_str(), "%d", &left) != 1 || std::sscanf(dump.frame_right.c_str(), "%d", &right) != 1) {
          fail_step(std::string("no frame numbers for ") + expected[i].what);
        } else if (dump.right_id != expected[i].right_id) {
          fail_step(std::string("wrong active right for ") + expected[i].what);
        } else if (right - left != expected[i].offset) {
          fail_step(std::string(expected[i].what) + ": expected offset " + std::to_string(expected[i].offset) + ", got " + std::to_string(right - left));
        } else if (i < 4) {
          // stepping a right video must never move the left
          if (left_before_seek >= 0 && left != left_before_seek) {
            fail_step(std::string("left moved while stepping (") + expected[i].what + ")");
          }
          left_before_seek = left;
        } else if (i == 4 && left <= left_before_seek) {
          fail_step("global seek did not move the left forward");
        }
      }
      if (exit_code == EXIT_SUCCESS) {
        std::printf("PASS right-step stepped each right video on its own, kept the offsets across a seek, and reset them\n");
      }
    }
  } else if (scenario == Scenario::RightStepPlay) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    auto fail_play = [&](const std::string& message) {
      std::fprintf(stderr, "FAIL right-step-play: %s\n", message.c_str());
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: right=%s frame_left=%s frame_right=%s\n", i, dumps[i].right_id.c_str(), dumps[i].frame_left.c_str(), dumps[i].frame_right.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    constexpr size_t kDumps = 8;
    const char* const right_ids[kDumps] = {"1", "1", "1", "2", "1", "2", "2", "1"};
    int left[kDumps] = {};
    int right[kDumps] = {};
    if (dumps.size() < kDumps) {
      fail_play("expected " + std::to_string(kDumps) + " Shift+X dumps, got " + std::to_string(dumps.size()));
    }
    for (size_t i = 0; i < kDumps && exit_code == EXIT_SUCCESS; ++i) {
      if (std::sscanf(dumps[i].frame_left.c_str(), "%d", &left[i]) != 1 || std::sscanf(dumps[i].frame_right.c_str(), "%d", &right[i]) != 1) {
        fail_play("no frame numbers in dump " + std::to_string(i));
      } else if (dumps[i].right_id != right_ids[i]) {
        fail_play("wrong active right in dump " + std::to_string(i));
      }
    }
    auto offset = [&](const size_t i) { return right[i] - left[i]; };
    if (exit_code == EXIT_SUCCESS) {
      const int clamped = offset(3);
      if (offset(0) != 0) {
        fail_play("sides were not level before stepping");
      } else if (offset(1) != 0 || left[1] != left[0]) {
        fail_play("- then + did not put right 1 back where it was");
      } else if (offset(2) != 10 || left[2] != left[0]) {
        fail_play("Ctrl++ did not step right 1 by 10 frames alone");
      } else if (right[3] != 0 || left[3] != left[0]) {
        fail_play("Alt+- past the start did not stop right 2 at its first frame");
      } else if (output.find("Right #2: ") == std::string::npos || output.find("reached start") == std::string::npos) {
        fail_play("no reached-start notice for right 2");
      } else if (!(left[4] > left[3])) {
        fail_play("playback did not move the left forward");
      } else if (offset(4) != 10) {
        fail_play("right 1 lost its +10 once playback resumed: " + std::to_string(offset(4)));
      } else if (offset(5) != clamped) {
        fail_play("right 2 lost its offset once playback resumed: " + std::to_string(offset(5)) + " vs " + std::to_string(clamped));
      } else if (!(left[6] > left[5])) {
        fail_play("seek during playback did not move the left forward");
      } else if (offset(6) != clamped) {
        fail_play("right 2 lost its offset across a seek during playback: " + std::to_string(offset(6)) + " vs " + std::to_string(clamped));
      } else if (offset(7) != 10) {
        fail_play("right 1 lost its +10 across a seek during playback: " + std::to_string(offset(7)));
      } else {
        std::printf("PASS right-step-play: -/+ undo, Ctrl +10, Alt -100 clamped at %d, offsets kept through playback and a seek mid-play\n", clamped);
      }
    }
  } else if (scenario == Scenario::CropPreview) {
    std::vector<std::string> selections;
    const std::string prefix = "Crop preview: ";
    size_t pos = 0;
    while ((pos = output.find(prefix, pos)) != std::string::npos) {
      pos += prefix.size();
      const size_t end = output.find_first_of("\r\n", pos);
      selections.push_back(output.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    }
    size_t cancels = 0;
    for (size_t at = output.find("Crop preview cancelled"); at != std::string::npos; at = output.find("Crop preview cancelled", at + 1)) {
      ++cancels;
    }
    // "Crop preview cancelled" also starts with the prefix; keep the selections only.
    selections.erase(std::remove(selections.begin(), selections.end(), std::string("cancelled")), selections.end());

    if (selections.size() != 2 || cancels != 2) {
      std::fprintf(stderr, "FAIL crop-preview: expected 2 previews and 2 cancels, got %zu and %zu\ncaptured stdout:\n%s\n", selections.size(), cancels, output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (selections[0] != "120x80 at 20,20") {
      std::fprintf(stderr, "FAIL crop-preview: drag 20,20 -> 140,100 selected %s\n", selections[0].c_str());
      exit_code = EXIT_FAILURE;
    } else if (selections[1] != "100x110 at 200,60") {
      std::fprintf(stderr, "FAIL crop-preview: drag 300,170 -> 200,60 selected %s\n", selections[1].c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS crop-preview selected exactly what was dragged, either way round, and 4/6/R/Esc kept it alive\n");
    }
  } else if (scenario == Scenario::MixedRateRights) {
    const auto& dumps = checkpoints;
    std::string failure;
    std::vector<int> left(dumps.size()), right(dumps.size());
    if (dumps.size() != 21) {
      failure = "expected 21 frame dumps";
    }
    for (size_t i = 0; i < dumps.size() && failure.empty(); ++i) {
      left[i] = native_frame_index(dumps[i].left_pts);
      right[i] = native_frame_index(dumps[i].right_pts, false, i == 0 ? 25 : 30);
      if (dumps[i].playing || dumps[i].pending != 0 || dumps[i].looping) {
        failure = "navigation was not complete";
      } else if (i == 0 && left[i] != right[i]) {
        failure = "25 fps pair not level";
      } else if (i >= 2 && i <= 15) {
        const int distance = i <= 8 ? static_cast<int>(i - 1) : 14 - static_cast<int>(i - 1);
        if (left[i] != left[1] + distance || right[i] != right[1] + distance) {
          failure = "a native frame was skipped at dump " + std::to_string(i);
        }
      } else if (i == 16 && (left[i] <= left[15] || std::fabs(left[i] / 25.0 - right[i] / 30.0) > 0.027)) {
        failure = "playback did not resume synchronized";
      } else if ((i == 18 || i == 20) && (left[i] != left[i - 1] || right[i] != right[i - 1])) {
        failure = "Shift+A/D did not leave playback paused";
      }
    }
    if (!failure.empty()) {
      std::fprintf(stderr, "FAIL mixed-rate-rights: %s\ncaptured stdout:\n%s\n", failure.c_str(), output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS mixed-rate-rights: 25/30 fps videos step one native frame each way, playback resynchronizes, and stepping pauses playback/buffer loops\n");
    }
  } else if (scenario == Scenario::RightStepEdges) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    constexpr size_t kDumps = 4;
    int left[kDumps] = {};
    int right[kDumps] = {};
    bool parsed = dumps.size() >= kDumps;
    for (size_t i = 0; parsed && i < kDumps; ++i) {
      parsed = std::sscanf(dumps[i].frame_left.c_str(), "%d", &left[i]) == 1 && std::sscanf(dumps[i].frame_right.c_str(), "%d", &right[i]) == 1;
    }
    // 4 s at 25 fps: frames 0..99
    constexpr int kLastFrame = 99;
    auto fail_edges = [&](const std::string& message) {
      std::fprintf(stderr, "FAIL right-step-edges: %s\n", message.c_str());
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: frame_left=%s frame_right=%s\n", i, dumps[i].frame_left.c_str(), dumps[i].frame_right.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };
    if (!parsed) {
      fail_edges("expected 4 Shift+X dumps with frame numbers");
    } else if (right[0] != left[0]) {
      fail_edges("sides were not level before stepping");
    } else if (right[1] - left[1] != -10 || left[1] != left[0]) {
      fail_edges("Ctrl+- did not step right 1 back by 10 frames alone");
    } else if (right[2] != kLastFrame || left[2] != left[0]) {
      fail_edges("Alt++ past the end did not stop right 1 at its last frame");
    } else if (output.find("reached end") == std::string::npos) {
      fail_edges("no reached-end notice");
    } else if (right[3] != left[3] || left[3] != left[0]) {
      fail_edges("Ctrl+0 did not bring right 1 back level");
    } else {
      std::printf("PASS right-step-edges: Ctrl+- -10, Alt++ stopped at the last frame with a reached-end notice, Ctrl+0 level\n");
    }
  }

  if (exit_code == EXIT_SUCCESS) {
    std::printf("PASS VideoCompare returned without throwing\n");
  }

  return exit_code;
}

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage:\n");
  std::fprintf(stderr, "  %s baseline LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s event-injection LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s seek LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s still-seek LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s sync-mismatch LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s multi-right-sync LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s frame-navigation LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s frame-stepping LEFT RIGHT0 RIGHT1 REF\n", argv0);
  std::fprintf(stderr, "  %s frame-stepping-vfr LEFT RIGHT0 RIGHT1 REF\n", argv0);
  std::fprintf(stderr, "  %s shared-frame-stepping LEFT RIGHT0 RIGHT1 REF\n", argv0);
  std::fprintf(stderr, "  %s buffer-forward-only LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s buffer-pingpong LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-forward LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-mixed LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s crop-copy LEFT RIGHT0 RIGHT1 RIGHT2\n", argv0);
  std::fprintf(stderr, "  %s interactive-crop LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s dynamic-swap LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s right-step LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s right-step-play LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s crop-preview LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s right-step-edges LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s mixed-rate-rights LEFT_25 RIGHT0_25 RIGHT1_30\n", argv0);
  std::fprintf(stderr, "  %s ref-overlay LEFT RIGHT REF\n", argv0);
  std::fprintf(stderr, "  %s ref-step LEFT RIGHT REF\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const std::string name = argv[1];
  std::vector<std::string> files;
  for (int i = 2; i < argc; ++i) {
    files.push_back(argv[i]);
  }

  try {
    if (name == "baseline") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::Baseline, files);
    }
    if (name == "event-injection") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::EventInjection, files);
    }
    if (name == "seek") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::Seek, files);
    }
    if (name == "still-seek") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::StillSeek, files);
    }
    if (name == "sync-mismatch") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SyncMismatch, files);
    }
    if (name == "multi-right-sync") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::MultiRightSync, files);
    }
    if (name == "frame-stepping" || name == "frame-stepping-vfr" || name == "shared-frame-stepping") {
      if (files.size() != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(name == "shared-frame-stepping" ? Scenario::SharedFrameStepping : name == "frame-stepping" ? Scenario::FrameStepping : Scenario::FrameSteppingVfr, files);
    }
    if (name == "frame-navigation") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::FrameNavigation, files);
    }
    if (name == "buffer-forward-only") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::BufferForwardOnly, files);
    }
    if (name == "buffer-pingpong") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::BufferPingPong, files);
    }
    if (name == "seek-burst-forward") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SeekBurstForward, files);
    }
    if (name == "seek-burst-mixed") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SeekBurstMixed, files);
    }
    if (name == "crop-copy") {
      if (files.size() != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::CropCopy, files);
    }
    if (name == "interactive-crop") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::InteractiveCrop, files);
    }
    if (name == "dynamic-swap") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::DynamicSwap, files);
    }
    if (name == "right-step") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::RightStep, files);
    }
    if (name == "right-step-play") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::RightStepPlay, files);
    }
    if (name == "crop-preview") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::CropPreview, files);
    }
    if (name == "mixed-rate-rights") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::MixedRateRights, files);
    }
    if (name == "right-step-edges") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::RightStepEdges, files);
    }
    if (name == "ref-overlay") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::RefOverlay, files);
    }
    if (name == "ref-step") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::RefStep, files);
    }
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return EXIT_FAILURE;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
