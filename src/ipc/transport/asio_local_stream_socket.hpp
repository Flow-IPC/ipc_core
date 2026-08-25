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

#include "ipc/transport/detail/asio_local_stream_socket.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <flow/log/log.hpp>
#include <flow/common.hpp>
#include <boost/array.hpp>
#include <boost/io/ios_state.hpp>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <sys/types.h>
#include <sys/socket.h>

#ifndef FLOW_OS_LINUX // Sanity-re-check.  We'll be sending sockets through sockets, etc., which requires Linux.
static_assert(false, "Should not have gotten to this line; should have required Linux; this header assumes it.  "
                       "Might work in other POSIX OS (e.g., macOS) but must be checked/tested.");
#endif

// See asio_local_stream_socket_fwd.hpp for doc header (intro) to this namespace.
namespace ipc::transport::asio_local_stream_socket
{

// Types.

#ifndef FLOW_OS_LINUX
static_assert(false, "asio_local_stream_socket::Msg_batch_in is based on Linux-specific recvmmsg() API.");
#endif

/**
 * Similarly (but formally speaking not identically) to the `Msg_batch_in` concept (as taken by API
 * `{Blob|Native_handle}_receiver::async_receive_*_batch()` concept) implements persistent-across-reads
 * batched-receipt of messages over an `asio_local_stream_socket::Peer_socket<Protocol_pkt_stream>`.
 * There is a heavy focus on performance, including across multiple batched receives (nb_read()) on the same
 * `*this` without linear-time prep work ahead of each such receive op.
 *
 * @see Native_handle_receiver concept doc header for an explanation of receive-batching (including what
 *      native batching is; how `Msg_resource_t` template-param relates to things); and how the general design
 *      enables high perf.
 *
 * ### Rationale ###
 * Formally speaking this is a stand-alone class template, as opposed to an implementation of any particular
 * concept.  (See Native_socket_stream_msg_batch_in for the `Msg_batch_in` concept impl for
 * Native_socket_stream.  As noted below it in fact uses a `*this` most centrally in its impl.)  Put simply,
 * it (1) prepares the data structures needed for -2-; (2) wrangles `recvmmsg()` or equivalent; and (3) maintains
 * the aforementioned data structures, so that repetitions of -2- can be done with almost zero additional
 * work (and certainly not linear in batch-size).  In order to do something fancier, such as actually being
 * suitable as main arg `batch` to sync_io::Native_socket_stream::async_receive_blob_batch(), it would need to
 * speak a more advanced protocol rather than what it does speak... which is simply "receive whatever arrives
 * and save it for the user to access."  So why is it a public API?  Answer: For the same reason, e.g.,
 * nb_read_some_with_native_handle() (and other public members of the ipc::transport::asio_local_stream_socket
 * namespace) are public: It may well prove useful for low-level work directly on raw local sockets.
 * (That said, informally, we expect most users to use the somewhat higher-level Native_socket_stream
 * instead -- or something even higher-level such as struc::Channel.)
 *
 * Please note that we have intentionally made the receiver-engine-facing API, namely nb_read() and
 * reuse_result_payloads(), publicly accessible, again due to the above reasoning.  Again the idea
 * is that conceivably the Flow-IPC user could make use of it,
 * as it is fairly general.  As of this writing, however, only other Flow-IPC internal code (namely
 * sync_io::Native_socket_stream) uses this sub-API; but that need not be the case.
 *
 * ### Details ###
 * As of this writing this is internally done, specifically, via Linux-specific `recvmmsg()` API operating on a
 * `SOCK_SEQPACKET` connection (hence the aforementioned #Protocol_pkt_stream).  When/if support for non-Linux is
 * added, this class (or hypothetical similar one) could support any equivalent from other OS.
 *
 * This implementation is fairly general, down to allowing an arbitrary (albeit mandatorily consistent across a given
 * `*this` lifetime) #Mutable_buffer_sequence type.  (E.g., could provide a simple util::Blob_mutable
 * or an `array<Blob_mutable>` or `vector` thereof, etc.)  The main limitation is one shared with the rest
 * of transport::asio_local_stream_socket API -- as informed by the needs of the rest of Flow-IPC -- in that
 * in addition to arbitrary binary data blobs it supports the transmission of `Native_handle`s, namely either zero
 * or one such items per in-message.  As usual a given in-message may contain 0+ bytes of blob data and 0 or 1 handles;
 * but never 0 bytes and 0 handles (this is treated as a fatal error).
 *
 * It features the following APIs as of this writing:
 *
 *   - For user (including internal user struc::sync_io::Channel): `size_t max_msg_count`-taking ctor,
 *     initialized(), n_used(), full(), clear_used(), prepare_target_payload(), result_payload_blob(),
 *     result_payload_hndl() (for Native_handle_receiver only).
 *   - For higher-level receiver engine (such as sync_io::Native_socket_stream + Native_socket_stream_msg_batch_in
 *     working in concert): nb_read(), reuse_result_payloads().
 *     - The expected order of ops for each batched-read (after reaching initialized() originally) is:
 *       -# (Usually) Higher-level user invokes Blob_receiver::async_receive_blob_batch() or
 *          Native_handle_receiver::async_receive_native_handle_batch() which internally invokes:
 *          -# Core batch-receive: nb_read().
 *          -# Filter-out control messages without destroying them: reuse_result_payloads()
 *             (if applicable depending on your higher-level protocol if any).
 *             - Harvest any control-message info among the filtered-out in-messages.
 *       -# User consumes results: result_payload_blob() and possibly result_payload_hndl().
 *       -# User resets for next batch-read:
 *          -# Save N = n_used().
 *          -# clear_used().
 *          -# prepare_target_payload() (for each of the first N slots only).
 *
 * The algorithmic centerpieces of a `*this` are as follows.  Note that, as usual in this context, this info is
 * of interest not to the eventual user doing higher-level receiving via Blob_receiver and the like; but implementers
 * of specific `Blob_receiver`s (et al).  So we have:
 *
 *   - nb_read() which internally performs `recvmmsg()` (or equivalent op in other OS).
 *     - prepare_target_payload() and result_payload_blob() et al interact with it to maintain high perf.
 *   - reuse_result_payloads() which helps (at least Native_socket_stream_msg_batch_in but conceivably others)
 *     support higher-level protocols with both user in-messages and control messages (e.g., auto-ping, graceful-close)
 *     while maintaining high perf.
 *
 * ### Regarding structure of each target-blob area(s) user supplies to prepare_target_payload() ###
 * You can skip this section if you promise to, in every prepare_target_payload() call for a given `*this`,
 * supply the same number of scatter/gather buffers of the same respective lengths.  (E.g.: always a 2-buffer and
 * a 4096-buffer.)
 *
 * In general you *can* provide a different structure (e.g. 1 2-byte buffer and 3 1-byte buffers; or just 1 7-byte
 * buffer; or...) to each prepare_target_payload() versus the others.  It is conceivable to make use of this for
 * some real purpose.  However:
 *
 *   - Informally: one typically doesn't know (at this level) what's coming, so it is probably correct to provide, at
 *     least, the same total number of target bytes for each prepare_target_payload().  It is also likely more
 *     natural to have the same # and length-order of scatter/gather buffers across which to spread these bytes;
 *     though technically doing otherwise could be fine (just... why bother?).
 *   - This is even more-so if you plan to use reuse_result_payloads() which shall potentially swap slot order
 *     which would probably make keeping-up with what happened hard-to-impossible, unless each slot is structurally
 *     identical to all others.
 *
 * @internal
 *
 * ### Impl notes ###
 * Generally the details here are informed by the somewhat higher-level needs of Native_socket_stream and
 * Native_socket_stream_msg_batch_in which implement the protocol used by the former.  As such that protocol is
 * close to -- but not quite the same as -- simply supporting arbitrary in-datagrams (each with an optional
 * native-handle), which is what a `*this` supplies.
 *
 * The devil is really in the details, while being driven by the basic receive-batching design described in
 * Native_handle_receiver concept doc header.  The basic point is, after invoking ctor and N prepare_target_payload(),
 * subsequent per-batched-read (nb_read()) are not linear-time, so that:
 *
 *   - each actually received-to slot will understandably need processing;
 *   - but all others require little to no reset or post-processing.
 *
 * Thus we get the benefits of receiving potentially many in-messages via 1 OS-call; while requiring ~no extra
 * compute when compared with a conventional non-batched series of N single-message-receives-until-would-block.
 *
 * `recvmmsg()` makes this achievable, though far from trivial.  To summarize:
 *
 *   - `recvmmsg()` takes an array of `mmsghdr`, each of which contains an `msghdr` (identical to what `recvmsg()`
 *      takes one of) and `.msg_len`; and returns the number of `mmsghdr`s that were actually received-to;
 *      while each individual `.msg_len` holds the # of bytes received for that in-message (identical to what
 *      `recvmsg()` would have returned).
 *     - The `msghdr` has a pointer/length w/r/t a scatter/gather array of `iovec` structures in heap, each storing
 *       a location/size, being conceptually identical to our util::Blob_mutable.
 *     - It also has a pointer/length w/r/t a complex *ancillary data* structure in heap (we use it for 0-1 FDs).
 *   - If a given slot was not read-to by a `recvmmsg()`, then its `mmsghdr` array entry is untouched and can be
 *     reused as-is.
 *   - If a given slot *was* read-to, but via reuse_result_payloads() it is determined that it contained no
 *     user in-message but merely what the receiver-engine considers a control/metadata in-message -- not to be
 *     passed to the user as such -- then that `mmsghdr` (and related) array entry can also be reused as-is... almost.
 *     A couple of items do need to be set again; you'll see.  In any case these should be very cheap and relatively
 *     rare anyway.
 *     - If it was read-to, and it was indeed a user in-message, then naturally prepare_target_payload() has to be
 *       re-invoked.  However you'll see that even this requires only a certain subset of the work compared to
 *       the one-time initializing prepare_target_payload().
 *   - For our part we make use of this as follows:
 *     - We store the array of `mmsghdr`s, which is continually received-to, re-primed, received-to, re-primed, ....
 *       This is #m_mmsg_hdrs, a `vector`.
 *     - All other info, including the aforementioned `iovec`-array and ancillary-data
 *       `mmsghdr`-member-pointer-pointees, is stored in #m_mdts, a structurally identical `vector` (same
 *       size, capacity, and index order as `m_mmsg_hdrs`).
 *     - So slot indexed `idx` exactly consists of the two `struct`s `m_mmsg_hdrs[idx]` and `m_mdts[idx]`.
 *     - User's prepare_target_payload() calls sets them up until initialized() is true.
 *     - nb_read() and reuse_result_payloads() read into the first N slots, where N = # of pending messages capped
 *       at total # of slots we store.
 *     - User can consume any received-to slot `idx`'s data and then re-prepare that slot by supplying new target memory
 *       area(s).  They do this by calling `prepare_target_payload(..., idx)`.
 *     - Any slots that were untouched by nb_read() or reuse_result_payloads() simply carry-over to the next nb_read().
 *       They need not be touched at all.
 *
 * @endinternal
 *
 * @tparam Msg_resource_t
 *         Same as for Generic_msg_batch_in.  However, caution!  Be very sure that whatever is stored here, if it
 *         provides the backing bytes for the #Mutable_buffer_sequence passed to the corresponding
 *         prepare_target_payload(), does *not* change location in memory if the `Msg_resource_t` is *moved*
 *         onto another `Msg_resource_t`.  For example, this *does* hold for `flow::util::Blob` or `vector` or
 *         `unique_ptr<int>`; but *not* for `array` or `int`.
 * @tparam Mutable_buffer_sequence_t
 *         See #Mutable_buffer_sequence.
 */
template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
class Msg_batch_in :
  private boost::noncopyable
{
public:
  // Types.

  /// Buffer(s) description type (conforming to boost.asio `MutableBufferSequence` concept) supported by `*this`.
  using Mutable_buffer_sequence = Mutable_buffer_sequence_t;

  /// Alias for template param `Msg_resource_t`.
  using Msg_resource = Msg_resource_t;

  // Constructors/destructor.

  /**
   * Identical to ctor of Generic_msg_batch_in.
   * @param max_msg_count
   *        See above.
   */
  explicit Msg_batch_in(size_t max_msg_count);

#ifdef IPC_DOXYGEN_ONLY
  /// Clears all resources -- including any #Msg_resource currently attached to any receive-slot.
  ~Msg_batch_in();
#endif

  // Methods.

  /**
   * Identical to Generic_msg_batch_in.
   * @return See above.
   */
  bool initialized() const;

  /**
   * In range [0, `max_msg_count`] (see ctor), this is the number of slots, starting with the 1st slot (`[0]`),
   * currently marked as storing a received message each.
   *
   * @note result_payload_blob() and similar result-accessors *do* take `idx >=` this value
   *       (cf. Generic_msg_batch_in).  That is because of the possibility of reuse_result_payloads(); see that
   *       guy.
   *
   * It is meaningless until `initialized() == true`; incremented by nb_read(); decremented by reuse_result_payloads();
   * and reset to zero by clear_used().
   *
   * @return See above.
   */
  size_t n_used() const;

  /**
   * Identical to Generic_msg_batch_in.  That doc header is worth reading; especially see notes
   * on *implied would-block*.
   *
   * @return See above.
   */
  bool full() const;

  /**
   * Makes it so that n_used() would return zero (or, in advanced form, another value).  Typically used after an
   * nb_read() before the next one.
   *
   * @param new_n_used
   *        The value n_used() shall return.
   */
  void clear_used(size_t new_n_used = 0);

  /**
   * Identical to Generic_msg_batch_in (though the type of `target_blob` is *potentially* more complex).
   *
   * @param target_blob
   *        See above.
   * @param msg_resource
   *        See above.
   * @param idx
   *        See above.
   */
  void prepare_target_payload(const Mutable_buffer_sequence& target_blob, Msg_resource&& msg_resource,
                              size_t idx = -1);

  /**
   * Obtains information, chiefly the size of received data `<=` total-size of corresponding `target_blob`
   * from prepare_target_payload() but also optionally pointer to attached #Msg_resource, about an earlier-received-to
   * slot.
   *
   * The method is `const` (non-mutating) unless optional `msg_resource_ptr` arg is supplied (non-null).
   *
   * @param idx
   *        Which slot?  Nota bene: this method *does* allow `idx >= this->n_used()`
   *        (cf. Generic_msg_batch_in).  Probably: `idx < this->n_used()` when scanning messages yielded by
   *        nb_read() and *not* "un-yielded" by reuse_result_payloads() (if any); and `idx >= this->n_used()`
   *        when scanning the messages (if any!) "un-yielded" by reuse_result_payloads() (if any).  In the latter
   *        case presumably `idx` will not exceed the last such "un-yielded" message's index; but this is not
   *        detected let alone enforced.  Use with care.
   * @param msg_resource_ptr
   *        If not null then pointee `*msg_resource_ptr` is set to point to the #Msg_resource stored into this
   *        slot by the last prepare_target_payload().  It is typical to then move-away `move(**msg_resource_ptr)`
   *        into a Msg_resource object maintained by the caller.
   * @return Byte count of the message last received-to the `idx`th slot.
   */
  size_t result_payload_blob(size_t idx, Msg_resource** msg_resource_ptr = nullptr);

  /**
   * Obtains a copy of the `Native_handle` (potentially `.null()`) in an earlier-received-to slot.
   *
   * @internal
   *
   * @todo Only copy-access to a slot's stored `Native_handle` is provided: Msg_batch_in::result_payload_hndl(); a
   * receiver-engine rewinding n_used() (un-emitting slots) can thus close a stored handle (via the copy) but not
   * nullify the stored value, leaving a stale (closed) handle value recorded in a meaningless-territory slot.  For
   * cleanliness/defensiveness an engine-facing API -- e.g., close-and-nullify over an index range -- might be
   * helpful or required; see its would-be use in Native_socket_stream_msg_batch_in::nb_read() rewind logic.
   * Suggest also applying identical changes to `Generic_msg_batch_in` API, `async_receive_batch_emulation()`
   * doc header documenting the `tparam Batch`, and accordingly the rewind logic inside
   * `async_receive_batch_emulation_on_init_msg()`.  Both rewind spots as of this writing are marked with
   * associated to-dos.
   *
   * @endinternal
   *
   * @param idx
   *        See result_payload_blob().
   * @return See above.
   */
  Native_handle result_payload_hndl(size_t idx) const;

  /**
   * Performs a high-perf non-blocking batched-receive into the batch area with indices
   * [n_used(), `max_msg_count`).  See ctor for the latter.  Usually, but not necessarily, `this->n_used() == 0`,
   * as usually one would clear_used() first, or `*this` was just initialized().  Returns `false` if
   * the aforementioned range is empty -- that is full() is `true` and no-ops modulo logging; otherwise returns
   * `true` and proceeds.  Further documentation assumes the latter.
   *
   * Let `n_payloads` be the size of the aforementioned range (thus `n_payloads >= 1`).  This receives
   * at most `n_payloads` messages.  If no error but no in-messages are pending, it is would-block
   * (see specific code below).  If no error, and messages were pending, the number is not explicitly returned
   * but rather reflected in n_used() increasing by that # (at least 1, at most `n_payloads`).  If error,
   * and no messages were pending, emits that error and leaves n_used() unchanged.
   *
   * It is *not* a possible outcome that messages were emitted (n_used() changed) *and* an error is emitted.
   * See section "Deferred errors" just below however.
   *
   * @note Be aware of the *implied would-block* condition.  See full() doc header.  Spoiler alert:
   *       if we yielded data/no error, then it wasn't would-block... but if `full() == false` post-op, then
   *       the in-pipe is nevertheless in would-block state.  It would be wasteful to try another nb_read().
   *       Conversely, though, if `full()` then you can and should nb_read() again (assuming the goal is to
   *       drain the in-pipe such as in edge-triggered loops).
   *
   * ### Special graceful-close (a/k/a EOF) semantics ###
   * nb_read_some_with_native_handle(), a-la Boost, emits `boost::asio::error::eof` on encountering graceful-close
   * in the message stream -- never combined with emitting an in-message as well; so in a sense that "error" is
   * an in-message itself (never followed by any more in-messages).  This nb_read() never emits that code however.
   *
   * Instead: If and only if there is no error emitted (including would-block), and any pending in-messages (0 or more
   * of them) are capped with graceful-close, then `*graceful_close` is set to `true`; else to `false`.  The
   * graceful-close "in-message" thus is treated semantically as an in-message and is always emitted if and only if it
   * is pending.  To recap the possible outcomes:
   *
   *   - Socket-hosing error or `INVALID_ARGUMENT`: emitted via standard flow error semantics.
   *     - Subtle exception: error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE and
   *       error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL are not socket-hosing.  Further I/O
   *       can be attempted.  However:
   *       - In-direction: Informally, it is ill-advised to continue work in this direction.  In-messages potentially
   *         following the overflowing one, in this batch, would have been dropped.  In most protocols it would be
   *         difficult to continue from this point.
   *       - Out-direction: Work really can continue.  Informally, though, it might be ill-advised to take advantage
   *         of this -- but less so than in the previous bullet point.
   *   - No in-message(s) and no graceful-close pending: emitted via those same semantics as
   *     `boost::asio::error::would_block`.
   *   - Otherwise, at least 1 of (in-message, graceful-close) is pending.  So:
   *     0+ in-message(s) are emitted into `*this` (n_used() increases if 1+), and `*graceful_close` is assigned
   *     to `true` if graceful-close capped those 0+ in-messages; to `false` otherwise.
   *     - So: It is not possible that there is no error, n_used() did not increase (0 in-messages), *and*
   *       `*graceful_close = false` is set.
   *
   * @note Graceful-close, if detected (and it will be if indeed pending), is never deferred to the next nb_read().
   *       This is potentially important, if you're running an edge-triggered-poll (e.g., `EPOLLET` with `epoll_*()`)
   *       event loop.  If we were to defer reporting a pending graceful-close, you'd likely invoke `epoll_wait()` (or
   *       equivalent), and it will probably hang, as the *readability edge* has already been consumed.
   *       (We've empirically confirmed this in Linux.)
   *
   * ### Edge-triggered-poll friendliness ###
   * A key guarantee, for those who run an edge-triggered-poll event loop (e.g., `EPOLLET` with `epoll_*()`), is that
   * *all* pending in-messages (counting graceful-close as in-message) are flushed into `*this` (and `*graceful_close`),
   * up to `full() == true`.  Thus, if you nb_read() repeatedly until would-block is emitted, or `!full()`
   * (implied would-block), or socket-hosing error, and then await (edge-triggered) readability again, all will work
   * well.
   *
   * ### Deferred errors: Advanced discussion ###
   * (Some of this references implementation details.  In this advanced context we deem it appropriate.)
   *
   * We've noted that we will never emit a
   * (socket-hosing / `MESSAGE_SIZE_EXCEEDS_USER_STORAGE` / `LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL` /
   * `INVALID_ARGUMENT`, non-would-block, non-graceful-close)
   * error, if we do emit 1+ in-messages (let's count `*graceful_close = true` as an in-message for this discussion).
   * What happens, though, if the socket has 1+ in-messages pending *and* an error following those in-messages?  In
   * Linux, as of this writing, at least in some mainstream versions, the `man recvmmsg` page says that in this case (it
   * is even marked under "BUGS," probably out of caution more than anything) the error is not emitted; but any
   * following `recv*()` call will emit the pending error.  In short: we simply report (in this context) what the OS
   * does, so if that happens, we'll simply emit the same stuff (in fact we have no good way of knowing it's happening,
   * rather than the error simply happening a bit later).
   *
   * Not so fast, though: This opens the possibility of a subtle problem.  To wit: if the OS knows an error is pending
   * after 1+ in-messages, and `recvmmsg()` ignores it, but the next `recv*()` will report the error, then can't this
   * break the edge-triggered-poll-event-loop-friendly semantics, wherein *all* in-data must be flushed before
   * invoking another await-readability OS-call (`epoll_wait()` in Linux)?  That is, an `epoll_wait()` for readability
   * would hang in such a hypothetical scenario (if edge-triggered, `EPOLLET`; level-triggered is always safe).
   *
   * In short: As of this writing, in the case of Linux, it is not a problem in practice.  So don't worry about it...
   * but read on if you want to understand why/what is happening.
   *
   * Essentially it is not a problem simply because, in our case of dgram-based local IPC over Unix-domain sockets,
   * specifically when reading, there simply *is* no error condition that can be "queued" after in-messages.
   * Graceful-close is possible -- but we handle that, and it's not (internally in Linux) an error.  `ECONNRESET`
   * is not a thing in this context; `EPIPE` is for writing (not reading); others catastrophic problems such as
   * `EBADF` are not "queued."  With TCP, `ECONNRESET` would indicate an RST, but it's not a thing for us.  With UDP,
   * some sort of ICMP wackiness could maybe pop up -- but we aren't networking, and it's not UDP.
   *
   * That said, it is important to keep an eye on this (as Linux develops and/or we add other OS support); semantics
   * could differ there.  Even in Linux, we *did* empirically confirm that in general the situation wherein a
   * socket is in queued-error state, and `recv*()` would yield an error (namely `ECONNRESET` due to queued RST
   * intentionally sent by opposing side), then an `epoll_wait()` for readability, with `EPOLLET`, will hang and not
   * re-report readability despite the error-state of the socket.  Again, though, we could only force this in TCP, so
   * it's not immediately relevant -- but it is conceivable it could become relevant.  E.g., this code could be
   * extended to newer Linux that act differently and/or other OS that act differently and/or support networked
   * sockets (perhaps UDP).
   *
   * @param logger_ptr
   *        Logger (or null) to use for logging inside.
   * @param peer_socket_ptr
   *        Peer socket over which to receive.
   * @param graceful_close
   *        If no error is emitted, and we return `true`, then `*graceful_close` is assigned `true` if the 0+
   *        messages received (as reflected via n_used()) were followed by graceful-close in the in-message stream;
   *        to `false` if the 1+ messages received were not followed by graceful-close.  Untouched otherwise.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  Generated codes:
   *        error::Code::S_INVALID_ARGUMENT (`!this->initialized()` at entry);
   *        `boost::asio::error::would_block` (socket not readable: not even 1 message or graceful-close pending);
   *        those emitted by nb_read_some_with_native_handle()
   *        (including error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE)
   *        (except would-block which has different meaning as noted; and
   *        `boost::asio::error::eof` (graceful-close) which is communicated via
   *        `graceful_close` arg as noted).
   * @param message_flags
   *        See nb_read_some_with_native_handle().
   * @param total_rcvd_bytes_or_null
   *        If not null, `*total_rcvd_bytes_or_null` is set to the sum of bytes received (into user buffers) across all
   *        received datagrams.  Useful for stats: this is total low-level bytes including all scatter/gather segments.
   *        Meaningless on error or would-block (left unset).
   * @return `false` if pre-condition `!this->full()` is failed; `true` otherwise.  See above.
   */
  bool nb_read(flow::log::Logger* logger_ptr, Peer_socket<Protocol_pkt_stream>* peer_socket_ptr, bool* graceful_close,
               Error_code* err_code = nullptr, int message_flags = 0,
               size_t* total_rcvd_bytes_or_null = nullptr);

  /**
   * Scans every in-message slot in range [`start_idx`, `this->n_used()); performantly rearranges this range
   * so that all in-messages for which `is_unused_func(M) == false` (if any + without mutual order changing) are
   * at the start of the range, followed by the other ones (if any; no ordering guarantees); and adjusts
   * (no-op or decreases) n_used() by the # of the `is_unused_func(M) == true` slots detected.
   *
   * @see Msg_batch_in class doc header for rationale/background/discussion.  In short, though, the idea is
   *      `is_unused_func(M) == false` indicates a "user in-message" (worthy of remaining within the
   *      0-to-`n_used()` range), while the converse indicates a control/metadatum message (which should
   *      be moved past the "used" range).
   *
   * Conceptually this is similar to `std::remove_if()`, except swaps -- instead of mere moves -- are used, thus
   * ensuring the slots potentially change relative order, but the set of slots by value is unchanged; and
   * n_used() changes instead of a returned iterator are the mechanism of communicating how many items are
   * "filtered out."
   *
   * ### What to do with the filtered-out slots? ###
   * One option is to do nothing.  You can nb_read() again; these slots will be readable-into; no problem.
   * So then you'd just consume the ones up to `this->n_used()` as normal, and that's that... ready to nb_read()
   * again.
   *
   * Otherwise -- namely if the filtered-out messages potentially carry useful information, or something --
   * you can do the first or possibly both of the following.
   *
   *   - Scan them (even though they're past the first `n_used()` slots) via `result_payload_*()`.
   *   - Consume and re-prepare (prepare_target_payload()) them.
   *
   * Bottom line is, you can (but need not) do stuff to them just as you can do to the not-unused ones.  Have care.
   *
   * @param start_idx
   *        Index (0-based) of first message to scan (see above).  Typically this would be what n_used() returned
   *        before the preceding nb_read().  (Fittingly the last message to scan is indicated by
   *        current/post-`nb_read()` value of also n_used().)
   * @param is_unused_func
   *        See above.  We suggest this is written to be as performant as humanly possible.  E.g. try to find
   *        the cheapest-and-likeliest conditions first and thus early-return whenever possible.
   */
  template<typename Is_unused_func>
  void reuse_result_payloads(size_t start_idx, const Is_unused_func& is_unused_func);

  /**
   * Identical to Generic_msg_batch_in.
   * @param os
   *        See above.
   */
  void to_ostream(std::ostream* os) const;

private:
  // Types.

  /// Per-batch-slot information beyond what is stored in the corresponding slot's native `mmsghdr m_mmsg_hdrs[idx]`.
  struct Mdt_per_payload
  {
    // Data.

    /**
     * Efficient (via adapt-as-we-go technique) object that adapts the boost.asio portable #Mutable_buffer_sequence
     * (sequence of boost.asio util::Blob_mutable location/size structures) into the POSIX native `iovec` array
     * containing the exact same information.
     *
     * The implementation is cleverly specialized, so that depending on what concrete type #Mutable_buffer_sequence
     * actually is, the storage and access to the resulting `iovec`s is as performant as possible.  In particular
     * there are specializations for a single `Blob_mutable`; STL and Boost `array<2>` thereof; and (worst-case)
     * variable-length things such as `vector<Blob_mutable>`.  E.g., in Flow-IPC a major use-case is
     * Native_socket_stream_msg_batch_in which in fact uses `Mutable_buffer_sequence = array<Blob_mutable, 2>`.
     *
     * ### Defense of using an internal (`detail`) boost.asio class ###
     * Well, it's useful.  We could roll "our own" by copy/pasting it instead, but in practice that's just
     * busy-work.  That said there is some risk of this changing under us with a newer Boost version; we accept this
     * small risk.
     */
    boost::asio::detail::buffer_sequence_adapter<util::Blob_mutable, Mutable_buffer_sequence> m_buf_seq_adapter;

    /// Per-slot resource moved from user's object via `prepare_target_payload()`.
    Msg_resource m_resource;

    /**
     * The target/result `Native_handle` to receive-to for this slot; null originally; null or not upon
     * a receive op into the slot.
     */
    Native_handle m_result_hndl;

    /**
     * Stores this slot's ancillary-data `cmsg*` structure, into which ancillary data if any are received.
     *
     * This must be the last member to avoid gcc error "flexible array member 'cmsghdr::__cmsg_data' not at end of ...";
     * though we feel in our case it is safe due to measures taken inside Msg_control_as_union.
     */
    Msg_control_as_union m_msg_control_as_union;

    // Constructors/destructor.

    /**
     * Ctor.
     * @param buf_seq
     *        The buffers.
     * @param msg_resource
     *        Object to move-assign to #m_resource.
     */
    Mdt_per_payload(const Mutable_buffer_sequence& buf_seq, Msg_resource&& msg_resource);

    // Methods.

    /**
     * Implements maximally efficient swapping, as needed by reuse_result_payloads().  The algorithm is very much
     * specific to Msg_batch_in; it would be insufficient if used in some generic sense outside that context; so
     * e.g. it assumes certain values *in our case* never change after prepare_target_payload().
     *
     * @param that
     *        Other guy to swap-with; does not equal `this`.
     */
    void swap(Mdt_per_payload* that);
  }; // struct Mdt_per_payload

  // Constants.

  /// Value we give for `msghdr::msg_flags` as in-arg.  It is also an out-arg, so sometimes we must reassign this.
  static constexpr int S_PER_MSG_FLAGS = 0;

  // Data.

  /**
   * The slots, excluding what is in #m_mmsg_hdrs.  `.capacity() == max_msg_count` (see ctor); `.size()` is same
   * once initialized().
   *
   * ### Rationale: perf ###
   * Note we store the `Mdt_per_payload`s "directly" in the `vector<>` as opposed to wrapping in `unique_ptr`
   * handles or similar.  (Similarly we don't wrap any member(s) inside in `unique_ptr` either.)  Why?  Answer:
   * For perf... but how so?  Answer: added heap-allocation and indirection (mostly the former) are relatively costly.
   * On the other hand any moving of elements inside the `vector` would counteract this (somewhat, depending).  Do
   * we actually do that?  We use `.reserve()` and a constant `.capacity()`, so with just prepare_target_payload()
   * init and nb_read(), no, we don't move these elements as of this writing, with two exceptions.  1, there's
   * the initial load via prepare_target_payload().  2, *if* they choose to reuse_result_payloads(), then we do
   * sometimes move (swap) them (albeit relatively rarely in practice).  (Also the swap compute itself is fairly
   * painstakingly minimal (see reuse_result_payloads()).)
   *
   * All in all, on researching this in some detail, it is cheaper than adding heap-indirection, particularly if
   * #Msg_resource is efficiently-movable (usually it is: usually it'll be like a `vector<uint8_t>` move or cheaper).
   *
   * ### Corollary ###
   * Because we made this choice, *and* because we take `Msg_resource&&` (not a handle thereto) in
   * prepare_target_payload(), we add the requirement that any backing data structures provided by the user
   * as #Msg_resource objects are such that their locations do *not* change when a `Msg_resource` is moved,
   * if reuse_result_payloads() is ever used.  See class doc header's template param docs, where we announce this
   * requirement.
   */
  std::vector<Mdt_per_payload> m_mdts;

  /**
   * The slots, excluding what is in #m_mdts; a sub-array of this is passed by nb_read() to `recvmmsg()`.
   * `.capacity()`, `.size()` shall always equal those of #m_mdts; and a given index in one refers to the same slot
   * in the other.
   */
  std::vector<::mmsghdr> m_mmsg_hdrs;

  /**
   * See n_used().  Restating for informational convenience: meaningless until `initialized() == true`; incremented by
   * nb_read(); decremented by reuse_result_payloads(); and reset to zero by clear_used().
   */
  size_t m_n_used;
}; // class Msg_batch_in

#ifndef FLOW_OS_LINUX
static_assert(false, "Flow-IPC must define Opt_peer_process_credentials w/ Linux SO_PEERCRED semantics.  "
                       "Build in Linux only.");
#endif

/**
 * Gettable (read-only) socket option for use with asio_local_stream_socket::Peer_socket `.get_option()` in order to
 * get the connected opposing peer process's credentials (PID/UID/GID/etc.).  Note accessors and data are
 * in non-polymorphic base util::Process_credentials.
 *
 * If one calls `X.get_option(Opt_peer_process_credentials& o)` on #Peer_socket `X`, and `X` is connected to opposing
 * peer socket in process P, then `o.*()` credential accessors (such as `o.process_id()`) shall return values that were
 * accurate about process P, at the time P executed `Peer_socket::connect()` or `local_ns::connect_pair()` yielding
 * the connection to "local" peer `X`.
 *
 * @see boost.asio docs: `GettableSocketOption` in boost.asio docs: implemented concept.
 *
 * @internal
 * This is the Linux `getsockopt()` option with level `AF_LOCAL` (a/k/a `AF_UNIX`), name `SO_PEERCRED`.
 */
class Opt_peer_process_credentials :
  public util::Process_credentials
{
public:
  // Constructors/destructor.

  /// Default ctor: each value is initialized to zero or equivalent.
  Opt_peer_process_credentials();

  /**
   * Boring copy ctor.
   * @param src
   *        Source object.
   */
  Opt_peer_process_credentials(const Opt_peer_process_credentials& src);

  // Methods.

  /**
   * Boring copy assignment.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Opt_peer_process_credentials& operator=(const Opt_peer_process_credentials& src);

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::level()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `AF_LOCAL` a/k/a `AF_UNIX`.
   */
  template<typename Protocol>
  int level(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::name()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `SO_PEERCRED`.
   */
  template<typename Protocol>
  int name(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::data()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `SO_PEERCRED`.
   */
  template<typename Protocol>
  void* data(const Protocol& proto);

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::size()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `sizeof(ucred)`.
   */
  template<typename Protocol>
  size_t size(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::resize()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @param new_size_but_really_must_equal_current
   *        See concept API.
   *
   * @internal
   * Resizing is not allowed for this option, so really it's either a no-op, or -- if
   * `new_size_but_really_must_equal_current != size()` -- throws exception.
   */
  template<typename Protocol>
  void resize(const Protocol& proto, size_t new_size_but_really_must_equal_current) const;
}; // class Opt_peer_process_credentials

/* data() and size() feed a util::Process_credentials to getsockopt() as if it were a `::ucred`; so the former
 * must remain nothing but the latter, layout-wise. */
static_assert(std::is_standard_layout_v<util::Process_credentials>
                && (sizeof(util::Process_credentials) == sizeof(::ucred)),
              "util::Process_credentials must remain layout-identical to a lone `::ucred`; "
                "Opt_peer_process_credentials::data()/size() rely on it.");

// Free functions: in *_fwd.hpp.

// Msg_batch_in class template implementations.

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::Mdt_per_payload::Mdt_per_payload
  (const Mutable_buffer_sequence& buf_seq, Msg_resource&& msg_resource) :
  /* Internally, in Linux e.g., sets up iovec[], cumulative size, etc.  It is also extra-quick in that a specialization
   * is chosen at compile-time that adapts the concrete type Mutable_buffer_sequence extra-efficiently; for example
   * by keeping an `iovec[2]` when Mutable_buffer_sequence is `array<Blob_mutable, 2>`. */
  m_buf_seq_adapter(buf_seq),
  m_resource(std::move(msg_resource))
{
  /* Caution!  `man cmsg` in Linux says:
   * "When initializing a buffer that will contain a series of cmsghdr structures (e.g., to be sent with sendmsg(2)),
   * that buffer should first be zero-initialized to ensure the correct operation of CMSG_NXTHDR()."
   * (We are recv*(), not send*(), but seems it would apply too... unless kernel would do it in that case.)
   * We don't use CMSG_NXTHDR() though, so skip `m_msg_control_as_union.m_buf.fill(0)` here for perf.
   * Careful!  May need to add this later, if adding more types of ancillary data (but not if merely increasing
   * N_PAYLOAD_FDS). */
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::Msg_batch_in(size_t max_msg_count) :
  m_n_used(0)
{
  m_mdts.reserve(max_msg_count);
  m_mmsg_hdrs.reserve(max_msg_count);
  // They're empty though.  Let them set up each guy via prepare_target_payload(-1) x N times.
  assert((m_mdts.capacity() == max_msg_count) && "We use .capacity() to query max_msg_count.");
  assert((m_mmsg_hdrs.capacity() == max_msg_count) && "We use .capacity() to query max_msg_count.");
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
void Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::prepare_target_payload
       (const Mutable_buffer_sequence& target_blob, Msg_resource&& msg_resource, size_t idx)
{
  using Buf_seq_adapter = decltype(Mdt_per_payload::m_buf_seq_adapter);

  if (idx == size_t(-1)) // Set up new slot.
  {
    // Ensure .emplace_back() wouldn't increase .capacity().
    assert((!initialized())
           && "At this time the fully-initialized batch size shall be permanently set via ctor; cannot grow.");

    m_mdts.emplace_back(target_blob, std::move(msg_resource));
    auto& mdt = m_mdts.back();

    /* This part is identical to what we do in nb_read_some_with_native_handle() when making msghdr, so omitting cmnts.
     * (@todo Code reuse here is arguably possible but would probably (1) be more lines overall and (2) might
     * be challenging due to this one doing .emplace_back(), the other simply declaring on stack.  Revisit.) */
    m_mmsg_hdrs.emplace_back(); // @todo Is there some way to .emplace_back({ ... })?
    auto& mmsg_hdr = m_mmsg_hdrs.back();
    auto& msg_hdr = mmsg_hdr.msg_hdr;

    // Identical to msghdr setup in nb_read_some_with_native_handle(); omitting those comments.
    msg_hdr.msg_name = nullptr; // (Unused.)
    msg_hdr.msg_namelen = 0; // (Unused.)
    // (Ptr does not change, not even via prepare_target_payload(); see below.)
    msg_hdr.msg_iov = mdt.m_buf_seq_adapter.buffers();
    // (Does not change except via prepare_target_payload() or reuse_result_payloads(); see below.)
    msg_hdr.msg_iovlen = mdt.m_buf_seq_adapter.count();
    msg_hdr.msg_control = mdt.m_msg_control_as_union.m_buf.c_array(); // (Ptr does not change.)
    // (Compile-time constant as in-arg *and* is modified by recv[m]msg() as out-arg; but nb_read() shall correct it.)
    msg_hdr.msg_controllen = sizeof(Msg_control_as_union::m_buf);
    // (Compile-time constant as in-arg *and* is modified by recv[m]msg() as out-arg; but nb_read() shall correct it.)
    msg_hdr.msg_flags = S_PER_MSG_FLAGS;
    // mmsg_hdr.msg_len: Out-arg (queried in, e.g., result_payload_blob()).  Can leave uninit.

    // Caution!  Any changes or reasoning about the above <=> check reuse_result_payloads()'s swap logic too!
  }
  else if (idx < m_mdts.size()) // Re-prepare slot to describe new target memory area.
  {
    auto& mdt = m_mdts[idx];

    /* Buf_seq_adapter{target_blob} is self-evidently correct; then move-assign (really copy-assign) the temporary
     * into mdt.m_buf_seq_adapter.  Perf: as of this writing (and likely ~forever) it copies a size_t total-size;
     * and the iovec array by value.  (As of this writing -- when used by Native_socket_stream_msg_batch_in --
     * that is array<2>; so a couple void* and a couple size_t; but in general it could be a heavier op.) */
    mdt.m_buf_seq_adapter = Buf_seq_adapter{target_blob};

    mdt.m_resource = std::move(msg_resource); // Same as when initializing.

    // The other `mdt` members are out-args; can be left alone.  (Up to user to query responsibly.)

    /* Now as for m_mmsg_hdrs[idx]:  For unchanging/unused members it is uncontroversial to do nothing.
     * That leaves msg_iov and msg_iovlen; let's discuss those.  (It probably seems highly questionable to not
     * simply reassign them again; the safest thing would be to just do that; but in for a penny, in for a pound
     * perf-wise.)
     *   - msg_iov: Oh boy... to avoid excessive verbiage we point you to reuse_result_payloads() which discusses it,
     *     namely how it never changes.
     *   - msg_iovlen: For a general Mutable_buffer_sequence there's no choice but to reassign this simple integer.
     *     Again please read comment in reuse_result_payloads().
     *     @todo It is conceivable to skip it depending on Mutable_buffer_sequence; the logic could be specialized
     *     (much like buffer_sequence_adapter does).  For example, if we are used by Native_socket_stream_msg_batch_in,
     *     then as of this writing Mutable_buffer_sequence is array<2>, so `mdt.m_buf_seq_adapter.count() == 2`, period.
     *     Considering this is a mere integer assignment, the to-do is hardly the highest-priority thing. */
    auto& msg_hdr = m_mmsg_hdrs[idx].msg_hdr;
    msg_hdr.msg_iovlen = mdt.m_buf_seq_adapter.count();

    assert((msg_hdr.msg_controllen == sizeof(mdt.m_msg_control_as_union))
           && "It is initialized to this constant value, and while recv*() shall modify it, nb_read() should have "
                "immediately reset it again.");
  }
  else // if (idx >= m_mdts.size()) && not -1
  {
    assert(false && "prepare_target_payload(x) must either replace an existing slot x or add slot via `x = -1`.");
  }
} // Msg_batch_in::prepare_target_payload()

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
size_t Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::result_payload_blob
         (size_t idx, Msg_resource** msg_resource_ptr)
{
  assert(idx < m_mmsg_hdrs.size());
  // Note: We intentionally allow `idx >= m_n_used` due to reuse_result_payloads(); see our doc header.

  if (msg_resource_ptr)
  {
    *msg_resource_ptr = &(m_mdts[idx].m_resource);
  }

  return m_mmsg_hdrs[idx].msg_len;
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
Native_handle Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::result_payload_hndl(size_t idx) const
{
  assert(idx < m_mdts.size());
  // Note: We intentionally allow `idx >= m_n_used`; same reason as for result_payload_blob().

  return m_mdts[idx].m_result_hndl;
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
bool Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::initialized() const
{
  return m_mdts.size() == m_mdts.capacity();
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
size_t Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::n_used() const
{
  return m_n_used;
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
bool Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::full() const
{
  return m_n_used == m_mdts.size();
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
void Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::clear_used(size_t new_n_used)
{
  m_n_used = new_n_used;
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
void Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::Mdt_per_payload::swap(Mdt_per_payload* that)
{
  using std::swap; // This is the proper ADL-friendly style (do *not* inline std::swap() instead).

  /* Rationale for caring about swapping of guys like *this: (at least) Msg_batch_in::reuse_result_payloads().
   * It might be best to look at that thing for context too when grokking this.
   *
   * Why the custom swap()?  Answer:
   *   - At least one thing need not actually be swapped; see below.
   *   - If there's a custom, faster swap(Msg_resource_t&, Msg_resource_t&), then the below will invoke it
   *     (as opposed to the default which would do 3 move-assigns).
   *   - There *is* a custom, faster Native_handle swap, so we invoke it (similar reasoning). */

  /* This ultimately swaps the `iovec` arrays (and perhaps some other stuff such as the computed total size) stored
   * inside boost::asio::detail::buffer_sequence_adapter.  (Depending on the Mutable_buffer_sequence specialization
   * they might not be arrays but, e.g., just single `iovec`s... but memory-wise equivalent to arrays of some constant
   * length.)  So most likely it can be thought of as 3 memcpy()s.  Actually as of this writing Mutable_buffer_sequence,
   * when we are used by Native_socket_stream_msg_batch_in, is array<2> (b/c its prepare_target_payload() takes just
   * a single Mutable_buffer, and we pre-pend m_msg_type's location/size); so in that case it's more like:
   * 3-op swap of 2 void*, 3-op swap of 2 size_t.  That is to say it's pretty quick.  In general though depending
   * on Mutable_buffer_sequence it could be slower; but that's life.
   *
   * Perf aside the point is: this is a by-value swap. */
  swap(m_buf_seq_adapter, that->m_buf_seq_adapter);

  /* m_msg_control_as_union is out-arg, and nb_read() saves any contents to m_result_hndl; so this is garbage.
   * No need to swap via `swap(m_msg_control_as_union, that->m_msg_control_as_union)`. */

  swap(m_resource, that->m_resource); // Either std::swap() or custom-swap the `Msg_resource_t`s (ADL).

  /* This is an important out-arg, so of course we should swap it.  It is just an int really.
   * Though since we're expounding on every little thing already... std::swap()
   * of Mdt_per_payload would cause 3 Native_handle move-assigns, and as of this writing actually that'd
   * waste cycles on unnecessarily nullifying source ->m_result_hndl, 3 times; so it's nice to do this. */
  swap(m_result_hndl, that->m_result_hndl);
} // Mdt_per_payload::swap()

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
template<typename Is_unused_func>
void
  Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::reuse_result_payloads(size_t start_idx,
                                                                                 const Is_unused_func& is_unused_func)
{
  using std::swap; // This is the proper ADL-friendly style (do *not* inline std::swap() instead).

  /* This is adapted from a GNU STL remove_if() impl; but swap()s instead of merely move()ing.
   * (To illustrate, remove_if("abc def ghi ", " ") would result in "abcdefghihi "; as the defghi chars
   * in the back would get moved-to the front area, hence the move-from would just duplicate them; for other element
   * types it would nullify them.  Our algorithm would instead yield "abcdefghi   ", the earlier-on spaces being
   * swap-moved to the back.)  Also we deal in indices and is_unused(idx) calls as opposed to iterators/F(*it); but
   * logically same thing. */

  auto& first = start_idx; // (Mere alias so as to match the GNU STL impl more closely.)

  for (; (first != m_n_used) && (!is_unused_func(first)); ++first) {}

  if (first == m_n_used)
  {
    return; // Unchanged -- all elements are used.
  }
  // else

  auto result = first++;
  for (; first != m_n_used; ++first)
  {
    if (!is_unused_func(first))
    {
      // Must swap slots [result] and [first].  Slot = m_mmsg_hdrs[idx] + m_mdts[idx].

      m_mdts[result].swap(&m_mdts[first]); // See inside this for key comments.
      // ^-- @todo swap(..., ...) would be better, but it's a private inner struct, so we haven't bothered....

      /* There is no custom swap(mmsghdr&, mmsghdr&), so the default would happen if we simply did
       *   swap(m_mmsg_hdrs[result], m_mmsg_hdrs[first]);
       * Namely the scalars therein would be swapped using the usual 3-step thing.  They are all integers or pointers,
       * so the perf is fine... but (1) in for a penny, in for a pound: We decided to be precious about perf and swap
       * only what is necessary; and (2) more importantly in our case some of the values point to m_mdts[idx] -- so
       * the proper "swap" is to do nothing in that case!  So it wouldn't just be "slow" but also wrong.
       *
       * Let's justify what we actually do do.  To wit:
       *
       *   - Firstly we can ignore (for perf) the various unused and constant-valued fields:
       *     msg_hdr.{msg_name, msg_namelen, msg_flags, msg_controllen}.
       *     - msg_hdr.msg_controllen is a subtle beast.  As an in-arg, it is always set to a constant value in our case
       *       (sizeof(a thing)).  Then, however, recv*() modifies it!  It is unclear to *what* exactly it is modified;
       *       presumably something like the size of received ancillary data; but docs do not say how to use it and
       *       moreover say CMSG*() macros are to be used to traverse ancillary data (and we do just that in nb_read(),
       *       where we grab the native handles if any and save in m_mdts[...].m_result_hndl).  That does not, however,
       *       mean msg_controllen (as out-arg) is not used; maybe CMSG*() grabs it/uses it somehow.  That said: by the
       *       time we are executed, nb_read() has already set m_result_hndl (and the m_mdts[].swap() dealt with it
       *       above), and reset msg_controllen to the constant value it must equal as in-arg in the next nb_read().
       *       Therefore: bottom line: It is always set to a constant value, except for a short time inside nb_read()
       *       just after recv*().  Therefore, here, we can ignore it as a constant-valued field.
       *   - The out-arg mmsghdr::msg_len (# of bytes received) is clearly correct to swap in normal fashion; it
       *     is accessed via public accessor(s).
       *   - msg_control points to where ::recvmmsg() is to place received FDs.
       *     Its value is: mdt[...].m_msg_control_as_union.m_buf.c_array(): ptr to element 0 of array, namely the
       *     ancillary-data (FD) storage area of a certain constant (no matter the slot) size.  Even if the
       *     values inside said array mattered (they don't as of this writing; nb_read() copies FD(s) into m_mdts[]),
       *     pointing to the same-index-th mdts[].m_msg_control_as_union would have been correct; meaning
       *     the contents would've been swapped above, so the pointers should remain unchanged.  (This is a bit tricky
       *     IMO (ygoldfel) to realize and might require some pencil-and-paper to prove to oneself.)
       *   - msg_iov and msg_iovlen: Okay, so IMHO (ygoldfel) this is shockingly difficult to grok, if one is not
       *     very careful in one's reasoning.  It also very much relies on how
       *     boost::asio::detail::buffer_sequence_adapter is implemented; it's in detail:: in the first place and
       *     in any case internally could change; but we've already decided to take that risk elsewhere.  So let's
       *     just go with the facts of its current state (Boost-1.87).  buffer_sequence_adapter, for the purposes of
       *     this discussion, should be thought of as being isomorphic to array<iovec, N>, where N is a constant
       *     (yes, it could even just be simply equal to `iovec`, depending on Mutable_buffer_sequence, but that still
       *     fits).  Then, msg_iov is a pointer &...[0] of that array; and `msg_iovlen <= N` is how much of that
       *     array to use.  (This all would be different if .buffers() returned some malloc()ed heap address instead
       *     of a pointer right into m_mdts[] memory area itself.)  So given that info:
       *       - Up above, we did in fact swap the *contents* of the two array<iovec, N>s.  Therefore
       *         the *pointers* msg_iov should *not* be swapped: they are pointing at the proper iovec-arrays already.
       *       - However the active *lengths* do need to be swapped!  E.g., picture [1 2 x]<=>[3 4 5]; this becomes
       *         [3 4 5]...[1 2 x].  msg_iov (unchanged) were pointing at 1 and 3; now to 3 and 1; that's right.
       *         But if the lengths were 2<=>3, their becoming 3...2 is correct; whereas remaining 2...3 would be
       *         blatantly wrong; the msg_iovlen=2 would ignore the element `5`, and msg_iovlen=3 would refer to
       *         the element `x` among others.
       *     So yes, as odd it may seem, the correct thing is to not touch msg_iov; but to swap msg_iovlen.
       *     @todo See also to-do in prepare_target_payload() regarding conceivable specialized logic that could
       *     at times skip this swap as well.
       *     - Just to confuse you even more: if we're being real, any user of reuse_result_payloads() is highly
       *       likely to simply keep msg_iovlen always at the same value, and same for the individual buffers'
       *       lengths (inside each iovec), when comparing between any two slots.  If it's possible their algorithm
       *       allows for swapping slots like this, then they pretty much have to have identical structure.  Granted,
       *       perhaps one could have, like, 3 1-buffers and 1 3-buffer (totaling the same capacity, 3) in 2 respective
       *       slots; but come on, why do such a thing?  So the swapping is likely a no-op anyway... but hey, if
       *       we want to be correct, we should be correct. */

      auto& mmsg_hdr1 = m_mmsg_hdrs[result];
      auto& mmsg_hdr2 = m_mmsg_hdrs[first];
      auto& msg_hdr1 = mmsg_hdr1.msg_hdr;
      auto& msg_hdr2 = mmsg_hdr2.msg_hdr;

      swap(mmsg_hdr1.msg_len, mmsg_hdr2.msg_len);
      swap(msg_hdr1.msg_iovlen, msg_hdr2.msg_iovlen);

#if 0 // This is pretty well tested by now -- cut it out for perf when assert()s enabled.
      // Paranoia strikes deep... opportunistically check this invariant... perhaps again.
      assert((msg_hdr1.msg_controllen == sizeof(Msg_control_as_union::m_buf))
             &&
             (msg_hdr2.msg_controllen == sizeof(Msg_control_as_union::m_buf))
             && "It is initialized to this constant value, and while recv*() shall modify it, nb_read() should have "
                  "immediately reset it again.");
#endif
      // End of swap code.

      ++result;
    } // if (!is_unused_func(first))
  } // for (; first != m_n_used; ++first)

  m_n_used = result;

  /* @todo Conceptually, our approach to swapping the m_mdts[] and m_mmsg_hdrs[] pairs is arranged as follows:
   * Skip anything constant/unused; swap m_mdts[] first; swap non-m_mdts[]-referencing parts of m_mmsg_hdrs[]
   * (i.e., msg_len); and lastly update/swap the relevant m_mdts[]-referencing parts (i.e., msg_hdr.msg_iovlen).
   * The part of that that's at all tricky is this subset: swap m_mdts[] by value, so no pointer into m_mdts[]
   * needs updating.  This *is* correct (hence all the verbiage above), but perf-wise: there's the
   * m_buf_seq_adapter swap (linear in sizeof(Mutable_buffer_sequence) -- could be quite quick, could be less so);
   * and m_resource swap.  The potential to-do would be to switch the paradigm: m_mdts becomes a mere store
   * (wouldn't even need to be a vector<>; could be anything like set<>; but vector<> is faster for our purposes;
   * point is, the index into it no longer has meaning); and the swapping would only swap various pointers
   * in m_mmsg_hdrs[].  So m_mdts[] are *not* swapped by value, only various referents into it are.
   * This would avoid those *sort of* expensive m_buf_seq_adapter and m_resource swaps.  Some of the reasoning
   * above wouldn't need to be so annoying (arguable).  Probably would need to use the trick where msg_name would store
   * a pointer into m_mdts, for our own purposes, at least to be able to find the proper m_mdts[].m_result_hndl.
   *
   * Should we do it?  In terms of simplifying stuff, it might be a wash.  In terms of perf, it depends on how
   * we're used.  As of this writing the swap-stuff/reuse_result_payloads() algorithm exists because of
   * how Native_socket_stream_msg_batch_in uses us, so let's consider that use case; see
   * Native_socket_stream_msg_batch_in::nb_read(), namely how it uses reuse_result_payloads() as of this writing.
   * To wit: illegal in-dgrams are non-existent; and there's at most one graceful-close per connection; so the
   * only reason to actually swap would be auto-ping in-dgrams.  Those might seem like they could be quite prevalent;
   * but consider that if the system is so loaded to where *this* level of optimization at all matters, then
   * such a connection would hardly ever even need to send auto-pings (which occur in an idle channel, not otherwise).
   * So... I (ygoldfel) would at this point say, keep an eye on profiling results, and if reuse_result_payloads() looks
   * like a hot-spot, then look into it.  Otherwise, leave it alone. */
} // Msg_batch_in::reuse_result_payloads()

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
bool
  Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::nb_read(flow::log::Logger* logger_ptr,
                                                                   Peer_socket<Protocol_pkt_stream>* peer_socket_ptr,
                                                                   bool* graceful_close, Error_code* err_code,
                                                                   int message_flags,
                                                                   size_t* total_rcvd_bytes_or_null)
{
  using flow::log::Sev;
  using boost::system::system_category;
  using boost::io::ios_all_saver;
  namespace sys_err_codes = boost::system::errc;
  using std::swap;
  using ::recvmmsg;
  using ::recvmsg;
  using ::mmsghdr;
  using ::cmsghdr;
  // using ::SOL_SOCKET; // It's a macro apparently.
  using ::SCM_RIGHTS;
  using ::MSG_DONTWAIT;
  using ::MSG_TRUNC;
  using ::MSG_CTRUNC;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, nb_read, logger_ptr, peer_socket_ptr, graceful_close, _1, message_flags,
                                     total_rcvd_bytes_or_null);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  assert((m_mmsg_hdrs.size() == m_mdts.size()) && "Gotta be a bug somewhere in Msg_batch_in.");
  if (!initialized()) // As advertised:
  {
    FLOW_LOG_WARNING("Local_stream_batch [" << *this << "]: "
                     "Attempt to batch-read without adding all [" << m_mdts.capacity() << "] slots; so far "
                     "only [" << m_mdts.size() << "] slots have been added via prepare_target_payload().  "
                     "This is the API user's error, though since this API is used internally in the library "
                     "too, it might be a bug in the library.  We can't tell from here.  Emitting invalid-argument "
                     "error.");
    *err_code = error::Code::S_INVALID_ARGUMENT;
    return true;
  }
  // else

  const size_t n_payloads = m_mdts.size() - m_n_used;

  if (n_payloads == 0)
  {
    return false;
  }
  // else if (n_payloads >= 1):

  /* To reader/maintainer: The rest is similar to nb_read_some_with_native_handle() in various places ("just"
   * applied to 2+ potential in-dgrams), so we will skip some comments.
   *
   * The difference is basically that for each of the following, there are now roughly speaking n_payloads
   * of that thing:
   *   - msghdr to the OS-call:
   *     - Instead there's an array of them, one per potential in-dgram, arranged into mmsghdr [sic] structs.
   *     - And therefore buf_seq_adapter (iovec array with target in-bufs in msghdr):
   *       - Instead there's (essentially) a sequence of them (yes, seq of seqs), m_mdts[*].m_buf_seq_adapter.
   *     - Also therefore msg_control_as_union (which sits ready to receive an FD, or none, upon recvmsg()):
   *       - Instead there's one of these per msghdr, m_mdts[*].m_msg_control_as_union.
   *   - n_rcvd_or_error (bytes received into buf_seq, returned by recvmsg(), or -1 for error):
   *     - Instead recvmmsg() returns -1 for error, else the # of leading `msghdr`s that did receive data, and
   *       for each of those the `n_rcvd` part, so to speak, is populated in that mmsghdr, m_mmsg_hdrs[*].msg_len.
   *
   * Due to the design of *this class, we've been painstakingly maintaining all of those things, so we can
   * pretty much execute ::recvmmsg() right away with no linear-time prep.  Reminder: We don't target the
   * *entire* m_mmsg_hdrs+m_mdts vectors; but rather the trailing n_payloads of them, starting with
   * m_...[m_n_used]. */

  assert(peer_socket_ptr);
  auto& peer_socket = *peer_socket_ptr;
  const Native_handle peer_hndl{peer_socket.native_handle()};

  /* Since this method and class are to be heavily optimized: Use this optimization that caches
   * whether TRACE messages should be logged instead of computing it per-message.  The negatives are:
   *   - The code is a bit more involved (not too bad, when perf is truly at stake).
   *   - If the verbosity is changed under us *during* the method's execution, this will not respond to it
   *     immediately.  It's fine: the lag is only a split-second; and a slight unevenness in taking verbosity
   *     config changes is an expected/accepted phenomenon generally. */
  const bool do_log_trace = logger_ptr && logger_ptr->should_log(Sev::S_TRACE, get_log_component());

  auto recvmmsg_hdr_ptr = &(m_mmsg_hdrs[m_n_used]);
  int n_rcvd_msgs_or_error;

  /* Massage message_flags for our required purposes.
   * Same comments as when calling ::recvmsg() in nb_read_some_with_native_handle(). */
  message_flags = (message_flags | (MSG_DONTWAIT | MSG_TRUNC));

  if (n_payloads == 1)
  {
    // There is only one target dgram; try to optimize somewhat by reverting to single-msg native API.
    const auto n_rcvd
      = recvmsg(peer_hndl.m_native_handle, &recvmmsg_hdr_ptr->msg_hdr, message_flags);
    if (do_log_trace)
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
         "tried to batch-read up-to 1 in-message, "
         "potentially into a scattered-blob plus possibly a native handle; "
         "for efficiency used recvmsg() instead of recvmmsg(); it yielded return value "
         "[" << n_rcvd << "]; we shall now continue as-if this was done using recvmmsg().");
    }

    if (n_rcvd == -1)
    {
      n_rcvd_msgs_or_error = -1;
    }
    else
    {
      n_rcvd_msgs_or_error = 1;
      recvmmsg_hdr_ptr->msg_len = static_cast<unsigned int>(n_rcvd);
      // Note: n_rcvd might be 0, indicating EOF (graceful-close).  recvmmsg() *would* also result in `->msg_len == 0`.
    }
  }
  else
  {
    n_rcvd_msgs_or_error
      = recvmmsg(peer_hndl.m_native_handle, recvmmsg_hdr_ptr, n_payloads, message_flags,
                 nullptr); // Don't mess with timeouts.
  }

  /* Carefully check all the outputs.  Return value first.
   * (Code reuse with nb_read_some_with_native_handle() is possible, but there are enough little differences, such
   * as logging, to where it feels more unwieldy than it is worth.  @todo Maybe revisit.) */

  if (n_rcvd_msgs_or_error == -1)
  {
    /* Not even 1 byte of a blob was read; and hence nor was any FD.  (Omitting various comments from
     * nb_read_some_with_native_handle() that apply equally here.) */

    const Error_code sys_err_code{errno, system_category()};
    if ((sys_err_code == sys_err_codes::operation_would_block) || // EWOULDBLOCK
        (sys_err_code == sys_err_codes::resource_unavailable_try_again)) // EAGAIN (same meaning)
    {
      if (do_log_trace)
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
           "tried to batch-read up-to [" << n_payloads << "] in-messages, each "
           "potentially into a scattered-blob plus possibly a native handle per each; but read attempt "
           "indicated would-block; not an error condition.  No in-messages received.");
      }
      *err_code = boost::asio::error::would_block;
      return true; // m_n_used unchanged.
    }
    // else

    assert(sys_err_code);

    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
    FLOW_LOG_WARNING("Local_stream_batch [" << *this << "]: "
                     "Connected local peer socket [" << peer_hndl << "] "
                     "tried to batch-read up-to [" << n_payloads << "] in-messages, each "
                     "potentially into a scattered-blob plus possibly a native handle per each; "
                     "but an unrecoverable error occurred.  No in-messages received.");
    *err_code = sys_err_code;
    return true; // m_n_used unchanged.
  } // if (n_rcvd_msgs_or_error == -1)
  // else if (n_rcvd_msgs_or_error != -1)

  if (n_rcvd_msgs_or_error == 0)
  {
    assert((n_payloads > 1) && "We should have returned false on 0; and used recvmsg() on 1, resulting in "
                                 "n_rcvd_msgs_or_error being -1 or 1, not zero.");
    // The assert() states that recvmmsg() was used.  So given that:

    FLOW_LOG_FATAL
      ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
       "tried to batch-read up-to [" << n_payloads << "] in-messages, each "
       "potentially into a scattered-blob plus possibly a native handle per each; "
       "but recvmmsg() returned 0; this is an unexpected semantic; EOF (graceful-close) should result in "
       "an in-message with msg_len=0 instead.  Aborting; please look into this situation; it could be a bug.");
    assert(false
             && "Tried to batch-read up-to 2+ in-messages, each "
                "potentially into a scattered-blob plus possibly a native handle per each; "
                "but recvmmsg() returned 0; this is an unexpected semantic; EOF (graceful-close) should result in "
                "an in-message with msg_len=0 instead.  Aborting; please look into this situation; it could be a bug.");
    std::abort();
  }
  // else if (n_rcvd_msgs_or_error > 0):

  /* In-message(s) received.  Here in-message means either an actual in-message or a graceful-close "in-message."
   * (A fairly-important, albeit undocumented and obscure fact: graceful-close is emitted as an in-message record
   * with msg_len=0 (no data)... followed by identical such records up to n_payloads total records.)
   *
   * Check each one for corner case errors and finalize each one's out-args.  Again:
   * Omitting various comments that apply equally in nb_read_some_with_native_handle(). */

  const auto recvmmsg_hdr_end = recvmmsg_hdr_ptr + n_rcvd_msgs_or_error;
  auto mdt_ptr = &(m_mdts[m_n_used]);

  /* Used inside the loop on any error; essentially -- seeing as how we are emitting failure <=> no emitted msgs --
   * it (1) cleans-up *this (m_mmsg_hdrs, m_mdts) that may have been set to non-unused-state before the
   * current (recvmmsg_hdr_ptr, mdt_ptr) one that triggered the error; and (2) ensures no native-handles
   * that may have been received in the batch get leaked (since we emit no msgs) -- it returns these to OS.
   * (Subtlety: Un-emitted slots (i.e., ones starting with [m_n_used]) aren't required to have .m_result_hndl
   * in any particular state, .null() or otherwise.  So this helper doesn't "formally" promise anything about that;
   * only that any in-batch-contained raw handles get closed.)
   *
   * (1) might be unnecessary paranoia -- generally user should not and possibly cannot use the socket
   * after an error anyway -- but we've spent significant effort on ensuring msg_{controllen|flags} is in its in-arg
   * form as much as possible; this will make quite sure in the (relatively) rare error case.
   *
   * (2) accounts for an edge (given a sane opposing guy) scenario; but in general why not?
   *
   * Note this is only necessary for the slots recv*() actually touched.  So in particular no need to do this
   * at all unless `n_rcvd_msgs_or_error > 0`. */
  const auto clean_remaining_func = [&]() -> bool
  {
    assert((recvmmsg_hdr_ptr != recvmmsg_hdr_end) && "Internal pre-condition violated.");

    /* (1), in short, is just resetting msg_{controllen|flags} to their init-values.  For pre-error slots it's
     * already done; so we do it for the post-error (inclusive) slots.  This loop is over those.
     *
     * (2) needs to un-leak any handle-copies in the entire in-batch: the pre-error slots if any; and the
     * post-error slots (inclusive; so at least 1).  In this loop therefore we do the latter half. */
    do
    {
      auto& recvmsg_hdr = recvmmsg_hdr_ptr->msg_hdr;

      const auto recvmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&recvmsg_hdr); // Half of (2).
      if (recvmsg_hdr_cmsg_ptr
          && (recvmsg_hdr_cmsg_ptr->cmsg_level == SOL_SOCKET)
          && (recvmsg_hdr_cmsg_ptr->cmsg_type == SCM_RIGHTS))
      {
        Native_handle{*(reinterpret_cast<const Native_handle::handle_t*>(CMSG_DATA(recvmsg_hdr_cmsg_ptr)))}
          .close();
        /* Closed handle, as promised. / Did not touch *this (m_mdts[].m_result_hndl); allowed by our contract.
         * (In practice, as of this writing, this is equivalent behavior to never having received the
         * post-error slots; clearly sensible.  As of this writing that applies to the error-slot in particular too,
         * as calling code below only touches .m_result_hndl last, after eliminating all error possibilities.) */
      }

      // (1).  Has to be done after the above (msg_controllen affects CMSG_FIRSTHDR() behavior).
      recvmsg_hdr.msg_controllen = sizeof(Msg_control_as_union::m_buf);
      recvmsg_hdr.msg_flags = S_PER_MSG_FLAGS;
    }
    while ((++recvmmsg_hdr_ptr) != recvmmsg_hdr_end);

    /* That leaves the other half of (2): Un-leaking the handle-copies in pre-error slots if any.  mdt_ptr points
     * at the error-slot still, so we backtrack to the start of the target area of *this batch, closing any
     * `Native_handle`s we'd recorded.  (We could *not* instead just loop through all of m_mmsg_hdrs as above,
     * as msg_controllen would have been reset by the looping code below.  Even if we could:
     * This is just nice, as we can also re-nullify pre-error `.m_result_hndl`s -- not required but cleaner
     * than leaving bogus values in there.) */
    const auto mdt_ptr_first = &(m_mdts[m_n_used]);
    while (mdt_ptr != mdt_ptr_first)
    {
      (--mdt_ptr)->m_result_hndl.close(); // Close handle, as promised.  Nullify for bonus cleanliness.
    }

    return true;
  }; // clean_remaining_func =

  size_t n_rcvd_msgs_actual = n_rcvd_msgs_or_error; // For EOF-marker handling; you'll see.
  size_t total_rcvd_bytes = 0; // Running tally of msg_len across actual (non-EOF) dgrams; EOF markers contribute 0.
  do
  {
    auto& recvmsg_hdr = recvmmsg_hdr_ptr->msg_hdr;
    auto& mdt = *mdt_ptr;

    /* msg_hdr.msg_iov array-pointed area is already written-to... and .msg_len already holds how many bytes of that
     * area was used.  Though, as in nb_read_some_with_native_handle(), it may also indicate overflow
     * (N-byte in-dgram but <N bytes in target buffer).  Refer to that function; keeping comments light except ones
     * relevant here only. */

    const size_t target_payload_blob_sz = mdt.m_buf_seq_adapter.total_size();
    const auto msg_len = recvmmsg_hdr_ptr->msg_len;
    if (msg_len > target_payload_blob_sz)
    {
      FLOW_LOG_WARNING("Local_stream_batch [" << *this << "]: "
                       "Connected local peer socket [" << peer_hndl << "] "
                       "tried to batch-read up-to [" << n_payloads << "] in-messages, "
                       "each potentially into a scattered-blob plus possibly a native handle per each; "
                       "and it returned it read [" << n_rcvd_msgs_or_error << "] in-messages; and for message "
                       "index [" << (n_rcvd_msgs_or_error - (recvmmsg_hdr_end - recvmmsg_hdr_ptr)) << "] (0-based) "
                       "got [" << msg_len << "] bytes successfully, but this overflows "
                       "the blob's capacity [" << target_payload_blob_sz << "].  "
                       "Acting as if nothing received + error.");
      assert((recvmsg_hdr.msg_flags & MSG_TRUNC)
             && "In-dgram size overflows user-supplied buffer <=> MSG_TRUNC out-flag supposed to be set.");

      /* Note/caution: Suppose this is iteration 2+.  We emit the error anyway and don't report any received
       * data -- those in-dgrams are "eaten."  This doesn't violate the contract (wherein we promise to not
       * emit data *and* an error).  However it doesn't do the conceivable thing (that recvmmsg() itself does), where
       * we report the in-dgrams now and defer the error to the next receive-op later.  Why not?  Answer: Firstly
       * it would be hard-ish to implement; we'd have to actively store the error in a new m_, and so on.  Secondly
       * we feel it is not worth it, as this error is truly exceptional -- the other side is misbehaving.
       * @todo Perhaps revisit. */

      *err_code = error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE;
      return clean_remaining_func(); // m_n_used unchanged.
    }
    // else: No overflow; and msg_len may be 0 (EOF, handled specially below) or 1+.

    // Next, the out-flags.

    /* Subtlety: It is also an in-arg; so immediately reset it, in case no fatal error results from a non-zero out-flag.
     * clean_remaining_func() will do the same on error for all remaining .msg_flags (and this one again... no
     * biggie). */
    decltype(recvmsg_hdr.msg_flags) out_flags = S_PER_MSG_FLAGS;
    swap(out_flags, recvmsg_hdr.msg_flags);

    if (out_flags != 0)
    {
      if (logger_ptr && logger_ptr->should_log(Sev::S_INFO, get_log_component()))
      {
        ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::hex/etc. soon.
        FLOW_LOG_INFO_WITHOUT_CHECKING
          ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
           "tried to batch-read up-to [" << n_payloads << "] in-messages, each "
           "potentially into a scattered-blob plus possibly a native handle per each; "
           "and it returned it read [" << n_rcvd_msgs_or_error << "] in-messages; and for message "
           "index [" << (n_rcvd_msgs_or_error - (recvmmsg_hdr_end - recvmmsg_hdr_ptr)) << "] (0-based) "
           "got [" << msg_len << "] bytes successfully but also returned raw "
           "out-flags value [0x" << std::hex << out_flags << "].  "
           "Will check for relevant flags but otherwise ignoring if nothing bad.  "
           "Logging at elevated level because it's interesting; please investigate.");
      }

      if ((out_flags & MSG_CTRUNC) != 0)
      {
        if (logger_ptr && logger_ptr->should_log(Sev::S_WARNING, get_log_component()))
        {
          ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::hex/etc. soon.
          FLOW_LOG_WARNING_WITHOUT_CHECKING
            ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
             "tried to batch-read up-to [" << n_payloads << "] in-messages, "
             "each potentially into a scattered-blob plus possibly a native handle per each; "
             "and it returned it read [" << n_rcvd_msgs_or_error << "] in-messages; and for message "
             "index [" << (n_rcvd_msgs_or_error - (recvmmsg_hdr_end - recvmmsg_hdr_ptr)) << "] (0-based) "
             "got [" << msg_len << "] bytes successfully but also returned raw "
             "out-flags value [0x" << std::hex << out_flags << "] which includes MSG_CTRUNC.  "
             "That flag indicates more stuff was sent as ancillary data; but we expect at most 1 native "
             "handle.  Other side sent something strange.  Acting as if nothing received + error.");
        }

        // Note/caution: Same as in the S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE case above.

        *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
        return clean_remaining_func(); // m_n_used unchanged.
      }
      // else: Fall-through.
    } // if (out_flags != 0)

    /* Next, the ancillary data (with the FDs if any).  Again: cmnts light; see nb_read_some_with_native_handle().
     * This is a new point though: We don't necessarily have to save the FD anywhere; the accessor
     * result_payload_hndl() could just do the below itself and return the result from there directly.  However then:
     * the error handling has to move there which is annoying API-wise; and it's slower, as we have to do the
     * CMSG_FIRSTHDR() and the checking and... let's just take care of it and save the FD in a nice Native_handle for
     * that accessor to return.  It's just an int; let's be real. */

    cmsghdr* const recvmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&recvmsg_hdr);
    if (recvmsg_hdr_cmsg_ptr)
    {
      if ((recvmsg_hdr_cmsg_ptr->cmsg_level == SOL_SOCKET) &&
          (recvmsg_hdr_cmsg_ptr->cmsg_type == SCM_RIGHTS))
      {
        static_assert(N_PAYLOAD_FDS == 1, "Should be only dealing with one native handle with recvmsg() "
                                          "as of this writing.");
        mdt.m_result_hndl
          = *(reinterpret_cast<const Native_handle::handle_t*>(CMSG_DATA(recvmsg_hdr_cmsg_ptr)));
      }
      else
      {
        FLOW_LOG_WARNING("Local_stream_batch [" << *this << "]: "
                         "Connected local peer socket [" << peer_hndl << "] "
                         "tried to batch-read up-to [" << n_payloads << "] in-messages, "
                         "each potentially into a scattered-blob plus possibly a native handle per each; "
                         "and it returned it read [" << n_rcvd_msgs_or_error << "] in-messages; and for message "
                         "index [" << (n_rcvd_msgs_or_error - (recvmmsg_hdr_end - recvmmsg_hdr_ptr)) << "] (0-based) "
                         "got [" << msg_len << "] bytes successfully but also "
                         "unexpected ancillary data of cmsg_level|cmsg_type "
                         "[" << recvmsg_hdr_cmsg_ptr->cmsg_level << '|' << recvmsg_hdr_cmsg_ptr->cmsg_type << "].  "
                         "Acting as if nothing received + error.");

        // Note/caution: Same as in the S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE case above.

        *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
        return clean_remaining_func(); // m_n_used unchanged.
      }
      // else: Fall-through (.m_result_hndl is set just above).
    }
    else
    {
      // No ancillary data, meaning no native handle; that's quite normal; indicate it as promised:
      mdt.m_result_hndl = {};
    }

    /* Please see nb_read_some_with_native_handle() comment near msg_controllen init; as well as
     * reuse_result_payloads() comment discussing it.  That justifies the following (as well as clean_remaining_func())
     * in more detail -- but basically we have the invariant that recvmsg_hdr.msg_controllen is always this constant
     * value as in-arg, and recv*() happens to set it to something else as out-arg.  So we always undo it either here
     * (up to detecting an error) or via clean_remaining_func() (upon detecting error). */
    recvmsg_hdr.msg_controllen = sizeof(Msg_control_as_union::m_buf);

    /* Almost done except for the following fairly-important, albeit ~undocumented and obscure fact:
     * recvmmsg() will *not* simply return 0 on EOF (nor defer doing so until the next recv*(), if 1+ in-messages
     * preceded it in the present recvmmsg()).  Instead, it will yield endless 0-length data-free "messages,"
     * apparently mimicking what would happen if we were to instead trigger repeated recvmsg() calls (each would
     * return 0, hence the corresponding mmsg_hdr.msg_len is set to 0 also).  Therefore: */
    const bool eof_marker = (msg_len == 0) && mdt.m_result_hndl.null();
    if (eof_marker)
    {
      /* K (see below) <= n_rcvd_msgs_or_error - 1.
       * If current n_rcvd_msgs_actual = its original value, then K < n_rcvd_msgs_actual, so n_rcvd_msgs_actual
       * shall now become the new smaller value, marking the *first* empty "in-dgram" (really, EOF marker).
       * If current n_rcvd_msgs_actual has been set by a previously set EOF marker in an earlier iteration,
       * then K > n_rcvd_msgs_actual; hence n_rcvd_msgs_actual remains unchanged and continues to mark the first
       * EOF marker "in-dgram."  So this is a nice compact/fast way to express setting it only once at most.
       *
       * However:
       *   - This assumes there won't be non-data-free datagrams following an EOF-marker one.  Indeed that is
       *     impossible, unless sender sends an entirely data-free datagram; we disallow this formally and also
       *     even if that weren't followed, then by either updating m_n_used accordingly, or emitting `eof` error (see
       *     after loop), we'll essentially ignore anything following the first EOF-marker -- no matter what that is.
       *     That's fine.
       *   - This doesn't care if, after EOF-marker 1, all available slots are filled with EOF-markers
       *     (n_rcvd_msgs_or_error == n_payloads), or if some were left untouched (n_rcvd_msgs_or_error < n_payloads).
       *     Any touched ones shall be reset; any untouched ones don't need any reset; and n_rcvd_msgs_actual (and
       *     therefore m_n_used or *err_code) shall be set based on EOF-marker 1.  That said as of this writing
       *     in reality Linux at least will fill all subsequent slots with EOF-marker "datagrams"
       *     (n_rcvd_msgs_or_error == n_payloads).  Our code is agnostic of this and does not verify this.
       *   - We could also (on seeing EOF-marker 1) stop the loop, etc. etc.  However that actually seemed more complex
       *     to code; particularly given our desire to reset the in-arg/out-args .msg_flags and .msg_controllen for
       *     potential reuse/whatever.  So we go the pithier way. */
      n_rcvd_msgs_actual = std::min(n_rcvd_msgs_actual,
                                    // Let the following arg's value be K in the above comment.
                                    size_t(size_t(n_rcvd_msgs_or_error)
                                           - size_t(recvmmsg_hdr_end - recvmmsg_hdr_ptr))); // This is always >= 1.
    } // if (eof_marker)

    if (do_log_trace)
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Local_stream_batch [" << *this << "]: "
         "recvmmsg[" << peer_hndl << "] reports for message index "
         "[" << (n_rcvd_msgs_or_error - (recvmmsg_hdr_end - recvmmsg_hdr_ptr)) << "] (0-based) receipt of possible "
         "native handle [" << mdt.m_result_hndl << "]; as well as "
         "[" << msg_len << "] bytes of the blob's capacity [" << target_payload_blob_sz << "]; "
         "EOF-marker? = [" << eof_marker << "].  "
         "Total in-message records received: [" << n_rcvd_msgs_or_error << "] out of max [" << n_payloads << "].  "
         "Potentially corrected (due to 1st EOF-marker record) in-message count: [" << n_rcvd_msgs_actual << "].");
    }

    total_rcvd_bytes += msg_len; // EOF markers contribute 0; no extra cost.

    // All done (for this in-message)!
  }
  while ((++mdt_ptr, ++recvmmsg_hdr_ptr) != recvmmsg_hdr_end);

  assert(graceful_close);
  if (n_rcvd_msgs_actual == size_t(n_rcvd_msgs_or_error))
  {
    // Mainstream case.  Note that `n_rcvd_msgs_or_error > 0`; hence 'n_rcvd_msgs_actual > 0'.
    *graceful_close = false;
  }
  else
  {
    // Occurs at most once per connection.

    assert((n_rcvd_msgs_or_error > 0) && "n=0 and n=-1 should have been dealt with earlier + return.");
    *graceful_close = true;

    if (do_log_trace)
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Local_stream_batch [" << *this << "]: Connected local peer socket [" << peer_hndl << "] "
         "tried to batch-read up-to [" << n_payloads << "] in-messages, each "
         "potentially into a scattered-blob plus possibly a native handle per each; "
         "and got ostensibly [" << n_rcvd_msgs_or_error << "] of those; but only [" << n_rcvd_msgs_actual << "] were "
         "actual in-messages (0 = allowed), while the rest were EOF-markers; therefore emitting "
         "graceful-close=1 flag.");
    }
  }

  err_code->clear();

  if (total_rcvd_bytes_or_null)
  {
    *total_rcvd_bytes_or_null = total_rcvd_bytes;
  }

  m_n_used += n_rcvd_msgs_actual; // m_n_used increases here; except it might be unchanged, if *graceful_close.
  return true; // OMG.  Amazing.
} // Msg_batch_in::nb_read()

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
void Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>::to_ostream(std::ostream* os_ptr) const
{
  auto& os = *os_ptr;
  os << "Lcl[slots-total/rdy/rcvd [" << m_mdts.size() << '/' << m_mdts.capacity() << '/' << m_n_used << "]]@" << this;
}

template<typename Mutable_buffer_sequence_t, typename Msg_resource_t>
std::ostream& operator<<(std::ostream& os,
                         const Msg_batch_in<Mutable_buffer_sequence_t, Msg_resource_t>& val)
{
  val.to_ostream(&os);
  return os;
}

// Free function template implementations.

template<typename Protocol, typename Const_buffer_sequence>
size_t nb_write_some_with_native_handle(flow::log::Logger* logger_ptr,
                                        Peer_socket<Protocol>* peer_socket_ptr,
                                        Native_handle payload_hndl,
                                        const Const_buffer_sequence& payload_blob,
                                        Error_code* err_code)
{
  using util::Blob_const;
  using boost::system::system_category;
  using boost::array;
  using boost::asio::detail::buffer_sequence_adapter;
  using std::is_same_v;
  namespace sys_err_codes = boost::system::errc;
  using ::sendmsg;
  using ::msghdr;
  using ::cmsghdr;
  // using ::SOL_SOCKET; // It's a macro apparently.
  using ::SCM_RIGHTS;
  using ::MSG_DONTWAIT;
  using ::MSG_NOSIGNAL;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, (nb_write_some_with_native_handle<Protocol, Const_buffer_sequence>),
                                     logger_ptr, peer_socket_ptr, payload_hndl, payload_blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  /* To reader/maintainer: The below isn't that difficult, but if you need exact understanding and definitely if you
   * plan to make changes -- even just clarifications or changes in the doc header -- then read the entire doc header
   * of nb_read_some_with_native_handle() first. */

  assert(peer_socket_ptr);
  auto& peer_socket = *peer_socket_ptr;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  /* Let us send as much as possible of payload_blob; and the native socket payload_hndl.
   * (For Protocol_pkt_stream as much as possible -- if not none -- shall be *all* of payload_blob.)
   * As explicitly documented in boost.asio docs: it does not provide an API for the latter (fairly hairy
   * and Linux-specific) feature, but it is doable by using sendmsg() natively via peer_socket.native_handle()
   * which gets the native handle (a/k/a FD). */

  buffer_sequence_adapter<Blob_const, Const_buffer_sequence> buf_seq_adapter{payload_blob}; // See explanation below.
  const size_t payload_blob_sz = buf_seq_adapter.total_size();

  FLOW_LOG_TRACE("Connected local peer socket wants to write scattered-blob "
                 "([" << buf_seq_adapter.count() << "] sub-blobs, total size [" << payload_blob_sz << "], "
                 "first sub-blob at @[" << buf_seq_adapter.buffers()->iov_base << "]) "
                 "plus native handle [" << payload_hndl << "].  Will try to send.");

  /* What's left is to set up msg_control[len] which is how we send the native handle (payload_hndl).
   * This is based on example snippet taken from Linux `man cmsg`.  FWIW it's rather difficult to figure out
   * how to write it from the rest of the documentation, which is likely complete and correct but opaque; the
   * example is invaluable.  Note we are particularly using the SOL_SOCKET/SCM_RIGHTS technique. */

  Msg_control_as_union msg_control_as_union;
  /* Caution!  `man cmsg` in Linux says:
   * "When initializing a buffer that will contain a series of cmsghdr structures (e.g., to be sent with sendmsg(2)),
   * that buffer should first be zero-initialized to ensure the correct operation of CMSG_NXTHDR()."
   * We don't use CMSG_NXTHDR() though, so skip `msg_control_as_union.m_buf.fill(0)` here for perf.
   * Careful!  May need to add this later, if adding more types of ancillary data (but not if merely increasing
   * N_PAYLOAD_FDS). */

  msghdr sendmsg_hdr =
  {
    nullptr, // msg_name - Address; not used; we are connected.
    0, // msg_namelen - Ditto.
    /* msg_iov - Scatter/gather array.  Here we use boost.asio's neat buffer_sequence_adapter which in this OS
     * does iovec[] work for us: sets up the array and the count and computes the total size as it goes.
     * (See doc header of Msg_batch_in::Mdt_per_payload::m_buf_seq_adapter for relevant info/defense of its use.) */
    buf_seq_adapter.buffers(),
    buf_seq_adapter.count(), // msg_iovlen - # elements in msg_iov.
    // The next 2 fields msg_control[len] contain ancillary data to send; we used it for the FDs.
    msg_control_as_union.m_buf.c_array(), // Still need to load it with stuff after this (uninit for now).
    sizeof(Msg_control_as_union::m_buf),
    0 // msg_flags is unused.  The language won't let me leave it as garbage though.
  };

  // Load up .msg_control with the FDs (accessed via macro thingies).

  cmsghdr* const sendmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&sendmsg_hdr); // Or cast from &m_buf; or use &m_align....
  sendmsg_hdr_cmsg_ptr->cmsg_level = SOL_SOCKET;
  sendmsg_hdr_cmsg_ptr->cmsg_type = SCM_RIGHTS;
  sendmsg_hdr_cmsg_ptr->cmsg_len = CMSG_LEN(sizeof(Native_handle::handle_t) * N_PAYLOAD_FDS);
  // Copy the FDs.  We have just the one; simply assign (omit `memcpy`) but static_assert() to help future-proof.
  static_assert(N_PAYLOAD_FDS == 1, "Should be only passing one native handle into sendmsg() as of this writing.");
  *(reinterpret_cast<Native_handle::handle_t*>(CMSG_DATA(sendmsg_hdr_cmsg_ptr))) = payload_hndl.m_native_handle;

  const auto n_sent_or_error
    = sendmsg(peer_socket.native_handle(), &sendmsg_hdr,
              /* If socket is un-writable then don't block;
               * EAGAIN/EWOULDBLOCK instead.  Doing it this way is easier than messing with fcntl(), particularly
               * since we're working with a boost.asio-managed socket; that's actually fine -- there's a portable
               * API native_non_blocking() in boost.asio for setting this -- but feels like the less interaction between
               * portable boost.asio code we use and this native stuff, the better -- so just keep it local here. */
              MSG_DONTWAIT | MSG_NOSIGNAL);
              // ^-- Dealing with SIGPIPE is a pointless pain; if conn closed that way just give us an EPIPE error.

  if (n_sent_or_error == -1)
  {
    /* Not even 1 byte of the blob was sent; and hence nor was payload_hndl.  (Docs don't explicitly say that 2nd
     * part after the semicolon, but it's essentially impossible that it be otherwise, as then it'd be unknowable.
     * Update: Confirmed in kernel source and supported by this delightfully reassuring (in several ways) link:
     * [ https://gist.github.com/kentonv/bc7592af98c68ba2738f4436920868dc ] (Googled "SCM_RIGHTS gist").) */

    const Error_code sys_err_code{errno, system_category()};
    if ((sys_err_code == sys_err_codes::operation_would_block) || // EWOULDBLOCK
        (sys_err_code == sys_err_codes::resource_unavailable_try_again)) // EAGAIN (same meaning)
    {
      FLOW_LOG_TRACE("Write attempt indicated would-block; not an error condition.  Nothing sent.");
      /* Subtlety: We could just set it to sys_err_code; but we specifically promised in contract we'd set it to the
       * boost::asio::error::would_block representation of would-block condition.  Why did we promise that?  2 reasons:
       * 1, that is what boost.asio's own spiritually similar Peer_socket::write_some() would do in non_blocking() mode.
       * 2, then we can promise a specific code instead of making them check for the above 2 errno values.
       *
       * Subtlety: net_flow::Peer_socket::sync_receive() uses somewhat different semantics; it indicates would-block by
       * returning 0 *but* a falsy *err_code.  Why are we inconsistent with that?  Answer: Because net_flow is not
       * trying to be a boost.asio extension; we are.  In net_flow's context (as of this writing) no one is surprised
       * when semantics are somewhat different from boost.asio; but in our context they might be quite surprised
       * indeed. */
      *err_code = boost::asio::error::would_block;
      return 0;
    }
    // else

    assert(sys_err_code);

    // All other errors are fatal.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
    FLOW_LOG_WARNING("Connected local peer socket tried to write scattered-blob "
                     "([" << buf_seq_adapter.count() << "] sub-blobs, total size [" << payload_blob_sz << "], "
                     "first sub-blob at @[" << buf_seq_adapter.buffers()->iov_base << "]) "
                     "plus native handle [" << payload_hndl << "]; "
                     "but an unrecoverable error occurred.  Nothing sent.");
    *err_code = sys_err_code;
    return 0;
  } // if (n_sent_or_error == -1)
  // else if (n_sent_or_error != -1)

  if constexpr(is_same_v<Protocol, Protocol_pkt_stream>)
  {
    assert((static_cast<size_t>(n_sent_or_error) == payload_blob_sz)
           && "sendmsg() returned neither -1 nor full-dgram-sent length; unexpected behavior for SEQPACKET!  "
                "Dgram too large?  Misunderstood/misdocumented OS API?");

    FLOW_LOG_TRACE("sendmsg() reports the native handle [" << payload_hndl << "] was successfully sent; as "
                   "were all of the dgram blob's [" << payload_blob_sz << "] bytes.");
  }
  else
  {
    assert((n_sent_or_error > 0)
           && "sendmsg() should return either -1 or a positive number (indicates success).");

    FLOW_LOG_TRACE("sendmsg() reports the native handle [" << payload_hndl << "] was successfully sent; as "
                   "were [" << n_sent_or_error << "] of the blob's [" << payload_blob_sz << "] bytes.");
  }

  err_code->clear();
  return static_cast<size_t>(n_sent_or_error);
} // nb_write_some_with_native_handle()

template<typename Protocol, typename Mutable_buffer_sequence>
size_t nb_read_some_with_native_handle(flow::log::Logger* logger_ptr,
                                       Peer_socket<Protocol>* peer_socket_ptr,
                                       Native_handle* target_payload_hndl_ptr,
                                       const Mutable_buffer_sequence& target_payload_blob,
                                       Error_code* err_code,
                                       int message_flags)
{
  using flow::log::Sev;
  using boost::asio::detail::buffer_sequence_adapter;
  using boost::io::ios_all_saver;
  using boost::system::system_category;
  using boost::array;
  using std::is_same_v;
  namespace sys_err_codes = boost::system::errc;
  using ::recvmsg;
  using ::msghdr;
  using ::cmsghdr;
  // using ::SOL_SOCKET; // It's a macro apparently.
  using ::SCM_RIGHTS;
  using ::MSG_DONTWAIT;
  using ::MSG_TRUNC;
  using ::MSG_CTRUNC;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, (nb_read_some_with_native_handle<Protocol, Mutable_buffer_sequence>),
                                     logger_ptr, peer_socket_ptr, target_payload_hndl_ptr,
                                     target_payload_blob, _1, message_flags);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  /* To reader/maintainer: The below isn't that difficult, but if you need exact understanding and definitely if you
   * plan to make changes -- even just clarifications or changes in the doc header -- then read the entire doc header
   * of nb_read_some_with_native_handle() first. */

  assert(peer_socket_ptr);
  assert(target_payload_hndl_ptr);

  buffer_sequence_adapter<util::Blob_mutable, Mutable_buffer_sequence> buf_seq{target_payload_blob};

  auto& peer_socket = *peer_socket_ptr;
  auto& target_payload_hndl = *target_payload_hndl_ptr;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  target_payload_hndl = {}; // We promised to set to this if no native-handle received (including on error).

  /* Let us receive as much as possible into buf_seq area up to its size; and a native socket (if any) into
   * target_payload_hndl (if none, then it'll remain null()).
   * As explicitly documented in boost.asio docs: it does not provide an API for the latter (fairly hairy
   * and Linux-specific) feature, but it is doable by using recvmsg() natively via peer_socket.native_handle()
   * which gets the native handle (a/k/a FD).
   *
   * Recommending first looking at nb_write_some_with_native_handle(), as we operate symmetrically/similarly. */

  const size_t target_payload_blob_sz = buf_seq.total_size();

  /* Set up .msg_control*.  This, and then interpreting the output after the call, is based on the out-equivalent in
   * nb_write_some_with_native_handle() as well as cross-referencing with some Internet sources.
   *
   * Before the call, we must reserve space for the ancillary out-data, if any; set msg_control to point to that;
   * and msg_controllen to the size of that thing. */

  Msg_control_as_union msg_control_as_union;
  /* Caution!  `man cmsg` in Linux says:
   * "When initializing a buffer that will contain a series of cmsghdr structures (e.g., to be sent with sendmsg(2)),
   * that buffer should first be zero-initialized to ensure the correct operation of CMSG_NXTHDR()."
   * (We are recv*(), not send*(), but seems it would apply too... unless kernel would do it in that case.)
   * We don't use CMSG_NXTHDR() though, so skip `msg_control_as_union.m_buf.fill(0)` here for perf.
   * Careful!  May need to add this later, if adding more types of ancillary data (but not if merely increasing
   * N_PAYLOAD_FDS). */

  msghdr recvmsg_hdr =
  {
    nullptr, // msg_name - Address; not used; we are connected.
    0, // msg_namelen - Ditto.
    /* msg_iov - Scatter/gather array.  See similarly-themed notes at similar
     * spot in nb_write_some_with_native_handle(); they apply here too, more or less. */
    buf_seq.buffers(),
    // msg_iovlen - # elements in msg_iov.  This is an *input* arg only; output is total length read: the ret value.
    buf_seq.count(),
    /* The following fields:
     * msg_control - In-arg; points to complex ancillary data area (which can only be properly navigated using
     *   CMSG* macros) which is an out-arg of sorts.  This value itself is an in-arg however.
     * msg_controllen - In-arg *and* out-arg.  As in-arg, it's the length of area pointed by msg_control.
     *   As out-arg, it describes the size of resulting ancillary data; however it is not to be itself checked
     *   or used apparently; CMSG* macros shall be used to traverse ancillary data.  Nevertheless msg_controllen
     *   can/will be set by recv[m]msg() (not important for us here but can be important if reusing inputs in
     *   a subsequent calls; see class Msg_batch_in).
     * msg_flags - This is an out-arg containing special feature flags.  These may be checked below after call. */
    msg_control_as_union.m_buf.c_array(),
    sizeof(Msg_control_as_union::m_buf),
    0 // May or may not need to be initialized, but the language won't let me avoid it, and anyway it seems prudent.
  };

  // Massage message_flags for our required purposes.  Re. message_flags please read notes in our doc header.

  /* If socket is un-readable then don't block; EAGAIN/EWOULDBLOCK instead. ...Further comment omitted;
   * see sendmsg() elsewhere in this header.  Same thing here. */
  if constexpr(is_same_v<Protocol, Protocol_pkt_stream>)
  {
    // For dgram-based protocol: Also use MSG_TRUNC, so we can (at least) log some info on overflow.
    message_flags = (message_flags | (MSG_DONTWAIT | MSG_TRUNC));
  }
  else
  {
    message_flags = (message_flags | MSG_DONTWAIT);
  }

  const auto n_rcvd_or_error
    = recvmsg(peer_socket.native_handle(), &recvmsg_hdr, message_flags);

  // Carefully check all the outputs.  Return value first.

  if (n_rcvd_or_error == -1)
  {
    /* Not even 1 byte of a blob was read; and hence nor was any target_payload_hndl.  (See comment in similar
     * spot in nb_write_some_with_native_handle(); applies here equally.) */

    const Error_code sys_err_code{errno, system_category()};
    if ((sys_err_code == sys_err_codes::operation_would_block) || // EWOULDBLOCK
        (sys_err_code == sys_err_codes::resource_unavailable_try_again)) // EAGAIN (same meaning)
    /* Reader: "But, ygoldfel, why didn't you just check errno itself against those 2 actual Evalues?"
     * ygoldfel: "Because it makes me feel better about portable-ish style.  Shut up, that's why.  Get off my lawn." */
    {
      FLOW_LOG_TRACE("Connected local peer socket tried to read into scattered-blob "
                     "([" << buf_seq.count() << "] sub-blobs, "
                     "up to total size [" << target_payload_blob_sz << "], "
                     "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                     "plus possibly a native handle; and read attempt indicated would-block; "
                     "not an error condition.  Nothing received.");
      // Subtlety x 2: ...omitted.  See similar spot in nb_write_some_with_native_handle().  Same here.
      *err_code = boost::asio::error::would_block;
      return 0; // target_payload_hndl already set.
    }
    // else

    assert(sys_err_code);

    // All other errors are fatal.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
    FLOW_LOG_WARNING("Connected local peer socket tried to read into scattered-blob "
                     "([" << buf_seq.count() << "] sub-blobs, "
                     "up to total size [" << target_payload_blob_sz << "], "
                     "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                     "plus possibly a native handle; but an unrecoverable error occurred.  Nothing received.");
    *err_code = sys_err_code;
    return 0; // target_payload_hndl already set.
  } // if (n_rcvd_or_error == -1)
  // else if (n_rcvd_or_error != -1)

  if (n_rcvd_or_error == 0)
  {
    /* WARNING doesn't feel right: it's a graceful connection end.
     * INFO could be good, but it might be too verbose depending on the application.
     * Use TRACE to be safe; caller can always log differently if desired. */
    FLOW_LOG_TRACE("Connected local peer socket tried to read into scattered-blob "
                   "([" << buf_seq.count() << "] sub-blobs, "
                   "up to total size [" << target_payload_blob_sz << "], "
                   "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                   "plus possibly a native handle; "
                   "but it returned EOF meaning orderly connection shutdown by peer.  Nothing received.");
    *err_code = boost::asio::error::eof;
    return 0; // target_payload_hndl already set.
  }
  // else if (n_rcvd_or_error > 0):

  err_code->clear(); // Used by some checks below to mean "no error condition found yet."

  // Check for in-dgram overflow (if applicable).
  if constexpr(is_same_v<Protocol, Protocol_pkt_stream>)
  {
    if (size_t(n_rcvd_or_error) > target_payload_blob_sz) // This is detectable due to *input* flag MSG_TRUNC.
    {
      /* As advertised log a WARNING and treat it as an error.  Officially we said the in-dgram is eaten, and it is;
       * but in reality also a truncated in-dgram is now in the target buffer(s), though we didn't mention that. */
      FLOW_LOG_WARNING("Connected local peer socket tried to read into scattered-blob "
                       "([" << buf_seq.count() << "] sub-blobs, "
                       "up to total size [" << target_payload_blob_sz << "], "
                       "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                       "plus possibly a native handle; "
                       "and it returned it read [" << n_rcvd_or_error << "] bytes successfully, but this overflows "
                       "the aforementioned total-size; emitting error accordingly.  Socket remains operational, but "
                       "the overflowing in-dgram is lost.");

      // Sanity-check that the *output* flag MSG_TRUNC is active (<=> ret value shows overflow, per docs + tests).
      assert((recvmsg_hdr.msg_flags & MSG_TRUNC)
             && "In-dgram size overflows user-supplied buffer <=> MSG_TRUNC out-flag supposed to be set.");

      *err_code = error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE;
      /* target_payload_hndl already set.
       * Would `return 0` here but want to avoid leaking received handles, so defer until below. */
    }
    // else { No overflow. }
  }
  // else { No overflow possible in stream mode. }

  /* Assuming no error found yet:
   *
   * Next, buf_seq-pointed area... which is already written to (its first n_rcvd_or_error bytes).  Nothing to do.
   *
   * Next, recvmsg_hdr.msg_flags.  Basically only the following is relevant: */
  if ((!*err_code) && (recvmsg_hdr.msg_flags != 0))
  {
    if (logger_ptr && logger_ptr->should_log(Sev::S_INFO, get_log_component()))
    {
      ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::hex/etc. soon.
      FLOW_LOG_INFO_WITHOUT_CHECKING
        ("Connected local peer socket tried to read into scattered-blob "
         "([" << buf_seq.count() << "] sub-blobs, "
         "up to total size [" << target_payload_blob_sz << "], "
         "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
         "plus possibly a native handle; "
         "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also returned raw "
         "out-flags value [0x" << std::hex << recvmsg_hdr.msg_flags << "].  "
         "Will check for relevant flags but otherwise "
         "ignoring if nothing bad.  Logging at elevated level because it's interesting; please investigate.");
    }

    if ((recvmsg_hdr.msg_flags & MSG_CTRUNC) != 0)
    {
      if (logger_ptr && logger_ptr->should_log(Sev::S_WARNING, get_log_component()))
      {
        ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::hex/etc. soon.
        FLOW_LOG_WARNING_WITHOUT_CHECKING
          ("Connected local peer socket tried to read into scattered-blob "
           "([" << buf_seq.count() << "] sub-blobs, "
           "up to total size [" << target_payload_blob_sz << "], "
           "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
           "plus possibly a native handle; "
           "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also returned raw "
           "out-flags value [0x" << std::hex << recvmsg_hdr.msg_flags << "] which includes MSG_CTRUNC.  "
           "That flag indicates more stuff was sent as ancillary data; but we expect at most 1 native "
           "handle.  Other side sent something strange.  Acting as if nothing received + error.");
      }
      *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
      /* target_payload_hndl already set.
       * Would `return 0` here but want to avoid leaking received handles, so defer until below. */
    }
    // else { No truncation occurred. }
  } // if ((!*err_code) && (recvmsg_hdr.msg_flags != 0)) (but *err_code may have become truthy inside)

  /* Lastly examine ancillary data (which might have also been truncated: MSG_CTRUNC above).
   * Use, basically, the method from `man cmsg` in Linux.
   *
   * We are interested whether *err_code (un-leak any received hndl) or !*err_code (emit any received hndl). */
  cmsghdr* const recvmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&recvmsg_hdr);
  if (recvmsg_hdr_cmsg_ptr)
  {
    // There is some ancillary data.  It can only (validly according to our expected protocol) be one thing.
    if ((recvmsg_hdr_cmsg_ptr->cmsg_level == SOL_SOCKET) &&
        (recvmsg_hdr_cmsg_ptr->cmsg_type == SCM_RIGHTS))
    {
      static_assert(N_PAYLOAD_FDS == 1,
                    "Should be only dealing with one native handle with recvmsg() as of this writing.");
      target_payload_hndl.m_native_handle
        = *(reinterpret_cast<const Native_handle::handle_t*>(CMSG_DATA(recvmsg_hdr_cmsg_ptr)));
      if (*err_code)
      {
        target_payload_hndl.close(); // Avoid the native-handle (which is a received copy) leak.
      }
      // else { Cool: Native-handle received and emitted. }
    }
    else if (!*err_code) // && (unknown ancillary data type)
    {
      FLOW_LOG_WARNING("Connected local peer socket tried to read into scattered-blob "
                       "([" << buf_seq.count() << "] sub-blobs, "
                       "up to total size [" << target_payload_blob_sz << "], "
                       "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                       "plus possibly a native handle; "
                       "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also "
                       "unexpected ancillary data of cmsg_level|cmsg_type "
                       "[" << recvmsg_hdr_cmsg_ptr->cmsg_level << '|' << recvmsg_hdr_cmsg_ptr->cmsg_type << "].  "
                       "Acting as if nothing received + error.");
      *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
      // target_payload_hndl already set.  We shall return below.
    }
    // else if (*err_code && (unknown ancillary data type)) { Detected error before; stick with that one. }

    /* Used to check here that CMSG_NXTHDR() would yield null (meaning no more ancillary data) and issued
     * LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL if not.  However it was a perf giveaway that was likely pointless,
     * even with a misbehaving opposing peer; we've only provided space for 1 FD, and MSG_CTRUNC (checked above) would
     * have pointed out if that were insufficient.  So just move on. */
  }
  // else { No ancillary data, meaning no native handle; that's quite normal. }

  if (*err_code)
  {
    return 0; // Any potential handle-leak avoided; can get out.
  }
  // else:

  if constexpr(is_same_v<Protocol, Protocol_pkt_stream>)
  {
    FLOW_LOG_TRACE("Connected local peer socket tried to read into scattered-blob "
                   "([" << buf_seq.count() << "] sub-blobs, "
                   "up to total size [" << target_payload_blob_sz << "], "
                   "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                   "plus possibly a native handle; recvmsg() reports receipt of possible "
                   "native handle [" << target_payload_hndl << "]; as well as "
                   "an in-dgram taking [" << n_rcvd_or_error << "] of the "
                   "blob's [" << target_payload_blob_sz << "]-byte capacity.");
  }
  else
  {
    FLOW_LOG_TRACE("Connected local peer socket tried to read into scattered-blob "
                   "([" << buf_seq.count() << "] sub-blobs, "
                   "up to total size [" << target_payload_blob_sz << "], "
                   "first sub-blob at @[" << buf_seq.buffers()->iov_base << "]) "
                   "plus possibly a native handle; recvmsg() reports receipt of possible "
                   "native handle [" << target_payload_hndl << "]; as well as "
                   "[" << n_rcvd_or_error << "] of the blob's [" << target_payload_blob_sz << "]-byte capacity.");
  }

  return static_cast<size_t>(n_rcvd_or_error);
} // nb_read_some_with_native_handle()

// Opt_peer_process_credentials template implementations.

template<typename Protocol>
int Opt_peer_process_credentials::level(const Protocol&) const
{
  return AF_LOCAL;
}

template<typename Protocol>
int Opt_peer_process_credentials::name(const Protocol&) const
{
  return SO_PEERCRED;
}

template<typename Protocol>
void* Opt_peer_process_credentials::data(const Protocol&)
{
  return static_cast<void*>(static_cast<util::Process_credentials*>(this));
}

template<typename Protocol>
size_t Opt_peer_process_credentials::size(const Protocol&) const
{
  return sizeof(util::Process_credentials);
}

template<typename Protocol>
void Opt_peer_process_credentials::resize(const Protocol& proto, size_t new_size_but_really_must_equal_current) const
{
  using flow::util::ostream_op_string;
  using std::length_error;

  if (new_size_but_really_must_equal_current != size(proto))
  {
    throw length_error(ostream_op_string
                         ("Opt_peer_process_credentials does not actually support resizing; requested size [",
                          new_size_but_really_must_equal_current, "] differs from forever-size [",
                          size(proto), "].  boost.asio internal bug or misuse?"));
  }
}

} // namespace ipc::transport::asio_local_stream_socket
