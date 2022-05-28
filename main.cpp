#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/error.hpp>
#include <cstddef>
#include <utility>
#include <new>
#include <future>
#include <memory>
#include <array>
#include <gsl/gsl>

#include <iostream>
#include <csignal>

namespace {
    volatile std::sig_atomic_t g_program_ending = false;
    void callback_signal_handler(const int signal_val) {
        g_program_ending = true;
    }
}

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
        this->m_socket->async_read_some(
                asio::buffer(*this->m_buffer),
                [instance = std::move(instance)]
                    (const boost::system::error_code error, const std::size_t amount_bytes_read) -> void {
                    if (error.operator bool()) {
                        std::ostringstream message;
                        message << "Error in reader callback chain: " << error;
                        if (error == asio::error::eof) {
                            message << " eof";
                        }
                        message << " Remote endpoint: " << instance->m_remote_endpoint << std::endl;
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
                        if (!g_program_ending) {
                            instance->reading_chain();
                        }
                    }
                });
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
        this->m_acceptor.async_accept(
                *sock,
                [sock = sock, instance = std::move(instance)](boost::system::error_code error) mutable -> void
                {
                    if (error.operator bool()) {
                        std::ostringstream message;
                        message << "Error in acceptor callback chain: " << error << '\n';
                        std::cout << message.str() << std::flush;
                    }
                    else {
                        std::ostringstream message;
                        message << "Successfully connected to: " << sock->remote_endpoint() << '\n';
                        std::cout << message.str() << std::flush;
                        {
                            auto reader = reader_chain::New(instance->m_butler, std::move(sock));
                        }
                    }
                    if (!g_program_ending) {
                        instance->callback_chain();
                    }
                });
    }
public:
    static std::shared_ptr<acceptor_chain> New(std::shared_ptr<asio::io_context> butler,
                                        const asio::ip::tcp::endpoint& local_endpoint) {
        auto ret = std::shared_ptr<acceptor_chain>(new acceptor_chain(std::move(butler), local_endpoint));
        ret->callback_chain();
        return ret;
    }
};

int main() {
    std::signal(SIGINT, callback_signal_handler);
    std::signal(SIGTERM, callback_signal_handler);
    try {
        const auto butler = std::make_shared<asio::io_context>();
        auto work_guard = asio::make_work_guard(*butler);
        std::future<void> thread_running_butler =
                std::async(std::launch::async,
                           [butler]() -> void {
                               while (!g_program_ending) {
                                   butler->restart();
                                   butler->run();
                               }
                           });
        {
            auto acceptor = acceptor_chain::New(
                    butler,
                    asio::ip::tcp::endpoint{asio::ip::tcp::v4(), 80});
        }
        work_guard.reset();
        while (true) {
            if (g_program_ending) {
                butler->stop();
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
