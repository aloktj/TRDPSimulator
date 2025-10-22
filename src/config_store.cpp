#include "trdp_simulator/config_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace trdp_sim {

namespace {
std::string sanitize_name(const std::string &name)
{
    std::string result(name);
    std::replace(result.begin(), result.end(), ' ', '_');
    return result;
}
}

ConfigStore::ConfigStore(std::filesystem::path base_directory)
    : base_directory_(std::move(base_directory))
{
    if (base_directory_.empty()) {
        base_directory_ = std::filesystem::current_path() / "config" / "library";
    }
    std::error_code ec;
    if (!std::filesystem::exists(base_directory_, ec)) {
        std::filesystem::create_directories(base_directory_, ec);
        if (ec) {
            throw std::runtime_error("Unable to create configuration directory: " + base_directory_.string());
        }
    }
}

std::vector<std::string> ConfigStore::list() const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(base_directory_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto filename = entry.path().filename();
        if (filename.extension() != ".xml") {
            continue;
        }
        names.push_back(filename.stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ConfigStore::exists(const std::string &name) const
{
    if (!is_valid_name(name)) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path_for(name), ec);
}

std::string ConfigStore::load_xml(const std::string &name) const
{
    if (!is_valid_name(name)) {
        throw std::runtime_error("Invalid configuration name: " + name);
    }
    std::ifstream stream(path_for(name));
    if (!stream) {
        throw std::runtime_error("Configuration not found: " + name);
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void ConfigStore::save(const std::string &name, const std::string &xml) const
{
    if (!is_valid_name(name)) {
        throw std::runtime_error("Invalid configuration name: " + name);
    }
    auto target = path_for(name);
    std::ofstream stream(target);
    if (!stream) {
        throw std::runtime_error("Unable to write configuration: " + target.string());
    }
    stream << xml;
    stream.flush();
    if (!stream) {
        throw std::runtime_error("Failed to write configuration: " + target.string());
    }
}

std::filesystem::path ConfigStore::path_for(const std::string &name) const
{
    return base_directory_ / (sanitize_name(name) + ".xml");
}

bool ConfigStore::is_valid_name(const std::string &name)
{
    if (name.empty()) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

}  // namespace trdp_sim
