#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace st {

struct WsMessage {
  std::string data;
  bool binary = false;
};
using WsMessagePtr = std::shared_ptr<const WsMessage>;

// Fan-out for the push-only telemetry socket.
//
// A message is serialized once and dropped into each connection's own queue; the
// connection's own thread is the only thing that ever writes to its socket, so there is no
// cross-thread send race. A client that cannot keep up loses its oldest frames rather than
// stalling the publisher.
class WsHub {
 public:
  struct Client {
    std::mutex m;
    std::condition_variable cv;
    std::deque<WsMessagePtr> q;
    bool closed = false;
  };
  using ClientPtr = std::shared_ptr<Client>;

  static constexpr size_t kMaxQueued = 32;

  ClientPtr add();
  void remove(const ClientPtr& c);
  void publish(WsMessagePtr msg);
  void shutdown();

  size_t clients() const;

  // Blocks until a message is queued, the client is removed, or the timeout expires.
  // Returns nullptr on timeout or close.
  WsMessagePtr wait(const ClientPtr& c, int timeout_ms);

 private:
  mutable std::mutex m_;
  std::vector<ClientPtr> clients_;
};

}  // namespace st
