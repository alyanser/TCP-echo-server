#include <algorithm>
#include <future>
#include <asio/dispatch.hpp>
#include <asio/steady_timer.hpp>
#include <asio/read.hpp>
#include <tcp_server.hxx>

void tcp_server::start() noexcept {
         if(m_server_running) return;

         m_server_running = true;

         auto worker_thread = [&m_io_context = m_io_context,&m_server_running = m_server_running]() noexcept {
                  while(m_server_running){
                           m_io_context.run();
                  }
         };

         for(uint8_t i = 0;i < m_thread_count;i++){
                  asio::post(m_thread_pool,worker_thread);
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
         m_thread_pool.join();

         m_logger.server_log("shutdown");
}

void tcp_server::connection_timeout() noexcept {
         m_acceptor.cancel();
         m_logger.server_log("deaf state");

         auto timeout_timer = std::make_shared<asio::steady_timer>(m_io_context);

         timeout_timer->expires_from_now(std::chrono::seconds(timeout_seconds));

         timeout_timer->async_wait([this,timeout_timer](const auto & error_code) noexcept {
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
                  assert(m_active_client_ids.count(client_id));
                  m_active_client_ids.erase(client_id);
         }

         try{
                  ssl_socket->lowest_layer().shutdown(tcp_socket::shutdown_both);
                  ssl_socket->lowest_layer().close();
         }catch(const std::system_error & error){
                  m_logger.error_log(error.what());
         }

         --m_active_connections;
         m_logger.server_log("connection closed with client [",client_id,']');
}

void tcp_server::listen() noexcept {
         m_acceptor.listen();
         m_logger.server_log("listening state");

         if(m_active_connections > max_connections){
                  m_logger.error_log("max connections reached. taking a connection timeout for",timeout_seconds,"seconds");
                  asio::post(m_io_context,std::bind(&tcp_server::connection_timeout,this));
                  return;
         }

         using id_task_type = std::packaged_task<uint64_t()>;

         auto client_id_task = std::make_shared<id_task_type>(std::bind(&tcp_server::get_spare_id,this));
         asio::post(m_io_context,std::bind(&id_task_type::operator(),client_id_task));

         auto ssl_socket = std::make_shared<ssl_tcp_socket>(m_io_context,m_ssl_context);

         auto on_connection_attempt = [this,ssl_socket,client_id_task](const auto & error_code) noexcept {
                  if(!error_code){
                           auto new_client_id = client_id_task->get_future().get();

                           {
                                    std::lock_guard client_id_guard(m_client_id_mutex);
                                    m_active_client_ids.insert(new_client_id);
                           }

                           m_logger.server_log("new client [",new_client_id,"] attempting to connect. handshake pending");
                         
                           asio::post(m_io_context,std::bind(&tcp_server::attempt_handshake,this,ssl_socket,new_client_id));
                           asio::post(m_io_context,std::bind(&tcp_server::listen,this));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           // socket could not connect - no shutdown required
                  }
         };

         m_acceptor.async_accept(ssl_socket->lowest_layer(),on_connection_attempt);
}

void tcp_server::process_message(std::shared_ptr<ssl_tcp_socket> ssl_socket,std::shared_ptr<std::string> message,const uint64_t client_id,
         const asio::error_code & connection_code) noexcept
{
         auto on_write = [this,ssl_socket,client_id](const auto & error_code,const auto bytes_sent) noexcept {
                  if(!error_code){
                           m_logger.server_log(bytes_sent,"bytes sent to client [",client_id,']');
                           {
                                    std::shared_lock received_messages_guard(m_received_messages_mutex);

                                    auto & all_client_messages = m_received_messages[client_id];
                                    assert(m_received_messages.count(client_id));

                                    received_messages_guard.unlock();

                                    m_logger.send_log(client_id,all_client_messages);

                                    received_messages_guard.lock();
                                    all_client_messages.clear();
                           }
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                  }
                  
                  asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
         };

         m_logger.server_log("processing message from client [",client_id,']');
         m_logger.receive_log(client_id,*message);

         if(m_received_messages.count(client_id)){
                  std::shared_lock received_messages_guard(m_received_messages_mutex);
                  m_received_messages[client_id] += std::move(*message);
         }else{
                  std::unique_lock received_messages_guard(m_received_messages_mutex);
                  m_received_messages[client_id] = std::move(*message);
         }

         
         if(connection_code){
                  asio::async_write(*ssl_socket,asio::buffer(m_received_messages[client_id]),on_write);
         }else{
                  asio::post(m_io_context,std::bind(&tcp_server::read_message,this,ssl_socket,client_id));
         }
}

void tcp_server::read_message(std::shared_ptr<ssl_tcp_socket> ssl_socket,const uint64_t client_id) noexcept {

         auto on_read = [this,ssl_socket,client_id](auto read_buffer,const auto & error_code,const auto bytes_read) noexcept {
                  if(!error_code && bytes_read){
                           asio::error_code connection_code;

                           if(error_code.value() == asio::error::eof || error_code.value() == asio::error::no_permission){
                                    connection_code = asio::error::network_down;
                           }

                           asio::post(m_io_context,std::bind(&tcp_server::process_message,this,ssl_socket,read_buffer,client_id,connection_code));
                           return;
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         auto on_read_wait_over = [this,ssl_socket,client_id,on_read](const auto & error_code) noexcept {
                  if(!error_code){
                           m_logger.server_log("message received from client [",client_id,']');

                           auto read_buffer = std::make_shared<std::string>(ssl_socket->lowest_layer().available(),'\0');

                           using std::placeholders::_1;
                           using std::placeholders::_2;

                           asio::async_read(*ssl_socket,asio::buffer(*read_buffer),std::bind(on_read,read_buffer,_1,_2));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         ssl_socket->lowest_layer().async_wait(tcp_socket::wait_read,on_read_wait_over);
}

void tcp_server::attempt_handshake(std::shared_ptr<ssl_tcp_socket> ssl_socket,const uint64_t client_id) noexcept {
         
         auto on_handshake = [this,ssl_socket,client_id](const auto & error_code) noexcept {
                  if(!error_code){
                           m_logger.server_log("handshake successful with client [",client_id,']');
                           ++m_active_connections;
                           asio::post(m_io_context,std::bind(&tcp_server::read_message,this,ssl_socket,client_id));
                  }else{
                           m_logger.error_log(error_code,error_code.message());
                           asio::post(m_io_context,std::bind(&tcp_server::shutdown_socket,this,ssl_socket,client_id));
                  }
         };

         m_logger.server_log("handshake attempt with client [",client_id,']');
         ssl_socket->async_handshake(asio::ssl::stream_base::handshake_type::server,on_handshake);
}

uint64_t tcp_server::get_spare_id() const noexcept {
         uint64_t unique_id;
         std::shared_lock client_id_guard(m_client_id_mutex);
         
         do{
                  unique_id = id_range(generator);
         }while(m_active_client_ids.count(unique_id));

         return unique_id;
}