#include <aws_tcp/server_impl/reader_chain.hpp>

#include <boost/current_function.hpp>

#include <sstream>
#include <exception>
#include <stdexcept>

namespace tcp {
    std::shared_ptr<reader_chain> reader_chain::New(std::shared_ptr<strand_t> strand,
                                      std::shared_ptr<asio::ip::tcp::socket> socket,
                                      std::function<drop_e(std::span<const std::byte>)> callback_receive,
                                      std::function<void()> callback_closed,
                                      const handle_id_t id,
                                      util::logger_t logger) {
        auto instance = std::shared_ptr<reader_chain>{
                new reader_chain{
                        std::move(strand),
                        std::move(socket),
                        std::move(callback_receive),
                        std::move(callback_closed),
                        id,
                        std::move(logger)
                }
        };
        instance->reading_chain();
        return instance;
    }

    void reader_chain::stop() {
        auto callback_shutdown = [instance = this->shared_from_this()]() -> void {
            try {
                if (instance->m_socket_state == socket_state_e::active) {
                    instance->m_socket_state = socket_state_e::shutdown;
                    boost::system::error_code ec{};
                    // Shutdown conceptually places an eof in the TCP stream.
                    // that causes callback_read to end early.
                    instance->m_socket->shutdown(
                            asio::socket_base::shutdown_type::shutdown_both, ec);
                    if (ec) {
                        std::ostringstream err_msg;
                        err_msg << "Error shutting down socket: " << ec.message();
                        instance->report_error(err_msg.str());
                    }
                }
            }
            catch (std::exception &e) {
                std::ostringstream err_msg;
                err_msg << R"(Error in function "callback_shutdown".)"
                        << " An exception was caught: " << e.what();
                instance->report_error(err_msg.str());
            }
        };
        // Shutdown must be called from the same thread as the tcp socket.
        // that's why we're calling shutdown using the strand that was used
        // to create the socket.
        asio::post(*this->m_strand, std::move(callback_shutdown));
        this->m_sync_close_socket.wait_until_done();
    }

    void reader_chain::report_error(const std::string_view msg) {
        std::ostringstream err_msg;
        err_msg << msg
                << " The error occured in handle_id: " << this->m_id
                << " remote endpoint: " << this->m_remote_endpoint;
        this->m_logger.log_error(err_msg.str());
    }

    void reader_chain::close_socket() {
        if (!this->m_strand->running_in_this_thread()) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the socket (the same strand)."
                    << " This assertion is meant to help prevent a race condition.";
            throw std::logic_error{err_msg.str()};
        }
        this->m_socket_state = socket_state_e::closed;
        boost::system::error_code ec{};
        this->m_socket->close(ec);
        this->on_close();
        if (ec) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " an error has occurred when closing the socket: " << ec.message();
            this->report_error(err_msg.str());
        }
    }

    void reader_chain::on_close() {
        if (!this->m_strand->running_in_this_thread()) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the socket (the same strand)."
                    << " This assertion is meant to help prevent a race condition.";
            throw std::logic_error{err_msg.str()};
        }
        this->m_sync_close_socket.done();
        try {
            this->m_callback_closed.get()();
        }
        catch (std::exception &e) {
            std::ostringstream err_msg;
            err_msg << "Caught exception in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " while running the callback function m_callback_closed: "
                    << e.what();
            this->report_error(err_msg.str());
        }
    }

    reader_chain::reader_chain(std::shared_ptr <strand_t> strand,
                               std::shared_ptr <asio::ip::tcp::socket> socket,
                               std::function<drop_e(std::span<const std::byte>)> callback_receive,
                               std::function<void()> callback_closed,
                               const handle_id_t id,
                               util::logger_t logger)
                               : m_strand{std::move(strand)},
                                m_socket{std::move(socket)},
                                m_remote_endpoint{this->m_socket->remote_endpoint()},
                                m_callback_receive{std::move(callback_receive)},
                                m_callback_closed{std::move(callback_closed)},
                                m_id{id},
                                m_logger{std::move(logger)}
    {
    // TODO: Assert that m_socket was constructed using m_strand
    }

    void reader_chain::reading_chain() {
        auto instance = this->shared_from_this();
        auto callback_read = [instance = std::move(instance)](
                const boost::system::error_code ec,
                const std::size_t amount_bytes_read) -> void {
            try {
                if (ec) {
                    if (ec != asio::error::eof) {
                        std::ostringstream message;
                        message << "Error in reader callback chain: " << ec.message();
                        instance->report_error(message.str());
                    } else {
                        if (instance->m_socket_state == socket_state_e::shutdown) {
                            // Server initiated shutdown
                        } else {
                            // Client initiated shutdown
                        }
                    }
                    instance->close_socket();
                } else {
                    const auto bytes_received{
                            std::span<const std::byte>{
                                    instance->m_buffer->cbegin(),
                                    instance->m_buffer->cbegin() + static_cast<std::ptrdiff_t>(amount_bytes_read)
                            }
                    };
                    const drop_e drop_client{
                            instance->m_callback_receive.get()(bytes_received)
                    };
                    if (drop_client == drop_e::drop_client) {
                        boost::system::error_code ec_shutdown{};
                        instance->m_socket->shutdown(
                                asio::socket_base::shutdown_type::shutdown_both, ec_shutdown);
                        if (ec_shutdown) {
                            std::ostringstream err_msg;
                            err_msg << "Error shutting down socket"
                                    << " after received drop_e::drop_client."
                                    << " error message: " << ec_shutdown.message();
                            instance->report_error(err_msg.str());
                        }
                        instance->close_socket();
                    } else {
                        instance->reading_chain();
                    }
                }
            }
            catch (std::exception &e) {
                std::ostringstream err_msg;
                err_msg << R"(Error in function "callback_read".)"
                        << " An exception was caught: " << e.what();
                instance->report_error(err_msg.str());
            }
        };
        this->m_socket->async_read_some(asio::buffer(*this->m_buffer), std::move(callback_read));
    }
}
