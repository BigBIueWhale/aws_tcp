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
#include <exception>
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
    explicit reader_chain(std::shared_ptr<asio::io_context> butler,
                            std::shared_ptr<asio::ip::tcp::socket> socket)
        : m_butler{ std::move(butler) },
        m_socket{ std::move(socket) },
        m_remote_endpoint{ this->m_socket->remote_endpoint() }
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
                if (instance->m_butler->stopped()) {
                    message << " io_context was stopped.";
                }
                message << '\n';
                std::cout << message.str() << std::flush;
            }
            else {
                std::ostringstream message;
                message << "Read " << amount_bytes_read << " bytes"
                        << " from: " << instance->m_remote_endpoint << '\n';
                message << "The data: ";
                message.write(
                        std::launder(reinterpret_cast<const char*>(instance->m_buffer->data())),
                        static_cast<std::ptrdiff_t>(amount_bytes_read));
                message << '\n';
                std::cout << message.str() << std::flush;
                instance->reading_chain();
            }
        };
        this->m_socket->async_read_some(asio::buffer(*this->m_buffer), std::move(callback_read));
    }
public:
    static std::shared_ptr<reader_chain> New(std::shared_ptr<asio::io_context> butler,
                                             std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto instance = std::shared_ptr<reader_chain>(new reader_chain{ std::move(butler), std::move(socket) });
        instance->reading_chain();
        return instance;
    }
};

class acceptor_chain : public std::enable_shared_from_this<acceptor_chain> {
protected:
    gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;
    asio::ip::tcp::acceptor m_acceptor;
    explicit acceptor_chain(std::shared_ptr<asio::io_context> butler,
                            const asio::ip::tcp::endpoint& local_endpoint)
        : m_butler{ std::move(butler) },
        m_acceptor{ *this->m_butler, local_endpoint }
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
                message << "Error in acceptor callback chain: " << error << '\n';
                if (instance->m_butler->stopped()) {
                    io_context_was_stopped = true;
                    message << "io_context was stopped.";
                }
                std::cout << message.str() << std::flush;
            }
            else {
                std::ostringstream message;
                message << "Successfully connected to: " << sock->remote_endpoint() << '\n';
                std::cout << message.str() << std::flush;
                // The reading_chain object will extend its own lifetime
                {
                    auto reader = reader_chain::New(instance->m_butler, std::move(sock));
                }
            }
            if (!io_context_was_stopped) {
                instance->callback_chain();
            }
        };
        this->m_acceptor.async_accept(*sock, std::move(callback_accept));
    }
public:
    static std::shared_ptr<acceptor_chain> New(std::shared_ptr<asio::io_context> butler,
                                        const asio::ip::tcp::endpoint& local_endpoint) {
        auto ret = std::shared_ptr<acceptor_chain>(new acceptor_chain(std::move(butler), local_endpoint));
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
        catch (...) {
            std::ostringstream err_msg;
            err_msg << "Can\'t throw exception from: " << __FUNCTION__
                    << " The error message is unavailable" << "\n";
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
            auto acceptor = acceptor_chain::New(
                    butler,
                    asio::ip::tcp::endpoint{asio::ip::tcp::v4(), 1234});
        }
        work_guard.reset();
        while (true) {
            if (g_program_ending) {
                stop_butler_and_running_thread();
            }
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
