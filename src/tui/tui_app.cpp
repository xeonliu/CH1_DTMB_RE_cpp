#include "tui_app.hpp"

#include <ftxui/component/app.hpp>        // App (aka ScreenInteractive)
#include <ftxui/component/component.hpp>  // CatchEvent, Renderer
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lme2510/tui/tui_engine.hpp"
#include "lme2510/tui/tui_model.hpp"
#include "lme2510/util/arg_parser.hpp"

namespace lme2510::tui {

namespace {

using ftxui::Color;
using ftxui::Decorator;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

constexpr int kMinColumns = 100;  // Below this the console page stacks.

enum class Page { kFrequencies, kConsole, kEpg, kDebug };
enum class RightTab { kOutput, kDetail };

struct Confirm {
  enum class Action { kNone, kTune, kScan, kQuit };
  Action action = Action::kNone;
  int mhz = 0;
  std::vector<int> scanList;
  bool active() const { return action != Action::kNone; }
};

std::vector<int> defaultFrequencyList() {
  std::vector<int> out;
  for (int mhz = 474; mhz <= 858; mhz += 8) {
    out.push_back(mhz);
  }
  return out;
}

// ---- small formatting helpers ----------------------------------------------

std::string percentText(int value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%d%%", value);
  return buffer;
}

std::string hexText(int value) {
  if (value < 0) {
    return "--";
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%02X", value & 0xFF);
  return buffer;
}

std::string rateText(double mbps) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%.1f Mbit/s", mbps);
  return buffer;
}

std::string bytesText(std::uint64_t value) {
  char buffer[32];
  if (value >= 1000000000ULL) {
    std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                  static_cast<double>(value) / 1000000000.0);
  } else if (value >= 1000000ULL) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                  static_cast<double>(value) / 1000000.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
  }
  return buffer;
}

std::string epgTimeText(std::uint64_t utcSeconds) {
  if (utcSeconds == 0) {
    return "--:--";
  }
  const std::time_t time = static_cast<std::time_t>(utcSeconds);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d-%02d %02d:%02d",
                local.tm_mon + 1, local.tm_mday, local.tm_hour,
                local.tm_min);
  return buffer;
}

std::string epgDurationText(std::uint32_t seconds) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02u:%02u", seconds / 3600,
                (seconds % 3600) / 60);
  return buffer;
}

int utf8Codepoint(std::string_view text, std::size_t index,
                  std::size_t* length) {
  const std::uint8_t byte = static_cast<std::uint8_t>(text[index]);
  if (byte < 0x80) {
    *length = 1;
    return byte;
  }
  const std::size_t remaining = text.size() - index;
  if ((byte & 0xE0) == 0xC0 && remaining >= 2) {
    *length = 2;
    return ((byte & 0x1F) << 6) |
           (static_cast<std::uint8_t>(text[index + 1]) & 0x3F);
  }
  if ((byte & 0xF0) == 0xE0 && remaining >= 3) {
    *length = 3;
    return ((byte & 0x0F) << 12) |
           ((static_cast<std::uint8_t>(text[index + 1]) & 0x3F) << 6) |
           (static_cast<std::uint8_t>(text[index + 2]) & 0x3F);
  }
  if ((byte & 0xF8) == 0xF0 && remaining >= 4) {
    *length = 4;
    return ((byte & 0x07) << 18) |
           ((static_cast<std::uint8_t>(text[index + 1]) & 0x3F) << 12) |
           ((static_cast<std::uint8_t>(text[index + 2]) & 0x3F) << 6) |
           (static_cast<std::uint8_t>(text[index + 3]) & 0x3F);
  }
  *length = 1;
  return 0;
}

bool isWideCodepoint(int cp) {
  // Rough East-Asian-width approximation (double-width unless narrow).
  if (cp < 0x1100) {
    return false;
  }
  if (cp <= 0x115F || cp == 0x2329 || cp == 0x232A) {
    return true;
  }
  if ((cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6)) {
    return true;
  }
  return cp >= 0x20000;
}

int cellWidth(std::string_view text) {
  int width = 0;
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t length = 1;
    const int cp = utf8Codepoint(text, index, &length);
    width += isWideCodepoint(cp) ? 2 : 1;
    index += length;
  }
  return width;
}

/// Truncates to at most width display cells, then pads with spaces to exactly
/// width cells so every row has a deterministic box width.
std::string fit(std::string_view text, int width) {
  if (width <= 0) {
    return std::string();
  }
  std::string out;
  out.reserve(static_cast<std::size_t>(width));
  int used = 0;
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t length = 1;
    const int cp = utf8Codepoint(text, index, &length);
    const int cpWidth = isWideCodepoint(cp) ? 2 : 1;
    if (used + cpWidth > width) {
      break;
    }
    out.append(text, index, length);
    used += cpWidth;
    index += length;
  }
  out.append(static_cast<std::size_t>(width - used), ' ');
  return out;
}

std::string padLeft(std::string text, int width) {
  const int missing = width - cellWidth(text);
  if (missing > 0) {
    text.insert(0, static_cast<std::size_t>(missing), ' ');
  }
  return fit(text, width);
}

std::string padRight(std::string text, int width) {
  const int missing = width - cellWidth(text);
  if (missing > 0) {
    text.append(static_cast<std::size_t>(missing), ' ');
  }
  return fit(text, width);
}

std::string barText(int width, int percent) {
  std::string bar;
  bar.reserve(static_cast<std::size_t>(width));
  const int fill = std::clamp(width * percent / 100, 0, width);
  for (int index = 0; index < width; ++index) {
    bar.push_back(index < fill ? '#' : '-');
  }
  return bar;
}

std::string boolText(bool value) { return value ? "开" : "关"; }

bool isKey(const Event& event, char lower, char upper) {
  return event == Event::Character(lower) || event == Event::Character(upper);
}

// ---- stdout tee ----------------------------------------------------------------
//
// FTXUI draws its frames through std::cout, while the receiver/demodulator
// backends also print plain diagnostics to std::cout.  Escape-sequence output
// (FTXUI frames) must reach the terminal untouched; plain text must not (it
// would corrupt the alternate screen), so it is captured for the status row.
class StdoutTee : public std::streambuf {
 public:
  explicit StdoutTee(std::streambuf* sink, std::size_t maxLines = 600)
      : sink_(sink), maxLines_(maxLines) {}

  std::string latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.empty() ? std::string() : lines_.back();
  }

 protected:
  int_type overflow(int_type character) override {
    if (character == traits_type::eof()) {
      return traits_type::not_eof(character);
    }
    const char byte = static_cast<char>(character);
    feed(&byte, 1);
    return traits_type::not_eof(character);
  }

  std::streamsize xsputn(const char* text, std::streamsize count) override {
    if (count > 0) {
      feed(text, static_cast<std::size_t>(count));
    }
    return count;
  }

  int sync() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return sink_->pubsync();
  }

 private:
  void feed(const char* text, std::size_t count) {
    const bool control = std::memchr(text, '\x1b', count) != nullptr;
    if (control) {
      std::lock_guard<std::mutex> lock(mutex_);
      sink_->sputn(text, static_cast<std::streamsize>(count));
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t index = 0; index < count; ++index) {
      if (text[index] == '\n') {
        appendLine(pending_);
        pending_.clear();
      } else {
        pending_.push_back(text[index]);
        if (pending_.size() > 4096) {
          appendLine(pending_);
          pending_.clear();
        }
      }
    }
  }

  void appendLine(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == ' ')) {
      trimmed.pop_back();
    }
    if (!trimmed.empty()) {
      lines_.push_back(trimmed);
      if (lines_.size() > maxLines_) {
        lines_.pop_front();
      }
    }
  }

  std::streambuf* sink_;
  std::size_t maxLines_;
  std::string pending_;
  std::deque<std::string> lines_;
  mutable std::mutex mutex_;
};

// ---- UI state ---------------------------------------------------------------

class TuiUi {
 public:
  TuiUi(const Options& options, TuiModel& model, TuiEngine& engine,
        std::ifstream& commandLog, const StdoutTee* outputTee,
        std::function<void()> exitApp)
      : options_(options),
        model_(model),
        engine_(engine),
        commandLog_(commandLog),
        outputTee_(outputTee),
        exitApp_(std::move(exitApp)) {}

  bool OnEvent(const Event& event);
  Element Render();

 private:
  struct Metrics {
    int dimx = 80;
    int dimy = 24;
    bool wide = true;
    int headerRows = 3;
    int helpRows = 1;
    int signalRows = 0;
    int bodyRows = 0;
    int leftW = 0;   // console left-pane content width (gap excluded)
    int rightW = 0;  // console right-pane content width
    int leftRows = 0;   // rows allocated to the left pane (narrow mode)
    int rightRows = 0;  // rows allocated to the right pane
  };

  Metrics computeMetrics() const;
  void clampCursors();
  void refreshCommandLog();

  Element renderHeaderRow(int width) const;
  Element renderContextRow(int width) const;
  Element renderPhaseRow(int width);
  void pushSignalRows(int width, Elements& rows) const;
  Element renderHelpRow(int width) const;
  Element renderConfirmOverlay() const;
  Element renderFatalBody(int width) const;
  Element renderFrequenciesBody(int width, int height);
  Element renderConsoleBody(int width);
  Element renderEpgBody(int width, int height);
  Element renderDebugBody(int width, int height) const;

  std::vector<std::string> frequencyLines(int width, int height);
  std::vector<std::string> consoleLeftLines(int width, int height);
  std::vector<std::string> consoleRightLines(int width, int height);
  std::vector<std::string> outputPaneLines(int width) const;
  std::vector<std::string> detailPaneLines(int width) const;
  std::vector<std::string> epgLines(int width, int height);

  std::string phaseText() const;

  const Options& options_;
  TuiModel& model_;
  TuiEngine& engine_;
  std::ifstream& commandLog_;
  const StdoutTee* outputTee_;
  std::function<void()> exitApp_;

  Page page_ = Page::kFrequencies;
  RightTab rightTab_ = RightTab::kOutput;
  std::size_t freqCursor_ = 0;
  std::size_t freqTop_ = 0;
  std::size_t svcCursor_ = 0;
  std::size_t svcTop_ = 0;
  std::size_t paneTop_ = 0;
  std::size_t epgService_ = 0;
  std::size_t epgScroll_ = 0;
  std::size_t debugTop_ = 0;
  bool pendingTune_ = false;
  std::chrono::steady_clock::time_point busyStart_{};
  Confirm confirm_;
  bool quitRequested_ = false;
  std::deque<std::string> commandLines_;
};

// ---- events ------------------------------------------------------------------

bool TuiUi::OnEvent(const Event& event) {
  if (quitRequested_) {
    return true;
  }
  if (event == Event::Custom) {
    refreshCommandLog();
    return true;
  }
  if (event == Event::Escape && !confirm_.active()) {
    return true;  // reserved for overlay cancel.
  }

  if (model_.fatal()) {
    if (isKey(event, 'q', 'Q')) {
      quitRequested_ = true;
      exitApp_();
    }
    return true;
  }

  if (confirm_.active()) {
    if (isKey(event, 'y', 'Y')) {
      const Confirm pending = confirm_;
      confirm_ = Confirm();
      switch (pending.action) {
        case Confirm::Action::kTune:
          pendingTune_ = true;
          engine_.requestTune(pending.mhz);
          break;
        case Confirm::Action::kScan:
          engine_.requestScan(pending.scanList);
          break;
        case Confirm::Action::kQuit:
          quitRequested_ = true;
          exitApp_();
          break;
        case Confirm::Action::kNone:
          break;
      }
    } else if (isKey(event, 'n', 'N') || event == Event::Escape ||
               event == Event::Return || isKey(event, 'q', 'Q')) {
      confirm_ = Confirm();
    }
    return true;
  }

  const bool outputting = model_.streaming() || model_.recording();
  const bool monitoring = model_.monitoring();

  if (isKey(event, 'q', 'Q')) {
    if (outputting) {
      Confirm confirm;
      confirm.action = Confirm::Action::kQuit;
      confirm_ = confirm;
    } else {
      quitRequested_ = true;
      exitApp_();
    }
    return true;
  }

  // Global navigation.
  if (isKey(event, '1', '1') || isKey(event, 'f', 'F')) {
    page_ = Page::kFrequencies;
    return true;
  }
  if (isKey(event, '2', '2')) {
    if (monitoring && !model_.busy()) {
      page_ = Page::kConsole;
    }
    return true;
  }
  if (isKey(event, '4', '4')) {
    page_ = Page::kDebug;
    return true;
  }

  switch (page_) {
    case Page::kFrequencies: {
      if (model_.busy() || !model_.ready()) {
        return true;
      }
      const std::vector<FreqEntry> entries = model_.frequencies();
      if (entries.empty()) {
        return true;
      }
      if (freqCursor_ >= entries.size()) {
        freqCursor_ = entries.size() - 1;
      }
      const Metrics metrics = computeMetrics();
      const int pageRows = std::max(1, metrics.bodyRows - 3);

      if (event == Event::ArrowUp || isKey(event, 'k', 'K')) {
        freqCursor_ = freqCursor_ == 0 ? entries.size() - 1 : freqCursor_ - 1;
        return true;
      }
      if (event == Event::ArrowDown || isKey(event, 'j', 'J')) {
        freqCursor_ = (freqCursor_ + 1) % entries.size();
        return true;
      }
      if (event == Event::PageUp) {
        freqCursor_ =
            freqCursor_ <= static_cast<std::size_t>(pageRows)
                ? 0
                : freqCursor_ - static_cast<std::size_t>(pageRows);
        return true;
      }
      if (event == Event::PageDown) {
        freqCursor_ = std::min(entries.size() - 1,
                               freqCursor_ + static_cast<std::size_t>(pageRows));
        return true;
      }
      if (event == Event::Character(' ')) {
        model_.setChecked(freqCursor_, !entries[freqCursor_].checked);
        return true;
      }
      if (isKey(event, 'a', 'A')) {
        bool anyUnchecked = false;
        for (const FreqEntry& entry : entries) {
          anyUnchecked = anyUnchecked || !entry.checked;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
          model_.setChecked(index, anyUnchecked);
        }
        return true;
      }
      if (isKey(event, 's', 'S')) {
        std::vector<int> toScan;
        for (const FreqEntry& entry : entries) {
          if (entry.checked) {
            toScan.push_back(entry.mhz);
          }
        }
        if (toScan.empty()) {
          toScan.push_back(entries[freqCursor_].mhz);
        }
        if (outputting) {
          Confirm confirm;
          confirm.action = Confirm::Action::kScan;
          confirm.scanList = toScan;
          confirm_ = confirm;
        } else {
          pendingTune_ = false;
          engine_.requestScan(toScan);
        }
        return true;
      }
      if (event == Event::Return) {
        if (outputting) {
          Confirm confirm;
          confirm.action = Confirm::Action::kTune;
          confirm.mhz = entries[freqCursor_].mhz;
          confirm_ = confirm;
        } else {
          pendingTune_ = true;
          engine_.requestTune(entries[freqCursor_].mhz);
        }
        return true;
      }
      return true;
    }

    case Page::kConsole: {
      if (!monitoring) {
        return true;
      }
      const std::vector<MuxService> services = model_.services();
      if (!services.empty() && svcCursor_ >= services.size()) {
        svcCursor_ = services.size() - 1;
      }
      const Metrics metrics = computeMetrics();
      const int pageRows = std::max(1, metrics.leftRows - 4);

      if (event == Event::ArrowUp || isKey(event, 'k', 'K')) {
        if (!services.empty()) {
          svcCursor_ = svcCursor_ == 0 ? services.size() - 1 : svcCursor_ - 1;
        }
        return true;
      }
      if (event == Event::ArrowDown || isKey(event, 'j', 'J')) {
        if (!services.empty()) {
          svcCursor_ = (svcCursor_ + 1) % services.size();
        }
        return true;
      }
      if (event == Event::PageUp) {
        if (!services.empty()) {
          svcCursor_ =
              svcCursor_ <= static_cast<std::size_t>(pageRows)
                  ? 0
                  : svcCursor_ - static_cast<std::size_t>(pageRows);
        }
        return true;
      }
      if (event == Event::PageDown) {
        if (!services.empty()) {
          svcCursor_ = std::min(
              services.size() - 1,
              svcCursor_ + static_cast<std::size_t>(pageRows));
        }
        return true;
      }
      if (event == Event::Tab) {
        rightTab_ = rightTab_ == RightTab::kOutput ? RightTab::kDetail
                                                    : RightTab::kOutput;
        paneTop_ = 0;
        return true;
      }
      if (isKey(event, 'e', 'E') && !services.empty()) {
        epgService_ = svcCursor_ < services.size() ? svcCursor_ : 0;
        epgScroll_ = 0;
        page_ = Page::kEpg;
        return true;
      }
      if (event == Event::Return && svcCursor_ < services.size()) {
        if (!model_.busy()) {
          engine_.requestSelectService(services[svcCursor_].programNumber);
        }
        return true;
      }
      if (isKey(event, 'c', 'C') && !model_.busy()) {
        engine_.requestClearService();
        return true;
      }
      if (model_.busy()) {
        return true;
      }
      if (event == Event::Character(' ')) {
        engine_.requestStream(!model_.streaming());
        return true;
      }
      if (isKey(event, 'r', 'R')) {
        engine_.requestRecord(!model_.recording());
        return true;
      }
      return true;
    }

    case Page::kEpg: {
      const std::vector<MuxService> services = model_.services();
      if (!services.empty()) {
        if (epgService_ >= services.size()) {
          epgService_ = services.size() - 1;
        }
        const std::size_t eventCount = services[epgService_].events.size();
        const std::size_t maxTop = eventCount > 0 ? eventCount - 1 : 0;
        const Metrics metrics = computeMetrics();
        const std::size_t pageRows =
            std::max(static_cast<std::size_t>(1),
                     static_cast<std::size_t>(metrics.bodyRows) - 1);

        if (event == Event::ArrowUp || isKey(event, 'k', 'K')) {
          epgScroll_ = epgScroll_ == 0 ? 0 : epgScroll_ - 1;
          return true;
        }
        if (event == Event::ArrowDown || isKey(event, 'j', 'J')) {
          epgScroll_ = std::min(epgScroll_ + 1, maxTop);
          return true;
        }
        if (event == Event::PageUp) {
          epgScroll_ = epgScroll_ <= pageRows ? 0 : epgScroll_ - pageRows;
          return true;
        }
        if (event == Event::PageDown) {
          epgScroll_ = std::min(epgScroll_ + pageRows, maxTop);
          return true;
        }
        if (event == Event::Character('[')) {
          epgService_ = epgService_ == 0 ? services.size() - 1
                                         : epgService_ - 1;
          epgScroll_ = 0;
          return true;
        }
        if (event == Event::Character(']')) {
          epgService_ = (epgService_ + 1) % services.size();
          epgScroll_ = 0;
          return true;
        }
      }
      if (event == Event::Escape) {
        page_ = Page::kConsole;
        return true;
      }
      return true;
    }

    case Page::kDebug: {
      const Metrics metrics = computeMetrics();
      const int pageRows = std::max(1, metrics.bodyRows - 4);
      if (event == Event::ArrowUp || isKey(event, 'k', 'K')) {
        debugTop_ = std::min(debugTop_ + 1, commandLines_.size());
        return true;
      }
      if (event == Event::ArrowDown || isKey(event, 'j', 'J')) {
        debugTop_ = debugTop_ == 0 ? 0 : debugTop_ - 1;
        return true;
      }
      if (event == Event::PageUp) {
        debugTop_ = std::min(debugTop_ + static_cast<std::size_t>(pageRows),
                             commandLines_.size());
        return true;
      }
      if (event == Event::PageDown) {
        debugTop_ = debugTop_ > static_cast<std::size_t>(pageRows)
                        ? debugTop_ - static_cast<std::size_t>(pageRows)
                        : 0;
        return true;
      }
      return true;
    }
  }
  return false;
}

// ---- model/status helpers -----------------------------------------------------

std::string TuiUi::phaseText() const {
  if (model_.fatal()) {
    return "错误: " + model_.errorText();
  }
  std::string text = model_.phaseText();
  if (text.empty()) {
    text = model_.ready() ? "设备就绪" : "初始化…";
  }
  if (!model_.errorText().empty()) {
    text += "  [" + model_.errorText() + "]";
  }
  return text;
}

void TuiUi::refreshCommandLog() {
  if (!commandLog_.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(commandLog_, line)) {
    if (line.size() > 240) {
      line.resize(240);
      line += "...";
    }
    commandLines_.push_back(line);
    if (commandLines_.size() > 2000) {
      commandLines_.pop_front();
    }
  }
}

void TuiUi::clampCursors() {
  const std::vector<FreqEntry> entries = model_.frequencies();
  if (!entries.empty() && freqCursor_ >= entries.size()) {
    freqCursor_ = entries.size() - 1;
  }
  const std::vector<MuxService> services = model_.services();
  if (!services.empty() && svcCursor_ >= services.size()) {
    svcCursor_ = services.size() - 1;
  }
}

TuiUi::Metrics TuiUi::computeMetrics() const {
  Metrics metrics;
  const ftxui::Dimensions terminal = ftxui::Terminal::Size();
  metrics.dimx = std::max(terminal.dimx, 40);
  metrics.dimy = std::max(terminal.dimy, 12);
  metrics.wide = metrics.dimx >= kMinColumns;
  metrics.signalRows = 0;
  if (page_ == Page::kConsole) {
    metrics.signalRows = 2;
  } else if (page_ == Page::kFrequencies && model_.monitoring()) {
    metrics.signalRows = 2;
  }
  const int fixedRows =
      metrics.headerRows + metrics.helpRows + metrics.signalRows;
  metrics.bodyRows = std::max(1, metrics.dimy - fixedRows);

  if (page_ == Page::kConsole && metrics.wide) {
    metrics.leftW = (metrics.dimx * 55) / 100 - 1;
    metrics.leftW = std::max(metrics.leftW, 30);
    metrics.rightW = metrics.dimx - metrics.leftW - 1;
    metrics.rightW = std::max(metrics.rightW, 30);
    metrics.leftRows = metrics.bodyRows;
    metrics.rightRows = metrics.bodyRows;
  } else {
    metrics.leftW = metrics.dimx;
    metrics.rightW = metrics.dimx;
    if (page_ == Page::kConsole) {
      metrics.leftRows = std::max(4, metrics.bodyRows * 55 / 100);
      metrics.leftRows = std::min(metrics.leftRows, metrics.bodyRows - 3);
      metrics.rightRows = std::max(2, metrics.bodyRows - metrics.leftRows - 1);
    } else {
      metrics.leftRows = metrics.bodyRows;
      metrics.rightRows = 0;
    }
  }
  return metrics;
}

// ---- render -------------------------------------------------------------------

Element TuiUi::Render() {
  clampCursors();

  if (pendingTune_ && model_.monitoring() && model_.currentMhz() > 0) {
    pendingTune_ = false;
    page_ = Page::kConsole;
  }

  const Metrics metrics = computeMetrics();
  const int width = metrics.dimx;

  Elements rows;
  rows.push_back(renderHeaderRow(width));
  rows.push_back(renderContextRow(width));
  rows.push_back(renderPhaseRow(width));

  if (model_.fatal()) {
    rows.push_back(renderFatalBody(width));
  } else {
    switch (page_) {
      case Page::kFrequencies:
        rows.push_back(renderFrequenciesBody(width, metrics.bodyRows));
        break;
      case Page::kConsole:
        rows.push_back(renderConsoleBody(width));
        break;
      case Page::kEpg:
        rows.push_back(renderEpgBody(width, metrics.bodyRows));
        break;
      case Page::kDebug:
        rows.push_back(renderDebugBody(width, metrics.bodyRows));
        break;
    }
  }

  pushSignalRows(width, rows);
  rows.push_back(renderHelpRow(width));

  Element document = ftxui::vbox(rows);
  if (confirm_.active()) {
    document = ftxui::dbox({document, renderConfirmOverlay()});
  }
  return document;
}

Element TuiUi::renderHeaderRow(int width) const {
  std::string title;
  switch (page_) {
    case Page::kFrequencies:
      title = "[1 频率页]  474-858 MHz / 8 MHz 网格";
      break;
    case Page::kConsole:
      title = "[2 控制台]  节目 + 输出控制 + 信号同屏";
      break;
    case Page::kEpg:
      title = "[3 节目单]  当前台 EPG / EIT";
      break;
    case Page::kDebug:
      title = "[4 调试]  寄存器 / 命令日志";
      break;
  }
  return ftxui::text(fit("lme2510_stream --tui   " + title, width)) | ftxui::bold;
}

Element TuiUi::renderContextRow(int width) const {
  std::string text;
  const std::vector<FreqEntry> entries = model_.frequencies();
  std::size_t checkedCount = 0;
  std::size_t lockedCount = 0;
  for (const FreqEntry& entry : entries) {
    checkedCount += entry.checked ? 1 : 0;
    lockedCount += entry.scanned && entry.locked ? 1 : 0;
  }
  if (page_ == Page::kFrequencies) {
    text = "勾选 " + std::to_string(checkedCount) + "/" +
           std::to_string(entries.size()) + "    已锁定 " +
           std::to_string(lockedCount);
    if (model_.monitoring() && model_.currentMhz() > 0) {
      text += "    监视中: " + std::to_string(model_.currentMhz()) + " MHz";
    }
    if (model_.streaming() || model_.recording()) {
      text += "    输出中: 调频/扫描/退出需确认";
    }
  } else if (page_ == Page::kConsole) {
    const ServiceSelectionSnapshot selected = model_.selectedService();
    text = std::to_string(model_.currentMhz()) + " MHz | 台: " +
           (selected.active ? selected.name : std::string("整频点")) +
           " | 输出范围: " +
           (selected.active ? std::string("单台 PID 过滤")
                            : std::string("整频点（全 PID）")) +
           " | 模式: ";
    std::string mode;
    if (model_.streaming()) {
      mode += "推流";
    }
    if (model_.recording()) {
      if (!mode.empty()) {
        mode += "+";
      }
      mode += "录制";
    }
    if (mode.empty()) {
      mode = "监视";
    }
    text += mode;
  } else if (page_ == Page::kEpg) {
    const std::vector<MuxService> services = model_.services();
    if (!services.empty()) {
      const std::size_t index =
          std::min(epgService_, services.size() - 1);
      text = std::to_string(model_.currentMhz()) +
             " MHz | 节目单: " + services[index].name + "（prog " +
             std::to_string(services[index].programNumber) + "）| 共 " +
             std::to_string(services[index].events.size()) + " 条";
    } else {
      text = std::to_string(model_.currentMhz()) + " MHz | 暂无节目表";
    }
  } else {
    text = "reg-log: " + options_.regLogPath;
  }
  return ftxui::text(fit(text, width));
}

Element TuiUi::renderPhaseRow(int width) {
  const auto now = std::chrono::steady_clock::now();
  std::string text = phaseText();
  if (model_.busy()) {
    if (busyStart_ == std::chrono::steady_clock::time_point()) {
      busyStart_ = now;
    }
    const double elapsed =
        std::chrono::duration<double>(now - busyStart_).count();
    text += "   · 引擎处理中 " + std::to_string(static_cast<int>(elapsed)) +
            " 秒";
  } else {
    busyStart_ = std::chrono::steady_clock::time_point();
  }
  if (outputTee_ != nullptr) {
    const std::string latest = outputTee_->latest();
    if (!latest.empty()) {
      text += "  |  " + latest;
    }
  }
  Element element = ftxui::text(fit(text, width));
  if (!model_.errorText().empty() || model_.fatal()) {
    element = element | ftxui::color(Color::RedLight);
  }
  return element;
}

Element TuiUi::renderFatalBody(int width) const {
  Elements body;
  body.push_back(ftxui::text(""));
  body.push_back(ftxui::text(fit("设备初始化失败，请检查 USB 电视棒与权限。", width)));
  body.push_back(ftxui::text(fit("按 Q 退出。", width)));
  return ftxui::vbox(body);
}

void TuiUi::pushSignalRows(int width, Elements& rows) const {
  if (page_ == Page::kFrequencies && !model_.monitoring()) {
    return;
  }
  if (page_ == Page::kDebug || page_ == Page::kEpg) {
    return;
  }
  const SignalSnapshot signal = model_.signal();
  const TsMetricsSnapshot stats = model_.counters();

  std::string lockText = signal.has ? (signal.lock ? "锁定: YES" : "锁定: NO")
                                    : "未锁定 / 等待状态包";
  const int barWidth = std::clamp((width - 58) / 2, 6, 24);

  std::string first = lockText;
  first += "  信号";
  const std::string strength =
      signal.has ? percentText(signal.strengthPct) : "--";
  first += " " + barText(barWidth, signal.has ? signal.strengthPct : 0) +
           " " + strength;
  first += "  质量";
  const std::string quality =
      signal.has ? percentText(signal.qualityPct) : "--";
  first += " " + barText(barWidth, signal.has ? signal.qualityPct : 0) + " " +
           quality;
  first += "  TS " + rateText(stats.rateMbps);
  rows.push_back(ftxui::text(fit(first, width)));

  std::string second;
  if (signal.has) {
    second = "EP8A: sig=" + hexText(signal.signalRaw) + " snr=" +
             hexText(signal.snrRaw) + " hi=" + hexText(signal.hi) +
             " lo=" + hexText(signal.lo) + "  状态包命中/丢失: " +
             std::to_string(signal.hits) + "/" +
             std::to_string(signal.misses);
  } else {
    second = "EP 0x8A 状态包尚未到达…（已读取 " +
             std::to_string(model_.statusMisses()) + " 次超时）";
  }
  const TelemetrySnapshot telemetry = model_.telemetry();
  if (telemetry.has) {
    second += "   寄存器: " + telemetry.detail;
  }
  rows.push_back(ftxui::text(fit(second, width)));
}

Element TuiUi::renderHelpRow(int width) const {
  std::string text;
  switch (page_) {
    case Page::kFrequencies:
      text =
          "Space 勾选 | A 全选/全不选 | S 扫描勾选集 | Enter 调谐 | "
          "1/2 页 | 4 日志 | Q 退出";
      break;
    case Page::kConsole:
      text =
          "Space 推流 | R 录制 | Enter 选台 | C 整频点 | Tab 右栏子页 | "
          "F/1 频率 | 4 日志 | Q 退出";
      break;
    case Page::kEpg:
      text =
          "↑↓ 滚动 | [ / ] 换台 | Esc/2 返回控制台 | F/1 频率 | 4 日志 | "
          "Q 退出";
      break;
    case Page::kDebug:
      text = "↑↓/PgUp/PgDn 滚动 | F/1 返回频率页 | 4 当前页 | Q 退出";
      break;
  }
  return ftxui::text(fit(text, width)) | ftxui::dim;
}

Element TuiUi::renderConfirmOverlay() const {
  std::string title;
  std::string body;
  switch (confirm_.action) {
    case Confirm::Action::kTune:
      title = "确认调谐到 " + std::to_string(confirm_.mhz) + " MHz？";
      body = "推流/录制进行中。调谐将重启采集，并停止当前输出。";
      break;
    case Confirm::Action::kScan:
      title = "确认扫描 " + std::to_string(confirm_.scanList.size()) + " 个频点？";
      body = "推流/录制进行中。扫描将重启采集，并停止当前输出。";
      break;
    case Confirm::Action::kQuit:
      title = "确认退出？";
      body = "推流/录制进行中。退出将停止输出并释放设备。";
      break;
    case Confirm::Action::kNone:
      return ftxui::text("");
  }
  const Metrics metrics = computeMetrics();
  const int panelWidth = std::clamp(metrics.dimx - 8, 36, 72);

  Elements lines;
  lines.push_back(ftxui::text(fit(title, panelWidth)) | ftxui::bold);
  lines.push_back(ftxui::separator());
  lines.push_back(ftxui::text(fit(body, panelWidth)));
  lines.push_back(ftxui::text(""));
  lines.push_back(ftxui::text(
      fit("[y/Y] 继续        [Enter/n/N/Esc] 取消（默认）", panelWidth)));

  Element panel = ftxui::vbox(lines);
  panel = panel | ftxui::border | ftxui::clear_under;
  return ftxui::vbox({ftxui::filler(), ftxui::hbox({ftxui::filler(), panel,
                                                    ftxui::filler()}),
                      ftxui::filler()});
}

// ---- frequency page -----------------------------------------------------------

std::vector<std::string> TuiUi::frequencyLines(int width, int height) {
  std::vector<std::string> out;
  const std::vector<FreqEntry> entries = model_.frequencies();

  out.push_back(fit(
      " 频率MHz  状态      锁定  信号  质量  码率         CC错/丢   备注", width));

  const int rowsAvailable = std::max(0, height - 1);
  if (entries.empty()) {
    return out;
  }
  std::size_t cursor = std::min(freqCursor_, entries.size() - 1);
  if (cursor < freqTop_) {
    freqTop_ = cursor;
  }
  if (freqTop_ + static_cast<std::size_t>(rowsAvailable) <= cursor) {
    freqTop_ = cursor + 1 - static_cast<std::size_t>(rowsAvailable);
  }

  std::size_t shown = 0;
  for (std::size_t index = freqTop_;
       index < entries.size() &&
       shown < static_cast<std::size_t>(rowsAvailable);
       ++index, ++shown) {
    const FreqEntry& entry = entries[index];
    std::string state;
    if (entry.current) {
      state = "当前";
    } else if (entry.scanned) {
      state = entry.locked ? "已锁定" : "无锁定";
    } else if (entry.checked) {
      state = "待扫描";
    }
    const std::string lock =
        entry.scanned ? (entry.locked ? "YES" : "NO") : "-";
    const std::string signal =
        entry.scanned ? percentText(entry.strengthPct) : "-";
    const std::string quality =
        entry.scanned ? percentText(entry.qualityPct) : "-";
    const std::string rate = entry.scanned ? rateText(entry.rateMbps) : "-";
    const std::string cc = entry.scanned
                               ? std::to_string(entry.ccErrors) + "/" +
                                     std::to_string(entry.ccLost)
                               : "-";

    std::string line;
    line += entry.checked ? "x" : " ";
    line += " " + padLeft(std::to_string(entry.mhz), 3) + "MHz ";
    line += padRight(state, 9) + " ";
    line += padLeft(lock, 3) + "  ";
    line += padRight(signal, 5) + " ";
    line += padRight(quality, 5) + " ";
    line += padRight(rate, 12) + " ";
    line += padRight(cc, 10);
    if (!entry.note.empty()) {
      line += " " + entry.note;
    }
    if (index == cursor) {
      line = ">" + line.substr(1);
    }
    out.push_back(fit(line, width));
  }
  return out;
}

Element TuiUi::renderFrequenciesBody(int width, int height) {
  std::vector<std::string> lines = frequencyLines(width, height);
  Elements rows;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const bool isCursor =
        index > 0 && !lines.empty() &&
        index - 1 == freqCursor_ - freqTop_;
    if (isCursor) {
      rows.push_back(ftxui::text(lines[index]) |
                     ftxui::bgcolor(Color::GrayDark));
    } else {
      rows.push_back(ftxui::text(lines[index]));
    }
  }
  return ftxui::vbox(rows);
}

// ---- console page -------------------------------------------------------------

std::vector<std::string> TuiUi::consoleLeftLines(int width, int height) {
  std::vector<std::string> out;
  const std::vector<MuxService> services = model_.services();
  out.push_back(fit("节目 / Services（" + std::to_string(services.size()) +
                        " 个）",
                    width));

  if (services.empty()) {
    out.push_back(fit("等待节目表（约 1-2 秒，需先锁定）…", width));
    while (out.size() < static_cast<std::size_t>(height)) {
      out.push_back(fit("", width));
    }
    return out;
  }

  out.push_back(fit("状态 节目号  类型        名称", width));
  std::size_t cursor = std::min(svcCursor_, services.size() - 1);
  if (cursor < svcTop_) {
    svcTop_ = cursor;
  }
  const int rowsAvailable = std::max(0, height - 2);
  if (svcTop_ + static_cast<std::size_t>(rowsAvailable) <= cursor) {
    svcTop_ = cursor + 1 - static_cast<std::size_t>(rowsAvailable);
  }

  std::size_t shown = 0;
  for (std::size_t index = svcTop_;
       index < services.size() &&
       shown < static_cast<std::size_t>(rowsAvailable);
       ++index, ++shown) {
    const MuxService& service = services[index];
    const ServiceSelectionSnapshot selected = model_.selectedService();
    const bool current =
        selected.active && selected.programNumber == service.programNumber;
    std::string line;
    line += current ? "*" : " ";
    line += " " + padLeft(std::to_string(service.programNumber), 6);
    line += " " + padRight(service.typeName, 12);
    line += "  " + service.name;
    if (index == cursor) {
      line = ">" + line.substr(1);
    }
    out.push_back(fit(line, width));
  }
  while (out.size() < static_cast<std::size_t>(height)) {
    out.push_back(fit("", width));
  }
  return out;
}

std::vector<std::string> TuiUi::outputPaneLines(int width) const {
  std::vector<std::string> out;
  const TsMetricsSnapshot stats = model_.counters();
  const ServiceSelectionSnapshot selected = model_.selectedService();

  out.push_back("-- 输出控制 --（Tab → 节目详情）");
  out.push_back("推流: " + boolText(model_.streaming()) +
                "   [Space] 开/关");
  out.push_back("录制: " + boolText(model_.recording()) + "   [R] 开/关");
  out.push_back("文件: " +
                (model_.recordPath().empty()
                     ? (options_.filePath.empty()
                            ? "-（录制时自动 record-<ts>.ts）"
                            : options_.filePath)
                     : model_.recordPath()));
  if (options_.noUdp) {
    out.push_back("UDP: 未启用（--no-udp）");
  } else {
    out.push_back("UDP: " + options_.udpTarget);
  }
  if (!options_.rawUdpTarget.empty()) {
    out.push_back("raw-UDP: " + options_.rawUdpTarget);
  }
  out.push_back("输出范围: " +
                std::string(selected.active
                                ? ("单台 " + selected.name + "（prog " +
                                   std::to_string(selected.programNumber) + ")")
                                : "整频点（全 PID）"));
  out.push_back("TS 速率: " + rateText(stats.rateMbps) +
                "   输出速率: " + rateText(stats.outputRateMbps));
  out.push_back("TS 字节: " + bytesText(stats.bytes) +
                "   包: " + std::to_string(stats.packets));
  out.push_back("UDP 字节: " + bytesText(stats.udpBytes) +
                "   数据报: " + std::to_string(stats.udpDatagrams));
  if (!options_.rawUdpTarget.empty()) {
    out.push_back("raw 字节: " + bytesText(stats.rawBytes) +
                  "   数据报: " + std::to_string(stats.rawDatagrams));
  }
  out.push_back("CC 错误: " + std::to_string(stats.ccErrors) +
                "   丢失: " + std::to_string(stats.ccLost) +
                "   传输错误: " + std::to_string(stats.transportErrors));
  char lossBuffer[24];
  std::snprintf(lossBuffer, sizeof(lossBuffer), "%.3f%%", stats.lossPct);
  out.push_back("丢包估算: " + std::string(lossBuffer) +
                "   重同步: " + std::to_string(stats.resyncs) +
                "   丢弃: " + bytesText(stats.droppedBytes));
  out.push_back("帧: " + std::to_string(stats.frames) +
                "   超时: " + std::to_string(stats.timeouts));
  const TelemetrySnapshot telemetry = model_.telemetry();
  if (telemetry.has) {
    out.push_back("寄存器: " + telemetry.detail);
  }
  for (std::string& line : out) {
    line = fit(line, width);
  }
  return out;
}

std::vector<std::string> TuiUi::detailPaneLines(int width) const {
  std::vector<std::string> out;
  const std::vector<MuxService> services = model_.services();
  out.push_back("-- 节目详情 --（Tab → 输出控制）");
  if (services.empty()) {
    out.push_back(fit("尚无节目表，等待 PAT/PMT/SDT 解析…", width));
    return out;
  }
  std::size_t cursor = std::min(svcCursor_, services.size() - 1);
  const MuxService& service = services[cursor];
  out.push_back("名称  : " + service.name);
  out.push_back("类型  : " + service.typeName);
  out.push_back("节目号: " + std::to_string(service.programNumber));
  char pidText[24];
  std::snprintf(pidText, sizeof(pidText), "0x%04X", service.pmtPid);
  out.push_back(std::string("PMT PID: ") + pidText);
  std::snprintf(pidText, sizeof(pidText), "0x%04X", service.pcrPid);
  out.push_back(std::string("PCR PID: ") + pidText);
  out.push_back("流数  : " + std::to_string(service.streamPids.size()));
  out.push_back("PID 列表:");
  for (const std::uint16_t pid : service.streamPids) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "  0x%04X", pid);
    out.push_back(buffer);
  }
  out.push_back("EPG 事件: " + std::to_string(service.events.size()) +
                " 条（按 E 查看节目单）");
  for (std::string& line : out) {
    line = fit(line, width);
  }
  return out;
}

std::vector<std::string> TuiUi::consoleRightLines(int width, int height) {
  std::vector<std::string> lines =
      rightTab_ == RightTab::kOutput ? outputPaneLines(width)
                                     : detailPaneLines(width);
  if (lines.size() > static_cast<std::size_t>(height)) {
    const std::size_t maxTop = lines.size() - static_cast<std::size_t>(height);
    if (paneTop_ > maxTop) {
      paneTop_ = maxTop;
    }
    lines = std::vector<std::string>(lines.begin() +
                                         static_cast<std::ptrdiff_t>(paneTop_),
                                     lines.begin() +
                                         static_cast<std::ptrdiff_t>(paneTop_) +
                                         static_cast<std::ptrdiff_t>(height));
  }
  while (lines.size() < static_cast<std::size_t>(height)) {
    lines.push_back(fit("", width));
  }
  return lines;
}

Element TuiUi::renderConsoleBody(int width) {
  const Metrics metrics = computeMetrics();
  if (metrics.wide) {
    const int leftW = metrics.leftW;
    const int rightW = metrics.rightW;
    const std::vector<std::string> left =
        consoleLeftLines(leftW, metrics.leftRows);
    const std::vector<std::string> right =
        consoleRightLines(rightW, metrics.rightRows);

    const std::size_t rows =
        std::max(left.size(), right.size());
    Elements frame;
    const std::size_t listStart = 2;  // pane title + column header
    for (std::size_t index = 0; index < rows; ++index) {
      std::string leftLine = index < left.size() ? left[index] : "";
      const std::string rightLine = index < right.size() ? right[index] : "";

      const bool cursorRow =
          !model_.services().empty() && index >= listStart &&
          index - listStart == svcCursor_ - svcTop_;
      Element leftEl = ftxui::text(fit(leftLine, leftW));
      if (cursorRow) {
        leftEl = leftEl | ftxui::bgcolor(Color::GrayDark);
      }
      Element rightEl = ftxui::text(fit(rightLine, rightW));
      frame.push_back(ftxui::hbox(
          {leftEl, ftxui::color(Color::GrayDark, ftxui::text(" ")),
           rightEl}));
    }
    return ftxui::vbox(frame);
  }

  // Narrow terminal: stack services above the right-hand pane.
  const int leftRows = std::max(4, metrics.leftRows);
  const std::vector<std::string> left = consoleLeftLines(width, leftRows);
  const std::vector<std::string> right =
      consoleRightLines(width, metrics.rightRows);

  Elements frame;
  const std::size_t listStart = 2;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const bool cursorRow =
        !model_.services().empty() && index >= listStart &&
        index - listStart == svcCursor_ - svcTop_;
    Element element = ftxui::text(left[index]);
    if (cursorRow) {
      element = element | ftxui::bgcolor(Color::GrayDark);
    }
    frame.push_back(element);
  }
  frame.push_back(ftxui::color(Color::GrayDark,
                               ftxui::text(fit("", width))));
  for (const std::string& line : right) {
    frame.push_back(ftxui::text(line));
  }
  return ftxui::vbox(frame);
}

// ---- EPG page ------------------------------------------------------------------

std::vector<std::string> TuiUi::epgLines(int width, int height) {
  std::vector<std::string> out;
  const std::vector<MuxService> services = model_.services();
  if (services.empty()) {
    out.push_back(fit("没有节目表…（先锁定并进入控制台）", width));
    while (out.size() < static_cast<std::size_t>(height)) {
      out.push_back(fit("", width));
    }
    return out;
  }

  const std::size_t index = std::min(epgService_, services.size() - 1);
  const MuxService& service = services[index];
  const std::string title =
      "-- 节目单: " + service.name + "（prog " +
      std::to_string(service.programNumber) + "）--";
  out.push_back(fit(title, width));

  const int rows = std::max(0, height - 1);
  if (service.events.empty()) {
    out.push_back(fit("本台暂无 EPG 事件；继续监视等待 EIT（PID 0x0012）…",
                      width));
    while (out.size() < static_cast<std::size_t>(height)) {
      out.push_back(fit("", width));
    }
    return out;
  }

  if (epgScroll_ >= service.events.size()) {
    epgScroll_ = service.events.size() - 1;
  }
  std::size_t shown = 0;
  for (std::size_t eventIndex = epgScroll_;
       eventIndex < service.events.size() &&
       shown < static_cast<std::size_t>(rows);
       ++eventIndex, ++shown) {
    const EpgEvent& event = service.events[eventIndex];
    std::string line = epgTimeText(event.startUtc);
    line += "  " + epgDurationText(event.durationSec) + "  ";
    if (event.name.empty()) {
      line += "(无名称)";
    } else {
      line += event.name;
    }
    if (eventIndex == epgScroll_) {
      line = "> " + line;
    } else {
      line = "  " + line;
    }
    out.push_back(fit(line, width));
  }
  while (out.size() < static_cast<std::size_t>(height)) {
    out.push_back(fit("", width));
  }
  return out;
}

Element TuiUi::renderEpgBody(int width, int height) {
  const std::vector<std::string> lines = epgLines(width, height);
  Elements rows;
  rows.reserve(lines.size());
  for (const std::string& line : lines) {
    rows.push_back(ftxui::text(line));
  }
  return ftxui::vbox(rows);
}

// ---- debug page -----------------------------------------------------------------

Element TuiUi::renderDebugBody(int width, int height) const {
  Elements rows;
  const std::size_t available = static_cast<std::size_t>(std::max(1, height));
  const std::size_t total = commandLines_.size();
  const std::size_t end =
      total > debugTop_ ? total - debugTop_ : total;  // newest at bottom
  const std::size_t start = end > available ? end - available : 0;
  for (std::size_t index = start; index < end; ++index) {
    rows.push_back(ftxui::text(fit(commandLines_[index], width)));
  }
  while (rows.size() < available) {
    rows.push_back(ftxui::text(fit("", width)));
  }
  return ftxui::vbox(rows);
}

}  // namespace

int runTui(const Options& options, RegLogger& regLogger,
           std::ostream& statusLog) {
  (void)regLogger;

  TuiModel model;
  TuiEngine engine(options, &regLogger, &statusLog, model);
  model.replaceFrequencies(defaultFrequencyList());

  // FTXUI draws through std::cout, but the backend also prints plain
  // diagnostics there.  Route stdout through a tee that forwards FTXUI's
  // escape output verbatim and captures plain text for the status row.
  StdoutTee tee(std::cout.rdbuf());
  std::streambuf* originalOutput = std::cout.rdbuf(&tee);

  std::ifstream commandLog(options.regLogPath);
  engine.start();

  ftxui::App app = ftxui::App::Fullscreen();

  TuiUi ui(options, model, engine, commandLog, &tee, [&app] { app.Exit(); });

  auto component = ftxui::Renderer([&ui] { return ui.Render(); });
  component = ftxui::CatchEvent(
      component, [&ui](const Event& event) { return ui.OnEvent(event); });

  std::atomic<bool> keepRefreshing{true};
  std::thread refresher([&app, &keepRefreshing] {
    while (keepRefreshing.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      app.PostEvent(Event::Custom);
    }
  });

  app.Loop(component);
  keepRefreshing.store(false);
  if (refresher.joinable()) {
    refresher.join();
  }

  engine.requestQuit();
  engine.wait();
  std::cout.rdbuf(originalOutput);

  if (model.fatal()) {
    std::cerr << "error: " << model.errorText() << '\n';
  }
  return model.fatal() ? 1 : 0;
}

}  // namespace lme2510::tui
