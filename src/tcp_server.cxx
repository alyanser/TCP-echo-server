#include <iostream>
#include <algorithm>
#include <memory>
#include <random>
#include <future>
#include <asio/ssl.hpp>
#include <tcp_server.hpp>
#include <asio/dispatch.hpp>
#include <asio/steady_timer.hpp>
#include <asio/placeholders.hpp>
#include <asio/read.hpp>

tcp_server::tcp_server(uint8_t thread_count,const uint16_t listen_port,const std::string & auth_dir)
         : m_ssl_context(asio::ssl::context::sslv23), m_executor_guard(asio::make_work_guard(m_io_context)),
         m_acceptor(m_io_context), m_listen_port(listen_port), m_auth_dir(auth_dir),
         m_thread_count(std::max(thread_count,static_cast<uint8_t>(MINIMUM_THREAD_COUNT)))
{
         if(!m_auth_dir.empty() && m_auth_dir.back() != '/') m_auth_dir += '/';
}

tcp_server::~tcp_server(){
         shutdown();
}

void tcp_server::start() noexcept {
         if(m_server_running) return;
         m_server_running = true;

         auto worker_thread = [this]() noexcept {
                  while(m_server_running){
                           m_io_context.run();
                  }
         };

         if(m_thread_pool.empty()){
                  m_thread_pool.reserve(m_thread_count);

                  for(uint8_t i = 0;i < m_thread_count;i++){
                           m_thread_pool.emplace_back(std::thread(worker_thread));
                  }
         }else{
                  for(auto & thread : m_thread_pool){
                           assert(thread.joinable());
                           thread = std::thread(worker_thread);
                  }
         }

         m_logger.server_log("started with",static_cast<uint16_t>(m_thread_count),"threads");

         configure_ssl_context();
         configure_acceptor();
         asio::post(m_io_context,std::bind(&tcp_server::listen,this));
}

void tcp_server::shutdown() noexcept {
         if(!m_server_running) return;
         m_logger.server_log("shutting down");
         
         m_server_running = false;

         m_executor_guard.reset();
         m_acceptor.cancel();
         m_acceptor.close(); 
         m_io_context.stop();

         m_logger.server_log("deaf state");

         for(auto & thread : m_thread_pool){
                  assert(thread.joinable());
                  thread.join();
         }

         m_logger.server_log("shutdown");
}

void tcp_server::configure_acceptor() noexcept {
         asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::any(),m_listen_port);
         m_acceptor.open(endpoint.protocol());
         m_acceptor.set_option(asio::ip::tcp::socket::reuse_address(true));
         m_acceptor.bind(endpoint);
         m_logger.server_log("acceptor bound to port number",m_listen_port);
}

void tcp_server::configure_ssl_context() noexcept {
         m_ssl_context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::verify_peer);
         m_ssl_context.use_certificate_file(m_auth_dir + "certificate.pem",asio::ssl::context_base::pem);
         m_ssl_context.use_rsa_private_key_file(m_auth_dir + "private_key.pem",asio::ssl::context_base::pem);
}

// timeout for the acceptor - prevents further connections for TIMEOUT_SECONDS
void tcp_server::connection_timeout() noexcept {
         m_acceptor.cancel();
         m_logger.server_log("deaf state");

         auto timeout_timer = std::make_shared<asio::steady_timer>(m_io_context);

         timeout_timer->expires_from_now(std::chrono::seconds(TIMEOUT_SECONDS));

         timeout_timer->async_wait([this,timeout_timer](const asio::error_code & error_code) noexcept {
                  if(!error_code){
                           m_logger.server_log("connection timeout over. shifting to listening state");
                           asio::post(m_io_context,std::bind(&tcp_server::listen,this));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                  }
         });
}

void tcp_server::shutdown_socket(std::shared_ptr<ssl_tcp_socket> ssl_socket,const uint64_t client_id) noexcept {
         {
                  std::lock_guard client_id_guard(m_client_id_mutex);
                  // current client should be in active clients
                  assert(m_active_client_ids.count(client_id));
                  m_active_client_ids.erase(client_id);
         }

         ssl_socket->lowest_layer().shutdown(tcp_socket::shutdown_both); // shutdown read and write
         ssl_socket->lowest_layer().close();
         --m_active_connections;
         m_logger.server_log("connection closed with client [",client_id,']');
}

// called when a new client attempts to connect
void tcp_server::listen() noexcept {
         m_acceptor.listen();
         m_logger.server_log("listening state");

         if(m_active_connections > MAX_CONNECTIONS){
                  m_logger.error_log("max connections reached. taking a connection timeout for",TIMEOUT_SECONDS,"seconds");
                  asio::post(m_io_context,std::bind(&tcp_server::connection_timeout,this));
                  return;
         }

         // get a spare client id meanwhile
         using task_type = std::packaged_task<uint64_t()>;
         auto client_id_task = std::make_shared<task_type>(std::bind(&tcp_server::get_spare_id,this));
         asio::post(m_io_context,std::bind(&task_type::operator(),client_id_task));

         auto ssl_socket = std::make_shared<ssl_tcp_socket>(m_io_context,m_ssl_context);

         auto on_connection_attempt = [this,ssl_socket,client_id_task](const asio::error_code & error_code) noexcept {
                  if(!error_code){
                           // client attempting to connect - get the id
                           auto new_client_id = client_id_task->get_future().get();

                           {
                                    std::lock_guard client_id_guard(m_client_id_mutex);
                                    m_active_client_ids.insert(new_client_id);
                           }

                           m_logger.server_log("new client [",new_client_id,"] attempting to connect. handshake pending");
                           // forward client to different thread to perform handshake and wait for next client
                           asio::post(m_io_context,std::bind(&tcp_server::attempt_handshake,this,ssl_socket,new_client_id));
                           // post for the next client
                           asio::post(m_io_context,std::bind(&tcp_server::listen,this));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           // don't have to shutdown socket
                  }
         };

         m_acceptor.async_accept(ssl_socket->lowest_layer(),on_connection_attempt);
}

void tcp_server::process_request(std::shared_ptr<ssl_tcp_socket> ssl_socket,std::shared_ptr<std::string> request,const uint64_t client_id,
         const bool eof) noexcept 
{
         m_logger.server_log("processing request from client [",client_id,']');
         m_logger.client_log(client_id,*request);

         auto on_write = [this,ssl_socket,client_id,request,eof](const asio::error_code & error_code,const auto bytes_written){
                  if(!error_code){
                           // successful write
                           m_logger.server_log(bytes_written," bytes sent to client [",client_id,']');
                           // if the last read included end of file then terminate the connection otherwise post for further reading
                           if(eof){
                                    asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                           }else{
                                    asio::post(m_io_context,std::bind(&tcp_server::read_request,this,ssl_socket,client_id));
                           }
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         // echo the same request back
         asio::async_write(*ssl_socket,asio::buffer(*request),on_write);
}

void tcp_server::read_request(std::shared_ptr<ssl_tcp_socket> ssl_socket,const uint64_t client_id) noexcept {

         auto on_read_wait_over = [this,ssl_socket,client_id](const asio::error_code & error_code) noexcept {
                  if(!error_code){
                           // ready to read
                           m_logger.server_log("request received from client [",client_id,']');
                           auto read_buffer = std::make_shared<std::string>(ssl_socket->lowest_layer().available(),NULL_BYTE);

                           auto on_read = [this,ssl_socket,read_buffer,client_id](const asio::error_code & error_code,auto /* bytes_read */) noexcept {
                                    if(!error_code || error_code == asio::error::eof){
                                             asio::post(m_io_context,std::bind(&tcp_server::process_request,this,ssl_socket,read_buffer,client_id,
                                                      error_code == asio::error::eof));
                                    }else{
                                             m_logger.error_log(error_code,error_code.message());
                                             asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                                    }
                           };

                           asio::async_read(*ssl_socket,asio::buffer(*read_buffer),on_read);
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         ssl_socket->lowest_layer().async_wait(tcp_socket::wait_read,on_read_wait_over);
}

void tcp_server::attempt_handshake(std::shared_ptr<ssl_tcp_socket> ssl_socket,const uint64_t client_id) noexcept {
         
         auto on_handshake = [this,ssl_socket,client_id](const asio::error_code & error_code) noexcept {
                  if(!error_code){
                           m_logger.server_log("handshake successful with client [",client_id,']');
                           ++m_active_connections;
                           asio::post(m_io_context,std::bind(&tcp_server::read_request,this,ssl_socket,client_id));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         m_logger.server_log("handshake attempt with client [",client_id,']');
         ssl_socket->async_handshake(asio::ssl::stream_base::handshake_type::server,on_handshake);
}

uint64_t tcp_server::get_spare_id() const noexcept {
         static std::mt19937 generator(std::random_device{}());
         static std::uniform_int_distribution<uint64_t> id_range(0,std::numeric_limits<uint64_t>::max());

         uint64_t unique_id;
         std::lock_guard client_id_guard(m_client_id_mutex);
         
         do{
                  unique_id = id_range(generator);
         }while(m_active_client_ids.count(unique_id));

         return unique_id;
}