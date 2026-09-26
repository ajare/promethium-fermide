#include "metrics/MetricsHttpServer.h"
#include "metrics/PrometheusTextFormatter.h"
#include <httplib.h>
#include <thread>
namespace metrics {
struct MetricsHttpServer::Impl {
    httplib::Server server;
    std::thread thread;
    int port = -1;
    std::string diagnostic;
    explicit Impl(core::SimulationMetricsCollector const& collector) {
        // cpp-httplib defaults to SO_REUSEPORT on Linux, which would silently
        // share scrapes with another process instead of reporting a conflict.
        server.set_socket_options([](socket_t socket) {
            int value = 1;
#ifdef _WIN32
            setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&value), sizeof(value));
#else
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
#endif
        });
        server.set_payload_max_length(0);
        server.set_read_timeout(2); server.set_write_timeout(2);
        server.set_pre_routing_handler([](httplib::Request const& req, httplib::Response& res) {
            if (req.method != "GET") { res.status = 405; res.set_header("Allow", "GET"); return httplib::Server::HandlerResponse::Handled; }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/healthz", [](auto const&, auto& res) { res.set_content("ok\n", "text/plain"); });
        server.Get("/metrics", [&collector](auto const&, auto& res) { res.set_content(PrometheusTextFormatter::format(collector.snapshot()), "text/plain; version=0.0.4"); });
    }
};
MetricsHttpServer::MetricsHttpServer(core::SimulationMetricsCollector const& collector) : impl(std::make_unique<Impl>(collector)) {}
MetricsHttpServer::~MetricsHttpServer() { stop(); }
bool MetricsHttpServer::start(int port) {
    stop(); impl->diagnostic.clear();
    if (port < 0 || port > 65535) { impl->diagnostic = "Invalid metrics port"; return false; }
    impl->port = port == 0 ? impl->server.bind_to_any_port("127.0.0.1") : (impl->server.bind_to_port("127.0.0.1", port) ? port : -1);
    if (impl->port < 0) { impl->diagnostic = "Metrics unavailable: cannot bind 127.0.0.1:" + std::to_string(port); return false; }
    impl->thread = std::thread([this] { impl->server.listen_after_bind(); });
    impl->server.wait_until_ready();
    return true;
}
void MetricsHttpServer::stop() { impl->server.stop(); if (impl->thread.joinable()) impl->thread.join(); impl->port = -1; }
int MetricsHttpServer::port() const { return impl->port; }
std::string const& MetricsHttpServer::diagnostic() const { return impl->diagnostic; }
}
