#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/error.hpp>
#include <cstddef>
#include <utility>
#include <concepts>
#include <new>
#include <future>
#include <memory>
#include <array>
#include <list>
#include <exception>
#include <stdexcept>
#include <functional>
#include <gsl/gsl>

#include <iostream>
#include <csignal>

namespace asio = boost::asio;

class reader_chain : public std::enable_shared_from_this<reader_chain> {
protected:
    gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;
    gsl::not_null<std::shared_ptr<asio::ip::tcp::socket>> m_socket;
    asio::ip::tcp::endpoint m_remote_endpoint{};
    gsl::not_null<std::unique_ptr<std::array<std::byte, 0xffff>>> m_buffer{
        std::make_unique<std::array<std::byte, 0xffff>>()
    };
    gsl::not_null<std::function<void(std::span<const std::byte>)>> m_callback_receive;
    gsl::not_null<std::function<void(std::string_view)>> m_callback_error;
    explicit reader_chain(std::shared_ptr<asio::io_context> butler,
                          std::shared_ptr<asio::ip::tcp::socket> socket,
                          std::function<void(std::span<const std::byte>)> callback_receive,
                          std::function<void(std::string_view)> callback_error)
        : m_butler{ std::move(butler) },
        m_socket{ std::move(socket) },
        m_remote_endpoint{ this->m_socket->remote_endpoint() },
        m_callback_receive{ std::move(callback_receive) },
        m_callback_error{ std::move(callback_error) }
    {}
    void reading_chain() {
        auto instance = this->shared_from_this();
        auto callback_read =
                [instance = std::move(instance)](const boost::system::error_code error, const std::size_t amount_bytes_read) -> void
        {
            if (error.operator bool()) {
                std::ostringstream message;
                message << "Error in reader callback chain: " << error;
                if (error == asio::error::eof) {
                    message << " eof";
                }
                message << " Remote endpoint: " << instance->m_remote_endpoint;
                if (error == asio::error::connection_aborted) {
                    message << " connection_aborted";
                }
                boost::system::error_code close_error{};
                instance->m_socket->close(close_error);
                if (close_error.operator bool()) {
                    message << " failed to close socket: " << close_error;
                }
                instance->m_callback_error.get()(message.str());
            }
            else {
                const auto bytes_received{
                    std::span<const std::byte>{
                            instance->m_buffer->cbegin(),
                            instance->m_buffer->cbegin() + static_cast<std::ptrdiff_t>(amount_bytes_read)
                    }
                };
                instance->m_callback_receive.get()(bytes_received);
                instance->reading_chain();
            }
        };
        this->m_socket->async_read_some(asio::buffer(*this->m_buffer), std::move(callback_read));
    }
public:
    static std::shared_ptr<reader_chain> New(std::shared_ptr<asio::io_context> butler,
                                             std::shared_ptr<asio::ip::tcp::socket> socket,
                                             std::function<void(std::span<const std::byte>)> callback_receive,
                                             std::function<void(std::string_view)> callback_error) {
        auto instance = std::shared_ptr<reader_chain>{
                new reader_chain{
                        std::move(butler),
                        std::move(socket),
                        std::move(callback_receive),
                        std::move(callback_error)
                }
        };
        instance->reading_chain();
        return instance;
    }
};

class acceptor_chain : public std::enable_shared_from_this<acceptor_chain> {
protected:
    gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;
    asio::ip::tcp::acceptor m_acceptor;
    gsl::not_null<std::function<void(std::shared_ptr<asio::ip::tcp::socket>)>> m_callback_socket;
    gsl::not_null<std::function<void(std::string_view)>> m_callback_errors;
    explicit acceptor_chain(std::shared_ptr<asio::io_context> butler,
                            const asio::ip::tcp::endpoint& local_endpoint,
                            std::function<void(std::shared_ptr<asio::ip::tcp::socket>)> callback_socket,
                            std::function<void(std::string_view)> callback_errors)
        : m_butler{ std::move(butler) },
        m_acceptor{ *this->m_butler, local_endpoint },
        m_callback_socket{ std::move(callback_socket) },
        m_callback_errors{ std::move(callback_errors) }
    {
        this->m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    }
    void callback_chain() {
        const auto sock = std::make_shared<asio::ip::tcp::socket>(*this->m_butler);
        auto instance = this->shared_from_this();
        auto callback_accept =
                [sock = sock, instance = std::move(instance)](boost::system::error_code error) mutable -> void
        {
            bool io_context_was_stopped = false;
            if (error.operator bool()) {
                std::ostringstream message;
                message << "Error in acceptor callback chain: " << error;
                if (instance->m_butler->stopped()) {
                    io_context_was_stopped = true;
                    message << " io_context was stopped.";
                }
                instance->m_callback_errors.get()(message.str());
            }
            else {
                instance->m_callback_socket.get()(std::move(sock));
            }
            if (!io_context_was_stopped) {
                instance->callback_chain();
            }
        };
        this->m_acceptor.async_accept(*sock, std::move(callback_accept));
    }
public:
    static std::shared_ptr<acceptor_chain> New(std::shared_ptr<asio::io_context> butler,
                                               const asio::ip::tcp::endpoint& local_endpoint,
                                               std::function<void(std::shared_ptr<asio::ip::tcp::socket>)> callback_socket,
                                               std::function<void(std::string_view)> callback_errors)
    {
        auto ret = std::shared_ptr<acceptor_chain>{
                new acceptor_chain{
                        std::move(butler),
                        local_endpoint,
                        std::move(callback_socket),
                        std::move(callback_errors)
                }
        };
        ret->callback_chain();
        return ret;
    }
};

template <typename callable_T>
concept function_like_object_with_side_effects = requires(callable_T callable) {
    { callable() } -> std::same_as<void>;
};

template <function_like_object_with_side_effects callable_T>
class exit_failure {
protected:
    callable_T m_func;
    int m_exceptions_count{};
public:
    exit_failure() = delete;
    explicit exit_failure(callable_T&& callable)
        : m_func{ std::forward<callable_T>(callable) },
          m_exceptions_count{ std::uncaught_exceptions() }
    {}
    ~exit_failure() noexcept {
        try {
            if (std::uncaught_exceptions() > this->m_exceptions_count) {
                this->m_func();
            }
        }
        catch (std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Can\'t throw exception from: " << __FUNCTION__
                    << " Error message: " << e.what() << "\n";
            std::cerr << err_msg.str() << std::flush;
        }
    }
};

namespace {
    volatile std::sig_atomic_t g_program_ending = false;
    void callback_signal_handler(const int signal_val) {
        g_program_ending = true;
    }
}

int main() {
    std::signal(SIGINT, callback_signal_handler);
    std::signal(SIGTERM, callback_signal_handler);
    try {
        const auto butler = std::make_shared<asio::io_context>();
        auto work_guard = asio::make_work_guard(*butler);
        const auto early_stop = std::make_shared<std::atomic<bool>>(false);
        const auto stop_butler_and_running_thread = [butler, early_stop]() -> void
        {
            early_stop->store(true);
            // TODO: Cancel all open sockets gracefully before calling io_context.stop().
            //       Do this by calling tcp::socket.shutdown(tcp::socket::shutdown_receive)
            //       from the same thread as the io_context.
            //       That will cause a "reading_chain::callback_read" to close the socket.
            //       Maybe maintain a list of weak_ptr to the sockets ?
            //       https://github.com/boostorg/beast/issues/2004#issuecomment-653046539
            butler->stop();
        };
        const auto stop_butler_upon_exception = exit_failure{
            [stop_butler_and_running_thread]() -> void
            {
                stop_butler_and_running_thread();
            }
        };
        auto thread_run_butler = [butler, early_stop]() -> void {
            while (!early_stop->load()) {
                butler->restart();
                butler->run();
            }
        };
        std::future<void> thread_running_butler = std::async(std::launch::async, thread_run_butler);

        // The acceptor_chain will extend its own lifetime
        {
            auto callback_socket =
                [butler = gsl::make_not_null(butler)](std::shared_ptr<asio::ip::tcp::socket> sock) -> void
            {
                auto callback_receive = [sock](const std::span<const std::byte> bytes_received) -> void
                {
                    std::ostringstream message;
                    message << "Received " << bytes_received.size() << " bytes" << '\n';
                    std::cout << message.str() << std::flush;
                };
                auto callback_error = [](const std::string_view error_message) -> void
                {
                    std::cout << error_message << std::endl;
                };
                // The reading_chain object will extend its own lifetime
                {
                    const auto reader = reader_chain::New(
                            butler,
                            std::move(sock),
                            std::move(callback_receive),
                            std::move(callback_error));
                }
            };
            auto callback_errors = [](const std::string_view error_message) -> void
            {
                std::cout << error_message << std::endl;
            };
            const auto acceptor = acceptor_chain::New(
                    butler,
                    asio::ip::tcp::endpoint{ asio::ip::tcp::v4(), 1234 },
                    std::move(callback_socket),
                    std::move(callback_errors));
        }
        work_guard.reset();

        while (true) {
            if (g_program_ending) {
                stop_butler_and_running_thread();
            }
            // Sleep to avoid busy-waiting on the global variable.
            // TODO: Why don't really need to try to join the running thread here
            const auto join_status = thread_running_butler.wait_for(std::chrono::milliseconds(14));
            if (join_status == std::future_status::ready) {
                thread_running_butler.get();
                break;
            }
        }
    }
    catch (std::exception& e) {
        std::ostringstream output;
        output << "Exception caught: " << e.what() << '\n';
        std::cout << output.str() << std::flush;
    }
    return 0;
}
