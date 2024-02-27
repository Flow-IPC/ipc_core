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

#include "ipc/transport/native_socket_stream.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/transport/sync_io/detail/async_adapter_snd.hpp"
#include "ipc/transport/sync_io/detail/async_adapter_rcv.hpp"
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include <cstddef>
#include <flow/async/single_thread_task_loop.hpp>

namespace ipc::transport
{

// Types.

/**
 * Internal, non-movable pImpl implementation of Native_socket_stream class.
 * In and of itself it would have been directly and publicly usable; however Native_socket_stream adds move semantics
 * which are essential to cooperation with Native_socket_stream_acceptor and overall consistency with the rest
 * of ipc::transport API and, arguably, boost.asio API design.
 *
 * @see All discussion of the public API is in Native_socket_stream doc header; that class forwards to this one.
 *      All discussion of pImpl-related notions is also there.  See that doc header first please.  Then come back here.
 *
 * Impl design
 * -----------
 * ### Intro / history ###
 * Native_socket_stream::Impl is, one could argue, the core class of this entire library, Flow-IPC.
 * Beyond that, even the closest alternatives (which, in the first place, cannot do some of what it can -- namely the
 * `Blob_stream_mq_*` guys cannot transmit native handles) tend to use many of the same techniques.
 * So, like, this impl design is important -- for performance on behalf of the ipc::transport use and ipc::session
 * internals (which bases its session master channel on a single-`Native_socket_stream` Channel); and as
 * a conceptual template for how various other things are implemented (at least in part).
 *
 * Truthfully, in some cases historically speaking I (ygoldfel) tweaked aspects of the concept APIs it
 * implements (Native_handle_sender, Native_handle_receiver, Blob_sender, Blob_receiver) based on lessons
 * learned in writing this impl.  (Yes, it "should" be the other way around -- concept should be maximally
 * useful, impl should fulfill it, period -- but I am just being real.)
 *
 * That said: at some point during the initial development of Flow-IPC, the `sync_io` pattern emerged as a
 * necessary alternative API -- alternative to the async-I/O pattern of `*_sender` and `*_receiver` concepts.
 * (See util::sync_io doc header.  It describes the pattern and the rationale for it.)  It was of course possible,
 * at that point, to have the 2 be decoupled: Native_socket_stream (async-I/O pattern) is implemented like so,
 * and sync_io::Native_socket_stream (`sync_io` pattern) is implemented separate like *so*.  Historically speaking,
 * Native_socket_stream was already written and working great, with a stable API and insides;
 * but sync_io::Native_socket_stream didn't exist and needed to be written -- in fact, the 1st `sync_io` guy to
 * be written.
 *
 * Naturally I scrapped the idea of simply writing the new guy from scratch and leaving existing guy as-is.
 * Instead I moved the relevant aspects of the old guy into the new guy, and (re)wrote the formerly-old guy
 * (that's the present class Native_socket_stream::Impl) *in terms of* the new guy.  (I then repeated this for
 * its peers Blob_stream_mq_sender -- which is like `*this` but for blobs-only and outgoing-only -- and
 * Blob_stream_mq_receiver, which is (same) but incoming-only.)  Any other approach would have resulted in 2x
 * the code and 2x the bug potential and maintenance going forward.  (Plus it allowed for the nifty
 * concept of being able to construct a Native_socket_stream from a sync_io::Native_socket_stream "core";
 * this was immediately standardized to all the relevant concepts.) Note -- however -- that as Native_socket_stream
 * underwent this bifurcation into Native_socket_stream and sync_io::Native_socket_stream, its API did not change
 * *at all* (modulo the addition of the `sync_io`-core-subsuming constructor).
 *
 * As a result -- whether that was a good or bad design (I say, good) -- at least the following is true:
 * Understanding the impl of `*this` class means: understanding the `sync_io` core sync_io::Native_socket_stream
 * *as a black box* (!); and then understanding the (actually quite limited) work necessary to build on top of that
 * to provide the desired API (as largedly mandated by Native_handle_sender and Native_handle_receiver concept
 * APIs).  Moreover, given a `sync_op::_*sender` the PEER-state logic for `*_sender` is always the same;
 * and similarly for `*_receiver`.  Because of that I factored out this adapter logic into
 * sync_io::Async_adapter_sender and sync_io::Async_adapter_receiver; so 49.5% of Native_socket_stream PEER-state
 * logic forwards to `Async_adapter_sender`, 49.5% to `Async_adapter_receiver`; and accordingly
 * 99% of each of `Blob_stream_mq_sender/receiver` forwards to `Async_adapter_sender/receiver`.  If more
 * sender/receiver types were to be written they could do the same.  So only the `sync_io::X` core is different.
 *
 * So... actually... it is pretty straightforward now!  Pre-decoupling it was much more of a challenge.
 * Of course, you may want to change its insides; in that case you might need to get into
 * sync_io::Native_socket_stream::Impl.  Though, that guy is simpler too, as its mission is much reduced --
 * no more "thread W" or async-handlers to invoke.
 *
 * That said you'll need to in fact understand how sync_io::Native_socket_stream (as a black box) works which means
 * getting comfortable with the `sync_io` pattern.  (See util::sync_io doc header.)
 *
 * ### Threads and thread nomenclature ###
 * *Thread U* refers to the user "meta-thread" from which the public APIs are invoked after construction.
 * Since ("Thread safety" in Native_socket_stream doc header) they are to protect from any concurrent API
 * calls on a given `*this`, all access to the APIs can be thought of as coming from a single "thread" U.  There is a
 * caveat: User-supplied completion handler callbacks are explicitly said to be invoked from an unspecified
 * thread that is *not* U, and the user is generally explicitly allowed to call public APIs directly from these
 * callbacks.  *Thread W* (see below) is the "unspecified thread that is not U" -- the worker thread.  Therefore, at
 * the top of each public API body, we write the internal comment "We are in thread U *or* thread W (but always
 * serially)." or similar.
 *
 * Generally speaking the threads are used as follows internally by `*this`:
 *   - User invokes `this->f()` in thread U.  We ask, immediately if possible, for `m_sync_io` (the `sync_io` core)
 *     to finish `m_sync_io.f()` as synchronously as it can.  If it can, wonderful.  If it cannot, it will
 *     (per `sync_io` pattern) ask to async-wait on an event (readable or writable) on some FD of its choice.
 *     We `.async_wait(F)` on it -- and as a result, on ready event, `F()` executes in thread W.
 *     - In thread W, `F()` eventually executes, so *within* `F()` we (1) tell `m_sync_io` of the ready event
 *       by calling into an `m_sync_io` API.
 *   - Into thread W, we `post()` user completion handlers, namely from `async_receive_*()` and async_end_sending().
 *     As promised that's the "unspecified thread" from which we do this.
 *
 * The thread-W `post()`ing does not require locking.  It does need to happen in a separate boost.asio task, as
 * otherwise recursive mayhem can occur (we want to do A-B synchronously, but then between A and B they invoke
 * `this->something()`, which starts something else => mayhem).
 *
 * However: the other 2 bullet points (first one and the sub-bullet under it) do operate on the same data
 * from threads U and W potentially concurrently.  (Note: usually it is not necessary to engage thread W,
 * except for receive-ops.  A receive-op may indeed commonly encounter would-block, simply because no data have
 * arrived yet.  Otherwise: For send-ops, `m_sync_io` will synchronously succeed in each send, including that of
 * graceful-close due to `*end_sending()`, unless the kernel buffer is full and gives would-block -- only if the
 * opposing side is not reading in timely fashion.  The sparing use of multi-threading, instead completing stuff
 * synchronously whenever humanly possible, is a good property of this design that uses a `sync_io` core.)
 *
 * That brings us to:
 *
 * ### Locking ###
 * All receive logic is bracketed by a lock, sync_io::Async_adapter_receiver::m_mutex.
 * Lock contention occurs only if more `async_receive_*()` requests are made while one is in progress.
 * This is, we guess, relatively rare.
 *
 * All send logic is similarly bracketed by a different lock, sync_io::Async_adapter_sender::m_mutex.  Lock
 * contention occurs only if (1) `send_*()` (or, one supposes, `*end_sending()`, but that's negligible) encounters
 * would-block inside `m_sync_io.send_*()`; *and* (2) more `send_*()` (+ `*end_sending()`) requests are made
 * during this state.  (1) should be infrequent-to-non-existent assuming civilized opposing-side receiving behavior.
 * Therefore lock contention should be rare.
 *
 * ### Outgoing-direction impl design ###
 * @see sync_io::Async_adapter_sender where all that is encapsulated.
 *
 * ### Incoming-direction impl design ###
 * @see sync_io::Async_adapter_receiver where all that is encapsulated.
 */
class Native_socket_stream::Impl :
  public flow::log::Log_context,
  private boost::noncopyable // And not movable.
{
public:
  // Constructors/destructor.

  /**
   * See Native_socket_stream counterpart.
   *
   * @param logger_ptr
   *        See Native_socket_stream counterpart.
   * @param nickname_str
   *        See Native_socket_stream counterpart.
   */
  explicit Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param logger_ptr
   *        See Native_socket_stream counterpart.
   * @param native_peer_socket_moved
   *        See Native_socket_stream counterpart.
   * @param nickname_str
   *        See Native_socket_stream counterpart.
   */
  explicit Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                Native_handle&& native_peer_socket_moved);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See Native_socket_stream counterpart.
   */
  explicit Impl(sync_io::Native_socket_stream&& sync_io_core_in_peer_state_moved);

  /**
   * See Native_socket_stream counterpart.
   *
   * @internal
   * The unspecified thread from which remaining handlers are potentially invoked, as of this writing,
   * is not the usual thread W.  There is rationale discussion and detail in the body, but it seemed prudent to
   * point it out here.
   * @endinternal
   */
  ~Impl();

  // Methods.

#if 0
  /** XXX
   * In PEER state only, with no prior send or receive ops, returns a sync_io::Native_socket_stream core
   * (as-if just constructed) operating on `*this` underlying low-level transport `Native_handle`; while
   * `*this` becomes as-if default-cted.
   *
   * This can be useful if one desires a `sync_io` core -- e.g., to bundle into a Channel to then feed to
   * a struc::Channel ctor -- after a successful async-I/O-style async_connect() advanced `*this`
   * from NULL to CONNECTING to PEER state.
   *
   * In a sense it's the reverse of `*this` `sync_io`-core-adopting ctor.
   *
   * Behavior is undefined if `*this` is not in PEER state, or if it is, but you've invoked `async_receive_*()`,
   * `send_*()`, `*end_sending()`, auto_ping(), or idle_timer_run() in the past.  Please be careful.
   *
   * @return See above.
   */
  Sync_io_obj release();

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  sync_io::Native_socket_stream release();
#endif

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  const std::string& nickname() const;

  /**XXX
   * See Native_socket_stream counterpart.
   *
   * @param absolute_name
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool sync_connect(const Shared_name& absolute_name, Error_code* err_code);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param err_code
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code) const;

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  size_t send_blob_max_size() const;

  /**
   * See Native_socket_stream counterpart.
   *
   * @param hndl_or_null
   *        See Native_socket_stream counterpart.
   * @param meta_blob
   *        See Native_socket_stream counterpart.
   * @param err_code
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob, Error_code* err_code);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param blob
   *        See Native_socket_stream counterpart.
   * @param err_code
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_end_sending(flow::async::Task_asio_err&& on_done_func);

  /// See Native_socket_stream counterpart.  @return Ditto.
  bool end_sending();

  /**
   * See Native_socket_stream counterpart.
   *
   * @param period
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool auto_ping(util::Fine_duration period);

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  size_t receive_blob_max_size() const;

  /**
   * See Native_socket_stream counterpart.
   *
   * @param target_hndl
   *        See Native_socket_stream counterpart.
   * @param target_meta_blob
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_receive_native_handle(Native_handle* target_hndl,
                                   const util::Blob_mutable& target_meta_blob,
                                   flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param target_blob
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param timeout
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool idle_timer_run(util::Fine_duration timeout);

private:
  // Constructors.

  /**
   * Core delegated-to ctor: does everything except `m_sync_io.start_*_ops()`.
   * Delegating ctor shall do nothing further if starting in NULL state;
   * else (starting directly in PEER state) it will delegate `m_sync_io.start_receive/send_*_ops()`
   * to #m_snd_sync_io_adapter and #m_rcv_sync_io_adapter which it will initialize.
   *
   * Therefore if entering PEER state due to successful sync_connect(), #m_snd_sync_io_adapter and
   * #m_rcv_sync_io_adapter will be created at that time.
   *
   * `get_logger()` and nickname() values are obtained from `sync_io_core_moved`.
   *
   * @param sync_io_core_moved
   *        A freshly constructed sync_io::Native_socket_stream subsumed by `*this` (this object
   *        becomes as-if-default-cted).  It can be in NULL or PEER state.
   * @param tag
   *        Ctor-selecting tag.
   */
  explicit Impl(sync_io::Native_socket_stream&& sync_io_core_moved, std::nullptr_t tag);

  // Data.

  // General data (both-direction pipes, not connect-ops-related).

  /**
   * Single-thread worker pool for all internal async work in both directions.  Referred to as thread W in comments.
   * See doc header impl section for discussion.
   *
   * Ordering: Must be either declared after mutex(es), or `.stop()`ed explicitly in dtor: Thread must be joined,
   * before mutex possibly-locked-in-it destructs.
   *
   * Why is it wrapped in `unique_ptr`?  As of this writing the only reason is release() needs to be able to
   * `move()` it to a temporary stack object before destroying it outright.
   */
  boost::movelib::unique_ptr<flow::async::Single_thread_task_loop> m_worker;

  /**
   * The core `Native_socket_stream` engine, implementing the `sync_io` pattern (see util::sync_io doc header).
   * See our class doc header for overview of how we use it (the aforementioned `sync_io` doc header talks about
   * the `sync_io` pattern generally).
   *
   * Thus, #m_sync_io is the synchronous engine that we use to perform our work in our asynchronous boost.asio
   * loop running in thread W (#m_worker) while collaborating with user thread(s) a/k/a thread U.
   * (Recall that the user may choose to set up their own event loop/thread(s) --
   * boost.asio-based or otherwise -- and use their own equivalent of an #m_sync_io instead.)
   *
   * ### Order subtlety versus `m_worker` ###
   * When constructing #m_sync_io, we need the `Task_engine` from #m_worker.  On the other hand tasks operating
   * in #m_worker access #m_sync_io.  So in destructor it is important to `m_worker.stop()` explicitly, so that
   * the latter is no longer a factor.  Then when automatic destruction occurs in the opposite order of
   * creation, the fact that #m_sync_io is destroyed before #m_worker has no bad effect.
   */
  sync_io::Native_socket_stream m_sync_io;

  // Send-ops data.

  /**
   * Null until PEER state, this handles ~all send-ops logic in that state.  sync_io::Async_adapter_sender adapts
   * any sync_io::Native_handle_sender and makes available ~all necessary async-I/O Native_handle_sender APIs.
   * So in PEER state we forward ~everything to this guy.
   *
   * ### Creation ###
   * By its contract, this guy's ctor will handle what it needs to, as long as #m_worker (to which it stores a pointer)
   * has been `.start()`ed by that time, and #m_sync_io (to which it stores... ditto) has been
   * `.replace_event_wait_handles()`ed as required.
   *
   * ### Destruction ###
   * By its contract, this guy's dtor will handle what it needs to, as long as #m_worker (to which it stores a pointer)
   * has been `.stop()`ed by that time, and any queued-up (ready to execute) handlers on it have been
   * `Task_enginer::poll()`ed-through by that time as well.
   */
  std::optional<sync_io::Async_adapter_sender<decltype(m_sync_io)>> m_snd_sync_io_adapter;

  // Receive-ops data.

  /**
   * Null until PEER state, this handles ~all receive-ops logic in that state.  sync_io::Async_adapter_sender adapts
   * any sync_io::Native_handle_receiver and makes available ~all necessary async-I/O Native_handle_receiver APIs.
   * So in PEER state we forward ~everything to this guy.
   *
   * ### Creation ###
   * See #m_snd_sync_io_adapter -- same stuff.
   *
   * ### Destruction ###
   * See #m_snd_sync_io_adapter -- same stuff.
   */
  std::optional<sync_io::Async_adapter_receiver<decltype(m_sync_io)>> m_rcv_sync_io_adapter;
}; // class Native_socket_stream::Impl

// Free functions.

/**
 * Prints string representation of the given Native_socket_stream::Impl to the given `ostream`.
 *
 * @relatesalso Native_socket_stream::Impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream::Impl& val);

} // namespace ipc::transport
