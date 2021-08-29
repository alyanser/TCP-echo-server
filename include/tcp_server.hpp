#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP
#pragma once

#include <vector>
#include <mutex>
#include <thread>
#include <asio/io_context.hpp>
#include <dns_resolver.hpp>
#include <asio/ssl.hpp>
#include <asio/executor_work_guard.hpp>

class tcp_server {
         using tcp_socket = asio::ip::tcp::socket;
public:
         tcp_server(uint8_t thread_count,uint16_t listen_port);
         tcp_server(const tcp_server & rhs) = delete;
         tcp_server(tcp_server && rhs) = delete;
         ~tcp_server();

         void listen();
         void handle_client(std::shared_ptr<asio::ssl::stream<tcp_socket>> ssl_stream);
private:
         asio::io_context m_io_context;
         asio::ssl::context m_ssl_context;
         asio::executor_work_guard<asio::io_context::executor_type> m_executor_guard;
         dns_resolver m_resolver;
         std::vector<std::thread> m_thread_pool;
         std::mutex m_mutex;
         const uint16_t m_listen_port;
};

#endif