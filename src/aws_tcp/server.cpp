#include <aws_tcp/server.hpp>

#include <boost/current_function.hpp>

#include <sstream>
#include <exception>
#include <stdexcept>

namespace tcp {
    server::server(
            asio::ip::tcp::endpoint local_endpoint,
            std::shared_ptr<asio::io_context> butler,
            std::function<filter_func_sig> filter_incoming_connections,
            std::function<callback_receive_data_func_sig> callback_received_data,
            std::function<void(handle_id_t)> callback_closed_socket,
            std::function<void()> callback_stopped,
            util::logger_t logger)
            : m_local_endpoint{std::move(local_endpoint)},
              m_butler{std::move(butler)},
              m_filter_incoming_connections{std::move(filter_incoming_connections)},
              m_callback_received_data{std::move(callback_received_data)},
              m_callback_closed_socket{std::move(callback_closed_socket)},
              m_callback_stopped{ std::move(callback_stopped) },
              m_logger{std::move(logger)}
    {
        this->start_acceptor_chain();
    }

    // Cancel all open sockets gracefully instead of calling io_context.stop().
    // Do this by calling tcp::socket.shutdown(tcp::socket::shutdown_receive)
    // from the same thread as the io_context.
    // That will cause a "reader_chain::callback_read" to close the socket.
    // In order to achieve that, we maintain a list of weak_ptr to the sockets.
    // https://github.com/boostorg/beast/issues/2004#issuecomment-653046539
    //
    // With a tcp acceptor we have to call cancel and then close.
    // https://stackoverflow.com/a/11192750/7753444
    //
    // The "stop" function will be called in the destructor in any case.
    void server::stop() noexcept {
        this->stop_acceptor_chain();
        this->stop_all_reader_chains();
    }

    server::~server() {
        this->stop();
    }

    void server::start_acceptor_chain() {
        auto callback_socket = [this](
                std::shared_ptr<strand_t> strand,
                std::shared_ptr<asio::ip::tcp::socket> sock) -> void
        {
            const handle_id_t current_handle_id = this->m_handle_id_counter++;
            const allow_e allow_connection{
                    this->m_filter_incoming_connections.get()(*sock, current_handle_id)
            };
            if (allow_connection != allow_e::allow) {
                boost::system::error_code ec_shutdown{};
                sock->shutdown(
                        asio::socket_base::shutdown_type::shutdown_both, ec_shutdown);
                if (ec_shutdown) {
                    std::ostringstream err_msg;
                    err_msg << "Error occured after filter returned allow_e::deny."
                            << " shutting down the socket failed."
                            << " The error message: " << ec_shutdown.message();
                    this->m_logger.log_error(err_msg.str());
                }
                boost::system::error_code ec_close{};
                sock->close(ec_close);
                if (ec_close) {
                    std::ostringstream err_msg;
                    err_msg << "Error occured after filter returned allow_e::deny."
                            << " closing the socket failed."
                            << " The error message: " << ec_close.message();
                    this->m_logger.log_error(err_msg.str());
                }
                this->m_callback_closed_socket.get()(current_handle_id);
                return;
            }
            const auto delete_recently_dead_connections = [this]() -> void
            {
                const std::vector<connections_list_t::const_iterator> dead_connections_vec{
                        this->m_recently_dead_connections->embezzle()
                };
                for (const typename connections_list_t::const_iterator& dead_connection : dead_connections_vec) {
                    this->m_connections->erase(dead_connection);
                }
            };
            delete_recently_dead_connections();

            auto callback_receive =
                    [this,
                    sock,
                    current_handle_id](const std::span<const std::byte> bytes_received) -> drop_e
            {
                const drop_e drop_client = this->m_callback_received_data.get()(
                        bytes_received,
                        sock,
                        current_handle_id);
                return drop_client;
            };
            this->m_connections->emplace_back();
            const auto it_current_connection{
                    std::prev(this->m_connections->end())
            };
            auto weak_rdc{
                    std::weak_ptr<dead_connections_list_t>{ this->m_recently_dead_connections }
            };
            auto callback_done_closing_socket =
                    [this,
                    weak_rdc = std::move(weak_rdc),
                    it_current_connection,
                    current_handle_id]() -> void
            {
                const auto recently_dead_connections_strong = weak_rdc.lock();
                if (recently_dead_connections_strong != nullptr) {
                    recently_dead_connections_strong->push_back(it_current_connection);
                }
                this->m_callback_closed_socket.get()(current_handle_id);
            };
            // The reader_chain object will extend its own lifetime.
            // The only reason we keep a weak reference to it, is so that we can cancel it.
            *it_current_connection = reader_chain::New(
                    std::move(strand),
                    std::move(sock),
                    std::move(callback_receive),
                    std::move(callback_done_closing_socket),
                    current_handle_id,
                    this->m_logger);
        };
        auto callback_stopped = [this]() -> void
        {
            this->m_callback_stopped.get()();
        };
        // The acceptor_chain will extend its own lifetime
        this->m_acceptor_chain_weak = acceptor_chain::New(
                this->m_butler,
                this->m_local_endpoint,
                std::move(callback_socket),
                std::move(callback_stopped),
                this->m_logger);
    }

    void server::stop_acceptor_chain() noexcept
    {
        try {
            const auto acceptor_chain_strong = this->m_acceptor_chain_weak.lock();
            if (acceptor_chain_strong != nullptr) {
                this->m_acceptor_chain_weak.reset();
                acceptor_chain_strong->stop();
            }
        }
        catch (std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " exception caught: " << e.what();
            this->m_logger.log_error(err_msg.str());
        }
    };

    // Call stop_acceptor_chain() before calling this function
    // because otherwise there will be a race condition with "m_connections".
    void server::stop_all_reader_chains() noexcept
    {
        if (!this->m_acceptor_chain_weak.expired()) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " stop_all_reader_chains requires that the acceptor_chain"
                    << " has already been stopped."
                    << " This assertion is meant to help prevent a race condition.";
            this->m_logger.log_error(err_msg.str());
            return;
        }
        try {
            for (const std::weak_ptr<reader_chain>& connection_weak : *m_connections) {
                const auto connection_strong = connection_weak.lock();
                if (connection_strong != nullptr) {
                    try {
                        connection_strong->stop();
                    }
                    catch (std::exception& e) {
                        std::ostringstream err_msg;
                        err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                                << " exception caught while calling reader_chain.stop(): "
                                << e.what();
                        this->m_logger.log_error(err_msg.str());
                    }
                }
            }
        }
        catch (std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " exception caught: " << e.what();
            this->m_logger.log_error(err_msg.str());
        }
    }
}
