#include "trdp_simulator/config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace trdp_sim {

namespace {
std::vector<std::uint8_t> from_hex(const std::string &hex)
{
    std::string sanitized;
    sanitized.reserve(hex.size());
    for (unsigned char c : hex) {
        if (!std::isspace(c)) {
            sanitized.push_back(static_cast<char>(c));
        }
    }

    std::vector<std::uint8_t> data;
    data.reserve(sanitized.size() / 2);
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Invalid hex character: " + std::string(1, c));
    };
    if (sanitized.size() % 2 != 0) {
        throw std::runtime_error("Hex payload must contain an even number of characters");
    }
    for (std::size_t i = 0; i < sanitized.size(); i += 2) {
        const int high = hex_value(sanitized[i]);
        const int low = hex_value(sanitized[i + 1]);
        data.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return data;
}

std::vector<std::uint8_t> from_text(const std::string &text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> from_file(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open payload file: " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const auto stringData = buffer.str();
    return std::vector<std::uint8_t>(stringData.begin(), stringData.end());
}
}  // namespace

std::vector<std::uint8_t> load_payload(const PayloadConfig &payload)
{
    switch (payload.format) {
    case PayloadConfig::Format::Hex:
        return from_hex(payload.value);
    case PayloadConfig::Format::Text:
        return from_text(payload.value);
    case PayloadConfig::Format::File:
        return from_file(payload.value);
    }
    throw std::runtime_error("Unsupported payload format");
}

}  // namespace trdp_sim
