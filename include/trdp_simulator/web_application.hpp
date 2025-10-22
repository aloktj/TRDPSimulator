#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "trdp_simulator/runtime_metrics.hpp"

namespace trdp_sim {

class Simulator;

class WebApplication {
public:
    WebApplication(std::string host, unsigned short port);
    ~WebApplication();

    void run();
    void request_stop();

private:
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

    HttpResponse handle_get_config(const std::string &query) const;
    HttpResponse handle_list_configs() const;
    HttpResponse handle_save_config(const std::string &body) const;
    HttpResponse handle_upload_config(const std::string &body) const;

    bool start_simulator(const std::string &config_path, std::string &message);
    bool stop_simulator(std::string &message);
    std::string build_status_json() const;
    std::string build_metrics_json() const;

    static std::string main_page_html();
    static std::string json_escape(const std::string &value);
    static std::string url_decode(const std::string &value);
    static std::string extract_parameter(const std::string &query, const std::string &key);

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

    mutable std::mutex simulator_mutex_;
    std::condition_variable simulator_cv_;
    std::thread simulator_thread_;
    std::shared_ptr<Simulator> active_simulator_;
    bool simulator_running_{false};
    bool simulator_start_pending_{false};
    std::string current_config_;
    std::optional<std::string> last_error_;
    mutable RuntimeMetrics::Snapshot last_metrics_snapshot_;
    mutable bool has_metrics_snapshot_{false};
};

}  // namespace trdp_sim
