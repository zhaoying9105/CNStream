/*************************************************************************
 * Copyright (C) [2019] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include "cnstream_eventbus.hpp"

#include <list>
#include <memory>
#include <thread>
#include <utility>

#include "cnstream_pipeline.hpp"

namespace cnstream {

EventBus::~EventBus() {
  Stop();
}

bool EventBus::IsRunning() {
  return running_.load();
}

bool EventBus::Start() {
  running_.store(true);
  event_thread_ = std::thread(&EventBus::EventLoop, this);
  return true;
}

void EventBus::Stop() {
  if (IsRunning()) {
    running_.store(false);
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
  }
}

// @return The number of bus watchers that has been added to this event bus.
uint32_t EventBus::AddBusWatch(BusWatcher func, Pipeline *watcher) {
  std::lock_guard<std::mutex> lk(watcher_mtx_);
  bus_watchers_.push_front(std::make_pair(func, watcher));
  return bus_watchers_.size();
}

void EventBus::ClearAllWatchers() {
  std::lock_guard<std::mutex> lk(watcher_mtx_);
  bus_watchers_.clear();
}

const std::list<std::pair<BusWatcher, Pipeline *>> &EventBus::GetBusWatchers() const {
  return bus_watchers_;
}

bool EventBus::PostEvent(Event event) {
  if (!running_.load()) {
    LOG(WARNING) << "Post event failed, pipeline not running";
    return false;
  }
  // LOG(INFO) << "Recieve Event from [" << event.module->GetName() << "] :" << event.message;
  queue_.Push(event);
  return true;
}

Event EventBus::PollEvent() {
  Event event;
  event.type = EVENT_INVALID;
  while (running_.load()) {
    if (queue_.WaitAndTryPop(event, std::chrono::milliseconds(100))) {
      break;
    }
  }
  if (!running_.load()) event.type = EVENT_STOP;
  return event;
}

void EventBus::EventLoop() {
  const std::list<std::pair<BusWatcher, Pipeline *>> &kWatchers = GetBusWatchers();
  EventHandleFlag flag = EVENT_HANDLE_NULL;

  SetThreadName("cn-EventLoop", pthread_self());
  // start loop
  while (IsRunning()) {
    Event event = PollEvent();
    if (event.type == EVENT_INVALID) {
      LOG(INFO) << "[EventLoop] event type is invalid";
      break;
    } else if (event.type == EVENT_STOP) {
      LOG(INFO) << "[EventLoop] Get stop event";
      break;
    }
    std::unique_lock<std::mutex> lk(watcher_mtx_);
    for (auto &watcher : kWatchers) {
      flag = watcher.first(event, watcher.second);
      if (flag == EVENT_HANDLE_INTERCEPTION || flag == EVENT_HANDLE_STOP) {
        break;
      }
    }
    if (flag == EVENT_HANDLE_STOP) {
      break;
    }
  }
  LOG(INFO) << "Event bus exit.";
}

}  // namespace cnstream