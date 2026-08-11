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

#include "ipc/transport/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/util/shared_name_fwd.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/util/native_handle.hpp"
#include <flow/log/log.hpp>
#include <flow/async/util.hpp>
#include <experimental/propagate_const>

namespace ipc::transport
{

// Types.

/**
 * Implements both Native_handle_sender and Native_handle_receiver concepts by using a Unix domain
 * socket, allowing high-performance but non-zero-copy transmission of discrete messages, each containing a native
 * handle, a binary blob, or both.  This is a low-level (core) transport mechanism; higher-level (structured)
 * transport mechanisms may use Native_socket_stream to enable their work.  Native_socket_stream, as of this writing,
 * is unique in Flow-IPC, in that it is able to transmit not only blobs but also native handles.
 *
 * @see sync_io::Native_socket_stream and util::sync_io doc headers.  The latter describes a general pattern which
 *      the former implements.  In general we recommend you use a `*this` rather than a sync_io::Native_socket_stream --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * ### Quick note on naming ###
 * It appears somewhat inconsistent to name it `Native_socket_stream[_acceptor]`
 * and not `Native_handle_stream[_acceptor]`, given the implemented concept names `Native_handle_*er` and
 * a key payload type being `Native_handle`.  It's subjective and a matter of aesthetics even, of course,
 * but the reasoning is: It briefly conveys (or at least suggests) that the underlying transport
 * is the Unix domain *socket* (with messages arranged in a *stream* at that); while also suggesting that *native*
 * handles are transmissible over it (and it in fact is unique in that capability by the way).  Perhaps
 * something like `Native_handle_socket_stream[_acceptor]` would've been more accurate, but brevity is a virtue.
 *
 * @note The same reasoning applies for Socket_stream_channel and Socket_stream_channel_of_blobs with the
 *       `native_` part being left out for orthogonal aesthetic reasons.
 *
 * ### Blob_sender and Blob_receiver concept compatibility ###
 * Native_socket_stream also implements the Blob_sender and Blob_receiver concepts.  To wit:
 * You must choose whether you shall use this as a `Blob_sender/receiver` or `Native_handle_sender/receiver`,
 * and conversely the other side must do the same in matched fashion.  Doing otherwise leads to undefined behavior.
 * To use the latter concept pair simply use send_native_handle() and async_receive_native_handle() and never
 * send_blob() or async_receive_blob().  To use the former concept pair simply do the reverse.
 * On the other side do the matched thing.  All the other methods/dtor are the same regardless of concept pair chosen.
 *
 * How does this work?  Trivially: consider, say, send_native_handle() which can send a native handle, or none;
 * and a blob, or none (but at least 1 of the 2).  send_blob() can only take a blob (which must be present) and
 * internally will simply act as send_native_handle() with a null handle.  Conversely async_receive_native_handle()
 * receives a handle (or none) and blob (or none); so async_receive_blob() will receive only a blob.  The only added
 * behaviors in `Blob_sender/receiver` "mode":
 *   - send_blob() requires a blob of size 1+.  (send_native_handle() allows an empty meta-blob; i.e., no blob.)
 *   - async_receive_blob() will emit an error if a payload arrives with a native handle in it:
 *     error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB.  This would only occur if the other side were using the mismatched
 *     API.
 *
 * Further discussion might ignore the degraded `Blob_*` concepts, as they are a trivial degenerate case of
 * `Native_handle_*`.
 *
 * @see other, dedicated Blob_sender and Blob_receiver impls namely, at least, the persistent-MQ-based
 *      Blob_stream_mq_sender and Blob_stream_mq_receiver.  They may appeal due to potentially better performance
 *      and/or a creation/addressing scheme that is more appealing, depending.
 *
 * ### Informal comparison to other core transport mechanisms ###
 * Firstly, as noted, it is currently unique in that it can transmit native handles.  The rest of the discussion
 * here is about transmitting blobs (called meta-blobs, meaning they potentially accompany native handles).
 *
 * It is intended for transmission of relatively short messages -- rough guidance
 * being for max length being in the 10s-of-KiB range.  With a modern Linux kernel on server hardware from about 2014
 * to 2020, our performance tests show that its raw speed for messages of aforementioned size is comparable to
 * zero-copy mechanisms based on POSIX message queues and Boost SHM-based message queues.  Technically we found it
 * to be somewhat faster than the latter and somewhat slower than the former.  However, for best perf, it is recommended
 * to send handles to SHM areas containing arbitrarily long structured messages (such as ones [de]serialized using
 * zero-copy builders: capnp and the like).  This further reduces the importance of relative perf compared to
 * other low-level transports (which, as noted, is pretty close regardless -- though this is bound to stop being true
 * for much longer messages, if the send-SHM-handles technique were to *not* be used).
 *
 * ### Cleanup ###
 * Some core transports rely on SHM or SHM-style semantics, wherein the transport has kernel persistence, meaning
 * some or all processes exiting does not free the resources involved.  For those transports special measures must
 * be taken to ensure cleanup after both relevant processes die or no longer use a transport instance.  This is not the
 * case for Native_socket_stream: When the underlying (hidden from user) Unix domain socket handle is closed by both
 * sides, all resources are automatically cleaned up; and processes exiting always closes all such handles.
 * These matters for all transports are hidden from the user as much as possible, but these internal facts are still
 * relevant knowledge for the reader/user.
 *
 * ### How to use ###
 * Generally, this class's behavior is dictated by the 2 main concepts it implements: Native_handle_sender and
 * Native_handle_receiver.  Though internally a single mechanism is used for both (incoming and outgoing) *pipes*
 * (a term I use generically, not meaning "Unix FIFOs"), the two pipes are mostly decoupled.  Bottom line:
 * Behavior is dictated by the 2 concepts, so please see their respective doc headers first.  Next, see the doc
 * headers of the main concrete APIs which discuss any behaviors relevant to Native_socket_stream specifically:
 *   - dtor;
 *   - send_native_handle() (or send_blob()), `*end_sending()`, auto_ping();
 *   - async_receive_native_handle() (or async_receive_blob()), idle_timer_run();
 *     - async_receive_native_handle_batch() (or async_receive_blob_batch());
 *   - send_meta_blob_max_size() (or send_blob_max_size());
 *   - receive_meta_blob_max_size() (or receive_blob_max_size());
 *   - default ctor;
 *   - move ctor, move assignment.
 *
 * There is also:
 *   - remote_peer_process_credentials() which is specific to Native_socket_stream and not those concepts.
 *
 * That above list (in?)conspicuously omits the initialization API.  So how does one create this connection
 * and hence the two required `Native_socket_stream`s that are connected to each other (each in PEER state)?
 *
 * A Native_socket_stream object is always in one of 2 states:
 *   - PEER.  Upon entering this state, the peer is connected (is an actual peer).  At this stage it exactly
 *     implements the concepts Native_handle_sender, Native_handle_receiver, Blob_sender, Blob_receiver.
 *     If the pipe is hosed (via an error or graceful-close), it remains in this PEER state (but cannot do anything
 *     useful anymore).  It is not possible to exit PEER state.
 *     - The only added behavior on top of the implemented concepts is remote_peer_process_credentials() (which is not
 *       a concept API but logically only makes sense when already connected, i.e., in PEER state).
 *   - NULL.  In this state, the object is doing nothing; neither connected nor connecting.
 *
 * Therefore, to be useful, one must somehow get to PEER state in which it implements the aforementioned concepts.
 * How to do this?  Answer: There are 2* ways:
 *   - Use Native_socket_stream_acceptor on the side you've designated as the server for that connection or set of
 *     connections.  This allows to wait for a connection and eventually, on success, moves the target
 *     Native_socket_stream passed to Native_socket_stream_acceptor::async_accept() to PEER state.
 *     - On the other side, use a Native_socket_stream in NULL state and invoke sync_connect() which will move
 *       synchronously and non-blockingly to PEER state (mission accomplished).
 *       If the connect fails (also synchronously, non-blockingly), it will go back to NULL state.
 *   - Use a mechanism, as of this writing ipc::transport::Channel at least, that uses the following technique:
 *     -# A process-wide Native_socket_stream connection is established using via the client-server method in the
 *        above bullet point.
 *     -# A socket-pair-generating OS call generates 2 pre-connected native Unix domain socket handles.
 *        -# E.g., use boost.asio `asio_local_stream_socket::connect_pair()`.
 *     -# 1 of the 2 handles is passed to the other side using the process-wide connection from step 1.
 *     -# On each side, construct Native_socket_stream using the #Native_handle-taking ctor thus entering PEER
 *        state directly at construction (on each side).
 *
 * Way 2 is better, because it requires no `Shared_name`s whatsoever, hence there are no thorny naming issues.
 * The drawback is it requires an already-existing Native_socket_stream in order to transmit the handle!  Chicken/egg!
 * Therefore, the informal recommendation is -- if practical -- use way 1 once (create chicken); then subsequently use
 * way 2 as needed to easily create further connections.
 *
 * Native_socket_stream is move-constructible and move-assignable (but not copyable).  This is, at any rate,
 * necessary to work with the boost.asio-style Native_socket_stream_acceptor::async_accept() API.
 * A moved-from Native_socket_stream is as-if default-constructed; therefore it enters NULL state.
 *
 * (*) There is, in fact, another way -- one could call it way 2a.  It is just like way 2; except that -- upon
 * obtaining a pre-connected `Native_handle` (one of a pair) -- one first constructs a "`sync_io` core,"
 * namely a sync_io::Native_socket_stream object, using the *exact* same signature (which takes a `Logger*`,
 * a nickname, and the pre-connected `Native_handle`).  Then, one constructs a `*this` by `move()`ing that
 * guy into the `sync_io`-core-adopting ctor.  The result is exactly the same.  (Internally, whether one uses way 1
 * or way 2, `*this` will create a sync_io::Native_socket_stream core itself.  I tell you this, in case you are
 * curious.)
 *
 * ### Why no async-connect method, only sync-connect? ###
 * Without networking, the other side (Native_stream_socket_acceptor) either exists/is listening; or no.
 * Connecting is a synchronous, non-blocking operation; so an `async_connect()` API in this context only makes
 * life harder for the user.  (However, there are some serious plans to add a networking-capable counterpart
 * (probably via TCP at least) to Native_socket_stream; that one will almost certainly have an `async_connect()`,
 * while its `sync_connect()` will probably become potentially blocking.)
 *
 * If you are worried about attempting to connect to a Native_stream_socket_acceptor whose internal backlog limit has
 * been reached: please don't.  It won't happen; we have contemplated the details.
 *
 * ### Thread safety ###
 * We add no more thread safety guarantees than those mandated by the main concepts.  To wit:
 *   - Concurrent access to 2 separate Native_socket_stream objects is safe.
 *   - After construction, concurrent access to the main transmission API methods (or
 *     remote_peer_process_credentials()) is not safe.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Internally Native_socket_stream strictly uses the pImpl idiom (see https://en.cppreference.com/w/cpp/language/pimpl
 * for an excellent overview).  Update: Oops, that lasted a while, but with the introduction of
 * `async_receive_*_batch<Msg_resource>()` (a method template), strict pImpl (`Impl` an inner class, stable ABI,
 * and so on) is no longer possible.  The template cannot be converted sanely into a function taking a `function<>`
 * (and similar tech), unlike other templates merely using t-params for handler functors.  (It's doable, but it would
 * hurt perf unacceptably.)  So, we use what I (ygoldfel) term "pImpl-lite".  That is: it is pImpl that achieves
 * performant and easily-coded move-semantics -- in the face of fairly complex async impl details --
 * but does *not* achieve a stable ABI (the thing where one can change impl method bodies without recompiling
 * the code/changing the binary signature of the class).  Long story short-ish:
 *
 *   - The "true" Native_socket_stream is actually the self-contained, but not publicly exposed,
 *     Native_socket_stream_impl class.
 *   - #m_impl is the `unique_ptr` to `*this` object's `Native_socket_stream_impl`.  This becomes null only when
 *     Native_socket_stream is moved-from; but if one attempts to call a method (such as sync_connect()) on the
 *     moved-from `*this` #m_impl is lazily re-initialized to a new default-cted (therefore NULL-state)
 *     Native_socket_stream_impl.
 *   - Every public method of Native_socket_stream `*this` forwards to essentially the same-named method
 *     of Native_socket_stream_impl `*m_impl`.  Subtleties:
 *     - Method templates that take a t-param like `Task_err_sz` for a functor forward to a corresponding
 *       non-template method in Native_socket_stream_impl, converting (e.g.) `Task_err_sz` for concretely-typed
 *       `Function<> flow::async::Task_asio_err_sz`.  Historically this was done as a requirement of doing strict
 *       pImpl (which for aforementioned other reasons we can longer do), but since the impl needs to store these
 *       guys as `Function<>`s anyway, we left this alone; as (1) don't fix what's not broken, (2) functions are
 *       less annoying than function templates generally, and (3) the perf is identical.
 *     - For a method template (like async_receive_blob_batch()) that cannot forward to a regular method
 *       (for aforementioned reasons), we forward to a method template in Native_socket_stream_impl.
 *       Hence, Native_socket_stream_impl cannot be an incomplete type in this class body and header file; and
 *       therefore it isn't strict pImpl.  So we simply `#include` the header(s) with Native_socket_stream_impl.
 *   - Because the implementation is 100% encapsulated inside the `Impl` #m_impl, move-assignment and move-construction
 *     are acquired for "free":
 *     - Coding-ease "free": Default-generated move-ctor and move-assignment simply move the `unique_ptr` #m_impl.
 *     - Perf "free": `unique_ptr` move is lightning-quick (nullify the source pointer after copying the pointer value
 *       into the target pointer).  Though, there is an extra use of the heap per `*this`.
 *
 * Okay, if one is familiar with pImpl, none of this is surprising in terms of how it works.  *Why* though?  Answer:
 *   - I (ygoldfel) chose pImpl *not* to maintain a stable ABI/guarantee the implementation of methods can be changed
 *     without needing to recompile the code that `#include`s the present header file Native_socket_stream.hpp...
 *   - ...but rather specifically to get the move-ction and move-assignment for "free" as briefly explained above.
 *     - Consider the alternative.  Suppose Native_socket_stream is implemented directly inside Native_socket_stream
 *       itself; and we still want move-semantics available to the user.
 *     - Writing the move-ctor/assignment in and of itself is very much non-trivial.  The implementation involves
 *       many delicate data members including async task-engines and threads some of which may be related to one
 *       another.  Swapping them properly, etc. etc., is no mean feat.  Plus it's not exactly perf-y either.
 *       - Yet with pImpl-facilitated move semantics it becomes trivial.  `Impl` is non-copyable, non-movable and can
 *         rely on a stable `this`.  Move semantics become entirely the concern of the wrapper Native_socket_stream:
 *         nice and clean.
 *
 * This leaves two questions.
 *   - One: why do we want public move-semantics anyway?  Why not just provide a non-movable Native_socket_stream
 *     and let the user worry about wrapping it in a `shared_ptr` or `unique_ptr`, if they want to move these objects
 *     around?  Answer: Sure, that's perfectly reasonable.  However:
 *     - Native_socket_stream_acceptor would need to impose shared-ownership/factory API in supplying the connected
 *       Native_socket_stream to the user asynchronously.  That could be fine -- but it'd be nice to have
 *       acceptor semantics that are like boost.asio's as opposed to our own design.
 *     - Nevertheless Native_socket_stream_acceptor API could still be designed in factory fashion, even if it's
 *       inconsistent with what boost.asio users might expect.  But: now consider the other guys implementing
 *       Blob_sender and Blob_receiver, namely the message queue-based (MQ-based) Blob_stream_mq_sender and
 *       and Blob_stream_mq_receiver.  They don't use an acceptor API at all by their nature; and hence they don't
 *       need any factory semantics.  So now some Blob_sender and Blob_receiver implementing classes use ownership
 *       semantics X, while others (Native_socket_stream) use ownership semantics Y -- despite implementing the
 *       same concepts.  Is this okay?  Sure, it's not bad; but it's not ideal all in all.
 *     - Therefore for consistency inside the overall ipc::transport API, as well as versus boost.asio API,
 *       making Native_socket_stream directly constructible (and movable when needed for acceptor semantics)
 *       ultimately is the nicer approach.  (Historical note: An early version of this class and
 *       Native_socket_stream_acceptor indeed used the alternative, factory-based approach.  So we've tried both.)
 *   - Two: what about the negatives of pImpl?  It is after all a trade-off.  I won't go over all the negatives here
 *     "formally" (see the cppreference.com page mentioned above for a survey).  Long story short:
 *     - The indirection of all calls through a pointer is probably a minor perf hit in and of itself.
 *       - Factories involve their own perf hits, particularly since typically they use `shared_ptr` and not
 *         `unique_ptr` (which is quite quick in comparison).
 *     - The laborious need to code a copy of the entire public API, so as to forward to `Impl`, is *easily*
 *       no big deal compared to what it would take to make Native_socket_stream movable directly.
 *     - pImpl is not inlining-friendly and cannot be used for a header-only library.  However:
 *       - The entire Flow-IPC library -- and all of Flow too -- to begin with refuses to be inlining-friendly and
 *         header-only; so this does not change that.
 *       - Both Flow-IPC and Flow encourage the use of LTO which will inline everything anyway.
 *         If LTO is not available then the resulting perf loss already applies to both libraries; so this
 *         does not change that.  Conversely adding LTO will benefit everything including Native_socket_stream.
 *       - So we are consistent in this decision for better or worse.
 *
 * That's my (ygoldfel) story, and I am sticking to it.  I should note that the same design philosophy is followed
 * in much of Flow-IPC, `Native_socket_stream` being the first one to be coded.
 *
 * Again, though, it's no longer strict pImpl; more like just an impl class wrapped in a `unique_ptr` for easy/fast
 * move-semantics implementation.
 *
 * The rest of the implementation is inside Native_socket_stream_impl and is discussed in that class's doc header.
 *
 * @see Native_socket_stream_impl doc header.
 *
 * @endinternal
 *
 * @see Native_handle_sender: implemented concept.
 * @see Native_handle_receiver: implemented concept.
 * @see Blob_sender: alternatively implemented concept.
 * @see Blob_receiver: alternatively implemented concept.
 */
class Native_socket_stream
{
private:
  // Types.

  /// Short-hand for the impl type we're wrapping.  Cannot simply forward-declare as in pImpl; we do pImpl-lite.
  using Impl = Native_socket_stream_impl;

public:
  // Types.

  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Native_socket_stream;
  /// You may disregard.
  using Async_io_obj = Null_peer;

  /// Implements Native_handle_receiver concept API.
  template<typename Msg_resource>
  using Native_handle_batch_in = typename Impl::template Native_handle_batch_in<Msg_resource>;
  /// Implements Blob_receiver concept API.
  template<typename Msg_resource>
  using Blob_batch_in = typename Impl::template Blob_batch_in<Msg_resource>;

  /// Implements Blob_sender concept API.
  using Blob_snd_stats = transport::stat::Blob_snd_stats;
  /// Implements Native_handle_sender concept API.  Identical to #Blob_snd_stats for this impl.
  using Native_handle_snd_stats = transport::stat::Blob_snd_stats;
  /// Implements Blob_receiver concept API.
  using Blob_rcv_stats = transport::stat::Blob_rcv_stats;
  /// Implements Native_handle_receiver concept API.  Identical to #Blob_rcv_stats for this impl.
  using Native_handle_rcv_stats = transport::stat::Blob_rcv_stats;

  // Constants.

  /// Implements concept API.
  static const Shared_name& S_RESOURCE_TYPE_ID;

  /**
   * Implements concept API; namely it is `true`: async_receive_native_handle() with
   * smaller-than-`receive_meta_blob_max_size()` size shall *not* lead to error::Code::S_INVALID_ARGUMENT; hence
   * error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE is possible.
   *
   * @see Native_handle_receiver::S_META_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in that concept class's doc header for potentially important discussion.
   */
  static constexpr bool S_META_BLOB_UNDERFLOW_ALLOWED = true;

  /**
   * Implements concept API; namely it is `true`: async_receive_blob() with smaller-than-`receive_blob_max_size()`
   * size shall *not* lead to error::Code::S_INVALID_ARGUMENT; hence error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE
   * is possible.
   *
   * @see Blob_receiver::S_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in sister concept class's doc header for potentially important discussion.
   */
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = S_META_BLOB_UNDERFLOW_ALLOWED;

  /**
   * Implements concept API.  As of this writing this value depends on
   * Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT and `S_USE_OS_DGRAM_SUPPORT`; iff and only if both are
   * `true`, then this value is `> 1`; otherwise it is `1`.
   */
  static constexpr size_t S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION
    = Sync_io_obj::S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION;

  /// Implements concept API.  Same notes as for #S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION; the values are equal.
  static constexpr size_t S_RCV_BLOB_BATCH_SZ_RECOMMENDATION = Sync_io_obj::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION;

  // Constructors/destructor.

  /**
   * Default ctor (object is in NULL state).
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   * All the notes for both concepts' default ctors apply.
   *
   * This ctor is informally intended for the following uses:
   *   - A moved-from Native_socket_stream (i.e., the `src` arg move-ctor and move-assignment operator)
   *     becomes as-if defaulted-constructed.
   *   - A target Native_socket_stream for Native_socket_stream_acceptor::async_accept() shall typically be
   *     default-cted; Native_socket_stream_acceptor shall asynchronously move-assign a logger-apointed,
   *     nicely-nicknamed into that target `*this`.
   *
   * Therefore it would be unusual (though allowed) to make direct calls such as sync_connect() and send_blob()
   * on a default-cted Native_socket_stream without first moving a non-default-cted object into it.
   */
  Native_socket_stream();

  /**
   * Creates a Native_socket_stream in NULL (not connected) state.
   *
   * This ctor is informally intended for the following use:
   *   - You create a Native_socket_stream that is logger-appointed and nicely-nicknamed; then you call
   *     sync_connect() on it in order to move it to, hopefully, PEER states.
   *     It will retain the logger and nickname throughout.
   *
   * Alternatively:
   *   - If you intend to get a Native_socket_stream from a pre-connected #Native_handle:
   *     Use that Native_socket_stream ctor.
   *   - If you intend to get a Native_socket_stream by accepting it server-style:
   *     Use the default Native_socket_stream ctor; pass that object to Native_socket_stream_acceptor::async_accept().
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname_str
   *        Human-readable nickname of the new object, as of this writing for use in `operator<<(ostream)` and
   *        logging only.
   */
  explicit Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str);

  /**
   * Constructs the socket-and-meta-blob stream by taking over an already-connected native Unix domain socket handle.
   * The socket must be freshly connected without any traffic exchanged on the connection so far; otherwise behavior
   * undefined.
   *
   * ### Performance ###
   * The taking over of `native_peer_socket_moved` should be thought of as light-weight.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param native_peer_socket_moved
   *        The wrapped native handle shall be taken over by `*this`; and this wrapper object will be made
   *        `.null() == true`.  In plainer English, the main point is, this is the native socket
   *        over which traffic will proceed.
   * @param nickname_str
   *        Human-readable nickname of the new object, as of this writing for use in `operator<<(ostream)` and
   *        logging only.
   */
  explicit Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                Native_handle&& native_peer_socket_moved);

  /**
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   *
   * ### Performance ###
   * The taking over of `sync_io_core_in_peer_state_moved` should be thought of as light-weight.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See above.
   */
  explicit Native_socket_stream(Sync_io_obj&& sync_io_core_in_peer_state_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   *
   * @param src
   *        See above.
   */
  Native_socket_stream(Native_socket_stream&& src);

  /// Copy construction is disallowed.
  Native_socket_stream(const Native_socket_stream&) = delete;

  /**
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   * All the notes for both concepts' destructors apply but as a reminder:
   *
   * Destroys this peer endpoint which will end both-direction pipes (of those still open) and cancel any pending
   * completion handlers by invoking them ASAP with error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   * As of this writing these are the completion handlers that would therefore be called:
   *   - Any handler passed to async_receive_native_handle() that has not yet been invoked.
   *     There can be 0 or more of these.
   *   - The handler passed to async_end_sending().
   *     Since it is not valid to call async_end_sending() more than once, there is at most 1 of these.
   */
  ~Native_socket_stream();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   * Implements Native_handle_sender *and* Native_handle_receiver APIs at the same time, per their concept contracts.
   * (Also implements Blob_sender *and* Blob_receiver APIs; they are identical.)
   *
   * @see ~Native_socket_stream().
   *
   * @param src
   *        See above.
   * @return `*this` (see concept API).
   */
  Native_socket_stream& operator=(Native_socket_stream&& src);

  /// Copy assignment is disallowed.
  Native_socket_stream& operator=(const Native_socket_stream&) = delete;

  /**
   * Returns nickname, a brief string suitable for logging.  This is included in the output by the `ostream<<`
   * operator as well.  This method is thread-safe in that it always returns the same value.
   *
   * @return See above.
   */
  const std::string& nickname() const;

  // Connect-ops API.

  /**
   * To be invoked in NULL state only, it synchronously and non-blockingly attempts to connect to an opposing
   * Native_socket_stream_acceptor or sync_io::Native_socket_stream_acceptor
   * listening at the given absolute Shared_name; and synchronously reports failure or success, the latter
   * showing `*this` has entered PEER state.  Failure means `*this` remains in NULL state.
   *
   * If invoked outside of NULL state this returns `false` and otherwise does nothing.
   *
   * Note that `*this` (modulo moves) that has entered PEER state can never change state subsequently
   * (even on transmission error); once a PEER, always a PEER.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * system codes most likely from `boost::asio::error` or `boost::system::errc` (but never would-block).
   *
   * @see Class doc header, section "Why no async-connect method, only sync-connect?" for potentially interesting
   *      context.
   *
   * @return `false` if and only if invoked outside of NULL state (that is: in PEER state).
   *
   * @param absolute_name
   *        Absolute name at which the `Native_socket_stream_acceptor` is expected to be listening.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        system codes most likely from `boost::asio::error` or `boost::system::errc` (but never would-block).
   */
  bool sync_connect(const Shared_name& absolute_name, Error_code* err_code = nullptr);

  // Send-ops API.

  /**
   * Implements Native_handle_sender API per contract.  Note this value equals send_blob_max_size() at any given
   * time which is *not* a concept requirement.  Its PEER-state constant value can also be accessed as
   * non-concept-mandated Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH.
   *
   * @return See above.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * Implements Blob_sender API per contract.  Note this value equals send_meta_blob_max_size() at any given
   * time which is *not* a concept requirement.  Its PEER-state constant value can also be accessed as
   * non-concept-mandated Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH.
   *
   * @return See above.
   */
  size_t send_blob_max_size() const;

  /**
   * Implements Native_handle_sender API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * @param hndl_or_null
   *        See above.
   * @param meta_blob
   *        See above.  Reminder: The memory area described by this arg need only be valid until this
   *        method returns.  Perf reminder: That area will not be copied except for very rare circumstances.
   * @param err_code
   *        See above.  Reminder: In rare circumstances, an error emitted here may represent something
   *        detected during handling of a *preceding* send_native_handle() call but after it returned.
   *        #Error_code generated:
   *        error::Code::S_INVALID_ARGUMENT (non-pipe-hosing error: `meta_blob.size()` exceeds
   *        send_meta_blob_max_size()),
   *        error::Code::S_SENDS_FINISHED_CANNOT_SEND (`*end_sending()` was called earlier),
   *        error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND (the incoming-direction processing detected
   *        that the underlying transport is hosed; specific code was logged and can be obtained via
   *        async_receive_native_handle()),
   *        `boost::asio::error::eof` (underlying transport hosed due to graceful closing by the other side),
   *        other system codes most likely from `boost::asio::error` or `boost::system::errc` (but never
   *        would-block), indicating the underlying transport is hosed for that specific reason, as detected during
   *        outgoing-direction processing.
   * @return See above.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                          Error_code* err_code = nullptr);

  /**
   * Implements Blob_sender API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * Reminder: `blob.size() == 0` results in undefined behavior (assertion may trip).
   *
   * @param blob
   *        See above.  Reminder: The memory area described by this arg need only be valid until this
   *        method returns.  Perf reminder: That area will not be copied except for very rare circumstances.
   * @param err_code
   *        See above.  Reminder: In rare circumstances, an error emitted here may represent something
   *        detected during handling of a *preceding* send_blob() call but after it returned.
   *        #Error_code generated: see send_native_handle().
   * @return See above.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = nullptr);

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.
   * Reminder: It's not thread-safe to call this concurrently with other transmission methods on
   * the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND (same meaning as for send_native_handle()/send_blob()),
   * `boost::asio::error::eof` (ditto),
   * other system codes most likely from `boost::asio::error` or `boost::system::errc` (ditto),
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   *
   * Reminder: In rare circumstances, an error emitted there may represent something
   * detected during handling of a preceding send_native_handle() or send_blob() call but after it returned.
   *
   * @tparam Task_err
   *         See above.
   * @param on_done_func
   *        See above.
   * @return See above.  Reminder: If and only if it returns `false`, we're not in PEER state, or `*end_sending()` has
   *         already been called; and `on_done_func()` will never be called, nor will an error be emitted.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * Implements Native_handle_sender, Blob_sender API per contract.  Reminder: It is equivalent to async_end_sending()
   * but with a no-op `on_done_func`.
   *
   * @return See async_end_sending().
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
   * Implements Blob_sender API per contract.
   *
   * If not in PEER state returns a zeroed-out stats object.
   *
   * Thread-safe: can be called concurrently with any other method including `send_*()`.
   * The returned copy is a consistent snapshot.
   *
   * @return Stats snapshot by value.
   */
  Blob_snd_stats blob_send_stats() const;

  /**
   * Implements Blob_sender API per contract.
   *
   * If not in PEER state this is a no-op.
   *
   * Thread-safe: can be called concurrently with any other method including `send_*()`.
   */
  void blob_send_stats_reset();

  /**
   * Returns the same stats as blob_send_stats(): since this transport uses a single Unix-domain-socket
   * pipe for both blobs and native handles, the statistics are identical.
   *
   * Implements Native_handle_sender API per contract.
   *
   * Thread-safe: can be called concurrently with any other method including `send_*()`.
   * The returned copy is a consistent snapshot.
   *
   * @return Stats snapshot by value.
   */
  Blob_snd_stats native_handle_send_stats() const;

  /**
   * Equivalent to blob_send_stats_reset(); see blob_send_stats() rationale.
   *
   * Implements Native_handle_sender API per contract.
   *
   * Thread-safe: can be called concurrently with any other method including `send_*()`.
   */
  void native_handle_send_stats_reset();

  // Receive-ops API.

  /**
   * Implements Native_handle_receiver API per contract.  Note this value equals receive_blob_max_size() at any given
   * time which is *not* a concept requirement.
   *
   * @return See above.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * Implements Blob_receiver API per contract.  Note this value equals receive_meta_blob_max_size() at any given
   * time which is *not* a concept requirement.
   *
   * @return See above.
   */
  size_t receive_blob_max_size() const;

  /**
   * Implements Native_handle_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   * error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE (opposing peer has sent a message with a meta-blob exceeding
   * `target_meta_blob.size()` in length; in particular one can give an empty buffer if no meta-blob expected),
   * error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE (peer gracefully closed pipe via `*end_sending()`),
   * error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE (the outgoing-direction processing detected
   * that the underlying transport is hosed; specific code was logged and can be obtained via send_native_handle()),
   * `boost::asio::error::eof` (underlying transport hosed due to graceful closing by the other side),
   * other system codes most likely from `boost::asio::error` or `boost::system::errc` (but never
   * would-block), indicating the underlying transport is hosed for that specific reason, as detected during
   * incoming-direction processing.
   *
   * error::Code::S_INVALID_ARGUMENT shall never be emitted due to `target_meta_blob.size()` (or as of this writing
   * for any other reason).  See #S_META_BLOB_UNDERFLOW_ALLOWED.
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_hndl
   *        See above.
   * @param target_meta_blob
   *        See above.  Reminder: The memory area described by this arg must be valid up to
   *        completion handler entry.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err_sz>
  bool async_receive_native_handle(Native_handle* target_hndl, const util::Blob_mutable& target_meta_blob,
                                   Task_err_sz&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`: see async_receive_native_handle().
   * In addition: error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB (opposing peer seems to have used a
   * Native_socket_stream::send_native_handle() call, which they shouldn't in the first place, and supplied
   * a non-null #Native_handle, which this Blob_receiver cannot accept).
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_blob
   *        See above.  Reminder: The memory area described by this arg must be valid up to
   *        completion handler entry.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob, Task_err_sz&& on_done_func);

  /**
   * Implements Native_handle_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`: see async_receive_native_handle(),
   * except error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE applies to `batch->target_payload_size()`; and
   * additionally:
   * error::Code::S_INVALID_ARGUMENT (if `!batch->initialized()` or `batch->full()`).
   *
   * @tparam Msg_resource
   *         See above.
   * @tparam Task_err
   *         See above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Msg_resource, typename Task_err>
  bool async_receive_native_handle_batch(Native_handle_batch_in<Msg_resource>* batch, bool assume_would_block,
                                         Task_err&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier `async_receive_*()`.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods on the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`: see async_receive_native_handle_batch().
   * In addition: error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB (opposing peer seems to have used a
   * Native_socket_stream::send_native_handle() call, which they shouldn't in the first place, and supplied
   * a non-null #Native_handle, which this Blob_receiver cannot accept).
   *
   * @tparam Msg_resource
   *         See above.
   * @tparam Task_err
   *         See above.
   * @param batch
   *        See above.
   * @param assume_would_block
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Msg_resource, typename Task_err>
  bool async_receive_blob_batch(Blob_batch_in<Msg_resource>* batch, bool assume_would_block, Task_err&& on_done_func);

  /**
   * Implements Native_handle_receiver, Blob_receiver API per contract.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout = boost::chrono::seconds{5});

  /**
   * Implements Blob_receiver API per contract.
   *
   * If not in PEER state returns a zeroed-out stats object.
   *
   * Thread-safe: can be called concurrently with any other method including `async_receive_*()`.
   * The returned copy is a consistent snapshot.
   *
   * @see Native_socket_stream_msg_batch_in keeps additional per-batch receive stats orthogonal to these.
   *
   * @return Stats snapshot by value.
   */
  Blob_rcv_stats blob_receive_stats() const;

  /**
   * Implements Blob_receiver API per contract.
   *
   * If not in PEER state this is a no-op.
   *
   * Thread-safe: can be called concurrently with any other method including `async_receive_*()`.
   */
  void blob_receive_stats_reset();

  /**
   * Returns the same stats as blob_receive_stats(): since this transport uses a single Unix-domain-socket
   * pipe for both blobs and native handles, the statistics are identical.
   *
   * Implements Native_handle_receiver API per contract.
   *
   * Thread-safe: can be called concurrently with any other method including `async_receive_*()`.
   * The returned copy is a consistent snapshot.
   *
   * @return Stats snapshot by value.
   */
  Blob_rcv_stats native_handle_receive_stats() const;

  /**
   * Equivalent to blob_receive_stats_reset(); see blob_receive_stats() rationale.
   *
   * Implements Native_handle_receiver API per contract.
   *
   * Thread-safe: can be called concurrently with any other method including `async_receive_*()`.
   */
  void native_handle_receive_stats_reset();

  // Misc API.

  /**
   * OS-reported process credential (PID, etc.) info about the *other* connected peer's process, at the time
   * that the OS first established (via local-socket-connect or local-socket-connected-pair-generate call) that
   * opposing peer socket -- unless overridden via mutator (please read here).
   *
   * @see session::Session::remote_peer_process_credentials() offers centralized, caveat-free access to similar info.
   *      It is however higher-level and requires the use of ipc::session infrastructure.
   *      struc::Channel::remote_peer_process_credentials() may also be useful.
   *
   * ### Exact semantics ###
   * If one is not careful, it is possible to get in hard-to-realize trouble with this accessor.  Here are the
   * exact semantics; then we'll briefly discuss their implications.
   *
   *   - If called in non-PEER state, returns util::NULL_PROCESS_CREDENTIALS but emits no error.
   *   - If called in PEER state, but incoming/outgoing-direction processing earlier detected
   *     that the underlying transport is hosed, returns `NULL_PROCESS_CREDENTIALS` (unless throws) and emits
   *     error::Code::S_LOW_LVL_TRANSPORT_HOSED.
   *     - As of this writing no other errors are possible.
   *   - Otherwise (main case):
   *     - If remote_peer_process_credentials() *mutator* has been called, returns copy of the value given to that
   *       mutator last.  Otherwise:
   *     - Returns the values accurate at the time that the connection was originally established.  To wit:
   *       - If `*this` established connection via sync_connect() (or equivalent such as via native OS-connect):
   *         returns the opposing process's info saved at the moment of successful OS-connect.
   *       - If `*this` established connection via Native_socket_stream_acceptor::async_accept() (or equivalent such
   *         as via native OS-accept):
   *         returns the opposing process's info saved at the moment of successful OS-connect (background-accept op
   *         subsequent to native OS-listen).
   *       - If `*this` established connection via boost.asio `connect_pair()` (or equivalent such as via native OS
   *         `socketpair()`): returns the info for the process that executed OS-socket-pair, at the time that
   *         it was called.
   *
   * ### Rationale/implications ###
   * In the *main case* path above: All of that is reasonable and as one would expect, with the exception of the very
   * last bullet point, because typically (not always) one would then transmit one of the handles (FDs) returned by
   * OS-socket-pair call to another process (over yet another Native_socket_stream) and then communicate over
   * that channel.  In particular `ipc::session`-created `Channel`s internally operate that way.  In that case
   * one might invoke remote_peer_process_credentials() and expect the PID/etc. of the opposing process; but that
   * will be accurate only for the guy who received the handle/FD; whereas for the guy who sent it this method
   * will return that guy's own PID... unhelpful, usually.
   *
   * The following are the key implications/corollaries of that.
   *   - You should be aware of it, instead of assuming "opposing peer" means what you think it means.
   *   - If you'd like to override this behavior, when it is inconvenient, use the mutator
   *     remote_peer_process_credentials() (as shown above, those values will then be returned).  You would need
   *     the correct values to pass to it, but these are usually available from the socket-stream used to
   *     transmit the FD.  Namely one would perhaps call *that* guy's `.remote_peer_process_credentials()`.
   *     - The `Channel`s returned by ipc::session already internally use this mechanism.  That is
   *       Channel::remote_peer_process_credentials() will yield the actual opposing process's stuff, assuming
   *       an ipc::session::Session or variant generated the Channel.  (If you create the `Channel` yourself, then
   *       it is up to you to use the mutator to correct for this yourself.)
   *
   * @return See above; or util::NULL_PROCESS_CREDENTIALS if invoked outside of PEER state or in case of error.
   *         The 2 eventualities can be distinguished by checking `*err_code` truthiness.  Better yet
   *         only call remote_peer_process_credentials() in PEER state, as it is otherwise conceptually meaningless.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_LOW_LVL_TRANSPORT_HOSED (the incoming/outgoing-direction processing detected
   *        that the underlying transport is hosed; specific code was logged and can be obtained via
   *        async_receive_native_handle() or similar).
   */
  const util::Process_credentials& remote_peer_process_credentials(Error_code* err_code = nullptr) const;

  /**
   * Overrides what same-named accessor shall return subsequently; useful when a socket-stream endpoint travels
   * to another process relative to where the connection was first established.
   *
   * After calling this mutator, the same-named accessor shall return a value equal to `creds`, unless it yields
   * `{}` (error case).
   *
   * @see doc header for remote_peer_process_credentials() accessor for important relevant information.
   *
   * @param creds
   *        The value to save.
   * @return `true` if called in PEER state (`creds` was saved);
   *         `false` if called in another state (`creds` was not saved).
   */
  bool remote_peer_process_credentials(const util::Process_credentials& creds);

private:
  // Types.

  /// Short-hand for `const`-respecting wrapper around Native_socket_stream_impl for the pImpl-lite idiom.
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
   * It returns #m_impl... but if it is null, then it is first re-initialized to a new default-cted
   * Native_socket_stream_impl.  That is:
   *   - Any ctor will make #m_impl non-null.
   *   - But moving-from `*this` will make #m_impl null.
   *   - However *if* the user were to invoke a public method of `*this` after the latter, then that public method
   *     shall invoke impl() which shall make #m_impl non-null again, which will ensure the public method works fine.
   *
   * Why?  Answer: This is the usual question of what to do with null m_impl (e.g., it's mentioned in cppreference.com
   * pImpl page) which occurs when one moves-from a given object.  There are various possibilities; but in our case we
   * do want to enable regular use of a moved-from object; hence we promised that a moved-from object is simply in
   * NULL (not-connected) state, as-if default-cted.  So we do that lazily (on-demand) on encountering
   * null m_impl.  Of course we could do it directly inside move ctor/assignment (non-lazily), but in many cases such
   * objects are abandoned in practice, so it's best not to start threads, etc., unless really desired.  The
   * null check cost is minor.
   *
   * Caveat: To be used by `const` methods this method must be `const`; but it must at times modify #m_impl
   * (the pointer... not the pointee).  For that reason #m_impl must be `mutable`.  That is fine: `mutable` for
   * lazy evaluation is a vanilla pattern.
   *
   * @return Reference to #m_impl.
   */
  Impl_ptr& impl() const;

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

/* Template implementations.
 * It is forwarding to impl() (essentially m_impl) a-la pImpl (but pImpl-lite... we have templates... while
 * most of these can be de-templatized by using Function<> to maintain strict pImpl, the *_batch() guys cannot
 * due to `typename Msg_resource`). */

template<typename Task_err>
bool Native_socket_stream::async_end_sending(Task_err&& on_done_func)
{
  return impl()->async_end_sending(std::move(on_done_func));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_native_handle(Native_handle* target_hndl,
                                                       const util::Blob_mutable& target_meta_blob,
                                                       Task_err_sz&& on_done_func)
{
  /* Perf note: In all cases, as of this writing, Native_socket_stream_impl would wrap the various handler
   * parameterized args in concrete Function<>s anyway for its own impl ease; so we change nothing by
   * (implicitly, if needed) constructing flow::async::Task_asio_err_sz higher up in the call stack in this
   * template and its siblings below. */

  return impl()->async_receive_native_handle(target_hndl, target_meta_blob, std::move(on_done_func));
}

template<typename Task_err_sz>
bool Native_socket_stream::async_receive_blob(const util::Blob_mutable& target_blob, Task_err_sz&& on_done_func)
{
  return impl()->async_receive_blob(target_blob, std::move(on_done_func));
}

template<typename Msg_resource, typename Task_err>
bool Native_socket_stream::async_receive_native_handle_batch(Native_handle_batch_in<Msg_resource>* batch,
                                                             bool assume_would_block, Task_err&& on_done_func)
{
  return impl()->async_receive_native_handle_batch<Msg_resource>(batch, assume_would_block, std::move(on_done_func));
}

template<typename Msg_resource, typename Task_err>
bool Native_socket_stream::async_receive_blob_batch(Blob_batch_in<Msg_resource>* batch,
                                                    bool assume_would_block, Task_err&& on_done_func)
{
  return impl()->async_receive_blob_batch<Msg_resource>(batch, assume_would_block, std::move(on_done_func));
}

} // namespace ipc::transport
