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
#pragma once

#include "ipc/transport/detail/transport_fwd.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include <flow/log/log.hpp>
#include <flow/async/single_thread_task_loop.hpp>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Internal-use type that adapts a given PEER-state sync_io::Native_handle_sender or sync_io::Blob_sender *core* into
 * the async-I/O-pattern Native_handle_sender or Blob_sender.  State-mutating logic of the latter is forwarded
 * to a `*this`; while trivial `const` (in PEER state) things like `.send_blob_max_size()` are forwarded directly to the
 * core `sync_io::X`.
 *
 * @see transport::Native_socket_stream::Impl uses this for 99% of its outgoing-direction
 *      (PEER-state by definition) logic.
 * @see transport::Blob_stream_mq_sender_impl uses this for 99% of its logic.
 *
 * @see Async_adapter_receiver for the opposite-direction thing.  E.g., transport::Native_socket_stream::Impl
 *      uses that for 99% of its incoming-direction logic.
 *
 * ### Threads and thread nomenclature; locking ###
 * Thread U, thread W... locking... just see those sections in transport::Native_socket_stream::Impl class doc header.
 * We adopt that nomenclature and logic.  However, as we are concerned with only one direction (op-type),
 * we only deal with code in either thread U or W concerned with that.  The other-direction code -- if applicable
 * (e.g., applicable for `Native_socket_stream` which deals with both over 1 socket connection; N/A
 * for `Blob_stream_mq_*` which uses separate objects entirely) -- simply co-exists in the same thread W and "thread"
 * U.  (If `Native_socket_stream` wanted to, it could even parallelize stuff in thread W by using separate worker
 * threads Ws and Wr.  As of this writing it does not, but it could -- nothing in `*this` would change.)
 *
 * Note, again, that we have our own `m_mutex`.  If there is an opposing-direction counterpart Async_adapter_receiver,
 * then it has its own `m_mutex`; hence things can proceed concurrently.
 *
 * ### Impl design ###
 * This is almost entirely subsumed by our `sync_io` core, Async_adapter_sender::Core, an instance of
 * sync_io::Native_handle_sender or sync_io::Blob_sender.  It has a send op-type (possibly among others), so we invoke
 * its `"sync_io::*_sender:start_send_blob_ops()"` during our initialization.  After that:
 *
 * The main method, send_native_handle() (and its degenerate version send_blob())
 * lacks a completion handler and hence can be forwarded to `Core m_sync_io`... that's it.  auto_ping() -- same deal.
 *
 * async_end_sending() takes a completion handler, however.  Simple enough -- we could just capture
 * this `on_done_func` (from the user arg) in a lambda, then when `m_sync_io` calls our internal handler
 * (synchronously), we `post(on_done_func)` onto thread W, and that's it.  However, that's not sufficient,
 * as if they call our dtor before that can complete, then we are to invoke `on_done_func(E)` where E =
 * operation-aborted (by our contract).  So we save `on_done_func` into `m_end_sending_on_done_func_or_empty`
 * instead of capturing it.
 */
template<typename Core_t>
class Async_adapter_sender :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// The `sync_io::X` type being adapted into async-I/O-pattern `X`.
  using Core = Core_t;

  // Constructors/destructor.

  /**
   * Constructs the adapter around `sync_io::X` object `*sync_io`.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param log_pfx
   *        String that shall precede ~all logged messages (e.g., `lexical_cast<string>(x)`, where `x` is an `X`.)
   * @param worker
   *        The worker thread loop of `X`.  Background work, as needed, will be posted onto this
   *        "thread W."  Note that `X` may (or may not) share this thread with unrelated tasks;
   *        for example `Native_socket_stream` uses it for both a `*this` (outgoing-direction)
   *        and an Async_adapter_receiver (incoming-direction).  `*worker* must already be `->start()`ed.
   * @param sync_io
   *        The core object of `X`.  It should have just (irreversibly) entered state PEER.
   */
  Async_adapter_sender(flow::log::Logger* logger_ptr, util::String_view log_pfx,
                       flow::async::Single_thread_task_loop* worker, Core* sync_io);

  /**
   * To be invoked after `->stop()`ping `*worker` (from ctor), as well as flushing any still-queued
   * tasks in its `Task_engine` (via `.restart()` and `.poll()`), this satisfies the customer adapter
   * dtor's contract which is to invoke any not-yet-fired completion handlers with special
   * operation-aborted error code.  In our case that is either nothing or 1 `async_end_sending()` completion
   * handler.  If applicable the dtor returns once that handler has completed in an unspecified thread
   * that is not the calling thread.
   */
  ~Async_adapter_sender();

  // Methods.

  /**
   * See Native_handle_sender counterpart.  However, this one is `void`, as there is no way `*this` is not in PEER state
   * (by definition).
   *
   * @param hndl_or_null
   *        See Native_handle_sender counterpart.
   * @param meta_blob
   *        See Native_handle_sender counterpart.
   * @param err_code
   *        See Native_handle_sender counterpart.
   */
  void send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob, Error_code* err_code);

  /**
   * See Blob_sender counterpart.  However, this one is `void`, as there is no way `*this` is not in PEER state
   * (by definition).
   *
   * @param blob
   *        See Blob_sender counterpart.
   * @param err_code
   *        See Blob_sender counterpart.
   */
  void send_blob(const util::Blob_const& blob, Error_code* err_code);

  /**
   * See Native_handle_sender counterpart; or leave `on_done_func_or_empty.empty()` for
   * Native_handle_sender::end_sending().
   *
   * @param on_done_func_or_empty
   *        See Native_handle_sender counterpart.  See above.
   * @return See Native_handle_sender counterpart.
   */
  bool async_end_sending(flow::async::Task_asio_err&& on_done_func_or_empty);

  /**
   * See Native_handle_sender counterpart.
   *
   * @param period
   *        See Native_handle_sender counterpart.
   * @return See Native_handle_sender counterpart.
   */
  bool auto_ping(util::Fine_duration period);

private:
  // Methods.

  /**
   * Handles the completion of `m_sync_io.async_end_sending()` operation whether synchronously or asynchronously.
   * Can be invoked from thread U or thread W, and #m_mutex must be locked.  #m_end_sending_on_done_func_or_empty
   * must not be `.empty()`.  Post-condition: it has been made `.empty()`, and a `move()`d version of it has
   * posted onto thread W.
   *
   * @param err_code
   *        Result to pass to user.
   */
  void on_sync_io_end_sending_done(const Error_code& err_code);

  // Data.

  /// See `log_pfx` arg of ctor.
  const std::string m_log_pfx;

  /**
   * The `on_done_func` argument to async_end_sending(), possibly `.empty()` if originally user invoked `end_sending()`,
   * or if neither was used, or if it has fired.  Note that if async_end_sending() does not complete
   * before dtor executes, dtor will invoke the handler (unless `.empty()`) with operation-aborted code.
   *
   * It would not need to be a member -- could just be captured in lambdas while async_end_sending() is outstanding --
   * except for the need to still invoke it with operation-aborted from dtor in the aforementioned case.
   *
   * Protected by #m_mutex.
   */
  flow::async::Task_asio_err m_end_sending_on_done_func_or_empty;

  /**
   * Protects #m_end_sending_on_done_func_or_empty and, more importantly, send-ops data of #m_sync_io.
   * For example `send_*()` engages `Core::send_*()` in thread U which might
   * add messages to its internal pending-out-messages-during-would-block queue; while
   * a util::sync_io::Event_wait_func `on_active_ev_func()` invocation (on writable transport) as requested by
   * #m_sync_io will invoke logic inside the latter, which might pop items from that queue
   * and send them off -- all in thread W.  (This information is provided for context, as formally it's a black
   * box inside `m_sync_io`.)
   */
  mutable flow::util::Mutex_non_recursive m_mutex;

  /// Single-thread worker pool for all internal async work.  Referred to as thread W in comments.
  flow::async::Single_thread_task_loop& m_worker;

  /**
   * The core #Core engine, implementing the `sync_io` pattern (see util::sync_io doc header).
   * See our class doc header for overview of how we use it (the aforementioned `sync_io` doc header talks about
   * the `sync_io` pattern generally).
   *
   * Thus, #m_sync_io is the synchronous engine that we use to perform our work in our asynchronous boost.asio
   * loop running in thread W (#m_worker) while collaborating with user thread(s) a/k/a thread U.
   * (Recall that the user may choose to set up their own event loop/thread(s) --
   * boost.asio-based or otherwise -- and use their own equivalent of an #m_sync_io instead.)
   */
  Core& m_sync_io;
}; // class Async_adapter_sender

// Template implementations.

template<typename Core_t>
Async_adapter_sender<Core_t>::Async_adapter_sender(flow::log::Logger* logger_ptr,
                                                   util::String_view log_pfx,
                                                   flow::async::Single_thread_task_loop* worker,
                                                   Core* sync_io) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_log_pfx(log_pfx),
  m_worker(*worker),
  m_sync_io(*sync_io)
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::util::Lock_guard;

  // We've just entered PEER state, so set up the send-ops.

  /* Hook up the m_sync_io=>*this interaction.  m_sync_io will use these callbacks to ask
   * us to ->async_wait() on `Asio_waitable_native_handle`s for it.
   *
   * The *this=>m_sync_io interaction shall be our APIs, like send_blob(),
   * simply invoking the same API in m_sync_io (m_sync_io.send_blob() for that example). */

  /* (.start_send_native_handler_ops() would do the same thing, if it exists.  If it exists, that's because it has
   * both to satisfy two concepts -- for when the user uses the sync_io::X directly -- but we don't care about that;
   * we know they are the same in this case; so just use the one we know exists for any X.) */
#ifndef NDEBUG
  const bool ok =
#endif
  m_sync_io.start_send_blob_ops([this](Asio_waitable_native_handle* hndl_of_interest,
                                       bool ev_of_interest_snd_else_rcv,
                                       Task_ptr&& on_active_ev_func)
  {
    /* We are in thread U or thread W; m_sync_io.<?>() has called its m_..._ev_wait_func();
     * it has protected access to *hndl_of_interest as needed. */

    FLOW_LOG_TRACE(m_log_pfx << ": Sync-IO send-ops event-wait request: "
                   "descriptor [" << hndl_of_interest->native_handle() << "], "
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

      // Protect m_sync_io and m_* against send-ops (`send_*()`, *end_sending(), ...).
      Lock_guard<decltype(m_mutex)> lock(m_mutex);

      /* Inform m_sync_io of the event.  This can synchronously invoke handler we have registered via m_sync_io
       * API (e.g., `send_*()`, auto_ping()).  In our case -- if indeed it triggers a handler -- it will
       * have to do with async_end_sending() completion. */

      (*on_active_ev_func)();
      // (That would have logged sufficiently inside m_sync_io; let's not spam further.)

      // Lock_guard<decltype(m_mutex)> lock(m_mutex): unlocks here.
    }); // hndl_of_interest->async_wait()
  }); // m_sync_io.start_send_blob_ops()
  assert(ok);
} // Async_adapter_sender::Async_adapter_sender()

template<typename Core_t>
Async_adapter_sender<Core_t>::~Async_adapter_sender()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;

  /* Pre-condition: m_worker is stop()ed, and any pending tasks on it have been executed.
   * Our promised job is to invoke any pending handlers with operation-aborted.
   * The decision to do it from a one-off thread is explained in transport::Native_socket_stream::Impl::~Impl()
   * and used in a few places; so see that.  Let's just do it.
   * @todo It would be cool, I guess, to do it all in one one-off thread instead of potentially starting, like,
   * 2 for some of our customers.  Well, whatever.  At least we can avoid it if we know there are no handlers
   * to invoke; speaking of which there's just the one: */

  if (!m_end_sending_on_done_func_or_empty.empty())
  {
    Single_thread_task_loop one_thread(get_logger(), ostream_op_string(m_log_pfx, "-snd-temp_deinit"));
    one_thread.start([&]()
    {
      FLOW_LOG_INFO(m_log_pfx << ": In transient snd-finisher thread: "
                    "Shall run pending graceful-sends-close completion handler.");
      m_end_sending_on_done_func_or_empty(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
      FLOW_LOG_INFO("Transient finisher exiting.");
    });
  } // if (!m_end_sending_on_done_func_or_empty.empty())
} // Async_adapter_sender::~Async_adapter_sender()

template<typename Core_t>
void Async_adapter_sender<Core_t>::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::Lock_guard;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  /* See comments in send_native_handle(); we are just a somewhat degraded version of that.
   * It's just that send_native_handle() might not even compile, if m_sync_io does not have that method.
   * Logic, as pertains to us, is the same though. */
  Lock_guard<decltype(m_mutex)> lock(m_mutex);
#ifndef NDEBUG
  const bool ok =
#endif
  m_sync_io.send_blob(blob, err_code);
  assert(ok && "We are by definition in PEER state, and ctor starts send-ops; so that should never "
                 "return false.  Bug somewhere?");
}

template<typename Core_t>
void Async_adapter_sender<Core_t>::send_native_handle(Native_handle hndl, const util::Blob_const& meta_blob,
                                                      Error_code* err_code)
{
  using flow::util::Lock_guard;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  /* This one is particularly simple, as there is no async completion handler to invoke.  So we essentially just
   * forward it to m_sync_io.send_native_handler() with identical signature.  The only subtlety -- and it is
   * quite important -- is that we must lock m_mutex.  That might seem odd if one does not remember that
   * m_sync_io send-ops explicit API (.send_native_handler() in this case) must never be invoked concurrently
   * with the start_send_*_ops()-passed Event_wait_func's `on_active_ev_func()` -- which might call
   * on_sync_io_end_sending_done().  So we lock it, and elsewhere we lock it when calling on_active_ev_func().
   *
   * Specifically (though formally speaking we are just following the sync_io pattern, and technically the following
   * info is about inside the black box that is m_sync_io), m_sync_io at least stores an out-queue of messages
   * to send, non-empty in would-block conditions; send_native_handle() pushes items onto that queue, while
   * on_active_ev_func() (on writable socket) pops items off it upon successfully sending them off over socket. */

  Lock_guard<decltype(m_mutex)> lock(m_mutex);
#ifndef NDEBUG
  const bool ok =
#endif
  m_sync_io.send_native_handle(hndl, meta_blob, err_code);
  assert(ok && "We are by definition in PEER state, and ctor starts send-ops; so that should never "
                 "return false.  Bug somewhere?");
} // Async_adapter_sender::send_native_handle()

template<typename Core_t>
bool Async_adapter_sender<Core_t>::async_end_sending(flow::async::Task_asio_err&& on_done_func_or_empty)
{
  using flow::util::Lock_guard;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  /* While the same comments about locking m_mutex apply, in addition to that:
   * This is somewhat more complex than the other send-ops, as there is a completion handler.
   * (However the pattern is quite similar to transport::Native_socket_stream::Impl::async_connect() which also needs
   * to save into a m_*_on_done_func for the same reason(s).) XXX */

  Lock_guard<decltype(m_mutex)> lock(m_mutex);

  if (on_done_func_or_empty.empty())
  {
    return m_sync_io.end_sending();
  }
  // else

  if (!m_end_sending_on_done_func_or_empty.empty())
  {
    FLOW_LOG_WARNING(m_log_pfx << ": async_end_sending(F) invoked; but we have a saved "
                     "completion handler -- which has not yet fired -- for it already, so it must be a dupe-call.  "
                     "Ignoring.");
    return false;
  }
  /* else: It might also be .empty() and still be a dupe call (if it has fired already -- then they called us again).
   *       m_sync_io.async_end_sending() will catch that fine.  For now though: */

  /* Save it (instead of capturing it in lambda) in case dtor runs before async-end-sending completes;
   * it would then invoke it with operation-aborted code. */
  m_end_sending_on_done_func_or_empty = std::move(on_done_func_or_empty);

  Error_code sync_err_code;
  const bool ok = m_sync_io.async_end_sending(&sync_err_code, [this](const Error_code& err_code)
  {
    /* We are in thread W.  m_mutex is locked (by the handler inside the function inside ctor).
     * This'll, inside, post the handler onto thread W. */
    on_sync_io_end_sending_done(err_code);
  });

  if (!ok)
  {
    // False start (dupe call).  It logged enough.
    m_end_sending_on_done_func_or_empty.clear();
    return false;
  }
  // else: It either finished synchronously; or it will finish asynchronously.

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Async-wait needed before we can complete.  Live to fight another day.
    return true;
  }
  /* else: Completed synchronously.  Since there can be no async-end-sending chains, it's no big deal
   * to just use on_sync_io_end_sending_done() again. */

  FLOW_LOG_INFO(m_log_pfx << ": Sync-IO async-end-sending completed immediately.  "
                "Posting handler onto async-worker thread.");

  on_sync_io_end_sending_done(sync_err_code);

  return true;
  // Lock_guard<decltype(m_mutex)> lock(m_mutex): unlocks here.
} // Async_adapter_sender::async_end_sending()

template<typename Core_t>
void Async_adapter_sender<Core_t>::on_sync_io_end_sending_done(const Error_code& err_code)
{
  using flow::util::Lock_guard;

  /* We are in thread U or W.  m_mutex is locked (as required for m_end_sending_on_done_func_or_empty
   * and m_sync_io access). */

  assert(err_code != boost::asio::error::operation_aborted);

  FLOW_LOG_TRACE(m_log_pfx << ":: Earlier async-wait => event active => snd-mutex lock => "
                 "on-active-event-func => sync_io module => here (on-end-sending-done handler).  Or else "
                 "snd-mutex lock => no async-wait needed => here... (ditto).");

  /* Just invoke user completion handler.  post() it onto thread W.  Why post() onto W?
   * - If we are in thread U: It is plainly required, as we promised to invoke completion handlers
   *   from unspecified thread that isn't U.
   * - If we are in thread W: To avoid recursive mayhem if they choose to call some other API
   *   inside on_done_func(), we queue it onto thread W by itself. */
  m_worker.post([this, err_code, on_done_func = std::move(m_end_sending_on_done_func_or_empty)]()
  {
    // We are in thread W.  Nothing is locked.
    FLOW_LOG_TRACE(m_log_pfx << ": Invoking on-end-sending-done handler.");
    on_done_func(err_code);
    FLOW_LOG_TRACE("Handler completed.");
  });

  m_end_sending_on_done_func_or_empty.clear(); // Just in case move() didn't do it.
} // Async_adapter_sender::on_sync_io_end_sending_done()

template<typename Core_t>
bool Async_adapter_sender<Core_t>::auto_ping(util::Fine_duration period)
{
  using flow::util::Lock_guard;

  // Like send_native_handle() and others (keeping comments light).

  Lock_guard<decltype(m_mutex)> lock(m_mutex);
  return m_sync_io.auto_ping(period);
}

} // namespace ipc::transport::sync_io
