#pragma once

#include <string>

#include "trdp_simulator/config.hpp"

namespace trdp_sim {

SimulatorConfig load_configuration(const std::string &path);
void validate_configuration(const SimulatorConfig &config);

}  // namespace trdp_sim
