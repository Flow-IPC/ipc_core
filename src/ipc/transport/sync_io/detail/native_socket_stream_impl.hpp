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

#include "ipc/transport/detail/transport_fwd.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/transport/asio_local_stream_socket.hpp"
#include "ipc/util/sync_io/detail/timer_ev_emitter.hpp"
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include <cstddef>
#include <flow/util/blob.hpp>
#include <queue>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Internal, non-movable pImpl implementation of sync_io::Native_socket_stream class.
 * In and of itself it would have been directly and publicly usable; however Native_socket_stream adds move semantics
 * which are essential to cooperation with sync_io::Native_socket_stream_acceptor and overall consistency with the rest
 * of ipc::transport API and, arguably, boost.asio API design.
 *
 * @see All discussion of the public API is in sync_io::Native_socket_stream doc header; that class forwards to this
 *      one.  All discussion of pImpl-related notions is also there.  See that doc header first please.
 *      Then come back here.
 *
 * Impl design
 * -----------
 * ### Intro / history ###
 * In the past transport::Native_socket_stream was one monolithic thing and therefore was was not exactly easy
 * to understand as a unit.  It has since been split into this `sync_io`-pattern core (which, to be clear, may
 * well be useful in and of itself) and transport::Native_socket_stream built around it.  Due to this split,
 * the complexity is split too, and each part is really quite manageable.
 *
 * To understand `*this`, the only real obstacle is grokking the `sync_io` pattern (see util::sync_io doc header).
 * We will not recap it here in any depth.  The essential idea, though, is that when `*this` internally needs to
 *   - async-read or async-write over `m_peer_socket` (the low-level transport); or
 *   - schedule a timer to fire in T time
 *
 * we must express either operation in terms of an async-wait (outsourced to the `*this` user).
 * Namely:
 *   - async-read/write = async-wait on FD for readable/writable (outsourced) + on active event, we nb-read/write.
 *   - scheduling timer = with the help of util::sync_io::Timer_event_emitter, make it so that when it does fire,
 *     a certain FD is made readable; async-wait on FD for readable (outsourced) + on active event, we read
 *     to the firing.
 *
 * Those are the building blocks.  Other than that, it's a matter of designating our 2 pipes and how each one's
 * algorithm works.  Both are async algorithms but expressed in terms of the above building blocks (in the
 * `sync_io` pattern).
 *
 * Reminder: we do not deal with threads.  To summarize our thread safety guarantees:
 *   - there are none, on a given `*this`; except
 *   - once in PEER state, send-ops (including `(*on_active_ev_func)()` from `start_send_*_ops()`) are allowed to
 *     be invoked concurrently to receive-ops (including `(*on_active_ev_func)()` from `start_receive_*_ops()`).
 *
 * To allow for the latter, the design is carefully split into 2 op-types; and the only data they ever touch
 * in common comprise `m_*peer_socket`.  This makes makes perfect sense: a Unix domain socket connection
 * is full-duplex but done over a single FD.  See Impl::m_peer_socket doc header regarding how we ensure
 * safe concurrency without sacrificing perf.  (Spoiler alert: a tiny and simple critical section w/r/t
 * `m_*peer_socket` only.)
 *
 * There is, also, NULL state, wherein one can async_connect().  This is separate from the other 2, in this case
 * preceding them entirely.  We will soon discuss each algorithm: connect-ops, send-ops, receive-ops (though
 * connect-ops is treated more as an afterthought at the end).  Before we can speak of send-ops and receive-ops
 * algorithms, we have to establish the substrate over which they operate: the protocol they speak.  Don't worry;
 * it is a simple protocol.
 *
 * Before even speaking of *that* more generally, there was this specific question to settle:
 *
 * ### Shutting down individual-direction pipes ###
 * As required by the concepts, the out-pipe can be (arbitrarily preceding dtor call) ended via APIs
 * `*end_sending()`.  How is this internally implemented?  I (ygoldfel) faced a dilemma in
 * answering this question.  Initially I was tempted to use the `shutdown()` native/boost.asio `Peer_socket` call
 * which takes an argument `int how`, specifying which-direction pipe to shut down.  Eventually I decided against it,
 * because the exact semantics of `shutdown()` for stream UDS are rather unclear; vaguely speaking how they act is
 * documented mostly w/r/t to TCP sockets, not local (UDS) ones, and even assuming UDS act the same isn't great, since
 * TCP involves an unreliable medium with FIN/ACK/etc., while stream UDS internally just reliably shuffles around bytes
 * in kernel memory.  It's possible to get straight answers by reading kernel code, but how portable/maintainable is
 * that?  Lastly, when using `shutdown()`, the signaling-other-side semantics are particularly unclear; e.g., if I
 * `shutdown(how=WRITE)`, will opposing peer get an "EOF" (connection gracefully closed)?  Possibly but does that imply
 * the other side was also closed?  `EPIPE` is also involved (if closing reader -- N/A for us but...)... it's just
 * low-level-obscure and hairy.  (Color commentary: POSIX and Linux documented behavior doesn't have the formal
 * completeness that Boost docs do; and if it did it'd still IMO (ygoldfel) be arguably less formally
 * considered in its design.)
 *
 * Therefore I (ygoldfel) decided to instead to take full control of it by *not* using `shutdown()` and instead using
 * the following semantics/impl.  The semantics = inspired both by common sense and `man` page references to similar
 * behavior when using actual `shutdown()` on native stream UDS; but really the latter is a sanity-check on the former;
 * and the former is king.
 *   - Two local flags, #m_snd_finished and a conceptual rcv_finished, which start `false` and can only be set to `true`
 *     (1x).  (rcv_finished is really just for exposition; it is represented by the general condition wherein
 *     #m_rcv_pending_err_code stores a truthy `Error_code`.)
 *   - If side A user does `*end_sending()`, this sets #m_snd_finished and sends a special very-short
 *     "graceful half-duplex close" message -- albeit in order after any actual user messages from
 *     preceding `send_*()` calls; side B receives this and sets its rcv_finished (assigns a particular
 *     receives-finished `Error_code` to #m_rcv_pending_err_code).
 *     - #m_snd_finished means: Send-through any pending-to-be-sent user messages from `send_*()`; then
 *       the graceful-close message; and any `send_*()` calls after `*end_sending()` are
 *       an immediate error::Code::S_SENDS_FINISHED_CANNOT_SEND at best (though could be another error, if something
 *       else is detected... but to the user error=error and is probably to be treated the same).
 *   - If rcv_finished became true (due to the other side sending a graceful-close):
 *     - There will be no subsequent low-level data after that, by definition of how the protocol is designed, meaning
 *       the other peer shouldn't send any more (as said a few bullet points ago).  On our side, though, we don't
 *       rely on that and simply stop reading, as on socket error.
 *     - This condition will be emitted via the `async_receive_*()` completion handler like any other
 *       connection-ending error (in this case, via error::code::S_RECEIVES_FINISHED_CANNOT_RECEIVE; but for example
 *       `boost::asio::error::eof` is a graceful-UDS-close "error" and is conceptually very similar).
 *
 * The required internal code does require some care; but the resulting semantic certainty and control are
 * IMO (ygoldfel) worth it.  Using `shutdown()` instead might seem easy, but I strongly suspect the pain of semantic
 * uncertainty would make up for that many times over.
 *
 * OK; we've decided how to deal with pipe ending -- basically, graceful-close is a message we explicitly need to
 * encode when sending and understand when receiving -- so now we can get into the topic of the protocol.
 *
 * ### Internal protocol design ###
 * This is most natural to discuss from the sending perspective; the receiving perspective will flow naturally from
 * that.  (We'll talk about send_native_handle() and async_receive_native_handle(); the `*_blob()` variants
 * are mere degenerate versions of those.  We'll also talk of end_sending() as opposed to async_end_sending(),
 * as the latter involves the same protocol -- only adding a way of signaling the local user about completion
 * which is irrelevant here.)
 *
 * To summarize, overall, send_native_handle() takes a non-`null()` #Native_handle, a non-empty util::Blob_const
 * (buffer), or both; call the two together (one of which may be null/empty) a *user message*.  The other side is to
 * receive the entire *user message* unchanged and without re-ordering w/r/t other user messages from preceding or
 * succeeding calls.  In addition, if user invokes end_sending(), the effect on the other side should be as if a special
 * (safely distinguished from normal user messages) user message was received indicating sending-direction pipe
 * closing by the user (via a particular #Error_code emitted to async_receive_native_handle() completion handler).
 *
 * Lastly auto_ping() mandates some kind of internal ping message which is to be ignored by the receiver other
 * than resetting any idle time engaged via idle_timer_run().
 *
 * That describes the top layer on either side.  At the bottom (the low-level transport mechanism) is a local (Unix
 * domain) peer socket through which we can send things.  Briefly summarizing, it is capable of all of
 * the above essentially directly with the following important exceptions or caveats:
 *   -# Stream UDS don't usually# implement message boundary preservation -- hence messages must be
 *      encoded somehow in such a way as to indicate message boundaries (and decoded on the other side).
 *      This is further complicated by the case when a native handle must also be sent together with the
 *      boundary-preserved blob.
 *      - (#) `SOCK_STREAM` does not preserve message boundaries.  `SEQ_PACKET` is very similar, apparently, but *does*.
 *        I (ygoldfel) decided against it due to a subjective feeling of "baggage": it's Linux-only (not POSIX), rarely
 *        used, lightly documented, and apparently spiritually associated with the obscure SCTP protocol.  None of this
 *        is fatal (that I know of) but does not seem worth it given that, ultimately, a couple of bytes to encode
 *        a length over `SOCK_STREAM` = both easy and of low perf cost.  (`SOCK_DGRAM` is not suitable, because we
 *        require a connection-based model.)
 *   -# There is no# equivalent to the end_sending() (close-one-direction-but-not-the-other) feature
 *      natively -- hence it must be encoded somehow yet without accidentally clashing with a normal user message
 *      (some kind "escaping" or equivalent is needed).
 *      - (#) We avoid `shutdown(int how)` for the reasons detailed above.
 *   -# There is no built-in ping message either.
 *
 * Here's the protocol to handle these:
 *   - Each user message (send_native_handle()) is represented, in the same order, by 1+ bytes and optionally
 *     a handle: all sent over stream UDS.  Suppose (at least for exposition) we support blobs of size up to 64KiB.
 *     The user message is then encoded as 1-2 *payloads* in this order:
 *     - Payload 1 (required): The *meta-length*, encoded as a native-endianness 2-byte number, indicating size
 *       of the meta-blob provided by the user; 0x0000 indicates no meta-blob.  Plus, byte 1 may be paired with
 *       a native handle; or no such handle if user provided no handle.  Note that meta-length=0 means there
 *       *must* be a handle; otherwise the user's sending nothing (not allowed by API).
 *     - Payload 2 (present unless meta-length=0x0000): The user-provided meta-blob, with given meta-length, verbatim.
 *   - Graceful-close message (from end_sending()) is encoded as 1 *payload*:
 *     - Payload 1 (required): 2-byte number 0x0000; no native handle.  Note this doesn't conflict with the
 *       above, since there 0x0000 implies there is a handle too.
 *   - Auto-ping message (from auto_ping()) is encoded as 1 *payload*:
 *     - Payload 1 (required): 2-byte number 0xFFFF; no native handle.  Note this doesn't conflict with the
 *       above, since the length 0xFFFF is not allowed (due to #S_MAX_META_BLOB_LENGTH excluding it).
 *
 * Naturally if user doesn't end_sending() before the whole thing is destroyed, then there is no
 * graceful-close message either.
 *
 * ### Outgoing-direction impl design ###
 * At construction, or entry to PEER state, we get a connected #m_peer_socket; and then it's off to the races.
 * Every message, whether from `send_*()`, the periodic auto_ping() timer firing, or `*end_sending()`, translates
 * into a series of 1+ *low-level payloads*: each low-level payload containing: 0-1 `Native_handle`s associated with
 * the 1st byte of the following; and ("the following") a contiguous location in memory (pointer + length) to send.
 * At steady state, we can simply attempt to nb-send each payload, in sequence.  Unless would-block is encountered --
 * which should not occur often assuming good opposing-side behavior -- this will simply work.
 *
 * If it does not -- would-block does occur -- then:
 *   - Any unsent payload(s), headed potentially by a fragment of one of them (if one was partially sent), have
 *     to be *copied* and *enqueued* on a pending-payloads queue.  This is Impl::m_snd_pending_payloads_q.
 *   - We initiate an async-wait on Impl::m_peer_socket becoming writable, executed via `sync_io` pattern.
 *
 * While this queue is not empty, any additional payloads (from the aforementioned sources -- `send_*()`, etc.)
 * that might be built go to the back of `m_snd_pending_payloads_q`.
 *
 * While in this would-block state (where that queue is not empty), we await the async-wait's completion.
 * Once it does come in we just try to send off the payloads again, this time off the queue.  The sent stuff
 * gets dequeued, if anything is left another async-wait is in order; etc.
 *
 * ### Incoming-direction impl design ###
 * It is dealing with the in-pipe, which is independent of the out-pipe, with the exception of sharing the same
 * handle (FD), `m_peer_socket`.  (We've spoke of this already.  Just see `m_peer_socket` doc header.)
 * To deal with it, operate based on what we know of our own out-pipe:
 *
 * The outgoing-direction algorithm is written to be agnostic as to what each low-level payload means:
 * user messages (including graceful-close and auto-ping) are simply translated into "1+" payloads each, and then
 * `m_snd_pending_payloads_q` just treats them as a sequence of payloads (it no longer matters how they came to be).
 * However the incoming-direction algorithm does need to decode them back into the user messages that they
 * originally meant.  To see how, just go back to the "Internal protocol design" above.  Each message starts with
 * "payload 1," which is also the only one that might contain a #Native_handle.  The payload 1 blob, always of length 2,
 * is the "meta-length" field.  In exactly 1 scenario -- where it's neither 0 nor 0xFFFF -- is there payload 2 to
 * read; and that one's length is the meta-length, and it is simply the entire meta-blob the sender sent.  In
 * all other scenarios, there is no payload 2 -- the message is finalized; and it's off to the next one again.
 *
 * So it's a state machine; not too exotic.  Read payload 1; if that's the whole message back to the start;
 * otherwise read payload 2; back to the start; if error (including idle-timeout or graceful-close)
 * then stop algorithm.
 *
 * As in the Native_handle_receiver/Blob_receiver concepts at most 1 `async_receive_*()` is outstanding at a time,
 * that's all there is to the algorithm.  That said the code can get somewhat hairy, due to the difficulty of
 * expressing things that would've been pretty simple with boost.asio, such as `boost::asio::async_read()`
 * free function (which will read exactly N bytes, no less, unless error occurs).  We cannot do that stuff, as
 * we have to issue async-waits to the user via `sync_io` pattern.  #Native_handle complicates it a bit more too.
 * That said all that stuff is tactical really.  Just see the code.
 *
 * ### Error handling ###
 * See Impl::m_snd_pending_err_code and Impl::m_rcv_pending_err_code doc headers.  It's pretty simple:
 * one that pipe is hosed, the appropriate one is set to truthy.  Now any user sending or receiving (whichever
 * is applicable to the pipe) immediately yields that error.
 *
 * @todo Native_socket_stream should have at least basic protocol negotiation at connection start, for forward
 * compatibility.  The protocol is simple and transmits arbitrary messages (leaving structured meaning to higher
 * levels).  So we don't intend to change it... but famous last words!  Protocol negotiation might work something
 * like this: Firstly each side sends some forever-constant magic value (receiver errors-out if wrong).
 * Next each sender says, compactly, "I can speak versions in strict range [M, N]" (so it just sends the M, N values).
 * After the exchange, each side has their range [M1, N1] and the peer range [M2, N2]; computes the intersection/overlap
 * [M', N']; and then speaks the highest protocol in the range: N'.  Both sides will compute the same N'; and if
 * there is no overlap, then both sides will error-out (though 1 is enough).  In the first version, it's [1, 1],
 * so in that case this whole header is basically a matter of sending that magic string including [1, 1] and ensuring
 * almost that same magic string is received back (except the N can be >= 1, not just 1).
 *
 * @todo Internal Native_socket_stream and Native_socket_stream_acceptor queue
 * algorithms and data structures should be checked for RAM use; perhaps
 * something should be periodically shrunk if applicable.  Look for `vector`s, `deque`s (including inside `queue`s).
 */
class Native_socket_stream::Impl :
  public flow::log::Log_context,
  private boost::noncopyable // And not movable.
{
public:
  // Constants.

  /// See Native_socket_stream counterpart.
  static const size_t S_MAX_META_BLOB_LENGTH;

  // Constructors/destructor.

  /**
   * See Native_socket_stream counterpart.
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

  /// See Native_socket_stream counterpart.
  ~Impl();

  // Methods.

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  Native_socket_stream release();

  /**
   * See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  const std::string& nickname() const;

  /**
   * See Native_socket_stream counterpart.
   *
   * @param create_ev_wait_hndl_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool replace_event_wait_handles
         (const Function<util::sync_io::Asio_waitable_native_handle ()>& create_ev_wait_hndl_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param err_code
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code) const;

  // Connect-ops API.

  /**
   * See Native_socket_stream counterpart.
   *
   * @param ev_wait_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool start_connect_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param absolute_name
   *        See Native_socket_stream counterpart.
   * @param sync_err_code
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_connect(const Shared_name& absolute_name, Error_code* sync_err_code,
                     flow::async::Task_asio_err&& on_done_func);

  // Send-ops API.

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
   * @param ev_wait_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool start_send_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param ev_wait_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool start_send_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func);

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
   * @param sync_err_code
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_end_sending(Error_code* sync_err_code, flow::async::Task_asio_err&& on_done_func);

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

  // Receive-ops API.

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
   * @param ev_wait_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool start_receive_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param ev_wait_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool start_receive_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param target_hndl
   *        See Native_socket_stream counterpart.
   * @param target_meta_blob
   *        See Native_socket_stream counterpart.
   * @param sync_err_code
   *        See Native_socket_stream counterpart.
   * @param sync_sz
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_receive_native_handle(Native_handle* target_hndl, const util::Blob_mutable& target_meta_blob,
                                   Error_code* sync_err_code, size_t* sync_sz,
                                   flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * See Native_socket_stream counterpart.
   *
   * @param target_blob
   *        See Native_socket_stream counterpart.
   * @param sync_err_code
   *        See Native_socket_stream counterpart.
   * @param sync_sz
   *        See Native_socket_stream counterpart.
   * @param on_done_func
   *        See Native_socket_stream counterpart.
   * @return See Native_socket_stream counterpart.
   */
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          Error_code* sync_err_code, size_t* sync_sz,
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
  // Types.

  /// Overall state of a Native_socket_stream::Impl.
  enum class State
  {
    /**
     * Not a peer.  Barring moves-from/moves-to: Possible initial state (via certain ctors); goes to CONNECTING
     * (via async_connect()).  Only async_connect() is possible in this state (other public methods tend to return
     * immediately).
     */
    S_NULL,

    /**
     * Not a peer but async_connect() in progress to try to make it a peer.  Barring moves-from/moves-to:
     * Entry from NULL; goes to PEER or NULL.  Public methods tend to return immediately in this state.
     */
    S_CONNECTING,

    /**
     * Is or was a connected peer.  Barring moves-from/moves-to:
     * Possible initial state (via certain ctors); entry from CONNECTING; does not transition to any other state
     * (once a PEER, always a PEER).  async_connect() is not possible in this state (it returns immediately); other
     * public methods are possible.
     */
    S_PEER
  }; // enum class State

  /**
   * Data store representing a payload corresponding to exactly one attempted async write-op, albeit
   * used if and only if we encounter would-block in `send_*()` or snd_on_ev_auto_ping_now_timer_fired()
   * or `*end_sending()` and have to queue (and therefore one-time-copy) data in #m_snd_pending_payloads_q.
   *
   * Note this does *not* represent an original user message given to Native_socket_stream::send_native_handle()
   * but rather a low-level payload per the protocol described in the impl section of class
   * doc header (payload 1, payload 2 if applicable).  We store them in queue via #Ptr.
   */
  struct Snd_low_lvl_payload
  {
    // Types.

    /// Short-hand for `unique_ptr` to this.
    using Ptr = boost::movelib::unique_ptr<Snd_low_lvl_payload>;

    // Data.

    /// The native handle to transmit in the payload; `.null() == true` to transmit no such thing.
    Native_handle m_hndl_or_null;

    /**
     * The buffer to transmit in the payload; `!m_blob.empty() == true`, period.  Note that this is the actual buffer
     * and not a mere location/size of an existing blob somewhere.
     */
    flow::util::Blob m_blob;
  }; // struct Snd_low_lvl_payload

  /**
   * Identical to sync_io::Async_adapter_receiver::User_request, except we only keep at most 1 of these
   * and thus don't need a `Ptr` alias inside.  As in that other class, this records the args from an
   * async_receive_native_handle() or async_receive_blob() call.
   */
  struct Rcv_user_request
  {
    // Data.

    /// Same as in sync_io::Async_adapter_receiver::User_request.
    Native_handle* m_target_hndl_ptr;
    /// Same as in sync_io::Async_adapter_receiver::User_request.
    util::Blob_mutable m_target_meta_blob;
    /// Same as in sync_io::Async_adapter_receiver::User_request.
    flow::async::Task_asio_err_sz m_on_done_func;
  }; // struct Rcv_user_request

  /**
   * Used to organize tje incoming-direction state machine tactically, this indicates what part of payload 1
   * ("head payload," mandatory) or payload 2 ("meta-blob payload," optional) we are currently reading, based
   * on the next byte to be read.  Relevant only if #m_rcv_user_request is not null (an async-receive is in progress).
   */
  enum class Rcv_msg_state
  {
    /// Reading start of payload 1: already have nothing -- no #Native_handle; no 1st byte.
    S_MSG_START,

    /**
     * Reading payload 1, but at least byte 2: alreadu have at least byte 1 and either the #Native_handle or its lack.
     * At least 1 more byte, of `sizeof(m_rcv_target_meta_length)`, is remaining.  (As of this writing -- that
     * `sizeof()` is 2... so in fact in this state exactly 1 byte remains.  No need to rely on that in code though.)
     */
    S_HEAD_PAYLOAD,

    /**
     * Reading payload 2; M bytes remain, where M is in [1, N], and N is the meta-length encoded in preceding
     * payload 1.
     */
    S_META_BLOB_PAYLOAD
  }; // enum class Rcv_msg_state

  /**
   * Compile-time tagging enumeration identifying the op-type of a given `sync_io` activity.
   * We use it to save on boiler-plate code which is almost identical regardless of the op-type in question.
   * `if constexpr()`, etc.
   */
  enum class Op
  {
    // Send-ops, as started via `start_send_*_ops()`.
    S_SND,
    // Receive-ops, as started via `start_receive_*_ops()`.
    S_RCV,
    // Connect-ops, as started via start_connect_ops().
    S_CONN
  };

  /// The type used to encode the meta-blob length; this puts a cap on how long the meta-blobs can be.
  using low_lvl_payload_blob_length_t = uint16_t;
  static_assert(std::numeric_limits<low_lvl_payload_blob_length_t>::is_integer
                  && (!std::numeric_limits<low_lvl_payload_blob_length_t>::is_signed),
                "low_lvl_payload_blob_length_t is a length type, so it must be an unsigned integer of some kind.");

  // Constants.

  /**
   * Value for the length field in payload 1 that means "not a length; indicating this is a ping message."
   * The other special values is 0 which indicates graceful close.  Also #S_MAX_META_BLOB_LENGTH must be adjusted
   * accordingly; S_META_BLOB_LENGTH_PING_SENTINEL being 0xFF....
   */
  static const low_lvl_payload_blob_length_t S_META_BLOB_LENGTH_PING_SENTINEL;

  // Constructors.

  /**
   * Helper delegated-to ctor that sets up everything except #m_peer_socket (left null) and
   * #m_ev_wait_hndl_peer_socket (left holding no native handle); and sets NULL #m_state;
   * the real ctor shall set both of them to their real initial values.
   *
   * @param logger_ptr
   *        See other ctors.
   * @param nickname_str
   *        See other ctors.
   * @param tag
   *        Ctor-selecting tag.
   */
  Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str, std::nullptr_t tag);

  // Methods.

  // Connect-ops.

  /**
   * Handler for the async-wait in case async_connect() cannot synchronously complete the #m_peer_socket
   * connect (boost.asio yields would-block).  Post-condition: `on_done_func()` has finished.
   * Note that this situation is unlikely, this being a Unix domain socket (details commented inside async_connect()).
   * (That comment says that in Linux as of this writing it is impossible.)  Moreover if we did get here,
   * then we have to assume it's writable -- connected -- and can't (or at least don't) try to see if
   * the connect really failed.  That said, trying to use it will expose any error shortly (in PEER state).
   *
   * @param on_done_func
   *        See async_connect().
   */
  void conn_on_ev_peer_socket_writable(flow::async::Task_asio_err&& on_done_func);

  // Send-ops.

  /**
   * `*end_sending()` body.
   *
   * @param sync_err_code_ptr_or_null
   *        See async_end_sending().  Null if and only if invoked from the no-arg end_sending().
   *        Note well: even if `async_end_sending(sync_err_code = nullptr)` was used, this must not be null.
   * @param on_done_func_or_empty
   *        See async_end_sending().  `.empty() == true` if and only if invoked from the no-arg end_sending().
   * @return See async_end_sending() and end_sending().
   */
  bool async_end_sending_impl(Error_code* sync_err_code_ptr_or_null,
                              flow::async::Task_asio_err&& on_done_func_or_empty);

  /**
   * Handler for the async-wait, via util::sync_io::Timer_event_emitter, of the auto-ping timer firing;
   * if all is cool, sends auto-ping and schedules the next such async-wait.  That wait itself can be rescheduled
   * when non-idleness (other send attempts) occurs.  If all is not cool -- sends finished via `*end_sending()`,
   * out-pipe hosed -- then neither sends auto-ping nor schedules another.
   */
  void snd_on_ev_auto_ping_now_timer_fired();

  /**
   * Either synchronously sends `hndl_or_null` handle (if any) and `orig_blob` low-level blob over #m_peer_socket,
   * or if an async-send is in progress queues both to be sent later; in the former case any unsent trailing portion
   * of the payload is queued and async-sent via snd_async_write_q_head_payload(), with dropping-sans-queuing
   * allowed under certain circumstances in `avoid_qing` mode.  For details on the latter see below.
   *
   * `*err_code` is set to the error to return, suitably for #m_snd_pending_err_code; and if no such
   * outgoing-pipe-hosing is synchronously encountered it is set to falsy.  In particular, if `!*err_code` upon return,
   * you may call this again to send the next low-level payload.  Otherwise #m_peer_socket cannot be subsequently used
   * in either direction (connection is hosed).
   *
   * ### `avoid_qing` mode for auto-ping ###
   * Setting this arg to `true` slightly modifies the above behavior as follows.  Suppose `orig_blob` encodes an
   * auto-ping message (see auto_ping()).  (So `handle_or_null` must be `.null()`.)  Its purpose is to inform the
   * opposing side that we are alive/not idle.  So suppose this method is unable to send *any* of `orig_blob`,
   * either because there are already-queued bytes waiting to be sent pending writability (due to earlier would-block),
   * or because the kernel out-buffer has already-queued bytes waiting to be popped by receiver, and there is no
   * space there to enqueue any of `orig_blob`.  Then the receiver must not be keeping up with us, and the next
   * pop of the kernel buffer will get *some* message, even if it's not the auto-ping we wanted to send;
   * hence they'll know we are not-idle without the auto-ping.  So in that case this method shall:
   *   - disregard `orig_blob` (do not queue it -- drop it, as it would be redundant anyway);
   *   - return `true` if and only if the size of the out-queue is 0 (though as of this writing the caller should
   *     not care: auto-ping is a fire-and-forget operation, as long as it does not detect a pipe-hosing error).
   *
   * @param hndl_or_null
   *        Similar to send_native_handle().  However it must be `.null()` if `avoid_qing == true`.
   * @param orig_blob
   *        Blob to send, with `hndl_or_null` associated with byte 1 of this.  It must have size 1 or greater,
   *        or behavior is undefined.
   * @param err_code
   *        Success or failure is registered in `*err_code` per above.  If null behavior is undefined.
   * @param avoid_qing
   *        See above.  `true` <=> will return success (act as-if all of `orig_blob` was sent)
   *        if no bytes of `orig_blob` could be immediately sent.
   * @return `false` if outgoing-direction pipe still has queued stuff in it that must be sent once transport
   *         becomes writable; `true` otherwise.  If `true` is returned, but `avoid_qing == true`, then
   *         possibly `orig_blob` was not sent (at all); was dropped.
   */
  bool snd_sync_write_or_q_payload(Native_handle hndl_or_null,
                                   const util::Blob_const& orig_blob, Error_code* err_code,
                                   bool avoid_qing);

  /**
   * Initiates async-write over #m_peer_socket of the low-level payload at the head of out-queue
   * #m_snd_pending_payloads_q, with completion handler snd_on_ev_peer_socket_writable_or_error().
   * The first step of this is an async-wait via `sync_io` pattern.
   *
   * `*err_code` has the same semantics as snd_sync_write_or_q_payload().
   *
   * @param err_code
   *        Same as snd_sync_write_or_q_payload().
   */
  void snd_async_write_q_head_payload(Error_code* err_code);

  /**
   * Completion handler, from outside event loop via `sync_io` pattern, for the async-wait initiated by
   * snd_async_write_q_head_payload().  Continues the async-send chain by trying to
   * snd_nb_write_low_lvl_payload() as much of #m_snd_pending_payloads_q as it can synchronously;
   * invokes snd_async_write_q_head_payload() again if not all could be so sent.  Lastly, if indeed it sends-out
   * everything, or encounters out-pipe being hosed, and async_end_sending() completion handler is pending
   * to be called -- it ensures that occurs (synchronously inside).
   */
  void snd_on_ev_peer_socket_writable_or_error();

  /**
   * Utility that sends non-empty `blob`, and (unless null) `hndl_or_null` associated with its 1st byte,
   * synchronously to the maximum extent possible without blocking, over #m_peer_socket.
   * This is used for all sync-writes in this class.  Note the result semantics are slightly different from
   * boost.asio and its extensions, so watch out about would-block and such:
   *
   * `*err_code` shall be set to success unless #m_peer_socket connection is found to be hosed at any point throughout
   * the operation.  In particular neither would-block nor would-block after non-zero amount of data had been sent
   * are considered errors; 0 or a value `< blob.size()` are returned in those situations respectively.
   * If `*err_code` is made truthy, #m_peer_socket shall not be used (in either direction) subsequently.
   *
   * @param hndl_or_null
   *        Same as snd_sync_write_or_q_payload().
   * @param blob
   *        Same as snd_sync_write_or_q_payload().
   * @param err_code
   *        Same as snd_sync_write_or_q_payload().  Also see above: would-block is not an error.
   * @return 0 meaning neither `hndl_or_null` (if any) nor any `blob` bytes were sent;
   *         [1, `blob.size()`] if `hndl_or_null` (if any) and that number of `blob` bytes were sent.
   *         `*err_code` shall be truthy only if (but not necessarily if) `< blob.size()` is returned.
   */
  size_t snd_nb_write_low_lvl_payload(Native_handle hndl_or_null, const util::Blob_const& blob, Error_code* err_code);

  // Receive-ops.

  /**
   * Handler for the async-wait, via util::sync_io::Timer_event_emitter, of the idle timer firing;
   * if still relevant it records the idle-timeout error in #m_rcv_pending_err_code; and if
   * an async_receive_native_handle_impl() is in progress (awaiting data via async-wait), it completes
   * that operation with the appropriate idle-timeout error (completion handler in #m_rcv_user_request runs
   * synchronously).  If not still relevant -- #m_rcv_pending_err_code already is truthy -- then no-ops.
   */
  void rcv_on_ev_idle_timer_fired();

  /**
   * No-ops if idle_timer_run() is not engaged; otherwise reacts to non-idleness of the in-pipe by
   * rescheduling idle timer to occur in #m_rcv_idle_timeout again.
   * (Other code calls this, as of this writing, on receipt of a complete message.)
   *
   * Note that this can only occur while an async_receive_native_handle_impl() is in progress; as otherwise
   * we will not be reading the low-level in-pipe at all.  This is a requirement for using
   * idle_timer_run(), so it's not our fault, if they don't do it and get timed-out.
   */
  void rcv_not_idle();

  /**
   * Body of both async_receive_native_handle() and async_receive_blob().  In the latter case
   * `target_hndl_or_null` shall be null (otherwise it shall not).
   *
   * @param target_hndl_or_null
   *        See async_receive_native_handle(); or null if async_receive_blob().
   * @param target_meta_blob
   *        See async_receive_native_handle().
   * @param sync_err_code
   *        See async_receive_native_handle().
   * @param sync_sz
   *        See async_receive_native_handle().
   * @param on_done_func
   *        See async_receive_native_handle().
   * @return See async_receive_native_handle().
   */
  bool async_receive_native_handle_impl(Native_handle* target_hndl_or_null, const util::Blob_mutable& target_meta_blob,
                                        Error_code* sync_err_code, size_t* sync_sz,
                                        flow::async::Task_asio_err_sz&& on_done_func);

  /**
   * Begins read chain (completing it as synchronously as possible, async-completing the rest) for the next
   * in-message.
   *
   * Given the pre-condition that (1) async_receive_native_handle_impl() is oustanding (#m_rcv_user_request not null),
   * (2) in the in-pipe we expect byte 1 of the next in-message next, (3) there is no known in-pipe error
   * already detected, and (4) there is no known would-block condition on the in-pipe: this reads (asynchronously
   * if would-block is encountered at some point in there) the next message.
   *
   * @param sync_err_code
   *        Outcome out-arg: error::Code::S_SYNC_IO_WOULD_BLOCK if async-wait triggered, as message could not be
   *        fully read synchronously; falsy if message fully read synchronously; non-would-block truthy value,
   *        if pipe-hosing condition encountered.
   * @param sync_sz
   *        Outcome out-arg: If `*sync_err_code` truthy then zero; else size of completed in-message.
   */
  void rcv_read_msg(Error_code* sync_err_code, size_t* sync_sz);

  /**
   * A somewhat-general utility that continues read chain with the aim to complete the present in-message,
   * with the pre-condition (among others) that (1) there is no known would-block condition on the in-pipe, and
   * (2) at least byte 1 + the handle-or-not within payload 1 have already been acquired.
   *
   * @param msg_state
   *        Rcv_msg_state::S_HEAD_PAYLOAD or Rcv_msg_state::S_META_BLOB_PAYLOAD, indicating which
   *        thing is being read: trailing bytes (not including byte 1) of payload 1; or
   *        all or trailing bytes of payload 2 (the meta-blob).
   * @param target_blob
   *        The target buffer: its size indicates the bytes necessary to complete the payload `msg_state`.
   *        E.g., if META_BLOB_PAYLOAD and 3 of (whatever m_rcv_target_meta_length indicates, from payload 1)
   *        bytes are still needed, then `target_blob.size() == 3`, and `target_blob.data()` points to
   *        the end of the meta-blob (in #m_rcv_user_request) minus 3.
   * @param sync_err_code
   *        Outcome out-arg: error::Code::S_SYNC_IO_WOULD_BLOCK if async-wait triggered, as message could not be
   *        fully read synchronously; falsy if message fully read synchronously; non-would-block truthy value,
   *        if pipe-hosing condition encountered.
   * @param sync_sz
   *        Outcome out-arg: If `*sync_err_code` truthy then zero; else size of completed in-message.
   */
  void rcv_read_blob(Rcv_msg_state msg_state, const util::Blob_mutable& target_blob,
                     Error_code* sync_err_code, size_t* sync_sz);

  /**
   * Helper of rcv_read_msg() -- it could have been inlined instead of a method but for readability concerns --
   * that reacts to that guy's initial nb-read (into `Native_handle` + leading bytes of payload 1 blob)
   * getting at least 1 byte (and therefore the `Native_handle` if any).  A number of things can happen
   * depending on `n_rcvd`; and if `n_rcvd` = all of payload 1, it's rcv_on_head_payload() time.
   *
   * @param hndl_or_null
   *        The handle, or none, received with byte 1 of payload 1.
   * @param n_rcvd
   *        How many bytes of payload 1 were received.  Must be at least 1, or behavior undefined (assertion may
   *        trip).
   * @param sync_err_code
   *        Outcome out-arg: error::Code::S_SYNC_IO_WOULD_BLOCK if async-wait triggered, as message could not be
   *        fully read synchronously; falsy if message fully read synchronously; non-would-block truthy value,
   *        if pipe-hosing condition encountered.
   * @param sync_sz
   *        Outcome out-arg: If `*sync_err_code` truthy then zero; else size of completed in-message.
   */
  void rcv_on_handle_finalized(Native_handle hndl_or_null, size_t n_rcvd,
                               Error_code* sync_err_code, size_t* sync_sz);

  /**
   * Reacts to payload 1 having been completely received.  At this point #m_rcv_target_meta_length is ready,
   * and #m_peer_socket is not known to be in would-block state (which isn't to say it's *not* in would-block
   * state).
   *   - So if that value contains auto-ping marker, then it's rcv_read_msg() all over again.
   *   - If it's the graceful-close marker, then the in-pipe (but not out-pipe necessarily) is hosed with
   *     graceful-close "error"; hence the completion handler in #m_rcv_user_request is readied for execution;
   *     the read-chain stops until the next async_receive_native_handle_impl().
   *   - Otherwise we need payload 2; rcv_read_blob() is initiated (with Rcv_msg_state::S_META_BLOB_PAYLOAD)
   *     to read all of it.
   *
   * @param sync_err_code
   *        Outcome out-arg: error::Code::S_SYNC_IO_WOULD_BLOCK if async-wait triggered, as message could not be
   *        fully read synchronously; falsy if message fully read synchronously; non-would-block truthy value,
   *        if pipe-hosing condition encountered.
   * @param sync_sz
   *        Outcome out-arg: If `*sync_err_code` truthy then zero; else size of completed in-message.
   */
  void rcv_on_head_payload(Error_code* sync_err_code, size_t* sync_sz);

  /**
   * Completion handler, from outside event loop via `sync_io` pattern, for the async-wait initiated by
   * various `rcv_*()` methods trying to get to the goal of obtaining a complete in-message.
   * It tries to resume from whatever point in that algorithm we were at, when an rcv_nb_read_low_lvl_payload()
   * indicated would-block, precipitating the async-wait that has now completed.
   * That point is indicated by the args, which were memorized (captured) at the time the async-wait was
   * started.
   *
   * @param msg_state
   *        Communicates where we're at in the in-message: the very start; or at byte 2+ of payload 1;
   *        or in payload 2.
   * @param n_left
   *        How many bytes are left to read within payload 1 or payload 2 (depending on `msg_state`).
   *        Ignored for Rcv_msg_state::S_MSG_START (by definition all of payload 1 remains then).
   */
  void rcv_on_ev_peer_socket_readable_or_error(Rcv_msg_state msg_state, size_t n_left);

  /**
   * Utility that synchronously, non-blockingly attempts to read over #m_peer_socket into the target blob and
   * (optionally) `Native_handle` (nullifying it if not present), reporting error or would-block if encountered.
   * This is used for all sync-reads in this class.  Note the result semantics are slightly different from
   * boost.asio and its extensions, so watch out about would-block and such:
   *
   * `*err_code` shall be set to success unless #m_peer_socket connection is found to be hosed at any point throughout
   * the operation.  In particular neither would-block nor would-block after non-zero amount of data had been read
   * are considered errors; 0 or a value `< target_payload_blob.size()` are returned in those situations respectively.
   * If `*err_code` is made truthy, #m_peer_socket shall not be used (in either direction) subsequently.
   *
   * @param target_payload_hndl_or_null
   *        If null, we will use a plain-read (even if there is a `Native_handle`, we will be none-the-wiser --
   *        we are not expecting it, so only pass null at the proper points in the protocol stream).
   *        Otherwise, if and only if 1+ bytes are read OK, then the pointee is set to either null (byte 1 not
   *        accompanied by a handle) or non-null (it was-too accompanied by a handle).
   * @param target_payload_blob
   *        Target buffer (its size indicating how many bytes we want if possible).
   * @param err_code
   *        See above: would-block is not an error.  Return value should be ignored if `*err_code` has been made truthy.
   * @return If `*err_code` is falsy at return time:
   *         0 meaning`*target_payload_hndl_or_null` is indeterminate (if ptr was not null), and no bytes were
   *         received + would-block;
   *         [1, `blob.size()`] if `*target_payload_hndl_or_null` has been finalized (if ptr was not null),
   *         and that many bytes were indeed read into the start of `target_payload_blob`.
   *         `*err_code` shall be truthy only if (but not necessarily if) `< blob.size()` is returned.
   */
  size_t rcv_nb_read_low_lvl_payload(Native_handle* target_payload_hndl_or_null,
                                     const util::Blob_mutable& target_payload_blob,
                                     Error_code* err_code);

  // Utilities.

  /**
   * Checks whether #m_state has reached State::S_PEER; if so returns `true`; if not logs WARNING and returns `false`.
   * In the latter case the caller should immediately return; in the former it should continue.
   *
   * Intended use: All public APIs that require PEER state shall do this ~first-thing.
   *
   * ### Rationale ###
   * In the context of reaching PEER state, #m_state is essentially a barrier of sorts: PEER is the terminal state
   * (one cannot exit it until dtor), and connected-state operations all promise to immediately return `false`
   * (or equivalent) unless PEER has been reached.
   *
   * @param context
   *        Description of caller (probably `"...func_name...(...more-info-maybe...)"`) for logging.
   * @return See above.
   */
  bool state_peer(util::String_view context) const;

  /**
   * For boiler-plate-reducing generic code: Returns the `m_*_ev_wait_func` corresponding to the given Op.
   *
   * @tparam OP
   *         See Op.
   * @return See above.
   */
  template<Op OP>
  const util::sync_io::Event_wait_func* sync_io_ev_wait_func() const;

  /**
   * `const` version of the other overload.
   *
   * @tparam OP
   *         See above.
   * @return See above.
   */
  template<Op OP>
  util::sync_io::Event_wait_func* sync_io_ev_wait_func();

  /**
   * Helper that returns `true` silently if the given Op `start_*_ops()` has been called; else
   * logs WARNING and returns `false`.
   *
   * @param context
   *        For logging: the algorithmic context (function name or whatever).
   * @return See `start_*_ops()`.
   */
  template<Op OP>
  bool op_started(util::String_view context) const;

  /**
   * Boiler-plate-reducing body of `start_*_ops()` for the given Op.
   *
   * @tparam OP
   *         See Op.
   * @param ev_wait_func
   *        See `start_*_ops()`.
   * @return See `start_*_ops()`.  Note that start_connect_ops(), as a special case, no-ops and returns `false`
   *         if #m_state is not NULL.
   */
  template<Op OP>
  bool start_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  // Data.

  // General data (both-direction pipes, connects, general).

  /// See nickname().
  std::string m_nickname;

  /**
   * The current state of `*this`.
   *
   * @see `State` doc header for details about transitions, initial and terminal states, etc.
   *
   * ### Rationale ###
   * Long story short this exists purely to ensure (1) the user only attempts transmission-related public ops
   * once in the (terminal) PEER state; and (2) the user only attempts async_connect() while in the (initial)
   * NULL state (and not, say, while already CONNECTING; or already connected (PEER)).
   *
   * state_peer() handles the check for PEER.
   *
   * async_connect() and its completion handler conn_on_async_connect_or_error() handle all transitions
   * (NULL -> CONNECTING, CONNECTING -> NULL, CONNECTING -> PEER).
   *
   * ### Thread safety ###
   * First take a look at "Thread safety" in sync_io::Native_socket_stream class public doc header.
   * Long story short, it says that the only relevant concurrency we must allow is a receive-op being
   * invoked concurrently with a send-op while in PEER state.  And indeed most such methods do check
   * #m_state near the top.  However, by definition, in PEER state, #m_state is constant.  Therefore
   * no locking is needed.  (Contrast, potentially, with `m_*peer_socket` -- the only other mutable datum
   * accessed by both directions' algorithms.)
   */
  State m_state;

  /**
   * The `Task_engine` for #m_peer_socket.  It is necessary to construct the `Peer_socket`, but we never
   * use that guy's `->async_*()` APIs -- only non-blocking operations, essentially leveraging boost.asio's
   * portable transmission APIs but not its actual, um, async-I/O abilities in this case.  Accordingly we
   * never load any tasks onto #m_nb_task_engine and certainly never `.run()` (or `.poll()` or ...) it.
   *
   * In the `sync_io` pattern the user's outside event loop is responsible for awaiting readability/writability
   * of a guy like #m_peer_socket via our exporting of its `.native_handle()`.
   */
  flow::util::Task_engine m_nb_task_engine;

  /**
   * The `Task_engine` for `m_*ev_wait_hndl_*`, unless it is replaced via replace_event_wait_handles().
   * There are 2 possibilities:
   *   - They leave this guy associated with `m_*ev_wait_hndl_*`.  Then no one shall be doing `.async_wait()` on
   *     them, and instead the user aims to perhaps use raw `[e]poll*()` on their `.native_handle()`s.
   *     We still need some `Task_engine` to construct them though, so we use this.
   *   - They use replace_event_wait_handles(), and therefore this becomes dissociated with `m_*ev_wait_hndl_*` and
   *     becomes completely unused in any fashion, period.  Then they shall probably be doing
   *     their own `.async_wait()` -- associated with their own `Task_engine` -- on `m_*ev_wait_hndl_*`.
   *
   * This is all to fulfill the `sync_io` pattern.
   */
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /**
   * The peer stream-type Unix domain socket; or null pointer if we've detected the connection has become hosed
   * and hence closed+destroyed the boost.asio `Peer_socket`.  Note that (1) it starts non-null, and is in
   * "open" (FD-bearing) state from the start -- even if the NULL-state ctor was used; and (2) can only
   * become null (irreversibly so) when in PEER state, only on error.
   *   - In State::S_NULL #m_state, this is an "open" (FD-bearing) but unconnected socket.
   *   - In State::S_CONNECTING #m_state, this is the same socket, in the process of connecting.
   *     (If connect fails, the FD persists but goes back to just a steady-state "open" unconnected state.)
   *   - In State::S_PEER #m_state, this is either the same socket, now connected; or, upon a detected error in
   *     either direction, a null pointer.
   *
   * Moreover the FD cannot be replaced with another FD: if PEER-state ctor is used, then that FD is connected
   * from the start; if NULL-state ctor is used, then that FD is unconnected but may become connected (if/when)
   * State::S_PEER is reached (via async_connect()).
   *
   * `*m_peer_socket` is used *exclusively* for non-blocking calls; *never* `.async_*()`.  That conforms to
   * the `sync_io` pattern.  See #m_nb_task_engine doc header.
   *
   * ### Rationale ###
   * The above facts are important -- namely that from the very start (up to an error) the FD is loaded and never
   * changes -- because the watcher FD-wrappers (1 for each op-type: connection, sends, receives) used in
   * the `sync_io` pattern can be simply and correctly initialized at any time and not change thereafter.
   * E.g., `start_send_*_ops()` can be called by the user at any time, even in NULL state.
   *
   * As for the null-pointer state, it exists for 2 reasons which are somewhat in synergy:
   *   - It is a way of giving back the FD-resource to the kernel as early as possible.
   *     (Destroying `Peer_socket` closes the contained FD/socket.)
   *   - It is a way for the incoming-direction processing logic to inform the outgoing-direction counterpart logic
   *     that it has detected the socket is entirely hosed,in both directions (and hence no need to even try further
   *     operations on it); and vice versa.
   *
   * As to that 2nd reason: a couple of other approaches would work:
   *   - We could simply let the 2nd-to-detect-failure processing direction discover the hosedness by naively
   *     trying an operation and getting the inevitable error from the kernel (probably the same one).
   *     - This is actually a pretty good alternative approach; it might even be better:
   *       As written direction 2 will get a catch-all `S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND` or
   *       `S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE` error; while with the alternative approach we could
   *       get the "real" error in both directions.  There are benefits to each approach; the alternative one
   *       provides more info when looking at just one direction, while the existing one is quicker and
   *       less entropy-laden (arguably), returns the FD/socket resource sooner, and still makes the true error
   *       available.  See to-do at the end of this doc header, as the alternative approach may carry another
   *       benefit.
   *   - We could have incoming-direction logic check #m_snd_pending_err_code/outgoing-direction check
   *     #m_rcv_pending_err_code ahead of accessing #m_peer_socket.
   *     - This idea sucks.  It breaks the intentional and important (for perf) guarantee under
   *       "Thread safety" in the sync_io::Native_socket_stream doc header, wherein
   *       receive-ops logic and send-ops logic may execute concurrently (as now the latter accesses some of
   *       the former's data and vice versa).
   *
   * ### Concurrency, synchronization ###
   * That brings us to the key discussion of concurrency protection: #m_peer_socket is the
   * *only* non-`const`-accessed datum in `*this` that may be accessed concurrently by receive-ops methods
   * and send-ops methods.  Namely: send-ops are started by `start_send_*_ops()`; receive-ops are started by
   * `start_receive_*_ops()`; in PEER state all logic to do with sending can occur entirely concurrently with all
   * logic to do with receiving.  Except, however, they both use #m_peer_socket -- for *non-blocking calls only!* --
   * as #m_peer_socket is bidirectional.  In particular, for example, `m_peer_socket->read_some()` might execute
   * concurrently to `m_peer_socket->write_some()`.  In addition, #m_peer_socket pointer might be nullified (due to
   * error) by one direction's logic, while the other is trying to dereference it and execute a method.
   *
   * This is resolved by our locking #m_peer_socket_mutex, albeit in a very tight critical section that always
   * looks like:
   *   -# Lock mutex.
   *   -# Check if it's null; if so unlock/exit algorithm.  Else:
   *   -# Attempt synchronous, non-blocking operation on dereferenced pointer.
   *   -# If it exposed socket error, nullify pointer.
   *   -# Unlock mutex.
   *
   * It's tight, so lock contention should be minimal.  Also no other mutex is locked inside such a critical
   * section, so deadlock chance is nil.  However see the following to-do for an alternative approach.
   *
   * @todo `Peer_socket m_peer_socket` synchronous-read ops (`read_some()`) are actually
   * documented in boost::asio to be thread-safe against concurrently invoked synchronous-write ops
   * (`write_some()`), as are OS calls `"::recvmsg()"`, `"::sendmsg()"`; therefore for possible perf bump
   * consider never nullifying Impl::m_peer_socket; eliminating Impl::m_peer_socket_mutex; and
   * letting each direction's logic discover any socket-error independently.  (But, at the moment,
   * `m_peer_socket_mutex` also covers mirror-guy #m_ev_wait_hndl_peer_socket, so that must be worked out.)
   *
   * To be clear, the text above
   * w/r/t concurrency/synchronization says `->write_some()` and `->read_some()` might execute concurrently --
   * but boost.asio docs (`basic_stream_socket` page) indicate that is actually okay; and implies
   * the same about underlying OS calls (which includes `readmsg()` and `writemsg()` which we use when
   * `Native_handle` transmission is needed).  Now, the nullification versus dereferencing of the `unique_ptr`
   * wrapping `m_peer_socket`: that is indeed not thread-safe, which is why we'd need to also get rid of that method
   * of communicating socket-hosed state from one direction to the other.  However, as we wrote above, the
   * alternate approach, where each direction discovers the socket error independently, would be compatible
   * with `m_peer_socket` not needing to be a `unique_ptr` at all.  That said the socket would not be given
   * back to the system as early (potentially) -- note that boost.asio `close()` is noted to not be
   * safe against concurrent reads/writes -- and in general the approach "feels" more entropy-laden.
   * I (ygoldfel) must say, though; it does sound entirely viable nevertheless; and it would be a limited change
   * that would also eliminate error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND
   * and error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE.  One could even argue it is elegant, in that
   * it decouples the 2 directions as much as humanly possible, to the point of even using the kernel's support
   * for it.  (Last note: the boost.asio docs single out `connect()`, `shutdown()`, `send()`, `receive()`
   * as okay to invoke concurrently; not `read_some()` and `write_some()`; but I very strongly suspect
   * this is only an omission, perhaps due to `*_some()` appearing for uniformity in a boost.asio overhaul
   * after those docs were written; in the source it is clear they invoke the same exact stuff.  Googling shows,
   * generally, the underlying OS calls including `*msg()` are thread-safe to use in this manner also.)
   */
  boost::movelib::unique_ptr<asio_local_stream_socket::Peer_socket> m_peer_socket;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_peer_socket.
   *
   * Protected by #m_peer_socket_mutex, along with #m_peer_socket.  To be accessed only if an error hasn't
   * nullified #m_peer_socket since the last critical section.  Now, "accessed" *always* (once set up)
   * means calling `m_*_ev_wait_wait_func` with this guy as the 1st arg; it can be pictured as a `sync_io`
   * equivalent of `m_peer_socket->async_wait()`.  However it does mean the `*this` user must be careful
   * not to lock something in that function that is already locked when they call `(*on_active_ev_func)()` --
   * that would cause a deadlock -- and if it's a recursive mutex, then they risk an AB/BA deadlock.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_peer_socket;

  /// Protects #m_peer_socket and its bro #m_ev_wait_hndl_peer_socket.
  flow::util::Mutex_non_recursive m_peer_socket_mutex;

  /**
   * As typical in timer-needing `sync_io`-pattern-implementing objects, maintains a thread exclusively for
   * `Timer` wait completion handlers which ferry timer-fired events to internal IPC-mechanisms waitable
   * by the `sync_io`-pattern-using outside event loop.  In our case we (optionally) maintain the auto-ping timer
   * (send direction) and idle timer (receive direction).
   *
   * @see util::sync_io::Timer_event_emitter doc header for design/rationale discussion.
   */
  util::sync_io::Timer_event_emitter m_timer_worker;

  // Connect-ops data.

  /**
   * Function (set forever in start_connect_ops()) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to connect-ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_conn_ev_wait_func;

  // Outgoing-direction data.

  /**
   * Queue storing (at head) the currently in-progress async write-op of a Snd_low_lvl_payload; followed by
   * the payloads that should be written after that completes, in order.
   *
   * Relevant only once terminal State::S_PEER is reached.  In that state only touched if would-block is
   * encountered in... well, see Snd_low_lvl_payload doc header.
   */
  std::queue<Snd_low_lvl_payload::Ptr> m_snd_pending_payloads_q;

  /**
   * The first and only connection-hosing error condition detected when attempting to low-level-write on
   * #m_peer_socket; or falsy if no such error has yet been detected.  Among possible other uses, it is returned
   * by send_native_handle() and the completion handler of async_end_sending().
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  Error_code m_snd_pending_err_code;

  /**
   * `false` at start; set to `true` forever on the first `*end_sending()` invocation;
   * `true` will prevent any subsequent send_native_handle()/send_blob() calls from proceeding.
   * See class doc header impl section for design discussion.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  bool m_snd_finished;

  /**
   * Function passed to async_end_sending(), if it returned `true` and was unable to synchronously flush everything
   * including the graceful-close itself (synchronously detecting new or previous pipe-hosing error *does* entail
   * flushing everything); otherwise `.empty()`.  It's the completion handler of that graceful-close-send API.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  flow::async::Task_asio_err m_snd_pending_on_last_send_done_func_or_empty;

  /**
   * Equals `zero()` before auto_ping(); immutably equals `period` (auto_ping() arg) subsequently to that first
   * successful call (if any).
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  util::Fine_duration m_snd_auto_ping_period;

  /**
   * Timer that fires snd_on_ev_auto_ping_now_timer_fired() (which sends an auto-ping) and is always
   * scheduled to fire #m_snd_auto_ping_period after the last send (send_native_handle(),  auto_ping(),
   * snd_on_ev_auto_ping_now_timer_fired() itself).  Each of these calls indicates a send occurs, hence
   * at worst the pipe will be idle (in need of auto-ping) in #m_snd_auto_ping_period.  Note that
   * `*end_sending()`, while also sending bytes, does not schedule #m_snd_auto_ping_timer, as `*end_sending()`
   * closes the conceptual pipe, and there is no need for auto-pinging (see Native_handle_receiver::idle_timer_run()).
   *
   * Since we implement `sync_io` pattern, the timer is obtained from, and used via, util::sync_io::Timer_event_emitter
   * #m_timer_worker.  See that member's doc header for more info.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  flow::util::Timer m_snd_auto_ping_timer;

  /**
   * Read-end of IPC-mechanism used by #m_timer_worker to ferry timer-fired events from #m_snd_auto_ping_timer
   * to `sync_io` outside async-wait to our actual on-timer-fired handler logic.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Timer_event_emitter::Timer_fired_read_end* m_snd_auto_ping_timer_fired_peer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_snd_auto_ping_timer_fired_peer.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Asio_waitable_native_handle m_snd_ev_wait_hndl_auto_ping_timer_fired_peer;

  /**
   * Function (set forever in `start_send_*_ops()`) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to send-ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_snd_ev_wait_func;

  // Incoming-direction data.

  /**
   * Null if no `async_receive_*()` is currently pending; else describes the arguments to that pending
   * `async_receive_*()`.
   *
   * Relevant only once terminal State::S_PEER is reached.
   *
   * ### Rationale ###
   * It exists for a hopefully obvious reasons: At least a non-immediately-completed `async_receive_*()` needs
   * to keep track of the request so as to know where to place results and what completion handler to invoke.
   *
   * As for it being nullable: this is used to guard against `async_receive_*()` being invoked while another
   * is already outstanding.  We do not queue pending requests per
   * sync_io::Blob_receiver / sync_io::Native_handle_receiver concept.  (However the non-`sync_io` a/k/a async-I/O
   * Blob_receiver + Native_handle_receiver transport::Native_socket_stream does.  Therefore
   * the latter class does internally implement a `User_request` queue.  Rather sync_io::Async_adapter_receiver
   * does.)
   */
  std::optional<Rcv_user_request> m_rcv_user_request;

  /**
   * Direct-write target, storing the length in bytes of the next meta-blob; 0 meaning the
   * current user message contains no meta-blob but only a native handle (or represents graceful-close if that
   * is also not present); and 0xFF... (#S_META_BLOB_LENGTH_PING_SENTINEL) meaning it's a mere ping.
   *
   * This plus a `Native_handle` is payload 1 received for each message; if, after successful read,
   * this is neither 0 nor #S_META_BLOB_LENGTH_PING_SENTINEL, then payload 2 -- the non-zero-sized
   * meta-blob -- is also read directly into the location specified by `m_rcv_user_request->m_target_meta_blob`.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  low_lvl_payload_blob_length_t m_rcv_target_meta_length;

  /**
   * The first and only connection-hosing error condition detected when attempting to low-level-read on
   * #m_peer_socket; or falsy if no such error has yet been detected.  Among possible other uses, it is emitted
   * to the ongoing-at-the-time `async_receive_*()`'s completion handler (if one is indeed outstanding)
   * and immediately to any subsequent `async_receive_*()`.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  Error_code m_rcv_pending_err_code;

  /**
   * `timeout` from idle_timer_run() args; or `zero()` if not yet called.  #m_rcv_idle_timer stays inactive
   * until this becomes not-`zero()`.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  util::Fine_duration m_rcv_idle_timeout;

  /**
   * Timer that fires rcv_on_ev_idle_timer_fired() (which hoses the in-pipe with idle timeour error) and is
   * (re)scheduled to fire in #m_rcv_idle_timeout each time `*this` receives a complete message
   * on #m_peer_socket.  If it does fire, without being preempted by some error to have occurred since then,
   * the in-pipe is hosed with a particular error indicating idle-timeout (so that `Error_code` is saved
   * to #m_rcv_pending_err_code), while the out-pipe continues (#m_peer_socket lives).
   *
   * Since we implement `sync_io` pattern, the timer is obtained from, and used via, util::sync_io::Timer_event_emitter
   * #m_timer_worker.  See that member's doc header for more info.
   *
   * Relevant only once terminal State::S_PEER is reached.
   */
  flow::util::Timer m_rcv_idle_timer;

  /**
   * Read-end of IPC-mechanism used by #m_timer_worker to ferry timer-fired events from #m_rcv_idle_timer
   * to `sync_io` outside async-wait to our actual on-timer-fired handler logic.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Timer_event_emitter::Timer_fired_read_end* m_rcv_idle_timer_fired_peer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_rcv_idle_timer_fired_peer.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Asio_waitable_native_handle m_rcv_ev_wait_hndl_idle_timer_fired_peer;

  /**
   * Function (set forever in `start_receive_*_ops()`) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to receive-ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_rcv_ev_wait_func;
}; // class Native_socket_stream::Impl

// Template implementations.

template<Native_socket_stream::Impl::Op OP>
util::sync_io::Event_wait_func* Native_socket_stream::Impl::sync_io_ev_wait_func()
{
  if constexpr(OP == Op::S_SND)
  {
    return &m_snd_ev_wait_func;
  }
  else if constexpr(OP == Op::S_RCV)
  {
    return &m_rcv_ev_wait_func;
  }
  else
  {
    static_assert(OP == Op::S_CONN, "What the....");
    return &m_conn_ev_wait_func;
  }
} // Native_socket_stream::Impl::sync_io_ev_wait_func()

template<Native_socket_stream::Impl::Op OP>
const util::sync_io::Event_wait_func* Native_socket_stream::Impl::sync_io_ev_wait_func() const
{
  return const_cast<Impl*>(this)->sync_io_ev_wait_func<OP>();
}

template<Native_socket_stream::Impl::Op OP>
bool Native_socket_stream::Impl::op_started(util::String_view context) const
{
  if (sync_io_ev_wait_func<OP>()->empty())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Op-type [" << int(OP) << "]: "
                     "In context [" << context << "] we must be start_...()ed, "
                     "but we are not.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else
  return true;
}

template<Native_socket_stream::Impl::Op OP>
bool Native_socket_stream::Impl::start_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  const auto ev_wait_func_ptr = sync_io_ev_wait_func<OP>();

  if (!ev_wait_func_ptr->empty())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Op-type [" << int(OP) << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  if constexpr(OP == Op::S_CONN)
  {
    if (m_state == State::S_PEER)
    {
      FLOW_LOG_WARNING("Socket stream [" << *this << "]: Start-connect-ops requested, but we are already (and "
                       "irreversibly) in PEER state.  Ignoring.");
      return false;
    }
    // else
    assert((m_state == State::S_NULL)
           && "Should not be able to get to CONNECTING state without start_connect_ops() in the first place.");
  }

  *ev_wait_func_ptr = std::move(ev_wait_func);

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Op-type [" << int(OP) << "]: Start-ops requested.  Done.");
  return true;
} // Native_socket_stream::Impl::start_ops()

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

} // namespace ipc::transport::sync_io
