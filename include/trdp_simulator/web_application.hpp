#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "trdp_simulator/runtime_metrics.hpp"
#include "trdp_simulator/config_store.hpp"
#include "trdp_simulator/config.hpp"

namespace trdp_sim {

class Simulator;

class WebApplication {
public:
    WebApplication(std::string host, unsigned short port);
    ~WebApplication();

    void run();
    void request_stop();

private:
    friend int run_web_application_tests();

    struct HttpResponse {
        int status_code;
        std::string status_message;
        std::string content_type;
        std::string body;
    };

    void accept_loop();
    void handle_client(int client_fd);
    HttpResponse handle_request(const std::string &method, const std::string &target,
                                const std::string &body);

    HttpResponse handle_list_configs();
    HttpResponse handle_get_config(const std::string &method, const std::string &query,
                                   const std::string &body);
    HttpResponse handle_save_config(const std::string &body);
    HttpResponse handle_upload_config(const std::string &body);

    bool start_simulator(const std::string &config_path, const std::string &config_label, std::string &message);
    bool stop_simulator(std::string &message);
    std::string build_status_json() const;
    std::string build_metrics_json() const;
    std::string build_config_summary_json(const SimulatorConfig &config) const;
    std::string build_payloads_json() const;
    std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string &body) const;
    HttpResponse respond_json(int status, const std::string &body) const;

    static std::string main_page_html();
    static std::string json_escape(const std::string &value);
    static std::string url_decode(const std::string &value)
    {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                std::string hex = value.substr(i + 1, 2);
                char *end = nullptr;
                long decoded = std::strtol(hex.c_str(), &end, 16);
                if (end != nullptr && *end == '\0') {
                    result.push_back(static_cast<char>(decoded));
                    i += 2;
                    continue;
                }
            } else if (value[i] == '+') {
                result.push_back(' ');
                continue;
            }
            result.push_back(value[i]);
        }
        return result;
    }
    static std::string extract_parameter(const std::string &query, const std::string &key)
    {
        std::size_t start = 0;
        while (start < query.size()) {
            auto end = query.find('&', start);
            if (end == std::string::npos) {
                end = query.size();
            }
            auto eq = query.find('=', start);
            if (eq != std::string::npos && eq < end) {
                std::string name = query.substr(start, eq - start);
                if (name == key) {
                    return url_decode(query.substr(eq + 1, end - eq - 1));
                }
            } else {
                if (query.substr(start, end - start) == key) {
                    return std::string();
                }
            }
            start = end + 1;
        }
        return std::string();
    }

    static bool is_safe_config_relative_path(const std::string &path);
    static bool is_xml_file_name(const std::string &name);
    static std::filesystem::path config_root();
    static HttpResponse make_error_response(int status, const std::string &message);
    static const std::filesystem::path &resolved_config_root();
    static bool ensure_config_directory(std::string &error);
    static bool ensure_config_directory();
    static std::string absolute_path_string(const std::filesystem::path &path);
    static HttpResponse write_config_file(const std::string &relative_path, const std::string &contents,
                                          const std::string &success_message);

    void simulator_worker(std::string config_path);

    std::string host_;
    unsigned short port_;

    std::atomic<bool> stop_requested_{false};
    int server_fd_{-1};

    ConfigStore config_store_;

    mutable std::mutex simulator_mutex_;
    std::condition_variable simulator_cv_;
    std::thread simulator_thread_;
    std::shared_ptr<Simulator> active_simulator_;
    bool simulator_running_{false};
    bool simulator_start_pending_{false};
    std::string current_config_;
    std::string current_config_label_;
    std::string pending_config_label_;
    std::optional<std::string> last_error_;
    mutable RuntimeMetrics::Snapshot last_metrics_snapshot_;
    mutable bool has_metrics_snapshot_{false};
};

}  // namespace trdp_sim
