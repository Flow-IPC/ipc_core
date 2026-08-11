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

#include "ipc/transport/native_socket_stream_cfg.hpp"
#include "ipc/transport/blob_transport_stats.hpp"
#include "ipc/transport/detail/transport_fwd.hpp"
#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>
#include <flow/util/stat/histo.hpp>
#include <type_traits>
#include <boost/array.hpp>
#include <boost/move/unique_ptr.hpp>

namespace ipc::transport
{

// Types.

/**
 * `Msg_batch_in` concept implementation for at least sync_io::Native_socket_stream (as taken by APIs
 * Native_socket_stream::async_receive_blob_batch() and Native_socket_stream::async_receive_native_handle_batch())
 * for high-speed *natively batched* receiving.
 *
 * @see Native_handle_receiver concept doc header for an explantion of receive-batching (including what
 *      native batching is; how `Msg_resource_t` template-param relates to things); and how the general design
 *      enables high perf.
 *
 * It features the following public APIs as of this writing:
 *
 *   - For user (including internal user struc::sync_io::Channel): `size_t max_msg_count`-taking ctor,
 *     initialized(), n_used(), full(), clear_used(), prepare_target_payload(), target_payload_size(),
 *     result_payload_blob(), result_payload_hndl().
 *   - For receiver-engine, namely Native_socket_stream internals: a `private` `this->` API.
 *     - The expected order of ops for each batched-read (after reaching initialized() originally) is:
 *       -# User invokes sync_io::Native_socket_stream::async_receive_blob_batch() or
 *          sync_io::Native_socket_stream::async_receive_native_handle_batch() (or similar) which internally invokes
 *          a private `*this` API (spoiler alert: `nb_read()`).
 *       -# User consumes results: result_payload_blob() and possibly result_payload_hndl() (the latter only
 *          in the `async_receive_native_handle_batch()` case).
 *       -# User resets for next batch-read:
 *          -# Save N = n_used().
 *          -# clear_used().
 *          -# prepare_target_payload() (for each of the first N slots only).
 *
 * @internal
 * ### Priviliged access to nb_read() ###
 * nb_read() is the `private` API that performs the actual reading-into `*this`, after the user has prepped
 * via prepare_target_payload() et al and before they consume the results of the read.
 *
 * Flow-IPC internals may access it through `detail/`-residing `friend` facade type
 * Native_socket_stream_msg_batch_in_privileged.  It is very simple.
 *
 * ### Impl notes ###
 * Internally a `*this` just stores a asio_local_stream_socket::Msg_batch_in called #m_batch.  It is the
 * general workhorse dealing with the local socket and native OS API(s) and so on.  We "just" build slightly
 * on top of that to support our specific *slightly* higher-level protocol which adds some special control messages
 * needed for sync_io::Native_socket_stream, like auto-ping and graceful-close.
 *
 * To understand `*this` impl, then, it should be sufficient to grok asio_local_stream_socket::Msg_batch_in
 * and then read inline comments in `*this`.
 *
 * @endinternal
 *
 * @tparam Msg_resource_t
 *         Same as for Generic_msg_batch_in.
 */
template<typename Msg_resource_t>
class Native_socket_stream_msg_batch_in :
  private boost::noncopyable
{
public:
  // Types.

  /// Alias for template param `Msg_resource_t`.
  using Msg_resource = Msg_resource_t;

  /// Buffer(s) description type (conforming to boost.asio `MutableBufferSequence` concept) supported by `*this`.
  using Mutable_buffer_sequence = util::Blob_mutable;

  // Constructors/destructor.

  /**
   * Identical to ctor of Generic_msg_batch_in.
   * @param max_msg_count
   *        See above.
   */
  explicit Native_socket_stream_msg_batch_in(size_t max_msg_count);

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
   * Identical to Generic_msg_batch_in.
   * @return See above.
   */
  size_t n_used() const;

  /**
   * Identical to Generic_msg_batch_in.
   * @return See above.
   */
  bool full() const;

  /// Identical to Generic_msg_batch_in.
  void clear_used();

  /**
   * Identical to Generic_msg_batch_in.
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
   * Identical to Generic_msg_batch_in.
   * @return See above.
   */
  size_t target_payload_size() const;

  /**
   * Identical to Generic_msg_batch_in.
   * @param idx
   *        See above.
   * @param msg_resource_ptr
   *        See above.
   * @return See above.
   */
  size_t result_payload_blob(size_t idx, Msg_resource** msg_resource_ptr = nullptr);

  /**
   * Identical to Generic_msg_batch_in.
   * @param idx
   *        See above.
   * @return See above.
   */
  Native_handle result_payload_hndl(size_t idx) const;

  /**
   * Identical to Generic_msg_batch_in.
   * @param os
   *        See above.
   */
  void to_ostream(std::ostream* os) const;

  /**
   * Stats: Histogram tracking how many raw in-dgrams were received by each raw OS batch-read (`recvmmsg()` in Linux).
   * The would-block outcome results in the value 0 (and should be rare to non-existent, if a read is engaged only
   * when the kernel-socket event-tracking system reports readiness).  So does a socket error, but this would
   * hose the socket usually.  1 means no actual batching occurred (only 1 message was ready).
   *
   * @return See above.
   */
  const flow::util::stat::Histogram_counter& histo_raw_read_n_msgs() const;

  /**
   * Stats: Histogram tracking how many of the in-dgrams counted by histo_raw_read_n_msgs() were then filtered-out as
   * not representing user-reportable in-messages; as of this writing either auto-ping or graceful-close tokens.
   *
   * A graceful-close token appears at most once per `*this`.  An auto-ping token can be ubiquitous but only under
   * low-traffic conditions, as auto-pings are sent only when a connection is not being kept-alive by user
   * traffic (e.g., if the auto-ping interval is 5 sec, then an auto-ping would be sent 5 sec after the last
   * user message or auto-ping, whichever was last).
   *
   * Hence, outside of short-lived connections and low-traffic ones, this histogram should show results near zero.
   * It can be viewed as a sanity-check of the algorithm.
   *
   * @return See above.
   */
  const flow::util::stat::Histogram_counter& histo_raw_read_n_msgs_filtered_out() const;

  /**
   * Stats: Histogram tracking how many OS batch-reads (`recvmmsg()` in Linux) resulted from each
   * Native_socket_stream::async_receive_blob_batch() or Native_socket_stream::async_receive_native_handle_batch()
   * call.  The outcome 1 is the minimum and should be by far the most frequent.  A 2nd read would occur if and only
   * if *all* of the following happened in the 1st read:
   *   - `this->full() == true` immediately following it (filled entire `*this`, so don't know if we're now in
   *     would-block or happened to grab exactly the number of in-dgrams that were ready);
   *   - at least 1 of those was auto-ping or graceful-close (or any other non-user-message dgram, if the protocol
   *     features more such possibilities; not as of this writing).
   *
   * A 3rd read would occur if and only if this happened again; and so forth.
   *
   * Hence, outside of short-lived connections and low-traffic ones, this histogram should show results near 1.
   * It can be viewed as a sanity-check of the algorithm.
   *
   * @return See above.
   */
  const flow::util::stat::Histogram_counter& histo_usr_read_n_raw_reads() const;

private:
  // Friends.

  /// Facade type that exposes specific `private` APIs of `*this` to other internal Flow-IPC code.
  friend struct Native_socket_stream_msg_batch_in_privileged<Native_socket_stream_msg_batch_in>;

  // Types.

  /**
   * Alias (for more accurate naming in context; brevity) for type of the message-type internal field, sent/received
   * in as the first bytes of every valid dgram.  (Despite the mention of length in the alias target, in our case
   * this has nothing to do with any length of anything; but the same type is used in the alternative non-dgram-based
   * mode, and there it can be a length; see Native_socket_stream_cfg if you want to know more... but really we don't
   * care here other than the fact it's mandatory to use the same type for both modes/purposes, as some at least
   * send-side common code in Native_socket_stream impl handles both modes.)
   */
  using msg_type_t = Native_socket_stream_cfg::low_lvl_payload_blob_length_t;

  /**
   * The `Msg_resource` type attached to each slot of our internal #m_batch.  Naturally this includes the user's
   * chosen #Msg_resource type; but we also add the backing (tiny) area where the first in-dgram bytes are always
   * written, containing the #msg_type_t.
   */
  struct Msg_resource_impl
  {
    /// Per-slot resource moved from user's object via `prepare_target_payload()`.
    Msg_resource m_msg_resource;

    /**
     * This slot's potential/actual in-dgram's message-type field, where the actual OS receive-batch call
     * shall place (and/or has placed) the first bytes of the in-dgram.
     *
     * ### Rationale for indirect storage via smart-pointer handle ###
     * There is exactly one reason it's not simply `msg_type_t m_msg_type;`: the address of the leading
     * scatter/gather buffer (that we pass-up to #m_batch in our prepare_target_payload() wrapper around
     * asio_local_stream_socket::Msg_batch_in::prepare_target_payload()) will continue to be accurate,
     * even as `*this` Msg_resource_impl is moved in memory.  This requirement is documented on
     * the template param's doc in `Msg_batch_in` class doc header; the explanation is around there too
     * (spoiler alert: `reuse_result_payloads()`, which we use, is a culprit; as is the initial load
     * via `prepare_target_payload(Msg_resource_impl&&)` -- which is unavoidable as-is).
     *
     * The main cost is that of [de]allocating these little guys per-`prepare_target_payload()`.  Perf-wise it's
     * okay, but actually it is a bit annoying nevertheless.  Could it be avoided?  Formally, as-is, no:
     * it's a requirement as noted above.  Could we eliminate the requirement?  Maybe, but it is
     * no joke: to begin with, the #Mutable_buffer_sequence_impl given to prepare_target_payload() has to remain
     * accurate, as our `Msg_resource_impl` is moved-into #m_batch.  Then there are the swaps via
     * reuse_result_payloads().  At the very least some serious thinking is involved (let's not try to do it here).
     *
     * That said let's have some perspective.  Yes, it's a tiny buffer, but it's a buffer; it is pretty normal
     * to expect receive-APIs (even as relatively complex as ours here) to assume the target buffer(s) do not
     * move around in memory.  It is probably just an acceptable cost in the end.
     */
    boost::movelib::unique_ptr<msg_type_t> m_msg_type;
  }; // struct Msg_resource_impl

  /**
   * Scatter/gather structure subsuming the user's "big" `Blob_mutable` preceded by our internal-needs
   * tiny #msg_type_t prefix.
   */
  using Mutable_buffer_sequence_impl = boost::array<util::Blob_mutable, 2>;

  // Methods.

  /**
   * The API for the receiver-engine (sync_io::Native_socket_stream) to perform the core batch-receive.
   * Access through internal facade helper type Native_socket_stream_msg_batch_in_privileged.
   *
   * This guy does the bulk of (batched) receive-side logic for sync_io::Native_socket_stream (including
   * even dealing with its internal Protocol_negotiator receive-side).  So all that big-guy has to do is
   * manage getting to non-would-block, and knowing to abandon the pipe on error; once it is time to read
   * (even at the very start), this `nb_read()` will take care of business.
   *
   * However!  It is very important to understand this `nb_read()`'s contract semantics -- particularly around
   * how reading data and emitting errors works when combined.  Please read carefully.
   *
   * So: nb_read() performs a high-perf non-blocking batched-receive into the batch area with indices
   * [n_used(), `max_msg_count`).  See ctor for the latter.  Usually, but not necessarily, `this->n_used() == 0`,
   * as usually one would clear_used() first, or `*this` was just initialized().  Returns `false` if
   * the aforementioned range is empty -- that is full() is `true` and no-ops modulo logging; otherwise returns
   * `true` and proceeds.  Further documentation assumes the latter.
   *
   * Let `n_payloads` be the size of the aforementioned range (thus `n_payloads >= 1`).  This receives
   * at most `n_payloads` user in-messages.  If no error but no in-messages are pending, it is would-block
   * (see specific code below; *note* we choose to emit the low-level boost.asio code, not
   * error::Code::S_SYNC_IO_WOULD_BLOCK; performing the translation is up to the caller).
   * If no error, and messages were pending, the number is not explicitly returned
   * but rather reflected in n_used() increasing by that # (at least 1, at most `n_payloads`).  If error,
   * and no messages were pending, emits that error and leaves n_unused() unchanged.  If messages were received,
   * and *then* an error was detected, then emits that error *and* modifies n_unused() as described earlier
   * in the paragraph.
   *
   * @warning Read that again please! It *is* a possible outcome that messages were emitted (n_unused() changed)
   *          *and* an error is emitted.  We do *not* defer the error as some other APIs.  (Rationale omitted but
   *          trust us.)  This possibility includes *exactly* the following errors:
   *          (1) `Native_socket_stream`-protocol graceful-close (error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE) and
   *          (2) low-level graceful-close a/k/a EOF (`boost::asio::error::eof`).  The caller of course can (and as of
   *          this writing does) do the deferring themselves if appropriate.  All errors outside of those 2 are *not*
   *          combined with emitting 1+ messages and can informally be thought of as truly exceptional; though
   *          informally it is likely best not to rely on this in your code.
   *
   * @note Be aware of the *implied would-block* condition.  See full() doc header.  Spoiler alert:
   *       if we yielded data/no error, then it wasn't would-block... but if `full() == true` post-op, then
   *       the in-pipe is nevertheless in would-block state.  It would be wasteful to try another nb_read().
   *       Conversely, though, if `!full()` then you can and should nb_read() again (assuming the goal is to
   *       drain the in-pipe such as in edge-triggered loops).
   *
   * ### Additional context, for Native_socket_stream implementer/maintainer ###
   * Arguably the most complex thing this does is emit only user in-messages while
   * transparently but performantly filtering-out control messages (including auto-ping and graceful-close)
   * For completeness/context here is how some control messages are handled.  Arguably these are impl details,
   * but we're all friends (but not `friend`s) here.
   *
   *   - Leading Protocol_negotiator stuff is checked as appropriate (emitting negotiation error if appropriate,
   *     continuing transparently as appropriate).
   *   - Auto-pings are ignored (their slots are reset for the next nb_read() performantly)
   *     except implying non-idleness (see `not_idle_on_would_block` arg).
   *   - Our-protocol graceful-close message is detected and causes emission of
   *     error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE (as to be delivered to user).
   *
   * @param logger_ptr
   *        Logger to which to log (or null).
   * @param peer_socket_ptr
   *        The (presumably) connected dgram-based socket.
   * @param protocol_negotiator
   *        The Native_socket_stream protocol-negotiator which is fully managed by this method.
   * @param no_hndls
   *        If `false`, receiving a native handle in an in-dgram is normal and is performed when relevant;
   *        if `true`, receiving a native handle triggers the appopriate error.
   * @param not_idle_on_would_block
   *        Out-arg which has meaning if and only if we emit `boost::asio::error::would_block`; in that
   *        case: `true` means that, although (per `would_block` definition above) no user in-messages were
   *        read + no further data are pending, nevertheless internally *some* data *were* received
   *        (as of this writing: auto-ping(s)), and therefore the connection is not idle per
   *        the Blob_receiver concept definition of "idle."  (Any other non-error-emitting outcome by definition
   *        means user in-messages were received and thus => not idle.)  (Hence `false` means that given it's a
   *        would-block, also no traffic at all internally was received.)
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_INVALID_ARGUMENT (`!this->initialized()` at entry);
   *        error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER (illegal in-dgram: other side misbehaved);
   *        error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB (`no_hndls == true`, but other side sent a handle: misbehaved);
   *        error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE (graceful-close received: other side did
   *          `Blob_sender::async_end_sending()` or equivalent);
   *        those emitted by asio_local_stream_socket::Msg_batch_in::nb_read();
   *        `boost::asio::error::would_block` (none of the above errors apply -- including `eof` error from prev bullet
   *          point, and not even 1 user in-message was pending).
   * @param message_flags
   *        See nb_read_some_with_native_handle().
   * @param rcv_stats
   *        If not null, `nb_read()` accumulates into `*rcv_stats`.
   * @return `false` if pre-condition `!this->full()` is failed; `true` otherwise.  See above.
   */
  bool nb_read(flow::log::Logger* logger_ptr,
               asio_local_stream_socket::Peer_socket<asio_local_stream_socket::Protocol_pkt_stream>* peer_socket_ptr,
               Protocol_negotiator* protocol_negotiator,
               bool no_hndls, bool* not_idle_on_would_block, Error_code* err_code = nullptr, int message_flags = 0,
               stat::Blob_rcv_stats* rcv_stats = nullptr);

  /**
   * nb_read() helper: Checks in-dgram at given index, returning `true` if and only if the dgram represents anything
   * except a valid message suitable to pass-through to Native_socket_stream::async_receive_blob_batch() (et al)
   * user.
   *
   * @param logger_ptr
   *        See nb_read().
   * @param idx
   *        See result_payload_blob().
   * @param no_hndls
   *        See nb_read().
   * @return `false` if vanilla user in-message, `true` otherwise.
   */
  bool is_not_user_message(flow::log::Logger* logger_ptr, size_t idx, bool no_hndls) const;

  // Data.

  /**
   * The natively-batched batch containing target buffer(s) from the user plus a bit of added buffer-age of ours
   * (namely the #msg_type_t per slot) needed to support the Native_socket_stream protocol.
   */
  asio_local_stream_socket::Msg_batch_in<Mutable_buffer_sequence_impl, Msg_resource_impl> m_batch;

  /**
   * Determined permanently by the first prepare_target_payload(), this is the max size in bytes of each
   * in-message (per slot); 0 before then.
   */
  size_t m_target_payload_sz;

  /// See histo_raw_read_n_msgs().
  flow::util::stat::Histogram_counter m_histo_raw_read_n_msgs;

  /// See histo_raw_read_n_msgs_filtered_out().
  flow::util::stat::Histogram_counter m_histo_raw_read_n_msgs_filtered_out;

  /// See histo_usr_read_n_raw_reads().
  flow::util::stat::Histogram_counter m_histo_usr_read_n_raw_reads;

  /**
   * The results in #m_histo_raw_read_n_msgs et al shall be logged opportunistically inside `nb_read()` once
   * the time passes this time-point; `zero()` indicates `nb_read()` has not yet been called, so this will be
   * first set when it is called.  Always `zero()` and unused if
   * Native_socket_stream_cfg::S_STATS_LOG_ENABLED is `false`.
   */
  util::Fine_time_pt m_stats_next_output_when;
}; // class Native_socket_stream_msg_batch_in

// Template implementations.

template<typename Msg_resource_t>
Native_socket_stream_msg_batch_in<Msg_resource_t>::Native_socket_stream_msg_batch_in(size_t max_msg_count) :
  m_batch(max_msg_count),
  m_target_payload_sz(0),
  /* A bucket for 0 in-dgrams (would-block), then buckets for all outcomes up to and including getting the max
   * (each one covering BUCKET_SZ adjacent incomes; e.g., 2 => [1, 2][3, 4][5, 6]...[63, 64]). */
  m_histo_raw_read_n_msgs(1 + (max_msg_count / Native_socket_stream_cfg::S_STATS_HISTO_MSG_CT_BUCKET_SZ),
                          1, Native_socket_stream_cfg::S_STATS_HISTO_MSG_CT_BUCKET_SZ, 0),
  // Ditto (this covers a subset of events tracked in m_histo_raw_read_n_msgs).
  m_histo_raw_read_n_msgs_filtered_out(1 + (max_msg_count / Native_socket_stream_cfg::S_STATS_HISTO_MSG_CT_BUCKET_SZ),
                                       1, Native_socket_stream_cfg::S_STATS_HISTO_MSG_CT_BUCKET_SZ, 0),
  /* A bucket for an nb_read() leading to 1 raw read; then one for each outcome 2, 3, ..., 9; and lastly a bucket
   * for 10-or-more raw reads.  Really any result other than 1 is quite unlikely... 10+ is almost science fiction. */
  m_histo_usr_read_n_raw_reads(10, 1, 1, 1)
{
  // OK.
}

template<typename Msg_resource_t>
void Native_socket_stream_msg_batch_in<Msg_resource_t>::prepare_target_payload
       (const Mutable_buffer_sequence& target_blob, Msg_resource&& msg_resource, size_t idx)
{
  using util::Blob_mutable;

  if (target_payload_size() == 0)
  {
    m_target_payload_sz = target_blob.size();
  }
  else
  {
    assert((target_payload_size() == target_blob.size())
           && "Msg_batch_in concept requires the max message size be the same across all slots.");
  }

  /* Wrap their Msg_resource into a wrapper Msg_resource_t of our own which is the
   * the struct containing their Msg_resource and our msg_type_t value target.  And, make a simple 2-array
   * of Blob_mutable (iovec-like) things which describe this scatter/gather blob target:
   *   [msg_type_t][their Blob_mutable target]
   * So then a given in-dgram will be received into the 1st fixed-size thing (the message type), and any remaining
   * bytes (possibly none) will go to the user's target buffer.  this->nb_read() will then populate as many
   * such in-dgrams as possible -- then check each one (is it a user message? a ping? etc.).  Spoiler alert:
   * illegal in-dgrams aside, the user buffer will be touched only when it's a user in-message with meta-blob;
   * all the other legal in-messages by definition feature no meta-blob, so it won't be written-to. */

  Msg_resource_impl msg_resource_impl{ std::move(msg_resource),
                                       boost::movelib::unique_ptr<msg_type_t>(new msg_type_t{0}) };
  Mutable_buffer_sequence_impl buf_seq{ Blob_mutable{msg_resource_impl.m_msg_type.get(), sizeof(msg_type_t)},
                                        target_blob };

  m_batch.prepare_target_payload(buf_seq, std::move(msg_resource_impl), idx);
}

template<typename Msg_resource_t>
size_t Native_socket_stream_msg_batch_in<Msg_resource_t>::target_payload_size() const
{
  return m_target_payload_sz;
}

template<typename Msg_resource_t>
size_t
  Native_socket_stream_msg_batch_in<Msg_resource_t>::result_payload_blob(size_t idx,
                                                                         Msg_resource** msg_resource_ptr)
{
  assert((idx < n_used()) && "Attempt by outside user to access a result in a slot that was not received-to"
                               "and/or holds no user in-message; could also be an internal bug in *this class.");

  size_t n_rcvd;
  if (msg_resource_ptr)
  {
    Msg_resource_impl* msg_resource_impl;
    n_rcvd = m_batch.result_payload_blob(idx, &msg_resource_impl);
    *msg_resource_ptr = &(msg_resource_impl->m_msg_resource);
  }
  else // Don't waste time on grabbing msg_resource_impl, if we won't use it.
  {
    n_rcvd = m_batch.result_payload_blob(idx);
  }

  assert((n_rcvd >= sizeof(msg_type_t))
         && "Bug in *this class?  Apparently gave user access to not-a-user-in-message.");
  return n_rcvd - sizeof(msg_type_t);
}

template<typename Msg_resource_t>
Native_handle Native_socket_stream_msg_batch_in<Msg_resource_t>::result_payload_hndl(size_t idx) const
{
  return m_batch.result_payload_hndl(idx);
}

template<typename Msg_resource_t>
bool Native_socket_stream_msg_batch_in<Msg_resource_t>::initialized() const
{
  return m_batch.initialized();
}

template<typename Msg_resource_t>
size_t Native_socket_stream_msg_batch_in<Msg_resource_t>::n_used() const
{
  return m_batch.n_used();
}

template<typename Msg_resource_t>
bool Native_socket_stream_msg_batch_in<Msg_resource_t>::full() const
{
  return m_batch.full();
}

template<typename Msg_resource_t>
void Native_socket_stream_msg_batch_in<Msg_resource_t>::clear_used()
{
  m_batch.clear_used();
}

template<typename Msg_resource_t>
bool Native_socket_stream_msg_batch_in<Msg_resource_t>::nb_read
       (flow::log::Logger* logger_ptr,
        asio_local_stream_socket::Peer_socket<asio_local_stream_socket::Protocol_pkt_stream>* peer_socket_ptr,
        Protocol_negotiator* protocol_negotiator,
        bool no_hndls, bool* not_idle_on_would_block, Error_code* err_code, int message_flags,
        stat::Blob_rcv_stats* rcv_stats)
{
  using flow::log::Sev;
  using util::Blob_mutable;
  constexpr auto PING = Native_socket_stream_cfg::S_PING_SENTINEL;
  constexpr auto TYPE0 = msg_type_t{0x0000};
  constexpr auto MSG_TYPE_SZ = sizeof(msg_type_t);

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, nb_read,
                                     logger_ptr, peer_socket_ptr, protocol_negotiator, no_hndls,
                                     not_idle_on_would_block, _1, message_flags, rcv_stats);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert(not_idle_on_would_block);

  if (full())
  {
    return false; // As promised special degenerate case.
  }
  // else: Now we needn't worry about it.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  bool not_idle_val = false; // This is for *not_idle_on_would_block.

  if (protocol_negotiator->negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN)
  {
    /* No data have been received on this connection, so we've yet to do protocol negotiation in this direction.
     * Reminder: It means we must receive an in-dgram exactly MSG_TYPE_SZ bytes long which shall contain
     * a version number; and *protocol_negotiator will either accept it or spit out a truthy Error_code.
     * In the latter case we emit that and GTFO before trying to receiving anything else.
     *
     * We choose to do this with a separate 1-msg read as opposed to trying to fold it into the main m_batch.nb_read()
     * below (which accepts potentially many in-dgrams in one go).  Could we do it?  Probably yes; we could
     * detect it using is_not_user_message() below (along with auto-pings, graceful-closes, etc.) and so on.
     * However this'd break the rationale/conventions/procedure behind the Protocol_negotiator pattern; the whole
     * point is to read, ahead of anything else, one version-bearing thing whose format is agreed-upon indefinitely.
     * If it doesn't pass Protocol_negotiator check, then we don't (formally speaking anyway) know anything about
     * further messages that might be received and should not be checking anything about them -- it could even lead
     * to a crash, for example, if we make some assumption about lengths or who knows what.
     *
     * Furthermore it is probably simpler to do it separately like this; no need to worry about stuff about protocol
     * negotiation being written into user buffers... that type of thing.  Is there a perf penalty?  Answer: Not really;
     * doing a single separate read at the start of the connection is negligible. */

    Native_handle hndl_or_null;
    msg_type_t proto_ver_as_msg_type;
    const auto n_rcvd
      = asio_local_stream_socket::nb_read_some_with_native_handle<asio_local_stream_socket::Protocol_pkt_stream>
          (logger_ptr, peer_socket_ptr, &hndl_or_null, Blob_mutable{&proto_ver_as_msg_type, MSG_TYPE_SZ},
           err_code, message_flags);
    if (*err_code)
    {
      /* Fatal error => obviously overall fatal error.  Would-block => can't read any more messages either.
       * So we are done here.  Though in would-block case a contract subtlety requires the following. */
      (*err_code == boost::asio::error::would_block) && (*not_idle_on_would_block = true); // Contract fulfilled.
      return true;
    }
    /* else: Got something (an in-dgram and possibly a handle).  Check for various protocol misbehaviors first.
     * Caveat: We only gave it a msg_type_t-sized target buffer; and as of this writing if any more bytes were
     * received in dgram (1) they're simply thrown out and (2) we are not notified.  So that particular misbehavior
     * we simply do not worry about... it's fine; this is a safety check -- not a security check (discussion
     * elsewhere about that philosophy). */
    if (n_rcvd < MSG_TYPE_SZ)
    {
      *err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
      FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                       "Native_socket_stream batch nb-read of protocol-negotiation dgram: "
                       "illegal too-short in-dgram.");
      return true;
    }
    // else if (n_rcvd == MSG_TYPE_SZ):

    if (!hndl_or_null.null())
    {
      FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                       "Native_socket_stream batch nb-read of protocol-negotiation dgram: "
                       "Expecting protocol-negotiation (first) in-dgram "
                       "to contain *only* a meta-blob: but received Native_handle is non-null which is "
                       "unexpected.");
  #ifndef NDEBUG
      const bool ok =
  #endif
      protocol_negotiator->compute_negotiated_proto_ver(Protocol_negotiator::S_VER_UNKNOWN, err_code);
      assert(ok && "Protocol_negotiator breaking contract?  Bug?");
      assert(*err_code
             && "Protocol_negotiator should have emitted error given intentionally bad version.");
      return true;
    } // if (hndl_or_null)
    // else: All cool format-wise; check the version finally.

    /* Protocol_negotiator handles everything (invalid value, incompatible range...); we just know
     * the encoding is to shove the version number into what is normally the msg_type_t field. */
#ifndef NDEBUG
    const bool ok =
#endif
    protocol_negotiator->compute_negotiated_proto_ver
      (static_cast<Protocol_negotiator::proto_ver_t>(proto_ver_as_msg_type), err_code);
    assert(ok && "Protocol_negotiator breaking contract?  Bug?");

    if (*err_code)
    {
      return true; // Protocol negotiation did not pass.  It logged.
    }
    // else: Cool; done with protocol negotiation forever (it is marked in *protocol_negotiator).

    not_idle_val = true; // Certainly we got an in-message.

    // Protocol-negotiation dgram = MSG_TYPE_SZ low-level bytes (not a user message though).
    rcv_stats && (rcv_stats->m_total_low_lvl_bytes += MSG_TYPE_SZ);

    /* Now what?  Answer: We asked for 1 msg and got 1 msg; so now it might be in would-block or not.  Essentially
     * it's as-if protocol negotiation was *not* necessary after all; and now we begin the actual nb-read
     * into m_batch.  So, outside of having already marked not_idle_val as true, we "start" fresh by
     * simply falling-through. */
  } // if (*protocol_negotiator indicated we need to do protocol negotiation) [Inside {}, we made it so we don't now.]

  // See justification for this thing in similar spot around `do_log_trace` in Msg_batch_in::nb_read().
  const bool do_log_trace = logger_ptr && logger_ptr->should_log(Sev::S_TRACE, get_log_component());

  /* Possible legal payloads (m_msg_type, meta-blob-length, hndl-or-none):
   *   - 0x0000,  0, hndl => user in-message (with handle but no meta-blob) [NOTE: Illegal if no_hndls is true.]
   *   - 0x0000, >0, none => user in-message (no handle but with meta-blob)
   *   - 0x0000, >0, hndl => user in-message (with handle and meta-blob) [NOTE: Illegal if no_hndls is true.]
   *   - 0x0000,  0, none => graceful-close [NOTE: This + no-other-error => emit graceful-close error.]
   *   - 0xFFFF,  0, none => auto-ping [NOTE: Ignore it, other than it too implies non-idleness.]
   *   - Others = illegal.
   * We want the top 3 to be at the start, so we will use m_batch.reuse_result_payloads() to swap stuff around,
   * moving the others (including illegal ones, which should essentially not exist, but we'll still check) at the end
   * of [0, this->n_used()) range.  The ones at the start can then just be left alone (user will grab 'em as desired).
   * Then, we can scan the trailing ones: any illegals => error; graceful-close => graceful-close error;
   * auto-ping => ignore.
   *
   * Caveats/stuff:
   *   - Generally, would-block means we shall emit would-block as per contract.  Recall however that
   *     while there is the standard would-block semantic (got 0 messages + would-block error), there is also the
   *     special subtle (perf-saving) additional semantic, wherein m_batch.nb_read() yields no error but also
   *     does not fill all available slots => would-block.  No problem; we just need to detect this.
   *     - In any case: would-block is just another error really, and we emit it as such.  Just remember that
   *       certainly we must make any user-messages also acquired available as opposed to somehow thrown away.
   *   - However it is also possible (and if n_used() is such that there is exactly 1 slot left, common) that
   *     m_batch.nb_read() *does* fill all available slots.  Now we do *not* know whether the connection reached
   *     would-block; maybe it has, maybe it hasn't.  Well, no problem: after all, all slots filled => nothing more
   *     we can read-into, so we have to stop (the caller will do what it wants; spoiler alert: typically try another
   *     read after handling all the stuff we got this time)...
   *   - ...but a corner case occurs where, yes, m_batch.nb_read() *does* fill all available slots; but then
   *     the aforementioned m_batch.reuse_result_payloads() eliminates a bunch of non-user-messages but encounters
   *     no error (i.e., it finds 1+ auto-pings).  So now on one hand we don't know whether connection is in
   *     would-block, but we *do* have 1+ available slots again.  So in this case we must do another m_batch.nb_read()
   *     and repeat the algorithm.  So that's why it must be a `while` loop that stops either on error
   *     (including would-block, or graceful-close for that matter) or running out of slots -- *after*
   *     weeding out the auto-pings.
   *   - Our-protocol graceful-close error is annoyingly special.  If there are no user messages, then no prob.  But if
   *     there is 1+ user message and then graceful-close, by contract we must emit the error *and also* leave the
   *     normal messages for the user.  (By contract what to do subsequently is the caller's responsibility.)  That said
   *     we need not really do anything special; nothing can follow graceful-close, so there's no further reading -- as
   *     with any other error -- but still we must be mindful of that situation and follow our contract in terms of
   *     post-conditions.  In a sense would-block is similar actually, as far as we are concerned (not so much the
   *     caller, which would stop messing with this in-pipe on graceful-close/other errors but not would-block).
   *   - Low-level graceful-close (`eof`) is also annoyingly special.  m_batch.nb_read() emits this condition by setting
   *     our low_graceful_close=true, and it can do this while also emitting 1+ messages (or 0 of them, too).  We need
   *     to detect this and emit `eof` while keeping any in-messages as well.
   *   - All other errors (so not would-block, not graceful-close of either kind) we specifically advertised as
   *     being uncombinable with emitting 1+ in-messages.  They indicate some kind of catastrophic exceptional hosedness
   *     (like a truncated message maybe, or even EBADF) and we promised to emit no messages then; so if iteration 1
   *     emits some, then iteration 2 finds such an exceptional problem, we have to undo any n_used() change from
   *     iteration 1. */

  bool got_a_user_msg = false; // For a particular corner-case condition check: true on 1+ user msgs received.
  /* For each iteration mark true if and only if overall this->nb_read() yielded the situation wherein we've received 1+
   * user messages total, yet the last actual nb-read did not fill all available slots. */
  bool implied_would_block;

  unsigned int n_iters = 0;
  const auto orig_n_used = m_batch.n_used(); // Set back to this on exceptional error in iteration 2+.

  do
  {
    ++n_iters;

    /* It's a tricky algorithm to reason about, so let's go in detail at each step.  At this point:
     *   - Before the .nb_read():
     *     - There is no error on the connection (including from any earlier loop iteration in
     *       the present function, as that stops the loop; see below).
     *     - .n_used() is not necessarily 0; it might the be the first iteration, but for some reason they
     *       ran us with some slots already filled; or it might be iteration 2+, and the preceding .nb_read()
     *       or further processing yielded no error of any kind (including would-block) but did as a result of
     *       the "further processing" free up 1+ slot(s) at the end.  In any case .n_used() is not max_msg_count,
     *       so we should .nb_read(). */

    const auto n_used_pre_read = m_batch.n_used();
    bool low_graceful_close;
    size_t iter_low_lvl_bytes;
#ifndef NDEBUG
    const bool ok =
#endif
    m_batch.nb_read(logger_ptr, peer_socket_ptr, &low_graceful_close, err_code, message_flags,
                    &iter_low_lvl_bytes);
    assert(ok && "For 1st iteration we should've returned pre-loop; "
                   "for others we should've stopped the loop (b/c full()) before trying another nb-read.");

    /* At this point the possibilities are:
     *   - *err_code is false, but 0 messages read (<=> `low_graceful_close == true`), then it is as-if
     *      m_batch.nb_read() emitted `eof`; so just pretend that is what happened: assign it accordingly, then
     *      execute the next bullet point.  (`low_graceful_close == true` with 1+ messages is handled below, not here.)
     *   - *err_code is true:
     *     - If would-block: No more to read, period.  Loop should stop.
     *       Official would-block means 0 in-messages read; so that's it; nothing to post-process.
     *       - Emit the would-block via *err_code, if no prior iteration has produced even 1 user in-message.
     *         Otherwise clear *err_code.  (Caller can still detect the overall "implied would-block.")
     *         However, mark that it is "implied would-block": loop should stop.
     *     - If other error:  No more to read (socket hosed), period.  Loop should stop.
     *       Emit the error via *err_code; *and* (unless it is `eof`) reset n_used() to n_used_pre_read.
     *     - However, any legal messages from any prior iterations have been recorded.  It is up to the caller to
     *       proceed how they want.
     *   - *err_code is false; and 1+ messages read (but low_graceful_close may be true or false).
     *     - `not_idle_val = true`: The connection is not idle.
     *     - If slots still remain, it is "implied would-block": loop should stop.  But do *not* emit this in
     *       *err_code; and the next bullet points still apply.
     *     - Must post-process them:
     *       - Place all regular user messages at the start of the read-to range, swapping-away the others
     *         to the end of that range: m_batch.reuse_result_payloads().
     *       - For each swapped-away slot (if any):
     *         - If illegal, set *err_code to reflect it (stop scanning the range; loop should stop).
     *           In addition: reset n_used() to n_used_pre_read.
     *         - If graceful-close, set *err_code to reflect it (stop scanning the range; loop should stop).
     *           (Note: We are reasonably but not maximally paranoid about checking for legality; that is e.g.
     *           graceful-close means no need to check further; but there could be slots after that which are illegal.
     *           We could keep going and search for illegality or assume the best and stop for perf.
     *           Philosophically these steps are for safety, not security, so we extend some trust to the other side
     *           and check for illegality opportunistically/defensively (against bugs by the other side) only.)
     *         - Note: All else being appropriate (basically: if all these slots are just auto-pings), they can be
     *           reused for future reads without any further work required on them.  The target blob areas are still
     *           fine to read-to, etc.
     *     - If the post-processing did not emit a truthy *err_code, check low_graceful_close.  If it's true, then
     *       set *err_code to `eof`; otherwise (mainstream case) nothing special to do.
     *     - If by now we have truthy *err_code, loop should now stop.  However, any legal messages
     *       have been recorded.  It is up to the caller to proceed how they want.
     *       - Else (falsy *err_code still):
     *         - If, after the post-processing, we see that "all regular user messages" = "0 such messages,"
     *           then:
     *           - If "implied would-block" detected above:
     *             - If no prior iteration has produced even 1 user in-message: Set *err_code = would-block.
     *               (Loop will soon stop due to *err_code.)
     *             - Else: just continue.  (Loop will soon stop due to "implied would-block.")
     *           - Else: just continue.  (Loop might continue: no slots remained, but post-processing might have freed
     *             some up.  Otherwise it'll stop due no slots remaining.)
     *         - Else (if got 1+ user-messages): just continue.
     *     - If *err_code still falsy:
     *       - If "implied-would-block" detected above: loop stops.
     *       - Else: If *after post-processing* no slots remain: loop stops.
     *       - Else: Gotta do another .nb_read() / loop iteration (slots remain; no would-block yet detected).
     *     - Else (if *err_code is truthy): loop stops. */

    // Read did proceed: slots were available.

    const auto n_used_post_read = m_batch.n_used();

    m_histo_raw_read_n_msgs.record_value(n_used_post_read - n_used_pre_read);

    if (*err_code)
    {
      assert((n_used_post_read == n_used_pre_read) && "Expected low-level .nb_read() to emit error <=> no in-msgs.");

      if ((*err_code == boost::asio::error::would_block) && got_a_user_msg)
      {
        // Interesting and rare enough for INFO log-level.
        FLOW_LOG_INFO("Native_socket_stream_batch [" << *this << "]: "
                      "Native_socket_stream batch nb-read: raw nb-read got would-block in iteration 2+; but earlier "
                      "iteration got a user in-message + filled all slots + enough auto-pings to free some slot(s) "
                      "for further iteration(s).  Will not emit official would-block but will stop reading due to "
                      "overall implied would-block.");

        err_code->clear();
        implied_would_block = true;
      }
      else
      {
        implied_would_block = false; // Don't log; m_batch->nb_read() logged enough.

        /* If e_c==would_block && !got_a_user_msg: m_batch.n_used() has not been modified from orig_n_used.
         *   So the following statement is a no-op.
         * If e_c!=would_block: m_batch.n_used() >= orig_n_used (from preceding iteration(s)), but m_batch.nb_read()
         *   contract is that any non-would-block error is exceptional, meaning *this* iteration did not itself
         *   increase m_batch.n_used().  Therefore we similarly -- as promised for any non-graceful-close error --
         *   guarantee that this overall this->nb_read() does not modify m_batch.n_used().  Hence this statement.
         *   A/k/a: Un-emit any in-messages from prev iteration(s): exceptional error. */
        m_batch.clear_used(orig_n_used);
      }

      m_histo_raw_read_n_msgs_filtered_out.record_value(0);
    } // if (*err_code) [But it may have become falsy { inside }.]

    else if (n_used_pre_read == n_used_post_read) // && !*err_code
    {
      assert(low_graceful_close && "Basic nb_read() yielding no error but no messages must mean low_graceful_close.");

      /* Per algorithm described above -- low-level graceful-close *not* following any in-msgs is simple enough to
       * be handled almost the same as on truthy *err_code just above; simpler really.  That is: Keep any in-messages
       * from prev iteration(s) (do not reset m_batch.n_used()), and it is not would-block of any kind; most saliently
       * emit this as the specific `eof` error. */

      *err_code = boost::asio::error::eof;
      implied_would_block = false; // Don't log; m_batch->nb_read() logged enough.
    }

    else // if (!*err_code) && (n_used_pre_read > n_used_post_read)
    {
      not_idle_val = true; // Got anything => not idle.
      implied_would_block = !full(); // m_batch->nb_read() logged enough.

      // Heuristic check BEGIN -->

      /* To continue grokking the impl, in terms of correctness/functionality, skip to "Heuristic check END."
       * Before then we perform a heuristic pre-check such that, if it passes, we can skip everything past
       * "Heuristic check END," replacing it with about ~2 statements and `continue`.  Now to explain what we do
       * here and why:
       *
       * Basically what we would do now (as prescribed in the large comment higher-up) is
       * reuse_result_payloads(F), where F is a functor wrapping this->is_not_user_message().  That might have
       * filtered-out 1+ non-user-in-messages (which we must then scan) and lowered n_used() as a result.
       * That code is written (we feel) quite tightly, but nevertheless there are many branches and busy-work
       * including setting up the functor F.  Now consider the eventuality wherein reuse_result_payloads(F)
       * ends up being a no-op.  Then, if you run through that code, you'll see all that work amounts to
       * `got_a_user_msg = true`.  Since the present code is *extremely* perf-sensitive under load, it'd be
       * an improvement if we could make a computation such that if it results in TRUE, then reuse_result_payloads()
       * would *definitely* be a no-op; and therefore we could in fact skip that expensive-ish processing;
       * set got_a_user_msg to true; and `continue`.  Naturally key questions must be answered for this to be
       * worthwhile.  To wit:
       *   - Is there a quick such computation?  Answer: Yes.  If (and only if, actually) every message in
       *     range [n_used_pre_read, n_used_post_read) is a legal user in-message, then reuse_result_payloads()
       *     would no-op.  (Among other optimizations -- as you'll see shortly -- we need not assemble a functor
       *     and can make some simple inline checks instead.)
       *   - Will it result in TRUE almost all of the time?  Answer: Yes but it requires reasoning/explanation:
       *     When would it return FALSE?  The only non-vanilla (legal user in-message) dgrams are: illegal (~never
       *     happens in reality), graceful-close (happens at most once per entire connection's lifetime), and
       *     auto-ping.  Therefore only auto-pings matter in practice, perf-wise.
       *     - When under any kind of load (the only case where the added compute of a FALSE-resulting pre-check would
       *       matter in practice) there shall be *no* auto-pings, by definition (they are sent after ~seconds of
       *       idleness).
       *     - When there is no load there will indeed be auto-pings.  But in that case the FALSE-resulting pre-check,
       *       while technically a (slight) waste of compute, has ~no impact on overall perf.
       *
       * Oh, and another possibility is low_graceful_close (low-level graceful-close), similarly rare as the
       * high-level graceful-close we just discussed.  So we simply add the check for !low_graceful_close; if that is
       * not the case, then the pre-check immediately fails, and we do the full algorithm (which will handle
       * low_graceful_close as applicable).
       *
       * We've shown that this is a worthwhile endeavour.  So here's the computation. */

      /* idx_past_user_msg shall point to right after the last *contiguous* user in-message (if any) in
       * [n_used_pre_read, n_used_post_read).  Meaning, it'll point to the first message from the start that is *not*
       * a user message.  (There may be user messages past that (but not contiguously with the leading ones)!)
       *   - == n_used_pre_read => there are 0 such messages.
       *   - == n_used_post_read => they are all such messages (fast-path). */
      size_t idx_past_user_msg;
      if (low_graceful_close)
      {
        idx_past_user_msg = n_used_pre_read; // That'll fail the pre-check.
      }
      // Some code is duplicated as a result, but by checking this outside loop we avoid repeating `no_hndls` check.
      else if (no_hndls) // && !low_graceful_close
      {
        /* Go through each in-dgram.  Break out of loop if and only if a non-vanilla one is found.
         * If post-loop idx_past_user_msg indicates we were able to get through all in-dgrams,
         * pre-check result is TRUE, else FALSE.
         *
         * Refer to "Possible legal payloads" table above, specifically the legal user in-message rows. */
        for (idx_past_user_msg = n_used_pre_read; idx_past_user_msg != n_used_post_read; ++idx_past_user_msg)
        { // Note: We know this loop has 1+ iterations.
          /* no_hndls allowed; therefore only the following is a vanilla msg:
           *   - 0x0000, >0, none => user in-message (no handle but with meta-blob)
           * Thus: dgram size must *exceed* MSG_TYPE_SZ; first two bytes must equal 0x0000; and there must be no
           * native handle. */
          Msg_resource_impl* msg_resource_impl;
          const auto n_rcvd = m_batch.result_payload_blob(idx_past_user_msg, &msg_resource_impl);
          if ((n_rcvd <= MSG_TYPE_SZ) || (*msg_resource_impl->m_msg_type != TYPE0)
              || (!m_batch.result_payload_hndl(idx_past_user_msg).null()))
          {
            break;
          }
          // else: Record user message in histogram of user bytes per message, so we don't have to loop yet again.
          if (rcv_stats) { rcv_stats->m_histo_payload_sz.record_value(n_rcvd - MSG_TYPE_SZ); }
        }
      }
      else // if (!no_hndls) // && !low_graceful_close
      {
        // Same cmnts as in the other branch except where noted.
        for (idx_past_user_msg = n_used_pre_read; idx_past_user_msg != n_used_post_read; ++idx_past_user_msg)
        {
          /* !no_hndls (handles allowed); therefore only the following are vanilla msgs:
           *   - 0x0000,  0, hndl => user in-message (with handle but no meta-blob)
           *   - 0x0000, >0, none => user in-message (no handle but with meta-blob)
           *   - 0x0000, >0, hndl => user in-message (with handle and meta-blob)
           * Note that if first two bytes were indeed received and equal 0x0000, then the only in-dgram that
           * is *not* vanilla is:
           *   - 0x0000,  0, none => graceful-close
           * Thus: dgram size must >= MSG_TYPE_SZ; first two bytes must equal 0x0000; and either
           * dgram size *exceeds* MSG_TYPE_SZ (likeliest), or there's a handle, or both. */
          Msg_resource_impl* msg_resource_impl;
          const auto n_rcvd = m_batch.result_payload_blob(idx_past_user_msg, &msg_resource_impl);

          /* Need this either for ++rcv_stats->m_msgs_with_hndls or for the pre-check or both.  Perf-wise:
           *   - If pre-check passes: Fast-path; definitely need the flag (unless !rcv_stats but in practice
           *     it's gonna be non-null as of this writing).
           *   - If it (eventually) fails: Slow-path; so doesn't matter if we needlessly grab this value. */
          const bool got_hndl = !m_batch.result_payload_hndl(idx_past_user_msg).null();

          if ((n_rcvd < MSG_TYPE_SZ) || (*msg_resource_impl->m_msg_type != TYPE0)
              || ((n_rcvd == MSG_TYPE_SZ) && (!got_hndl)))
          {
            break;
          }
          // else:
          if (rcv_stats)
          {
            rcv_stats->m_histo_payload_sz.record_value(n_rcvd - MSG_TYPE_SZ);
            got_hndl && (++rcv_stats->m_msgs_with_hndls);
          }
        }
      }
      if (idx_past_user_msg == n_used_post_read) // Pre-check passed -- we can skip much stuff and (don't even log)....
      {
        m_histo_raw_read_n_msgs_filtered_out.record_value(0);

        // Stats: all dgrams are user messages; 0 filtered out.  iter_low_lvl_bytes is the total from m_batch.nb_read().
        if (rcv_stats)
        {
          const auto n_user_msgs = n_used_post_read - n_used_pre_read;
          rcv_stats->m_total_msgs += n_user_msgs;
          rcv_stats->m_total_low_lvl_bytes += iter_low_lvl_bytes;
          rcv_stats->m_total_bytes += (iter_low_lvl_bytes - (MSG_TYPE_SZ * n_user_msgs));
          // rcv_stats->m_histo_payload_sz and ->m_msgs_with_hndls are already good to go.
        }

#if 0
        got_a_user_msg = true; // Got 1+ user-message.
        continue;
#else
        /* `continue` is correct, but we can optimize further actually:
         * The loop-ending condition is: implied_would_block || *err_code || full().  *err_code is false, so it's:
         * `implied_would_block || full()`.  However, we've just set: `implied_would_block = !full()`.
         * So it's `true || false` or `false || true`; i.e., true; i.e., loop ends.  So we can `break` instead of
         * `continue`.
         *
         * Sanity check: How can it even be not true then?  Answer: In the non-vanilla case (which we've eliminated),
         * full() would be true pre-prune; but 1+ auto-pings being present would make it become false (freeing up
         * some space for another iteration).  In our case, though, such complex eventualities are not possible.
         *
         * Lastly: we can also skip `got_a_user_msg = true`, because it would only be checked/used in a subsequent
         * iteration; but that will not happen. */
        break;
#endif
      }
      /* else if (at least 1 non-vanilla in-dgram, and/or low_graceful_close): Do the full algorithm.
       *
       * There is also an annoying corner case here regarding rcv_stats->m_histo_payload_sz and ->m_msgs_with_hndls:
       * We recorded all (0+) user in-messages in the pre-check loop above, opportunistically.  There shall be 0+ more
       * user in-messages past that point however.  So we need to (1) count those but (2) not count the ones
       * already counted.  The part that makes this less stressful is that in this slow-path we're much less
       * concerned about saving every cycle.  E.g., another loop through part of the range = no big deal.
       * Look for that below. */

      if (do_log_trace)
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Native_socket_stream_batch [" << *this << "]: "
           "Native_socket_stream batch iteration [" << n_iters << "] (1-based): raw nb-read yielded 1+ in-dgrams; "
           "heuristic-optimization pre-check detected low-graceful-close "
           "and/or at least 1 non-vanilla in-dgram; therefore we must execute "
           "the full post-processing algorithm (that looks for auto-pings, graceful-close x 2, and illegal msgs); "
           "proceeding.  This should NOT occur under load, with the exception of a potential 1-time graceful-close.");
      }

      // <-- Heuristic check END

      m_batch.reuse_result_payloads(n_used_pre_read, // @todo Can we scan from idx_past_user_msg?  Probably.
                                    [this, logger_ptr, no_hndls](size_t idx) -> bool
                                      { return is_not_user_message(logger_ptr, idx, no_hndls); });

      const auto n_used_post_prune = m_batch.n_used();
      /* Non-empty range [n_used_pre_read, n_used_post_read) has been split into:
       * Ruser = [n_used_pre_read, n_used_post_prune) and Rother = [n_used_post_prune, n_used_post_read). */
      assert((n_used_post_prune <= n_used_post_read)
             && "Expected .reuse_result_payloads() to lower or not-touch .n_used().");
      assert((n_used_pre_read <= n_used_post_prune)
             && "Expected .reuse_result_payloads() to operate only on range [n_used_pre_read, ...).");

      m_histo_raw_read_n_msgs_filtered_out.record_value(n_used_post_read - n_used_post_prune);

      if (n_used_pre_read != n_used_post_prune) // I.e., pre_read<post_prune.
      {
        got_a_user_msg = true; // Got 1+ user-message (Ruser is not empty).
      }

      // Post-process the pruned-away a/k/a swapped-away guys if any (Rother).  See large-ish comment just above.
      size_t rother_auto_pings = 0; // Stats: count auto-pings in Rother.
      for (size_t idx = n_used_post_prune; (!*err_code) && (idx != n_used_post_read); ++idx)
      { // May well be no-op loop.
        Msg_resource_impl* msg_resource_impl;
        const auto n_rcvd = m_batch.result_payload_blob(idx, &msg_resource_impl);
        if (n_rcvd < MSG_TYPE_SZ)
        {
          // *msg_resource_impl->m_msg_type was not fully read => illegal.
          *err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
          FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                           "Native_socket_stream batch nb-read post-post-processing scan of slot [" << idx << "]: "
                           "illegal too-short in-dgram.");
          continue;
        }
        // else

        const auto& msg_type = *msg_resource_impl->m_msg_type;
        if (msg_type == PING)
        {
          if ((n_rcvd != MSG_TYPE_SZ) || (!m_batch.result_payload_hndl(idx).null()))
          {
            // Auto-ping sentinel + any data or hndl => illegal.
            *err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
            m_batch.clear_used(orig_n_used); // Un-emit any in-messages from prev iteration(s): exceptional error.
            FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                             "Native_socket_stream batch nb-read post-post-processing scan of slot [" << idx << "]: "
                             "illegal auto-ping-like in-dgram: "
                             "contains blob payload sized [" << (n_rcvd - MSG_TYPE_SZ) << "] and/or "
                             "native-handle [" << m_batch.result_payload_hndl(idx) << "].");
          }
          else
          {
            ++rother_auto_pings;
          }
          assert(!*err_code);
          continue; // Don't log about ping; is_not_user_message() doing so is plenty.
        }
        // else if (not PING):

        if (msg_type != TYPE0)
        {
          // Unknown msg_type (neither PING nor 0000) => illegal.
          *err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
          m_batch.clear_used(orig_n_used); // Un-emit any in-messages from prev iteration(s): exceptional error.
          FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                           "Native_socket_stream batch nb-read post-post-processing scan of slot [" << idx << "]: "
                           "illegal in-dgram with unexpected msg-type value [" << msg_type << "].");
          continue;
        }
        // else if (msg_type == 0000):

        /* Refer to the table earlier-up and also recall that in Rother we have no legal messages (they're in Ruser).
         * Hence it's either graceful-close or illegal (due to handle presence).
         *   - If !no_hndls: The 3/4 possibilities that aren't graceful-close are all legal.  So it must be
         *     graceful-close.
         *   - Else: The 2/4 illegal cases all are illegal due to having handle.  So if no handle, graceful-close.
         *     Otherwise, illegal due to handle presence. */
        if ((!no_hndls) || m_batch.result_payload_hndl(idx).null())
        {
          *err_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE; // Our-protocol (high-level) graceful-close.
          // (Keep any in-messages from prev iteration(s).  Do not m_batch.clear_used(orig_n_used).)
          FLOW_LOG_INFO("Native_socket_stream_batch [" << *this << "]: "
                        "Native_socket_stream batch nb-read post-post-processing scan of slot [" << idx << "]: "
                        "got NSS-protocol graceful-close.");
        }
        else
        {
          *err_code = error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB;
          m_batch.clear_used(orig_n_used); // Un-emit any in-messages from prev iteration(s): exceptional error.
          FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                           "Native_socket_stream batch nb-read post-post-processing scan of slot [" << idx << "]: "
                           "illegal in-dgram contains only a native handle "
                           "[" << m_batch.result_payload_hndl(idx) << "], but handles unexpected in this channel.");
        }
      } // for (idx in Rother = [n_used_post_prune, n_used_post_read)) [and break if *err_code becomes truthy inside.]

      /* Didn't detect exceptional error or higher-level graceful-close; so by the algorithm in the large-ish comment
       * above: low-level graceful-close may apply... */
      if ((!*err_code) && low_graceful_close)
      {
        // ...and indeed it does (low_graceful_close == true).
        *err_code = boost::asio::error::eof;
        // (Keep any in-messages from prev iteration(s).  Do not m_batch.clear_used(orig_n_used).  Same as above.)

        /* Quick discussion: Why check low_graceful_close after high-level graceful-close + illegal
         * in-messages?  Answer: Simply, it would be emitted by opposing side after those out-messages; meaning
         * first some write OS-call(s), and only then close OS-call(s).  Or another way of thinking about it is
         * that in-messages are the user protocol we are trying to speak, while low_graceful_close is the substrate
         * protocol; in fact the our-protocol graceful-close is intentionally instituted as a more predictable/civilized
         * graceful-close, common to all blob-transport impls including *but not limited to* Native_socket_stream.
         * So it's in a sense a fallback. */
      }

      else if ((!*err_code) && implied_would_block && (!got_a_user_msg)) // && !low_graceful_close
      {
        if (do_log_trace)
        {
          FLOW_LOG_TRACE_WITHOUT_CHECKING
            ("Native_socket_stream_batch [" << *this << "]: "
             "Native_socket_stream batch nb-read: raw nb-read got 0 user in-messages but 1+ auto-pings, "
             "with empty slots available (implied would-block); and 0 user in-messages in any preceding "
             "iterations also.  Since pings are ignorable, this amounts to overall actual would-block; "
             "will emit that.");
        }
        *err_code = boost::asio::error::would_block;
        implied_would_block = false;
      }
      /* else:
       *   The above logic is a transcription of the wordy algorithm comment above.  That said let's contemplate
       *   the other possibilities, to ensure we're doing the right thing (in doing nothing in this case).
       *   - If *err_code: It's a high-level graceful-close or illegal message.  Of course we should stop and will, when
       *     *err_code is checked shortly.  So the do/while() condition check should proceed normally.
       *   - If !*err_code but !implied_would_block: The raw nb-read filled all slots, so potentially there are more
       *     data; loop should continue as long as !full() (which might indeed be the case due to post-processing
       *     detecting 1+ auto-pings).  So the do/while() condition check should proceed normally.
       *   - If !*err_code and implied_would_block, but got_a_user_msg: Basically, either in this iteration or
       *     preceding, there was at least 1 user in-message.  Loop shall stop due to the implied_would_block; and
       *     report 1+ user-messages.  So the do/while() condition check should proceed normally. */

      /* Stats (slow-path/pre-check failed): Rother = control messages; Ruser = user messages.
       * Each Rother msg = MSG_TYPE_SZ low-level bytes (header only).
       * Ruser low-level bytes = total - Rother low-level bytes.
       * On catastrophic error (kinds that un-emit everything from before / no trusted opposing peer behaves this way),
       * let's not even touch stats.  The calculation would be potentially wrong (can't assume each non-user message
       * is MSG_TYPE_SZ bytes), and making it right for something close to being assert()-worthy isn't worth the
       * code effort.  However, the m_histo_payload_sz and m_msgs_with_hndls counted during the (failed) pre-check
       * loop above are already committed even on catastrophic error; that's fine too.
       *
       * Reminder: When perf matters, the vast majority of the cases this code won't run; so the
       * *err_code check (or two) is negligible. */
      if (rcv_stats && ((!*err_code)
                        || (*err_code == error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE)
                        || (*err_code == boost::asio::error::would_block)
                        || (*err_code == boost::asio::error::eof)))
      {
        const auto n_rother = n_used_post_read - n_used_post_prune;
        const auto rother_low_lvl_bytes = n_rother * MSG_TYPE_SZ;
        const auto n_ruser = n_used_post_prune - n_used_pre_read;

        rcv_stats->m_auto_pings += rother_auto_pings;
        rcv_stats->m_total_low_lvl_bytes += iter_low_lvl_bytes;
        rcv_stats->m_total_msgs += n_ruser;
        /* Reminder: m_total_bytes = bytes inside actual user messages, no headers.  Low-level bytes in
         * user-message-bearing datagrams is the first term (total minus Rother).  Then subtract the headers. */
        rcv_stats->m_total_bytes += ((iter_low_lvl_bytes - rother_low_lvl_bytes) - (MSG_TYPE_SZ * n_ruser));

        /* Per-Ruser-message stats: payload-size histogram and handle counting.
         * As foreshadowed before: we must skip the ones we opportunistically counted while doing the
         * pre-check loop.  Hence begin at idx_past_user_msg (which must be >= n_used_pre_read).  This should
         * get any (potentially 0) Ruser dgrams that weren't detected during pre-check. */
        for ( ; idx_past_user_msg != n_used_post_prune; ++idx_past_user_msg)
        {
          rcv_stats->m_histo_payload_sz.record_value(m_batch.result_payload_blob(idx_past_user_msg) - MSG_TYPE_SZ);

          (!no_hndls) && (!m_batch.result_payload_hndl(idx_past_user_msg).null())
            && (++rcv_stats->m_msgs_with_hndls);
        }
      } // if (rcv_stats && no catastrophic [non-graceful-close] error)
    } // else if (!*err_code) && (n_used_pre_read > n_used_post_read) [But *e_c may have become truthy { inside }.]

    /* Refer to the large-ish comment above regarding loop-stop conditions.  Basically it comes down to,
     * usually we stop; the only case where we don't is that: no error was detected,
     * and the actual nb-read yielded all possible slots filled, and the post-processing then freed 1+ slots
     * to re-target.  Little optimization: implied would-block (i.e., actual nb-read filled not-all-slots) --
     * particularly in the first iteration -- is the likeliest stop condition, especially since a batch-receive
     * would often be triggered when knowing the socket is readable.  Point is check that first.  Then a would-block
     * is second-likeliest, so check *err_code next. */
  }
  while ((!implied_would_block) && (!*err_code) && (!full()));

  m_histo_usr_read_n_raw_reads.record_value(n_iters);

  if (do_log_trace)
  {
    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("Native_socket_stream_batch [" << *this << "]: "
       "Native_socket_stream batch nb-read, after iteration, stopped for one of these reasons: "
       "implied would-block? = [" << implied_would_block << "]; "
       "error/would-block [" << *err_code << "] [" << err_code->message() << "]; "
       "all slots filled? = [" << full() << "].");
  }

  // As advertised....
  (*err_code == boost::asio::error::would_block) && (*not_idle_on_would_block = not_idle_val);

  if constexpr(Native_socket_stream_cfg::S_STATS_LOG_ENABLED)
  {
    constexpr auto STATS_SEV = Native_socket_stream_cfg::S_STATS_LOG_SEV;
    if (logger_ptr && logger_ptr->should_log(STATS_SEV, get_log_component()))
    {
      using boost::chrono::milliseconds;
      using boost::chrono::round;
      using flow::Fine_clock;
      using util::Fine_duration;

      const Native_handle peer_hndl{peer_socket_ptr->native_handle()};
      const auto now = Fine_clock::now();

      if (m_stats_next_output_when.time_since_epoch() == Fine_duration::zero())
      {
        m_stats_next_output_when = (now + Native_socket_stream_cfg::S_STATS_LOG_PERIOD);
        FLOW_LOG_WITHOUT_CHECKING
          (STATS_SEV,
           "STATS: Native_socket_stream_batch [" << *this << '/' << peer_hndl << "]: "
             "First log checkpt; next real one shall be in "
             "[" << round<milliseconds>(m_stats_next_output_when - now) << "].");
      }
      else if (now >= m_stats_next_output_when)
      {
        const auto since_last_checkpt = round<milliseconds>((now - m_stats_next_output_when)
                                                            + Native_socket_stream_cfg::S_STATS_LOG_PERIOD);

        FLOW_LOG_WITHOUT_CHECKING
          (STATS_SEV,
           "STATS: Native_socket_stream_batch [" << *this << '/' << peer_hndl << "]: "
             "Since last log [" << since_last_checkpt << "]: "
             "Histogram: # raw in-dgrams from each raw batch-recv: "
             "[" << m_histo_raw_read_n_msgs << "].");
        FLOW_LOG_WITHOUT_CHECKING
          (STATS_SEV,
           "STATS: Native_socket_stream_batch [" << *this << '/' << peer_hndl << "]: "
             "Since last log [" << since_last_checkpt << "]: "
             "Histogram: # ping/close/bad in-dgrams from each raw batch-recv: "
             "[" << m_histo_raw_read_n_msgs_filtered_out << "].");
        FLOW_LOG_WITHOUT_CHECKING
          (STATS_SEV,
           "STATS: Native_socket_stream_batch [" << *this << '/' << peer_hndl << "]: "
             "Since last log [" << since_last_checkpt << "]: "
             "Histogram: # raw batch-recvs from each higher-lvl batch-recv: "
             "[" << m_histo_usr_read_n_raw_reads << "].");

        m_stats_next_output_when = (now + Native_socket_stream_cfg::S_STATS_LOG_PERIOD);
      }
      else if (do_log_trace)
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("STATS: Native_socket_stream_batch [" << *this << '/' << peer_hndl << "]: "
             "Between checkpts; next one shall be in "
             "[" << round<milliseconds>(m_stats_next_output_when - now) << "].");
      }
    } // if (should_log)
  } // if constexpr(Native_socket_stream_cfg::S_STATS_LOG_ENABLED)

  return true;
} // Native_socket_stream_msg_batch_in::nb_read()

template<typename Msg_resource_t>
bool Native_socket_stream_msg_batch_in<Msg_resource_t>::is_not_user_message(flow::log::Logger* logger_ptr,
                                                                            size_t idx, bool no_hndls) const
{
  constexpr auto TYPE0 = msg_type_t{0x0000};
  constexpr auto MSG_TYPE_SZ = sizeof(msg_type_t);

  /* Do not log unless will return `true`.  `false` is the fast-path; try to detect it ASAP and GTFO.
   *
   * That said as of this writing nb_read() has a pre-check that will, whenever calling us could possibly affect
   * perf, avoid calling us in the first place.  So we can slightly relax including setting up logging and forgoing
   * the do_log_trace trick that nb_read() does use (but we don't).
   *
   * (*That* said this code was originally written before the pre-check existed, so it tries hard to be as tight
   * as possible; and there's no reason to undo that; and remaining code and comments are written in that
   * perf-jealous spirit.) */
  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  /* No choice but to first ensure we got the entire m_msg_type at least.  Pretty much always that'll be the case,
   * so pretty much always this won't wastefully set msg_resource_impl (i.e., it'll set it -- then we'll use it). */
  Msg_resource_impl* msg_resource_impl;
  const auto n_rcvd = const_cast<Native_socket_stream_msg_batch_in*>(this)
                        ->m_batch.result_payload_blob(idx, &msg_resource_impl);
  // (const_cast<> used because we use result_payload_blob() but in an explicitly non-destructive way.  @todo Hmmm.)
  if (n_rcvd < MSG_TYPE_SZ) // *msg_resource_impl->m_msg_type was not fully read => illegal.
  {
    FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                     "Native_socket_stream batch nb-read post-processing slot [" << idx << "]: "
                     "illegal in-dgram of only [" << n_rcvd << "] bytes.");
    return true;
  }
  // else: We can safely examine *msg_resource_impl->m_msg_type and the blob's length.  [likeliest]

  /* Now refer to the table in the comment above (where we outline the possible combinations of payloads).
   * We must return false if and only if it is a legal in-message that is in fact a user in-message.
   * Anything else => not a regular user message; or just illegal. */

  if (*msg_resource_impl->m_msg_type == TYPE0) // [likeliest]
  {
    // Either user in-message, or graceful-close, or illegal due to handle presence while no_hndls.

    if (n_rcvd != MSG_TYPE_SZ) // [likeliest]
    {
      // Non-empty blob: cannot be graceful close; and is legal if and only if no unexpected handle.

      if (m_batch.result_payload_hndl(idx).null() // No handle (even if disallowed) => legal.  [likeliest]
          || (!no_hndls)) // Got handle but handles allowed => legal.
      {
        return false;
      }
      // else if (no_hndls && (got handle)):
      FLOW_LOG_WARNING("Native_socket_stream_batch [" << *this << "]: "
                       "Native_socket_stream batch nb-read post-processing slot [" << idx << "]: "
                       "illegal in-dgram contains a native handle, but we were instructed that is illegal in this "
                       "channel.");
      return true;
    }
    /* else: Empty blob.  If no handle then graceful-close => true.  If yes handle but no_hndls then illegal => true.
     *       Else, legal user message (with handle but no blob) => false.
     * At this point it's pretty much just either/or; can't think of how to get to likeliest answer faster; so
     * just check which one it is.  In any case the likeliest case was handled above first. */
    if ((!no_hndls) && (!m_batch.result_payload_hndl(idx).null()))
    {
      return false;
    }
    // else:

    FLOW_LOG_TRACE
      ("Native_socket_stream_batch [" << *this << "]: "
       "Native_socket_stream batch nb-read post-processing slot [" << idx << "]: "
       "in-dgram (msg-type 0x0) contains graceful-close, or else it contains only a native handle, but "
       "we were instructed handles are illegal in this channel (will emit the proper error code "
       "depending).");
    return true;
  } // if (*m_msg_type == TYPE0) // [likeliest]
  // else if (*m_msg_type == PING or illegal value): Either auto-ping or illegal.

  FLOW_LOG_TRACE
    ("Native_socket_stream_batch [" << *this << "]: "
     "Native_socket_stream batch nb-read post-processing slot [" << idx << "]: "
     "(msg-type [" << *msg_resource_impl->m_msg_type << "]): "
     "either auto-ping or unknown-type or illegal auto-ping-like in-dgram (if 1 of the latter 2: "
     "will emit error).");
  return true;
} // Native_socket_stream_msg_batch_in::is_not_user_message()

template<typename Msg_resource_t>
void Native_socket_stream_msg_batch_in<Msg_resource_t>::to_ostream(std::ostream* os) const
{
  *os << "Nss[blob_sz[" << target_payload_size() << "] " << m_batch << ']';
  // So it looks like "Nss[blob_sz[1024] Lcl[slots-total/rdy/rcvd [...]]@...]" as of this writing.
}

template<typename Msg_resource_t>
const flow::util::stat::Histogram_counter& Native_socket_stream_msg_batch_in<Msg_resource_t>::histo_raw_read_n_msgs() const
{
  return m_histo_raw_read_n_msgs;
}

template<typename Msg_resource_t>
const flow::util::stat::Histogram_counter&
  Native_socket_stream_msg_batch_in<Msg_resource_t>::histo_raw_read_n_msgs_filtered_out() const
{
  return m_histo_raw_read_n_msgs_filtered_out;
}

template<typename Msg_resource_t>
const flow::util::stat::Histogram_counter&
  Native_socket_stream_msg_batch_in<Msg_resource_t>::histo_usr_read_n_raw_reads() const
{
  return m_histo_usr_read_n_raw_reads;
}

template<typename Msg_resource_t>
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_msg_batch_in<Msg_resource_t>& val)
{
  val.to_ostream(&os);
  return os;
}

} // namespace ipc::transport
