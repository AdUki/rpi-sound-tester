#pragma once

#include <pthread.h>
#include <sched.h>

#include "util/log.h"

namespace st {

// Puts the calling thread on SCHED_FIFO at `prio`. Failing is never fatal — a desktop build
// without the rtprio limit just runs at normal priority and says so — because a thread that
// refuses to start over its priority takes the whole device down for a performance hint.
inline bool make_realtime(int prio, const char* who) {
  sched_param p{};
  p.sched_priority = prio;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0) {
    LOG_WARN("could not set SCHED_FIFO {} on the {} thread (running at normal priority)", prio,
             who);
    return false;
  }
  LOG_INFO("{} thread running SCHED_FIFO {}", who, prio);
  return true;
}

}  // namespace st
