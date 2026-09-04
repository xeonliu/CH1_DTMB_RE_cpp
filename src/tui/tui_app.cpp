#include "tui_app.hpp"

#include <ncurses.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lme2510/tui/tui_engine.hpp"
#include "lme2510/tui/tui_model.hpp"
#include "output_ring.hpp"

namespace lme2510::tui {

namespace {

enum class View { kFrequencies, kMonitor, kServices, kEpg, kCommands };

std::vector<int> defaultFrequencyList() {
  std::vector<int> out;
  for (int mhz = 474; mhz <= 858; mhz += 8) {
    out.push_back(mhz);
  }
  return out;
}

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

void clearLine(int y) {
  if (y >= 0 && y < LINES) {
    move(y, 0);
    clrtoeol();
  }
}

void drawBar(int y, int x, int width, int percent) {
  move(y, x);
  clrtoeol();
  if (width <= 0) {
    return;
  }
  const int fill = (percent * width) / 100;
  for (int index = 0; index < width; ++index) {
    addch(index < fill ? '#' : '-');
  }
}

void drawTitleAndStatus(View view) {
  const char* title = nullptr;
  switch (view) {
    case View::kFrequencies:
      title = "1 Frequencies";
      break;
    case View::kMonitor:
      title = "2 Monitor / Stream";
      break;
    case View::kServices:
      title = "3 Services";
      break;
    case View::kEpg:
      title = "4 EPG";
      break;
    case View::kCommands:
      title = "5 Commands";
      break;
  }
  attrset(A_BOLD);
  clearLine(0);
  mvprintw(0, 1, "lme2510_stream --tui   [%s]", title);
  attroff(A_BOLD);
  move(1, 0);
  clrtoeol();
  mvhline(1, 0, '-', COLS);
}

void drawHelpLine(int y, const char* text) {
  clearLine(y);
  if (y >= 0 && y < LINES) {
    mvaddstr(y, 1, text);
  }
}

void drawFrequencies(const TuiModel& model, std::size_t cursor) {
  const std::vector<FreqEntry> entries = model.frequencies();
  if (entries.empty()) {
    return;
  }
  if (cursor >= entries.size()) {
    cursor = entries.size() - 1;
  }

  int y = 3;
  attrset(A_BOLD);
  clearLine(y);
  mvprintw(y, 1, "%-5s %-9s %-10s %-7s %-7s %-12s %s",
           "scan", "freq", "state", "str", "qual", "rate", "CC loss");
  attroff(A_BOLD);
  ++y;

  const int lastRow = LINES - 4;
  for (std::size_t index = 0; index < entries.size() && y < lastRow;
       ++index, ++y) {
    const FreqEntry& entry = entries[index];
    const bool highlighted = index == cursor;
    clearLine(y);
    if (highlighted) {
      attrset(A_REVERSE);
    }

    std::string state;
    if (entry.scanned) {
      state = entry.locked ? "LOCKED" : "no-lock";
    } else {
      state = entry.checked ? "selected" : "";
    }
    if (entry.current) {
      state = "CURRENT";
    }

    const std::string ccText =
        entry.scanned
            ? std::to_string(entry.ccErrors) + "/" +
                  std::to_string(entry.ccLost)
            : "-";
    const std::string rate =
        entry.scanned ? rateText(entry.rateMbps) : std::string();

    mvprintw(y, 1, "%c    %-9d %-10s %-7s %-7s %-12s %s",
             entry.checked ? 'x' : ' ', entry.mhz, state.c_str(),
             entry.scanned ? percentText(entry.strengthPct).c_str() : "-",
             entry.scanned ? percentText(entry.qualityPct).c_str() : "-",
             rate.c_str(), ccText.c_str());
    if (!entry.note.empty() && COLS > 62) {
      const std::string note = " " + entry.note;
      mvaddstr(y, COLS - static_cast<int>(note.size()) - 2,
               note.c_str());
    }
    if (highlighted) {
      attroff(A_REVERSE);
    }
  }

  drawHelpLine(LINES - 3,
               "Space: toggle scan-set   A: all   S: scan marked   "
               "Enter: tune+monitor this freq");
  drawHelpLine(LINES - 2,
               "Tab: switch window   Q: quit   Up/Down: move");
}

std::string epgClock(std::uint64_t utc) {
  if (utc == 0) {
    return "-- --:--";
  }
  const std::time_t when = static_cast<std::time_t>(utc);
  struct tm local {};
  localtime_r(&when, &local);
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local);
  return buffer;
}

std::string epgDuration(std::uint32_t seconds) {
  if (seconds == 0) {
    return "";
  }
  const unsigned int hours = seconds / 3600;
  const unsigned int minutes = (seconds % 3600) / 60;
  char buffer[16];
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%uh%02um", hours, minutes);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%um", minutes);
  }
  return buffer;
}

/// Truncates a UTF-8 string to at most width bytes without splitting a
/// multi-byte character at the tail.
std::string fitUtf8(const std::string& text, std::size_t width) {
  if (text.size() <= width) {
    return text;
  }
  std::size_t end = width;
  while (end > 0 && (static_cast<std::uint8_t>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

void drawServices(const TuiModel& model, std::size_t cursor,
                  int currentMhz) {
  const std::vector<MuxService> services = model.services();
  const ServiceSelectionSnapshot selected = model.selectedService();

  clearLine(2);
  mvprintw(2, 1, "%d MHz  -  %zu service(s) found", currentMhz,
           services.size());
  if (selected.active) {
    attrset(A_BOLD);
    mvprintw(2, COLS > 42 ? COLS - 30 : 24,
             "selected: %s", selected.name.c_str());
    attroff(A_BOLD);
  }

  if (services.empty()) {
    mvprintw(5, 1,
             "Waiting for PAT/PMT/SDT tables. This usually takes 1-2 "
             "seconds on a locked channel.");
    drawHelpLine(LINES - 3, "F: frequency list   Tab: switch   Q: quit");
    drawHelpLine(LINES - 2,
                 "No service tables yet - check lock/signal in Monitor.");
    return;
  }

  if (cursor >= services.size()) {
    cursor = services.size() - 1;
  }
  int y = 4;
  attrset(A_BOLD);
  clearLine(y);
  mvprintw(y, 1, "%-7s %-9s %-7s %-7s %-8s %s", "sel", "prog", "pmt",
           "pcr", "pids", "name");
  attroff(A_BOLD);
  ++y;

  const int lastRow = LINES - 4;
  for (std::size_t index = 0; index < services.size() && y < lastRow;
       ++index, ++y) {
    const MuxService& service = services[index];
    const bool highlighted = index == cursor;
    const bool current = selected.active &&
                         selected.programNumber == service.programNumber;
    clearLine(y);
    if (highlighted) {
      attrset(A_REVERSE);
    }
    mvprintw(y, 1, "%-7s %-9u 0x%-5X 0x%-5X %-8zu %s",
             current ? "*" : "",
             static_cast<unsigned>(service.programNumber),
             static_cast<unsigned>(service.pmtPid),
             static_cast<unsigned>(service.pcrPid), service.streamPids.size(),
             service.name.c_str());
    if (highlighted) {
      attroff(A_REVERSE);
    }
  }

  drawHelpLine(LINES - 3,
               "Enter: stream/record this service   E: EPG   C: clear mux");
  drawHelpLine(LINES - 2, "F: frequency list   Tab: switch window   Q: quit");
}

void drawEpg(const TuiModel& model, std::size_t serviceIndex,
             std::size_t scroll) {
  const std::vector<MuxService> services = model.services();
  const ServiceSelectionSnapshot selected = model.selectedService();
  clearLine(2);
  if (services.empty()) {
    mvprintw(2, 1,
             "No service tables yet - check lock/signal in Monitor "
             "(waiting for PAT/SDT).");
    drawHelpLine(LINES - 3, "F: frequency list   Tab: switch   Q: quit");
    drawHelpLine(LINES - 2,
                 "No EPG yet - keep monitoring so EIT sections arrive.");
    return;
  }
  if (serviceIndex >= services.size()) {
    serviceIndex = services.size() - 1;
  }
  const MuxService& service = services[serviceIndex];
  const bool streaming = selected.active &&
                         selected.programNumber == service.programNumber;

  char header[600];
  std::snprintf(header, sizeof(header), "%d MHz - %s (prog %u, %s)   %zu "
                                        "event(s)%s",
                model.currentMhz(), service.name.c_str(),
                static_cast<unsigned>(service.programNumber),
                service.typeName.c_str(), service.events.size(),
                streaming ? "   [streaming]" : "");
  attrset(A_BOLD);
  if (COLS > 2) {
    mvaddnstr(2, 1,
              fitUtf8(header, static_cast<std::size_t>(COLS - 2)).c_str(),
              COLS - 2);
  }
  attroff(A_BOLD);

  const std::time_t now = std::time(nullptr);
  if (service.events.empty()) {
    mvprintw(5, 1,
             "No EIT/EPG data for this service yet.  Keep monitoring so the "
             "EIT table arrives.");
    drawHelpLine(LINES - 3,
                 "<-/-> channel   C: services   F: frequencies");
    drawHelpLine(LINES - 2, "Tab: switch window   Q: quit");
    return;
  }

  const std::size_t rows =
      static_cast<std::size_t>(std::max(0, LINES - 7));
  const std::size_t safeScroll =
      std::min(scroll, service.events.size() > rows
                           ? service.events.size() - rows
                           : std::size_t(0));
  const std::size_t end = service.events.size();
  const std::size_t start = safeScroll;

  clearLine(3);
  attrset(A_BOLD);
  mvprintw(3, 1, "%-5s %-13s %-8s %s", "", "start (local)", "len",
           "programme");
  attroff(A_BOLD);
  int y = 4;
  for (std::size_t index = start; index < end && y < LINES - 4;
       ++index, ++y) {
    const EpgEvent& event = service.events[index];
    const bool onAir =
        event.startUtc <= static_cast<std::uint64_t>(now) &&
        now < static_cast<std::time_t>(event.startUtc + event.durationSec);
    clearLine(y);
    if (onAir) {
      attrset(A_BOLD);
    }
    mvprintw(y, 1, "%-5s", onAir ? "NOW" : "");
    mvprintw(y, 6, "%-13s", epgClock(event.startUtc).c_str());
    mvprintw(y, 20, "%-8s", epgDuration(event.durationSec).c_str());
    const int nameX = 29;
    if (nameX < COLS) {
      mvaddnstr(y, nameX,
                fitUtf8(event.name, static_cast<std::size_t>(COLS - nameX - 1))
                    .c_str(),
                COLS - nameX - 1);
    }
    if (onAir) {
      attroff(A_BOLD);
    }
  }

  drawHelpLine(LINES - 3,
               "<-/-> channel   Up/Down: scroll   Enter: play service   "
               "C: services");
  drawHelpLine(LINES - 2, "F: frequency list   Tab: switch window   Q: quit");
}

void drawMonitor(const TuiModel& model, const Options& options) {
  const int current = model.currentMhz();
  const bool streaming = model.streaming();
  const bool recording = model.recording();
  const SignalSnapshot signal = model.signal();
  const TelemetrySnapshot telemetry = model.telemetry();
  const TsMetricsSnapshot stats = model.counters();
  const ServiceSelectionSnapshot selection = model.selectedService();

  clearLine(2);
  attrset(A_BOLD);
  std::string mode = streaming ? "STREAMING" : "monitoring";
  if (recording) {
    mode += "+REC";
  }
  mvprintw(2, 1,
           "Frequency: %d MHz   Mode: %s   TS %s   Out %s",
           current, mode.c_str(),
           rateText(stats.rateMbps).c_str(),
           rateText(stats.outputRateMbps).c_str());
  attroff(A_BOLD);
  const std::vector<MuxService> services = model.services();
  if (selection.active) {
    mvprintw(3, 1, "Service: %s (program %u, PMT 0x%X, %zu found)",
             selection.name.c_str(),
             static_cast<unsigned>(selection.programNumber),
             static_cast<unsigned>(selection.pmtPid), services.size());
  } else {
    mvprintw(3, 1, "Service: whole multiplex (all PIDs, %zu found)",
             services.size());
  }

  int y = 4;
  if (signal.has) {
    mvprintw(y++, 1, "Lock: %-3s   Strength: %s   Quality: %s",
             signal.lock ? "YES" : "NO",
             percentText(signal.strengthPct).c_str(),
             percentText(signal.qualityPct).c_str());
    mvprintw(y++, 1, "EP8A signal=%s snr=%s hi=%s lo=%s",
             hexText(signal.signalRaw).c_str(),
             hexText(signal.snrRaw).c_str(), hexText(signal.hi).c_str(),
             hexText(signal.lo).c_str());
    ++y;
    const int barWidth = COLS > 60 ? 38 : 20;
    mvprintw(y, 1, "signal ");
    drawBar(y, 8, barWidth, signal.strengthPct);
    mvprintw(y, 9 + barWidth, " %s", percentText(signal.strengthPct).c_str());
    ++y;
    mvprintw(y, 1, "quality");
    drawBar(y, 8, barWidth, signal.qualityPct);
    mvprintw(y, 9 + barWidth, " %s", percentText(signal.qualityPct).c_str());
    ++y;
  } else {
    mvprintw(y++, 1, "Waiting for EP 0x8A status packets...");
  }
  ++y;

  const double lossPct = stats.lossPct;
  mvprintw(y++, 1,
           "TS bytes=%-10llu packets=%-10llu frames=%-8llu timeouts=%llu",
           static_cast<unsigned long long>(stats.bytes),
           static_cast<unsigned long long>(stats.packets),
           static_cast<unsigned long long>(stats.frames),
           static_cast<unsigned long long>(stats.timeouts));
  mvprintw(y++, 1,
           "resyncs=%-5llu dropped=%-8llu CC_errors=%-8llu CC_lost=%llu",
           static_cast<unsigned long long>(stats.resyncs),
           static_cast<unsigned long long>(stats.droppedBytes),
           static_cast<unsigned long long>(stats.ccErrors),
           static_cast<unsigned long long>(stats.ccLost));
  mvprintw(y++, 1,
           "transport_errors=%llu   loss_est=%.3f%%   status hits=%llu "
           "misses=%llu",
           static_cast<unsigned long long>(stats.transportErrors), lossPct,
           static_cast<unsigned long long>(signal.hits),
           static_cast<unsigned long long>(signal.misses));
  mvprintw(y++, 1,
           "UDP bytes=%llu dgrams=%llu   raw bytes=%llu dgrams=%llu",
           static_cast<unsigned long long>(stats.udpBytes),
           static_cast<unsigned long long>(stats.udpDatagrams),
           static_cast<unsigned long long>(stats.rawBytes),
           static_cast<unsigned long long>(stats.rawDatagrams));
  if (!options.noUdp) {
    mvprintw(y++, 1, "UDP target: %s", options.udpTarget.c_str());
  }
  if (!options.filePath.empty()) {
    mvprintw(y++, 1, "File: %s", options.filePath.c_str());
  } else if (recording && !model.recordPath().empty()) {
    mvprintw(y++, 1, "File: %s", model.recordPath().c_str());
  }
  if (telemetry.has) {
    mvprintw(y++, 1, "Registers: %.180s", telemetry.detail.c_str());
  }

  drawHelpLine(LINES - 3,
               streaming
                   ? "[SPACE] Stop stream   [R] Stop record   [C] Services"
                   : "[SPACE] Start stream  [R] Start record  [C] Services");
  drawHelpLine(LINES - 2,
               "F: frequency list   Tab: switch window   Q: quit");
}

void drawCommands(const TuiModel& model, const Options& options,
                  const std::deque<std::string>& lines,
                  std::size_t scrollOffset) {
  (void)model;
  const std::size_t rows =
      static_cast<std::size_t>(std::max(0, LINES - 5));
  const std::size_t safeScroll =
      std::min(scrollOffset, lines.size());
  const std::size_t shown = lines.size() > rows ? rows : lines.size();
  const std::size_t end = lines.size() - safeScroll;
  const std::size_t start = end > shown ? end - shown : 0;

  int y = 3;
  for (std::size_t index = start; index < end && y < LINES - 2; ++index, ++y) {
    clearLine(y);
    const std::string& line = lines[index];
    const std::size_t width =
        COLS > 2 ? static_cast<std::size_t>(COLS - 2) : 0;
    mvaddnstr(y, 1, line.c_str(), static_cast<int>(width));
  }

  std::string tailText = "register log: " + options.regLogPath;
  drawHelpLine(LINES - 2, tailText.c_str());
}

void refreshCommandsLog(std::ifstream& input,
                        std::deque<std::string>& lines) {
  if (!input.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() > 240) {
      line.resize(240);
      line += "...";
    }
    lines.push_back(line);
    if (lines.size() > 2000) {
      lines.pop_front();
    }
  }
}

std::string modelStatusLine(const TuiModel& model,
                            const std::string& programOutput) {
  if (model.fatal()) {
    return "FATAL: " + model.errorText();
  }
  if (!model.ready()) {
    return model.phaseText().empty() ? "initializing..." : model.phaseText();
  }
  std::string text = model.phaseText();
  if (!model.errorText().empty()) {
    text += "   [" + model.errorText() + "]";
  }
  if (!programOutput.empty()) {
    text += "   " + programOutput;
  }
  return text;
}

}  // namespace

int runTui(const Options& options, RegLogger& regLogger,
           std::ostream& statusLog) {
  (void)regLogger;
  OutputRing outputRing;
  std::streambuf* originalOutput = outputRing.attach(std::cout);

  TuiModel model;
  TuiEngine engine(options, &regLogger, &statusLog, model);
  model.replaceFrequencies(defaultFrequencyList());

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  timeout(100);
  nodelay(stdscr, FALSE);

  std::ifstream commandLog(options.regLogPath);
  engine.start();

  View view = View::kFrequencies;
  std::size_t cursor = 0;
  std::size_t serviceCursor = 0;
  std::size_t epgService = 0;
  std::size_t epgScroll = 0;
  std::size_t commandScroll = 0;
  std::deque<std::string> commandLines;
  bool pendingTune = false;
  bool pendingService = false;
  std::uint16_t pendingProgram = 0;
  bool quit = false;

  while (!quit) {
    refreshCommandsLog(commandLog, commandLines);

    erase();
    drawTitleAndStatus(view);
    const std::string status =
        modelStatusLine(model, outputRing.latest());
    drawHelpLine(LINES - 1, status.c_str());

    if (model.fatal()) {
      drawHelpLine(LINES - 4, "Device initialization failed.");
      drawHelpLine(LINES - 3, "Check the USB stick and permissions.");
      drawHelpLine(LINES - 2, "Q: quit");
    } else {
      switch (view) {
        case View::kFrequencies:
          drawFrequencies(model, cursor);
          break;
        case View::kMonitor:
          drawMonitor(model, options);
          break;
        case View::kServices:
          drawServices(model, serviceCursor, model.currentMhz());
          break;
        case View::kEpg:
          drawEpg(model, epgService, epgScroll);
          break;
        case View::kCommands:
          drawCommands(model, options, commandLines, commandScroll);
          break;
      }
    }
    refresh();

    const int key = getch();
    if (key == ERR) {
      continue;
    }
    if (key == 'q' || key == 'Q') {
      quit = true;
      break;
    }
    if (key == '\t') {
      const View startView = view;
      do {
        view = static_cast<View>((static_cast<int>(view) + 1) % 5);
      } while ((view == View::kMonitor || view == View::kServices ||
                view == View::kEpg) &&
               !model.monitoring() && view != startView);
      continue;
    }
    if (key == '1') {
      view = View::kFrequencies;
      continue;
    }
    if (key == '2' && model.monitoring()) {
      view = View::kMonitor;
      continue;
    }
    if (key == '3' && model.monitoring()) {
      view = View::kServices;
      continue;
    }
    if (key == '4' && model.monitoring()) {
      view = View::kEpg;
      epgScroll = 0;
      continue;
    }
    if (key == '5') {
      view = View::kCommands;
      commandScroll = 0;
      continue;
    }

    switch (view) {
      case View::kFrequencies: {
        if (model.busy() || !model.ready()) {
          break;
        }
        const std::vector<FreqEntry> entries = model.frequencies();
        if (entries.empty()) {
          break;
        }
        if (cursor >= entries.size()) {
          cursor = entries.size() - 1;
        }

        if (key == KEY_UP || key == 'k') {
          cursor = cursor == 0 ? entries.size() - 1 : cursor - 1;
        } else if (key == KEY_DOWN || key == 'j') {
          cursor = (cursor + 1) % entries.size();
        } else if (key == ' ' || key == 'x' || key == 'X') {
          const bool checked = !entries[cursor].checked;
          model.setChecked(cursor, checked);
        } else if (key == 'a' || key == 'A') {
          bool anyUnchecked = false;
          for (const FreqEntry& entry : entries) {
            anyUnchecked = anyUnchecked || !entry.checked;
          }
          for (std::size_t index = 0; index < entries.size(); ++index) {
            model.setChecked(index, anyUnchecked);
          }
        } else if (key == 's' || key == 'S') {
          std::vector<int> toScan;
          for (const FreqEntry& entry : entries) {
            if (entry.checked) {
              toScan.push_back(entry.mhz);
            }
          }
          if (toScan.empty()) {
            toScan.push_back(entries[cursor].mhz);
          }
          pendingTune = false;
          engine.requestScan(toScan);
        } else if (key == '\n' || key == KEY_ENTER || key == '\r') {
          pendingTune = true;
          engine.requestTune(entries[cursor].mhz);
        }
        break;
      }
      case View::kMonitor: {
        if (model.busy()) {
          break;
        }
        if (key == ' ') {
          engine.requestStream(!model.streaming());
        } else if (key == 'r' || key == 'R') {
          engine.requestRecord(!model.recording());
        } else if (key == 'c' || key == 'C' || key == '\n' ||
                   key == KEY_ENTER || key == '\r') {
          view = View::kServices;
        } else if (key == 'f' || key == 'F') {
          view = View::kFrequencies;
        }
        break;
      }
      case View::kServices: {
        if (model.busy() || !model.monitoring()) {
          break;
        }
        const std::vector<MuxService> services = model.services();
        if (services.empty()) {
          if (key == 'f' || key == 'F') {
            view = View::kFrequencies;
          }
          break;
        }
        if (serviceCursor >= services.size()) {
          serviceCursor = services.size() - 1;
        }
        if (key == KEY_UP || key == 'k') {
          serviceCursor =
              serviceCursor == 0 ? services.size() - 1 : serviceCursor - 1;
        } else if (key == KEY_DOWN || key == 'j') {
          serviceCursor = (serviceCursor + 1) % services.size();
        } else if (key == '\n' || key == KEY_ENTER || key == '\r' ||
                   key == ' ') {
          pendingService = true;
          pendingProgram = services[serviceCursor].programNumber;
          engine.requestSelectService(pendingProgram);
        } else if (key == 'e' || key == 'E') {
          epgService = serviceCursor;
          epgScroll = 0;
          view = View::kEpg;
        } else if (key == 'c' || key == 'C' || key == 27 /* ESC */) {
          engine.requestClearService();
          pendingService = false;
          view = View::kMonitor;
        } else if (key == 'f' || key == 'F') {
          view = View::kFrequencies;
        }
        break;
      }
      case View::kEpg: {
        if (model.busy() || !model.monitoring()) {
          break;
        }
        const std::vector<MuxService> services = model.services();
        if (services.empty()) {
          if (key == 'f' || key == 'F') {
            view = View::kFrequencies;
          }
          break;
        }
        if (epgService >= services.size()) {
          epgService = services.size() - 1;
        }
        const std::size_t eventCount =
            services[epgService].events.size();
        if (key == KEY_LEFT || key == 'h') {
          epgService = epgService == 0 ? services.size() - 1 : epgService - 1;
          epgScroll = 0;
        } else if (key == KEY_RIGHT || key == 'l') {
          epgService = (epgService + 1) % services.size();
          epgScroll = 0;
        } else if (key == KEY_UP || key == 'k') {
          epgScroll = std::min(epgScroll + 1, eventCount);
        } else if (key == KEY_DOWN || key == 'j') {
          epgScroll = epgScroll == 0 ? 0 : epgScroll - 1;
        } else if (key == '\n' || key == KEY_ENTER || key == '\r' ||
                   key == ' ') {
          pendingService = true;
          pendingProgram = services[epgService].programNumber;
          engine.requestSelectService(pendingProgram);
        } else if (key == 'c' || key == 'C' || key == 27 /* ESC */) {
          serviceCursor = epgService;
          view = View::kServices;
        } else if (key == 'f' || key == 'F') {
          view = View::kFrequencies;
        }
        break;
      }
      case View::kCommands: {
        if (key == KEY_UP) {
          commandScroll = std::min(
              commandScroll + 1, commandLines.size());
        } else if (key == KEY_DOWN) {
          commandScroll = commandScroll == 0 ? 0 : commandScroll - 1;
        } else if (key == KEY_PPAGE) {
          const std::size_t page =
              static_cast<std::size_t>(std::max(1, LINES - 8));
          commandScroll = std::min(commandScroll + page, commandLines.size());
        } else if (key == KEY_NPAGE) {
          const std::size_t page =
              static_cast<std::size_t>(std::max(1, LINES - 8));
          commandScroll = commandScroll > page ? commandScroll - page : 0;
        } else if (key == 'f' || key == 'F') {
          view = View::kFrequencies;
        }
        break;
      }
    }

    // Once a tuned frequency is being monitored, jump to the monitor view.
    if (pendingTune && model.monitoring() && model.currentMhz() > 0) {
      pendingTune = false;
      view = View::kMonitor;
    }
    if (pendingService && !model.busy() &&
        model.selectedService().active &&
        model.selectedService().programNumber == pendingProgram) {
      pendingService = false;
      view = View::kMonitor;
    }
  }

  engine.requestQuit();
  engine.wait();

  endwin();
  outputRing.detach(std::cout, originalOutput);

  if (model.fatal()) {
    std::cerr << "error: " << model.errorText() << '\n';
  }
  return model.fatal() ? 1 : 0;
}

}  // namespace lme2510::tui
