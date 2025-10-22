#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace trdp_sim {

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path base_directory);

    std::vector<std::string> list() const;
    bool exists(const std::string &name) const;
    std::string load_xml(const std::string &name) const;
    void save(const std::string &name, const std::string &xml) const;
    std::filesystem::path path_for(const std::string &name) const;

    static bool is_valid_name(const std::string &name);

private:
    std::filesystem::path base_directory_;
};

}  // namespace trdp_sim
