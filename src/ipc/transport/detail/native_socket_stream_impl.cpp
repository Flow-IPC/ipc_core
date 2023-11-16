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
#include <cstddef>

namespace ipc::transport
{

// Implementations.

// General + connect-ops.

// Delegated-to ctor (note the tag arg).
Native_socket_stream::Impl::Impl(sync_io::Native_socket_stream&& sync_io_core_moved, std::nullptr_t) :
  flow::log::Log_context(sync_io_core_moved.get_logger(), Log_component::S_TRANSPORT),

  m_worker(boost::movelib::make_unique<flow::async::Single_thread_task_loop>
             (sync_io_core_moved.get_logger(), sync_io_core_moved.nickname())),

  // Adopt the just-cted, idle sync_io::Native_socket_stream.  It may be in NULL state or PEER state.
  m_sync_io(std::move(sync_io_core_moved))
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;

  // Keep in sync with release()!

  m_worker->start();

  // We're using a boost.asio event loop, so we need to base the async-waited-on handles on our Task_engine.
#ifndef NDEBUG
  bool ok =
#endif
  m_sync_io.replace_event_wait_handles([this]() -> Asio_waitable_native_handle
                                         { return Asio_waitable_native_handle(*(m_worker->task_engine())); });
  assert(ok && "Did you break contract by passing-in a non-fresh sync_io core object to ctor?");

  /* Delegating ctor shall m_sync_io.start_connect_ops() if appropriate (NULL state only) or else
   * deal with m_snd_sync_io_adapter/rcv. */

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Created (NULL state).");
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  // Create core ourselves (NULL state); then delegate to other ctor.
  Impl(sync_io::Native_socket_stream(logger_ptr, nickname_str), nullptr)
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;

  // Keep in sync with release()!

  // Lastly, as we're in NULL state, set up connect-ops state machine.

  /* Hook up the m_sync_io=>*this interaction.  m_sync_io will use these callbacks to ask
   * us to ->async_wait() on `Asio_waitable_native_handle`s for it.
   *
   * The *this=>m_sync_io interaction shall be our APIs like async_connect(),
   * simply invoking the same API in m_sync_io (m_sync_io.async_connect() for that examples). */

#ifndef NDEBUG
  const bool ok =
#endif
  m_sync_io.start_connect_ops([this](Asio_waitable_native_handle* hndl_of_interest,
                                     bool ev_of_interest_snd_else_rcv,
                                     Task_ptr&& on_active_ev_func)
  {
    conn_sync_io_ev_wait(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
  });
  assert(ok && "Bug?  If replace_event_wait_handles() worked, start_*_ops() should too.");
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 Native_handle&& native_peer_socket_moved) :
  // Create core ourselves (in PEER state); then delegate to other ctor.
  Impl(sync_io::Native_socket_stream(logger_ptr, nickname_str, std::move(native_peer_socket_moved)), nullptr)
{
  using flow::util::ostream_op_string;

  // Lastly, as we're in PEER state, set up send-ops and receive-ops state machines.

  const auto log_pfx = ostream_op_string("Socket stream [", *this, ']');
  m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Created (PEER state) directly from pre-opened native handle.");
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(sync_io::Native_socket_stream&& sync_io_core_in_peer_state_moved) :
  // Adopt the PEER-state core given to us by user.
  Impl(std::move(sync_io_core_in_peer_state_moved), nullptr)
{
  using flow::util::ostream_op_string;

  // Keep in sync with release()!

  // Lastly, as we're in PEER state, set up send-ops and receive-ops state machines.

  const auto log_pfx = ostream_op_string("Socket stream [", *this, ']');
  m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: "
                 "Created (PEER state) by adopting fresh sync_io::Native_socket_stream core.");
}

sync_io::Native_socket_stream Native_socket_stream::Impl::release()
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::util::ostream_op_string;

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
  m_worker->start();

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

  // Lastly do what (NULL-state) ctor does regarding starting "new" m_sync_io (which is as-if-default-cted).
#ifndef NDEBUG
  ok =
#endif
  m_sync_io.start_connect_ops([this](Asio_waitable_native_handle* hndl_of_interest,
                                     bool ev_of_interest_snd_else_rcv,
                                     Task_ptr&& on_active_ev_func)
  {
    conn_sync_io_ev_wait(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
  });
  assert(ok && "Bug?  If replace_event_wait_handles() worked, start_*_ops() should too.");

  return core;
} // Native_socket_stream::Impl::release()

Native_socket_stream::Impl::~Impl()
{
  using flow::async::Single_thread_task_loop;
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
   * handlers (namely async_connect(), async_end_sending(), async_receive_*() ones)
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
   * So we do it.  Then, we do the one-off thread thing for what's in our direct purview, which is
   * any outstanding async_connect().  Lastly m_snd/rcv_sync_io_adapter's dtors will do the one-off thread thing
   * for their respective sets of pending completion handlers if any. */

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Continuing shutdown.  Next we will run pending handlers from some "
                "other thread.  In this user thread we will await those handlers' completion and then return.");

  Single_thread_task_loop one_thread(get_logger(), ostream_op_string(nickname(), "-temp_deinit"));
  one_thread.start([&]()
  {
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

    FLOW_LOG_INFO("Socket stream [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending user handlers (feeding operation-aborted).");

    if (!m_conn_on_done_func.empty())
    {
      FLOW_LOG_TRACE("Running pending async-connect completion handler.");
      m_conn_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
      FLOW_LOG_TRACE("User async-connect handler finished.");

      // No receiving or sending calls are allowed until out of CONNECTING state.
      assert(!m_snd_sync_io_adapter);
      assert(!m_rcv_sync_io_adapter);
    }
    // else { When m_*_sync_io_adapter dtors auto-execute, they'll do similar stuff for their handlers if any. }

    FLOW_LOG_INFO("Transient finisher exiting.  (Send-ops and receive-ops similar de-init may follow.)");
  }); // one_thread.start()
  // Here thread exits/joins synchronously.  (But the adapters might run their own similar ones.)
} // Native_socket_stream::Impl::~Impl()

bool Native_socket_stream::Impl::async_connect(const Shared_name& absolute_name,
                                               flow::async::Task_asio_err&& on_done_func)
{
  using flow::util::Lock_guard;
  using flow::util::ostream_op_string;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  Lock_guard<decltype(m_conn_mutex)> lock(m_conn_mutex);

  /* Save it (instead of capturing it in lambda) in case dtor runs before async-connect completes;
   * it would then invoke it with operation-aborted code. */
  m_conn_on_done_func = std::move(on_done_func);

  Error_code sync_err_code;
  const bool ok
    = m_sync_io.async_connect(absolute_name, &sync_err_code,
                              [this, absolute_name](const Error_code& err_code)
  {
    // We are in thread W.  m_conn_mutex is locked.  This'll, inside, post the handler onto thread W.
    conn_on_sync_io_connect(absolute_name, err_code);
  });

  if (!ok)
  {
    // We've even delegated such things as connect-during-connect check and connect-in-PEER-state-already check.
    return false;
  }
  // else

  /* m_sync_io.async_connect() is in progress, formally speaking.  That is, whether or not it has
   * just (synchronously) initiated async-wait via conn_sync_io_ev_wait() -- or alternatively (in reality
   * much more likely as of this writing) connected/failed-to-connect immediately without needing to perform any wait --
   * it would not have invoked conn_on_sync_io_connect() (which we gave it above) synchronously. */

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Async-wait needed before we can complete.  Live to fight another day.
    return true;
  }
  // else

  /* else: Completed synchronously.  Since there can be no async-connecting chains, it's no big deal
   * to just use conn_on_sync_io_connect() again. */

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Sync-IO async-connect completed immediately.  "
                "Posting handler onto async-worker thread.");

  conn_on_sync_io_connect(absolute_name, sync_err_code);

  return true;
  // Lock_guard<decltype(m_conn_mutex)> lock(m_conn_mutex): unlocks here.
} // Native_socket_stream::Impl::async_connect()

void Native_socket_stream::Impl::conn_on_sync_io_connect(const Shared_name& absolute_name, const Error_code& err_code)
{
  using flow::util::ostream_op_string;

  // We are in thread U or W.  m_conn_mutex is locked (as required for m_conn_on_done_func and m_sync_io access).

  assert(err_code != boost::asio::error::operation_aborted);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Earlier async-wait => event active => conn-mutex lock => "
                 "on-active-event-func => sync_io module => here (on-connect-done handler for "
                 "connection attempt to [" << absolute_name << "]).  Or else conn-mutex lock => "
                 "no async-wait needed => here... (ditto).");

  // PEER state!  Yay!  Do the thing PEER-state ctor would have done.
  const auto log_pfx = ostream_op_string("Socket stream [", *this, ']');
  m_snd_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);
  m_rcv_sync_io_adapter.emplace(get_logger(), log_pfx, m_worker.get(), &m_sync_io);

  /* Other than that: Just invoke user completion handler.  post() it onto thread W.  Why post() onto W?
   * - If we are in thread U: It is plainly required, as we promised to invoke completion handlers
   *   from unspecified thread that isn't U.
   * - If we are in thread W: To avoid recursive mayhem if they choose to async_connect() (at least)
   *   inside an on_done_func(), we queue it onto thread W by itself. */
  m_worker->post([this, err_code, on_done_func = std::move(m_conn_on_done_func)]()
  {
    // We are in thread W.  Nothing is locked.
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Invoking on-connect handler.");
    on_done_func(err_code);
    FLOW_LOG_TRACE("Handler completed.");
  });

  m_conn_on_done_func.clear(); // Just in case move() didn't do it.
} // Native_socket_stream::Impl::conn_on_sync_io_connect()

void Native_socket_stream::Impl::conn_sync_io_ev_wait(util::sync_io::Asio_waitable_native_handle* hndl_of_interest,
                                                      bool ev_of_interest_snd_else_rcv,
                                                      util::sync_io::Task_ptr&& on_active_ev_func)
{
  using util::sync_io::Asio_waitable_native_handle;
  using flow::util::Lock_guard;

  // We are in thread U or thread W.  m_conn_mutex is locked.

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Sync-IO connect-ops event-wait request: "
                 "descriptor [" << Native_handle(hndl_of_interest->native_handle()) << "], "
                 "writable-else-readable [" << ev_of_interest_snd_else_rcv << "].");

  // They want this async_wait().  Oblige.
  assert(hndl_of_interest);
  hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv
                                 ? Asio_waitable_native_handle::Base::wait_write
                                 : Asio_waitable_native_handle::Base::wait_read,
                               [this, on_active_ev_func = std::move(on_active_ev_func)]
                                 (const Error_code& err_code)
  {
    // We are in thread W.  Nothing is locked.

    if (err_code == boost::asio::error::operation_aborted)
    {
      return; // Stuff is shutting down.  GTFO.
    }
    // else

    // They want to know about completed async_wait().  Oblige.

    // Protect m_sync_io, m_conn_on_done_func against async_connect().
    Lock_guard<decltype(m_conn_mutex)> lock(m_conn_mutex);

    /* Inform m_sync_io of the event.  This can (indeed will for sure, in our case -- writable socket =>
     * connect completed) synchronously invoke handler we have registered via m_sync_io API (in our case via its
     * async_connect(), and in fact it will call conn_on_sync_io_connect()). */

    (*on_active_ev_func)();
    // (That would have logged sufficiently inside m_sync_io; let's not spam further.)

    // Lock_guard<decltype(m_conn_mutex)> lock(m_conn_mutex): unlocks here.
  }); // hndl_of_interest->async_wait()
} // Native_socket_stream::Impl::conn_sync_io_ev_wait()

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
  /* Note: No m_conn_mutex lock around the PEER-state check for the same reason as in receive_lob_max_size().
   * Ditto all over the place below. */
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
  // See comments regarding potentially locking a mutex here -- in receive_meta_blob_max_size().  Same here.
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
  /* We do not lock any mutex here, even though at least naively locking m_conn_mutex may seem right (as this
   * value changes from 0 to <a constant> when state changed from CONNECTING to PEER).
   * Why not?  Answer: Formally it is because we've declared in transport::Native_socket_stream class doc
   * header-contained thread-safety contract that calling *_max_size() while async_connect() is outstanding
   * (it has been called, but its completion handler has not yet executed) =>
   * undefined behavior.  Hence we formally can assume here that state is NULL or PEER.
   * Per m_sync_io class docs, *_max_size() will return constant positive values in PEER state and 0
   * in NULL state.  So no mutex-lock needed; Q.E.D.
   *
   * Informally the rationale for making that contract restriction is:
   *   - For perf it seems like a weird giveaway to have to lock a thing, just so user can access this
   *     silly constant even while CONNECTING.
   *   - Even if perf were not a concern, it has low utility (wow! yay! I got a 0! How great!).
   *     Meanwhile having to reason about mutexes in this odd no-man's-land of a state (CONNECTING)
   *     is best avoided, all else being equal.
   *
   * In other words: Why bother worrying about it? */
  return m_sync_io.receive_blob_max_size();
}

} // namespace ipc::transport
