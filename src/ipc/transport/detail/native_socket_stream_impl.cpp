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
#include "ipc/transport/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/error.hpp"
#include "flow/error/error.hpp"
#include <boost/move/make_unique.hpp>
#include <cstddef>

namespace ipc::transport
{

// Implementations.

// General + connect-ops.

// Delegated-to ctor (note the tag arg).
Native_socket_stream::Impl::Impl(sync_io::Native_socket_stream&& sync_io_core_moved, std::nullptr_t) :
  flow::log::Log_context(sync_io_core_moved.get_logger(), Log_component::S_TRANSPORT),

  m_worker(boost::movelib::make_unique<flow::async::Single_thread_task_loop>
             (get_logger(),
              /* (Linux) OS thread name will truncate .nickname() to 15-4=11 chars here; high chance that'll include
               * something decently useful; probably not everything though; depends on nickname.
               * It's a decent attempt. */
              flow::util::ostream_op_string("Sck-", sync_io_core_moved.nickname()))),
  // Adopt the just-cted, idle sync_io::Native_socket_stream.  It may be in NULL state or PEER state.
  m_sync_io(std::move(sync_io_core_moved))
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::async::reset_this_thread_pinning;

  m_worker->start(reset_this_thread_pinning); // Don't inherit any strange core-affinity!  Worker must float free.

  // We're using a boost.asio event loop, so we need to base the async-waited-on handles on our Task_engine.
#ifndef NDEBUG
  bool ok =
#endif
  m_sync_io.replace_event_wait_handles([this]() -> Asio_waitable_native_handle
                                         { return Asio_waitable_native_handle(*(m_worker->task_engine())); });
  assert(ok && "Did you break contract by passing-in a non-fresh sync_io core object to ctor?");

  /* Delegating PEER-state ctor shall deal with m_snd_sync_io_adapter/rcv.
   * Otherwise NULL-state ctor shall do no such thing, but a successful sync_connect() will do just that. */

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Created (NULL state).");
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  // Create core ourselves (NULL state); then delegate to other ctor.
  Impl(sync_io::Native_socket_stream(logger_ptr, nickname_str), nullptr)
{
  // Done.
}

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 Native_handle&& native_peer_socket_moved) :
  // Create core ourselves (in PEER state); then delegate to other ctor.
  Impl(sync_io::Native_socket_stream(logger_ptr, nickname_str, std::move(native_peer_socket_moved)), nullptr)
{
  using flow::util::ostream_op_string;

  // Lastly, as we're in PEER state, set up send-ops and receive-ops state machines.

  const auto log_pfx = ostream_op_string("Sck-", nickname()); // Brief-ish for use in OS thread names or some such.
  m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Created (PEER state) directly from pre-opened native handle.");
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(sync_io::Native_socket_stream&& sync_io_core_in_peer_state_moved) :
  // Adopt the PEER-state core given to us by user.
  Impl(std::move(sync_io_core_in_peer_state_moved), nullptr)
{
  using flow::util::ostream_op_string;

  // Lastly, as we're in PEER state, set up send-ops and receive-ops state machines.

  const auto log_pfx = ostream_op_string("Socket stream [", *this, ']');
  m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: "
                 "Created (PEER state) by adopting fresh sync_io::Native_socket_stream core.");
}

Native_socket_stream::Impl::~Impl()
{
  using flow::async::Single_thread_task_loop;
  using flow::async::reset_thread_pinning;
  using flow::util::ostream_op_string;

  // We are in thread U.  By contract in doc header, they must not call us from a completion handler (thread W).

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Shutting down.  All our "
                "internal async handlers will be canceled; and worker thread will be joined.");

  /* This (1) stop()s the Task_engine thus possibly
   * preventing any more handlers from running at all (any handler possibly running now is the last one to run); (2)
   * at that point Task_engine::run() exits, hence thread W exits; (3) joins thread W (waits for it to
   * exit); (4) returns.  That's a lot, but it's non-blocking. */
  m_worker->stop();
  // Thread W is (synchronously!) no more.

  /* As we promised in doc header(s), the destruction of *this shall cause any registered completion
   * handlers (namely async_end_sending(), async_receive_*() ones)
   * to be called from some unspecified thread(s) that aren't thread U, and once
   * they've all finished, the dtor returns.  So which thread should we use?  Well, we always use thread W for handlers,
   * so we should use it here too, right?  Answer: Well, we can't; W is finished.  Should we have kept it around,
   * m_worker.post()ed a task that'd call them all synchronously (from thread W) and then satisfied a
   * unique_future/promise; then here in thread U dtor awaited for this promise to be satisfied?  I (ygoldfel)
   * considered it, but that would mean first letting any other queued handlers run first as usual in boost.asio, before
   * we'd get to the user-handler-calling task.  That might work out fine, but (1) the entropy involved is a bit ???;
   * and hence (2) it's stressful trying to reason about it.  So I'd rather not.  (On the other hand arguably that's
   * equivalent to the destructor being called a little bit later during an async gap.  Still, let's see if there's a
   * clearly safer alternative.)
   *
   * Therefore, we'll just start a transient thread and take care of business.  It is, after all, an unspecified thread
   * that isn't thread U, as promised.  Since the thread owning various m_* resources below is finished, we can
   * access them from this new thread without issue.  Perf should be fine; even if this were a heavy-weight op, we
   * will not be called often; in any case it's at least a non-blocking op.
   *
   * Annoyingly, that's not all.  Consider async_receive_native_handle(F).  In thread W we will invoke F() on,
   * say, successful receive.  However, to avoid recursive mayhem (wherein we call user F(), user F() starts
   * another async_receive_*() in thread W, which modifies our state while still inside our internal thread-W code,
   * etc.): we always m_worker.post(F) by itself -- even though we are already in thread W.  Now
   * suppose our .stop() above stops/joins thread W before that post()ed thing even ran.  Then the on_done_func
   * (user handler) captured by the real-work lambda will just get thrown out; it won't be in
   * m_rcv_sync_io_adapter.m_pending_user_requests_q anymore (popped from there before getting post()ed), so it won't
   * get manually invoked below with S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER error.  What to do? Well, the problem
   * is not too different conceptually from the one solved using m_pending_user_requests_q though: a queue tracks the
   * requests; it's just that in this case the queue is inside m_worker (really inside a boost.asio Task_engine), and
   * the queued items are post()ed tasks, and the queue push op is post().  So we just want to invoke those tasks,
   * as-if from thread W, first.  This is equivalent to those tasks simply having executed just before the
   * dtor got called.  Fortunately boost.asio Task_engine has just the API for it: restart() undoes the .stop(),
   * in that Task_engine::stopped() becomes false and allows us to call Task_engine::poll() which will
   * *synchronously* execute any post()ed/not-yet-invoked tasks.  (It's like Task_engine::run(), except it doesn't block
   * waiting for async work to actually get done.)
   *
   * Anyway!  Formally m_snd/rcv_sync_io_adapter requires us to do that .restart()/.poll() for the above reason.
   * So we do it.  Lastly m_snd/rcv_sync_io_adapter's dtors will do the one-off thread thing
   * for their respective sets of pending completion handlers if any. */

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Continuing shutdown.  Next we will run pending handlers from some "
                "other thread.  In this user thread we will await those handlers' completion and then return.");

  Single_thread_task_loop one_thread(get_logger(), ostream_op_string("SckDeinit-", nickname()));
  one_thread.start([&]()
  {
    reset_thread_pinning(get_logger()); // Don't inherit any strange core-affinity.  Float free.

    FLOW_LOG_INFO("Socket stream [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending internal handlers (typically none).");

    const auto task_engine = m_worker->task_engine();
    task_engine->restart();
    const auto count = task_engine->poll();
    if (count != 0)
    {
      FLOW_LOG_INFO("Socket stream [" << *this << "]: "
                    "In transient finisher thread: Ran [" << count << "] internal handlers after all.");
    }
    task_engine->stop();

    /* When m_*_sync_io_adapter dtors auto-execute, they'll issue OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER to
     * their handlers if any. */
    FLOW_LOG_INFO("Transient finisher exiting.  (Send-ops and receive-ops de-init may follow.)");
  }); // one_thread.start()
  // Here thread exits/joins synchronously.  (But the adapters might run their own similar ones.)
} // Native_socket_stream::Impl::~Impl()

bool Native_socket_stream::Impl::sync_connect(const Shared_name& absolute_name, Error_code* err_code)
{
  using flow::util::ostream_op_string;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, sync_connect, absolute_name, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!m_sync_io.sync_connect(absolute_name, err_code))
  {
    return false; // Wasn't in NULL state.
  }
  // else:

  if (!*err_code)
  {
    // PEER state!  Yay!  Do the thing PEER-state ctor would have done.
    const auto log_pfx = ostream_op_string("Socket stream [", *this, ']');
    m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
    m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  }
  // else { Back in NULL state.  Perhaps they'll sync_connect() again later. }

  return true;
} // Native_socket_stream::Impl::sync_connect()

util::Process_credentials
  Native_socket_stream::Impl::remote_peer_process_credentials(Error_code* err_code) const
{
  return m_sync_io.remote_peer_process_credentials(err_code);
}

const std::string& Native_socket_stream::Impl::nickname() const
{
  return m_sync_io.nickname();
}

std::ostream& operator<<(std::ostream& os, const Native_socket_stream::Impl& val)
{
  return os << '[' << val.nickname() << "]@" << static_cast<const void*>(&val);
}

// Send-ops.

bool Native_socket_stream::Impl::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  return m_snd_sync_io_adapter
           ? (m_snd_sync_io_adapter->send_blob(blob, err_code), true) // It's void.
           : false; // Not in PEER state (ditto all over the place below).
}

bool Native_socket_stream::Impl::send_native_handle(Native_handle hndl, const util::Blob_const& meta_blob,
                                                    Error_code* err_code)
{
  return m_snd_sync_io_adapter
           ? (m_snd_sync_io_adapter->send_native_handle(hndl, meta_blob, err_code), true) // It's void.
           : false;
}

bool Native_socket_stream::Impl::end_sending()
{
  using flow::async::Task_asio_err;

  return async_end_sending(Task_asio_err());
}

bool Native_socket_stream::Impl::async_end_sending(flow::async::Task_asio_err&& on_done_func)
{
  return m_snd_sync_io_adapter
           ? m_snd_sync_io_adapter->async_end_sending(std::move(on_done_func))
           : false;
}

bool Native_socket_stream::Impl::auto_ping(util::Fine_duration period)
{
  return m_snd_sync_io_adapter
           ? m_snd_sync_io_adapter->auto_ping(period)
           : false;
}

size_t Native_socket_stream::Impl::send_meta_blob_max_size() const
{
  return send_blob_max_size();
}

size_t Native_socket_stream::Impl::send_blob_max_size() const
{
  return m_sync_io.send_blob_max_size();
}

// Receive-ops.

bool Native_socket_stream::Impl::async_receive_native_handle(Native_handle* target_hndl,
                                                             const util::Blob_mutable& target_meta_blob,
                                                             flow::async::Task_asio_err_sz&& on_done_func)
{
  return m_rcv_sync_io_adapter
           ? (m_rcv_sync_io_adapter->async_receive_native_handle
                (target_hndl, target_meta_blob, std::move(on_done_func)),
              true) // It's void.
           : false;
}

bool Native_socket_stream::Impl::async_receive_blob(const util::Blob_mutable& target_blob,
                                                    flow::async::Task_asio_err_sz&& on_done_func)
{
  return m_rcv_sync_io_adapter
           ? (m_rcv_sync_io_adapter->async_receive_blob(target_blob, std::move(on_done_func)), true) // It's void.
           : false;
}

bool Native_socket_stream::Impl::idle_timer_run(util::Fine_duration timeout)
{
  return m_rcv_sync_io_adapter
           ? m_rcv_sync_io_adapter->idle_timer_run(timeout)
           : false;
}

size_t Native_socket_stream::Impl::receive_meta_blob_max_size() const
{
  return receive_blob_max_size();
}

size_t Native_socket_stream::Impl::receive_blob_max_size() const
{
  return m_sync_io.receive_blob_max_size();
}

#if 0 // See the declaration in class { body }; explains why `if 0` yet still here.
sync_io::Native_socket_stream Native_socket_stream::Impl::release()
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::util::ostream_op_string;
  using flow::async::reset_this_thread_pinning;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Releasing idle-state object to new socket-stream core object.");

  // Keep in sync with NULL-state ctor!

  assert(m_snd_sync_io_adapter && m_rcv_sync_io_adapter && "By contract must be in PEER state.");

  /* The main thing is, we've got a PEER-state core m_sync_io; but it already is in non-as-if-just-cted state,
   * because we've executed start_*_ops() on it.  However m_sync_io.release() gets us that as-if-just-cted guy
   * and makes m_sync_io as-if-default-cted.
   *
   * So we invoke that. */
  auto core = m_sync_io.release();

  /* Cool.  That's done.  Now we need to get the rest of *this into as-if-default-cted form.
   * We need to basically repeat what a NULL-state ctor that takes null-Logger and empty nickname would do.
   * First up is Mr. m_worker Single_thread_task_loop guy.  `m_worker = make_unique<>(...)` would do it,
   * but we can't let the existing one be destroyed until m_sync_io* ops below are finished, because they
   * access things attached to m_worker->task_engine() before switching those things out.  So save it. */
  auto prev_worker = std::move(m_worker);

  // Now repeat what ctor does.
  m_worker
    = boost::movelib::make_unique<flow::async::Single_thread_task_loop>
        (core.get_logger(), core.nickname());
  m_worker->start(reset_this_thread_pinning);

  // m_sync_io is as-if-just-cted, so do what ctor does regarding .replace_event_wait_handles().
#ifndef NDEBUG
  bool ok =
#endif
  m_sync_io.replace_event_wait_handles([this]() -> Asio_waitable_native_handle
                                         { return Asio_waitable_native_handle(*(m_worker->task_engine())); });
  assert(ok && "Should work if m_sync_io.release() worked as advertised.");

  /* Lastly do what (NULL-state) ctor does regarding m_*_sync_io_adapter... namely leaves them null.
   * That is we must .reset() their optional<>s.  However this will run their dtors.  These will basically no-op,
   * since in we've-done-nothing-since-entering-PEER-state state by contract -- so there will be no pending
   * handlers to execute or anything.  To prepare for their dtors to run, we do what our dtor does namely
   * m_worker->stop(); except what they think of as m_worker (by reference) is prev_worker. */
  prev_worker->stop();
  // Now this should be fine.
  m_rcv_sync_io_adapter.reset();
  m_snd_sync_io_adapter.reset();

  return core;
} // Native_socket_stream::Impl::release()
#endif

} // namespace ipc::transport
