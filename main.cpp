#include <aws_tcp/server.hpp>

#include <boost/numeric/conversion/cast.hpp>
#include <thread>
#include <future>
#include <vector>

#include <iostream>
#include <csignal>
#include <cstring>

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
        const unsigned num_logical_processsors{ std::thread::hardware_concurrency() };
        // Example: An Intel CPU with 8 cores will have 16 logical processors.
        //          If we're running on that machine, we'll create 8 worker threads.
        const int num_worker_threads = std::max<int>(1, boost::numeric_cast<int>(num_logical_processsors) / 2);
        std::vector<std::future<void>> threads{};
        threads.reserve(num_worker_threads);
        
        // The work_gurd stops the butler->run() from returning immediately
        auto work_guard = asio::make_work_guard(*butler);
        for (int counter = 0; counter < num_worker_threads; ++counter) {
            auto thread_run_butler = [butler]() -> void {
                butler->run();
            };
            threads.push_back(std::async(std::launch::async, std::move(thread_run_butler)));
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
            static constexpr const char msg[] =
                "reply from aws_tcp server. Here all all possible byte values in order: ";
            std::vector<std::byte> out_buf{ sizeof(msg), static_cast<std::byte>(0) };
            std::memcpy(out_buf.data(), msg, out_buf.size());
            for (int ch = 0; ch <= 0xff; ++ch) {
                out_buf.push_back(static_cast<std::byte>(ch));
            }
            const auto non_owning_out_buf = asio::buffer(out_buf.data(), out_buf.size());
            auto callback_done_send =
                [out_buf = std::move(out_buf)](
                    const boost::system::error_code& ec,
                    const std::size_t bytes_sent) -> void
            {
                if (ec) {
                    std::ostringstream err_msg;
                    err_msg << "Error while running async_send."
                            << " " << ec.message() << '\n';
                    std::cout << err_msg.str() << std::flush;
                }
                else {
                    if (out_buf.size() == bytes_sent) {
                        std::ostringstream message;
                        message << "Successfully sent " << bytes_sent << " bytes.\n";
                        std::cout << message.str() << std::flush;
                    }
                    else {
                        std::ostringstream message;
                        message << "Tried to send " << out_buf.size()
                                << " bytes, but only managed to send: " << bytes_sent
                                << " bytes.\n";
                        std::cout << message.str() << std::flush;
                    }
                }
            };
            sender->async_send(non_owning_out_buf, std::move(callback_done_send));
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
        auto callback_stopped = []() -> void
        {
            g_program_ending = true;
            std::ostringstream message;
            message << "closed acceptor\n";
            std::cout << message.str() << std::flush;
        };
        tcp::server server{
            asio::ip::tcp::endpoint{ asio::ip::tcp::v4(), 1234 },
            butler,
            std::move(callback_filter_incoming_connections),
            std::move(callback_received_data),
            std::move(callback_closed_socket),
            std::move(callback_stopped),
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
        for (std::future<void>& worker_thread : threads) {
            try {
                worker_thread.get();
            }
            catch (std::exception& e) {
                std::ostringstream err_msg;
                err_msg << "Error while joining one of the worker threads: "
                        << e.what() << '\n';
                std::cout << err_msg.str() << std::flush;
            }
        }
    }
    catch (std::exception& e) {
        std::ostringstream output;
        output << "Exception caught: " << e.what() << '\n';
        std::cerr << output.str() << std::flush;
    }
    return 0;
}
