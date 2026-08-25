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

#include "ipc/util/shared_name_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <ostream>
#include <string>

/**
 * Flow-IPC module providing transmission of structured messages and/or low-level blobs (and more)
 * between pairs of processes.  See namespace ::ipc doc header for an overview of Flow-IPC modules including
 * how ipc::transport relates to the others.  Then return here.  A synopsis follows:
 *
 * The main transport features of ipc::transport are: class template struc::Channel (for structured message and
 * native handle transport) and various lower-level utilities (Channel; blob/handle streams; message queue
 * (MQ) streams).  Structured transmission facilities (struc::Channel being the main guy) are segregated in
 * sub-namespace transport::struc.
 *
 * Generally speaking, to communicate (via struc::Channel and others), the two processes A and B that intend to
 * talk must have established a broad conversation called a *session* within which all communication occurs.
 * In fact, a transport::struc::Channel wraps a transport::Channel, and the latter can be established, in factory-ish
 * fashion and otherwise, from an ipc::session::Session.  Hence, see namespace ipc::session doc header to learn about
 * establishing/terminating sessions.  Once you have a session::Session, you can actually use main ipc::transport
 * facilities.
 *
 * That said: ipc::transport does *not* require ipc::session to be used: One can instantiate all the various IPC
 * mechanisms therein directly.  ipc::session provides the lifecycle and organization to make this as simple as
 * possible (but no simpler).  In that sense ipc::transport has the essential building blocks; ipc::session provides
 * access to those building blocks in one possible fashion -- for example by establishing a naming convention
 * for the various required `Shared_name`s taken by the various ipc::transport constructors.  Formally speaking
 * there can certainly be other fashions of organizing ipc::transport resources.  Therefore the API design of
 * ipc::transport is not rigid.
 */
namespace ipc::transport
{

// Types.

// Find doc headers near the bodies of these compound types.

class Native_socket_stream_cfg;
class Native_socket_stream;
class Native_socket_stream_acceptor;
template<typename Msg_resource_t>
class Native_socket_stream_msg_batch_in;
class Posix_mq_handle;
class Bipc_mq_handle;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver;
class Null_peer;
template<typename Blob_sender_t, typename Blob_receiver_t,
         typename Native_handle_sender_t, typename Native_handle_receiver_t>
class Channel;
template<bool SIO>
class Socket_stream_channel;
template<bool SIO>
class Socket_stream_channel_of_blobs;
template<bool SIO,
         typename Persistent_mq_handle,
         typename Native_handle_sender_t = Null_peer, typename Native_handle_receiver_t = Null_peer>
class Mqs_channel;
template<bool SIO,
         typename Persistent_mq_handle>
class Mqs_socket_stream_channel;
class Protocol_negotiator;
template<typename Msg_resource_t, bool NO_HNDLS = true>
class Generic_msg_batch_in;

/// Convenience alias for the commonly used type util::Native_handle.
using Native_handle = util::Native_handle;

/// Convenience alias for the commonly used type util::Shared_name.
using Shared_name = util::Shared_name;

/**
 * Convenience alias: Blob_sender via unidirectional POSIX MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mq_sender = Blob_stream_mq_sender<Posix_mq_handle>;

/**
 * Convenience alias: Blob_receiver via unidirectional POSIX MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mq_receiver = Blob_stream_mq_receiver<Posix_mq_handle>;

/**
 * Convenience alias: Blob_sender via unidirectional bipc MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mq_sender = Blob_stream_mq_sender<Bipc_mq_handle>;

/**
 * Convenience alias: Blob_receiver via unidirectional bipc MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mq_receiver = Blob_stream_mq_receiver<Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver) at one end of full-duplex (bidirectional) pipe
 * composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mqs_channel_of_blobs = Mqs_channel<false, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver) at one end of full-duplex (bidirectional) pipe
 * composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mqs_channel_of_blobs = Mqs_channel<false, Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver)
 * at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs
 * (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain stream connection,
 * transmitting native-handle-and/or-meta-blob messages.
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mqs_socket_stream_channel = Mqs_socket_stream_channel<false, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver)
 * at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing unidirectional bipc MQs
 * (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain stream connection,
 * transmitting native-handle-and/or-meta-blob messages.
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mqs_socket_stream_channel = Mqs_socket_stream_channel<false, Bipc_mq_handle>;

// Free functions.

/**
 * Prints string representation of the given `Native_socket_stream` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);

/**
 * Prints string representation of the given `Native_socket_stream_msg_batch_in` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_msg_batch_in
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Msg_resource_t>
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_msg_batch_in<Msg_resource_t>& val);

/**
 * Prints string representation of the given `Native_socket_stream_acceptor` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_acceptor
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_receiver
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_sender
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given Bipc_mq_handle to the given `ostream`.
 *
 * @relatesalso Bipc_mq_handle
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Bipc_mq_handle& val);

/**
 * Prints string representation of the given Posix_mq_handle to the given `ostream`.
 *
 * @relatesalso Posix_mq_handle
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Posix_mq_handle& val);

/**
 * Prints string representation of the given `Channel` to the given `ostream`.
 *
 * @relatesalso Channel
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Blob_sender_t, typename Blob_receiver_t,
         typename Native_handle_sender_t, typename Native_handle_receiver_t>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Blob_sender_t, Blob_receiver_t,
                                       Native_handle_sender_t, Native_handle_receiver_t>& val);

/**
 * Dummy that is never invoked.  It must still exist in order for Channel to build successfully with at least 1
 * Null_peer template arg.
 *
 * Assertion may trip if this is invoked.  Formally behavior is undefined.
 *
 * @relatesalso Null_peer
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Null_peer& val);

/**
 * Prints string representation of the given `Generic_msg_batch_in` to the given `ostream`.
 *
 * @relatesalso Generic_msg_batch_in
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Msg_resource_t, bool NO_HNDLS>
std::ostream& operator<<(std::ostream& os, const Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>& val);

/**
 * Implements Persistent_mq_handle related concept: Swaps two objects.
 *
 * @relatesalso Bipc_mq_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Bipc_mq_handle& val1, Bipc_mq_handle& val2);

/**
 * Implements Persistent_mq_handle related concept: Swaps two objects.
 *
 * @relatesalso Posix_mq_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Posix_mq_handle& val1, Posix_mq_handle& val2);

} // namespace ipc::transport

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::transport::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Blob_snd_stats;
struct Blob_rcv_stats;

// Free functions.

/**
 * Declares the stats for Blob_snd_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Blob_snd_stats* src_stats, Blob_snd_stats* target_stats,
                   Visitor&& visitor);

/**
 * Maps a stats `struct` type to its core Blob_snd_stats; identity for Blob_snd_stats itself.
 * See Blob_sender::Blob_snd_stats and Native_handle_sender::Native_handle_snd_stats concept doc headers
 * for context.  Both concepts require this same function (the core send-stats type is shared).
 *
 * Generic use with proper ADL (let `Cool_sender` be a Blob_sender or Native_handle_sender impl instance).
 *   ~~~
 *   using ipc::transport::stat::blob_snd_stats;
 *   Cool_sender::Blob_snd_stats total_stats = ...;
 *   const auto& core_stats = blob_snd_stats(total_stats);
 *   // core_stats has type Blob_snd_stats.
 *   // If Cool_sender::Blob_snd_stats is Blob_snd_stats, then the present free function was invoked.
 *   // Otherwise an actual mapping function in `Cool_sender`s namespace was invoked via ADL.
 *   ~~~
 *
 * @param stats
 *        Thing.
 * @return `stats`.
 */
const Blob_snd_stats& blob_snd_stats(const Blob_snd_stats& stats);

/**
 * Non-`const` wrapper around blob_snd_stats().  Safe because the input is non-`const`, so the
 * `const_cast` merely undoes the `const` added by delegating to the `const` overload.
 *
 * @tparam Stats_t
 *         `Blob_snd_stats`, or an extended stats type with a `blob_snd_stats(const Stats_t&)` ADL overload.
 * @param stats
 *        Stats to map.
 * @return See blob_snd_stats(); but non-`const`.
 */
template<typename Stats_t>
Blob_snd_stats& blob_snd_stats_mutable(Stats_t& stats);

/**
 * Declares the stats for Blob_rcv_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Blob_rcv_stats* src_stats, Blob_rcv_stats* target_stats,
                   Visitor&& visitor);

/**
 * Maps a stats `struct` type to its core Blob_rcv_stats; identity for Blob_rcv_stats itself.
 * See Blob_receiver::Blob_rcv_stats and Native_handle_receiver::Native_handle_rcv_stats concept doc headers
 * for context.  Both concepts require this same function (the core receive-stats type is shared).
 *
 * Generic use with proper ADL (let `Cool_receiver` be a Blob_receiver or Native_handle_receiver impl instance).
 *   ~~~
 *   using ipc::transport::stat::blob_rcv_stats;
 *   Cool_receiver::Blob_rcv_stats total_stats = ...;
 *   const auto& core_stats = blob_rcv_stats(total_stats);
 *   // core_stats has type Blob_rcv_stats.
 *   // If Cool_receiver::Blob_rcv_stats is Blob_rcv_stats, then the present free function was invoked.
 *   // Otherwise an actual mapping function in `Cool_receiver`s namespace was invoked via ADL.
 *   ~~~
 *
 * @param stats
 *        Thing.
 * @return `stats`.
 */
const Blob_rcv_stats& blob_rcv_stats(const Blob_rcv_stats& stats);

/**
 * Non-`const` wrapper around blob_rcv_stats().  Safe because the input is non-`const`, so the
 * `const_cast` merely undoes the `const` added by delegating to the `const` overload.
 *
 * @tparam Stats_t
 *         `Blob_rcv_stats`, or an extended stats type with a `blob_rcv_stats(const Stats_t&)` ADL overload.
 * @param stats
 *        Stats to map.
 * @return See blob_rcv_stats(); but non-`const`.
 */
template<typename Stats_t>
Blob_rcv_stats& blob_rcv_stats_mutable(Stats_t& stats);

} // namespace ipc::transport::stat

/**
 * `sync_io`-pattern counterparts to async-I/O-pattern object types in parent namespace ipc::transport.
 * For example transport::sync_io::Native_socket_stream <=> transport::Native_socket_stream.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 */
namespace ipc::transport::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.
class Native_socket_stream;
class Native_socket_stream_acceptor;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver;

/// Convenience alias: sync_io::Blob_sender via unidirectional POSIX MQ (message queue).
using Posix_mq_sender = Blob_stream_mq_sender<Posix_mq_handle>;
/// Convenience alias: sync_io::Blob_receiver via unidirectional POSIX MQ (message queue).
using Posix_mq_receiver = Blob_stream_mq_receiver<Posix_mq_handle>;
/// Convenience alias: sync_io::Blob_sender via unidirectional bipc MQ (message queue).
using Bipc_mq_sender = Blob_stream_mq_sender<Bipc_mq_handle>;
/// Convenience alias: sync_io::Blob_receiver via unidirectional bipc MQ (message queue).
using Bipc_mq_receiver = Blob_stream_mq_receiver<Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver) at one end of full-duplex
 * (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 */
using Posix_mqs_channel_of_blobs = transport::Mqs_channel<true, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver) at one end of full-duplex
 * (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 */
using Bipc_mqs_channel_of_blobs = transport::Mqs_channel<true, Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver, sync_io::Native_handle_sender,
 * sync_io::Native_handle_receiver) at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing
 * unidirectional POSIX MQs (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain
 * stream connection, transmitting native-handle-and/or-meta-blob messages.
 */
using Posix_mqs_socket_stream_channel = Mqs_socket_stream_channel<true, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver, sync_io::Native_handle_sender,
 * sync_io::Native_handle_receiver) at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing
 * unidirectional bipc MQs (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain
 * stream connection, transmitting native-handle-and/or-meta-blob messages.
 */
using Bipc_mqs_socket_stream_channel = Mqs_socket_stream_channel<true, Bipc_mq_handle>;

// Free functions.

/**
 * Implements `{Native_handle|Blob}_receiver::async_receive_*_batch()` by emulating batch-receiving as a
 * series of single-message receive-ops.
 *
 * ### How to use ###
 * Just call us; we'll do it.  The main requirement is to provide a proper `async_rcv_impl_func()` which shall
 * perform a regular one-message receive.
 *
 * The formal requirements for `async_rcv_impl_func()` are as follows.  It shall be called in a `void` context
 * and have the following arguments, in order:
 *   - (If and only if `NO_HNDLS == false`) `Native_handle*`: Target handle object.
 *   - `bool`: If `true`, your function may assume the pipe is in would-block state already
 *     which may help it be more efficient in doing its task; otherwise it must make no such assumption.
 *     (This will *not* simply always equal the eponymous argument to async_receive_batch_emulation()!)
 *   - util::Blob_mutable: Target memory area for the async-read.
 *   - `Error_code*`: The error-code object for the op.  This shall *not* be null (you do *not* need to throw
 *     an exception to emit an error).
 *   - `size_t*`: Set the pointee to the received in-blob's size, unless an error is emitted.
 *   - Function-object of the specific type `flow::async::Task_asio_err_sz`, a/k/a
 *     `Function<void (const Error_code& err_code, size_t n_rcvd)>`.
 *     Attention!  This may, or may not, be `.empty()`.  See below.
 *
 * It shall act as-if `{Native_handle|Blob}_receiver::async_receive_{native_handle|blob}()` was called, except:
 *   - Which one it is: it must act consistently with `NO_HNDLS`.
 *     - It shall take the extra leading `Native_handle*` arg (see above) if `NO_HNDLS == false`.
 *   - It may assume the following checks have all passed: in PEER state, `start_*_ops()` has been called, no
 *     pipe-hosing error recorded yet from prior ops, no `async_receive_*()` already outstanding,
 *     `batch->initialized() && (!batch->full())`.
 *   - May act differently (probably for perf savings) depending on the `bool` arg (assume-would-block; see above).
 *   - If it emits non-success, non-would-block (in-pipe-hosing) error `E`, *and* the desired outcome *if* 1+
 *     in-messages have already been received successfully into `*batch` is *delayed error* (emit no error for this
 *     batch-receive, but next async-receive on that in-pipe shall instantly yield `E`), then `E` *must* be one of the
 *     following:
 *       - error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE,
 *       - `boost::asio::error::eof`.
 *       - Any other `E` in that situation will cause the batch-receive to emit `E` (and therefore leave
 *         `batch->n_used()` unchanged); 1+ in-messages shall be eaten.  (Promise: any `Native_handle`s received
 *         with the eaten in-messages shall be closed -- returned to the OS -- not leaked.)
 *   - The on-done handler may be non-empty (as required for normal user-triggered calls) or `.empty()`.
 *     If it's non-empty, act normally.  If it's empty, and no would-block is encountered, act normally; which is
 *     to say synchronously emit the result, and that's that (on-done handler ignored).  If it's empty, and
 *     would-block *is* encountered then there are 2 possibilities.
 *     - If would-block is encountered immediately (no partial message is read/non-steady state reached):
 *       Emit would-block as normal; but *end the async-receive op*.  Do not issue an async-wait for readability;
 *       do not save on-done handler (which is empty anyway).
 *     - If would-block is encountered after receiving part of an in-message/a non-steady state is reached:
 *       Do the same; plus:
 *       - Memorize the partial-message payload/non-steady state, so that if another
 *         async-receive is issued by something/someone later, your state machine starts from the point as-if
 *         the partial-message payload/state had already been received/reached.  (After all: it *had* already been
 *         received/reached -- it just ended up irrelevant to the previous async-receive; namely us.)
 *
 * Unfortunately that last bullet point can be tricky to implement (or even understand what it means),
 * so we must discuss.  Firstly the (potential) good news: If your protocol
 * combined with the underlying transport is such that reading a partial in-message
 * is impossible, and (non-hosed) pipe state is the same regardless of where a would-block occurs,
 * then the bullet point cannot apply, and things (your code) remain simple.
 *
 * Example: Native_socket_stream, when operating (and this is determined at compile-time, not run-time) in
 * datagram mode (asio_local_stream_socket::Protocol_pkt_stream, not `Protocol_byte_stream`; a/k/a
 * in Linux `SOCK_SEQPACKET`, not `SOCK_STREAM`) can (and in our impl as of this writing does) map each in-message
 * to one in-datagram, and the pipe is always in the same state (barring being hosed) upon receiving any in-dgram.
 *
 * However if it possible to read a payload which encodes (potentially) part of an in-message/non-steady state,
 * then the bullet point may apply, and you must code for it.
 *
 * Example: Blob_stream_mq_receiver is essentially dgram-based too, so each in-message maps to one lower-level
 * dgram (MQ message), *but* as of this writing our internal protocol is (for certain boring logical/technical reasons)
 * such that some (non-user-in-message) messages are represented by 2 dgrams (MQ messages), not 1: a CONTROL
 * message like an auto-ping is represented by an empty in-message (enters CONTROL state) and then a particular
 * enumeration-value-encoding message (indicates auto-ping; goes back to normal state).  So, the start of
 * async-receive cannot assume normal state; it might be in CONTROL state because of the above.  Of course this
 * is pretty easy to handle; just keep a CONTROL-or-not state flag; and resume the state machine based on its value.
 * There's no need to store any user payload copy.
 *
 * Example: Native_socket_stream, when operating in stream mode (asio_local_stream_socket::Protocol_byte_stream
 * a/k/a `SOCK_STREAM` unlike the earlier example) might hit would-block after any given byte whatsoever; right down
 * to the "worst case" of, say, reading all of a 60K-long payload except for the very last expected byte.  Then you'd
 * have to copy it from the would-be user buffer; and feed that part to the next async-receive's user buffer (another
 * copy).  Fortunately this eventuality should not be frequent enough to affect overall perf.
 *
 * In that case you'll need to do somehow save any partial-read results and apply them to the next async-read
 * *of any type* at the start of that async-read, before reading any further potential low-level data.  Usually
 * this isn't a matter of performance, as at worst it's part of only 1 in-message, but you'll need some extra code,
 * and the support for emulated batch-receiving will impact code not-itself-necessarily-related to batch-receiving.
 *
 * @warning It is important that `async_rcv_impl_func()` cache any new pipe-hosing error it encounters
 *          (and therefore emits via `Error_code*` -- but note it may emit non-pipe-hosing error(s) too, most notably
 *          would-block; that is different); so that if an async-receive is attempted subsequently, it knows to
 *          immediately emit it.  We mention this, because async_receive_batch_emulation() will -- in the case of
 *          encountering 1+ in-messages followed by error (e.g., graceful close
 *          error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE) -- *only* emit the 1+ messages and not any error; it will
 *          stop but ignore the error itself.  `{Native_handle|Blob}_receiver::async_receive_*()`
 *          contract is to emit the error next time in that
 *          case -- and async_receive_batch_emulation() shall take no steps of its own to make that happen.
 *          Your `async_rcv_impl_func()` must take care of that.
 *
 * ### Rationale / use-cases ###
 * @see Native_handle_receiver concept doc header "Batch-receiving" section for background.
 *
 * As of this writing Native_socket_stream and Blob_stream_mq_receiver use it internally.  The latter does so, since
 * (as of now anyway) there is no built-in batch-receiving OS support for POSIX MQs (nor bipc MQs).  The former does
 * so if and only if Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT is `false`.  In English -- in both
 * cases there's no low-level batch-receiving, so to provide that higher-level interface it has to be emulated
 * based on normal receiving.
 *
 * The same task would be faced by any potential Blob_receiver or Native_handle_receiver concept implementer,
 * including when a Flow-IPC user wants to do so; an advanced type of work but entirely supported.  Therefore it would
 * be relatively rare that a user would call this... but definitely possible.  Hence it is public.
 *
 * @tparam NO_HNDLS
 *         `true` if implementing Blob_receiver::async_receive_blob_batch().
 *         `false` if Native_handle_receiver::async_receive_native_handle_batch().
 * @tparam Batch
 *         Blob_receiver::Blob_batch_in or Native_handle_receiver::Native_handle_batch_in (see `NO_HNDLS`).
 *         In addition, it must have the APIs with identical names/semantics to
 *         `Generic_msg_batch_in::clear_used(size_t)`, Generic_msg_batch_in::emulate_result(),
 *         Generic_msg_batch_in::next_target_blob(), and -- if `NO_HNDLS == false` --
 *         Generic_msg_batch_in::next_target_hndl().  (Rationale: We could have instead simply required that
 *         `Batch` be an instance of `Generic_msg_batch_in<Msg_resource, NO_HNDLS>`.  Decided to formally allow
 *         any type, as long as it has the required behavior -- for freedom in custom scenarios.)
 * @tparam Task_err
 *         As for `{Native_handle|Blob}_receiver::async_receive_*_batch()`.
 * @tparam Async_rcv_impl_func
 *         See above.
 * @param logger_ptr
 *        Logger to use for logging in this op, including its async-continuation if applicable.
 * @param batch
 *        As for `{Native_handle|Blob}_receiver::async_receive_*_batch()`.
 * @param assume_would_block
 *        As for `{Native_handle|Blob}_receiver::async_receive_*_batch()`.
 * @param sync_err_code
 *        As for `{Native_handle|Blob}_receiver::async_receive_*_batch()`.
 * @param on_done_func
 *        As for `{Native_handle|Blob}_receiver::async_receive_*_batch()`.
 * @param async_rcv_impl_func
 *        Your `*_receiver`'s single-message receive op.  See above.
 */
template<bool NO_HNDLS, typename Batch, typename Task_err, typename Async_rcv_impl_func>
void async_receive_batch_emulation(flow::log::Logger* logger_ptr,
                                   Batch* batch, bool assume_would_block,
                                   Error_code* sync_err_code, Task_err&& on_done_func,
                                   Async_rcv_impl_func&& async_rcv_impl_func);

/**
 * Prints string representation of the given `Native_socket_stream` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);

/**
 * Prints string representation of the given `Native_socket_stream_acceptor` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_acceptor
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_sender
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_receiver
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle>& val);

} // namespace ipc::transport::sync_io
