add_library(aws_tcp STATIC
        server.cpp
        server.hpp
        server_impl/reader_chain.cpp
        server_impl/reader_chain.hpp
        server_impl/acceptor_chain.cpp
        server_impl/acceptor_chain.hpp
        server_impl/asio_declarations.hpp
        util/logger_t.hpp
        util/oneshot_wait.hpp
        util/thread_safe_vec.hpp)
