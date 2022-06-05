#include <aws_tcp/server_impl/acceptor_chain.hpp>

#include <boost/current_function.hpp>

#include <sstream>
#include <exception>
#include <stdexcept>

namespace tcp {
    std::shared_ptr<acceptor_chain> acceptor_chain::New(std::shared_ptr<asio::io_context> butler,
                                               const asio::ip::tcp::endpoint &local_endpoint,
                                               std::function<func_sig_callback_socket> callback_socket,
                                               std::function<void()> callback_closed_acceptor,
                                               util::logger_t callback_errors) {
        auto ret = std::shared_ptr<acceptor_chain> {
                new acceptor_chain{
                        std::move(butler),
                        local_endpoint,
                        std::move(callback_socket),
                        std::move(callback_closed_acceptor),
                        std::move(callback_errors)
                }
        };
        ret->callback_chain();
        return ret;
    }

    void acceptor_chain::stop() {
        auto callback_cancel = [instance = this->shared_from_this()]() -> void {
            if (instance->m_acceptor_state == acceptor_state_e::active) {
                instance->m_acceptor_state = acceptor_state_e::canceled;
                boost::system::error_code ec{};
                instance->m_acceptor.cancel(ec);
                if (ec) {
                    std::ostringstream err_msg;
                    err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                            << " the error occurred while cancelling the acceptor: " << ec.message();
                    instance->m_logger.log_error(err_msg.str());
                }
            }
        };
        // Cancel must be called from the same thread as the tcp acceptor.
        // that's why we're calling cancel using the strand that was used
        // to create the acceptor.
        asio::post(this->m_strand, std::move(callback_cancel));
        this->m_sync_close_acceptor.wait_until_done();
    }

    void acceptor_chain::close_acceptor() {
        if (!this->m_strand.running_in_this_thread()) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the acceptor (the same strand)."
                    << " this assertion is meant to help prevent a race condition";
            throw std::logic_error{err_msg.str()};
        }
        this->m_acceptor_state = acceptor_state_e::closed;
        boost::system::error_code ec{};
        this->m_acceptor.close(ec);
        try {
            this->m_callback_closed_acceptor.get()();
        }
        catch (std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << R"( an error has occurred while calling "callback_closed_acceptor": )"
                    << e.what();
            this->m_logger.log_error(err_msg.str());
        }
        this->m_sync_close_acceptor.done();
        if (ec) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " an error has occurred while closing the socket: " << ec.message();
            this->m_logger.log_error(err_msg.str());
        }
    }

    acceptor_chain::acceptor_chain(std::shared_ptr<asio::io_context> butler,
                                   const asio::ip::tcp::endpoint &local_endpoint,
                                   std::function<func_sig_callback_socket> callback_socket,
                                   std::function<void()> callback_closed_acceptor,
                                   util::logger_t logger)
                                   : m_butler{std::move(butler)},
                                    m_strand{ asio::make_strand(this->m_butler->get_executor()) },
                                    m_acceptor{ this->m_strand, local_endpoint },
                                    m_callback_socket{ std::move(callback_socket) },
                                    m_callback_closed_acceptor{ std::move(callback_closed_acceptor) },
                                    m_logger{ std::move(logger) }
    {
        this->m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    }

    void acceptor_chain::callback_chain() {
        // By constructing the websocket with the strand, we now have the ability to
        // post a function to handle the current socket in a thread safe manner.
        // Each socket will have its own strand that ensures thread safety.
        auto strand_for_current_socket{
                std::make_shared < asio::strand < asio::io_context::executor_type >> (
                asio::make_strand(this->m_butler->get_executor()))
        };
        const auto sock = std::make_shared<asio::ip::tcp::socket>(*strand_for_current_socket);
        auto instance = this->shared_from_this();
        auto callback_accept =
                [instance = std::move(instance),
                strand_for_current_socket = std::move(strand_for_current_socket),
                sock = sock](const boost::system::error_code& ec) mutable -> void
        {
            try {
                bool continue_chain = true;
                if (ec) {
                    const bool canceled = ec == asio::error::operation_aborted;
                    const bool is_planned_cancel{
                            canceled && instance->m_acceptor_state == acceptor_state_e::canceled
                    };
                    if (!is_planned_cancel) {
                        std::ostringstream message;
                        message << "Error in acceptor callback chain: " << ec.message();
                        instance->m_logger.log_error(message.str());
                    }
                    if (canceled) {
                        continue_chain = false;
                        instance->close_acceptor();
                    }
                } else {
                    instance->m_callback_socket.get()(std::move(strand_for_current_socket),
                                                        std::move(sock));
                }
                if (continue_chain) {
                    instance->callback_chain();
                }
            }
            catch (std::exception &e) {
                std::ostringstream err_msg;
                err_msg << R"(Error in function "callback_accept". An exception was caught: )" << e.what();
                instance->m_logger.log_error(err_msg.str());
            }
        };
        this->m_acceptor.async_accept(*sock, std::move(callback_accept));
    }
}
