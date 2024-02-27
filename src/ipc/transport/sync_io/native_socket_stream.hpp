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
#include "ipc/util/shared_name.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include <flow/log/log.hpp>
#include <flow/async/util.hpp>
#include <experimental/propagate_const>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Implements both sync_io::Native_handle_sender and sync_io::Native_handle_receiver concepts by using
 * a stream-oriented Unix domain socket, allowing high-performance but non-zero-copy transmission of
 * discrete messages, each containing a native handle, a binary blob, or both.  This is the `sync_io`-pattern
 * counterpart to transport::Native_socket_stream -- and in fact the latter use an instance of the present
 * class as its core.
 *
 * @see transport::Native_socket_stream and util::sync_io doc headers.  The latter describes the general pattern which
 *      we implement here; it also contrasts it with the async-I/O pattern, which the former implements.
 *      In general we recommend you use a transport::Native_socket_stream rather than a `*this` --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * ### Quick note on naming ###
 * Notes for transport::Native_socket_stream apply.
 *
 * ### sync_io::Blob_sender and sync_io::Blob_receiver concept compatibility ###
 * Notes for transport::Native_socket_stream apply analogously.
 *
 * ### Informal comparison to other core transport mechanisms ###
 * Notes for transport::Native_socket_stream apply.
 *
 * ### Cleanup ###
 * Notes for transport::Native_socket_stream apply.
 *
 * ### How to use ###
 * Notes for transport::Native_socket_stream apply.  The differences (some of which are quite important) are as
 * follows.
 *
 * XXX Before using async_connect(), one must set up the "connect" API according to the `sync_io` pattern.  Namely
 * one must use start_connect_ops() successfully.  See its doc header; but generally it is another instance of
 * a `sync_io` pattern with which one must be familiar before using this class (and `sync_io::` senders and
 * receivers generally).
 *
 * Similarly -- as described by the concepts being implemented -- one must use `start_send_*_ops()` before
 * the send API (`send_*()`, `*end_sending()`, auto_ping()) and/or `start_receive_*_ops()` before
 * the receive API (`async_receive_*()`, idle_timer_run()).
 *
 * Before `start_*_ops()`, it may be required to call replace_event_wait_handles() (depending on your use case).
 *
 * Regarding ctors: Naturally the transport::Native_socket_stream `sync_io`-core-adopting ctor does not exist here,
 * as we *are* a `sync_io` core.  (Or I suppose it's just the move ctor.)
 *
 * Thread safety
 * -------------
 * Boring stuff out of the way first: It is safe to concurrently act on 2 separate objects of this type.
 * nickname() and `ostream<<` are always safe to call, and they always yield the same value (modulo
 * across move-assignment).
 *
 * Now as to invoking operation X concurrently with operation Y on the same `*this`, where at least X is non-`const`:
 *
 * Firstly let us define operation: Unlike with most APIs in the library, operations don't merely comprise methods
 * (or related free functions).  Rather, in addition, invoking
 * util::sync_io::Event_wait_func `(*on_active_ev_func)()` -- to inform `*this` of an active
 * event due to an earlier async-wait requested by `*this` -- is formally an operation on `*this`.  It can be
 * thought of as a member of its (non-`const`) API.  For the below discussion we shall pretend these methods
 * actually exist, to simplify discussion of these operations:
 *   - XXX `connect_on_active_ev()` (`on_active_ev_func` originating from start_connect_ops()).
 *     - Recall this may synchronously trigger async_connect()-passed completion handler.
 *   - `send_on_active_ev()` (`on_active_ev_func` originating from `start_send_*_ops()`).
 *     - Recall this may synchronously trigger async_end_sending()-passed completion handler.
 *   - `receive_on_active_ev()` (`on_active_ev_func` originating from `start_receive_*_ops()`).
 *     - Recall this may synchronously trigger `async_receive_*()`-passed completion handler.
 *
 * Objects of most types simply declare it to be unsafe to invoke (on one `*this`) non-`const` operation X
 * concurrently with operation Y (whether X or Y are the same op or differ).  `Native_socket_stream`, however,
 * works as follows: By *default* that is indeed the rule...
 * with the exception of the following specific exceptions, wherein it *is* **intentionally** safe.
 *
 * ### In PEER state ###
 * Firstly, let's assume `*this` is in PEER state, which is achieved either by using the PEER-state ctor form
 * (where a pre-connected `Native_handle` is subsumed), or else by successfully completing XXX async_connect() -- that is,
 * once it has invoked `on_done_func()` (the completion handler specified by user).  Cool?  Cool.  We are in PEER
 * state.  Then:
 *
 * Boring ones first: XXX async_connect() simply returns `false` and is always safe to call (it is meant for NULL state).
 * `*_max_size()` always return the same respective constant values and are always safe to call.
 *
 * Much more significantly, we now list two specific categories of operations:
 *   - Send-ops:
 *     - `send_*()`, end_sending(), async_end_sending(), auto_ping();
 *        and `send_on_active_ev()` (reminder: not a real method but a real op/see definition above).
 *   - Receive-ops:
 *     - `async_receive_*()`, `idle_timer_run()`;
 *        and `receive_on_active_ev()` (reminder: not a real method but a real op/see definition above).
 *
 * Now then: It is safe to invoke (even on the same `*this`) any 1 operation from the "send-ops" list concurrently
 * with any 1 operation from the "receive-ops" list.
 *
 * Formally that's simply the case.
 *
 * Informally: it may be highly significant to performance of the user code
 * that this is the case.  It means that the two mutually-opposing pipes can operate concurrently, despite the
 * fact they're operating on the same socket.  E.g., an upload and download being highly active simultaneously
 * will proceed in parallel on separate processor cores if possible.  If your event loop is single-threaded in
 * any case, then this does not matter; but if 2+ threads are involved, then it may well matter quite a bit.
 * (For example: non-`sync_io` transport::Native_socket_stream is internally built on a sync_io::Native_socket_stream.
 * It starts a thread (internally dubbed thread W) in which to perform significant incoming-direction work, while most
 * -- but not all -- outgoing-direction work is done synchronously from its user's calling thread (dubbed thread U).
 * Therefore it can keep 2 separate mutexes (one for each direction) and lock only 1 when doing in-work
 * (in either thread U/W); and lock the other when doing out-work, in either thread U/W.  So if a send can
 * complete synchronously in thread U, while a receive does stuff in the background in thread W, the two
 * may execute concurrently as opposed to serially.  Running concurrently would decrease latency latency in
 * sending and/or receipt.)
 *
 * ### In NULL state ###
 * Now let's assume `*this` is in NULL state; meaning it has been cted using the NULL-state ctor; and either
 * XXX async_connect() has not been invoked at all, or it has but has indicated (via completion handler as explained
 * earlier) failure to connect.  Then:
 *
 * Boring ones first: All the send-ops and receive-ops listed above (with the exception of `*_on_active_ev()`,
 * which cannot be called at all until PEER state -- you'll have no such function to call) simply return `false`
 * and are always safe to call (they are meant for PEER state).  `*_max_size()` similarly will all return zero
 * and are always safe to call.
 *
 * That leaves only async_connect() and `connect_on_active_ev()` (again, not a real method/see definition above).
 * Formally we don't need to say this, but to be clear: the two may not be called concurrently to each other
 * or themselves.  However, to restate: The "boring" ones in the previous paragraph can be called safely, period
 * (they'll just be useless, as they only make sense in PEER state).
 *
 * ### What about CONNECTING state? ### XXX
 * CONNECTING state refers to the no-man's-land between NULL and PEER: You've called `async_connect(F)`, `F()`
 * being the completion handler, and `F(err_code_or_success)` has not yet been invoked to indicate the result.
 *
 * Formally we need not say anything additional to the text above that outlines the thread-safety rules.
 * For convenience/clarity though: It is not safe to invoke operations (other than nickname() and
 * `ostream` output) concurrently during CONNECTING state, if one of them is
 * non-`const`.  Moreover, since the only way to know for sure you're not in
 * CONNECTING state at a given moment is to never have called async_connect(), or for it to have triggered
 * the completion handler: Do not do potentially concurrent stuff while an async_connect() is outstanding.
 * It is not safe.
 *
 * XXX This rule should be easy to follow -- e.g., who would want to try a send_blob() until they're for-sure connected
 * (a/k/a in PEER state)?  There is however one trip-up point which may not be quite obvious:
 * the otherwise-innocuous `*_max_size()` accessors.  Don't call them during an oustanding async_connect()
 * (by which, again, we mean one whose completion handler has not yet been invoked); or, if you possibly do,
 * then ensure non-concurrency against async_connect() and `connect_on_active_ev()`, when state would switch
 * between NULL=>CONNECTING=>NULL/PEER.  They'll return 0 in NULL and CONNECTING states versus non-zero real
 * values in PEER state; but during a transition behavior is undefined.
 * While it is possible to use a mutex to guard against this, it may not be worth the trouble.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Notes for transport::Native_socket_stream apply: the pImpl stuff; and the fact that:
 *
 * The rest of the implementation is inside sync_io::Native_socket_stream::Impl and is discussed in that class's
 * doc header.
 *
 * @see sync_io::Native_socket_stream::Impl doc header.
 *
 * @endinternal
 *
 * @see sync_io::Native_handle_sender: implemented concept.
 * @see sync_io::Native_handle_receiver: implemented concept.
 * @see sync_io::Blob_sender: alternatively implemented concept.
 * @see sync_io::Blob_receiver: alternatively implemented concept.
 */
class Native_socket_stream
{
public:
  // Types.

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::Native_socket_stream;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  // Constants.

  /// Implements concept API.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /**
   * Implements concept API; namely it is `true`.  Notes for transport::Native_socket_stream apply.
   *
   * @see Native_handle_receiver::S_META_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in transport::Native_handle_receiver doc header.
   */
  static constexpr bool S_META_BLOB_UNDERFLOW_ALLOWED = true;

  /**
   * Implements concept API; namely it is `true`.  Notes for transport::Native_socket_stream apply.
   *
   * @see Native_handle_receiver::S_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in transport::Native_handle_receiver doc header.
   */
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = true;

  /**
   * Useful for generic programming: `true` to indicate a `*this` has a send_native_handle()
   * and an async_receive_native_handle().
   */
  static constexpr bool S_TRANSMIT_NATIVE_HANDLES = true;

  /**
   * The maximum length of a blob that can be sent by this protocol.
   * send_native_handle() shall synchronously emit a particular error, if `meta_blob.size()` exceeds this.
   * send_blob() shall do similarly for `blob` arg.  The same is accurate of
   * their async-I/O-pattern transport::Native_socket_stream counterparts.
   *
   * @internal
   * Why is it a reference?  Answer: To provide for strict pImpl adherence without exploding due to
   * indeterminate order of static initializations across translation units (.cpp files).
   */
  static const size_t& S_MAX_META_BLOB_LENGTH;

  // Constructors/destructor.

  /**
   * Default ctor (object is in NULL state).  Notes for transport::Native_socket_stream apply.
   *
   * @see Native_handle_sender::Native_handle_sender(): implemented concept.
   * @see Native_handle_receiver::Native_handle_receiver(): implemented concept.
   * @see Blob_sender::Blob_sender(): implemented concept.
   * @see Blob_receiver::Blob_receiver(): implemented concept.
   */
  Native_socket_stream();

  /**
   * Creates a Native_socket_stream in NULL (not connected) state.
   * Notes for transport::Native_socket_stream apply.
   *
   * @param logger_ptr
   *        See above.
   * @param nickname_str
   *        See above.
   */
  explicit Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str);

  /**
   * Constructs the socket-and-meta-blob stream by taking over an already-connected native Unix domain socket handle.
   * Notes for transport::Native_socket_stream apply.
   *
   * @param logger_ptr
   *        See above.
   * @param native_peer_socket_moved
   *        See above.
   * @param nickname_str
   *        See above.
   */
  explicit Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                Native_handle&& native_peer_socket_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Notes for transport::Native_socket_stream apply.
   *
   * @param src
   *        See above.
   *
   * @see Native_handle_sender::Native_handle_sender(): implemented concept.
   * @see Native_handle_receiver::Native_handle_receiver(): implemented concept.
   * @see Blob_sender::Blob_sender(): implemented concept.
   * @see Blob_receiver::Blob_receiver(): implemented concept.
   */
  Native_socket_stream(Native_socket_stream&& src);

  /// Copy construction is disallowed.
  Native_socket_stream(const Native_socket_stream&) = delete;

  /**
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   *
   * @see Native_handle_sender::~Native_handle_sender(): implemented concept.
   * @see Native_handle_receiver::~Native_handle_receiver(): implemented concept.
   * @see Blob_sender::~Blob_sender(): alternatively implemented concept.
   * @see Blob_receiver::~Blob_receiver(): alternatively implemented concept.
   */
  ~Native_socket_stream();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * Notes for transport::Native_socket_stream apply.
   *
   * @param src
   *        See above.
   * @return `*this`.
   *
   * @see Native_handle_sender move assignment: implemented concept.
   * @see Native_handle_receiver move assignment: implemented concept.
   * @see Blob_sender move assignment: implemented concept.
   * @see Blob_receiver move assignment: implemented concept.
   */
  Native_socket_stream& operator=(Native_socket_stream&& src);

  /// Copy assignment is disallowed.
  Native_socket_stream& operator=(const Native_socket_stream&) = delete;

#if 0 // XXX
  /**
   * In PEER state only, with no prior send or receive ops, returns an object of this same type
   * (as-if just constructed) operating on `*this` underlying low-level transport `Native_handle`; while
   * `*this` becomes as-if default-cted.  It is similar to returning `Native_socket_stream(std::move(*this))`,
   * except that any replace_event_wait_handles() and `start_*_ops()` -- generally irreversible publicly
   * otherwise -- are as-if undone on the returned object.
   *
   * Rationale: To be perfectly honest this was originally written in order to allow for
   * async-I/O-pattern transport::Native_socket_stream::release() to be writable.
   *
   * Behavior is undefined if `*this` is not in PEER state, or if it is, but you've invoked `async_receive_*()`,
   * `send_*()`, `*end_sending()`, auto_ping(), or idle_timer_run() in the past.  (`start_*_ops()` and
   * replace_event_wait_handles() are fine.)  Please be careful.
   *
   * @return See above.
   */
  Native_socket_stream release();
#endif

  /**
   * Returns nickname, a brief string suitable for logging.  Notes for transport::Native_socket_stream apply.
   *
   * @return See above.
   */
  const std::string& nickname() const;

  /**
   * Returns logger (possibly null).
   * @return See above.
   */
  flow::log::Logger* get_logger() const;

  /**
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   *
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   *
   * @see Native_handle_sender::replace_event_wait_handles(): implemented concept.
   * @see Native_handle_receiver::replace_event_wait_handles(): implemented concept.
   * @see Blob_sender::replace_event_wait_handles(): alternatively implemented concept.
   * @see Blob_receiver::replace_event_wait_handles(): alternatively implemented concept.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  // Connect-ops API.

  /** XXX
   * See #Async_io_obj counterpart for the essential semantics; however here according to `sync_io` pattern
   * the operation may complete synchronously thus emitting result immediately and ignoring `on_done_func`.
   *
   * Per `sync_io` pattern: if internally more work is required asynchronously pending 1+ native handles being
   * in 1+ active-event (readable, writable) state, this method shall later invoke the `Event_wait_func`
   * registered via start_connect_ops() by the user of `*this`; and the error code
   * error::Code::S_SYNC_IO_WOULD_BLOCK shall be emitted here synchronously (via `*sync_err_code` if not null,
   * exception if null -- per standard `flow::Error_code`-doc-header semantics).  Meanwhile the completion handler
   * `on_done_func()` shall execute once the required async-waits have been satisfied
   * by the `*this` user, synchronously from inside the `(*on_active_ev_func)()` call that achieves this state.
   *
   * If, by contrast, no more work is required -- the operation completed synchronously within this method -- then:
   * success or error *other than* error::code::S_SYNC_IO_WOULD_BLOB shall be emitted (again per standard
   * semantics) synchronously; and `on_done_func()` shall not be
   * saved nor ever executed by `*this`.  Thus the result of the operation shall be either output directly
   * synchronously -- if op completed synchronously -- or later via `on_done_func()` completion handler.
   *
   * @param absolute_name
   *        See above.
   * @param sync_err_code
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   *
   */
  bool sync_connect(const Shared_name& absolute_name, Error_code* err_code = 0);

  // Send-ops API.

  /**
   * Implements Native_handle_sender API per contract.  Notes for transport::Native_handle_sender apply.
   *
   * @return See above.
   *
   * @see Native_handle_sender::send_meta_blob_max_size(): implemented concept.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * Implements Blob_sender API per contract.  Notes for transport::Blob_sender apply.
   *
   * @return See above.
   *
   * @see Blob_sender::send_blob_max_size(): implemented concept.
   */
  size_t send_blob_max_size() const;

  /**
   * Implements Native_handle_sender API per contract.  See also start_send_blob_ops().
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.  In addition return `false`/WARNING/no-op, if start_send_blob_ops() earlier succeeded.
   *
   * @see Native_handle_sender::start_send_native_handle_ops(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Blob_sender API per contract.  In this implementation start_send_native_handle_ops()
   * and start_send_blob_ops() are interchangeable: calling either one gets the job done, and calling the other
   * subsequently is harmless but would return `false` and no-op/log WARNING.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.  In addition return `false`/WARNING/no-op, if start_send_native_handle_ops() earlier succeeded.
   *
   * @see Blob_sender::start_send_blob_ops(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_send_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Native_handle_sender API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * @param hndl_or_null
   *        See above.
   * @param meta_blob
   *        See above.
   * @param err_code
   *        See above.  Reminder: In rare circumstances, an error emitted here may represent something
   *        detected during handling of a *preceding* send_native_handle() call but after it returned.
   *        #Error_code generated: See #Async_io_obj counterpart doc header.
   * @return See above.
   *
   * @see Native_handle_sender::send_native_handle(): implemented concept.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                          Error_code* err_code = 0);

  /**
   * Implements Blob_sender API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * @param blob
   *        See above.
   * @param err_code
   *        See above.  Reminder: In rare circumstances, an error emitted here may represent something
   *        detected during handling of a *preceding* send_native_handle() call but after it returned.
   *        #Error_code generated: See #Async_io_obj counterpart doc header.
   * @return See above.
   *
   * @see Blob_sender::send_blob(): implemented concept.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.
   * Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See #Async_io_obj counterpart doc header.
   *
   * Reminder: In rare circumstances, an error emitted there may represent something
   * detected during handling of a preceding send_native_handle() or send_blob() call but after it returned.
   *
   * @tparam Task_err
   *         See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return See above.  Reminder: If and only if it returns `false`, we're in NULL state, or `*end_sending()` has
   *         already been called; and `on_done_func()` will never be called, nor will an error be emitted.
   *
   * @see Native_handle_sender::async_end_sending(): implemented concept.
   * @see Blob_sender::async_end_sending(): alternatively implemented concept.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.
   *
   * @return See above.
   *
   * @see Native_handle_sender::end_sending(): implemented concept.
   * @see Blob_sender::end_sending(): alternatively implemented concept.
   */
  bool end_sending();

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.
   *
   * @param period
   *        See above.
   * @return See above.
   *
   * @see Native_handle_sender::auto_ping(): implemented concept.
   * @see Blob_sender::auto_ping(): alternatively implemented concept.
   */
  bool auto_ping(util::Fine_duration period = boost::chrono::seconds(2));

  // Receive-ops API.

  /**
   * Implements Native_handle_receiver API per contract.  Notes for transport::Native_handle_receiver apply.
   *
   * @return See above.
   *
   * @see Native_handle_receiver::receive_meta_blob_max_size(): implemented concept.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * Implements Blob_receiver API per contract.  Notes for transport::Blob_receiver apply.
   *
   * @return See above.
   *
   * @see Blob_receiver::receive_blob_max_size(): implemented concept.
   */
  size_t receive_blob_max_size() const;

  /**
   * Implements Native_handle_receiver API per contract.  See also start_receive_blob_ops().
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.  In addition return `false`/WARNING/no-op, if start_receive_blob_ops() earlier succeeded.
   *
   * @see Native_handle_receiver::receive_blob_max_size(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Blob_receiver API per contract.  In this implementation start_receive_native_handle_ops()
   * and start_receive_blob_ops() are interchangeable: calling either one gets the job done, and calling the other
   * subsequently is harmless but would return `false` and no-op/log WARNING.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.  In addition return `false`/WARNING/no-op, if start_receive_native_handle_ops() earlier
   *         succeeded.
   *
   * @see Native_handle_receiver::start_receive_blob_ops(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_receive_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Native_handle_receiver API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See `Async_io_obj::async_receive_native_handle()` doc header
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`).
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_hndl
   *        See above.
   * @param target_meta_blob
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   *
   * @see Native_handle_receiver::async_receive_native_handle(): implemented concept.
   */
  template<typename Task_err_sz>
  bool async_receive_native_handle(Native_handle* target_hndl, const util::Blob_mutable& target_meta_blob,
                                   Error_code* sync_err_code, size_t* sync_sz,
                                   Task_err_sz&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See `Async_io_obj::async_receive_blob()` doc header
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`).
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_blob
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::async_receive_blob(): implemented concept.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob, Error_code* sync_err_code, size_t* sync_sz,
                          Task_err_sz&& on_done_func);

  /**
   * Implements Native_handle_receiver, Blob_receiver API per contract.  Reminder: Please peruse "Thread safety"
   * in class doc header.
   *
   * @param timeout
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::idle_timer_run(): implemented concept.
   * @see Native_handle_receiver::idle_timer_run(): alternatively implemented concept.
   */
  bool idle_timer_run(util::Fine_duration timeout = boost::chrono::seconds(5));

  // Misc API.

  /**
   * OS-reported process credential (PID, etc.) info about the *other* connected peer's process, at the time
   * that the OS first established (via local-socket-connect or local-socket-connected-pair-generate call) that
   * opposing peer socket.  The value returned, assuming a non-error-emitting execution, shall always be the same for a
   * given `*this`.
   *
   * Informally: To avoid (though, formally, not guarantee) error::Code::S_LOW_LVL_TRANSPORT_HOSED, it is best
   * to call this immediately upon entry of `*this` to PEER state and/or before
   * invoking any other APIs.
   *
   * If invoked outside of PEER state returns `Process_credentials()` immediately
   * and otherwise does nothing.
   *
   * @return See above; or `Peer_credentials()` if invoked outside of PEER state or in case of error.
   *         The 2 eventualities can be distinguished by checking `*err_code` truthiness.  Better yet
   *         only call remote_peer_process_credentials() in PEER state, as it is otherwise conceptually meaningless.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        See #Async_io_obj counterpart doc header.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code = 0) const;

private:
  // Types.

  // Forward declare the pImpl-idiom true implementation of this class.  See native_socket_stream_impl.hpp.
  class Impl;

  /// Short-hand for `const`-respecting wrapper around Native_socket_stream::Impl for the pImpl idiom.
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Impl>>;

  // Friends.

  /// Friend of Native_socket_stream.
  friend std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);
  /// Friend of Native_socket_stream.
  friend std::ostream& operator<<(std::ostream& os, const Impl& val);

  // Methods.

  /**
   * Helper that simply returns #m_impl while guaranteeing that #m_impl is non-null upon return.  All
   * forwarding-to-#m_impl methods (including `const` ones) shall access #m_impl through this impl() method only.
   *
   * ### Design/rationale ###
   * Notes from transport::Native_socket_stream apply.
   *
   * @return Reference to #m_impl.
   */
  Impl_ptr& impl() const;

  /**
   * Template-free version of replace_event_wait_handles() as required by pImpl idiom.
   *
   * @param create_ev_wait_hndl_func
   *        See replace_event_wait_handles().
   * @return See replace_event_wait_handles().
   */
  bool replace_event_wait_handles_fwd
         (const Function<util::sync_io::Asio_waitable_native_handle ()>& create_ev_wait_hndl_func);

  /**
   * Template-free version of start_send_native_handle_ops() as required by pImpl idiom.
   *
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  bool start_send_native_handle_ops_fwd(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * Template-free version of start_send_blob_ops() as required by pImpl idiom.
   *
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  bool start_send_blob_ops_fwd(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * Template-free version of async_end_sending() as required by pImpl idiom.
   *
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  bool async_end_sending_fwd(Error_code* sync_err_code, flow::async::Task_asio_err&& on_done_func);

  /**
   * Template-free version of start_receive_native_handle_ops() as required by pImpl idiom.
   *
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  bool start_receive_native_handle_ops_fwd(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * Template-free version of start_receive_blob_ops() as required by pImpl idiom.
   *
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  bool start_receive_blob_ops_fwd(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * Template-free version of async_receive_native_handle() as required by pImpl idiom.
   *
   * @param target_hndl
   *        See above.
   * @param target_meta_blob
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  bool async_receive_native_handle_fwd(Native_handle* target_hndl, const util::Blob_mutable& target_meta_blob,
                                       Error_code* sync_err_code, size_t* sync_sz,
                                       flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * Template-free version of async_receive_blob() as required by pImpl idiom.
   *
   * @param target_blob
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  bool async_receive_blob_fwd(const util::Blob_mutable& target_blob, Error_code* sync_err_code, size_t* sync_sz,
                              flow::async::Task_asio_err_sz&& on_done_func);

  // Data.

  /**
   * The true implementation of this class.  See also our class doc header; and impl() (in particular explaining
   * why this is `mutable`).
   *
   * Do not access directly but only via impl().
   */
  mutable Impl_ptr m_impl;
}; // class Native_socket_stream

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Create_ev_wait_hndl_func>
bool Native_socket_stream::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  using util::sync_io::Asio_waitable_native_handle;

  return replace_event_wait_handles_fwd(create_ev_wait_hndl_func);
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::sync_io::Event_wait_func;

  return start_send_native_handle_ops_fwd(Event_wait_func(std::move(ev_wait_func)));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_send_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::sync_io::Event_wait_func;

  return start_send_blob_ops_fwd(Event_wait_func(std::move(ev_wait_func)));
}

template<typename Task_err>
bool Native_socket_stream::async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func)
{
  using flow::async::Task_asio_err;

  /* Perf note: In all cases, as of this writing, Impl would wrap the various handler parameterized args in
   * concrete Function<>s anyway for its own impl ease; so we change nothing by doing this higher up in the call stack
   * in this template and its siblings below. */

  return async_end_sending_fwd(sync_err_code, Task_asio_err(std::move(on_done_func)));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::sync_io::Event_wait_func;

  return start_receive_native_handle_ops_fwd(Event_wait_func(std::move(ev_wait_func)));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_receive_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::sync_io::Event_wait_func;

  return start_receive_blob_ops_fwd(Event_wait_func(std::move(ev_wait_func)));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_native_handle(Native_handle* target_hndl,
                                                       const util::Blob_mutable& target_meta_blob,
                                                       Error_code* sync_err_code, size_t* sync_sz,
                                                       Task_err_sz&& on_done_func)
{
  using flow::async::Task_asio_err_sz;

  return async_receive_native_handle_fwd(target_hndl, target_meta_blob, sync_err_code, sync_sz,
                                         Task_asio_err_sz(std::move(on_done_func)));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_blob(const util::Blob_mutable& target_blob,
                                              Error_code* sync_err_code, size_t* sync_sz, Task_err_sz&& on_done_func)
{
  using flow::async::Task_asio_err_sz;

  return async_receive_blob_fwd(target_blob, sync_err_code, sync_sz, Task_asio_err_sz(std::move(on_done_func)));
}

} // namespace ipc::transport::sync_io
