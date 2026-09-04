#pragma once

#include <iosfwd>

#include "lme2510/util/arg_parser.hpp"

namespace lme2510 {

class RegLogger;

namespace tui {

/// Runs the full-screen FTXUI interface.  Only built/available on non-Windows
/// platforms (macOS and Linux).
int runTui(const Options& options, RegLogger& regLogger,
           std::ostream& statusLog);

}  // namespace tui
}  // namespace lme2510
