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

// Not compiled: for documentation only.  Contains concept docs as of this writing.
#ifndef IPC_DOXYGEN_ONLY
#  error "As of this writing this is a documentation-only "header" (the "source" is for humans and Doxygen only)."
#else // ifdef IPC_DOXYGEN_ONLY

namespace ipc::transport::sync_io
{

// Types.

/**
 * A documentation-only *concept* defining the behavior of an object that is the `sync_io`-pattern counterpart
 * of the async-I/O-pattern-following concept of the same name in our parent namespace: transport::Native_handle_sender.
 * This is paired with the sync_io::Native_handle_receiver concept which defines reception of such messages.
 *
 * @see transport::Native_handle_sender -- our async-I/O counterpart.
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following here.
 *
 * ### Concept contents ###
 * The concept defines the following behaviors/requirements.
 *   - The object has at least 2 states: Same notes as for transport::Native_handle_sender.
 *   - The (outgoing) transmission-of-messages methods, including transmission of graceful-close message.
 *     See their doc headers.  Same signatures as transport::Native_handle_sender methods but acting per
 *     `sync_io` pattern (synchronously).
 *   - Behavior when the destructor is invoked.  See its doc header.
 *   - All ctors and move assignment: Same notes as for transport::Native_handle_sender.  Exception: no
 *     `sync_io`-core-adopting ctor (since we *are* that core).
 *   - `sync_io`-pattern setup methods: replace_event_wait_handles(), start_send_native_handle_ops().
 *
 * The concept (intentionally) does *not* define the following behaviors:
 *   - How to create a Native_handle_sender, except the default and move ctors.
 *     Same notes as for transport::Native_handle_sender.
 *
 * @see sync_io::Native_socket_stream: as of this writing one key class that implements this concept (and also
 *      sync_io::Native_handle_receiver) -- using the Unix domain socket transport.
 *
 * ### Thread safety ###
 * For a given `*this`, it is not required to be safe for a non-`const` operation to be invoked concurrently with
 * another operation.  That's typical.  *In addition* -- and this is significant -- the following operation shall
 * count as a "non-`const` operation" (as opposed to the usual definition which is simply non-`const` methods and
 * related free functions):
 *   - `(*on_active_ev_func)()`, where `on_active_ev_func` is the so-named argument passed into an invocation of
 *     the `Event_wait_func` the user registers via start_send_native_handle_ops().
 *
 * Therefore: If you report an active event to `*this` (per `sync_io` pattern) from thread 1, and possibly
 * call methods such as `send_native_handle()` from thread 2, then you must use a mutex (or strand or ...)
 * to prevent concurrent execution.  For example: transport::Native_socket_stream, which internally operates
 * a sync_io::Native_socket_stream, uses a send-ops mutex (`transport::Native_socket_stream::Impl::m_snd_mutex`).
 *
 * ### Rationale: Why is send_native_handle() not asynchronous? ###
 * Same notes as for transport::Native_handle_sender.
 */
class Native_handle_sender
{
public:
  // Constants.

  /// Same notes as for transport::Native_handle_sender.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   * Same notes as for transport::Native_handle_sender.
   */
  Native_handle_sender();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Same notes as for transport::Native_handle_sender.
   *
   * @param src
   *        Same notes as for transport::Native_handle_sender.
   */
  Native_handle_sender(Native_handle_sender&& src);

  /// Disallow copying.
  Native_handle_sender(const Native_handle_sender&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it's
   * still active) and return resources to OS as applicable.
   */
  ~Native_handle_sender();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.  Same notes as for transport::Native_handle_sender.
   *
   * @param src
   *        Same notes as for transport::Native_handle_sender.
   * @return `*this`.
   */
  Native_handle_sender& operator=(Native_handle_sender&& src);

  /// Disallow copying.
  Native_handle_sender& operator=(const Native_handle_sender&) = delete;

  // `sync_io`-pattern API.

  /**
   * To be (optionally) invoked before any `start_*_ops()`, supplies a factory for the
   * util::sync_io::Asio_waitable_native_handle objects pointers to which shall be subsequently passed into
   * any `Event_wait_func` (as registered via `start_*_ops()`), when `*this` requires an async-wait on
   * a particular native-handle.  This is useful if the user event loop is built on boost.asio: the supplied factory
   * function can associate the handle-object with the user's `Task_engine` (or strand or ...); hence
   * when an async-wait is requested, the user can simply `hndl_of_interest->async_wait(..., F)`; `F()` will
   * be invoked directly within the user's event loop.
   *
   * (By contrast: if the user means to merely `poll(fd)` or `epoll_wait(fd)`, then they would simply do:
   * `auto fd = hndl_of_interest->native_handle().m_native_handle`.  In this case the associated execution
   * context/executor of `*hndl_of_interest` is of zero import.)
   *
   * This is a standard `sync_io`-pattern API per util::sync_io doc header.
   *
   * @tparam Create_ev_wait_hndl_func
   *         Function type matching signature `util::sync_io::Asio_waitable_native_handle F()`.
   * @param create_ev_wait_hndl_func
   *        See above.  Must return object `X` such that `X.is_open() == false` (i.e., storing no native handle/FD).
   * @return `false` -- indicating no-op, logging aside -- if and only if `start_*_ops()` has been called already.
   *         `true` indicating success.  Note that this method should succeed even in NULL state.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Sets up the `sync_io`-pattern interaction between `*this` and the user's event loop; required before
   * send_native_handle(), async_end_sending(), end_sending(), auto_ping() will work (as opposed to no-op/return
   * `false`).
   *
   * `ev_wait_func()` -- with signature matching util::sync_io::Event_wait_func -- is a key function memorized
   * by `*this`.  It shall be invoked by `*this` operations when some op cannot complete synchronously and requires
   * a certain event (readable/writable) to be active on a certain native-handle.
   *
   * @see util::sync_io::Event_wait_func doc header for useful and complete instructions on how to write an
   *      `ev_wait_func()` properly.  Doing so correctly is the crux of using the `sync_io` pattern.
   *
   * This is a standard `sync_io`-pattern API per util::sync_io doc header.
   *
   * @tparam Event_wait_func_t
   *         Function type matching util::sync_io::Event_wait_func.
   * @param ev_wait_func
   *        See above.
   * @return `false` if this (or equivalent) has already been invoked; no-op logging aside.  `true` otherwise.
   */
  template<typename Event_wait_func_t>
  bool start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  // General API (mirrors transport:: counterpart).

  /**
   * Same notes as for transport::Native_handle_sender.
   * @return See above.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * In PEER state: Synchronously, non-blockingly sends one discrete message, reliably/in-order, to the opposing peer;
   * the message consists of the provided native handle (if supplied); or the provided binary blob (if supplied);
   * or both (if both supplied).  The opposing peer's paired sync_io::Native_handle_receiver or
   * transport::Native_handle_receiver shall receive it reliably and in-order via `async_receive_native_handle()`.
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall synchronously invoke the `Event_wait_func`
   * registered via start_send_native_handle_ops() by the user of `*this`.
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).
   *
   * Providing neither a handle nor a blob results in undefined behavior (assertion may trip).
   * To *not* supply a handle, provide object with `.null() == true`.  To *not* supply a blob, provide
   * a buffer with `.size() == 0`.
   *
   * ### Blob copying behavior; synchronicity/blockingness guarantees ###
   * Same notes as for transport::Native_handle_sender.
   *
   * ### Error semantics ###
   * Same notes as for transport::Native_handle_sender.
   *
   * ### Thread safety ###
   * You may call this from any thread.  It is *not* required to be safe to call concurrently with
   * any Native_handle_sender API, including this one and the destructor, invoked on `*this`.
   *
   * @param hndl_or_null
   *        Same notes as for transport::Native_handle_sender.
   * @param meta_blob
   *        Same notes as for transport::Native_handle_sender.
   * @param err_code
   *        See above.
   * @return Same notes as for transport::Native_handle_sender.  In addition: return `false` if
   *         start_send_native_handle_ops() has not been called successfully.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob, Error_code* err_code = 0);

  /**
   * Equivalent to send_native_handle() but sends a graceful-close message instead of the usual payload; the opposing
   * peer's paired sync_io::Native_handle_receiver or transport::Native_handle_receiver shall receive it reliably
   * and in-order via `async_receive_native_handle()` in the form of
   * #Error_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE.  If invoked after already invoking
   * `*end_sending()`, the method shall no-op and return `false` and neither an exception nor truthy
   * `*err_code`.  Otherwise it shall return `true` -- but potentially emit a truthy #Error_code (if an error is
   * detected).
   *
   * Informally one should think of async_end_sending() as just another message, which is queued after
   * preceding ones; but: it is the *last* message to be queued by definition.  It's like an EOF or a TCP-FIN.
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall later invoke the `Event_wait_func`
   * registered via start_send_native_handle_ops() by the user of `*this`; and the error code
   * error::Code::S_SYNC_IO_WOULD_BLOCK shall be emitted here synchronously (via `*sync_err_code` if not null,
   * exception if null -- per standard `flow::Error_code`-doc-header semantics).  Meanwhile the completion handler
   * `on_done_func()` shall execute once the required async-waits have been satisfied
   * by the `*this` user, synchronously from inside the `(*on_active_ev_func)()` call that achieves this state.
   *
   * If, by contrast, no more work is required -- the operation completed synchronously within this method -- then:
   * success or error *other than* error::code::S_SYNC_IO_WOULD_BLOB shall be emitted (again per standard
   * semantics) synchronously, and `on_done_func()` shall not be saved nor ever executed by `*this`.
   * Thus the result of the operation shall be either output directly synchronously -- if op completed synchronously --
   * or later via `on_done_func()` completion handler.
   *
   * Informally:
   *   - emission of *not* `S_SYNC_IO_WOULD_BLOCK` is analogous to `SSL_write()` *not* returning
   *     `SSL_ERROR_WANT_*`.  The operation completed.
   *   - emission of `S_SYNC_IO_WOULD_BLOCK` is analogous to `SSL_write()` returning
   *     `SSL_ERROR_WANT_*`.  The operation requires an underlying transport to reach a certain readable/writable
   *     state before `SSL_write()` can complete.
   *
   * In addition: if `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns
   * `false` immediately instead and otherwise no-ops (logging aside).
   *
   * ### Error semantics ###
   * Same notes as for transport::Native_handle_sender.  Exception: error may be emitted synchronously.
   * Exception: error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER shall not be emitted.
   *
   * ### Thread safety ###
   * The notes for send_native_handle() apply.
   *
   * @internal
   * The semantic re. calling `*end_sending()` after already having called it and having that exclusively
   * return `false` and do nothing was a judgment call.  As of this writing there's a long-ish comment at the top of
   * of `"sync_io::Native_socket_stream::Impl::*end_sending()"`" body discussing why I (ygoldfel) went that way.
   * @endinternal
   *
   * @tparam Task_err
   *         Same notes as for transport::Native_handle_sender.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        Same notes as for transport::Native_handle_sender.  Plus see above.
   * @return Same notes as for transport::Native_handle_sender.  In addition: return `false` if
   *         start_send_native_handle_ops() has not been called successfully.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Equivalent to `async_end_sending(&E, F)` wherein `F()` does nothing, and `E` is some `Error_code` "sink"
   * ignored by the caller.
   *
   * Informally: ...Same notes as for transport::Native_handle_sender.
   *
   * @return Same as in async_end_sending().
   */
  bool end_sending();

  /**
   * In PEER state: Irreversibly enables periodic auto-pinging of opposing receiver with low-level messages that
   * are ignored except that they reset any idle timer as enabled via sync_io::Native_handle_receiver::idle_timer_run()
   * or transport::Native_handle_receiver::idle_timer_run() (or similar).  As to the behavior of auto-pings:
   * Same notes as for transport::Native_handle_sender.
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall synchronously invoke the `Event_wait_func`
   * registered via start_send_native_handle_ops() by the user of `*this`.
   *   - In *this* case the events waited-on are likely to be the periodic firing of an internal auto-ping timer.
   *     Therefore the correct auto-pinging behavior shall occur if and only if the user heeds the `sync_io` pattern:
   *     async-wait when requested, report resulting events when they occur.
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).  If auto_ping() has already been called successfuly,
   * subsequently it will return `false` and no-op (logging aside).  If `*end_sending()` has been called succesfully,
   * auto_ping() will return `false` and no-op (logging side).
   *
   * ### Behavior past `*end_sending()` ###
   * Same notes as for transport::Native_handle_sender.
   *
   * ### Thread safety ###
   * The notes for send_native_handle() apply.
   *
   * @param period
   *        Same notes as for transport::Native_handle_sender.
   * @return Same notes as for transport::Native_handle_sender.  In addition: return `false` if
   *         start_send_native_handle_ops() has not been called successfully.
   */
  bool auto_ping(util::Fine_duration period = default_value);
}; // class Native_handle_sender

/**
 * A documentation-only *concept* defining the behavior of an object that is the `sync_io`-pattern counterpart
 * of the async-I/O-pattern-following concept of the same name in our parent namespace:
 * transport::Native_handle_receiver.  This is paired with the sync_io::Native_handle_sender concept which defines
 * sending of such messages.
 *
 * ### Concept contents ###
 * The concept defines the behaviors/requirements mirroring those of sync_io::Native_handle_sender.
 * Notes from that doc header apply similarly; except that among the `sync_io`-specific method names replace
 * `send_native_handle` fragment of method names with `receive_native_handle`.
 *
 * @note The "Thread safety" section most definitely applies.  You should read it.
 *
 * @see Native_socket_stream: as of this writing one key class that implements this concept (and also
 *      sync_io::Native_handle_sender) -- using the Unix domain socket transport.
 */
class Native_handle_receiver
{
public:
  // Constants.

  /// Same notes as for transport::Native_handle_receiver.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// Same notes as for transport::Native_handle_receiver.
  static constexpr bool S_META_BLOB_UNDERFLOW_ALLOWED = value;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   * Same notes as for transport::Native_handle_receiver.
   */
  Native_handle_receiver();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Same notes as for transport::Native_handle_receiver.
   *
   * @param src
   *        Same notes as for transport::Native_handle_receiver.
   */
  Native_handle_receiver(Native_handle_receiver&& src);

  /// Disallow copying.
  Native_handle_receiver(const Native_handle_receiver&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it's
   * still active) and return resources to OS as applicable.
   */
  ~Native_handle_receiver();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.  Same notes as for transport::Native_handle_receiver.
   *
   * @param src
   *        Same notes as for transport::Native_handle_receiver.
   * @return `*this`.
   */
  Native_handle_receiver& operator=(Native_handle_receiver&& src);

  /// Disallow copying.
  Native_handle_receiver& operator=(const Native_handle_receiver&) = delete;

  // `sync_io`-pattern API.

  /**
   * Coincides with sync_io::Native_handle_sender concept counterpart.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Sets up the `sync_io`-pattern interaction between `*this` and the user's event loop; required before
   * async_receive_native_handle(), idle_timer_run() will work (as opposed to no-op/return
   * `false`).
   *
   * Otherwise the notes for sync_io::Native_handle_receiver::start_send_native_handle_ops() apply equally.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  template<typename Event_wait_func_t>
  bool start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  // General API (mirrors transport:: counterpart).

  /**
   * Mirrors sync_io::Native_handle_sender::send_meta_blob_max_size().
   * @return See above.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * In PEER state: Possibly-asynchronously awaits one discrete message -- as sent by the opposing peer via
   * Native_handle_sender::send_native_handle() or `"Native_handle_sender::*end_sending()"` -- and
   * receives it into the given target locations, reliably and in-order.  The message is, therefore, one of the
   * following:
   *   - A binary blob; a native handle; or both.  This is indicated by `on_done_func(Error_code(), N)` or
   *     the equivalent synchronous out-args (see below on that topic).
   *     The falsy code indicates success; `N <= target_meta_blob.size()` indicates the number of bytes received into
   *     `target_meta_blob.data()` (zero means no blob was sent in the message).  `*target_hndl` is set
   *     (`target_hndl->null() == true` means no handle was sent in the message).
   *   - Graceful-close.  This is indicated by `on_done_func(error::code::S_RECEIVES_FINISHED_CANNOT_RECEIVE, 0)` or
   *     the equivalent synchronous out-args (see below on that topic).
   *     Neither the target blob nor target native handle are touched.
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall later invoke the `Event_wait_func`
   * registered via start_receive_native_handle_ops() by the user of `*this`; and the error code
   * error::Code::S_SYNC_IO_WOULD_BLOCK shall be emitted here synchronously (via `*sync_err_code` if not null,
   * exception if null -- per standard `flow::Error_code`-doc-header semantics).  Meanwhile the completion handler
   * `on_done_func()` shall execute once the required async-waits have been satisfied
   * by the `*this` user, synchronously from inside the `(*on_active_ev_func)()` call that achieves this state.
   *
   * If, by contrast, no more work is required -- the operation completed synchronously within this method -- then:
   * success or error *other than* error::code::S_SYNC_IO_WOULD_BLOB shall be emitted (again per standard
   * semantics) synchronously; `*sync_sz` is set to 0 or bytes-transmitted, and `on_done_func()` shall not be
   * saved nor ever executed by `*this`.  Thus the result of the operation shall be either output directly
   * synchronously -- if op completed synchronously -- or later via `on_done_func()` completion handler.
   *
   * Informally:
   *   - emission of *not* `S_SYNC_IO_WOULD_BLOCK` is analogous to `SSL_read()` *not* returning
   *     `SSL_ERROR_WANT_*`.  The operation completed.
   *   - emission of `S_SYNC_IO_WOULD_BLOCK` is analogous to `SSL_read()` returning
   *     `SSL_ERROR_WANT_*`.  The operation requires an underlying transport to reach a certain readable/writable
   *     state before `SSL_read()` can complete.
   *
   * In addition: if `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns
   * `false` immediately instead and otherwise no-ops (logging aside).  Same if the preceding `async_receive_*()`
   * to have returned `true` has not yet executed its completion handler or synchronously completed.
   *
   * ### Error semantics ###
   * Same notes as for transport::Native_handle_receiver.  Exception: error may be emitted synchronously.
   * Exception: error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER shall not be emitted.
   *
   * @tparam Task_err_sz
   *         A functor type with signature identical to `flow::async::Task_asio_err_sz`.
   * @param target_hndl
   *        `*target_hndl` shall be set before `on_done_func()` is executed with a falsy code.
   * @param target_meta_blob
   *        `target_meta_blob.data()...` shall be written to before `on_done_func()` is executed with a falsy
   *        code, bytes numbering `N`, where `N` is passed to that callback.  `N` shall not exceed
   *        `target_meta_blob.size()`.  See "Error semantics" (there is an `Error_code` regarding
   *        overflowing the user's memory area).
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.  If null behavior undefined (assertion may trip).
   * @param on_done_func
   *        See above.
   * @return Same notes as for transport::Native_handle_receiver.  In addition: return `false` if
   *         start_receive_native_handle_ops() has not been called successfully, or if the preceding
   *         async-receive has not completed (completion handler has not executed).
   */
  template<typename Task_err_sz>
  bool async_receive_native_handle(Native_handle* target_hndl,
                                   Error_code* sync_err_code, size_t sync_sz,
                                   const util::Blob_mutable& target_meta_blob,
                                   Task_err_sz&& on_done_func);

  /**
   * In PEER state: Irreversibly enables a conceptual idle timer whose potential side effect is, once at least
   * the specified time has passed since the last received low-level traffic (or this call, whichever most
   * recently occurred), to emit the pipe-hosing error error::Code::S_RECEIVER_IDLE_TIMEOUT.  The implementation
   * shall guarantee the observer idle timeout is at least the provided value but may exceed this value for
   * internal reasons, as long as it's by a less than human-perceptible period (roughly speaking -- milliseconds,
   * not seconds).
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall synchronously invoke the `Event_wait_func`
   * registered via start_send_native_handle_ops() by the user of `*this`.
   *   - In *this* case the events waited-on are likely to be at most 1 (per PEER state) firing of an internal
   *     idle timer.
   *     - Indeed if that does occur, the `(*on_active_ev_func)()` call (by the `*this` user) that reported
   *       the timer firing shall also act as-if the currently pending async_receive_native_handle() (if any)
   *       encountered the pipe-hosing error error::Code::S_RECEIVER_IDLE_TIMEOUT.
   *       - No special code is necessary on the user's part to handle this: it will look like the
   *         async_receive_native_handle() failing with a pipe-hosing error; which any proper
   *         `on_done_func()` must handle anyway.
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).  If idle_timer_run() has already been called successfuly,
   * subsequently it will return `false` and no-op (logging aside).
   *
   * ### Important: Relationship between idle_timer_run() and async_receive_native_handle() ###
   * Notes for transport::Native_handle_receiver apply.  In short: if you use idle_timer_run(), then you'd best
   * have an async_receive_native_handle() outstanding at ~all times.
   *
   * ### Error semantics ###
   * If and only if the timeout does occur down the line, the aforementioned error will be emitted via
   * async_receive_native_handle() (or similar) handler.  It shall be treated as the reason to hose the pipe
   * (assuming it was not hosed by something else earlier).
   *
   * ### Suggested use ###
   * Notes for transport::Native_handle_receiver apply.
   *
   * @param timeout
   *        Notes for transport::Native_handle_receiver apply.
   * @return Same notes as for transport::Native_handle_receiver.  In addition: return `false` if
   *         start_receive_native_handle_ops() has not been called successfully.
   */
  bool idle_timer_run(util::Fine_duration timeout = default_value);
}; // class Native_handle_receiver

} // namespace ipc::transport::sync_io
