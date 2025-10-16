#include "trdp_simulator/trdp_stack_adapter.hpp"

#include <chrono>
#include <thread>

namespace trdp_sim {
namespace {
class StubTrdpStackAdapter : public TrdpStackAdapter {
public:
    void initialize(const NetworkConfig &, const LoggingConfig &) override {}
    void shutdown() override {}

    void register_pd_publisher(const PdPublisherConfig &config) override
    {
        (void)config;
    }

    void register_pd_subscriber(const PdSubscriberConfig &, PdHandler handler) override
    {
        subscriberHandler_ = std::move(handler);
    }

    void publish_pd(const std::string &publisherName, const std::vector<std::uint8_t> &data) override
    {
        if (subscriberHandler_) {
            subscriberHandler_({publisherName, 0u, data, 0u});
        }
    }

    void register_md_sender(const MdSenderConfig &, MdHandler handler) override
    {
        mdReplyHandler_ = std::move(handler);
    }

    void send_md_request(const std::string &senderName, const std::vector<std::uint8_t> &data) override
    {
        if (mdReplyHandler_) {
            mdReplyHandler_({senderName, 0u, data, 0u});
        }
    }

    void register_md_listener(const MdListenerConfig &, MdHandler handler) override
    {
        mdRequestHandler_ = std::move(handler);
    }

    void send_md_reply(const std::string &, const MdMessage &request, const std::vector<std::uint8_t> &data) override
    {
        if (mdReplyHandler_) {
            mdReplyHandler_({request.endpoint, request.comId, data, request.sessionId});
        }
    }

    void poll(std::chrono::milliseconds timeout) override
    {
        std::this_thread::sleep_for(timeout);
    }

private:
    PdHandler subscriberHandler_;
    MdHandler mdReplyHandler_;
    MdHandler mdRequestHandler_;
};
}  // namespace

std::unique_ptr<TrdpStackAdapter> create_stub_trdp_stack_adapter()
{
    return std::make_unique<StubTrdpStackAdapter>();
}

}  // namespace trdp_sim
