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

#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/native_socket_stream.hpp"
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <queue>
#include <utility>

namespace ipc::transport
{
// Types.

/**
 * A server object that binds to a `Shared_name` and listens for incoming `Native_socket_stream` connect
 * attempts to that name; and yields connected-peer sync_io::Native_socket_stream objects, one per counterpart
 * opposing `*_connect()`.
 *
 * @note The method is async_accept(); and it yields a PEER-state sync_io::Native_socket_stream object.
 *       Such an object is called a `sync_io` *core*.  It is often more convenient to work with an async-I/O-pattern
 *       object of type Native_socket_stream -- it'll perform work in the background without intervention, etc.
 *       To get this simply do: `Native_socket_stream async_x(std::move(sync_x))`, where
 *       `sync_x` is the aforementioned PEER-state object of type `sync_io::Native_socket_stream`.
 *       (`sync_x` then becomes as-if-default-constructed again.  You can even async_accept() into it again.)
 * @note Depending on your context you may also bundle `sync_x` into Channel `sync_c` -- then create an
 *       async-I/O-pattern Channel via: `auto async_c = sync_c.async_io_obj()`.
 *
 * @see Native_socket_stream doc header.
 * @see Native_socket_stream::sync_connect() doc header.
 *
 * This object is straightforward to use, and really the only difficulty comes from (1) choosing a `Shared_name` and
 * (2) the other side knowing that name.  Before deciding to use it and how to use it, it is essential to read
 * the "How to use" section of Native_socket_stream doc header.  It discusses when to use this, versus an easier
 * (name-free) way to yield a connected-peer Native_socket_stream.  As you'll see, the only difficulty in using the
 * latter is that it does require a *one*-time use of Native_socket_stream_acceptor after all.  However
 * ipc::session takes care of that internally -- so you would not need to set up a Native_socket_stream_acceptor
 * after all.
 *
 * So all in all:
 *   - If you've got ipc::session stuff, you don't need Native_socket_stream_acceptor: you can just open
 *     a Channel with a native-handles pipe; it'll contain a PEER-state Native_socket_stream.
 *   - If you are operating outside ipc::session then this guy here will let you set up a client/server mechanism
 *     for `Native_socket_stream`s.
 *
 * @todo At the moment, *if* one decides to use a Native_socket_stream_acceptor directly -- not really necessary
 * given ipc::session `Channel`-opening capabilities -- the the user must come up with their own naming scheme
 * that avoids name clashes; we could supply an ipc::session-facilitated system for providing this service instead.
 * I.e., ipc::session could either expose a facility for generating the `Shared_name absolute_name` arg to
 * the Native_socket_stream_acceptor ctor (and opposing Native_socket_stream::sync_connect() call).  Alternatively
 * it could provide some kind of Native_socket_stream_acceptor factory and corresponding opposing facility.
 * Long story short, within the ipc::session way of life literally only one acceptor exists, and it is set up
 * (and named) internally to ipc::session.  We could provide a way to facilitate the creation of more
 * acceptors if desired by helping to choose their `Shared_name`s.  (An original "paper" design did specify
 * a naming scheme for this.)
 *
 * @internal
 * ### Implementation ###
 * It is fairly self-explanatory.  Historic note: I (ygoldfel) wrote this after the far more complex
 * Native_socket_stream.  We do use the same concepts here: The ctor immediately starts a thread that reads off
 * all incoming connects ASAP.  There is a surplus queue of such connects; and a deficit queue of pending
 * async_accept() requests from user.  A steady state is maintained of either an empty surplus and possibly non-empty
 * deficit; or vice versa.  However there are no heavy-weight buffers involved, so copy avoidance is not a factor --
 * it is pretty simple.  Also there is no outgoing-direction pipe which further cuts complexity in at least half.
 *
 * Later, after the `sync_io` pattern was introduced, Native_socket_stream was split into
 * Native_socket_stream (identical API and semantics) and `sync_io` core sync_io::Native_socket_stream; and
 * the latter's core logic was moved to reused-elsewhere sync-to-async adapters sync_io::Async_adapter_sender
 * and sync_io::Async_adapter_receiver.  Furthermore (and this is more relevant here) internally
 * the latter guy was changed *away* from reading as many in-messages as were available in the kernel receive buffer --
 * even when no `async_receive_*()` user requests were pending.  (Before the change: Such "cached" messages would
 * need to be copied and stored until the user requests came -- a/k/a the *surplus* was resolved.)  So that was
 * changed; it would only read enough messages to satisfy the currently outstanding `async_receive_*()`s.  The
 * rest would be left in the kernel buffer.  In other words there could be a *deficit* still; but there could never
 * be a *surplus* in that changed (internal) design.
 *
 * Now, despite that change, Native_socket_stream_acceptor was left unchanged.  It still, internally, accepts
 * as many as are available; and caches a surplus internally if one occurs (until async_accept() x N come in to
 * balance it iout).  The rationale?  Well, it was just a good feature.  There's no copying (of anything sizable)
 * involved, and it seemed like a decent idea to not leave handles languishing in some kernel queue.  Essentially
 * the reasoning for that change inside Native_socket_stream (and Blob_stream_mq_receiver) -- well outside our scope
 * here -- simply did not apply to Native_socket_stream_acceptor.
 */
class Native_socket_stream_acceptor :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for type of target peer-socket objects targeted by async_accept().
  using Peer = Native_socket_stream::Sync_io_obj;

  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Native_socket_stream_acceptor;
  /// You may disregard.
  using Async_io_obj = Null_peer;

  // Constants.

  /// `Shared_name` relative-folder fragment (no separators) identifying this resource type.
  static const Shared_name& S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Creates the Native_socket_stream_acceptor and immediately begins listening in the background, so that other
   * process(es) can connect to it -- at the specified name -- once the constructor returns successfully.
   * The operation may fail; see `err_code` arg for how to detect this (either exception or via code return;
   * your choice).  An error will be logged on failure.
   *
   * On success, opposing processes can attempt Native_socket_stream::sync_connect() (or
   * sync_io::Native_socket_stream::sync_connect()) which will quickly succeed
   * yielding an opposing #Peer which will be connected.  On this side, async_accept() is used to grab
   * local peer #Peer.  The connection need not have an async_accept() pending to complete connection
   * as observed by the opposing process.
   *
   * Assuming this ctor succeeds, further background operation may detect an unrecoverable error.  If this occurs,
   * it will occur exactly once, and it is guaranteed no connections will be accepted subsequently; `async_accept()`s
   * beyond that point (once all successfully queued-up connects, if any, are exhausted) will fail with that error.
   *
   * `absolute_name` should be considered carefully.  It cannot clash with another acceptor in existence, and it
   * might even be a mediocre idea to clash with one that *has* recently existed.  Plus the opposing
   * Native_socket_stream (or sync_io::Native_socket_stream) needs to know `absolute_name`.
   *
   * ### Rationale/context ###
   * An Native_socket_stream_acceptor is unlikely to be used except by ipc::session, internally, in any case.
   * Once a session is established, one is ~certain to use ipc::transport::Channel to establish connections, and that
   * intentionally avoids Native_socket_stream_acceptor, because then one need not worry about this thorny naming issue.
   * Nevertheless we provide Native_socket_stream_acceptor as a public API, in case it's generally useful.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param absolute_name
   *        The absolute name at which to bind and listen for connections.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        `boost::asio::error::invalid_argument` (Native_socket_stream_acceptor failed to initialize specifically:
   *        because the given or computed address/name ended up too long to fit into natively-mandated data structures;
   *        or because there are invalid characters therein, most likely forward-slash),
   *        `boost::asio::error::address_in_use` (Native_socket_stream_acceptor failed to initialize due
   *        to a name clash), possibly other system codes (Native_socket_stream_acceptor failed to initialize for some
   *        other reason we could not predict here, but whatever it was was logged).
   */
  explicit Native_socket_stream_acceptor(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                         Error_code* err_code = 0);

  /**
   * Destroys this acceptor which will stop listening in the background and cancel any pending
   * completion handlers by invoking them ASAP with error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   *
   * You must not call this from directly within a completion handler; else undefined behavior.
   *
   * Each pending completion handler will be called from an unspecified thread that is not the calling thread.
   * Any associated captured state for that handler will be freed shortly after the handler returns.
   *
   * We informally but very strongly recommend that your completion handler immediately return if the `Error_code`
   * passed to it is error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.  This is similar to what one should
   * do when using boost.asio and receiving the conceptually identical `operation_aborted` error code to an
   * `async_...()` completion handler.  In both cases, this condition means, "we have decided to shut this thing down,
   * so the completion handlers are simply being informed of this."
   */
  ~Native_socket_stream_acceptor();

  // Methods.

  /**
   * Returns the full name/address to which the constructor bound, or attempted to bind, the listening socket.
   *
   * @return See above.
   */
  const Shared_name& absolute_name() const;

  /**
   * Asynchronously awaits for a peer connection to be established and calls `on_done_func()`,
   * once the connection occurs, or an error occurs, in the former case move-assigning a PEER-state
   * Native_socket_stream object to the passed-in Native_socket_stream `*target_peer`.
   * `on_done_func(Error_code())` is called on success.  `on_done_func(E)`, where `E` is a non-success
   * error code, is called otherwise.  In the latter case `*this` has met an unrecoverable error and should
   * be shut down via the destructor, as no further `async_accept()`s
   * will succeed (they'll quickly yield the same error).
   *
   * Multiple async_accept() calls can be queued while no connection is pending;
   * they will grab incoming connections in FIFO fashion as they arrive.
   *
   * The aforementioned #Peer generated and move-assigned to `*target_peer` on success shall
   * inherit `this->get_logger()` as its `->get_logger()`; and its sync_op::Native_socket_stream::nickname() shall be
   * something descriptive.
   *
   * `on_done_func()` shall be called from some unspecified thread, not the calling thread, but never concurrently with
   * other such completion handlers.  Your implementation must be non-blocking.  Informally we recommend it place the
   * true on-event logic onto some task loop of your own; so ideally it would consist of essentially a single `post(F)`
   * statement of some kind.  There are certainly reasons to not follow this recommendation, though, in some use cases.
   *
   * `on_done_func()` *will* be called; at the latest when the destructor is invoked (see below).
   *
   * You may call this from directly within a completion handler.  Handlers will still always be called
   * non-concurrently, and a handler will never be called from within a handler (so it is safe, e.g., to bracket
   * your handler with a non-recursive mutex lock).
   *
   * #Error_code generated and passed to `on_done_func()`:
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   * other system codes most likely from `boost::asio::error` or `boost::system::errc` (but never
   * would-block), indicating the underlying transport is hosed for that specific reason.
   *
   * @tparam Task_err
   *         Handler type matching signature of `flow::async::Task_asio_err`.
   * @param target_peer
   *        Pointer to sync_io::Native_socket_stream which shall be assigned a PEER-state (connected) as
   *        `on_done_func()` is called.  Not touched on error.
   * @param on_done_func
   *        Completion handler.  See above.  The captured state in this function object shall be freed shortly upon
   *        its completed execution from the unspecified thread.
   */
  template<typename Task_err>
  void async_accept(Peer* target_peer, Task_err&& on_done_func);

private:
  // Types.

  /// Short-hand for callback called on new peer-to-peer connection; or on unrecoverable error.
  using On_peer_accepted_func = flow::async::Task_asio_err;

  /// Short-hand for internally stored PEER-state sync_io::Native_socket_stream in #m_pending_results_q.
  using Peer_ptr = boost::movelib::unique_ptr<Peer>;

  /**
   * Data store representing a deficit user async-accept request that had to be saved due to lacking surplus of
   * finalized peer socket handles.  Essentially this stores args to async_accept() which is an
   * async-operating method.  We store them in queue via #Ptr.
   */
  struct User_request
  {
    /// Short-hand for `unique_ptr` to this.
    using Ptr = boost::movelib::unique_ptr<User_request>;

    // Data.

    /// See Native_socket_stream_acceptor::async_accept() `target_peer`.
    Peer* m_target_peer;

    /// See Native_socket_stream_acceptor::async_accept() `on_done_func`.
    On_peer_accepted_func m_on_done_func;
  }; // struct User_request

  // Methods.

  /**
   * Non-template impl of async_accept().
   *
   * @param target_peer
   *        See async_accept().
   * @param on_done_func
   *        See async_accept().
   */
  void async_accept_impl(Peer* target_peer, On_peer_accepted_func&& on_done_func);

  /**
   * Handler for incoming connection on #m_acceptor.
   *
   * @param sys_err_code
   *        Result code from boost.asio.
   */
  void on_next_peer_socket_or_error(const Error_code& sys_err_code);

  /**
   * In thread W, in steady state, introduces the just-established peer socket handle into the state machine and
   * synchronously advances the state machine into steady state again, with the possible side effect of synchronously
   * invoking the head waiting async-accept user request, if any.  The result, if not emitted, is enqueued to
   * #m_pending_results_q for emission to the next request if any.
   *
   * Pre-condition: Incoming-direction state machine is in steady state; we are in thread W;
   * #m_next_peer_socket is finalized and not empty.
   */
  void finalize_q_surplus_on_success();

  /**
   * In thread W, in steady state *except* for an `Error_code` just pushed to the back of #m_pending_results_q and
   * thus introduced into the state machine, synchronously advances the state machine into steady state again, with the
   * possible side effect of synchronously invoking *all* waiting async-accept user requests, if any.
   * In particular, #m_pending_user_requests_q becomes empty if it was not, while #m_pending_results_q is not changed;
   * in particular the just-pushed `Error_code` remains saved to be emitted to any future async-accept user requests.
   *
   * It is essential to understand the pre-condition that the state machine must be in steady state, followed by
   * exactly one modification to it: namely `Error_code` being pushed onto #m_pending_results_q.  For example it is
   * a bug for #m_pending_user_requests_q to be non-empty, if the pre-push #m_pending_results_q is
   * non-empty also; that isn't steady state since both a deficit and a surplus
   * were in effect before the `Error_code` was pushed.  In other words: this method takes an inductive step only;
   * it doesn't "flush" the state machine.
   */
  void finalize_q_surplus_on_error();

  /**
   * In thread W, gets back to steady state by feeding the given #Error_code (which must be the sole element in
   * #m_pending_results_q) to all queued user requests, popping them all.  The pre-condition is that doing so *would*
   * in fact get the deficit and surplus queues collectively to steady state; and also that there *is* in fact
   * a deficit (at least one request in #m_pending_user_requests_q).
   *
   * @param err_code
   *        The code to feed (must be at top of #m_pending_results_q).  This is supplied as an arg for perf at most.
   */
  void feed_error_result_to_deficit(const Error_code& err_code);

  /**
   * In thread W, gets back to steady state by feeding the given just-connected peer socket (which must have just been
   * popped from #m_pending_results_q) to the first queued user request, popping it.
   *
   * @param peer
   *        The peer stream handle to feed (must have just been popped from #m_pending_results_q).
   */
  void feed_success_result_to_deficit(Peer_ptr&& peer);

  // Data.

  /// See absolute_name().
  const Shared_name m_absolute_name;

  /**
   * Queue storing deficit async-accept requests queued up due to lacking pending ready peer socket handles in
   * #m_pending_results_q at async_accept() time.  In steady state the invariant is: either #m_pending_user_requests_q
   * is empty, or #m_pending_results_q is empty, or both are empty.  If in non-steady state each has at least one
   * handle, then any handles in #m_pending_results_q are "fed" to the callbacks in #m_pending_user_requests_q, thus
   * removing that number of entries in each queue and emptying #m_pending_results_q.
   *
   * Accessed from thread W only; hence needs no locking.
   */
  std::queue<User_request::Ptr> m_pending_user_requests_q;

  /**
   * Queue storing surplus finalized async-accept results queued up due to lacking async_accept() requests in
   * #m_pending_user_requests_q at connection finalization time.  There are 0+ peer socket handles, capped by 0 or 1
   * `Error_code`.
   *
   * Accessed from thread W only; hence needs no locking.
   */
  std::queue<std::variant<Peer_ptr, Error_code>> m_pending_results_q;

  /**
   * A single-threaded async task loop that starts in constructor and ends in destructor.  We refer to this informally
   * as thread W in comments.
   *
   * Ordering: Should be declared before #m_acceptor: It should destruct before its attached `Task_engine` does.
   */
  flow::async::Single_thread_task_loop m_worker;

  /**
   * Unix domain socket acceptor.  It is only accessed in thread W.  Assuming successful setup, it's listening
   * continuously in thread W, via async loop #m_worker.
   */
  boost::movelib::unique_ptr<asio_local_stream_socket::Acceptor> m_acceptor;

  /**
   * Unix domain peer socket, always empty/unconnected while a background `m_acceptor.async_accept()` is proceeding;
   * then (assuming a successful accept op) connected at the start to the `async_accept()` callback; then back to
   * empty/unconnected again just before the next `async_accept()` call; and so on.  Only accessed in thread W.
   *
   * Since there's only one thread, we can keep reusing this one target socket.  When the time comes (in the callback)
   * to pass it to the rest of the program, a "new" socket is move-constructed from it, thus making it
   * empty/unconnected again.
   *
   * ### Rationale/notes ###
   * As noted, this setup relies on move-construction.  Alternatively we could have used a `shared_ptr` pattern.
   * Stylistically it should be at least as simple/elegant.  Perf-wise, as of this writing, I (ygoldfel) have not
   * rigorously compared the two; but by definition move constructions should be close to optimal perf-wise, unless
   * boost.asio guys willfully made theirs slow.
   *
   * @todo Perform a rigorous analysis of the perf and style trade-offs between move-construction-based patterns
   * versus `shared_ptr`-based ones, possibly focusing on boost.asio socket objects in particular. */
  asio_local_stream_socket::Peer_socket m_next_peer_socket;
}; // class Native_socket_stream_acceptor

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Task_err>
void Native_socket_stream_acceptor::async_accept(Peer* target_peer, Task_err&& on_done_func)
{
  async_accept_impl(target_peer, On_peer_accepted_func(std::move(on_done_func)));
}

} // namespace ipc::transport
