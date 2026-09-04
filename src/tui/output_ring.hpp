#pragma once

#include <cstddef>
#include <deque>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

namespace lme2510::tui {

/// Thread-safe std::streambuf that captures complete lines written through an
/// ostream.  Used while the ncurses screen is active so backend threads can
/// still use std::cout without corrupting the alternate screen.
class OutputRing : public std::streambuf {
 public:
  explicit OutputRing(std::size_t maxLines = 600);

  /// Replaces the stream's buffer.  Returns the previous buffer for detach().
  std::streambuf* attach(std::ostream& stream);
  void detach(std::ostream& stream, std::streambuf* previous);

  std::vector<std::string> takeNew();
  std::string latest() const;

 protected:
  int_type overflow(int_type character) override;
  std::streamsize xsputn(const char* text, std::streamsize count) override;

 private:
  void appendBytes(const char* text, std::size_t count);
  void appendLine(const std::string& line);

  std::size_t maxLines_;
  std::string pending_;
  std::deque<std::string> lines_;
  mutable std::mutex mutex_;
};

}  // namespace lme2510::tui
