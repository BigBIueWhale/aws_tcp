#include <aws_tcp/server.hpp>

#include <future>

#include <iostream>
#include <csignal>

namespace {
    volatile std::sig_atomic_t g_program_ending = false;
    void callback_signal_handler(const int signal_val) {
        g_program_ending = true;
    }
}

int main() {
    namespace asio = boost::asio;
    std::signal(SIGINT, callback_signal_handler);
    std::signal(SIGTERM, callback_signal_handler);
    try {
        const auto butler = std::make_shared<asio::io_context>();
        auto work_guard = asio::make_work_guard(*butler);

        std::future<void> thread_running_butler{};
        {
            auto thread_run_butler = [butler]() -> void {
                butler->run();
            };
            thread_running_butler = std::async(std::launch::async, std::move(thread_run_butler));
        }
        auto callback_filter_incoming_connections =
            [](const asio::ip::tcp::socket& handle,
               const tcp::server::handle_id_t id) -> tcp::server::allow_e
        {
            std::ostringstream message;
            message << "New connection "
                    << " from remote_endpoint: " << handle.remote_endpoint()
                    << " on local_endpoint: " << handle.local_endpoint()
                    << " id: " << id << '\n';
            std::cout << message.str() << std::flush;
            return tcp::server::allow_e::allow;
        };
        auto callback_received_data = [](
            const std::span<const std::byte> bytes,
            const std::shared_ptr<boost::asio::ip::tcp::socket>& sender,
            const tcp::server::handle_id_t id) -> tcp::server::drop_e
        {
            std::ostringstream message;
            message << "Received " << bytes.size() << " bytes"
                    << " from remote_endpoint: " << sender->remote_endpoint()
                    << " on local_endpoint: " << sender->local_endpoint()
                    << " id: " << id << '\n';
            std::cout << message.str() << std::flush;
            return tcp::server::drop_e::keep_client;
        };
        auto callback_closed_socket = [](const tcp::server::handle_id_t id) -> void
        {
            std::ostringstream message;
            message << "closed socket."
                    << " id: " << id << '\n';
            std::cout << message.str() << std::flush;
        };
        auto log_error = [](const std::string_view error) -> void
        {
            std::ostringstream message;
            message << error << '\n';
            std::cout << message.str() << std::flush;
        };
        tcp::server server{
            asio::ip::tcp::endpoint{ asio::ip::tcp::v4(), 1234 },
            butler,
            std::move(callback_filter_incoming_connections),
            std::move(callback_received_data),
            std::move(callback_closed_socket),
            tcp::util::logger_t{ std::move(log_error) }
        };
        work_guard.reset();

        while (true) {
            if (g_program_ending) {
                server.stop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 14 });
        }
    }
    catch (std::exception& e) {
        std::ostringstream output;
        output << "Exception caught: " << e.what() << '\n';
        std::cerr << output.str() << std::flush;
    }
    return 0;
}
