#include "trdp_simulator/config.hpp"

#include <iostream>
#include <vector>

int main()
{
    using namespace trdp_sim;

    PayloadConfig payload;
    payload.format = PayloadConfig::Format::Hex;
    payload.value = "0A 0b\n0C\t0d";

    try {
        const auto data = load_payload(payload);
        const std::vector<std::uint8_t> expected{0x0A, 0x0B, 0x0C, 0x0D};
        if (data != expected) {
            std::cerr << "Whitespace was not ignored when parsing hex payload" << std::endl;
            return 1;
        }
    } catch (const std::exception &ex) {
        std::cerr << "Parsing hex payload with whitespace failed: " << ex.what() << std::endl;
        return 1;
    }

    payload.value = "0G";
    try {
        (void)load_payload(payload);
        std::cerr << "Invalid hex payload was accepted" << std::endl;
        return 1;
    } catch (const std::exception &) {
        // Expected path
    }

    return 0;
}
