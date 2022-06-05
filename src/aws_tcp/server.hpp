#pragma once

#include <aws_tcp/server_impl/asio_declarations.hpp>
#include <gsl/gsl>
#include <aws_tcp/server_impl/acceptor_chain.hpp>
#include <aws_tcp/server_impl/reader_chain.hpp>
#include <aws_tcp/util/logger_t.hpp>
#include <aws_tcp/util/thread_safe_vec.hpp>
#include <memory>
#include <list>

namespace tcp {
    class server {
    public:
        // Unique for each incoming connection in a single instance of server.
        using handle_id_t = reader_chain::handle_id_t;

        enum class allow_e : bool {
            deny = false,
            allow = true,
        };

        // Returns whether the connection should be denied (immediately closed)
        // Or whether a reader_chain should be started.
        using filter_func_sig = allow_e(
                const asio::ip::tcp::socket&,
                handle_id_t);

        using drop_e = reader_chain::drop_e;

        // Returns whether or not we should continue to listen for more data from that client.
        using callback_receive_data_func_sig = drop_e(
                /*received bytes, the user should define their own header/trailer
                 because a "packet" can always be cut into multiple chunks*/
                std::span<const std::byte>,
                /*socket, for the user to respond to the client,
                for the user to see the remote/local endpoint*/
                const std::shared_ptr<boost::asio::ip::tcp::socket>&,
                /*handle ID for the user to differentiate connections*/
                const handle_id_t);

        explicit server(
                asio::ip::tcp::endpoint local_endpoint,
                std::shared_ptr<asio::io_context> butler,
                std::function<filter_func_sig> filter_incoming_connections,
                std::function<callback_receive_data_func_sig> callback_received_data,
                std::function<void(handle_id_t)> callback_closed_socket,
                std::function<void()> callback_stopped,
                util::logger_t logger);

        void stop() noexcept;

        ~server();
    private:
        const asio::ip::tcp::endpoint m_local_endpoint{};

        gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;

        using connections_list_t = std::list<std::weak_ptr<reader_chain>>;
        using dead_connections_list_t = util::thread_safe_vec<connections_list_t::const_iterator>;

        handle_id_t m_handle_id_counter{ 0 };

        // Maintain a list of active connections, so that they can be cancelled gracefully.

        // In order to maintain thread safety, only access this list from
        // the "new socket" callback of acceptor_chain.
        const std::shared_ptr<connections_list_t> m_connections{
                std::make_shared<connections_list_t>()
        };
        // In order to maintain thread safety, we keep a thread-safe list of recently dead
        // connections.
        // When the callback of acceptor_chain calls us, we "flush" this list
        // by deleting all relevant elements in "m_connections".
        const std::shared_ptr<dead_connections_list_t> m_recently_dead_connections{
                std::make_shared<dead_connections_list_t>()
        };

        // acceptor_chain will extend its own lifetime.
        // We keep a weak reference in case we need to cancel acceptor_chain.
        std::weak_ptr<acceptor_chain> m_acceptor_chain_weak{};

        gsl::not_null<std::function<filter_func_sig>> m_filter_incoming_connections;
        gsl::not_null<std::function<callback_receive_data_func_sig>> m_callback_received_data;
        gsl::not_null<std::function<void(handle_id_t)>> m_callback_closed_socket;
        gsl::not_null<std::function<void()>> m_callback_stopped;

        util::logger_t m_logger;

        void start_acceptor_chain();

        void stop_acceptor_chain() noexcept;

        // Call stop_acceptor_chain() before calling this function
        // because otherwise there will be a race condition with "m_connections".
        void stop_all_reader_chains() noexcept;
    };
}
