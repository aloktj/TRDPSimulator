#include "trdp_simulator/config_loader.hpp"

#include <iostream>

int run_config_loader_test()
{
    using namespace trdp_sim;

    const char *xml = R"XML(<?xml version="1.0"?>
<trdpSimulator>
  <network interface="eth0" hostIp="10.0.0.1" />
  <pd>
    <publisher name="Pub" comId="100" cycleTimeMs="500">
      <payload format="hex">0A0B</payload>
    </publisher>
  </pd>
</trdpSimulator>
)XML";

    try {
        auto config = load_configuration_from_string(xml);
        if (config.network.interfaceName != "eth0") {
            std::cerr << "Unexpected interface name: " << config.network.interfaceName << std::endl;
            return 1;
        }
        if (config.pdPublishers.size() != 1 || config.pdPublishers.front().payload.format != PayloadConfig::Format::Hex) {
            std::cerr << "Configuration did not parse PD publisher correctly" << std::endl;
            return 1;
        }
        validate_configuration(config);
    } catch (const std::exception &ex) {
        std::cerr << "Configuration parsing failed: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
