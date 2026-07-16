#include "ws_hub.h"

#include <algorithm>
#include <chrono>

namespace st {

WsHub::ClientPtr WsHub::add() {
  auto c = std::make_shared<Client>();
  std::lock_guard<std::mutex> lock(m_);
  clients_.push_back(c);
  return c;
}

void WsHub::remove(const ClientPtr& c) {
  {
    std::lock_guard<std::mutex> lock(m_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
  }
  std::lock_guard<std::mutex> lock(c->m);
  c->closed = true;
  c->cv.notify_all();
}

void WsHub::publish(WsMessagePtr msg) {
  std::vector<ClientPtr> snapshot;
  {
    std::lock_guard<std::mutex> lock(m_);
    snapshot = clients_;
  }
  for (const auto& c : snapshot) {
    std::lock_guard<std::mutex> lock(c->m);
    if (c->closed) continue;
    if (c->q.size() >= kMaxQueued) c->q.pop_front();
    c->q.push_back(msg);
    c->cv.notify_one();
  }
}

void WsHub::shutdown() {
  std::vector<ClientPtr> snapshot;
  {
    std::lock_guard<std::mutex> lock(m_);
    snapshot = clients_;
    clients_.clear();
  }
  for (const auto& c : snapshot) {
    std::lock_guard<std::mutex> lock(c->m);
    c->closed = true;
    c->cv.notify_all();
  }
}

size_t WsHub::clients() const {
  std::lock_guard<std::mutex> lock(m_);
  return clients_.size();
}

WsMessagePtr WsHub::wait(const ClientPtr& c, int timeout_ms) {
  std::unique_lock<std::mutex> lock(c->m);
  if (c->q.empty() && !c->closed) {
    c->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                   [&c] { return !c->q.empty() || c->closed; });
  }
  if (c->q.empty()) return nullptr;
  WsMessagePtr m = c->q.front();
  c->q.pop_front();
  return m;
}

}  // namespace st
