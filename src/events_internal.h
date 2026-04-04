#ifndef LIBROCDXG_EVENTS_INTERNAL_H
#define LIBROCDXG_EVENTS_INTERNAL_H

#include <condition_variable>
#include <mutex>

#include "hsakmt/hsakmt.h"

namespace wsl {
namespace thunk {
namespace events {

class EventState {
public:
  EventState(bool manual_reset, bool signaled);

  void Set();
  void Reset();
  bool IsManualReset() const;
  bool IsSignaled() const;
  HSAKMT_STATUS Wait(HSAuint32 milliseconds);

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool signaled_;
  bool manual_reset_;
};

HSAKMT_STATUS WaitOnMultipleEvents(EventState *states[], HSAuint32 num_events,
                                   bool wait_on_all, HSAuint32 milliseconds,
                                   uint64_t *event_age);

} // namespace events
} // namespace thunk
} // namespace wsl

#endif
