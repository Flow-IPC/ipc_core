/* Flow-IPC: Core
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

/// @file

#include "ipc/util/sync_io/detail/timer_ev_emitter.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <boost/move/make_unique.hpp>

namespace ipc::util::sync_io
{

// Implementations.

Timer_event_emitter::Timer_event_emitter(flow::log::Logger* logger_ptr, String_view nickname_str) :
  flow::log::Log_context(logger_ptr, Log_component::S_UTIL),
  m_nickname(nickname_str),
  m_worker(get_logger(), std::string("tmr_ev-") + m_nickname)
{
  m_worker.start();
  FLOW_LOG_TRACE("Timer_event_emitter [" << *this << "]: Idle timer-emitter thread started.");
}

flow::util::Timer Timer_event_emitter::create_timer()
{
  using flow::util::Timer;

  return Timer(*(m_worker.task_engine()));
}

Timer_event_emitter::Timer_fired_read_end* Timer_event_emitter::create_timer_signal_pipe()
{
  using boost::asio::writable_pipe;
  using boost::asio::connect_pipe;
  using boost::movelib::make_unique;

  auto read_end = make_unique<Timer_fired_read_end>(*(m_worker.task_engine()));
  auto write_end = make_unique<writable_pipe>(*(m_worker.task_engine()));
  Error_code sys_err_code;
  connect_pipe(*read_end, *write_end, sys_err_code);

  if (sys_err_code)
  {
    // @todo Someday we should report this as an error, etc.  Low-priority, as at this stage everything is kaput.
    FLOW_LOG_FATAL("connect_pipe() failed; this should never happen.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_FATAL();
    assert(false && "connect_pipe() failed; this should never happen.  Details follow."); std::abort();
    return nullptr;
  }
  // else

  auto& read_end_ref = m_signal_pipe_readers.emplace_back(std::move(read_end));

#ifndef NDEBUG
  const auto result =
#endif
  m_signal_pipe_writers.insert({ read_end_ref.get(),
                                 std::move(write_end) });
  assert(result.second);

  FLOW_LOG_TRACE("Timer_event_emitter [" << *this << "]: Pipe created; read-end ptr = [" << read_end_ref.get() << "].");

  return read_end_ref.get();
} // Timer_event_emitter::create_timer_signal_pipe()

void Timer_event_emitter::timer_async_wait(flow::util::Timer* timer, Timer_fired_read_end* read_end)
{
  FLOW_LOG_TRACE("Timer_event_emitter [" << *this << "]: Starting timer async-wait; when/if it fires, "
                 "we will write to write-end corresponding to read-end ptr = [" << read_end << "].");

  /* Careful: this is a user thread, but the handler below is in our worker thread.
   * Accessing m_signal_pipe_writers inside there would be not thread-safe, as if some timer happens to fire,
   * while they (say) create_timer_signal_pipe() for another future timer's purposes, things could explode.
   * Save the value now; it is accurate and guaranteed valid until `*this` dies (which can only happen
   * after thread is joined, meaning handler couldn't possibly be executing). */
  const auto it = m_signal_pipe_writers.find(read_end);
  assert((it != m_signal_pipe_writers.end()) && "timer_async_wait() invoked on read_end from another *this?");
  const auto write_end = it->second.get();

  timer->async_wait([this, write_end, read_end](const Error_code& async_err_code)
  {
    auto sys_err_code = async_err_code;

    if (sys_err_code == boost::asio::error::operation_aborted)
    {
      // As promised we only report actual firings.  Stuff is shutting down, or timer canceled.  GTFO.
      return;
    }
    // else

    if (sys_err_code)
    {
      /* This decision (to simply pretend it was a success code) is borrowed from flow::async::schedule_*().
       * assert() is another option, but use Flow wisdom.  Note, e.g., snd_auto_ping_now() does the same. */
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      FLOW_LOG_WARNING("Timer_event_emitter [" << *this << "]: "
                       "Timer system error; just logged; totally unexpected; pretending it fired normally.");
    }

    FLOW_LOG_TRACE("Timer_event_emitter [" << *this << "]: Timer fired.  Pushing byte to pipe; "
                   "read_end ptr = [" << read_end << "].  User code should detect event soon and cause byte "
                   "to be popped.");

    util::pipe_produce(get_logger(), write_end);
  }); // timer->async_wait()
} // Timer_event_emitter::timer_async_wait()

void Timer_event_emitter::consume_timer_firing_signal(Timer_fired_read_end* read_end)
{
  FLOW_LOG_TRACE("Timer_event_emitter [" << *this << "]: Timer presumably fired and pushed byte to pipe; "
                 "we now pop it.");

  util::pipe_consume(get_logger(), read_end);
} // Timer_event_emitter::consume_timer_firing_signal()

std::ostream& operator<<(std::ostream& os, const Timer_event_emitter& val)
{
  return
    os << '[' << val.m_nickname << "]@" << static_cast<const void*>(&val);

}

} // namespace ipc::util::sync_io
