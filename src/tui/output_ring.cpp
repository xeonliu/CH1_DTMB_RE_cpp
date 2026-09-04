#include "output_ring.hpp"

#include <algorithm>

namespace lme2510::tui {

OutputRing::OutputRing(std::size_t maxLines) : maxLines_(maxLines) {}

std::streambuf* OutputRing::attach(std::ostream& stream) {
  std::lock_guard<std::mutex> lock(mutex_);
  return stream.rdbuf(this);
}

void OutputRing::detach(std::ostream& stream, std::streambuf* previous) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stream.rdbuf() == this) {
    stream.rdbuf(previous);
  }
}

std::vector<std::string> OutputRing::takeNew() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out(lines_.begin(), lines_.end());
  lines_.clear();
  return out;
}

std::string OutputRing::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lines_.empty() ? std::string() : lines_.back();
}

OutputRing::int_type OutputRing::overflow(int_type character) {
  if (character != traits_type::eof()) {
    const char byte = static_cast<char>(character);
    appendBytes(&byte, 1);
  }
  return traits_type::not_eof(character);
}

std::streamsize OutputRing::xsputn(const char* text, std::streamsize count) {
  if (count > 0) {
    appendBytes(text, static_cast<std::size_t>(count));
  }
  return count;
}

void OutputRing::appendBytes(const char* text, std::size_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t start = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (text[index] == '\n') {
      pending_.append(text + start, index - start);
      appendLine(pending_);
      pending_.clear();
      start = index + 1;
    }
  }
  if (start < count) {
    pending_.append(text + start, count - start);
    if (pending_.size() > 4096) {
      appendLine(pending_);
      pending_.clear();
    }
  }
}

void OutputRing::appendLine(const std::string& line) {
  std::string trimmed = line;
  while (!trimmed.empty() &&
         (trimmed.back() == '\r' || trimmed.back() == ' ')) {
    trimmed.pop_back();
  }
  lines_.push_back(trimmed);
  if (lines_.size() > maxLines_) {
    lines_.pop_front();
  }
}

}  // namespace lme2510::tui
