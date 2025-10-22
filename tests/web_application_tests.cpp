#include "trdp_simulator/web_application.hpp"

#include <iostream>

namespace trdp_sim {

int run_web_application_tests()
{
    using trdp_sim::WebApplication;

    const std::string encoded_name = "name=demo%20config";
    auto value = WebApplication::extract_parameter(encoded_name, "name");
    if (value != "demo config") {
        std::cerr << "Failed to decode space in query parameter: '" << value << "'\n";
        return 1;
    }

    const std::string encoded_spec = "config=saved%3Aexample";
    value = WebApplication::extract_parameter(encoded_spec, "config");
    if (value != "saved:example") {
        std::cerr << "Failed to decode colon in query parameter: '" << value << "'\n";
        return 1;
    }

    const std::string encoded_path = "config=/etc/trdp%20configs/main.xml";
    value = WebApplication::extract_parameter(encoded_path, "config");
    if (value != "/etc/trdp configs/main.xml") {
        std::cerr << "Failed to decode path parameter: '" << value << "'\n";
        return 1;
    }

    value = WebApplication::extract_parameter("missing=value", "other");
    if (!value.empty()) {
        std::cerr << "Unexpected parameter value returned" << std::endl;
        return 1;
    }

    return 0;
}

}  // namespace trdp_sim
