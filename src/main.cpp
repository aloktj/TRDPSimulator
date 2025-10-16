#include <csignal>
#include <iostream>
#include <string>

#include "trdp_simulator/config_loader.hpp"
#include "trdp_simulator/simulator.hpp"
#include "trdp_simulator/trdp_stack_adapter.hpp"

namespace {
trdp_sim::Simulator *gSimulator = nullptr;

void signal_handler(int)
{
    if (gSimulator != nullptr) {
        gSimulator->stop();
    }
}

void print_usage(const char *program)
{
    std::cerr << "Usage: " << program << " --config <path>" << std::endl;
}
}  // namespace

int main(int argc, char **argv)
{
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (configPath.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    try {
#ifdef TRDP_STACK_VERSION
        std::cout << "TRDP Simulator targeting TRDP stack version " << TRDP_STACK_VERSION;
#ifdef TRDPSIM_WITH_TRDP
        std::cout << " (real stack)";
#else
        std::cout << " (stub adapter)";
#endif
        std::cout << std::endl;
#endif
        auto config = trdp_sim::load_configuration(configPath);
        auto adapter = trdp_sim::create_trdp_stack_adapter();
        trdp_sim::Simulator simulator(std::move(config), std::move(adapter));
        gSimulator = &simulator;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        simulator.run();
        gSimulator = nullptr;
    } catch (const std::exception &ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
