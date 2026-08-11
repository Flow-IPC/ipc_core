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

#include "ipc/transport/sync_io/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/sync_io/detail/native_socket_stream_impl_rcv.hpp"
#include "ipc/transport/native_socket_stream_cfg.hpp"
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
 * a Unix domain socket, allowing high-performance but non-zero-copy transmission of
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
 * As described by the concepts being implemented -- one must use `start_send_*_ops()` before
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
 * (where a pre-connected `Native_handle` is subsumed), or else by successfully completing `*_connect()`.
 * Cool?  Cool.  We are in PEER state.  Then:
 *
 * Boring ones first: sync_connect() simply returns `false` and is always safe to call (it is meant for NULL state).
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
 * Nothing interesting here.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Notes for transport::Native_socket_stream apply: the pImpl-lite stuff; and the fact that:
 *
 * The rest of the implementation is inside sync_io::Native_socket_stream_impl and is discussed in that class's
 * doc header.
 *
 * @see sync_io::Native_socket_stream_impl doc header.
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
private:
  // Types.

  /// Short-hand for the impl type we're wrapping.  Cannot simply forward-declare as in pImpl; we do pImpl-lite.
  using Impl = Native_socket_stream_impl;

public:
  // Types.

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::Native_socket_stream;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  /// Implements Native_handle_receiver concept API.
  template<typename Msg_resource>
  using Native_handle_batch_in = typename Impl::template Native_handle_batch_in<Msg_resource>;
  /// Implements Blob_receiver concept API.
  template<typename Msg_resource>
  using Blob_batch_in = typename Impl::template Blob_batch_in<Msg_resource>;

  /// Implements sync_io::Blob_sender concept API.
  using Blob_snd_stats = transport::stat::Blob_snd_stats;
  /// Implements sync_io::Native_handle_sender concept API.  Identical to #Blob_snd_stats for this impl.
  using Native_handle_snd_stats = transport::stat::Blob_snd_stats;
  /// Implements sync_io::Blob_receiver concept API.
  using Blob_rcv_stats = transport::stat::Blob_rcv_stats;
  /// Implements sync_io::Native_handle_receiver concept API.  Identical to #Blob_rcv_stats for this impl.
  using Native_handle_rcv_stats = transport::stat::Blob_rcv_stats;

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
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = S_META_BLOB_UNDERFLOW_ALLOWED;

  /**
   * Implements concept API.  As of this writing this value depends on
   * Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT and `S_USE_OS_DGRAM_SUPPORT`; iff and only if both are
   * `true`, then this value is `> 1`; otherwise it is `1`.
   *
   * @internal
   * ### Value to choose ###
   * This paragraph is a quote from the concept doc:
   * This is "only" a recommendation, though other parts of Flow-IPC may well take it to heart
   * (struc::Channel does in particular, as of this writing, when deciding whether to even use the batching code
   * path -- when this is not set to 1 -- and if so then the batch-size value is taken from here).
   *
   * It is hopefully straightforward why the conservative decision is to set this to 1, unless
   * `USE_OS_DGRAM_SUPPORT` is in effect.  As for what value (at least 2) to use, when it *is* in effect:
   *
   * In typical code out there one tends to see 32 or 64 in this context.  Larger values would not be particularly
   * wasteful, as proper full use of Msg_batch_in concept (most importantly: reuse from receive call to receive call)
   * means zero cost (other than some memory) for unused slots.  However in practice such large in-batches are rare
   * in our experience, and 64 (or 32) is a nice vanilla choice for "big batch."
   */
  static constexpr size_t S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION
    = (Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT && Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT)
        ? 64 : 1;

  /**
   * Implements concept API.
   *
   * @see Blob_receiver::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION: implemented concept.  One transport, one value:
   *      it serves both of our receiver-concept personas equally (batch-receiving mechanics do not depend on
   *      whether native handles ride along).
   */
  static constexpr size_t S_RCV_BLOB_BATCH_SZ_RECOMMENDATION = S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION;

  /**
   * Useful for generic programming: `true` to indicate a `*this` has a send_native_handle()
   * and an async_receive_native_handle().
   *
   * @todo Shouldn't Native_socket_stream::S_TRANSMIT_NATIVE_HANDLES and its peer concept impls
   * be formally generalized in the appopriate concept?  E.g., as of this writing the Async_adapter_receiver impl
   * relies on it, so trying to use that guy to implement a custom `..._receiver` in terms of a custom
   * `sync_io::..._receiver` would cause a build error, unless the writer of the latter hypothetical custom guy
   * added a `S_TRANSMIT_NATIVE_HANDLES` member.  In practice they'd probably know to do so upon seeing the error
   * and the present code; but formally a relevant concept should specify it, so the custom-guy writer would've known
   * to do so from the start.
   */
  static constexpr bool S_TRANSMIT_NATIVE_HANDLES = true;

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
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  // Connect-ops API.

  /**
   * Identical to #Async_io_obj counterpart.
   *
   * @param absolute_name
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool sync_connect(const Shared_name& absolute_name, Error_code* err_code = nullptr);

  // Send-ops API.

  /**
   * Implements Native_handle_sender API per contract.  Notes for transport::Native_handle_sender apply.
   *
   * @return See above.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * Implements Blob_sender API per contract.  Notes for transport::Blob_sender apply.
   *
   * @return See above.
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
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                          Error_code* err_code = nullptr);

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
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = nullptr);

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
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.
   *
   * @return See above.
   */
  bool end_sending();

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.
   *
   * @param period
   *        See above.
   * @return See above.
   */
  bool auto_ping(util::Fine_duration period = boost::chrono::seconds{2});

  /**
   * Implements sync_io::Blob_sender API per contract.  Notes for transport::Native_socket_stream apply.
   * @return See above.
   */
  Blob_snd_stats blob_send_stats() const;

  /// Implements sync_io::Blob_sender API per contract.
  void blob_send_stats_reset();

  /**
   * Implements sync_io::Native_handle_sender API per contract.  Identical to blob_send_stats() for this impl.
   * @return See above.
   */
  Blob_snd_stats native_handle_send_stats() const;

  /// Implements sync_io::Native_handle_sender API per contract.  Identical to blob_send_stats_reset().
  void native_handle_send_stats_reset();

  // Receive-ops API.

  /**
   * Implements Native_handle_receiver API per contract.  Notes for transport::Native_handle_receiver apply.
   *
   * @return See above.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * Implements Blob_receiver API per contract.  Notes for transport::Blob_receiver apply.
   *
   * @return See above.
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
   */
  template<typename Event_wait_func_t>
  bool start_receive_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Native_handle_receiver API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See `Async_io_obj::async_receive_native_handle()` doc header
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`; and add `S_SYNC_IO_WOULD_BLOCK`).
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
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`; and add `S_SYNC_IO_WOULD_BLOCK`).
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
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob, Error_code* sync_err_code, size_t* sync_sz,
                          Task_err_sz&& on_done_func);

  /**
   * Implements Native_handle_receiver API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See `Async_io_obj::async_receive_native_handle_batch()` doc header
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`; and add `S_SYNC_IO_WOULD_BLOCK`).
   *
   * @tparam Msg_resource
   *         See above.
   * @tparam Task_err
   *         See above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Msg_resource, typename Task_err>
  bool async_receive_native_handle_batch(Native_handle_batch_in<Msg_resource>* batch, bool assume_would_block,
                                         Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.  Reminder: Please peruse "Thread safety" in class doc header.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See `Async_io_obj::async_receive_blob_batch()` doc header
   * (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`; and add `S_SYNC_IO_WOULD_BLOCK`).
   *
   * @tparam Msg_resource
   *         See above.
   * @tparam Task_err
   *         See above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Msg_resource, typename Task_err>
  bool async_receive_blob_batch(Blob_batch_in<Msg_resource>* batch, bool assume_would_block, Error_code* sync_err_code,
                                Task_err&& on_done_func);

  /**
   * Implements Native_handle_receiver, Blob_receiver API per contract.  Reminder: Please peruse "Thread safety"
   * in class doc header.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout = boost::chrono::seconds{5});

  /**
   * Implements sync_io::Blob_receiver API per contract.
   * @return See above.
   */
  Blob_rcv_stats blob_receive_stats() const;

  /// Implements sync_io::Blob_receiver API per contract.
  void blob_receive_stats_reset();

  /**
   * Implements sync_io::Native_handle_receiver API per contract.  Identical to blob_receive_stats() for this impl.
   * @return See above.
   */
  Blob_rcv_stats native_handle_receive_stats() const;

  /// Implements sync_io::Native_handle_receiver API per contract.  Identical to blob_receive_stats_reset().
  void native_handle_receive_stats_reset();

  // Misc API.

  /**
   * See transport::Native_socket_stream counterpart.
   *
   * @param err_code
   *        See transport::Native_socket_stream counterpart.
   * @return See transport::Native_socket_stream counterpart.
   */
  const util::Process_credentials& remote_peer_process_credentials(Error_code* err_code = nullptr) const;

  /**
   * See transport::Native_socket_stream counterpart.
   *
   * @param creds
   *        See transport::Native_socket_stream counterpart.
   * @return See transport::Native_socket_stream counterpart.
   */
  bool remote_peer_process_credentials(const util::Process_credentials& creds);

private:
  // Types.

  /// Short-hand for `const`-respecting wrapper around #Impl for the pImpl-lite idiom.
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Impl>>;

  // Friends.

  /// Friend of Native_socket_stream.
  friend std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);

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

  // Please see transport::Native_socket_stream_impl similar doc header; explains why this is dead code but remains.
#if 0
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
  return impl()->replace_event_wait_handles(create_ev_wait_hndl_func);
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  return impl()->start_send_native_handle_ops(std::move(ev_wait_func));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_send_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  return impl()->start_send_blob_ops(std::move(ev_wait_func));
}

template<typename Task_err>
bool Native_socket_stream::async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func)
{
  return impl()->async_end_sending(sync_err_code, std::move(on_done_func));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  return impl()->start_receive_native_handle_ops(std::move(ev_wait_func));
}

template<typename Event_wait_func_t>
bool Native_socket_stream::start_receive_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  return impl()->start_receive_blob_ops(std::move(ev_wait_func));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_native_handle(Native_handle* target_hndl,
                                                       const util::Blob_mutable& target_meta_blob,
                                                       Error_code* sync_err_code, size_t* sync_sz,
                                                       Task_err_sz&& on_done_func)
{
  /* Perf note: In all cases, as of this writing, Native_socket_stream_impl would wrap the various handler
   * parameterized args in concrete Function<>s anyway for its own impl ease; so we change nothing by
   * (implicitly, if needed) constructing flow::async::Task_asio_err_sz higher up in the call stack in this
   * template and its siblings below. */

  return impl()->async_receive_native_handle(target_hndl, target_meta_blob, sync_err_code, sync_sz,
                                             std::move(on_done_func));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_blob(const util::Blob_mutable& target_blob,
                                              Error_code* sync_err_code, size_t* sync_sz, Task_err_sz&& on_done_func)
{
  return impl()->async_receive_blob(target_blob, sync_err_code, sync_sz, std::move(on_done_func));
}

template<typename Msg_resource, typename Task_err>
bool Native_socket_stream::async_receive_native_handle_batch(Native_handle_batch_in<Msg_resource>* batch,
                                                             bool assume_would_block,
                                                             Error_code* sync_err_code, Task_err&& on_done_func)
{
  return impl()->async_receive_native_handle_batch<Msg_resource>(batch, assume_would_block, sync_err_code,
                                                                 std::move(on_done_func));
}

template<typename Msg_resource, typename Task_err>
bool Native_socket_stream::async_receive_blob_batch(Blob_batch_in<Msg_resource>* batch, bool assume_would_block,
                                                    Error_code* sync_err_code, Task_err&& on_done_func)
{
  return impl()->async_receive_blob_batch<Msg_resource>(batch, assume_would_block, sync_err_code,
                                                        std::move(on_done_func));
}

} // namespace ipc::transport::sync_io
