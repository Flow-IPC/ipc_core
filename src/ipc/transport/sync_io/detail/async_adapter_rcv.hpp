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
#include <flow/log/log.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include <queue>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Internal-use type that adapts a given PEER-state sync_io::Native_handle_receiver or sync_io::Blob_receiver *core*
 * into the async-I/O-pattern Native_handle_receiver or Blob_receiver.  State-mutating logic of the latter is forwarded
 * to a `*this`; while trivial `const` (in PEER state) things like `.receive_blob_max_size()` are forwarded directly to
 * the core `sync_io::X`.
 *
 * @see transport::Native_socket_stream::Impl uses this for 99% of its incoming-direction
 *      (PEER-state by definition) logic.
 * @see transport::Blob_stream_mq_receiver_impl uses this for 99% of its logic.
 *
 * @see Async_adapter_sender for the opposite-direction thing.  E.g., transport::Native_socket_stream::Impl
 *      uses that for 99% of its outgoing-direction logic.
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
 * Note, again, that we have our own `m_mutex`.  If there is an opposing-direction counterpart Async_adapter_sender,
 * then it has its own `m_mutex`; hence things can proceed concurrently.
 *
 * ### Impl design ###
 * This is almost entirely subsumed by our `sync_io` core, Async_adapter_receiver::Core, an instance of
 * sync_io::Native_handle_receiver or sync_io::Blob_receiver.  It has a receive op-type (possibly among others), so we
 * invoke its `"sync_io::*_sender:start_receive_blob_ops()"` during our initialization.  After that:
 *
 * For idle_timer_run(), we can again just forward it to `m_sync_io`.  There's no completion
 * handler either, unlike with `.async_end_sending()`, so it's even simpler -- just straight forwarding to
 * the `sync_io` core.
 *
 * For async_receive_native_handle() (and its degenerate version async_receive_blob()) the `*_receiver` concept
 * we implement has an extra feature on top of sync_io::Native_handle_receiver (and degenerate version,
 * sync_io::Blob_receiver): when 1 async_receive_native_handle() is in progress asynchronously, it is allowed
 * to invoke it again an arbitrary number of times, and they are to be served in the order they were called.
 * Therefore:
 *   - We maintain a "deficit" queue of these requests.  The currently-served request is stored in
 *     `User_request m_user_request`.  The queued-up subsequent ones are stored in queue with that
 *     same element type, `m_pending_user_requests_q`.
 *   - When the ongoing (single) `m_sync_io.async_receive_*()` does complete -- which occurs in thread W --
 *     we emit the result (`Error_code`, `sz`) to the `"User_request::m_on_done_func"` (the completion
 *     handler from the user).  Then we pop `m_pending_user_requests_q` (unless empty -- no further "deficit")
 *     into `m_user_request` and service *that* one via `m_sync_io.async_receive_*()`.  Rinse/repeat.
 *
 * If the destructor is invoked before `m_user_request` can get serviced, then in the dtor
 * we execute `on_done_func(E)`, where E is operation-aborted.  Once that's done the dtor can finish.
 */
template<typename Core_t>
class Async_adapter_receiver :
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
  Async_adapter_receiver(flow::log::Logger* logger_ptr, util::String_view log_pfx,
                         flow::async::Single_thread_task_loop* worker, Core* sync_io);

  /**
   * To be invoked after `->stop()`ping `*worker` (from ctor), as well as flushing any still-queued
   * tasks in its `Task_engine` (via `.restart()` and `.poll()`), this satisfies the customer adapter
   * dtor's contract which is to invoke any not-yet-fired completion handlers with special
   * operation-aborted error code.  In our case that is either nothing or 1 `async_end_sending()` completion
   * handler.  If applicable the dtor returns once that handler has completed in an unspecified thread
   * that is not the calling thread.
   */
  ~Async_adapter_receiver();

  // Methods.

  /**
   * See Native_handle_sender counterpart.  However, this one is `void`, as there is no way `*this` is not in PEER state
   * (by definition).
   *
   * @param target_hndl
   *        See Native_handle_sender counterpart.
   * @param target_meta_blob
   *        See Native_handle_sender counterpart.
   * @param on_done_func
   *        See Native_handle_sender counterpart.
   */
  void async_receive_native_handle(Native_handle* target_hndl,
                                   const util::Blob_mutable& target_meta_blob,
                                   flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See Blob_receiver counterpart.  However, this one is `void`, as there is no way `*this` is not in PEER state
   * (by definition).
   *
   * @param target_blob
   *        See Blob_receiver counterpart.
   * @param on_done_func
   *        See Blob_receiver counterpart.
   */
  void async_receive_blob(const util::Blob_mutable& target_blob,
                          flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See Native_handle_receiver counterpart.
   *
   * @param timeout
   *        See Native_handle_receiver counterpart.
   * @return See Native_handle_receiver counterpart.
   */
  bool idle_timer_run(util::Fine_duration timeout);

private:
  // Types.

  /**
   * Data store representing a deficit user async-receive request: either one being currently handled
   * by `m_sync_io` -- which can handle one `m_sync_io.async_receive_*()` at a time, no more -- or
   * one queued up behind it, if `async_receive_*()` was called before the current one could complete.
   *
   * Essentially this stores args to async_receive_native_handle() (or degenerate version, async_receive_blob())
   * which is an async-operating method.  We store them in queue via #Ptr.
   */
  struct User_request
  {
    // Types.

    /// Short-hand for `unique_ptr` to this.
    using Ptr = boost::movelib::unique_ptr<User_request>;

    // Data.

    /// See async_receive_native_handle() `target_hndl`.  Null for `async_receive_blob()`.
    Native_handle* m_target_hndl_ptr;

    /// See async_receive_native_handle() `target_meta_blob`.  Or see async_receive_blob() `target_blob`.
    util::Blob_mutable m_target_meta_blob;

    /// See async_receive_native_handle() or async_receive_blob() `on_done_func`.
    flow::async::Task_asio_err_sz m_on_done_func;
  }; // struct User_request

  // Methods.

  /**
   * Body of async_receive_native_handle() and async_receive_blob(); with `target_hndl` null if and only if
   * it's the latter as opposed to the former.
   *
   * @param target_hndl_or_null
   *        See async_receive_native_handle(); or null if it's the other API.
   *        If `!(Core::S_TRANSMIT_NATIVE_HANDLES)` this must be null.
   * @param target_meta_blob
   *        See async_receive_native_handle().
   * @param on_done_func
   *        See async_receive_native_handle().
   */
  void async_receive_native_handle_impl(Native_handle* target_hndl_or_null,
                                        const util::Blob_mutable& target_meta_blob,
                                        flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * Invoked via active-event API, handles the async completion
   * of `m_sync_io.async_receive_*()` operation.  Can be invoked from thread W only, and #m_mutex must be
   * locked.  #m_user_request must not be null.
   *
   * This method iteratively, synchronously leverages #m_sync_io to read as many in-messages available
   * in the transport as possible, until: the request deficit is met (either by reading enough messages
   * to satisfy #m_user_request and #m_pending_user_requests_q; or by encountering pipe-hosing error)
   * or would-block.  In the latter case another async-wait is initiated by this method synchronously.
   *
   * The first action, before those potential further reads, is to process_msg_or_error() the just-received
   * (or pipe-hosing would-be) message.  Then for each further in-message process_msg_or_error() is again
   * invoked.
   *
   * For each request satisfied, a separate user handler is posted onto thread W to execute in order.
   *
   * @param err_code
   *        Result to pass to user (if truthy, all pending requests; else to #m_user_request only).
   * @param sz
   *        Result to pass to user (ditto).
   */
  void on_sync_io_rcv_done(const Error_code& err_code, size_t sz);

  /**
   * Invoked from thread U/W (async_receive_native_handle_impl()) or thread W (active-event API), handles
   * a completed `m_sync_io.async_receive_*()` -- whose results are to be given as args -- by (1) updating
   * #m_user_request and #m_pending_user_requests_q and (2) posting any appropriate handlers onto thread W.
   *
   * See notes for on_sync_io_rcv_done().
   *
   * @param err_code
   *        See on_sync_io_rcv_done().
   * @param sz
   *        See on_sync_io_rcv_done().
   */
  void process_msg_or_error(const Error_code& err_code, size_t sz);

  // Data.

  /// See `log_pfx` arg of ctor.
  const std::string m_log_pfx;

  /**
   * The *head slot* containing the currently-being-serviced "deficit" async-receive request, with a meta-blob
   * *potentially* being async-written to; null if there is no pending async_receive_native_handle().
   * It is the "fulcrum" of the consumer-producer state machine described in doc header impl section's design
   * discussion: If null there is no deficit; if not null there is an *overall deficit*.  In the former case,
   * at steady state, `m_pending_user_requests_q.empty() == true`.
   *
   * Protected by #m_mutex.
   *
   * @see #m_pending_user_requests_q
   */
  typename User_request::Ptr m_user_request;

  /**
   * Queue storing deficit async-receive requests queued up due to #m_user_request being not null while
   * more `async_receive_*()` invocations being made by user.  One can think of the "overall" queue as being
   * #m_user_request followed by the elements in this #m_pending_user_requests_q.
   * See class doc header for design discussion.
   *
   * Protected by #m_mutex.
   *
   * ### Rationale for not subsuming #m_user_request directly into this queue ###
   * It's the same amount of stuff; so the reason is stylistic in a subjective way.  Basically a low-level
   * async read-op will target the meta-blob *directly* inside the head User_request in the "overall" queue but
   * never any of the subsequently queued requests; in my (ygoldfel) view it is clearer to express it as always
   * targeting #m_user_request rather than `*(m_pending_user_requests_q.front())`.
   */
  std::queue<typename User_request::Ptr> m_pending_user_requests_q;

  /// Protects #m_user_request, #m_pending_user_requests_q, and receive-ops data of #m_sync_io.
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
}; // class Async_adapter_receiver

// Template implementations.

template<typename Core_t>
Async_adapter_receiver<Core_t>::Async_adapter_receiver(flow::log::Logger* logger_ptr,
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

  // We've just entered PEER state, so set up the receive-ops.

  /* Hook up the m_sync_io=>*this interaction.  m_sync_io will use these callbacks to ask
   * us to ->async_wait() on `Asio_waitable_native_handle`s for it.
   *
   * The *this=>m_sync_io interaction shall be our APIs, like async_receive_native_handle(),
   * simply invoking the same API in m_sync_io (m_sync_io.async_receive_native_handle() for that example). */

  /* (.start_receive_native_handler_ops() would do the same thing, if it exists.  If it exists, that's because it has
   * both to satisfy two concepts -- for when the user uses the sync_io::X directly -- but we don't care about that;
   * we know they are the same in this case; so just use the one we know exists for any X.) */
#ifndef NDEBUG
  const bool ok =
#endif
  m_sync_io.start_receive_blob_ops([this](Asio_waitable_native_handle* hndl_of_interest,
                                          bool ev_of_interest_snd_else_rcv,
                                          Task_ptr&& on_active_ev_func)
  {
    /* We are in thread U or thread W; m_sync_io.<?>() has called its m_..._ev_wait_func();
     * it has protected access to *hndl_of_interest as needed. */

    FLOW_LOG_TRACE(m_log_pfx << ": Sync-IO receive-ops event-wait request: "
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

      // Protect m_sync_io and non-const non-ref m_* against receive-ops (async_receive_*(), ...).
      Lock_guard<decltype(m_mutex)> lock(m_mutex);

      /* Inform m_sync_io of the event.  This can synchronously invoke handler we have registered via m_sync_io
       * API (e.g., `send_*()`, auto_ping()).  In our case -- if indeed it triggers a handler -- it will
       * have to do with async_end_sending() completion. */

      (*on_active_ev_func)();
      // (That would have logged sufficiently inside m_sync_io; let's not spam further.)

      // Lock_guard<decltype(m_mutex)> lock(m_mutex): unlocks here.
    }); // hndl_of_interest->async_wait()
  }); // m_sync_io.start_receive_blob_ops()
  assert(ok);
} // Async_adapter_receiver::Async_adapter_receiver()

template<typename Core_t>
Async_adapter_receiver<Core_t>::~Async_adapter_receiver()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;

  /* Pre-condition: m_worker is stop()ed, and any pending tasks on it have been executed.
   * Our promised job is to invoke any pending handlers with operation-aborted.
   * The decision to do it from a one-off thread is explained in transport::Native_socket_stream::Impl::~Impl()
   * and used in a few places; so see that.  Let's just do it.
   * @todo It would be cool, I guess, to do it all in one one-off thread instead of potentially starting, like,
   * 3 for some of our customers.  Well, whatever. */

  if (m_user_request)
  {
    Single_thread_task_loop one_thread(get_logger(), ostream_op_string(m_log_pfx, "-rcv-temp_deinit"));
    one_thread.start([&]()
    {
      FLOW_LOG_TRACE("Running head slot async-receive completion handler.");
      m_user_request->m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, 0);
      FLOW_LOG_TRACE("User receive handler finished.");

      while (!m_pending_user_requests_q.empty())
      {
        FLOW_LOG_TRACE("Running a queued async-receive completion handler.");
        m_pending_user_requests_q.front()
          ->m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, 0);
        m_pending_user_requests_q.pop();
        FLOW_LOG_TRACE("User receive handler finished.  Popped from user request deficit queue.");
      } // while (!m_pending_user_requests_q.empty())
    });
  } // if (m_user_request)
  else
  {
    assert(m_pending_user_requests_q.empty()); // Sanity check.
  }
} // Async_adapter_receiver::~Async_adapter_receiver()

template<typename Core_t>
void Async_adapter_receiver<Core_t>::async_receive_native_handle(Native_handle* target_hndl,
                                                                 const util::Blob_mutable& target_meta_blob,
                                                                 flow::async::Task_asio_err_sz&& on_done_func)
{
  assert(target_hndl && "Native_socket_stream::async_receive_native_handle() must take non-null Native_handle ptr.");
  async_receive_native_handle_impl(target_hndl, target_meta_blob, std::move(on_done_func));

  /* Note, if our customer lacks async_receive_native_handle(), then they'll forward to (call) this method;
   * therefore it (*this being a template instance) will never be compiled; therefore target_hndl_or_null will never
   * be non-null; which is why that assert(false) inside `if constexpr(!S_TRANSMIT_NATIVE_HANDLES)` compile-time clause
   * shall never be reached. */
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::async_receive_blob(const util::Blob_mutable& target_blob,
                                                        flow::async::Task_asio_err_sz&& on_done_func)
{
  async_receive_native_handle_impl(nullptr, target_blob, std::move(on_done_func));
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::async_receive_native_handle_impl(Native_handle* target_hndl_or_null,
                                                                      const util::Blob_mutable& target_meta_blob,
                                                                      flow::async::Task_asio_err_sz&& on_done_func)
{
  using flow::util::Lock_guard;
  using boost::movelib::make_unique;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  /* We will be accessing m_user_request, m_pending_user_requests_q, and possibly m_sync_io receive-ops
   * sub-API -- while ctor's ev-wait function's async_wait() handler will be accessing them too from thread W -- so: */

  Lock_guard<decltype(m_mutex)> lock(m_mutex);

  /* This is essentially the only place in *this where we're more than a mere forwarder of the core m_sync_io
   * sync_io::*_receiver (which is synchronous) into an async *_receiver.  I.e., in this
   * case we add a feature on top; namely more than 1 async_receive_*() can be pending for us, whereas only
   * 1 async_receive_*() is allowed in the sync_io one.  We call this is a *deficit*, meaning there are more
   * user requests than available messages (there's only ever 1 message "available" -- once the
   * m_sync_io.async_receive_*() succeeds -- and only momentarily, as it's immediately fed to the
   * completion handler of our async_receive_*() that precipitated it.
   *
   * Thus, if there is no deficit, and async_receive_*() comes in, we trigger m_sync_io.async_receive_*()
   * and save the associated request info (basically our 3 args above) into m_user_request.
   * Once it succeeds we feed the result to the completion handler saved among those 3 args and nullify
   * m_user_request.  Meanwhile, if another one comes in while there's already a deficit (m_user_request
   * is not null), we queue it in m_pending_user_requests_q.  Lastly, having nullified
   * m_user_request as noted a couple sentences ago, we pop the _q (if not empty) into m_user_request
   * and trigger another m_sync_io.async_receive_*(), continuing the chain this way until _q is empty
   * and m_user_request is null (at which point there's no deficit and not reason to
   * m_sync_io.async_receive_*()).
   *
   * Oh, also, if an m_sync_io.async_receive_*() yields a socket-hosing error, then it is fed to the entire
   * deficit queue's handlers; as in any case any subsequent attempted m_sync_io.async_receive_*() would fail.
   *
   * Note that there can be a deficit (pending user requests) but no surplus (pending messages): we don't
   * make unnecessary m_sync_io.async_receive_*() calls and thus don't need to save surplus messages.
   * If we did, we'd need to copy any such surplus message (into user buffer) once this->async_receive_*() does
   * come in.  This is discussed and rationalized elsewhere, but for convenience, recap: The reason is, basically,
   * the send-side concept is obligated to internally copy-and-queue messages on encountering would-block.
   * Since the system will thus not lose messages (if the receiver side is being slow in popping them from the pipe),
   * the complexity of queuing stuff -- and the perf loss due to copying -- is kept to one side. */

  FLOW_LOG_TRACE(m_log_pfx << ": Incoming user async-receive request for "
                 "possible native handle and meta-blob (located @ [" << target_meta_blob.data() << "] of "
                 "max size [" << target_meta_blob.size() << "]).  In worker now? = [" << m_worker.in_thread() << "].");

  auto new_user_request = make_unique<User_request>();
  new_user_request->m_target_hndl_ptr = target_hndl_or_null;
  new_user_request->m_target_meta_blob = target_meta_blob;
  new_user_request->m_on_done_func = std::move(on_done_func);

  if (m_user_request)
  {
    m_pending_user_requests_q.emplace(std::move(new_user_request));
    FLOW_LOG_TRACE("At least 1 async-receive request is already in progress.  "
                   "After registering the new async-receive request: Head slot is non-empty; and "
                   "subsequently-pending deficit queue has size [" << m_pending_user_requests_q.size() << "].  "
                   "Will sync-IO-receive to handle this request once it reaches the front of that queue.");
    return;
  }
  // else if (!m_user_request):

  FLOW_LOG_TRACE("No async-receive request is currently in progress.  Starting sync-IO-receive chain to service the "
                 "new request and any further-queued requests that might appear in the meantime.");
  m_user_request = std::move(new_user_request);
  // new_user_request is now hosed.

  /* If receive completes synchronously (there are data pending on the "wire"), these will reflect that.
   * If not then sync_err_code will indicate would-block. */
  Error_code sync_err_code;
  size_t sync_sz;

#ifndef NDEBUG
  bool ok;
#endif
  if (m_user_request->m_target_hndl_ptr)
  {
    if constexpr(Core::S_TRANSMIT_NATIVE_HANDLES) // Prevent compile error from code that could never be reached.
    {
#ifndef NDEBUG
      ok =
#endif
      m_sync_io.async_receive_native_handle(m_user_request->m_target_hndl_ptr, m_user_request->m_target_meta_blob,
                                            &sync_err_code, &sync_sz,
                                            [this](const Error_code& err_code, size_t sz)
      {
        // We are in thread W.  m_mutex is locked.
        on_sync_io_rcv_done(err_code, sz); // Emit to handler; possibly pop queue and continue chain.
      });
    }
    else // if constexpr(!S_TRANSMIT_NATIVE_HANDLES)
    {
      assert(false && "This code should never be reached.");
    }
  } // if (m_user_request->m_target_hndl_ptr)
  else // Same deal/keeping comments light.
  {
#ifndef NDEBUG
    ok =
#endif
    m_sync_io.async_receive_blob(m_user_request->m_target_meta_blob, &sync_err_code, &sync_sz,
                                 [this](const Error_code& err_code, size_t sz)
      { on_sync_io_rcv_done(err_code, sz); });
  } // else if (!m_user_request->m_target_hndl_ptr)

  assert(ok && "We are by definition in PEER state, and ctor starts receive-ops, and we never start a "
               "sync_io async-receive before ensuring previous one has executed; so that should never "
               "return false.  Bug somewhere?");

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Async-wait started by m_sync_io.  It logged plenty.  We live to fight another day.
    return;
  }
  // else:

  /* Process the message and nullify m_user_request.  (It would also promote any m_pending_user_requests_q head
   * to m_user_request; but in our case that is not possible.  We are still in the user async-receive API!) */
  process_msg_or_error(sync_err_code, sync_sz);

  FLOW_LOG_TRACE("Message was immediately available; synchronously returned to user; handler posted onto "
                 "async worker thread.  Done until next request.");
} // Async_adapter_receiver::async_receive_native_handle_impl()

template<typename Core_t>
void Async_adapter_receiver<Core_t>::process_msg_or_error(const Error_code& err_code, size_t sz)
{
  using flow::util::Lock_guard;
  using std::queue;

  // We are in thread U or W.  m_mutex is locked.

  assert(err_code != boost::asio::error::operation_aborted);
  assert((err_code != error::Code::S_SYNC_IO_WOULD_BLOCK) && "By our contract.");

  FLOW_LOG_TRACE(m_log_pfx << ": Earlier async-wait => event active => rcv-mutex lock => "
                 "on-active-event-func => sync_io module => on-rcv-done handler => here.  "
                 "Or else: rcv-mutex lock => sync_io module => no async-wait needed => here.");

  /* As noted in our doc header, we have roughly two items on the agenda.
   *
   * 1, we need to invoke m_user_request->m_on_done_func, passing it the results (which are our args).
   *
   * 2, we need to update our m_* structures, such as popping stuff off m_pending_user_requests_q
   * and starting the next m_sync_io.async_receive_*() if any -- and so on.
   *
   * Oh and 2b: if err_code is truthy, we need to invoke all the pending handlers (if any) and nullify/empty
   * all of that.
   *
   * The question is what thread to do that stuff in.  There are a couple of approaches, at least, but the following
   * seems the least latency-ridden:
   *   - For 1 and 2b, we must post() the handler(s) onto thread W.
   *     - If we are in thread U: It is plainly required, as we promised to invoke completion handlers
   *       from unspecified thread that isn't U.
   *     - If we are in thread W: To avoid recursive mayhem if they choose to call some other API
   *       inside an on_done_func(), we queue it onto thread W by itself.  (But if it's 2b in addition to 1,
   *       then we can bundle them all together into one thread-W task; no need for the churn of post()ing
   *       each one individually.)
   *   - However for 2, there's no need to post() anything.  The mutex is already locked, so even if we're in thread
   *     U (m_sync_io.async_receive_*() succeeded synchronously, and this->async_receive_native_handle_impl()
   *     therefore invoked us synchronously, from thread U) we are within
   *     our rights to just finish the job here synchronously as well.  In fact, that's great!  It means
   *     data were waiting to be read right away, and we did everything we could synchronously in the
   *     original async_receive_*() call, post()ing onto thread W only 1 above -- the completion handler invoking (which
   *     we must do by contract).  Yay!  By the way, if that's the case (we are in thread U), then 2b doesn't apply;
   *     only 1 does.  (The situation where there are items in m_pending_user_requests_q means we wouldn't have
   *     started an actual m_sync_io.async_receive_*() in the first place, as one was already in progress.  So
   *     it couldn't have magically completed synchronously -- not having begun and all.) */

  // Okay, so first deal with m_*, here and now as discussed.  In so doing prepare the stuff to call in thread W.

  assert(m_user_request);
  typename User_request::Ptr ex_user_request(std::move(m_user_request));
  assert(!m_user_request);

  queue<typename User_request::Ptr> ex_pending_user_requests_q_or_none;
  if (err_code)
  {
    FLOW_LOG_TRACE("Error emitted by sync-IO => all [" << m_pending_user_requests_q.size() << "] pending "
                   "handlers (may well be none) will be invoked with error in addition to the head handler.");

    ex_pending_user_requests_q_or_none = std::move(m_pending_user_requests_q);
    assert(m_pending_user_requests_q.empty());
  }
  else if (!m_pending_user_requests_q.empty()) // && (!err_code)
  {
    FLOW_LOG_TRACE("Success emitted by sync-IO => lead handler will be invoked; next pending request will "
                   "be serviced by sync-IO; pending request count has become (after popping from queue into lead "
                   "slot): [" << (m_pending_user_requests_q.size() - 1) << "].\n");

    m_user_request = std::move(m_pending_user_requests_q.front());
    // m_pending_user_requests_q.front() is now null; we're gonna pop that null ptr presently.
    m_pending_user_requests_q.pop();
  }
  else // if (_q.empty() && (!err_code))
  {
    FLOW_LOG_TRACE("Success emitted by sync-IO => lead handler will be invoked; no pending requests queued up.\n");
  }

  // Second: Post the completion handlers as discussed.
  m_worker.post([this, err_code, sz,
                 /* Have to upgrade to shared_ptr<>s due to capturing requiring copyability (even though copying is not
                  * actually invoked by us).  unique_ptr and queue<unique_ptr> = not copyable. */
                 ex_user_request = boost::shared_ptr<User_request>(std::move(ex_user_request)),
                 ex_pending_user_requests_q_or_none
                   = boost::make_shared<decltype(ex_pending_user_requests_q_or_none)>
                       (std::move(ex_pending_user_requests_q_or_none))]()
  {
    // We are in thread W.  Nothing is locked.

    assert(ex_user_request);
    FLOW_LOG_TRACE(m_log_pfx << ": Invoking head handler.");
    (ex_user_request->m_on_done_func)(err_code, sz);
    FLOW_LOG_TRACE("Handler completed.");
    if (!ex_pending_user_requests_q_or_none->empty())
    {
      assert(err_code);
      assert(sz == 0);

      FLOW_LOG_TRACE(m_log_pfx << ": Invoking [" << ex_pending_user_requests_q_or_none->size() << "] "
                     "pending handlers in one shot (due to error).");
      while (!ex_pending_user_requests_q_or_none->empty())
      {
        ex_pending_user_requests_q_or_none->front()->m_on_done_func(err_code, 0);
        ex_pending_user_requests_q_or_none->pop();
        FLOW_LOG_TRACE("In-queue handler finished.");
      }
      // @todo For modest perf, iterate through it; then .clear().  Maybe use an underlying deque<> or list<> directly.
    } // if (!ex_pending_user_requests_q_or_none->empty())
  }); // m_worker.post()

  assert(!ex_user_request); // Hosed by move().
  assert(ex_pending_user_requests_q_or_none.empty());
} // Async_adapter_receiver::process_msg_or_error()

template<typename Core_t>
void Async_adapter_receiver<Core_t>::on_sync_io_rcv_done(const Error_code& err_code, size_t sz)
{
  using flow::util::Lock_guard;
  using std::queue;

  // We are in thread W.  m_mutex is locked.

  assert(err_code != boost::asio::error::operation_aborted);
  assert(err_code != error::Code::S_SYNC_IO_WOULD_BLOCK);

  FLOW_LOG_TRACE(m_log_pfx << ": Earlier async-wait => event active => rcv-mutex lock => "
                 "on-active-event-func => sync_io module => here (on-rcv-done handler).");

  /* This is not *too* different from thread-U (or thread-W if invoked from our own handler)
   * async_receive_native_handle_impl()... except that in our case more requests may have been queued (as summarized
   * in top comment in that method) during our async-wait that just finished.  And, of course, we need
   * to process the ready message first-thing.  But let's say that's taken care of.  After that: we can't just
   * stop; there may be queued request.  We shall process them as synchronously as possible in a do-while()
   * loop.  That's the executive summary. */

  /* So handle the message or error -- W-post any relevant handlers; update m_user_request and
   * m_pending_user_requests_q. */
  process_msg_or_error(err_code, sz);

  Error_code sync_err_code;
  auto& sync_sz = sz; // (Might as well reuse the arg.)

  /* First iteration: sync_err_code is definitely not would-block; m_user_request may be null.
   * Subsequent iterations: sync_err_code may be would-block. */
  while (m_user_request && (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK))
  {
    // @todo Code reuse with async_receive_native_handle_impl()?  For now keeping comments light.
#ifndef NDEBUG
    bool ok;
#endif
    if (m_user_request->m_target_hndl_ptr)
    {
      if constexpr(Core::S_TRANSMIT_NATIVE_HANDLES)
      {
#ifndef NDEBUG
        ok =
#endif
        m_sync_io.async_receive_native_handle(m_user_request->m_target_hndl_ptr, m_user_request->m_target_meta_blob,
                                              &sync_err_code, &sync_sz,
                                              [this](const Error_code& err_code, size_t sz)
        {
          on_sync_io_rcv_done(err_code, sz); // Back to us (async path).
        });
      }
      else // if constexpr(!S_TRANSMIT_NATIVE_HANDLES)
      {
        assert(false && "This code should never be reached.");
      }
    } // if (m_user_request->m_target_hndl_ptr)
    else
    {
#ifndef NDEBUG
      ok =
#endif
      m_sync_io.async_receive_blob(m_user_request->m_target_meta_blob, &sync_err_code, &sync_sz,
                                   [this](const Error_code& err_code, size_t sz)
        { on_sync_io_rcv_done(err_code, sz); });
    } // else if (!m_user_request->m_target_hndl_ptr)
    assert(ok && "We are by definition in PEER state, and ctor starts receive-ops, and we never start a "
                 "sync_io async-receive before ensuring previous one has executed; so that should never "
                 "return false.  Bug somewhere?");

    if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      continue; // Loop will end.
    }
    // else

    /* Remaining outcomes: sync_err_code truthy => process_msg_or_error() will do the right thing.
     *                     sync_err_code is falsy => ditto. */

    process_msg_or_error(sync_err_code, sync_sz);

    /* Outcomes: Error => m_user_request is null, m_pending_user_requests_q is null.  No req to service.  Loop end.
     *           Success => m_user_request is null, m_pending_user_requests_q is null.  No req to service.  Loop end.
     *           Success => m_user_request is NOT null, m_pending_user_requests_q is ???.  Req needs service.
     * In no case is sync_err_code (at this point) would-block.  Hence: time to check loop condition. */
  } // while (m_user_request && (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK));
} // Async_adapter_receiver::on_sync_io_rcv_done()

template<typename Core_t>
bool Async_adapter_receiver<Core_t>::idle_timer_run(util::Fine_duration timeout)
{
  using flow::util::Lock_guard;

  // Like Async_adapter_sender::send_native_handle() and others (keeping comments light).

  Lock_guard<decltype(m_mutex)> lock(m_mutex);
  return m_sync_io.idle_timer_run(timeout);
}

} // namespace ipc::transport::sync_io
