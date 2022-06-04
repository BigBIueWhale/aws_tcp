#include "utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
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
#include <stdexcept>
#include <list>
#include <iterator>

#include <iostream>
#include <csignal>
namespace tcp {

    namespace asio = boost::asio;

    using strand_t = asio::strand<asio::io_context::executor_type>;

    class reader_chain : public std::enable_shared_from_this<reader_chain> {
    public:
        using handle_id_t = long long;

        enum class drop_e : bool {
            keep_client = 0,
            drop_client = 1,
        };

        static std::shared_ptr<reader_chain> New(std::shared_ptr<strand_t> strand,
            std::shared_ptr<asio::ip::tcp::socket> socket,
            std::function<drop_e(std::span<const std::byte>)> callback_receive,
            std::function<void()> callback_closed,
            const handle_id_t id,
            logger_t logger) {
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
        handle_id_t handle_id() const {
            return this->m_id;
        }
        void stop() {
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
                catch (std::exception& e) {
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
    protected:
        gsl::not_null<std::shared_ptr<strand_t>> m_strand;
        gsl::not_null<std::shared_ptr<asio::ip::tcp::socket>> m_socket;
        enum class socket_state_e : int {
            active = 1,
            shutdown,
            closed,
        };
        socket_state_e m_socket_state{ socket_state_e::active };
        oneshot_wait m_sync_close_socket{};
        asio::ip::tcp::endpoint m_remote_endpoint{};
        gsl::not_null<std::unique_ptr<std::array<std::byte, 1024>>> m_buffer{
            std::make_unique<std::array<std::byte, 1024>>()
        };
        gsl::not_null<std::function<drop_e(std::span<const std::byte>)>> m_callback_receive;
        gsl::not_null<std::function<void()>> m_callback_closed;
        handle_id_t m_id{};
        logger_t m_logger;

        void report_error(const std::string_view msg) {
            std::ostringstream err_msg;
            err_msg << msg
                    << " The error occured in handle_id: " << this->m_id
                    << " remote endpoint: " << this->m_remote_endpoint;
            this->m_logger.log_error(err_msg.str());
        }

        void close_socket() {
            if (!this->m_strand->running_in_this_thread()) {
                std::ostringstream err_msg;
                err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the socket (the same strand)."
                    << " This assertion is meant to help prevent a race condition.";
                throw std::logic_error{ err_msg.str() };
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
        void on_close() {
            if (!this->m_strand->running_in_this_thread()) {
                std::ostringstream err_msg;
                err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the socket (the same strand)."
                    << " This assertion is meant to help prevent a race condition.";
                throw std::logic_error{ err_msg.str() };
            }
            this->m_sync_close_socket.done();
            try {
                this->m_callback_closed.get()();
            }
            catch (std::exception& e) {
                std::ostringstream err_msg;
                err_msg << "Caught exception in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " while running the callback function m_callback_closed: "
                    << e.what();
                this->report_error(err_msg.str());
            }
        }
        explicit reader_chain(std::shared_ptr<strand_t> strand,
            std::shared_ptr<asio::ip::tcp::socket> socket,
            std::function<drop_e(std::span<const std::byte>)> callback_receive,
            std::function<void()> callback_closed,
            const handle_id_t id,
            logger_t logger)
            : m_strand{ std::move(strand) },
            m_socket{ std::move(socket) },
            m_remote_endpoint{ this->m_socket->remote_endpoint() },
            m_callback_receive{ std::move(callback_receive) },
            m_callback_closed{ std::move(callback_closed) },
            m_id{ id },
            m_logger{ std::move(logger) }
        {
            // TODO: Assert that m_socket was constructed using m_strand
        }
        void reading_chain() {
            auto instance = this->shared_from_this();
            auto callback_read = [instance = std::move(instance)](
                const boost::system::error_code ec,
                const std::size_t amount_bytes_read) -> void
            {
                try {
                    if (ec) {
                        if (ec != asio::error::eof) {
                            std::ostringstream message;
                            message << "Error in reader callback chain: " << ec.message();
                            instance->report_error(message.str());
                        }
                        else {
                            if (instance->m_socket_state == socket_state_e::shutdown) {
                                // Server initiated shutdown
                            }
                            else {
                                // Client initiated shutdown
                            }
                        }
                        instance->close_socket();
                    }
                    else {
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
                        }
                        else {
                            instance->reading_chain();
                        }
                    }
                }
                catch (std::exception& e) {
                    std::ostringstream err_msg;
                    err_msg << R"(Error in function "callback_read".)"
                        << " An exception was caught: " << e.what();
                    instance->report_error(err_msg.str());
                }
            };
            this->m_socket->async_read_some(asio::buffer(*this->m_buffer), std::move(callback_read));
        }
    };

    class acceptor_chain : public std::enable_shared_from_this<acceptor_chain> {
    public:
        using func_sig_callback_socket =
            void(/*the strand that was used to create the socket*/
                std::shared_ptr<strand_t>,
                /*the socket of the just-opened connection*/
                std::shared_ptr<asio::ip::tcp::socket>);

        static std::shared_ptr<acceptor_chain> New(std::shared_ptr<asio::io_context> butler,
            const asio::ip::tcp::endpoint& local_endpoint,
            std::function<func_sig_callback_socket> callback_socket,
            logger_t callback_errors)
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
        void stop() {
            auto callback_cancel = [instance = this->shared_from_this()]() -> void
            {
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
            // that's why we're calling canacel using the strand that was used
            // to create the acceptor.
            asio::post(this->m_strand, std::move(callback_cancel));
            this->m_sync_close_acceptor.wait_until_done();
        }
    protected:
        gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;
        strand_t m_strand;
        asio::ip::tcp::acceptor m_acceptor;
        enum class acceptor_state_e : int {
            active = 1,
            canceled,
            closed,
        };
        acceptor_state_e m_acceptor_state{ acceptor_state_e::active };
        oneshot_wait m_sync_close_acceptor{};
        gsl::not_null<std::function<func_sig_callback_socket>> m_callback_socket;
        logger_t m_logger;
        void close_acceptor() {
            if (!this->m_strand.running_in_this_thread()) {
                std::ostringstream err_msg;
                err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " expected to be running in the same thread as the"
                    << " asynchronous operations of the acceptor (the same strand)."
                    << " this assertion is meant to help prevent a race condition";
                throw std::logic_error{ err_msg.str() };
            }
            this->m_acceptor_state = acceptor_state_e::closed;
            boost::system::error_code ec{};
            this->m_acceptor.close(ec);
            this->m_sync_close_acceptor.done();
            if (ec) {
                std::ostringstream err_msg;
                err_msg << "Error in function \"" << BOOST_CURRENT_FUNCTION << "\""
                    << " an error has occurred when closing the socket: " << ec.message();
                this->m_logger.log_error(err_msg.str());
            }
        }
        explicit acceptor_chain(std::shared_ptr<asio::io_context> butler,
            const asio::ip::tcp::endpoint& local_endpoint,
            std::function<func_sig_callback_socket> callback_socket,
            logger_t logger)
            : m_butler{ std::move(butler) },
            m_strand{ asio::make_strand(this->m_butler->get_executor()) },
            m_acceptor{ this->m_strand, local_endpoint },
            m_callback_socket{ std::move(callback_socket) },
            m_logger{ std::move(logger) }
        {
            this->m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        }
        void callback_chain() {
            // By constructing the websocket with the strand, we now have the ability to
            // post a function to handle the current socket in a thread safe manner.
            // Each socket will have its own strand that ensures thread safety.
            auto strand_for_current_socket{
                std::make_shared<asio::strand<asio::io_context::executor_type>>(
                        asio::make_strand(this->m_butler->get_executor()))
            };
            const auto sock = std::make_shared<asio::ip::tcp::socket>(*strand_for_current_socket);
            auto instance = this->shared_from_this();
            auto callback_accept =
                [instance = std::move(instance),
                strand_for_current_socket = std::move(strand_for_current_socket),
                sock = sock](boost::system::error_code ec) mutable -> void
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
                    }
                    else {
                        instance->m_callback_socket.get()(std::move(strand_for_current_socket), std::move(sock));
                    }
                    if (continue_chain) {
                        instance->callback_chain();
                    }
                }
                catch (std::exception& e) {
                    std::ostringstream err_msg;
                    err_msg << R"(Error in function "callback_accept". An exception was caught: )" << e.what();
                    instance->m_logger.log_error(err_msg.str());
                }
            };
            this->m_acceptor.async_accept(*sock, std::move(callback_accept));
        }
    };

    class server {
    public:
        // Unique for each incoming connection in a single instance of server.
        using handle_id_t = reader_chain::handle_id_t;

        enum class allow_e : bool {
            deny = 0,
            allow = 1,
        };
        using filter_func_sig = allow_e(
            const asio::ip::tcp::socket&,
            handle_id_t);

        using drop_e = reader_chain::drop_e;

        using callback_receive_data_func_sig =
            // Returns whether or not we should continue to listen for more data from that client.
            drop_e(/*received bytes, the user should define their own header/trailer
                 because a "packet" can always be cut into multiple chunks*/
                std::span<const std::byte>,
                /*socket, for the user to respond to the client,
                for the user to see the remote/local endpoint, and the handle id.*/
                const std::shared_ptr<boost::asio::ip::tcp::socket>&,
                const handle_id_t);

        explicit server(
            asio::ip::tcp::endpoint local_endpoint,
            std::shared_ptr<asio::io_context> butler,
            std::function<filter_func_sig> filter_incoming_connections,
            std::function<callback_receive_data_func_sig> callback_received_data,
            std::function<void(handle_id_t)> callback_closed_socket,
            logger_t logger)
            : m_local_endpoint{ std::move(local_endpoint) },
            m_butler{ std::move(butler) },
            m_filter_incoming_connections{ std::move(filter_incoming_connections) },
            m_callback_received_data{ std::move(callback_received_data) },
            m_callback_closed_socket{ std::move(callback_closed_socket) },
            m_logger{ std::move(logger) }
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
        void stop() noexcept {
            this->stop_acceptor_chain();
            this->stop_all_reader_chains();
        }
        ~server() {
            this->stop();
        }
    protected:
        const asio::ip::tcp::endpoint m_local_endpoint{};

        gsl::not_null<std::shared_ptr<asio::io_context>> m_butler;

        using connections_list_t = std::list<std::weak_ptr<reader_chain>>;
        using dead_connections_list_t = thread_safe_vec<connections_list_t::const_iterator>;

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

        logger_t m_logger;

        void start_acceptor_chain() {
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
                        // TODO: Put a mutex to protect the list of connections
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
            // The acceptor_chain will extend its own lifetime
            this->m_acceptor_chain_weak = acceptor_chain::New(
                this->m_butler,
                this->m_local_endpoint,
                std::move(callback_socket),
                this->m_logger);
        }

        void stop_acceptor_chain() noexcept
        {
            try {
                const auto acceptor_chain_strong = m_acceptor_chain_weak.lock();
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
        void stop_all_reader_chains() noexcept
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
        };
    };
}
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

        const auto stop_work_guard_upon_exception = make_scope_exit<exit_e::failure>(
                [&work_guard]() -> void
                {
                    work_guard.reset();
                },
                [](const std::string_view err) -> void
                {
                    std::cerr << "error in exit_failure: " << err;
                }
        );
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
            logger_t{ std::move(log_error) }
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
