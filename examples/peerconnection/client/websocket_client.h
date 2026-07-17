// websocket_client.h
#ifndef WEBSOCKET_CLIENT_H_
#define WEBSOCKET_CLIENT_H_

#include <string>
#include <functional>
#include <memory>
#include <libwebsockets.h>
#include <deque>
#include <chrono>

#include "rtc_base/thread.h"

#include "json/reader.h"
#include "json/value.h"
#include "json/writer.h"
#include "rtc_base/strings/json.h"

class WebSocketClient {
 public:
  using MessageCallback = std::function<void(const std::string&)>;
  using ConnectionCallback = std::function<void(bool)>;

  WebSocketClient();
  ~WebSocketClient();

  bool Connect(const std::string& url);
  void Close();
  bool SendMessage(const std::string& message);
  bool IsConnected() const { return is_connected_; }
  
  void SetMessageCallback(MessageCallback callback) {
    message_callback_ = std::move(callback);
  }
  
  void SetConnectionCallback(ConnectionCallback callback) {
    connection_callback_ = std::move(callback);
  }

  static int CallbackFunction(struct lws *wsi, 
                            enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

  void Service();
  bool ParseURL(const std::string& url);

  // New: Send periodic ping
  bool SendPing();

  // Keep-alive
  std::chrono::steady_clock::time_point last_ping_time_ =
      std::chrono::steady_clock::now();
  const std::chrono::seconds ping_interval_{30};  // 30-second keepalive

 private:
  void HandleCallback(struct lws *wsi, 
                     enum lws_callback_reasons reason,
                     void *user, void *in, size_t len);

  struct lws_context *context_;
  struct lws *websocket_;
  bool is_connected_;
  MessageCallback message_callback_;
  ConnectionCallback connection_callback_;
  std::deque<std::string> send_queue_;  // Queue for outgoing messages

  std::string message_buffer_;  // Buffer for accumulating partial messages
  bool receiving_message_ = false;

  std::string protocol_;
  std::string host_;
  int port_;
  std::string path_;
  std::string origin_;

  // Helper method to process complete messages
  bool ProcessCompleteMessage(const std::string& complete_message);
};

#endif  // WEBSOCKET_CLIENT_H_