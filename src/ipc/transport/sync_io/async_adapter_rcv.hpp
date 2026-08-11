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
#include "ipc/transport/blob_transport_stats.hpp"
#include "ipc/transport/error.hpp"
#include <flow/log/log.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include <boost/move/make_unique.hpp>
#include <queue>
#include <variant>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Type that adapts a given PEER-state sync_io::Native_handle_receiver or sync_io::Blob_receiver *core*
 * into the async-I/O-pattern Native_handle_receiver or Blob_receiver.  State-mutating logic of the latter is forwarded
 * to a `*this`; while trivial `const` (in PEER state) things like `.receive_blob_max_size()` are forwarded directly to
 * the core `sync_io::X`.
 *
 * Flow-IPC uses a `*this` to implement each of transport::Native_socket_stream (in-direction) and
 * transport::Blob_stream_mq_receiver; but this is a public API, as one can implement any custom
 * transport::Blob_receiver (et al) in terms of the corresponding custom sync_io::Blob_receiver (et al) impl.
 * It would be an advanced task but nevertheless fully supported/intended.
 *
 * @internal
 * @see transport::Native_socket_stream_impl uses this for 99% of its incoming-direction
 *      (PEER-state by definition) logic.
 * @see transport::Blob_stream_mq_receiver_impl uses this for 99% of its logic.
 * @endinternal
 *
 * @see Async_adapter_sender for the opposite-direction thing.  E.g., transport::Native_socket_stream_impl
 *      uses that for 99% of its outgoing-direction logic.
 *
 * @internal
 * Impl
 * ----
 * ### Threads and thread nomenclature; locking ###
 * Thread U, thread W... locking... just see those sections in transport::Native_socket_stream_impl class doc header.
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
 *     we emit the result (`Error_code`, `sz`) to the `User_request`-stored `m_on_done_func` (the completion
 *     handler from the user).  Then we pop `m_pending_user_requests_q` (unless empty -- no further "deficit")
 *     into `m_user_request` and service *that* one via `m_sync_io.async_receive_*()`.  Rinse/repeat.
 *
 * If the destructor is invoked before `m_user_request` can get serviced, then in the dtor
 * we execute `on_done_func(E)`, where E is operation-aborted.  Once that's done the dtor can finish.
 *
 * Update: The above remains quite accurate; but we added batch-receiving APIs to the `*_receiver` concepts and
 * therefore impls and therefore `*this`.  These are async_receive_native_handle_batch() and
 * async_receive_blob_batch().  All that changes is that a given user-request can now be either a single-message
 * receive request, or a batch receive request.  So #User_request can encode one or the other; so it is a
 * `variant` to that effect.  Hence when processing a #User_request in #m_user_request (or the queue, if we need
 * to look in there directly), we check which one it is and then invoke the appropriate #Core API
 * (e.g., `async_receive_blob()` versus `async_receive_blob_batch()`).
 * @endinternal
 *
 * @tparam Core_t
 *         The `sync_io::X` type being adapted into async-I/O-pattern `X`.
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
   * operation-aborted error code.  In our case that is either nothing or 1+ `async_receive_*()` completion
   * handler(s).  If applicable the dtor returns once such handler(s) has/have completed in an unspecified thread
   * that is not the calling thread.
   */
  ~Async_adapter_receiver();

  // Methods.

  /**
   * See transport::Native_handle_receiver counterpart.  However, this one is `void`, as there is no way `*this` is not
   * in PEER state (by definition).
   *
   * @param target_hndl
   *        See above.
   * @param target_meta_blob
   *        See above.
   * @param on_done_func
   *        See above.
   */
  void async_receive_native_handle(Native_handle* target_hndl,
                                   const util::Blob_mutable& target_meta_blob,
                                   flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See transport::Blob_receiver counterpart.  However, this one is `void`, as there is no way `*this` is not in PEER
   * state (by definition).
   *
   * @param target_blob
   *        See above.
   * @param on_done_func
   *        See above.
   */
  void async_receive_blob(const util::Blob_mutable& target_blob,
                          flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See transport::Native_handle_receiver counterpart.  However, this one is `void`, as there is no way `*this` is not
   * in PEER state (by definition).
   *
   * @tparam Batch
   *         `Core::Native_handle_batch_in<Msg_resource>` for some type `Msg_resource` which... see above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param on_done_func
   *        See above.
   */
  template<typename Batch>
  void async_receive_native_handle_batch(Batch* batch, bool assume_would_block,
                                         flow::async::Task_asio_err&& on_done_func);

  /**
   * See transport::Blob_receiver counterpart.  However, this one is `void`, as there is no way `*this` is not
   * in PEER state (by definition).
   *
   * @tparam Batch
   *         `Core::Blob_batch_in<Msg_resource>` for some type `Msg_resource` which... see above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param on_done_func
   *        See above.
   */
  template<typename Batch>
  void async_receive_blob_batch(Batch* batch, bool assume_would_block,
                                flow::async::Task_asio_err&& on_done_func);

  /**
   * See transport::Native_handle_receiver counterpart.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout);

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  stat::Blob_rcv_stats blob_receive_stats() const;

  /// See Native_socket_stream counterpart.
  void blob_receive_stats_reset();

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  stat::Blob_rcv_stats native_handle_receive_stats() const;

  /// See Native_socket_stream counterpart.
  void native_handle_receive_stats_reset();

private:
  // Types.

  /**
   * Data store representing a deficit user single-message async-receive request: either one being currently handled
   * by `m_sync_io` -- which can handle one `m_sync_io.async_receive_*()` at a time, no more -- or
   * one queued up behind it, if `async_receive_*()` was called before the current one could complete.
   *
   * Essentially this stores args to async_receive_native_handle() (or degenerate version, async_receive_blob())
   * which is an async-operating method.  We store them in queue via #User_req_ptr.
   */
  struct User_request_one
  {
    // Data.

    /// See async_receive_native_handle() `target_hndl`.  Null for `async_receive_blob()`.
    Native_handle* m_target_hndl_ptr;

    /// See async_receive_native_handle() `target_meta_blob`.  Or see async_receive_blob() `target_blob`.
    util::Blob_mutable m_target_meta_blob;

    /// See async_receive_native_handle() or async_receive_blob() `on_done_func`.
    flow::async::Task_asio_err_sz m_on_done_func;
  }; // struct User_request_one

  /**
   * Analogous to User_request_one but for a batch async-receive request as opposed to single-message request.
   *
   * Essentially this stores args to async_receive_native_handle_batch() or async_receive_blob_batch().
   * We store them in queue via #User_req_ptr.
   *
   * ### Impl design ###
   * Conceptually this is straightforward; much like User_request_one this basically stores the args from the user
   * to `async_receive_*_batch()`.  The impl, however, is somewhat less straightforward than in User_request_one.
   * Specifically, consider these bits of info:
   *   - (Easy) Whether it was `async_receive_native_handle_batch()` or `async_receive_blob_batch()`; determines
   *     which of the two eponymous methods of #m_sync_io we shall forward-to (call).
   *   - (Harder) The `batch` argument.
   *
   * The latter is really the crux (the former we save using the same mechanism opportunistically; but in and of itself
   * it could have just been a `bool` flag or similar; e.g., User_request_one encodes it in `bool(m_target_hndl_ptr)`.)
   * The problem with `batch` is its type is template-parameterized `Batch*`, where
   * `Batch` is a `typename` template parameter.  We have to save this info in #m_user_request and/or
   * #m_pending_user_requests_q, and when forwarding-to `m_sync_io.async_receive_*_batch()` we must
   * give it a value of type `Batch*`.  So it's a standard type-erasure
   * situation.  As such we use a typical technique in solving it: use `Function<T>`,
   * where `T` is a signature spec that is parameterized on `Batch`.  That is we
   * have #m_sync_rcv_batch_func which captures `batch` of whichever type (code generated at compile-time) and invokes
   * the appropriate method, either the `_native_handle_` one or the `_blob_` one, thus compactly (code-wise anyway)
   * encoding both the would-be flag (bullet point 1) and the `batch` value of the proper type (bullet point 2).
   *
   * The code is fairly slick, but there's a cost which is to some extent performance; `Function<>` does the
   * type erasure leg-work for us, but it is *somewhat* heavyweight, and calling it involves some `virtual` overhead.
   * The RAM weight doesn't much matter (overall it is small, and we move these around via `User_req_ptr`),
   * and we consider the `virtual` overhead to be acceptable.  (If profiling shows otherwise, we can revisit.)
   */
  struct User_request_batch
  {
    // Types.

    /**
     * Short-hand for polymorphic type of function which is the batch-receive equivalent of
     * Async_adapter_receiver::sync_receive().  See "Impl design" in `struct` doc header please.
     *
     * The following info is captured or encoded in the function body:
     *   - (Opportunistic/for convenience) Our daddy's `m_sync_io` ref and its appropriate method:
     *     either `async_receive_native_handle_batch()` or `async_receive_blob_batch()`.
     *   - (Opportunistic/for convenience) Value of arg `bool assume_would_block`.
     *   - (Required/couldn't be done another way) The type `Batch` (template parameter to the above method;
     *     part of the type of `batch` argument to the above method).
     *     - (Required/couldn't be done another way) `batch` arg itself.
     *
     * The following are the args to it:
     *   - `Error_code* sync_err_code` arg to pass to the above method.
     */
    using Rcv_batch_func = Function<void (Error_code*)>;

    // Data.

    /// See #Rcv_batch_func doc header.  Again though: This is the equivalent of `sync_receive()` for batch-receives.
    Rcv_batch_func m_sync_rcv_batch_func;

    /// See async_receive_native_handle_batch() or async_receive_blob_batch() `on_done_func`.
    flow::async::Task_asio_err m_on_done_func;
  }; // struct User_request_batch

  /// An async-receive request made by user, to be stored in #m_user_request and/or #m_pending_user_requests_q.
  using User_request = std::variant<User_request_one, User_request_batch>;

  /// Short-hand for smart-pointer handle to #User_request.
  using User_req_ptr = boost::movelib::unique_ptr<User_request>;

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
  void async_receive_impl(Native_handle* target_hndl_or_null, const util::Blob_mutable& target_meta_blob,
                          flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * With the pre-condition that #m_user_request is of type User_request_one (that is originating from
   * a single-message async-receive user request as opposed to batch-receive), executes the core synchronous
   * call to single-message `m_sync_io.async_receive_{native_handle|blob}()`.  If it succeeds synchronously,
   * the result is indicated via the out-args.  If it encounters would-block, `*sync_err_code` indicates this
   * on return, and async-wait is issued, and when/if user reports the completion of that wait,
   * our on_sync_io_rcv_done() shall execute.
   *
   * @note User_request_batch::m_sync_rcv_batch_func is the equivalent of us for batch-receives.
   *
   * @param req
   *        `get<User_request_one>(*m_user_request)()`.  It is an arg for perf only, as (as of this writing anyway)
   *        the caller would already have this.
   * @param sync_err_code
   *        See above.
   * @param sz
   *        See above; if `*sync_err_code` ends up falsy, `*sz` is set to 1+, the number of bytes received;
   *        else to 0.
   */
  void sync_receive(const User_request_one& req, Error_code* sync_err_code, size_t* sz);

  /**
   * Body of async_receive_native_handle_batch() and async_receive_blob_batch(); with t-param `HNDL_ELSE_BLOB`
   * `true` or `false` depending on whether it's the latter as opposed to the former.
   *
   * @tparam HNDL_ELSE_BLOB
   *         `true` if implementing async_receive_native_handle_batch();
   *         `false` if async_receive_blob_batch().
   * @tparam Batch
   *         See async_receive_native_handle_batch() or async_receive_blob_batch().
   * @param batch
   *        See async_receive_native_handle_batch() or async_receive_blob_batch().
   * @param assume_would_block
   *        See async_receive_native_handle_batch() or async_receive_blob_batch().
   * @param on_done_func
   *        See async_receive_native_handle_batch() or async_receive_blob_batch().
   */
  template<typename Batch, bool HNDL_ELSE_BLOB>
  void async_receive_batch_impl(Batch* batch, bool assume_would_block, flow::async::Task_asio_err&& on_done_func);

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
   * (or pipe-hosing would-be) single-message or batch (sized 1+ messages).  Then for each further
   * in-message/in-batch process_msg_or_error() is again invoked.
   *
   * For each request satisfied, a separate user handler is posted onto thread W to execute in order.
   *
   * @param err_code
   *        Result to pass to user (if truthy, all pending requests; else to #m_user_request only).
   * @param sz_if_applicable
   *        Result to pass to user (ditto); ignored/zero if #m_user_request encodes a User_request_batch;
   *        used/applicable (if `!err_code` and) if `m_user_request` encodes a User_request_one.
   */
  void on_sync_io_rcv_done(const Error_code& err_code, size_t sz_if_applicable);

  /**
   * Invoked from thread U/W (async_receive_impl() or async_receive_batch_impl()) or W (active-event API), handles
   * a completed `m_sync_io.async_receive_*()` -- whose results are to be given as args -- by (1) updating
   * #m_user_request and #m_pending_user_requests_q and (2) posting any appropriate handlers onto thread W.
   *
   * See notes for on_sync_io_rcv_done().
   *
   * @param err_code
   *        See on_sync_io_rcv_done().
   * @param sz_if_applicable
   *        See on_sync_io_rcv_done().
   */
  void process_msg_or_error(const Error_code& err_code, size_t sz_if_applicable);

  // Data.

  /// See `log_pfx` arg of ctor.
  const std::string m_log_pfx;

  /**
   * The *head slot* containing the currently-being-serviced "deficit" async-receive request, with meta-blob(s)
   * *potentially* being async-written to; null if there is no pending `async_receive_*()`.
   * It is the "fulcrum" of the consumer-producer state machine described in doc header impl section's design
   * discussion: If null there is no deficit; if not null there is an *overall deficit*.  In the former case,
   * at steady state, `m_pending_user_requests_q.empty() == true`.
   *
   * Protected by #m_mutex.
   *
   * @see #m_pending_user_requests_q
   */
  User_req_ptr m_user_request;

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
  std::queue<User_req_ptr> m_pending_user_requests_q;

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
      Lock_guard<decltype(m_mutex)> lock{m_mutex};

      /* Inform m_sync_io of the event.  This can synchronously invoke handler we have registered via m_sync_io
       * API (e.g., `async_receive_*()`). */

      (*on_active_ev_func)();
      // (That would have logged sufficiently inside m_sync_io; let's not spam further.)

      // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
    }); // hndl_of_interest->async_wait()
  }); // m_sync_io.start_receive_blob_ops()
  assert(ok);
} // Async_adapter_receiver::Async_adapter_receiver()

template<typename Core_t>
Async_adapter_receiver<Core_t>::~Async_adapter_receiver()
{
  using flow::async::Single_thread_task_loop;
  using flow::async::reset_thread_pinning;
  using flow::util::ostream_op_string;
  using std::get_if;
  using std::get;

  /* Pre-condition: m_worker is stop()ed, and any pending tasks on it have been executed.
   * Our promised job is to invoke any pending handlers with operation-aborted.
   * The decision to do it from a one-off thread is explained in transport::Native_socket_stream_impl::~dtor()
   * and used in a few places; so see that.  Let's just do it.
   * @todo It would be cool, I guess, to do it all in one one-off thread instead of potentially starting, like,
   * 3 for some of our customers.  Well, whatever. */

  if (m_user_request)
  {
    Single_thread_task_loop one_thread{get_logger(),
                                       ostream_op_string("ARcDeinit-", m_log_pfx)};
    one_thread.start([&]()
    {
      reset_thread_pinning(get_logger()); // Don't inherit any strange core-affinity.  Float free.

      FLOW_LOG_TRACE("Running head slot async-receive completion handler.");
      auto& req = *m_user_request;
      if (auto req1 = get_if<User_request_one>(&req))
      {
        req1->m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, 0);
      }
      else
      {
        get<User_request_batch>(req).m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
      }
      FLOW_LOG_TRACE("User receive handler finished.");

      while (!m_pending_user_requests_q.empty())
      {
        FLOW_LOG_TRACE("Running a queued async-receive completion handler.");
        auto& req = *(m_pending_user_requests_q.front());
        if (auto req1 = get_if<User_request_one>(&req))
        {
          req1->m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, 0);
        }
        else
        {
          get<User_request_batch>(req).m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
        }

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
  async_receive_impl(target_hndl, target_meta_blob, std::move(on_done_func));

  /* Note, if our customer lacks async_receive_native_handle(), then they'll forward to (call) this method;
   * therefore it (*this being a template instance) will never be compiled; therefore target_hndl_or_null will never
   * be non-null; which is why that assert(false) inside `if constexpr(!S_TRANSMIT_NATIVE_HANDLES)` compile-time clause
   * shall never be reached. */
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::async_receive_blob(const util::Blob_mutable& target_blob,
                                                        flow::async::Task_asio_err_sz&& on_done_func)
{
  async_receive_impl(nullptr, target_blob, std::move(on_done_func));
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::async_receive_impl(Native_handle* target_hndl_or_null,
                                                        const util::Blob_mutable& target_meta_blob,
                                                        flow::async::Task_asio_err_sz&& on_done_func)
{
  using flow::util::Lock_guard;
  using std::in_place_type;
  using std::get;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

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

  auto new_user_request = boost::movelib::make_unique<User_request>(in_place_type<User_request_one>);
  auto& req1 = get<User_request_one>(*new_user_request);

  req1.m_target_hndl_ptr = target_hndl_or_null;
  req1.m_target_meta_blob = target_meta_blob;
  req1.m_on_done_func = std::move(on_done_func);

  /* We will be accessing m_user_request, m_pending_user_requests_q, and possibly m_sync_io receive-ops
   * sub-API -- while ctor's ev-wait function's async_wait() handler will be accessing them too from thread W -- so: */

  Lock_guard<decltype(m_mutex)> lock{m_mutex};

  if (m_user_request)
  {
    m_pending_user_requests_q.emplace(std::move(new_user_request));
    FLOW_LOG_TRACE("At least 1 async-receive request is already in progress.  "
                   "After registering the new async-receive request: Head slot is non-empty; and "
                   "subsequently-pending deficit queue has size [" << m_pending_user_requests_q.size() << "].  "
                   "Will sync-IO-receive to handle this request once it reaches the front of that queue.");
    return; // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
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

  sync_receive(req1, &sync_err_code, &sync_sz);

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Async-wait started by m_sync_io.  It logged plenty.  We live to fight another day.
    return; // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
  }
  // else:

  /* Process the message and nullify m_user_request.  (It would also promote any m_pending_user_requests_q head
   * to m_user_request; but in our case that is not possible.  We are still in the user async-receive API!) */
  process_msg_or_error(sync_err_code, sync_sz);

  FLOW_LOG_TRACE("Message was immediately available; synchronously returned to user; handler posted onto "
                 "async worker thread.  Done until next request.");

  // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
} // Async_adapter_receiver::async_receive_impl()

template<typename Core_t>
void Async_adapter_receiver<Core_t>::sync_receive(const User_request_one& req, Error_code* sync_err_code, size_t* sz)
{
  using std::get;

#ifndef NDEBUG
  bool ok;
#endif

  if (req.m_target_hndl_ptr)
  {
    if constexpr(Core::S_TRANSMIT_NATIVE_HANDLES) // Prevent compile error from code that could never be reached.
    {
#ifndef NDEBUG
      ok =
#endif
      m_sync_io.async_receive_native_handle(req.m_target_hndl_ptr, req.m_target_meta_blob,
                                            sync_err_code, sz,
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
  } // if (req.m_target_hndl_ptr)
  else // Same deal/keeping comments light.
  {
#ifndef NDEBUG
    ok =
#endif
    m_sync_io.async_receive_blob(req.m_target_meta_blob, sync_err_code, sz,
                                 [this](const Error_code& err_code, size_t sz)
                                   { on_sync_io_rcv_done(err_code, sz); });
  } // else if (!req.m_target_hndl_ptr)

  assert(ok && "We are by definition in PEER state, and ctor starts receive-ops, and we never start a "
               "sync_io async-receive before ensuring previous one has executed; so that should never "
               "return false.  Bug somewhere?");
} // Async_adapter_receiver::sync_receive()

template<typename Core_t>
template<typename Batch>
void Async_adapter_receiver<Core_t>::async_receive_native_handle_batch(Batch* batch, bool assume_would_block,
                                                                       flow::async::Task_asio_err&& on_done_func)
{
  async_receive_batch_impl<Batch, true>(batch, assume_would_block, std::move(on_done_func));
}

template<typename Core_t>
template<typename Batch>
void Async_adapter_receiver<Core_t>::async_receive_blob_batch(Batch* batch, bool assume_would_block,
                                                              flow::async::Task_asio_err&& on_done_func)
{
  async_receive_batch_impl<Batch, false>(batch, assume_would_block, std::move(on_done_func));
}

template<typename Core_t>
template<typename Batch, bool HNDL_ELSE_BLOB>
void Async_adapter_receiver<Core_t>::async_receive_batch_impl(Batch* batch,
                                                              bool assume_would_block,
                                                              flow::async::Task_asio_err&& on_done_func)
{
  using flow::async::Task_asio_err;
  using flow::util::Lock_guard;
  using std::in_place_type;
  using std::get;

  // We are in thread U (or thread W in a completion handler, but not concurrently).

  /* This is mainly the equivalent of async_receive_impl(), except:
   *   - it handles receive-batch request instead of receive-one-message request; and
   *   - batch-receiving has certain technicalities (see User_request_batch doc header) which cause us to
   *     (instead of simply being able to call sync_receive() (as for single-message case)) prepare its equivalent
   *     and save it inside the User_request as a polymorphic Function<>.
   *
   * Other than that the same comments, including especially the big one at the top of
   * async_receive_impl(), apply.  Keeping comments light. */

  FLOW_LOG_TRACE(m_log_pfx << ": Incoming user async-receive-batch (with handles: no) request on "
                 "batch [" << *batch << "] with assume-would-block? = [" << assume_would_block << "]; "
                 "HNDL_ELSE_BLOB = [" << HNDL_ELSE_BLOB << "].  "
                 "In worker now? = [" << m_worker.in_thread() << "].");

  auto new_user_request = boost::movelib::make_unique<User_request>(in_place_type<User_request_batch>);
  auto& req_batch = get<User_request_batch>(*new_user_request);

  req_batch.m_on_done_func = std::move(on_done_func);
  // Note the type-erasure achieved by `batch` capture.
  req_batch.m_sync_rcv_batch_func = [this, assume_would_block, batch]
                                      (Error_code* sync_err_code)
  {
#ifndef NDEBUG
    bool ok;
#endif

    if constexpr(HNDL_ELSE_BLOB)
    {
#ifndef NDEBUG
      ok =
#endif
      m_sync_io.template async_receive_native_handle_batch<typename Batch::Msg_resource>
        (batch, assume_would_block, sync_err_code,
         [this](const Error_code& err_code)
      {
        // We are in thread W.  m_mutex is locked.
        on_sync_io_rcv_done(err_code, 0); // Emit to handler; possibly pop queue and continue chain.
        // Pass zero for sz_if_applicable ^-- (it is not applicable to batch-receiving).
      });
    }
    else // if constexpr(!HNDL_ELSE_BLOB)
    {
#ifndef NDEBUG
      ok =
#endif
      m_sync_io.template async_receive_blob_batch<typename Batch::Msg_resource>
        (batch, assume_would_block, sync_err_code,
         [this](const Error_code& err_code) { on_sync_io_rcv_done(err_code, 0); });
    } // else // if constexpr(!HNDL_ELSE_BLOB)

    assert(ok && "We are by definition in PEER state, and ctor starts receive-ops, and we never start a "
                 "sync_io async-receive before ensuring previous one has executed; so that should never "
                 "return false.  Bug somewhere?");
  }; // req_batch.m_sync_rcv_batch_func =

  // Accessing m_user_request, m_pending_user_requests_q, and possibly m_sync_io receive-ops => lock.
  Lock_guard<decltype(m_mutex)> lock{m_mutex};

  if (m_user_request)
  {
    m_pending_user_requests_q.emplace(std::move(new_user_request));
    FLOW_LOG_TRACE("At least 1 async-receive request is already in progress.  "
                   "After registering the new async-receive request: Head slot is non-empty; and "
                   "subsequently-pending deficit queue has size [" << m_pending_user_requests_q.size() << "].  "
                   "Will sync-IO-receive to handle this request once it reaches the front of that queue.");

    return; // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
  }
  // else if (!m_user_request):

  FLOW_LOG_TRACE("No async-receive request is currently in progress.  Starting sync-IO-receive chain to service the "
                 "new request and any further-queued requests that might appear in the meantime.");
  m_user_request = std::move(new_user_request);
  // new_user_request is now hosed.

  Error_code sync_err_code;
  req_batch.m_sync_rcv_batch_func(&sync_err_code);

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    return; // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
  }
  // else:

  // Process the message and nullify m_user_request.
  process_msg_or_error(sync_err_code, 0); // Pass zero for sz_if_applicable (it is not applicable to batches).

  FLOW_LOG_TRACE("In-batch was immediately available; synchronously returned to user; handler posted onto "
                 "async worker thread.  Done until next request.");

  // Lock_guard<decltype(m_mutex)> lock{m_mutex}: unlocks here.
} // Async_adapter_receiver::async_receive_batch_impl()

template<typename Core_t>
void Async_adapter_receiver<Core_t>::process_msg_or_error(const Error_code& err_code, size_t sz_if_applicable)
{
  using std::get_if;
  using std::get;

  // We are in thread U or W.  m_mutex is locked.

  assert(err_code != boost::asio::error::operation_aborted);
  assert((err_code != error::Code::S_SYNC_IO_WOULD_BLOCK) && "By our contract.");

  FLOW_LOG_TRACE(m_log_pfx << ": Earlier async-wait => event active => rcv-mutex lock => "
                 "on-active-event-func => sync_io module => on-rcv-done handler => here.  "
                 "Or else: rcv-mutex lock => sync_io module => no async-wait needed => here.");

  /* As noted in our doc header, we have roughly two items on the agenda.
   *
   * 1, we need to invoke m_user_request->m_on_done_func, passing it the results (which ise/are our arg(s)).
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
   *     U (m_sync_io.async_receive_*() succeeded synchronously, and this->async_receive[_*]_impl()
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
  User_req_ptr ex_user_request{std::move(m_user_request)};
  assert(!m_user_request);

  decltype(m_pending_user_requests_q) ex_pending_user_requests_q_or_none;
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
  m_worker.post([this, err_code, sz_if_applicable,
                 /* Have to upgrade to shared_ptr<>s due to capturing requiring copyability (even though copying is not
                  * actually invoked by us).  unique_ptr and queue<unique_ptr> = not copyable. */
                 ex_user_request = boost::shared_ptr<User_request>{std::move(ex_user_request)},
                 ex_pending_user_requests_q_or_none
                   = boost::make_shared<decltype(ex_pending_user_requests_q_or_none)>
                       (std::move(ex_pending_user_requests_q_or_none))]()
                  mutable
  {
    // We are in thread W.  Nothing is locked.

    assert(ex_user_request);
    FLOW_LOG_TRACE(m_log_pfx << ": Invoking head handler.");

    auto& req = *ex_user_request;
    if (auto req1 = get_if<User_request_one>(&req))
    {
      req1->m_on_done_func(err_code, sz_if_applicable);
    }
    else
    {
      assert((sz_if_applicable == 0) && "sz_if_applicable is not applicable to batches; should have been passed as 0.");
      get<User_request_batch>(req).m_on_done_func(err_code);
    }

    FLOW_LOG_TRACE("Handler completed.");
    if (!ex_pending_user_requests_q_or_none->empty())
    {
      assert(err_code);
      assert(sz_if_applicable == 0);

      FLOW_LOG_TRACE(m_log_pfx << ": Invoking [" << ex_pending_user_requests_q_or_none->size() << "] "
                     "pending handlers in one shot (due to error).");
      while (!ex_pending_user_requests_q_or_none->empty())
      {
        auto& req = *(ex_pending_user_requests_q_or_none->front());
        if (auto req1 = get_if<User_request_one>(&req))
        {
          req1->m_on_done_func(err_code, 0);
        }
        else
        {
          get<User_request_batch>(req).m_on_done_func(err_code);
        }

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
void Async_adapter_receiver<Core_t>::on_sync_io_rcv_done(const Error_code& err_code, size_t sz_if_applicable)
{
  using std::queue;
  using std::get_if;
  using std::get;

  // We are in thread W.  m_mutex is locked.

  assert(err_code != boost::asio::error::operation_aborted);
  assert(err_code != error::Code::S_SYNC_IO_WOULD_BLOCK);

  FLOW_LOG_TRACE(m_log_pfx << ": Earlier async-wait => event active => rcv-mutex lock => "
                 "on-active-event-func => sync_io module => here (on-rcv-done handler).");

  /* This is not *too* different from thread-U (or thread-W if invoked from our own handler)
   * async_receive[_*]_impl()... except that in our case more requests may have been queued (as summarized
   * in top comment in that method) during our async-wait that just finished.  And, of course, we need
   * to process the ready message/batch first-thing.  But let's say that's taken care of.  After that: we can't just
   * stop; there may be queued requests.  We shall process them as synchronously as possible in a do-while()
   * loop.  That's the executive summary. */

  /* So handle the message/batch or error -- W-post any relevant handlers; update m_user_request and
   * m_pending_user_requests_q. */
  process_msg_or_error(err_code, sz_if_applicable);

  Error_code sync_err_code;
  auto& sync_sz = sz_if_applicable; // (Might as well reuse the arg.)

  /* First iteration: sync_err_code is definitely not would-block; m_user_request may be null.
   * Subsequent iterations: sync_err_code may be would-block. */
  while (m_user_request && (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK))
  {
    auto& req = *m_user_request;
    if (auto req1 = get_if<User_request_one>(&req))
    {
      sync_receive(*req1, &sync_err_code, &sync_sz);
    }
    else
    {
      get<User_request_batch>(req).m_sync_rcv_batch_func(&sync_err_code);

      // sync_sz not applicable; for cleanliness (as of this writing process_msg_or_error() can trip assert otherwise):
      sync_sz = 0;
    }

    if (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      /* Remaining outcomes: sync_err_code truthy => process_msg_or_error() will do the right thing.
       *                     sync_err_code is falsy => ditto. */

      process_msg_or_error(sync_err_code, sync_sz);

      /* Outcomes: Error => m_user_request is null, m_pending_user_requests_q is null.  No req to service.  Loop end.
       *           Success => m_user_request is null, m_pending_user_requests_q is null.  No req to service.  Loop end.
       *           Success => m_user_request is NOT null, m_pending_user_requests_q is ???.  Req needs service.
       * In no case is sync_err_code (at this point) would-block.  Hence: time to check loop condition. */
    }
    // else if (sync_err_code == SYNC_IO_WOULD_BLOCK) { Loop will end. }
  } // while (m_user_request && (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK));
} // Async_adapter_receiver::on_sync_io_rcv_done()

template<typename Core_t>
bool Async_adapter_receiver<Core_t>::idle_timer_run(util::Fine_duration timeout)
{
  using flow::util::Lock_guard;

  // Like Async_adapter_sender::send_native_handle() and others (keeping comments light).

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  return m_sync_io.idle_timer_run(timeout);
}

template<typename Core_t>
stat::Blob_rcv_stats Async_adapter_receiver<Core_t>::blob_receive_stats() const
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  return m_sync_io.blob_receive_stats();
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::blob_receive_stats_reset()
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  m_sync_io.blob_receive_stats_reset();
}

template<typename Core_t>
stat::Blob_rcv_stats Async_adapter_receiver<Core_t>::native_handle_receive_stats() const
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  return m_sync_io.native_handle_receive_stats();
}

template<typename Core_t>
void Async_adapter_receiver<Core_t>::native_handle_receive_stats_reset()
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  m_sync_io.native_handle_receive_stats_reset();
}

} // namespace ipc::transport::sync_io
