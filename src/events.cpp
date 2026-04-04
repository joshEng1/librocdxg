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
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "events_internal.h"

namespace {

std::atomic<HSA_EVENTID> g_next_event_id{1};

wsl::thunk::events::EventState *GetEventState(HsaEvent *event) {
  return reinterpret_cast<wsl::thunk::events::EventState *>(event->EventData.HWData1);
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

  auto *state = new wsl::thunk::events::EventState(ManualReset, IsSignaled);
  if (!state) {
    free(event);
    return HSAKMT_STATUS_NO_MEMORY;
  }

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

  state->Set();
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

  state->Reset();
  SetSignaled(Event, state->IsSignaled());
  return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtQueryEventState(HsaEvent *Event) {
  CHECK_DXG_OPEN();
  if (!Event)
    return HSAKMT_STATUS_INVALID_HANDLE;

  auto *state = GetEventState(Event);
  if (!state)
    return HSAKMT_STATUS_INVALID_HANDLE;

  SetSignaled(Event, state->IsSignaled());
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

  std::vector<wsl::thunk::events::EventState *> states(NumEvents);
  for (HSAuint32 i = 0; i < NumEvents; i++) {
    if (!Events[i] || !GetEventState(Events[i]))
      return HSAKMT_STATUS_INVALID_HANDLE;
    states[i] = GetEventState(Events[i]);
  }

  auto status = wsl::thunk::events::WaitOnMultipleEvents(states.data(), NumEvents,
                                                         WaitOnAll,
                                                         Milliseconds,
                                                         event_age);
  if (status == HSAKMT_STATUS_SUCCESS) {
    for (HSAuint32 i = 0; i < NumEvents; i++)
      SetSignaled(Events[i], states[i]->IsSignaled());
  }

  return status;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtOpenSMI(HSAuint32 NodeId, int *fd) {
  CHECK_DXG_OPEN();
  pr_debug("node id %d\n", NodeId);
  assert(false);
  return HSAKMT_STATUS_SUCCESS;
}
