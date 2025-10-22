#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

#include "trdp_simulator/web_application.hpp"

namespace {
trdp_sim::WebApplication *gWebApp = nullptr;

void signal_handler(int)
{
    if (gWebApp != nullptr) {
        gWebApp->request_stop();
    }
}

void print_usage(const char *program)
{
    std::cout << "Usage: " << program << " [--host <address>] [--port <port>]" << std::endl;
    std::cout << "  --host, -H   IPv4 address to bind (default 0.0.0.0)" << std::endl;
    std::cout << "  --port, -p   TCP port to listen on (default 8080)" << std::endl;
}
}  // namespace

int main(int argc, char **argv)
{
    std::string host = "0.0.0.0";
    unsigned short port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "--host" || arg == "-H") && i + 1 < argc) {
            host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            int value = 0;
            try {
                value = std::stoi(argv[++i]);
            } catch (const std::exception &) {
                std::cerr << "Port must be a valid integer" << std::endl;
                return 1;
            }
            if (value < 0 || value > 65535) {
                std::cerr << "Port must be between 0 and 65535" << std::endl;
                return 1;
            }
            port = static_cast<unsigned short>(value);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    try {
        trdp_sim::WebApplication app(host, port);
        gWebApp = &app;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::cout << "TRDP Simulator web interface listening on " << host << ':' << port << std::endl;
        app.run();
        gWebApp = nullptr;
    } catch (const std::exception &ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
