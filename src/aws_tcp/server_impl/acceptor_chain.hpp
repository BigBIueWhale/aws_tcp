#pragma once

#include <aws_tcp/server_impl/asio_declarations.hpp>
#include <gsl/gsl>
#include <aws_tcp/util/logger_t.hpp>
#include <aws_tcp/util/oneshot_wait.hpp>
#include <memory>

namespace tcp {
    class acceptor_chain : public std::enable_shared_from_this<acceptor_chain> {
    public:
        using func_sig_callback_socket =
        void(/*the strand that was used to create the socket*/
                std::shared_ptr<strand_t>,
                /*the socket of the just-opened connection*/
                std::shared_ptr<asio::ip::tcp::socket>
        );

        static std::shared_ptr<acceptor_chain> New(std::shared_ptr<asio::io_context> butler,
                                                    const asio::ip::tcp::endpoint &local_endpoint,
                                                    std::function<func_sig_callback_socket> callback_socket,
                                                    std::function<void()> callback_closed_acceptor,
                                                    util::logger_t callback_errors);

        void stop();

    private:
        gsl::not_null <std::shared_ptr<asio::io_context>> m_butler;
        strand_t m_strand;
        asio::ip::tcp::acceptor m_acceptor;
        enum class acceptor_state_e : int {
            active = 1,
            canceled,
            closed,
        };
        acceptor_state_e m_acceptor_state{acceptor_state_e::active};
        util::oneshot_wait m_sync_close_acceptor{};
        gsl::not_null<std::function<func_sig_callback_socket>> m_callback_socket;
        gsl::not_null<std::function<void()>> m_callback_closed_acceptor;
        util::logger_t m_logger;

        void close_acceptor();

        explicit acceptor_chain(std::shared_ptr<asio::io_context> butler,
                                const asio::ip::tcp::endpoint &local_endpoint,
                                std::function<func_sig_callback_socket> callback_socket,
                                std::function<void()> callback_closed_acceptor,
                                util::logger_t logger);

        void callback_chain();
    };
}
