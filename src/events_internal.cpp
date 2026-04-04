#include <chrono>
#include <thread>

#include "events_internal.h"

namespace wsl {
namespace thunk {
namespace events {

EventState::EventState(bool manual_reset, bool signaled)
    : signaled_(signaled), manual_reset_(manual_reset) {}

void EventState::Set() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
  }
  if (manual_reset_)
    condition_.notify_all();
  else
    condition_.notify_one();
}

void EventState::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  signaled_ = false;
}

bool EventState::IsManualReset() const { return manual_reset_; }

bool EventState::IsSignaled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return signaled_;
}

HSAKMT_STATUS EventState::Wait(HSAuint32 milliseconds) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!signaled_) {
    if (milliseconds == HSA_EVENTTIMEOUT_INFINITE) {
      condition_.wait(lock, [this] { return signaled_; });
    } else if (!condition_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                    [this] { return signaled_; })) {
      return HSAKMT_STATUS_WAIT_TIMEOUT;
    }
  }

  if (!manual_reset_)
    signaled_ = false;

  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS WaitOnMultipleEvents(EventState *states[], HSAuint32 num_events,
                                   bool wait_on_all, HSAuint32 milliseconds,
                                   uint64_t *event_age) {
  if (!states || !num_events)
    return HSAKMT_STATUS_INVALID_PARAMETER;

  if (event_age)
    *event_age = 0;

  auto remaining_ms = milliseconds;
  auto start = std::chrono::steady_clock::now();
  const bool infinite = milliseconds == HSA_EVENTTIMEOUT_INFINITE;

  if (wait_on_all) {
    for (HSAuint32 i = 0; i < num_events; i++) {
      if (!states[i])
        return HSAKMT_STATUS_INVALID_HANDLE;

      auto status =
          states[i]->Wait(infinite ? HSA_EVENTTIMEOUT_INFINITE : remaining_ms);
      if (status != HSAKMT_STATUS_SUCCESS)
        return status;

      if (!infinite) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = static_cast<HSAuint32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                .count());
        remaining_ms =
            elapsed_ms >= milliseconds ? 0 : milliseconds - elapsed_ms;
      }
    }

    return HSAKMT_STATUS_SUCCESS;
  }

  while (true) {
    for (HSAuint32 i = 0; i < num_events; i++) {
      if (!states[i])
        return HSAKMT_STATUS_INVALID_HANDLE;

      auto status = states[i]->Wait(0);
      if (status == HSAKMT_STATUS_SUCCESS)
        return status;
      if (status != HSAKMT_STATUS_WAIT_TIMEOUT)
        return status;
    }

    if (!infinite) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = static_cast<HSAuint32>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
              .count());
      if (elapsed_ms >= milliseconds)
        return HSAKMT_STATUS_WAIT_TIMEOUT;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

} // namespace events
} // namespace thunk
} // namespace wsl
