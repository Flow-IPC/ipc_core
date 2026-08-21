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
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <flow/log/log.hpp>
#include <boost/asio.hpp>
#include <ostream>

/**
 * Additional (versus boost.asio) APIs for advanced work with local (Unix domain) stream and connected-datagram
 * sockets including transmission of native handles through them; and peer process credentials acquisition.
 *
 * ### Rationale ###
 * These exist, in the first place, because internally such things as Native_socket_stream needed them for
 * impl purposes.  However they are of general usefulness publicly and hence are not tucked away under `detail/`.
 * Because, from a public API point of view, they are orthogonal to the main public APIs (like Native_socket_stream),
 * they are in a segregated namespace.
 *
 * That said, to conserve time without sacrificing reusability, generally speaking features were implemented only
 * when there was an active use case for each -- or the cost of adding them was low.  Essentially APIs are written
 * in such a way as to be generally usable in the same spirit as built-in boost.asio APIs -- or at least reasonably
 * natural to get to that point in the future.
 *
 * ### Overview ###
 * As of this writing `asio_local_stream_socket` has the following features w/r/t local (Unix domain) sockets.
 *   - Convenience aliases (`local_ns`, #Peer_socket, #Acceptor, #Endpoint, etc.).
 *   - Socket option for use with boost.asio API `Peer_socket::get_option()` that gets the opposing process's
 *     credentials (PID, UID, ...) (Opt_peer_process_credentials).
 *   - Writing of blob + native handle combos (boost.asio supports only the former)
 *     (nb_write_some_with_native_handle()).
 *   - Reading of blob + native handle combos (boost.asio supports only the former)
 *     (nb_read_some_with_native_handle()).
 *     - Batch-reading of same (a-la Linux `recvmmsg()`).  Msg_batch_in class template does not simply wrap a single
 *       receive-message-batch OS-call; rather it retains the data structures from read to read, enabling repeated
 *       batch-reading with ~no setup cost per batch-read.
 *
 * The socket-transmission APIs generally support, via template argument, two types of local (Unix domain) sockets,
 * at least in Linux:
 *   - Streams (`SOCK_STREAM` type): By far the most commonly used type out there, this is the socket type that is
 *     TCP-like (but local, not networked).  That is, connected socket + stream of bytes, no message boundaries.
 *     - In boost.asio this is known as `stream_protocol` (#Protocol_byte_stream).
 *   - Connected-datagram (`SOCK_SEQPACKET` type): A somewhat exotic socket type, it has these characteristics:
 *     - Connected socket, same as the stream-type.  (So still TCP-like in that sense.)
 *     - Stream of *datagrams*, meaning message boundaries exist and are preserved.  (Thus UDP-like in that sense.)
 *     - In boost.asio this is known as `seq_packet_protocol` (#Protocol_pkt_stream).
 *
 * By definition batch-transmission (as via Msg_batch_in) can only apply to a message-boundary-preserving protocol;
 * therefore any support for batching applies only to #Protocol_pkt_stream, not #Protocol_byte_stream.
 *
 * @todo `asio_local_stream_socket` additional feature: APIs that can read and write native sockets together with
 * accompanying binary blobs can be extended to handle an arbitrary number of native handles (per call) as opposed to
 * only 0 or 1.  The main difficulty here is designing a convenient and stylish, yet performant, API.  Having achieved
 * this we could also similarly extend the Native_handle_sender, Native_handle_receiver
 * concepts and their impls.
 *
 * @todo Analogously to how Msg_batch_in supports batch-receipt, we could add API(s) for batch-send.  Having achieved
 * this we could also similarly extend the Blob_sender, Native_handle_sender concepts and their impls.  Without getting
 * into details: sending and receiving, especially in batches, are not really mirror images of each other conceptually.
 * Hence lacking one but not the other is not necessarily an omission due to lack of time; the need for it is not as
 * clear (we feel); and the API(s) would probably be designed fairly differently.
 */
namespace ipc::transport::asio_local_stream_socket
{

// Types.

/* (The @namespace and @brief thingies shouldn't be needed, but some Doxygen bug necessitated them.
 * See flow::log::fs for explanation... same thing here.) */

/**
 * @namespace ipc::transport::asio_local_stream_socket::local_ns
 * @brief Short-hand for boost.asio Unix domain socket namespace.  In particular `connect_pair()` free function lives
 *        here.
 */
namespace local_ns = boost::asio::local;

/**
 * Short-hand for boost.asio Unix domain stream-socket -- without built-in message boundary preservation -- protocol.
 *
 * In Unix-world this is known as `AF_LOCAL+SOCK_STREAM` (a/k/a `AF_UNIX+SOCK_STREAM`) and is widely supported in
 * POSIX-land.  It is similar to TCP (but local, with all simplifications this entails), and a key point is that
 * it *does not preserve message boundaries*.  That is, if I try to OS-write 10 bytes, it might write only the first
 * 5 (however, if an ancillary native-handle/FD was part of the write, then the FD will have been written if and only if
 * the OS-write reported writing-out at least 1 byte).  Therefore to send a bounded message, one must use
 * a length prefix or use a sentinel scheme.
 */
using Protocol_byte_stream = local_ns::stream_protocol;

#ifndef FLOW_OS_LINUX
static_assert(false, "Flow-IPC has some support for AF_LOCAL/SOCK_SEQPACKET and relies on Linux semantics for it; "
                       "this may be available in other OS with alleged spotty support, and we have not tested it "
                       "elsewhere.  For now build in Linux only.");
#endif

/**
 * Short-hand for boost.asio Unix domain stream-socket -- *with* built-in message boundary preservation -- protocol.
 *
 * In Linux-world this is known as `AF_LOCAL+SOCK_SEQPACKET` (a/k/a `AF_UNIX+SOCK_SEQPACKET`) and is known to be
 * well supported in Linux after a certain vintage.  (It is allegedly supported in some other Unixes and is allegedly
 * mentioned but not required by POSIX.  So far we have only tested in Linux, and generally internal code paths that
 * use it in Flow-IPC can fall-back to #Protocol_byte_stream if so configured at compile-time.)
 *
 * It is identical to #Protocol_byte_stream, including in terms of endpoints, `connect()` behavior, and `connect_pair()`
 * behavior.  The one, key difference is it *does preserve message boundaries*.  That is, if I try to OS-write 10 bytes,
 * it will either write 0 bytes, or it will write 10 bytes; it will not write 5 (or 2, or 9).  Accordingly:
 * on the other end, each OS-read will -- regardless of the size of the supplied buffer (except that it is big enough
 * to accept the message), or how many bytes are actually readable -- return either 0 bytes or N bytes, and N shall
 * equal exactly the # of bytes written (and requested to be written) by an OS-write call on the other end.
 *
 * The handle/FD transmission semantics are likely as one would expect and arguably even simpler than with
 * #Protocol_byte_stream; a handle/FD (if any) one attempts to be transmitted in an OS-write was indeed transmitted if
 * and only if the message was (no worries about it "belonging to byte 1").
 *
 * ### Rationale ###
 * Dealing in bounded messages, a/k/a datagrams, is pretty common (almost universal).  So naturally this is simply
 * convenient in many cases -- all else being equal; it is not necessary to send length-bearing prefixes or
 * sentinels/escaping.  Beyond convenience, though, when trying to squeeze out all possible performance from an
 * IPC system it allows one to significantly reduce the number of I/O syscalls, thus loading the kernel less
 * (reduced kernel locking).
 *
 * It also allows for the advanced feature, available at least in Linux, which is batched sends/receives
 * via `sendmmsg()` and `recvmmsg()`.  These guys allow one to send or receive several messages at once, with a
 * single syscall.  Msg_batch_in provides receive-batching support.
 *
 * ### Versus "datagram" protocol ###
 * `AF_LOCAL+SOCK_DGRAM` is -- in terms of send/receive semantics -- in practice identical.  (I (ygoldfel) should
 * say in theory in practice; it is what various docs say; but we have not as of this writing heavily verified it.)
 * It is also, subjectively speaking, less exotic/obscure -- perhaps owing to being similar to UDP (but
 * reliable and non-reordering, in this local context).
 *
 * However it is connectionless.  (Its `connect()` behavior is very different and is more of a memory and filter;
 * there is no built-in graceful-close "token" semantic; and it cannot be generated via `connect_pair()`.)  This is
 * not necessarily worse; but it is different.  Therefore as of this writing internal code paths in Flow-IPC
 * use either #Protocol_byte_stream or #Protocol_pkt_stream.
 */
using Protocol_pkt_stream = local_ns::seq_packet_protocol;

/**
 * Short-hand for boost.asio Unix domain stream-socket acceptor (listening guy) socket.
 * @tparam Protocol
 *         At least, `Protocol_byte_stream` or `Protocol_pkt_stream`.
 */
template<typename Protocol>
using Acceptor = typename Protocol::acceptor;

/**
 * Short-hand for boost.asio Unix domain peer stream-socket (usually-connected-or-empty guy).
 * @tparam Protocol
 *         At least, `Protocol_byte_stream` or `Protocol_pkt_stream`.
 */
template<typename Protocol>
using Peer_socket = typename Protocol::socket;

/**
 * Short-hand for boost.asio Unix domain peer stream-socket endpoint.
 * @tparam Protocol
 *         At least, `Protocol_byte_stream` or `Protocol_pkt_stream`.
 */
template<typename Protocol>
using Endpoint = typename Protocol::endpoint;

// Find doc headers near the bodies of these compound types.

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
class Msg_batch_in;

class Opt_peer_process_credentials;

// Free functions.

/**
 * boost.asio extension similar to
 * `peer_socket->non_blocking(true); auto n = peer_socket->send(payload_blob)` with the added
 * capability of accompanying the `Blob_const payload_blob` with a native handle to be transmitted to the
 * opposing peer.
 *
 * In other words it attempts to immediately send `payload_hndl` and at least 1 byte of `payload_blob`
 * (when `Protocol` is `Protocol_byte_stream`) or all of it (when `Protocol` is `Protocol_pkt_stream`);
 * returns would-block error code if this would require blocking; or another error if the connection has become hosed.
 * Performing `peer_socket->send()` given `peer_socket->non_blocking() == true` has the same semantics except
 * it cannot and will not transmit any native handle.
 *
 * @see Please read the "Blob/handle semantics" about working with native
 *      handle accompaniment, in the nb_read_some_with_native_handle() doc header.
 *
 * Certain aspects of `Peer_socket::send()` are not included in the present function, however, though merely
 * because they were not necessary as of this writing and hence excluded for simplicity; these are formally described
 * below.
 *
 * ### Formal behavior ###
 * This function requires that `payload_blob` be non-empty; and `payload_hndl.null() == false`.
 * (If you want to send a non-empty buffer but no handle, then just use boost.asio `Peer_socket::send()`.
 * As of this writing the relevant OS shall not support receiving a handle but a null buffer.)
 *
 * The function exits without blocking.  The sending occurs synchronously, if possible, or does not occur otherwise.
 * A successful operation is defined as sending 1+ bytes of the blob (for #Protocol_byte_stream) or
 * all bytes of the blob (for #Protocol_pkt_stream); and the native handle.  It is not possible
 * that the native handle is transmitted but 0 bytes of the blob (or, for #Protocol_pkt_stream, not-all bytes of
 * the blob) are.  See `flow::Error_code` docs for error reporting
 * semantics (if `err_code` is non-null, `*err_code` is set to code or success; else exception with that code is
 * thrown in the former [non-success] case).  (Informally: Most of the time, assuming no error condition on the
 * connection, the function will return success.  This deals with local (Unix domain as of this writing) peer
 * connections; and the other side likely uses ipc::transport::Native_socket_stream which takes care to read incoming
 * messages ASAP at all times; therefore would-block when sending should be rarer than even with remote TCP traffic.)
 *
 * The following are all the possible outcomes:
 *   - `N > 0` is returned (specifically `N` equals total size of `payload_blob` for #Protocol_pkt_stream);
 *     and `*err_code == {}` if non-null.
 *     This indicates 1 or more (`N`) bytes of the buffer, and the handle, were sent successfully.
 *     - If `N` is less than total size of `payload_blob`, then the remaining bytes cannot currently
 *       be sent without blocking and should be tried later.
 *       - This is possible with #Protocol_byte_stream but not #Protocol_pkt_stream.
 *   - If non-null `err_code`, then `N == 0` is returned; and `*err_code == E` is set to the triggering problem.
 *     If null, then `flow::error::Runtime_error` is thrown containing `Error_code E`.
 *     - `E == boost::asio::error::would_block` specifically indicates the non-fatal condition wherein `*peer_socket`
 *       cannot currently send, until it reaches writable state again.
 *     - Other `E` values indicate the connection is (potentially gracefully) permanently incapable of transmission.
 *     - `E == operation_aborted` is not possible.
 *
 * Items are extensively logged on `*logger_ptr`, and we follow the normal best practices to avoid verbose messages
 * at the severities strictly higher than `TRACE`.  In particular, any error is logged as a `WARNING`, so in particular
 * there's no need for caller to specifically log about the details of a non-false `E`.
 *
 * ### Features of `Peer_socket::send()` not provided here ###
 * We have (consciously) made these concessions:
 *  - This function never blocks, regardless of `peer_socket->non_blocking()`.  `send()` -- if unable to
 *    immediately send 1+ bytes -- will block until it can, if `peer_socket->non_blocking() == false` mode had been
 *    set.  (That's why we named it `nb_...()`.)
 *
 * The following is not a concession, and these words may be redundant, but: `Peer_socket::send()` has
 * ~2 overloads; one that throws exception on error; and one that takes an `Error_code&`; whereas we combine the two
 * via the `Error_code*` null-vs-not dichotomy.  (It's redundant, because it's just following the Flow pattern.)
 *
 * Lastly, `Peer_socket::send()` has an overload wherein one can
 * pass in a (largely unportable, I (ygoldfel) think) `message_flags` bit mask.  We do not provide this feature: again
 * because it is not needed, but also because depending on the flag it may lead to unexpected corner cases, and we'd
 * rather not deal with those unless needed in practice.
 *
 * ### Rationale ###
 * This function exists because elsewhere in ipc::transport needed it internally.  It is a public API basically
 * opportunistically: it's generic enough to be useful in its own right potentially, but as of this writing there's
 * no use case.  This explains the aforementioned concessions compared to boost.asio's function(s).
 * All can be implemented without controversy.  If we wanted to make an "official-looking" boost.asio extension then
 * there would be merit in no longer conceding those concessions.
 *
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `send()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param payload_hndl
 *        The native handle to transmit.  If `payload_hndl.null()` behavior is undefined (possible
 *        assertion trip).  Reiterating the above outcome semantics: if the return value `N` indicates even 1 byte
 *        was sent, then this was successfully sent also.
 * @param payload_blob
 *        The buffer (possibly scattered) to transmit.
 *        Reiterating the above outcome semantics: Either there is no error, and then the
 *        `N` returned will be 1+; or a truthy `Error_code` is returned either via the out-arg or via thrown
 *        `Runtime_error`, and in the former case 0 is returned.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        `boost::asio::error::would_block` (socket not writable, likely because other side isn't reading ASAP),
 *        other system codes (see notes above in the outcome discussion).
 * @return 0 if non-null `err_code` and truthy resulting `*err_code`, and hence no bytes or the handle was sent; 1+
 *         if that number of bytes were sent plus the native handle (and hence falsy `*err_code` if non-null).
 *         For #Protocol_pkt_stream, if this returns not-zero, then it shall equal the total size in `payload_blob`.
 * @tparam Protocol
 *         At least, `Protocol_byte_stream` or `Protocol_pkt_stream`.
 * @tparam Const_buffer_sequence
 *         See `ConstBufferSequence` concept in boost.asio docs.  For your convenience, refresher:
 *         This is often simply util::Blob_const (a single, non-scattered buffer); or
 *         `std::array<util::Blob_const, 2>` or `boost::array<util::Blob_const, 2>` (usually for a prefix-frame of
 *         known length and a payload frame of arbitrary length); or beyond that `vector` or `list` (etc.) of
 *         util::Blob_const.  (To obtain a `Blob_const` there exist boost.asio adapters for many common
 *         single-buffer-storing/representing containers/types including `vector<uint8_t>` and such.)
 */
template<typename Protocol, typename Const_buffer_sequence>
size_t nb_write_some_with_native_handle(flow::log::Logger* logger_ptr,
                                        Peer_socket<Protocol>* peer_socket,
                                        Native_handle payload_hndl,
                                        const Const_buffer_sequence& payload_blob,
                                        Error_code* err_code);

/**
 * boost.asio extension similar to
 * `peer_socket->non_blocking(true); auto n = peer_socket->receive(target_payload_blob)` with the added
 * capability of reading (from opposing peer) not only `target_payload_blob` but an optionally accompanying
 * native handle.
 *
 * In other words it attempts to immediately read at least 1 byte into `target_payload_blob`
 * and, if also present, the native handle into `*target_payload_hndl`; returns would-block error code if this
 * would require blocking; or another error if the connection has become hosed.  Performing `peer_socket->receive()`
 * given `peer_socket->non_blocking() == true` has the same semantics except it cannot and will not read any native
 * handle.  (It would probably just "eat it"/ignore it; though we have not tested that at this time.)
 *
 * Informally speaking:
 *   - For #Protocol_byte_stream: Message boundaries are not respected (are not a thing).  On successful read of
 *     1+ bytes, the number of bytes received shall be as many as can fit into `target_payload_blob` and are
 *     pending in the kernel.
 *   - For #Protocol_pkt_stream: Message boundaries are respected.  On successful read of
 *     1+ bytes, the number of bytes received shall be as many as are
 *     contained in the *next pending datagram* of length equal to what was supplied in the corresponding
 *     write call on the opposing side of the connection.  (If these cannot fit into `target_payload_blob`, it
 *     is an error; see below.)
 *
 * Certain aspects of `Peer_socket::receive()` are not included in the present function, however, though merely
 * because they were not necessary as of this writing and hence excluded for simplicity; these are formally described
 * below.
 *
 * ### Formal behavior ###
 * This function requires that `target_payload_blob` be non-empty.  It shall at entry set `*target_payload_hndl`
 * so that `target_payload_hndl->null() == true`.  `target_payload_hndl` (the pointer) must not be null.
 *
 * @note Suppose `Protocol` is #Protocol_byte_stream; what if you expect no handle?  Then you have two choices.
 *       (1) If you would rather just ignore an incoming handle, even if it does arrive, then use boost.asio's
 *       `Peer_socket::receive()` (or `read_some()`).  (2) If you want to catch that case (perhaps to throw an error),
 *       then use a `Native_handle dummy` together with this free function; and act accordingly if
 *       `dummy.null() == false` on return.
 *
 * @note Suppose `Protocol` is #Protocol_pkt_stream; what if you expect no handle?  Then:
 *       Firstly, **do not simply use `Peer_socket::receive()`**!  It does not, at least, handle the buffer-overflow
 *       (`target_payload_blob` too small) situation in a nice and portable way, whereas the present free function does,
 *       and you should take advantage of that.  So: use a `Native_handle dummy` together with this free function;
 *       and either ignore its out-value (if you don't care whether they sent handle after all), or act appropriately
 *       if `dummy.null() == false` on return (if you do care).
 *
 * The function exits without blocking.  The receiving occurs synchronously, if possible, or does not occur otherwise.
 * A successful operation is defined as receiving 1+ bytes into the blob; and the native handle if it was present.
 * It is not possible that a native handle is received but 0 bytes of the blob are.  See `flow::Error_code` docs for
 * error reporting semantics (if `err_code` is non-null, `*err_code` is set to code or success; else exception with
 * that code is thrown in the former [non-success] case).
 *
 * The following are all the possible outcomes:
 *   - `N > 0` is returned; and `*err_code == {}` if non-null.
 *     This indicates 1 or more (`N`) bytes were placed at the start of the buffer, and *if* exactly 1 native handle
 *     handle was transmitted along with some subset of those `N` bytes, *then* it was successfully received into
 *     `*target_payload_hndl`; or else the fact there were exactly 0 such handles was successfully determined and
 *     reflected via `target_payload_hndl->null() == true`.
 *     - If `N` is less than the total size of `target_payload_blob` then:
 *       - (For #Protocol_byte_stream) no further bytes can currently be read without blocking and should
 *         be tried later if desired.  (Informally: In that case consider `Peer_socket::async_wait()` followed by
 *         retrying the present function.)
 *       - (For #Protocol_pkt_stream) this is not particularly special, nor is it conceptually different from
 *         `N` being exactly equal to the total size of `target_payload_blob`.
 *         It just means the next pending in-datagram was of size `N`;
 *         it would be perfectly reasonable to try another read, as there may be more in-datagram(s) following.
 *   - If non-null `err_code`, then `N == 0` is returned; and `*err_code == E` is set to the triggering problem.
 *     If null, then `flow::error::Runtime_error` is thrown containing `Error_code E`.
 *     - `E == boost::asio::error::would_block` specifically indicates the non-fatal condition wherein `*peer_socket`
 *       cannot currently receive, until it reaches readable state again (i.e., bytes and possibly handle arrive from
 *       peer).
 *     - Other `E` values indicate a true error.
 *       - In most cases this means the connection is (potentially gracefully) permanently incapable of transmission.
 *         - In particular `E == boost::asio::error::eof` indicates the connection was gracefully closed by peer.
 *           (Informally, this is usually not to be treated differently from other fatal errors like
 *           `boost::asio::error::connection_reset`.)
 *         - The errors error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE and
 *           error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL are the only ones which do *not*
 *           mean the socket is hosed.
 *     - `E == operation_aborted` is not possible.
 *
 * Items are extensively logged on `*logger_ptr`, and we follow the normal best practices to avoid verbose messages
 * at the severities strictly higher than `TRACE`.  In particular, any error is logged as a `WARNING`, so in particular
 * there's no need for caller to specifically log about the details of a non-false `E`.
 * (If this is still too slow, you may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
 * temporarily, in that thread only, disable logging.  This is quite easy and performant.)
 *
 * ### Blob/handle semantics: `Protocol_byte_stream` ###
 * Non-blocking stream-blob-send/receive semantics must be familiar to you, such as from TCP and otherwise.
 * By adding native handles (further, just *handles*) as accompaniment to this system, non-trivial -- arguably
 * subtle -- questions are raised about how it all works together.  The central question is, perhaps, if I ask to send
 * N bytes and handle S, non-blockingly, what are the various possibilities for sending less than N bytes of the blob
 * and whether S is also sent?  Conversely, how will receiving on the other side work?  The following describes those
 * semantics and *mandates* how to properly handle it.  Not following these formally leads to undefined behavior.
 * (Informally, for the tested OS versions, it is possible to count on certain additional behaviors; but trying to do
 * so is (1) prone to spurious changes and breakage in different OS versions and types, since much of this is
 * undocumented; and (2) will probably just make your life *more* difficult anyway, not less.  Honestly I (ygoldfel)
 * designed it for ease of following as opposed to exactly maximal freedom of capability.  So... just follow these.)
 *
 * Firstly, as already stated, it is not possible to try sending a handle sans a blob; and it is not possible
 * to receive a handle sans a blob.  (The converse is a regular non-blocking blob write or receive op.)
 *
 * Secondly, one must think of the handle as associated with *exactly the first byte* of the blob arg to the
 * write call (nb_write_some_with_native_handle()).  Similarly, one must think of the handle as associated with
 * *exactly the first byte* of the blob arg to the read call (nb_read_some_with_native_handle()).  Moreover,
 * known OS make certain not-well-documented assumptions about message lengths.  What does this all
 * mean in practice?
 *   - You may design your protocol however you want, except the following requirement: Define a
 *     *handle-containing message* as a combination of a blob of 1+ bytes of some known (on both sides, at the time
 *     of both sending and receipt) length N *and* exactly *one* handle.  You must aim to send this message and
 *     receive it exactly as sent, meaning with message boundaries respected.  (To be clear, you're free to use
 *     any technique to make N known on both sides; e.g., it may be a constant; or it may be passed in a previous
 *     message.  However, it's not compatible with using a sentinel alone, as then N is unknown.)
 *     - You must only transmit handles as part of handle-containing messages.  Anything else is undefined behavior.
 *   - Let M be a given handle-containing message with blob B of size N; and handle H.
 *     Let a *write op* be nb_write_some_with_native_handle().
 *     - You shall attempt one write op for the blob B of size N together with handle H.  Do *not* intermix it with any
 *       other bytes or handles.
 *       - In the non-blocking write op case (nb_write_some_with_native_handle()) it may yield successfully sending
 *         N' bytes, where 1 <= N' < N.  This means the handle was successfully sent also, because the handle is
 *         associated with the *first byte* of the write -- and read -- op.  If this happens, don't worry about it;
 *         continue with the rest of the protocol, including sending at least the remaining (N - N') bytes of M.
 *     - On the receiver side, you must symmetrically execute the read op (nb_read_some_with_native_handle(), perhaps
 *       after a `Peer_socket::async_wait()`) to attempt receipt of all of M, including supplying a target
 *       blob of exactly N bytes -- without mixing with any other bytes or handles.  The "hard" part of this is mainly
 *       to avoid having the previous read op "cut into" M.
 *       - (Informally, a common-sense way to do it just make
 *         your protocol message-based, such that the length of the next message is always known on either side.)
 *       - Again, if the nb_read_some_with_native_handle() call returns N', where 1 <= N' < N, then no worries.
 *         The handle H *will* have been successfully received, being associated with byte 1 of M.
 *         Keep reading the rest of M (namely, the remaining (N - N') bytes of the blob B) with more read op(s).
 *       - (To put a fine point on it: In known Linux versions as of this writing, if you do try to read-op N' bytes
 *         having executed write-op with N'' bytes, where N'' > N', then you may observe very strange, undefined
 *         (albeit non-crashy), behavior such as H disappearing or replacing a following-message handle H'.  Don't.)
 *
 * That is admittedly many words, but really in practice it's fairly natural and simple to design a message-based
 * protocol and implementation around it.  Just do follow these; I merely wanted to be complete.
 *
 * ### Blob/handle semantics: `Protocol_pkt_stream` ###
 * The situation is quite a bit simpler than for `Protocol_byte_stream`.  A single write-call will send a datagram
 * of N bytes, and either a native handle or none; and the corresponding read-call will receive that datagram
 * (same contents including length) and the same handle, or none.
 *
 * However: If the total size of `target_payload_blob` is less than `M`, where `M` is the size of the next pending
 * in-datagram in the relevant kernel buffer, then:
 *   - We emit error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE, and the in-dgram is lost.  A warning may be logged,
 *     possibly indicating how long the in-dgram would have been.
 *   - The connection is *not* hosed: further I/O may be attempted.
 *   - (Informally: The practical implication of this is: It's best to agree on a max-size, so that you can
 *     supply a target-buffer that is always of that size and hence sufficiently large to handle any in-datagram.)
 *
 * @note Internally, in Linux, we do this using a combination of the `MSG_TRUNC` in-flag and the incidentally
 *       eponymous out-flag.  Do not supply `message_flags = MSG_TRUNC`; that stuff is our responsibility.
 *
 * ### Features of `Peer_socket::receive()` not provided here ###
 * We have (consciously) made these concessions: ...see nb_write_some_with_native_handle() doc header.  All of the
 * listed omitted features have common-sense counterparts in the case of the present function, except that we do
 * provide a `message_flags` arg.
 *
 * ### Rationale ###
 * This function exists because... [text omitted -- same reasoning as similar rationale for
 * nb_write_some_with_native_handle()].
 *
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to stream socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `receive()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param target_payload_hndl
 *        The native handle wrapper into which to copy the received handle; it shall be set such that
 *        `target_payload_hndl->null() == true` if the read-op returned 1+ (succeeded), but those bytes were not
 *        accompanied by any native handle.  It shall also be thus set if 0 is returned (indicating error
 *        including would-block and fatal errors).
 * @param target_payload_blob
 *        The buffer (possibly scattered) into which to write received blob data, namely up to the total size
 *        of this (possibly) scattered buffer.  If the total size is 0, behavior is undefined (assertion may trip).
 *        Reiterating the above outcome semantics: Either there is no error, and then the `N` returned will be 1+; or a
 *        truthy `Error_code` is returned either via the out-arg or via thrown `Runtime_error`, and in the former case
 *        0 is returned.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        `boost::asio::error::would_block` (socket not readable: no data pending),
 *        ipc::transport::error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL
 *        (strictly more than 1 handle detected in the read-op, but we support only 1 at this time; see above;
 *        maybe they didn't use above write-op function(s) and/or didn't follow anti-straddling suggestion above),
 *        ipc::transport::error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE
 *        (#Protocol_pkt_stream only: next pending in-datagram exceeds total size of `target_payload_blob`;
 *        the in-dgram is lost, but the connection is *not* hosed; see above),
 *        other system codes (see notes above in the outcome discussion).
 * @param message_flags
 *        See boost.asio `Peer_socket::receive()` overload with this arg.  As of this writing we see utility for
 *        `MSG_CMSG_CLOEXEC` (Linux at least); but otherwise have not delved into the effects of other flags.
 *        Also do not pass-in `MSG_DONTWAIT` or `MSG_TRUNC` (formally, behavior undefined).
 * @return 0 if non-null `err_code` and truthy resulting `*err_code`, and hence no bytes nor a handle was received; 1+
 *         if that number of bytes were received plus either a native handle or none (and hence falsy `*err_code`
 *         if non-null).  For #Protocol_pkt_stream: we reiterate that non-0 means that the in-datagram
 *         was sized N (where N is return value here).
 *
 * @tparam Protocol
 *         At least, `Protocol_byte_stream` or `Protocol_pkt_stream`.
 * @tparam Mutable_buffer_sequence
 *         See `MutableBufferSequence` concept in boost.asio docs.  See `Const_buffer_sequence` param doc header
 *         for nb_write_some_with_native_handle(); a similar refresher applies here but as applied to
 *         util::Blob_mutable instead of util::Blob_const.
 *
 * @internal
 * ### Implementation notes -- `Protocol_byte_stream` ###
 * Where does the content of "Blob/handle semantics" originate?  Answer: Good question, as reading `man` pages to do
 * with `sendmsg()/recvmsg()/cmsg/unix`, etc., gives hints but really is incomplete and certainly not formally complete.
 * Without such a description, one can guess at how `SOL_SOCKET/SCM_RIGHTS` (sending of FDs along with blobs) might work
 * but not conclusively.  I (ygoldfel) nevertheless actually correctly developed the relevant conclusions via common
 * sense/experience... and *later* confirmed them by reading kernel source and the delightfully helpful
 * write-up at [ https://gist.github.com/kentonv/bc7592af98c68ba2738f4436920868dc ] (Googled "SCM_RIGHTS gist").
 * Reading these may give the code inspector/maintainer (you?) more peace of mind.  Basically, though, the key gist
 * is:
 *   - The handle(s) are associated with byte 1 of the blob given to the `sendmsg()` call containing those handle(s).
 *     For this reason, to avoid protocol chaos, you should send each given handle with the same "synchronized" byte
 *     on both sides.
 *   - The length of that blob similarly matters -- which is not normal, as otherwise message boundaries are *not*
 *     normally maintained for stream connections -- and for this reason the read op must accept a result into a blob
 *     of at *least* the same size as the corresponding write op.  (For simplicity and other reasons my
 *     instructions say it should just be equal.)
 *
 * ### Implementation notes -- `Protocol_pkt_stream` ###
 * This (instead of `Protocol_byte_stream`) removes all that ambiguity.  On the other hand it's not super-well
 * documented itself (in `man` pages and such, that is).
 */
template<typename Protocol, typename Mutable_buffer_sequence>
size_t nb_read_some_with_native_handle(flow::log::Logger* logger_ptr,
                                       Peer_socket<Protocol>* peer_socket,
                                       Native_handle* target_payload_hndl,
                                       const Mutable_buffer_sequence& target_payload_blob,
                                       Error_code* err_code,
                                       int message_flags = 0);

/**
 * Serializes a Msg_batch_in to a standard output stream.
 *
 * @relatesalso Msg_batch_in
 *
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
std::ostream& operator<<(std::ostream& os,
                         const Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>& val);

} // namespace ipc::transport::asio_local_stream_socket
