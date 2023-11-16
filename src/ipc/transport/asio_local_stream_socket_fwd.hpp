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

/**
 * Additional (versus boost.asio) APIs for advanced work with local stream (Unix domain) sockets including
 * transmission of native handles through such streams; and peer process credentials acquisition.
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
 * As of this writing `asio_local_stream_socket` has the following features w/r/t local stream (Unix domain) sockets:
 *   - Convenience aliases (`local_ns`, #Peer_socket, #Acceptor, #Endpoint, etc.).
 *   - Socket option for use with boost.asio API `Peer_socket::get_option()` that gets the opposing process's
 *     credentials (PID, UID, ...) (Opt_peer_process_credentials).
 *   - Writing of blob + native handle combos (boost.asio supports only the former)
 *     (nb_write_some_with_native_handle(), async_write_with_native_handle(), etc.).
 *   - Reading of blob + native handle combos (boost.asio supports only the former)
 *     (nb_read_some_with_native_handle()).
 *   - More advanced composed blob reading operations (async_read_with_target_func(), at least).
 *
 * @todo At least asio_local_stream_socket::async_read_with_target_func() can be extended to
 * other stream sockets (TCP, etc.).  In that case it should be moved to a different namespace however
 * (perhaps named `asio_stream_socket`; could then move the existing `asio_local_stream_socket` inside that one
 * and rename it `local`).
 *
 * @todo `asio_local_stream_socket` additional feature: APIs that can read and write native sockets together with
 * accompanying binary blobs can be extended to handle an arbitrary number of native handles (per call) as opposed to
 * only 0 or 1.  The main difficulty here is designing a convenient and stylish, yet performant, API.
 *
 * @todo `asio_local_stream_socket` additional feature: APIs that can read and write native handles together with
 * accompanying binary blobs can be extended to handle scatter/gather semantics for the aforementioned blobs,
 * matching standard boost.asio API functionality.
 *
 * @todo `asio_local_stream_socket` additional feature: Non-blocking APIs like nb_read_some_with_native_handle()
 * and nb_write_some_with_native_handle() can gain blocking counterparts, matching standard boost.asio API
 * functionality.
 *
 * @todo `asio_local_stream_socket` additional feature: `async_read_some_with_native_handle()` --
 * async version of existing nb_read_some_with_native_handle().  Or another way to put it is,
 * equivalent of boost.asio `Peer_socket::async_read_some()` but able to read native handle(s) with the blob.
 * Note: This API would potentially be usable inside the impl of existing APIs (code reuse).
 *
 * @todo `asio_local_stream_socket` additional feature: `async_read_with_native_handle()` --
 * async version of existing nb_read_some_with_native_handle(), plus the "stubborn" behavior of built-in `async_read()`
 * free function.  Or another way to put it is, equivalent of boost.asio `async_read<Peer_socket>()` but able to read
 * native handle(s) with the blob.
 * Note: This API would potentially be usable inside the impl of existing APIs (code reuse).
 *
 * @internal
 * ### Implementation notes ###
 * As one would expect the native-handle-transmission APIs use Linux `recvmsg()` and `sendmsg()` with the
 * `SCM_RIGHTS` ancillary-message type.  For some of the
 * to-dos mentioned above:
 *   - Both of those functions take buffers in terms of `iovec` arrays; the present impl merely provides a 1-array.
 *     It would be pretty easy to extend this to do scatter/gather.
 *   - To offer blocking versions, one can simply start a `flow::async::Single_thread_task_loop` each time
 *     and `post()` onto it in `S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION` mode.  Alternatively one could write more
 *     performant versions that would directly use the provided sockets in blocking mode; this would be much more work.
 */
namespace ipc::transport::asio_local_stream_socket
{

// Types.

/// Short-hand for boost.asio Unix domain socket namespace.  In particular `connect_pair()` free function lives here.
namespace local_ns = boost::asio::local;

/// Short-hand for boost.asio Unix domain stream-socket protocol.
using Protocol = local_ns::stream_protocol;

/// Short-hand for boost.asio Unix domain stream-socket acceptor (listening guy) socket.
using Acceptor = Protocol::acceptor;

/// Short-hand for boost.asio Unix domain peer stream-socket (usually-connected-or-empty guy).
using Peer_socket = Protocol::socket;

/// Short-hand for boost.asio Unix domain peer stream-socket endpoint.
using Endpoint = Protocol::endpoint;

class Opt_peer_process_credentials;

// Free functions.

/**
 * boost.asio extension similar to
 * `boost::asio::async_write(Peer_socket&, Blob_const, Task_err_sz)` with the added capability of
 * accompanying the `Blob_const` with a native handle to be transmitted to the opposing peer.
 *
 * @see Please read the "Blob/handle semantics" about working with native handle
 *      handle accompaniment, in the nb_read_some_with_native_handle() doc header.
 *
 * boost.asio's `async_write()` free function is generically capable of sending a sequence of 1+ buffers on
 * a connected stream socket, continuing until either the entire sequence is fully sent; or an error (not counting
 * would-block, which just means keep trying to make progress when possible).  The capability we add is the native
 * handle in `payload_hndl` is also transmitted.  Certain aspects of `async_write()` are not included in the present
 * function, however, though merely because they were not necessary as of this writing and hence excluded for
 * simplicity; these are formally described below.
 *
 * ### Formal behavior ###
 * This function requires that `payload_blob` be non-empty; and `payload_hndl.null() == false`.
 * (If you want to send a non-empty buffer but no handle, then just use boost.asio `async_write()`.
 * As of this writing the relevant OS shall not support sending a handle but a null buffer.)
 *
 * The function exits without blocking.  The sending occurs asynchronously.  A successful operation is defined as
 * sending all of the blob; and the native handle.  `on_sent_or_error()` shall be called
 * at most once, indicating the outcome of the operation.  (Informally: Most of the time, though asynchronous, this
 * should be a very fast op.  This deals with local (Unix domain as of this writing) peer connections; and the other
 * side likely uses ipc::transport::Native_socket_stream which takes care to read incoming messages ASAP at all times;
 * therefore blocking when sending should be rarer than even with remote TCP traffic.)  The following are all the
 * possible outcomes:
 *   - `on_sent_or_error(Error_code())` is executed as if `post()`ed
 *     on `peer_socket->get_executor()` (the `flow::util::Task_engine`,
 *     a/k/a boost.asio `io_context`, associated with `*peer_socket`), where `N == payload_blob.size()`.
 *     This indicates the entire buffer, and the handle, were sent successfully.
 *   - `on_sent_or_error(E)`, where `bool(E) == true`, is executed similarly.
 *     This indicates the send did not fully succeed, and `E` specifies why this happened.
 *     No indication is given how many bytes were successfully sent (if any even were).
 *     (Informally, there's likely not much difference between those 2 outcomes.  Either way, the connection is
 *     hosed.)
 *     - `E == boost::asio::error::operation_aborted` is possible and indicates your own code canceled pending
 *       async work such as by destroying `*peer_socket`.  Informally, the best way to deal
 *       with it is know it's normal when stuff is shutting down; and to do nothing other than maybe logging,
 *       but even then to not assume all relevant objects even exist; really it's best to just return right away.
 *       Know that upon that return the handler's captured state will be freed, as in all cases.
 *     - `E` will never indicate would-block.
 *   - `on_sent_or_error()` is canceled and not called at all, such as possibly when `stop()`ing the underlying
 *      `Task_engine`.  This is similar to the aforementioned `operation_aborted` situation.
 *      Know that upon that return the handler's captured state *will* be freed at the time of
 *      whatever shutdown/cancellation step.
 *
 * Items are extensively logged on `*logger_ptr`, and we follow the normal best practices to avoid verbose messages
 * at the severities strictly higher than `TRACE`.  In particular, any error is logged as a `WARNING`, so in particular
 * there's no need to for caller to specifically log about the details of a non-false `Error_code`.
 *
 * Thread safety is identical to that of `async_write_some()`.
 *
 * ### Features of `boost::asio::async_write()` not provided here ###
 * We have (consciously) made these concessions:
 *  - `payload_blob` is a single blob.  `async_write()` is templated in such a way as to accept that or a
 *    *sequence* of `Blob_const`s, meaning it supports scatter/gather.
 *  - There are also fancier advanced-async-flow-control overloads of `async_write()` with more features we haven't
 *    replicated.  However the simplest overload only has the preceding 3 bullet points on us.
 *
 * ### Rationale ###
 * This function exists because elsewhere in ipc::transport needed it internally.  It is a public API basically
 * opportunistically: it's generic enough to be useful in its own right potentially, but as of this writing there's
 * no use case.  This explains the aforementioned concessions compared to boost.asio `async_write()` free function.
 * All, without exception, can be implemented without controversy.  It would be busy-work and was omitted
 * simply because there was no need.  If we wanted to make an "official-looking" boost.asio extension then there would
 * be merit in no longer conceding those concessions.
 *
 * @internal
 * Update: transport::Native_socket_stream's impl has been split into itself on top and
 * transport::sync_io::Native_socket_stream as its core -- also available for public use.  Because the latter
 * is now the part doing the low-level I/O, by `sync_io` pattern's nature it can no longer be doing boost.asio
 * async-waits but rather outsources them to the user of that object (transport::Native_socket_stream being
 * a prime example).  So that means the present function is no longer used by Flow-IPC internally as of this
 * writing.  Still it remains a perfectly decent API; so leaving it available.
 *
 * There are to-dos elsewhere to perhaps generalize this guy and his bro(s) to support both boost.asio
 * and `sync_io`.  It would be some ungodly API, but it could have a boost.asio-target wrapper unchanged from
 * the current signature.
 *
 * These 3 paragraphs apply, to one extent or another, to async_write_with_native_handle(),
 * async_read_with_target_func(), and async_read_interruptible().
 * @endinternal
 *
 * @tparam Task_err
 *         boost.asio handler with same signature as `flow::async::Task_asio_err`.
 *         It can be bound to an executor (commonly, a `strand`); this will be respected.
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to stream socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `async_write()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param payload_hndl
 *        The native handle to transmit.  If `payload_hndl.is_null()` behavior is undefined (possible
 *        assertion trip).
 * @param payload_blob
 *        The buffer to transmit.  Reiterating the above outcome semantics: Either there is no error, and then
 *        the `N` passed to the handler callback will equal `payload_blob.size()`; or there is a truthy `Error_code`,
 *        and `N` will be strictly less than `payload_blob.size()`.
 * @param on_sent_or_error
 *        Handler to execute at most once on completion of the async op.  It is executed as if via
 *        `post(peer_socket->get_executor())`, fully respecting any executor
 *        bound to it (via `bind_executor()`, such as a strand).
 *        It shall be passed `Error_code`.  The semantics of these values are shown above.
 *        Informally: falsy `Error_code` indicates total success of sending both items; truthy `Error_code`
 *        indicates a connection-fatal error prevented us from some or both being fully sent; one should disregard
 *        `operation_aborted` and do nothing; else one should consider the connection as hosed (possibly gracefully) and
 *        take steps having discovered this.
 */
template<typename Task_err>
void async_write_with_native_handle(flow::log::Logger* logger_ptr,
                                    Peer_socket* peer_socket,
                                    Native_handle payload_hndl, const util::Blob_const& payload_blob,
                                    Task_err&& on_sent_or_error);

/**
 * boost.asio extension similar to
 * `peer_socket->non_blocking(true); auto n = peer_socket->write_some(payload_blob)` with the added
 * capability of accompanying the `Blob_const payload_blob` with a native handle to be transmitted to the
 * opposing peer.
 *
 * In other words it attempts to immediately send `payload_hndl` and at least 1 byte of `payload_blob`;
 * returns would-block error code if this would require blocking; or another error if the connection has become hosed.
 * Performing `peer_socket->write_some()` given `peer_socket->non_blocking() == true` has the same semantics except
 * it cannot and will not transmit any native handle.
 *
 * @see Please read the "Blob/handle semantics" about working with native
 *      handle accompaniment, in the nb_read_some_with_native_handle() doc header.
 *
 * Certain aspects of `Peer_socket::write_some()` are not included in the present function, however, though merely
 * because they were not necessary as of this writing and hence excluded for simplicity; these are formally described
 * below.
 *
 * ### Formal behavior ###
 * This function requires that `payload_blob` be non-empty; and `payload_hndl.null() == false`.
 * (If you want to send a non-empty buffer but no handle, then just use boost.asio `Peer_socket::write_some()`.
 * As of this writing the relevant OS shall not support receive a handle but a null buffer.)
 *
 * The function exits without blocking.  The sending occurs synchronously, if possible, or does not occur otherwise.
 * A successful operation is defined as sending 1+ bytes of the blob; and the native handle.  It is not possible
 * that the native handle is transmitted but 0 bytes of the blob are.  See `flow::Error_code` docs for error reporting
 * semantics (if `err_code` is non-null, `*err_code` is set to code or success; else exception with that code is
 * thrown in the former [non-success] case).  (Informally: Most of the time, assuming no error condition on the
 * connection, the function will return success.  This deals with local (Unix domain as of this writing) peer
 * connections; and the other side likely uses ipc::transport::Native_socket_stream which takes care to read incoming
 * messages ASAP at all times; therefore would-block when sending should be rarer than even with remote TCP traffic.)
 *
 * The following are all the possible outcomes:
 *   - `N > 0` is returned; and `*err_code == Error_code()` if non-null.
 *     This indicates 1 or more (`N`) bytes of the buffer, and the handle, were sent successfully.
 *     If `N != payload_blob.size()`, then the remaining bytes cannot currently be sent without blocking and should
 *     be tried later.  (Informally: Consider async_write_with_native_handle() in that case.)
 *   - If non-null `err_code`, then `N == 0` is returned; and `*err_code == E` is set to the triggering problem.
 *     If null, then flow::error::Runtime_error is thrown containing `Error_code E`.
 *     - `E == boost::asio::error::would_block` specifically indicates the non-fatal condition wherein `*peer_socket`
 *       cannot currently send, until it reaches writable state again.
 *     - Other `E` values indicate the connection is (potentially gracefully) permanently incapable of transmission.
 *     - `E == operation_aborted` is not possible.
 *
 * Items are extensively logged on `*logger_ptr`, and we follow the normal best practices to avoid verbose messages
 * at the severities strictly higher than `TRACE`.  In particular, any error is logged as a `WARNING`, so in particular
 * there's no need to for caller to specifically log about the details of a non-false `E`.
 *
 * ### Features of `Peer_socket::write_some()` not provided here ###
 * We have (consciously) made these concessions:
 *  - `payload_blob` is a single blob.  `write_some()` is templated in such a way as to accept that or a
 *    *sequence* of `Blob_const`s, meaning it supports scatter/gather.
 *  - This function never blocks, regardless of `peer_socket->non_blocking()`.  `write_some()` -- if unable to
 *    immediately send 1+ bytes -- will block until it can, if `peer_socket->non_blocking() == false` mode had been
 *    set.  (That's why we named it `nb_...()`.)
 *
 * The following is not a concession, and these words may be redundant, but: `Peer_socket::write_some()` has
 * 2 overloads; one that throws exception on error; and one that takes an `Error_code&`; whereas we combine the two
 * via the `Error_code*` null-vs-not dichotomy.  (It's redundant, because it's just following the Flow pattern.)
 *
 * Lastly, `Peer_socket::send()` is identical to `Peer_socket::write_some()` but adds an overload wherein one can
 * pass in a (largely unportable, I (ygoldfel) think) `message_flags` bit mask.  We do not provide this feature: again
 * because it is not needed, but also because depending on the flag it may lead to unexpected corner cases, and we'd
 * rather not deal with those unless needed in practice.
 *
 * ### Rationale ###
 * This function exists because... [text omitted -- same reasoning as similar rationale for
 * async_write_with_native_handle()].
 *
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to stream socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `write_some()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param payload_hndl
 *        The native handle to transmit.  If `payload_hndl.is_null()` behavior is undefined (possible
 *        assertion trip).  Reiterating the above outcome semantics: if the return value `N` indicates even 1 byte
 *        was sent, then this was successfully sent also.
 * @param payload_blob
 *        The buffer to transmit.  Reiterating the above outcome semantics: Either there is no error, and then the
 *        `N` returned will be 1+; or a truthy `Error_code` is returned either via the out-arg or via thrown
 *        `Runtime_error`, and in the former case 0 is returned.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        `boost::asio::error::would_block` (socket not writable, likely because other side isn't reading ASAP),
 *        other system codes (see notes above in the outcome discussion).
 * @return 0 if non-null `err_code` and truthy resulting `*err_code`, and hence no bytes or the handle was sent; 1+
 *         if that number of bytes were sent plus the native handle (and hence falsy `*err_code` if non-null).
 */
size_t nb_write_some_with_native_handle(flow::log::Logger* logger_ptr,
                                        Peer_socket* peer_socket,
                                        Native_handle payload_hndl, const util::Blob_const& payload_blob,
                                        Error_code* err_code);

/**
 * boost.asio extension similar to
 * `peer_socket->non_blocking(true); auto n = peer_socket->read_some(target_payload_blob)` with the added
 * capability of reading (from opposing peer) not only `Blob_mutable target_payload_blob` but an optionally accompanying
 * native handle.
 *
 * In other words it attempts to immediately read at least 1 byte into `*target_payload_blob`
 * and, if also present, the native handle into `*target_payload_hndl`; returns would-block error code if this
 * would require blocking; or another error if the connection has become hosed.  Performing `peer_socket->read_some()`
 * given `peer_socket->non_blocking() == true` has the same semantics except it cannot and will not read any native
 * handle.  (It would probably just "eat it"/ignore it; though we have not tested that at this time.)
 *
 * @see Please read the "Blob/handle semantics" about working with native
 *      handle accompaniment, in the nb_read_some_with_native_handle() doc header.
 *
 * Certain aspects of `Peer_socket::read_some()` are not included in the present function, however, though merely
 * because they were not necessary as of this writing and hence excluded for simplicity; these are formally described
 * below.
 *
 * ### Formal behavior ###
 * This function requires that `target_payload_blob` be non-empty.  It shall at entry set `*target_payload_hndl`
 * so that `target_payload_hndl->null() == true`.  `target_payload_hndl` (the pointer) must not be null.
 * (If you want to receive into non-empty buffer but expect no handle, then just use boost.asio
 * `Peer_socket::read_some()`.  If you want to receive a non-empty buffer but no handle, then just use
 * boost.asio `async_write()`.  As of this writing the relevant OS shall not support receiving a handle but a
 * null buffer.)
 *
 * The function exits without blocking.  The receiving occurs synchronously, if possible, or does not occur otherwise.
 * A successful operation is defined as receiving 1+ bytes into the blob; and the native handle if it was present.
 * It is not possible that a native handle is received but 0 bytes of the blob are.  See `flow::Error_code` docs for
 * error reporting semantics (if `err_code` is non-null, `*err_code` is set to code or success; else exception with
 * that code is thrown in the former [non-success] case).
 *
 * The following are all the possible outcomes:
 *   - `N > 0` is returned; and `*err_code == Error_code()` if non-null.
 *     This indicates 1 or more (`N`) bytes were placed at the start of the buffer, and *if* exactly 1 native handle
 *     handle was transmitted along with some subset of those `N` bytes, *then* it was successfully received into
 *     `*target_payload_hndl`; or else the fact there were exactly 0 such handles was successfully determined and
 *     reflected via `target_payload_hndl->null() == true`.
 *     If `N != target_payload_blob.size()`, then no further bytes can currently be read without blocking and should
 *     be tried later if desired.  (Informally: In that case consider `Peer_socket::async_wait()` followed by retrying
 *     the present function, in that case.)
 *   - If non-null `err_code`, then `N == 0` is returned; and `*err_code == E` is set to the triggering problem.
 *     If null, then flow::error::Runtime_error is thrown containing `Error_code E`.
 *     - `E == boost::asio::error::would_block` specifically indicates the non-fatal condition wherein `*peer_socket`
 *       cannot currently receive, until it reaches readable state again (i.e., bytes and possibly handle arrive from
 *       peer).
 *     - Other `E` values indicate the connection is (potentially gracefully) permanently incapable of transmission.
 *       - In particular `E == boost::asio::error::eof` indicates the connection was gracefully closed by peer.
 *         (Informally, this is usually not to be treated differently from other fatal errors like
 *         `boost::asio::error::connection_reset`.)
 *       - It may be tempting to distinguish between "true" system errors (probably from `boost::asio::error::`)
 *         and "protocol" errors from `ipc::transport::error::Code` (as of this writing
 *         `S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL`): one *can* technically keep reading in the latter
 *         case, in that the underlying connection is still connected potentially.  However, formally, behavior is
 *         undefined if one reads more subsequently.  Informally: if the other side has violated protocol
 *         expectations -- or if your side has violated expectations on proper reading (see below section on that) --
 *         then neither side can be trusted to recover logical consistency and must abandon the connection.
 *     - `E == operation_aborted` is not possible.
 *
 * Items are extensively logged on `*logger_ptr`, and we follow the normal best practices to avoid verbose messages
 * at the severities strictly higher than `TRACE`.  In particular, any error is logged as a `WARNING`, so in particular
 * there's no need for caller to specifically log about the details of a non-false `E`.
 * (If this is still too slow, you may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
 * temporarily, in that thread only, disable logging.  This is quite easy and performant.)
 *
 * ### Blob/handle semantics ###
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
 *     Let a *write op* be nb_write_some_with_native_handle() (or an async_write_with_native_handle() based on it).
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
 *         The handle S *will* have been successfully received, being associated with byte 1 of M.
 *         Keep reading the rest of M (namely, the remaining (N - N') bytes of the blob B) with more read op(s).
 *       - (To put a fine point on it: In known Linux versions as of this writing, if you do try to read-op N' bytes
 *         having executed write-op with N'' bytes, where N'' > N', then you may observe very strange, undefined
 *         (albeit non-crashy), behavior such as S disappearing or replacing a following-message handle S'.  Don't.)
 *
 * That is admittedly many words, but really in practice it's fairly natural and simple to design a message-based
 * protocol and implementation around it.  Just do follow these; I merely wanted to be complete.
 *
 * ### Features of `Peer_socket::read_some()` not provided here ###
 * We have (consciously) made these concessions: ...see nb_write_some_with_native_handle() doc header.  All of the
 * listed omitted features have common-sense counterparts in the case of the present function.
 *
 * ### Advanced feature on top of `Peer_socket::read_some()` ###
 * Lastly, `Peer_socket::receive()` is identical to `Peer_socket::read_some()` but adds an overload wherein one can
 * pass in a (largely unportable, I (ygoldfel) think) `message_flags` bit mask.  We *do* provide a close cousin of this
 * feature via the (optional as of this writing) arg `native_recvmsg_flags`.  (Rationale: We specifically omitted it
 * in nb_write_some_with_native_handle(); yet add it here because the value `MSG_CMSG_CLOEXEC` has specific utility.)
 * One may override the default (`0`) by supplying any value one would supply as the `int flags` arg of Linux's
 * `recvmsg()` (see `man recvmsg`).  As one can see in the `man` page, this is a bit mask or ORed values.  Formally,
 * however, the only value supported (does not lead to undefined behavior) is `MSG_CMSG_CLOEXEC` (please read its docs
 * elsewhere, but in summary it sets the close-on-`exec` bit of any non-null received `*target_payload_blob`).
 * Informally: that flag may be important for one's application; so we provide for it; however beyond that one please
 * refer to the reasoning regarding not supporting `message_flags` in nb_write_some_with_native_handle() as explained
 * in its doc header.
 *
 * ### Rationale ###
 * This function exists because... [text omitted -- same reasoning as similar rationale for
 * async_write_with_native_handle()].
 *
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to stream socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `read_some()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param target_payload_hndl
 *        The native handle wrapper into which to copy the received handle; it shall be set such that
 *        `target_payload_hndl->null() == true` if the read-op returned 1+ (succeeded), but those bytes were not
 *        accompanied by any native handle.  It shall also be thus set if 0 is returned (indicating error
 *        including would-block and fatal errors).
 * @param target_payload_blob
 *        The buffer into which to write received blob data, namely up to `target_payload_blob->size()` bytes.
 *        If null, or the size is not positive, behiavor is undefined (assertion may trip).
 *        Reiterating the above outcome semantics: Either there is no error, and then the `N` returned will be 1+; or a
 *        truthy `Error_code` is returned either via the out-arg or via thrown `Runtime_error`, and in the former case
 *        0 is returned.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        `boost::asio::error::would_block` (socket not writable, likely because other side isn't reading ASAP),
 *        ipc::transport::error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL
 *        (strictly more than 1 handle detected in the read-op, but we support only 1 at this time; see above;
 *        maybe they didn't use above write-op function(s) and/or didn't follow anti-straddling suggestion above),
 *        other system codes (see notes above in the outcome discussion).
 * @param message_flags
 *        See boost.asio `Peer_socket::receive()` overload with this arg.
 * @return 0 if non-null `err_code` and truthy resulting `*err_code`, and hence no bytes or the handle was sent; 1+
 *         if that number of bytes were sent plus the native handle (and hence falsy `*err_code` if non-null).
 *
 * @internal
 * ### Implementation notes ###
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
 *     of at *least* the same same size as the corresponding write op.  (For simplicity and other reasons my
 *     instructions say it should just be equal.)
 *
 * Lastly, I note that potentially using `SOCK_SEQPACKET` (which purports to conserve message boundaries at all times)
 * instead of `SOCK_STREAM` (which we use) might remove all ambiguity.  On the other hand it's barely documented itself.
 * The rationale for the decision to use `SOCK_STREAM` is discussed elsewhere; this note is to reassure that I
 * (ygoldfel) don't quite find the above complexity reason enough to switch to `SOCK_SEQPACKET`.
 */
size_t nb_read_some_with_native_handle(flow::log::Logger* logger_ptr,
                                       Peer_socket* peer_socket,
                                       Native_handle* target_payload_hndl,
                                       const util::Blob_mutable& target_payload_blob,
                                       Error_code* err_code,
                                       int message_flags = 0);

/**
 * boost.asio extension similar to
 * `boost::asio::async_read(Peer_socket&, Blob_mutable, Task_err_sz)` with the difference that the target
 * buffer (util::Blob_mutable) is determined by calling the arg-supplied function at the time when at least 1 byte
 * is available to read, instead of the buffer being given direcly as an arg.  By determining where to target when
 * there are actual data available, one can avoid unnecessary copying in the meantime; among other applications.
 *
 * ### Behavior ###
 * boost.asio's `async_read()` free function is generically capable of receiving into a sequence of 1+ buffers on
 * a connected stream socket, continuing until either the entire sequence is fully filled to the byte; or an error (not
 * counting would-block, which just means keep trying to make progress when possible).  We act exactly the same with
 * the following essential differences being the exceptions:
 *   - The target buffer is determined once system indicates at least 1 byte of data is available to actually read
 *     off socket; it's not supplied as an arg.
 *     - To determine it, we call `target_payload_blob_func()` which must return the util::Blob_mutable.
 *   - One can cancel the (rest of the) operation via `should_interrupt_func()`.  This is called just after
 *     being internally informed the socket is ready for reading, so just before the 1st `target_payload_blob_func()`
 *     call, and ahead of each subsequent burst of non-blockingly-available bytes as well.
 *     If it returns `true`, then the operation is canceled; the target buffer (if it has even been determined)
 *     is not written to further; and `on_rcvd_or_error()` is never invoked.  Note that `should_interrupt_func()`
 *     may be called ages after whatever outside interrupt-causing condition has occurred; typically your
 *     impl would merely check for that condition being the case (e.g., "have we encountered idle timeout earlier?").
 *   - Once the handler is called, its signature is similar (`Error_code`, `size_t` of bytes received) but with one
 *     added arg, `const Blob_mutable& target_payload_blob`, which is simply a copy of the light-weight object returned
 *     per preceding bullet point.
 *     - It is possible `target_payload_blob_func()` is never called; in this case the error code shall be truthy, and
 *       the reported size received shall be 0.  In this case disregard the `target_payload_blob` value received; it
 *       shall be a null/empty blob.
 *     - The returned util::Blob_mutable may have `.size() == 0`.  In this case we shall perform no actual read;
 *       and will simply invoke the handler immediately upon detecting this; the reported error code shall be falsy,
 *       and the reported size received shall be 0.
 *   - If `peer_socket->non_blocking() == false` at entry to this function, it shall be `true`
 *     at entry to the handler, except it *might* be false if the handler receives a truthy `Error_code`.
 *     (Informally: In that case, the connection should be considered hosed in any case and must not be used for
 *     traffic.)
 *   - More logging, as a convenience.
 *
 * It is otherwise identical... but certain aspects of `async_read()` are not included in the present
 * function, however, though merely because they were not necessary as of this writing and hence excluded for
 * simplicity; these are formally described below.
 *
 * ### Thread safety ###
 * Like `async_read()`, the (synchronous) call is not thread-safe with respect to the given `*peer_socket` against
 * all/most calls operating on the same object.  Moreover, this extends to the brief time period when the first byte
 * is available.  Since there is no way of knowing when that might be, essentially you should consider this entire
 * async op as not safe for concurrent execution with any/most calls operating on the same `Peer_socket`, in the
 * time period [entry to async_read_with_target_func(), entry to `target_payload_blob_func()`].
 *
 * Informally, as with `async_read()`, it is unwise to do anything with `*peer_socket` upon calling the present
 * function through when the handler begins executing.
 *
 * `on_rcvd_or_error()` is invoked fully respecting any possible executor (typically none, else a `Strand`) associated
 * (via `bind_executor()` usually) with it.
 *
 * However `should_interrupt_func()` and `target_payload_blob()` are invoked directly via
 * `peer_socket->get_executor()`, and any potential associated executor on these functions themselves is
 * ignored.  (This is the case simply because there was no internal-to-rest-of-Flow-IPC use case for acting otherwise;
 * but it's conceivable to implement it later, albeit at the cost of some more processor cycles used.)
 *
 * ### Features of `boost::asio::async_read()` not provided here ###
 * We have (consciously) made these concessions: ...see async_write_with_native_handle() doc header.  All of the
 * listed omitted features have common-sense counterparts in the case of the present function.  In addition:
 *  - `peer_socket` has to be a socket of that specific type, a local stream socket.  `async_write()` is templated on
 *    the socket type and will work with other connected stream sockets, notably TCP.
 *
 * ### Rationale ###
 * - This function exists because... [text omitted -- same reasoning as similar rationale for
 *   async_write_with_native_handle()].
 *   - Namely, though, the main thing is being able to delay targeting the data until that data are actually
 *     available to avoid internal copying in Native_socket_stream internals.
 *   - The `should_interrupt_func()` feature was necessary because Native_socket_stream internals has a condition
 *     where it is obligated to stop writing to the user buffer and return to them an overall in-pipe error:
 *     - idle timeout (no in-traffic for N time).
 *     - But the whole point of `async_read()` and therefore the present extension is to keep reading until all N
 *       bytes are here, or an error.  The aforementioned condition is, in a sense, the latter: an error; except
 *       it originates locally.  So `should_interrupt_func()` is a way to signal this.
 * - As noted, this sets non-blocking mode on `*peer_socket` if needed.  It does not "undo" this.  Why not?
 *   After all a clean op would act as if it has nothing to do with this.  Originally I (ygoldfel) did "undo" it.
 *   Then I decided otherwise for 2 reasons.  1, the situation where the "undo" itself fails made it ambiguous how to
 *   report this through the handler if everything had worked until then (unlikely as that is).  2, in coming up
 *   with a decent semantic approach for this annoying corner case that'll never really happen, and then thinking of
 *   how to document it, I realized the following practical truths:  `Peer_socket::non_blocking()` mode affects only
 *   the non-`async*()` ops on `Peer_socket`, and it's common to want those to be non-blocking in the first place,
 *   if one also feels the need to use `async*()` (like the present function).  So why jump through hoops for purity's
 *   sake?  Of course this can be changed later.
 *
 * @tparam Task_err_blob
 *         See `on_rcvd_or_error` arg.
 * @tparam Target_payload_blob_func
 *         See `target_payload_blob_func` arg.
 * @tparam Should_interrupt_func
 *         See `should_interrupt_func` arg.
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param peer_socket
 *        Pointer to stream socket.  If it is not connected, or otherwise unsuitable, behavior is identical to
 *        attempting `async_read()` on such a socket.  If null behavior is undefined (assertion may trip).
 * @param on_rcvd_or_error
 *        Handler to execute at most once on completion
 *        of the async op.  It is executed as if via `post(peer_socket->get_executor())`, fully respecting
 *        any executor bound to it (via `bind_executor()`, such as a strand).
 *        It shall be passed, in this order, `Error_code` and a value equal to the one returned by
 *        `target_payload_blob_func()` earlier.  The semantics of the first value is identical to that
 *        for `boost::asio::async_read()`.
 * @param target_payload_blob_func
 *        Function to execute at most once, when it is
 *        first indicated by the system that data might be available on the connection.  It is executed as if via
 *        `post(peer_socket->get_executor())`, but any executor bound to it (via `bind_executor()` is ignored).
 *        It takes no args and shall return `util::Blob_mutable` object
 *        describing the memory area into which we shall write on successful receipt of data.
 *        It will not be invoked at all, among other reasons, if `should_interrupt_func()` returns `true` the
 *        first time *it* is invoked.
 * @param should_interrupt_func
 *        Function to execute ahead of nb-reading arriving data (copying it from kernel buffer to
 *        the target buffer); hence the general loop is await-readable/call-this-function/nb-read/repeat
 *        (until the buffer is filled, there is an error, or `should_interrupt_func()` returns `true`).
 *        It is executed as if via `post(peer_socket->get_executor())`, but any executor bound to it (via
 *        `bind_executor()` is ignored).  It takes no args and shall return
 *        `bool` specifying whether to proceed with the operation (`false`) or to interrupt the whole thing (`true`).
 *        If it returns `true` then async_read_with_target_func() will *not* call `on_rcvd_or_error()`.
 *        Instead the user should consider `should_interrupt_func()` itself as the completion handler being invoked.
 */
template<typename Task_err_blob, typename Target_payload_blob_func, typename Should_interrupt_func>
void async_read_with_target_func
       (flow::log::Logger* logger_ptr,
        Peer_socket* peer_socket,
        Target_payload_blob_func&& target_payload_blob_func,
        Should_interrupt_func&& should_interrupt_func,
        Task_err_blob&& on_rcvd_or_error);

/**
 * boost.asio extension similar to
 * `boost::asio::async_read(Peer_socket&, Blob_mutable, Task_err_sz)` with the difference that it can be
 * canceled/interrupted via `should_interrupt_func()` in the same way as the otherwise more
 * complex async_read_with_target_func().
 *
 * So think of it as either:
 *   - `async_read()` with added `should_interrupt_func()` feature; or
 *   - `async_read_with_target_func()` minus `target_payload_blob_func()` feature.  Or formally, it's as-if
 *     that one was used but with a `target_payload_blob_func()` that simply returns `target_payload_blob`.
 *
 * Omitting detailed comments already present in async_read_with_target_func() doc header; just skip the
 * parts to do with `target_payload_blob_func()`.
 *
 * @tparam Task_err
 *         See `on_rcvd_or_error` arg.
 * @tparam Should_interrupt_func
 *         See async_read_with_target_func().
 * @param logger_ptr
 *        See async_read_with_target_func().
 * @param peer_socket
 *        See async_read_with_target_func().
 * @param on_rcvd_or_error
 *        The completion handler; it is passed either the success code (all requested bytes were read),
 *        or an error code (pipe is hosed).
 * @param target_payload_blob
 *        `util::Blob_mutable` object
 *        describing the memory area into which we shall write on successful receipt of data.
 * @param should_interrupt_func
 *        See async_read_with_target_func().
 */
template<typename Task_err, typename Should_interrupt_func>
void async_read_interruptible
       (flow::log::Logger* logger_ptr, Peer_socket* peer_socket, util::Blob_mutable target_payload_blob,
        Should_interrupt_func&& should_interrupt_func, Task_err&& on_rcvd_or_error);

/**
 * Little utility that returns the raw Native_handle suitable for #Peer_socket to the OS.
 * This is helpful to close, without invoking a native API (`close()` really), a value returned by
 * `Peer_socket::release()` or, perhaps, received over a `Native_socket_stream`.
 *
 * The in-arg is nullified (it becomes `.null()`).
 *
 * Nothing is logged; no errors are emitted.  This is intended for no-questions-asked cleanup.
 *
 * @param peer_socket_native_or_null
 *        The native socket to close.  No-op (not an error) if it is `.null()`.
 *        If not `.null()`, `peer_socket_native_or_null.m_native_handle` must be suitable for
 *        `Peer_socket::native_handle()`.  In practice the use case informing release_native_peer_socket()
 *        is: `peer_socket_native = p.release()`, where `p` is a `Peer_socket`.  Update:
 *        Another use case came about: receiving `peer_socket_native` over a `Native_socket_stream`.
 */
void release_native_peer_socket(Native_handle&& peer_socket_native_or_null);

} // namespace ipc::transport::asio_local_stream_socket
