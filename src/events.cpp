/*
 * Copyright © 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

std::atomic<HSA_EVENTID> g_next_event_id{1};

struct EventState {
  std::mutex mutex;
  std::condition_variable condition;
  bool signaled = false;
  bool manual_reset = false;
};

EventState *GetEventState(HsaEvent *event) {
  return reinterpret_cast<EventState *>(event->EventData.HWData1);
}

void SetSignaled(HsaEvent *event, bool signaled) {
  event->EventData.HWData3 = signaled ? 1 : 0;
}

} // namespace

HSAKMT_STATUS HSAKMTAPI hsaKmtCreateEvent(HsaEventDescriptor *EventDesc,
                                          bool ManualReset, bool IsSignaled,
                                          HsaEvent **Event) {
  CHECK_DXG_OPEN();

  if (!EventDesc || !Event)
    return HSAKMT_STATUS_INVALID_PARAMETER;

  auto *event = reinterpret_cast<HsaEvent *>(calloc(1, sizeof(HsaEvent)));
  if (!event)
    return HSAKMT_STATUS_NO_MEMORY;

  auto *state = new EventState();
  if (!state) {
    free(event);
    return HSAKMT_STATUS_NO_MEMORY;
  }
  state->signaled = IsSignaled;
  state->manual_reset = ManualReset;

  event->EventId = g_next_event_id.fetch_add(1, std::memory_order_relaxed);
  event->EventData.EventType = EventDesc->EventType;
  event->EventData.EventData.SyncVar = EventDesc->SyncVar;
  event->EventData.HWData1 = reinterpret_cast<HSAuint64>(state);
  event->EventData.HWData2 = ManualReset ? 1 : 0;
  SetSignaled(event, IsSignaled);

  *Event = event;
  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtDestroyEvent(HsaEvent *Event) {
  CHECK_DXG_OPEN();
  if (!Event)
    return HSAKMT_STATUS_SUCCESS;

  delete GetEventState(Event);

  free(Event);
  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtSetEvent(HsaEvent *Event) {
  CHECK_DXG_OPEN();
  if (!Event)
    return HSAKMT_STATUS_INVALID_HANDLE;

  auto *state = GetEventState(Event);
  if (!state)
    return HSAKMT_STATUS_INVALID_HANDLE;

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->signaled = true;
  }
  state->condition.notify_all();

  SetSignaled(Event, true);
  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtResetEvent(HsaEvent *Event) {
  CHECK_DXG_OPEN();
  if (!Event)
    return HSAKMT_STATUS_INVALID_HANDLE;

  auto *state = GetEventState(Event);
  if (!state)
    return HSAKMT_STATUS_INVALID_HANDLE;

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->signaled = false;
  }
  SetSignaled(Event, false);
  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtQueryEventState(HsaEvent *Event) {
  CHECK_DXG_OPEN();
  if (!Event)
    return HSAKMT_STATUS_INVALID_HANDLE;

  if (!GetEventState(Event))
    return HSAKMT_STATUS_INVALID_HANDLE;

  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtWaitOnEvent(HsaEvent *Event,
                                          HSAuint32 Milliseconds) {
  return hsaKmtWaitOnEvent_Ext(Event, Milliseconds, NULL);
}

HSAKMT_STATUS HSAKMTAPI hsaKmtWaitOnEvent_Ext(HsaEvent *Event,
                                              HSAuint32 Milliseconds,
                                              uint64_t *event_age) {
  if (!Event)
    return HSAKMT_STATUS_INVALID_HANDLE;

  return hsaKmtWaitOnMultipleEvents_Ext(&Event, 1, true, Milliseconds,
                                        event_age);
}

HSAKMT_STATUS HSAKMTAPI hsaKmtWaitOnMultipleEvents(HsaEvent *Events[],
                                                   HSAuint32 NumEvents,
                                                   bool WaitOnAll,
                                                   HSAuint32 Milliseconds) {
  return hsaKmtWaitOnMultipleEvents_Ext(Events, NumEvents, WaitOnAll,
                                        Milliseconds, NULL);
}

HSAKMT_STATUS HSAKMTAPI hsaKmtWaitOnMultipleEvents_Ext(HsaEvent *Events[],
                                                       HSAuint32 NumEvents,
                                                       bool WaitOnAll,
                                                       HSAuint32 Milliseconds,
                                                       uint64_t *event_age) {
  CHECK_DXG_OPEN();

  if (!Events || !NumEvents)
    return HSAKMT_STATUS_INVALID_PARAMETER;

  if (event_age)
    *event_age = 0;

  if (NumEvents == 1 && Events[0] == nullptr) {
    std::this_thread::sleep_for(std::chrono::microseconds(20));
    return HSAKMT_STATUS_SUCCESS;
  }

  auto remaining_ms = Milliseconds;
  auto start = std::chrono::steady_clock::now();
  const bool infinite = Milliseconds == HSA_EVENTTIMEOUT_INFINITE;

  if (WaitOnAll) {
    for (HSAuint32 i = 0; i < NumEvents; i++) {
      if (!Events[i] || !GetEventState(Events[i]))
        return HSAKMT_STATUS_INVALID_HANDLE;

      auto *state = GetEventState(Events[i]);
      std::unique_lock<std::mutex> lock(state->mutex);
      if (!state->signaled) {
        if (infinite) {
          state->condition.wait(lock, [state] { return state->signaled; });
        } else {
          if (!state->condition.wait_for(lock, std::chrono::milliseconds(remaining_ms),
                                         [state] { return state->signaled; })) {
            return HSAKMT_STATUS_WAIT_TIMEOUT;
          }
        }
      }

      if (!state->manual_reset)
        state->signaled = false;

      SetSignaled(Events[i], state->manual_reset ? true : false);

      if (!infinite) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = static_cast<HSAuint32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        if (elapsed_ms >= Milliseconds)
          remaining_ms = 0;
        else
          remaining_ms = Milliseconds - elapsed_ms;
      }
    }

    return HSAKMT_STATUS_SUCCESS;
  }

  while (true) {
    for (HSAuint32 i = 0; i < NumEvents; i++) {
      if (!Events[i] || !GetEventState(Events[i]))
        return HSAKMT_STATUS_INVALID_HANDLE;

      auto *state = GetEventState(Events[i]);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->signaled)
          continue;

        if (!state->manual_reset)
          state->signaled = false;

        SetSignaled(Events[i], state->manual_reset ? true : false);
        return HSAKMT_STATUS_SUCCESS;
      }
    }

    if (!infinite) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = static_cast<HSAuint32>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
      if (elapsed_ms >= Milliseconds)
        return HSAKMT_STATUS_WAIT_TIMEOUT;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

HSAKMT_STATUS HSAKMTAPI hsaKmtOpenSMI(HSAuint32 NodeId, int *fd) {
  CHECK_DXG_OPEN();
  pr_debug("node id %d\n", NodeId);
  assert(false);
  return HSAKMT_STATUS_SUCCESS;
}
