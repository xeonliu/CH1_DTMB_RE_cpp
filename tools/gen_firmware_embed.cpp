#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readBinary(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open firmware: " + path);
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0) {
    throw std::runtime_error("firmware is empty: " + path);
  }
  input.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  if (!input) {
    throw std::runtime_error("failed to read firmware: " + path);
  }
  return data;
}

void writeByteArray(std::ofstream& out, const std::string& symbol,
                    const std::vector<uint8_t>& data) {
  out << "inline constexpr std::array<uint8_t, " << data.size() << "> "
      << symbol << " = {\n";
  for (std::size_t i = 0; i < data.size(); ++i) {
    out << "0x" << std::uppercase << std::hex
        << static_cast<unsigned int>(data[i]) << std::nouppercase << std::dec;
    if (i + 1 != data.size()) {
      out << ',';
    }
    if ((i + 1) % 16 == 0) {
      out << '\n';
    }
  }
  if (data.size() % 16 != 0) {
    out << '\n';
  }
  out << "};\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 8) {
    std::cerr
        << "Usage: gen_firmware_embed <output.h> <fw1> <sym1> <fw2> <sym2> "
           "<fw3> <sym3>\n";
    return 2;
  }

  try {
    const std::string outputPath = argv[1];
    const std::array<std::string, 3> paths = {argv[2], argv[4], argv[6]};
    const std::array<std::string, 3> symbols = {argv[3], argv[5], argv[7]};

    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(3);
    for (const auto& path : paths) {
      blobs.push_back(readBinary(path));
    }

    std::ofstream out(outputPath, std::ios::trunc);
    if (!out) {
      throw std::runtime_error("cannot write header: " + outputPath);
    }
    out << "#pragma once\n"
        << "#include <array>\n"
        << "#include <cstdint>\n"
        << "#include <cstddef>\n\n"
        << "namespace lme2510 {\nnamespace embedded {\n\n";
    for (int i = 0; i < 3; ++i) {
      writeByteArray(out, symbols[i], blobs[static_cast<std::size_t>(i)]);
      out << '\n';
    }
    out << "}  // namespace embedded\n}  // namespace lme2510\n";

    std::cout << "Embedded firmware: " << paths[0] << " ("
              << blobs[0].size() << " B), " << paths[1] << " ("
              << blobs[1].size() << " B), " << paths[2] << " ("
              << blobs[2].size() << " B)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "gen_firmware_embed: " << error.what() << '\n';
    return 1;
  }
}
