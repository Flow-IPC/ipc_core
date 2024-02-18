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

#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/transport/detail/blob_stream_mq_impl.hpp"
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include "ipc/util/sync_io/detail/timer_ev_emitter.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/blob.hpp>
#include <flow/error/error.hpp>
#include <boost/move/make_unique.hpp>
#include <queue>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Internal, non-movable pImpl-lite implementation of sync_io::Blob_stream_mq_sender class template.
 * In and of itself it would have been directly and publicly usable; however Blob_stream_mq_sender adds move semantics
 * which are essential to cooperation with the rest of the API, Channel in particular.
 *
 * @see All discussion of the public API is in Blob_stream_mq_sender doc header; that class template forwards to this
 *      one.  All discussion of pImpl-lite-related notions is also there.  See that doc header first please.  Then come
 *      back here.
 *
 * ### Impl design ###
 * Here's the thing.  All the `_sender` and `_receiver` stuff historically was written first in
 * what is now sync_io::Native_socket_stream::Impl.  Next, this MQ stuff was written to implement (basically) the same
 * concepts.  So everything is explained, first, in sync_io::Native_socket_stream::Impl.  Next I (ygoldfel)
 * wrote each of `*this` and Blob_stream_mq_receiver_impl.  Therefore:
 *   - `*this` impl is similar to the outgoing-direction, PEER-state logic in that socket-stream `Impl` class.
 *   - And accordingly for incoming-direction and `Blob_stream_mq_receiver_impl`.
 *
 * Therefore we will only explain here the differences between that socket-stream out-pipe algorithms and `*this` impl.
 * Before going into it more consider these points:
 *   - Socket-stream has to worry about both directions *and* (some) interaction between the two (even though it
 *     is full-duplex).  We do not.  There are mutually concurrency issues to speak of in particular.
 *   - Socket-stream has to worry about going from NULL state to PEER state via CONNECTING (optionally) --
 *     `async_connect()`.  We do not: the `_impl` is always in PEER state (unless ctor failed, but that's not
 *     interesting).
 *   - Socket-stream must be able to transmit `Native_handle`s, and when applicable they correspond in a tricky way
 *     to byte 1 of the containing send/receive call.  We do not transmit them -- just blobs.
 *   - Socket-stream is operating over a medium that lacks message boundaries; so it must worry about transmitting
 *     only part of a payload, etc.  We do not: a low-level message either goes through, or it does not (would-block).
 *     A user message is 1-1 with a low-level message; so we need not transmit length prefixes.  There are 2 exceptions:
 *     - An auto-ping is a special message.  We express it as an "escape" low-level message (0-length -- as a user
 *       message must not be 0-length) followed by an encoded enumeration value in the following message.
 *     - A graceful-close is a special message.  It is a similarly escaped enumeration value (2 low-level messages).
 *
 * I (ygoldfel) could find no way to reuse code between the 2 counterparts despite similarities.  Some comments
 * and some code *are* repeated, but they are also different enough to where it couldn't really be avoided.  That said:
 * if you have a reason to understand `Native_socket_stream::Impl` specifically first, then coming back here
 * afterward should be a piece of cake.
 *
 * If not, due to the simplified/reduced purview of a `*this`, it should be quite doable to follow the logic.
 * I won't pontificate with some overview beyond the above.  Simply grok the following simple protocol, then
 * the `sync_io` pattern (util::sync_io doc header) -- and then jump into the code.
 *
 * (For that last part, we recommend starting with the data member doc headers.)
 *
 * So, the protocol:
 *
 * Two states: steady state, CONTROL state.  In steady state (default) each user message corresponds to a message
 * over the MQ.  By the concept, these cannot be empty.  When it is necessary to send an auto-ping (auto_ping() and
 * then periodically as needed), we enter CONTROL state, sending an empty message, then `Control_cmd::S_PING` in
 * the next message, which enters steady state again.  Lastly, `*end_sending()` means we enter CONTROL state
 * similarly, then send `Control_cmd::S_END_SENDING` in the next message, which enters steady state again.
 *
 * ### Protocol negotiation ###
 * This adds a bit of stuff onto the above protocol.  It is very simple; please see Protocol_negotiator doc header;
 * we use that convention.  Moreover, since this is the init version (version 1) of the protocol, we need not worry
 * about speaking more than one version.  So all we do is send the version just-ahead of the first payload that would
 * otherwise be sent for any reason (user message from send_blob(), end-sending token from `*end_sending()`, or
 * ping from auto_ping(), as of this writing), as a special CONTROL message that encodes the value `-1`, meaning
 * version 1 (as the true `Control_cmd enum` values would be 0, 1, etc.).  Conversely Blob_stream_mq_receiver_impl
 * expects the first message it does receive to be that CONTROL message encoding a version number; but that's not
 * our concern here, as we are only the outgoing-direction class.
 *
 * After that, we just speak what we speak... which is the protocol's initial version -- as there is no other
 * version for us.  (The opposing-side peer is responsible for closing the MQs, if it is unable to speak version 1.)
 *
 * If we do add protocol version 2, etc., in the future, then things *might* become somewhat more complex (but even
 * then not necessarily so).  This is discussed in the `Protocol_negotiator m_protocol_negotiator` member doc header.
 *
 * @note We suggest that if version 2, etc., is/are added, then the above notes be kept more or less intact; then
 *       add updates to show changes.  This will provide a good overview of how the protocol evolved; and our
 *       backwards-compatibility story (even if we decide to have no backwards-compatibility).
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender_impl :
  public Blob_stream_mq_base_impl<Persistent_mq_handle>,
  public flow::log::Log_context,
  private boost::noncopyable // And non-movable.
{
public:
  // Types.

  /// Short-hand for our base with `static` goodies at least.
  using Base = Blob_stream_mq_base_impl<Persistent_mq_handle>;

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = typename Base::Mq;

  // Constructors/destructor.

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * @param logger_ptr
   *        See Blob_stream_mq_sender counterpart.
   * @param mq_moved
   *        See Blob_stream_mq_sender counterpart.
   * @param nickname_str
   *        See Blob_stream_mq_sender counterpart.
   * @param err_code
   *        See Blob_stream_mq_sender counterpart.
   */
  explicit Blob_stream_mq_sender_impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                      Mq&& mq_moved, Error_code* err_code);

  /// See Blob_stream_mq_sender counterpart.
  ~Blob_stream_mq_sender_impl();

  // Methods.

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See Blob_stream_mq_sender counterpart.
   * @param create_ev_wait_hndl_func
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  size_t send_blob_max_size() const;

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * @param ev_wait_func
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool start_send_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param blob
   *        See Blob_stream_mq_sender counterpart.
   * @param err_code
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param sync_err_code
   *        See Blob_stream_mq_sender counterpart.
   * @param on_done_func
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool end_sending();

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param period
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool auto_ping(util::Fine_duration period);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   * @return See Blob_stream_mq_sender counterpart.
   */
  const std::string& nickname() const;

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  const Shared_name& absolute_name() const;

private:
  // Types.

  /**
   * Data store representing a payload corresponding to exactly one attempted async write-op, albeit
   * used if and only if we encounter would-block in `send_blob()` or on_ev_auto_ping_now_timer_fired()
   * or `*end_sending()` and have to queue (and therefore one-time-copy) data in #m_pending_payloads_q.
   *
   * Note this does *not* represent an original user message given to Native_socket_stream::send_blob()
   * but rather an actual single low-level message to send over #m_mq.  (Spoiler alert: most low-level payloads
   * are indeed simply user messages; but each of graceful-close and auto-ping is represented by
   * an "escape" payload -- empty since empty user messages are disallowed -- followed by an enumeration-storing
   * payload specifying which "control" message it is.  Each of these is termed a CONTROL message.)
   *
   * We store them in queue via #Ptr.
   */
  struct Snd_low_lvl_payload
  {
    // Types.

    /// Short-hand for `unique_ptr` to this.
    using Ptr = boost::movelib::unique_ptr<Snd_low_lvl_payload>;

    // Data.

    /**
     * The buffer to transmit in the payload; possibly `.empty()`.  Note that this is the actual buffer
     * and not a mere location/size of an existing blob somewhere.
     * ### Rationale ###
     * Why not just have `using Snd_low_lvl_payload = unique_ptr<Blob>`?  Answer: No huge reason.  Just
     * consistency with sync_io::Native_socket_stream::Impl which has a quite similar outgoing-direction
     * algorithm and therefore type but with a more complex payload (they've got `Native_handle`s to worry about).
     */
    flow::util::Blob m_blob;
  }; // struct Snd_low_lvl_payload

  /// Short-hand for Blob_stream_mq_base_impl::Control_cmd.
  using Control_cmd = typename Base::Control_cmd;

  // Methods.

  /**
   * Helper that returns `true` silently if `start_*_ops()` has been called; else
   * logs WARNING and returns `false`.
   *
   * @param context
   *        For logging: the algorithmic context (function name or whatever).
   * @return See above.
   */
  bool op_started(util::String_view context) const;

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
  void on_ev_auto_ping_now_timer_fired();

  /**
   * Either synchronously sends `orig_blob` low-level blob over #m_mq,
   * or if an async-send is in progress queues it to be sent later; dropping-sans-queuing
   * allowed under certain circumstances in `avoided_qing` mode.  For details on the latter see below.
   *
   * #m_pending_err_code (pre-condition: it is falsy) is set to the error to ultimately return; and if no such
   * outgoing-pipe-hosing is synchronously encountered it is left untouched.  In particular, if falsy upon return,
   * you may call this again to send the next low-level payload.  Otherwise #m_mq cannot be subsequently used
   * (it is hosed).  Thus we maintain the invariant that #m_mq is null if and only if that guy is truthy.
   *
   * ### `avoided_qing` mode for auto-ping ###
   * If `avoided_qing_or_null` is null, then see above.  If it points to a `bool`, though, then:
   * the above behavior is slightly modified as follows.  Suppose `orig_blob` encodes an
   * auto-ping message (see auto_ping()).  Its purpose is to inform the
   * opposing side that we are alive/not idle.  So suppose this method is unable to send `orig_blob`,
   * either because there are already-queued data waiting to be sent pending writability (due to earlier would-block),
   * or because the MQ has already-queued bytes waiting to be popped by receiver, and there is no
   * space there to enqueue `orig_blob`.  Then the receiver must not be keeping up with us, and the next
   * pop of the MQ will get *some* message, even if it's not the auto-ping we wanted to send;
   * hence they'll know we are not-idle without the auto-ping.  So in that case this method shall:
   *   - disregard `orig_blob` (do not queue it -- drop it, as it would be redundant anyway);
   *   - return `true` if and only if the size of the out-queue is 0 (though as of this writing the caller should
   *     not care: auto-ping is a fire-and-forget operation, as long as it does not detect a pipe-hosing error);
   *   - set `*avoided_qing_or_null`.
   *
   * If `avoided_qing_or_null` is not null, but no error or would-block was encountered -- meaning it was
   * placed into MQ synchronously -- then `*avoided_qing_or_null` is set to `false`.
   *
   * Why do we even have this as an out-arg?  Answer: It is to be used for payload 1 of a CONTROL PING message;
   * so if this is set to `true`, then caller should *not* sync_write_or_q_payload() again for payload 2.
   * Otherwise (assuming #m_mq is still alive) it should.
   *
   * @param orig_blob
   *        Blob to send.  Empty blob is allowed.
   * @param avoided_qing_or_null
   *        See above.
   * @return `false` if outgoing-direction pipe still has queued stuff in it that must be sent once transport
   *         becomes writable; `true` otherwise.  If `true` is returned, but `avoided_qing_or_null != nullptr`, then
   *         possibly `orig_blob` was not sent (at all); was dropped (check `*avoided_qing_or_null` to see if so).
   */
  bool sync_write_or_q_payload(const util::Blob_const& orig_blob, bool* avoided_qing_or_null);

  /**
   * Writes the 2 payloads corresponding to CONTROL command `cmd` to #m_mq; if unable to do so synchronously --
   * either because items are already queued in #m_pending_payloads_q, or MQ is full -- then enqueue 1-2 of the
   * payloads onto that queue.  In the case of Control_cmd::S_PING it may drop both payloads silently
   * (see sync_write_or_q_payload() notes regarding `avoided_qing` mode).
   *
   * Essentially this combines 1-2 sync_write_or_q_payload() for the specific task of sending out a CONTROL command.
   * If one desires to send a normal (user) message -- use sync_write_or_q_payload() directly (it shall deal
   * with a single payload).
   *
   * If a sync-write uncovers #m_mq is hosed, then #m_pending_err_code is made truthy.
   *
   * @param cmd
   *        See above.
   * @return A-la sync_write_or_q_payload(): `false` if outgoing-direction pipe still has stuff in it; `true`
   *         otherwise.
   */
  bool sync_write_or_q_ctl_cmd(Control_cmd cmd);

  /**
   * Equivalent to sync_write_or_q_ctl_cmd() but takes the raw representation of the command to send;
   * this allows both sending the `enum Control_cmd` value or the special protocol negotiation payload.
   * We always send the latter as the first message, and never again; and the receiver side expects this;
   * after that only regular `Control_cmd`-bearing CONTROL messages are sent and expected.
   *
   * @param raw_cmd
   *        Either a non-negative value, which equals the cast of a `Control_cmd` to its underlying type;
   *        or a negative value, which equals the arithmetic negation of our highest (preferred) protocol
   *        version.  So in the latter case -1 means version 1, -2 means version 2, etc.
   * @return See sync_write_or_q_ctl_cmd().
   */
  bool sync_write_or_q_ctl_cmd_impl(std::underlying_type_t<Control_cmd> raw_cmd);

  /**
   * Initiates async-write over #m_mq of the low-level payload at the head of out-queue
   * #m_pending_payloads_q, with completion handler also inside this method.
   * The first step of this is an async-wait via `sync_io` pattern.
   */
  void async_write_q_head_payload();

  // Data.

  /// See nickname().
  const std::string m_nickname;

  /**
   * Handles the protocol negotiation at the start of the pipe.
   *
   * @see Protocol_negotiator doc header for key background on the topic.  In particular check out the discussion
   *      "Key tip: Coding for version-1 versus one version versus multiple versions."
   *
   * ### Maintenace/future ###
   * This version of the software talks only the initial version of the protocol: version 1 by Protocol_negotiator
   * convention.  (Deities willing, we won't need to create more, but possibly we will.)  As expained in the
   * above-mentioned doc header section, we have very little to worry about as the sender: Just send
   * our version, 1, as the first message (in fact, a special CONTROL message; and we send it just ahead of the
   * first payload that would be otherwise sent, whether it's a user payload, or the end-sending token, or
   * an auto-ping).  Only version 1 exists, so we simply speak it.
   *
   * This *might* change in the future.  If there is a version 2 or later, *and* at some point we decide to maintain
   * backwards-compatibility (not a no-brainer decision in either direction), meaning we pass a range of 2 versions
   * or more to Protocol_negotiator ctor, then:
   *   - We will need to know which version to speak when sending at least *some* of our messages.
   *   - Normally we'd get this via a separate incoming pipe -- a paired Blob_stream_mq_receiver handling
   *     a separate associated MQ.  So we'd need:
   *     - some kind of internal logic to enter a would-block state of sorts (if we need to know the version
   *       for a certain out-message, and we haven't been told it yet) during which stuff is queued-up and not
   *       yet sent until signaled with the negotiated protocol version;
   *     - some kind of API that would let the paired Blob_stream_mq_receiver signal us to tell us that version,
   *       when it knows;
   *     - possibly some kind of API that one could use in the absence of such a paired opposite-facing pipe,
   *       so that our user can simply tell us what protocol version to speak in lieu of the paired
   *       Blob_stream_mq_receiver.
   *
   * (Just to point a fine point on it: We are *one* direction of a *likely* bidirectional comm pathway.
   * Protocol_negotiator by definition applies to a bidirectional pathway.  Yet we *can* exist in isolation, with
   * no partner opposing MQ and therefore no partner local Blob_stream_mq_sender_impl.  If that is indeed the
   * case, then it is meaningless for *us* to somehow find out what protocol the either side speaks, as the other
   * side has no way of sending us *anything*!  Hence the dichotomy between those last 2 sub-bullets:
   * If we exist in isolation and are unidirectional, then you'd best simply tell us what to speak... but
   * if we're part of a bidirectional setup, then we can cooperate with the other direction more automagically.
   *
   * These are decisions and work for another day, though; it is not relevant until version 2 of this protocol
   * at the earliest.  That might not even happen.
   */
  Protocol_negotiator m_protocol_negotiator;

  /// See absolute_name().
  const Shared_name m_absolute_name;

  /**
   * The `Task_engine` for `m_mq_ready_*`.  It is necessary to construct those pipe-end objects, but we never
   * use that guy's `->async_*()` APIs -- only non-blocking operations, essentially leveraging boost.asio's
   * portable transmission APIs but not its actual, um, async-I/O abilities in this case.  Accordingly we
   * never load any tasks onto #m_nb_task_engine and certainly never `.run()` (or `.poll()` or ...) it.
   *
   * In the `sync_io` pattern the user's outside event loop is responsible for awaiting readability/writability
   * of a guy like #m_mq_ready_reader via our exporting of its `.native_handle()`.
   *
   * Never touched if Persistent_mq_handle::S_HAS_NATIVE_HANDLE.
   *
   * @todo Consider using specialization instead of or in addition `if constexpr()` w/r/t
   * `Mq::S_HAS_NATIVE_HANDLE`-based compile-time branching: it could save some RAM by eliminating `optional`s
   * such as the one near this to-do; though code would likely become wordier.
   */
  std::optional<flow::util::Task_engine> m_nb_task_engine;

  /**
   * The `Task_engine` for `m_ev_wait_hndl_*`, unless it is replaced via replace_event_wait_handles().
   * There are 2 possibilities:
   *   - They leave this guy associated with `m_ev_wait_hndl_*`.  Then no one shall be doing `.async_wait()` on
   *     them, and instead the user aims to perhaps use raw `[e]poll*()` on their `.native_handle()`s.
   *     We still need some `Task_engine` to construct them though, so we use this.
   *   - They use replace_event_wait_handles(), and therefore this becomes dissociated with `m_ev_wait_hndl_*` and
   *     becomes completely unused in any fashion, period.  Then they shall probably be doing
   *     their own `.async_wait()` -- associated with their own `Task_engine` -- on `m_ev_wait_hndl_*`.
   *
   * This is all to fulfill the `sync_io` pattern.
   */
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /**
   * The MQ handle adopted by the ctor, through which nb-sends and blocking-waits are executed.
   * It is not directly protected by any mutex; however it is accessed exclusively as follows:
   *   - By thread W, when #m_pending_payloads_q has elements (would-block state), thus thread U has tasked
   *     it with awaiting sendability -- which it is doing and is not yet done doing.
   *   - By thread U, when #m_pending_payloads_q is `.empty()` (no would-block at the moment); or
   *     it is not .empty(), but thread W has signaled it is done awaiting non-would-block, so thread U
   *     is trying to empty it via nb-writes.
   *
   * Exception: `m_mq->interrupt_sends()` is done from thread U (dtor), potentially *while* thread W
   * is inside `m_mq->wait_sendable()`.  However that specific action is allowed by Persistent_mq_handle contract
   * (in fact that is the main utility of Persistent_mq_handle::interrupt_sends()).  It makes the
   * Persistent_mq_handle::wait_sendable() immediately return.
   *
   * If and only if #m_pending_err_code is truthy, #m_mq is null; hence the handle's system resources (and the
   * handle object's memory itself) are given back at the earliest possible time.
   */
  typename Base::Auto_closing_mq m_mq;

  /**
   * Equals `m_mq.max_msg_size()`.
   * ### Rationale ###
   * It is saved in ctor, instead of invoking `m_mq->max_msg_size()` when needed, to keep the rules for when #m_mq
   * is accessed simple and as described in its doc header.  After all this value would never change anyway.
   */
  const size_t m_mq_max_msg_sz;

  /**
   * Read-end of IPC-pipe used by thread U to detect that a thread-W transmissibility-wait has completed,
   * meaning would-block has cleared, and therefore thread U can attempt to nb-transmit again.  The signal byte
   * is read out of #m_mq_ready_reader, making it empty again (the steady-state before the next time would-block
   * occurs, and a byte is written to it making it non-empty).
   *
   * @see #m_mq_ready_writer.
   *
   * Never touched if Persistent_mq_handle::S_HAS_NATIVE_HANDLE.
   */
  std::optional<util::Pipe_reader> m_mq_ready_reader;

  /**
   * Write-end of IPC-pipe used by thread W to inform thread U that a thread-W transmissibility-wait has completed,
   * meaning would-block has cleared, and therefore thread U can attempt to nb-transmit again.  A signal byte
   * is written to #m_mq_ready_writer, making it non-empty.
   *
   * @see #m_mq_ready_reader.
   *
   * Never touched if Persistent_mq_handle::S_HAS_NATIVE_HANDLE.
   */
  std::optional<util::Pipe_writer> m_mq_ready_writer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_mq_ready_reader; or if #m_mq can be watched directly (known at compile-time)
   * then as `m_mq` itself.  Its name suggests it's watching #m_mq (for transmissibility),
   * and that's true: In the latter case one directly watches it for transmissibility in the proper direction;
   * in the former case Persistent_mq_handle lacks a #Native_handle to watch directly -- but we use the
   * `m_mq_ready_*` pipe and a background thread to simulate it having one.  Thus in that case #m_mq_ready_reader FD =
   * this FD, and it being transmissible = MQ being transmissible.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_mq;

  /**
   * As typical in timer-needing `sync_io`-pattern-implementing objects, maintains a thread exclusively for
   * `Timer` wait completion handlers which ferry timer-fired events to internal IPC-mechanisms waitable
   * by the `sync_io`-pattern-using outside event loop.  In our case we (optionally) maintain the auto-ping timer.
   *
   * @see util::sync_io::Timer_event_emitter doc header for design/rationale discussion.
   */
  util::sync_io::Timer_event_emitter m_timer_worker;

  /**
   * Queue storing (at head) the currently in-progress async write-op of a Snd_low_lvl_payload; followed by
   * the payloads that should be written after that completes, in order.
   *
   * Only touched if would-block is encountered in... well, see Snd_low_lvl_payload doc header (et al).
   */
  std::queue<typename Snd_low_lvl_payload::Ptr> m_pending_payloads_q;

  /**
   * The first and only connection-hosing error condition detected when attempting to low-level-write on
   * #m_mq; or falsy if no such error has yet been detected.  Among possible other uses, it is returned
   * by send_blob() and the completion handler of async_end_sending().
   */
  Error_code m_pending_err_code;

  /**
   * `false` at start; set to `true` forever on the first `*end_sending()` invocation;
   * `true` will prevent any subsequent send_blob() calls from proceeding.
   * See class doc header impl section for design discussion.
   */
  bool m_finished;

  /**
   * Function passed to async_end_sending(), if it returned `true` and was unable to synchronously flush everything
   * including the graceful-close itself (synchronously detecting new or previous pipe-hosing error *does* entail
   * flushing everything); otherwise `.empty()`.  It's the completion handler of that graceful-close-send API.
   */
  flow::async::Task_asio_err m_pending_on_last_send_done_func_or_empty;

  /**
   * Equals `zero()` before auto_ping(); immutably equals `period` (auto_ping() arg) subsequently to that first
   * successful call (if any).
   */
  util::Fine_duration m_auto_ping_period;

  /**
   * Timer that fires on_ev_auto_ping_now_timer_fired() (which sends an auto-ping) and is always
   * scheduled to fire #m_auto_ping_period after the last send (send_blob(),  auto_ping(),
   * on_ev_auto_ping_now_timer_fired() itself).  Each of these calls indicates a send occurs, hence
   * at worst the pipe will be idle (in need of auto-ping) in #m_auto_ping_period.  Note that
   * `*end_sending()`, while also sending bytes, does not schedule #m_auto_ping_timer, as `*end_sending()`
   * closes the conceptual pipe, and there is no need for auto-pinging (see Blob_receiver::idle_timer_run()).
   *
   * Since we implement `sync_io` pattern, the timer is obtained from, and used via, util::sync_io::Timer_event_emitter
   * #m_timer_worker.  See that member's doc header for more info.
   */
  flow::util::Timer m_auto_ping_timer;

  /**
   * Read-end of IPC-mechanism used by #m_timer_worker to ferry timer-fired events from #m_auto_ping_timer
   * to `sync_io` outside async-wait to our actual on-timer-fired handler logic.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Timer_event_emitter::Timer_fired_read_end* m_auto_ping_timer_fired_peer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_auto_ping_timer_fired_peer.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_auto_ping_timer_fired_peer;

  /**
   * Function (set forever in start_send_blob_ops()) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events to our ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_ev_wait_func;

  /**
   * Worker thread W always in one of 2 states: idle; or (when #m_mq is in would-block condition) executing an
   * indefinite, interrupting blocking wait for transmissibility of #m_mq.  When thread U wants to send-out
   * a payload but gets would-block, it issues the wait on this thread W and a `sync_io`-pattern async-wait
   * for #m_ev_wait_hndl_mq; once that wait completes in thread W, it writes a byte to an internal IPC-pipe.
   * #m_ev_wait_hndl_mq becomes readable, the outside event loop lets `*this` know, which completes the async-wait.
   *
   * In dtor we stop thread W, including using Persistent_mq_handle::interrupt_sends() to abort the indefinite
   * wait in thread W, as it will no longer be used once `*this` is destroyed.
   *
   * Ordering: If we want to let things get auto-destroyed without explicit `m_blocking_worker->stop()` or
   * nullifying wrappers in an explicit order, then this must be declared after #m_mq.  Otherwise code may still be
   * finishing up in thread W when #m_mq is destroyed already.  Anyway -- as long as this is destroyed or `.stop()`ed
   * before #m_mq is gone, you're cool.
   *
   * Never touched if Persistent_mq_handle::S_HAS_NATIVE_HANDLE.
   */
  std::optional<flow::async::Single_thread_task_loop> m_blocking_worker;
}; // class Blob_stream_mq_sender

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Persistent_mq_handle>
Blob_stream_mq_sender_impl<Persistent_mq_handle>::Blob_stream_mq_sender_impl
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq,
   Error_code* err_code) :

  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_nickname(nickname_str),
  m_protocol_negotiator(get_logger(), nickname(),
                        1, 1), // Initial protocol!  @todo Magic-number `const`(s), particularly if/when v2 exists.
  m_absolute_name(mq.absolute_name()),
  // m_mq null for now but is set up below.
  m_mq_max_msg_sz(mq.max_msg_size()), // Just grab this now though.
  m_ev_wait_hndl_mq(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_timer_worker(get_logger(), flow::util::ostream_op_string(*this)),
  m_finished(false),
  m_auto_ping_period(util::Fine_duration::zero()), // auto_ping() not yet called.
  m_auto_ping_timer(m_timer_worker.create_timer()), // Inactive timer (auto_ping() not yet called).
  /* If it does become active, we'll use this readable-pipe-peer to get informed by m_timer_worker that
   * m_auto_ping_timer has fired.  That way we can use m_ev_wait_func() mechanism to have user
   * ferry timer firings to us. */
  m_auto_ping_timer_fired_peer(m_timer_worker.create_timer_signal_pipe()),
  // And this is its watchee mirror for outside event loop (sync_io pattern).
  m_ev_wait_hndl_auto_ping_timer_fired_peer
    (m_ev_hndl_task_engine_unused,
     Native_handle(m_auto_ping_timer_fired_peer->native_handle()))
{
  using flow::error::Runtime_error;
  using flow::util::ostream_op_string;
  using boost::asio::connect_pipe;

  auto& sys_err_code = m_pending_err_code;

  /* Read class doc header (particularly regarding lifetimes of things); then return here.  Summarizing:
   *
   * We've promised to enforce certain semantics, namely that a given MQ can be accessed, ever, by
   * at most 1 Blob_stream_mq_sender (and 1 _receiver) -- across all processes, not just this one.
   * All that is involved in setting up m_mq (which is null at the moment) and related/derived items
   * mentioned as not-yet-set-up above. */
  m_mq = Base::ensure_unique_peer(get_logger(),
                                  std::move(mq), true /* sender */, &sys_err_code); // Does not throw.
  assert(bool(m_mq) == (!sys_err_code));
  // On error *err_code will be truthy, and it will have returned null.

  /* Home free w/r/t m_mq.  No longer need to worry about anything but *this and other-side single _receiver.
   * Not even dtor has to worry (due to that deleter up there). */

  if (!sys_err_code)
  {
    /* So that leaves the setup of, ultimately, m_ev_wait_hndl_mq -- which outside event loop, via sync_io pattern,
     * will async-wait-on for us to detect m_mq transmissibility.  As noted in some data member doc headers, et al,
     * we can't watch m_mq through an FD directly; but we can simulate it by running the waits in thread W
     * (which is why it exists) and then simulate the active FD using a pipe whose read end m_mq_ready_reader
     * exports the sync_io-watched FD.
     *
     * However!  Delightfully if the particular Mq has Mq::native_handle(), then we *can* watch m_mq
     * through an FD.  This is known at compile-time, so in that case we can skip the above stuff entirely. */
    if constexpr(Mq::S_HAS_NATIVE_HANDLE)
    {
      // Load up the FD into the watchee mirror.  See also replace_event_wait_handles().
      m_ev_wait_hndl_mq.assign(m_mq->native_handle());
    } // if constexpr(Mq::S_HAS_NATIVE_HANDLE)
    else // if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
    {
      m_nb_task_engine.emplace();
      m_mq_ready_reader.emplace(*m_nb_task_engine); // No handle inside but will be set-up soon below.
      m_mq_ready_writer.emplace(*m_nb_task_engine); // Ditto.

      // Start thread W, for when we (and if) we need to use it to async-wait for transmissibility.
      m_blocking_worker.emplace(get_logger(),
                                ostream_op_string("mq_snd-", nickname())); // Thread W started just below.
      m_blocking_worker->start();

      // For now we just set up the IPC-pipe and the sync_io-watched FD mirror.  First things first... err, second....
      connect_pipe(*m_mq_ready_reader, *m_mq_ready_writer, sys_err_code);
      if (sys_err_code)
      {
        FLOW_LOG_WARNING
          ("Blob_stream_mq_sender [" << *this << "]: Constructing: connect-pipe failed.  Details follow.");
        FLOW_ERROR_SYS_ERROR_LOG_WARNING();

        m_mq.reset(); // Undo this.
      }
      else
      {
        // Lastly load up the read-end FD into the watchee mirror.  See also replace_event_wait_handles().
        m_ev_wait_hndl_mq.assign(Native_handle(m_mq_ready_reader->native_handle()));
      }
    } // if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
  } // if (!sys_err_code) (but it may have become truthy inside)

  assert(bool(m_mq) == (!sys_err_code));

  if (!m_mq)
  {
    // `mq` is untouched.  m_pending_err_code will cause that to be emitted on send_blob() and such.
    assert(sys_err_code);

    if (err_code)
    {
      *err_code = sys_err_code;
      return;
    }
    // else
    throw Runtime_error(sys_err_code, "Blob_stream_mq_sender(): ensure_unique_peer()");
  }
  // else: took over `mq` ownership.
  assert(!sys_err_code);

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: MQ-handle-watching apparatus ready including running "
                "blocking worker thread for would-block situations if necessary "
                "(is it? = [" << (!Mq::S_HAS_NATIVE_HANDLE) << "]).");
} // Blob_stream_mq_sender_impl::Blob_stream_mq_sender()

template<typename Persistent_mq_handle>
Blob_stream_mq_sender_impl<Persistent_mq_handle>::~Blob_stream_mq_sender_impl()
{
  if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
  {
    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Shutting down.  "
                  "Async-wait worker thread and timer thread will shut down/be joined shortly; but "
                  "we must interrupt any currently-running async-wait to ensure "
                  "async-wait worker thread actually exits.");

    /* No-op if no send is pending; but it'd be boring worrying about checking m_pending_payloads_q.empty() just
     * to avoid a no-op.  It's harmless (not *exactly* a no-op, but m_mq is not long for this world -- so who cares
     * if it dies while in interrupted-sends mode or otherwise). */

    if (m_mq)
    {
      m_mq->interrupt_sends();
    }

    /* That's it: If there was a (blocking) task on m_blocking_worker, then it has or will have stopped soon;
     * the m_timer_worker thread, at worst, might harmlessly fire auto-ping timer and signal that pipe -- which
     * we won't check or do anything about.  Now m_blocking_worker and m_timer_worker dtors will run in opposite
     * order, ending/joining those threads, and then this dtor will return.  Oh, and then m_mq is safely destroyed
     * too. */
  } // if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
} // Blob_stream_mq_sender_impl::~Blob_stream_mq_sender_impl()

template<typename Persistent_mq_handle>
template<typename Create_ev_wait_hndl_func>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  if (m_pending_err_code)
  {
    /* Ctor failed (without throwing, meaning they used the non-null-err_code semantic).
     * It is tempting to `return false` here, but that complicates our contract and breaks the concept contract
     * for our method.  Actually, *this will work "fine": it'll report m_pending_err_code if you try any transmission;
     * it won't trigger any async-waits -- so the below not running does not matter.  So it is enough to WARN
     * but carry on. */
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Cannot replace event-wait handles as requested: "
                     "ctor failed earlier ([" << m_pending_err_code << "] [" << m_pending_err_code.message() << "].  "
                     "Any transmission attempts will fail in civilized fashion, so we will just no-op here.");
    return true;
  }
  // else

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Replacing event-wait handles (probably to replace "
                "underlying execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl_mq.is_open());
  assert(m_ev_wait_hndl_auto_ping_timer_fired_peer.is_open());

  Native_handle saved(m_ev_wait_hndl_mq.release());
  m_ev_wait_hndl_mq = create_ev_wait_hndl_func();
  m_ev_wait_hndl_mq.assign(saved);

  saved.m_native_handle = m_ev_wait_hndl_auto_ping_timer_fired_peer.release();
  m_ev_wait_hndl_auto_ping_timer_fired_peer = create_ev_wait_hndl_func();
  m_ev_wait_hndl_auto_ping_timer_fired_peer.assign(saved);

  return true;
} // Blob_stream_mq_sender_impl::replace_event_wait_handles()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::
       start_send_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  m_ev_wait_func = std::move(ev_wait_func);

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Start-ops requested.  Done.");
  return true;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::op_started(util::String_view context) const
{
  if (m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: "
                     "In context [" << context << "] we must be start_...()ed, "
                     "but we are not.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else
  return true;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;
  using util::Fine_duration;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Blob_stream_mq_sender_impl<Persistent_mq_handle>::send_blob,
                                     flow::util::bind_ns::cref(blob), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert((blob.size() != 0) && "Do not send_blob() empty blobs.");

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   *
   * There is a *lot* of similarity between this and sync_io::Native_socket_stream::Impl::send_native_handle().
   * In fact I (ygoldfel) wrote that guy's send stuff first, then I based this off it.
   * Yet there are enough differences to where code reuse isn't really obviously achievable.
   * @todo Look into it.  It would be nice to avoid 2x the maintenance and ~copy/pasted comments. */

  if (!op_started("send_blob()"))
  {
    err_code->clear();
    return false;
  }
  // else

  FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Will send blob of size [" << blob.size() << "].");
  // Verbose and slow (100% skipped unless log filter passes).
  FLOW_LOG_DATA("Blob_stream_mq_sender [" << *this << "]: Blob contents are "
                "[\n" << buffers_dump_string(blob, "  ") << "].");

  if (m_finished)
  {
    /* If they called *end_sending() before, then by definition (see doc header impl discussion)
     * any future send attempt is to be ignored with this error.  Even though previously queued stuff can and should
     * keep being sent, once that's done this clause will prevent any more from being initiated.
     *
     * Corner case: If those queued sends indeed exist (are ongoing) then user should have a way of knowing when it's
     * done.  That isn't the case for regular send_blob() calls which are silently queued up as-needed,
     * which is why I mention it here.  So that's why in *end_sending() there's a way for
     * user to be informed (via sync callback) when everything has been sent through, or if an error stops it
     * from happening.  None of our business here though: we just refuse to do anything and emit this error. */
    *err_code = error::Code::S_SENDS_FINISHED_CANNOT_SEND;
    // Note that this clause will always be reached subsequently also.
  }
  else
    /* Check for message size limit.  Just catch it immediately and bail out; and no harm (to pipe) done otherwise.
     *
     * This is a subtler decision than it might seem.  If we simply skipped this, things would still work:
     * An immediate try_send() would yield S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW error; and if the send had
     * to be queued, then a post-async-wait try_send() would yield that same thing later.
     * However that would hose the pipe, as it is a low-level error.  The Blob_sender concept allows this: it says
     * a blob size limit *may* be imposed and yield INVALID_ARGUMENT, which shall not hose the pipe.
     * It's "may" -- not "shall" -- so we're just being nice and usable.  The only caveat is we're kinda preempting
     * the MQ's low-level check by doing it manually ourselves, querying/caching MQ's max_msg_size().  Arguably not
     * the prettiest, but the effect is correct (at least with Posix_mq_handle and Bipc_mq_handle anyway).
     *
     * We could also catch S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW and transform it to INVALID_ARGUMENT... but if it
     * has to be queued, then things get complicated or inconsistent; it would be reported in a later
     * user API call.  So let's not.  This way the behavior is same as with Native_socket_stream; not required by
     * the concept but good nevertheless. */
    if (blob.size() > m_mq_max_msg_sz) // && (!m_finished)
  {
    *err_code = error::Code::S_INVALID_ARGUMENT;
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Send: "
                     "User argument length [" << blob.size() << "] exceeds limit [" << send_blob_max_size() << "].  "
                     "Emitting error immediately; but pipe continues.  Note that if we don't do this, "
                     "then the low-level MQ will behave this way anyway.");
  }
  else if (m_pending_err_code) // && (!m_finished) && (blob.size() OK)
  {
    /* This --^ holds either the last inline-completed send_blob() call's emitted Error_code, or (~rarely) one
     * that was found while attempting to dequeue previously-would-blocked queued-up (due to incomplete s_blob())
     * payload(s).  This may seem odd, but that's part of the design of the send interface which we wanted to be
     * as close to looking like a series of synchronous always-inline-completed send_blob() calls as humanly possible,
     * especially given that 99.9999% of the time that will indeed occur given proper behavior by the opposing
     * receiver. */

    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: An error was detected earlier and saved for any "
                  "subsequent send attempts like this.  Will not proceed with send.  More info in WARNING below.");
    *err_code = m_pending_err_code;
  }
  else // if (!m_finished) && (blob.size() OK) && (!m_pending_err_code)
  {
    /* As the name indicates this may synchronously finish it or queue it up instead to be done once
     * a would-block clears, when user informs of this past the present function's return.
     *
     * Send-or-queue the 1 message, as-is.  There's a very high chance it is done inline;
     * but there's a small chance either there's either stuff queued already (we've delegated waiting for would-block
     * to clear to user), or not but this can't be pushed (encountered would-block).  We don't care here per
     * se; I am just saying for context, to clarify what "send-or-queue" means. */
    sync_write_or_q_payload(blob, nullptr);
    /* That may have returned `true` indicating everything (up to and including our payload) was synchronously
     * given to kernel successfuly; or this will never occur, because outgoing-pipe-ending error was encountered.
     * Since this is send_blob(), we do not care: there is no on-done
     * callback to invoke, as m_finished is false, as *end_sending() has not been called yet. */

    /* No new error: Emit to user that this op did not emit error (m_pending_err_code is still falsy).
     * New error: Any subsequent attempt will emit this truthy value; do not forget to emit it now via *err_code. */
    *err_code = m_pending_err_code;

    // Did it generate a new error?
    if (*err_code)
    {
      FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Wanted to send user message but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << *err_code << "] [" << err_code->message() << "].  "
                     "Saved error code to return in next user send attempt if any, after this attempt also "
                     "returns that error code synchronously first.");
    }
    else if (m_auto_ping_period != Fine_duration::zero()) // && (!*err_code)
    {
      /* Send requested, and there was no error; that represents non-idleness.  If auto_ping() has been called
       * (the feature is engaged), idleness shall occur at worst in m_auto_ping_period; hence reschedule
       * on_ev_auto_ping_now_timer_fired(). */

      const size_t n_canceled = m_auto_ping_timer.expires_after(m_auto_ping_period);

      FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Send request from user; hence rescheduled "
                     "auto-ping to occur in "
                     "[" << round<milliseconds>(m_auto_ping_period) << "] (will re-reschedule "
                     "again upon any other outgoing traffic that might be requested before then).  As a result "
                     "[" << n_canceled << "] previously scheduled auto-pings have been canceled; 1 is most likely; "
                     "0 means an auto-ping is *just* about to fire (we lost the race -- which is fine).");
      if (n_canceled == 1)
      {
        /* m_timer_worker will m_auto_ping_timer.async_wait(F), where F() will signal through pipe,
         * making *m_auto_ping_timer_fired_peer readable.  We've already used m_ev_wait_func() to start
         * wait on it being readable and invoke on_ev_auto_ping_now_timer_fired() in that case; but we've
         * canceled the previous .async_wait() that would make it readable; so just redo that part. */
        m_timer_worker.timer_async_wait(&m_auto_ping_timer, m_auto_ping_timer_fired_peer);
      }
      else
      {
        assert((n_canceled == 0) && "We only invoke one timer async_wait() at a time.");

        /* Too late to cancel on_ev_auto_ping_now_timer_fired(), so it'll just schedule next one itself.
         * Note that in practice the effect is about the same. */
      }
    } // else if (m_auto_ping_period != zero) && (!*err_code)
    // else if (m_auto_ping_period == zero) && (!*err_code) { Auto-ping feature not engaged. }
  } /* else if (!m_finished) && (blob.size() OK) && (!m_pending_err_code)
     *         (but m_pending_err_code may have become truthy inside) */

  if (*err_code)
  {
    // At the end try to categorize nature of error.
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Wanted to send blob of size "
                     "[" << blob.size() << "], but an error (not necessarily new error) "
                     "encountered on pipe or in user API args.  Error code details follow: "
                     "[" << *err_code << "] [" << err_code->message() << "];  "
                     "pipe hosed (sys/protocol error)? = "
                     "[" << ((*err_code != error::Code::S_INVALID_ARGUMENT)
                             && (*err_code != error::Code::S_SENDS_FINISHED_CANNOT_SEND)) << "]; "
                     "sending disabled by user? = "
                     "[" << (*err_code == error::Code::S_SENDS_FINISHED_CANNOT_SEND) << "].");
  }

  return true;
} // Blob_stream_mq_sender_impl::send_blob()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::end_sending()
{
  return async_end_sending_impl(nullptr, flow::async::Task_asio_err());
}

template<typename Persistent_mq_handle>
template<typename Task_err>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::async_end_sending(Error_code* sync_err_code_ptr,
                                                                         Task_err&& on_done_func)
{
  Error_code sync_err_code;
  // This guy either takes null/.empty(), or non-null/non-.empty(): it doesn't do the Flow-style error emission itself.
  const bool ok = async_end_sending_impl(&sync_err_code, flow::async::Task_asio_err(std::move(on_done_func)));

  if (!ok)
  {
    return false; // False start.
  }
  // else

  // Standard error-reporting semantics.
  if ((!sync_err_code_ptr) && sync_err_code)
  {
    throw flow::error::Runtime_error(sync_err_code, "Blob_stream_mq_sender_impl::async_end_sending()");
  }
  // else
  sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
  // And if (!sync_err_code_ptr) + no error => no throw.

  return true;
} // Blob_stream_mq_sender_impl::async_end_sending()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::async_end_sending_impl
       (Error_code* sync_err_code_ptr_or_null, flow::async::Task_asio_err&& on_done_func_or_empty)
{
  using flow::async::Task_asio_err;

  assert((bool(sync_err_code_ptr_or_null) == (!on_done_func_or_empty.empty()))
         && "Our contract: indicate whether we care about completion at all -- or not at all -- no in-between.");

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   *
   * There is a *lot* of similarity between this and sync_io::Native_socket_stream::Impl::async_end_sending[_impl]().
   * In fact I (ygoldfel) wrote that guy's send stuff first, then I based this off it.
   * Yet there are enough differences to where code reuse isn't really obviously achievable.
   * @todo Look into it.  Don't forget async_end_sending() that calls us: also quite similar to their counterpart. */

  if (!op_started("async_end_sending()"))
  {
    return false;
  }
  // else

  if (m_finished)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User wants to end sending; we're in sends-finished "
                     "state already.  Ignoring.");
    return false;
  }
  // else
  m_finished = true; // Cause future send_blob() to emit S_SENDS_FINISHED_CANNOT_SEND and return.

  bool qd; // Set to false to report results *now*: basically true <=> stuff is still queued to send.
  if (m_pending_err_code)
  {
    qd = false; // There was outgoing-pipe-ending error detected before us; so should immediately report.
  }
  else
  {
    /* Send the payloads per aforementioned (class doc header) strategy:
     * I.e., send empty message (receiver enters CONTROL state) and immediately Control_cmd::S_END_SENDING-bearing
     * message. */

    // Queue has been entirely flushed (encountering error counts as: yes, flushed) <=> qd = false.
    qd = !sync_write_or_q_ctl_cmd(Control_cmd::S_END_SENDING);
    if (qd && sync_err_code_ptr_or_null)
    {
      /* It has not been flushed (we will return would-block).
       * Save this to emit once everything (including the thing we just made) has been sent off, since
       * they care about completion (sync_err_code_ptr_or_null not null). */
      assert(m_pending_on_last_send_done_func_or_empty.empty());
      m_pending_on_last_send_done_func_or_empty = std::move(on_done_func_or_empty);
      // on_done_func_or_empty is potentially hosed now.
    }
    /* else if (qd && (!sync_err_code_ptr_or_null))
     *   { on_done_func_or_empty is .empty() anyway.  Anyway they don't care about emitting result.  Done:
     *     It'll be async-sent when/if possible. }
     * else if (!qd)
     *   { All flushed synchronously.  We will emit it synchronously, if they're interested in that. } */
  } // if (!m_pending_err_code) (but it may have become truthy inside)

  // Log the error, if any; report the result synchronously if applicable.

  if (m_pending_err_code)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User wanted to end sending, but error (not "
                     "necessarily new error) encountered on pipe synchronously when trying to send graceful-close.  "
                     "Nevertheless locally sends-finished state is now active.  Will report completion via "
                     "sync-args (if any).  Error code details follow: "
                     "[" << m_pending_err_code << "] [" << m_pending_err_code.message() << "].");
    assert(!qd);
  }
  else if (qd)
  {
    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: User wanted to end sending.  Success so far but "
                  "out-queue has payloads -- at least the graceful-close payload -- still pending while waiting for "
                  "writability.  Locally sends-finished state is now active, and the other side will be informed "
                  "of this barring subsequent system errors.  "
                  "We cannot report completion via sync-args (if any).");
  }
  else // if ((!m_pending_err_code) && (!qd))
  {
    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: User wanted to end sending.  Immediate success: "
                  "out-queue flushed permanently.  "
                  "Locally sends-finished state is now active, and the other side will be informed of this.  "
                  "Locally will report completion via sync-args (if any).");
  }

  if (sync_err_code_ptr_or_null)
  {
    *sync_err_code_ptr_or_null = qd ? error::Code::S_SYNC_IO_WOULD_BLOCK
                                    : m_pending_err_code; // Could be falsy (probably is usually).
  }
  // else { Don't care about completion. }

  return true;
} // Blob_stream_mq_sender_impl::async_end_sending_impl()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::auto_ping(util::Fine_duration period)
{
  using util::Fine_duration;
  using util::Task;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   *
   * There is a *lot* of similarity between this and sync_io::Native_socket_stream::Impl::auto_ping().
   * In fact I (ygoldfel) wrote that guy's send stuff first, then I based this off it.
   * Yet there are enough differences to where code reuse isn't really obviously achievable.  @todo Look into it. */

  if (!op_started("auto_ping()"))
  {
    return false;
  }
  // else

  assert(period.count() > 0);

  if (m_finished)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User wants to start auto-pings, but we're in "
                     "sends-finished state already.  Ignoring.");
    return false;
  }
  // else

  /* By concept requirements we are to do 2 things: send an auto-ping now as a baseline; and then make it so
   * *some* message (auto-ping or otherwise) is sent at least every `period` until *end_sending() or error. */

  /* Prepare/send the payload per class doc header strategy.
   * Keeping comments somewhat light, as this is essentially a much simplified version of send_blob()
   * and is very similar to what async_end_sending() does in this spot. */

  if (m_auto_ping_period != Fine_duration::zero())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User wants to start auto-pings, but this "
                     "has already been engaged earlier.  Ignoring.");
    return false;
  }
  // else
  m_auto_ping_period = period; // Remember this, both as flag (non-zero()) and to know how often to reschedule it.

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: User wants to start auto-pings so that there are "
                "outgoing messages at least as frequently as every "
                "[" << round<milliseconds>(m_auto_ping_period) << "].  Sending baseline auto-ping and scheduling "
                "first subsequent auto-ping; it may be rescheduled if more user traffic occurs before then.");

  if (m_pending_err_code)
  {
    /* Concept does not require us to report any error via auto_ping() itself.  It's for receiver's benefit anyway.
     * The local user will discover it, assuming they have interest, via the next send_*() or *end_sending(). */
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User wanted to start auto-pings, but an error was "
                     "previously encountered on pipe; so will not auto-ping.  "
                     "Error code details follow: [" << m_pending_err_code << "] "
                     "[" << m_pending_err_code.message() << "].");
    return true;
  }
  // else

  sync_write_or_q_ctl_cmd(Control_cmd::S_PING);
  if (m_pending_err_code)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Wanted to send initial auto-ping but "
                     "detected error synchronously.  "
                     "Error code details follow: [" << m_pending_err_code << "] "
                     "[" << m_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not schedule periodic auto-pings.");
    return true;
  }
  // else

  // Initial auto-ping partially- or fully-sent fine (anything unsent was queued).  Now schedule next one per above.

  /* Now we can schedule it, similarly to send_blob() and on_ev_auto_ping_now_timer_fired() itself.
   * Recall that we can't simply .async_wait(F) on some timer and have the desired code --
   * on_ev_auto_ping_now_timer_fired() which sends the ping -- be the handler F.  We have to use the sync_io
   * pattern, where the user waits on events for us... but there's no timer FD, so we do it in the separate thread and
   * then ping via a pipe. */

  m_ev_wait_func(&m_ev_wait_hndl_auto_ping_timer_fired_peer,
                 false, // Wait for read.
                 // Once readable do this: pop pipe; send ping; schedule again.
                 boost::make_shared<Task>
                   ([this]() { on_ev_auto_ping_now_timer_fired(); }));
  /* Reminder: That has no effect (other than user recording stuff) until this method returns.
   * So it cannot execute concurrently or anything.  They'd need to do their poll()/epoll_wait() or do so
   * indirectly by returning from the present boost.asio task (if they're running a boost.asio event loop). */

  /* Set up the actual timer.  The second call really does m_auto_ping_timer.async_wait().
   * Note that could fire immediately, even concurrently (if m_auto_ping_period is somehow insanely short,
   * and the timer resolution is amazing)... but it would only cause on_ev_auto_ping_now_timer_fired()
   * once they detect the pipe-readable event, which (again) can only happen after we return. */
  m_auto_ping_timer.expires_after(m_auto_ping_period);
  m_timer_worker.timer_async_wait(&m_auto_ping_timer, m_auto_ping_timer_fired_peer);

  return true;
} // Blob_stream_mq_sender_impl::auto_ping()

template<typename Persistent_mq_handle>
void Blob_stream_mq_sender_impl<Persistent_mq_handle>::on_ev_auto_ping_now_timer_fired()
{
  using util::Blob_const;
  using util::Task;

  /* This is an event handler!  Specifically for the *m_auto_ping_timer_fired_peer pipe reader being
   * readable.  To avoid infinite-loopiness, we'd best pop the thing that was written there. */
  m_timer_worker.consume_timer_firing_signal(m_auto_ping_timer_fired_peer);

  // Now do the auto-ping itself.

  if (m_pending_err_code)
  {
    /* Concept does not require us to report any error via auto_ping() itself.  It's for receiver's benefit anyway.
     * The local user will discover it, assuming they have interest, via the next send_*() or *end_sending(). */
    FLOW_LOG_WARNING("Blob_stream_mq_sender_impl [" << *this << "]: Auto-ping timer fired, but an error was "
                     "previously encountered in 2-way pipe; so will neither auto-ping nor schedule next auto-ping.  "
                     "Error code details follow: [" << m_pending_err_code << "] "
                     "[" << m_pending_err_code.message() << "].");
    return;
  }
  // else

  if (m_finished)
  {
    // This is liable to be quite common and not of much interest at the INFO level; though it's not that verbose.
    FLOW_LOG_TRACE("Blob_stream_mq_sender_impl [" << *this << "]: "
                   "Auto-ping timer fired; but graceful-close API earlier instructed us to no-op.  No-op.");
    return;
  }
  // else

  // This may be of some interest sufficient for INFO.  @todo Reconsider due to non-trivial verbosity possibly.
  FLOW_LOG_INFO("Blob_stream_mq_sender_impl [" << *this << "]: "
                "Auto-ping timer fired; sending/queueing auto-ping; scheduling for next time; it may be "
                "rescheduled if more user traffic occurs before then.");

  // The next code is similar to the initial auto_ping().

  sync_write_or_q_ctl_cmd(Control_cmd::S_PING);
  if (m_pending_err_code)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_sender_impl [" << *this << "]: Wanted to send non-initial auto-ping "
                     "but detected error synchronously.  "
                     "Error code details follow: [" << m_pending_err_code << "] "
                     "[" << m_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not continue scheduling periodic auto-pings.");
    return;
  }
  // else

  m_ev_wait_func(&m_ev_wait_hndl_auto_ping_timer_fired_peer,
                 false, // Wait for read.
                 boost::make_shared<Task>
                   ([this]() { on_ev_auto_ping_now_timer_fired(); }));
  m_auto_ping_timer.expires_after(m_auto_ping_period);
  m_timer_worker.timer_async_wait(&m_auto_ping_timer, m_auto_ping_timer_fired_peer);
} // Blob_stream_mq_sender_impl::on_ev_auto_ping_now_timer_fired()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::sync_write_or_q_payload(const util::Blob_const& orig_blob,
                                                                               bool* avoided_qing_or_null)
{
  using flow::util::Blob;
  using util::Blob_const;

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   * This one is actually fairly different from sync_io::Native_socket_stream::Impl::snd_sync_write_or_q_payload(),
   * as we are dealing with a message boundary-preserving low-level transport among other differences. */

  assert((!m_pending_err_code) && "After m_mq is hosed, no point in trying to do anything; pre-condition violated.");

  const auto protocol_ver_to_send_if_needed = m_protocol_negotiator.local_max_proto_ver_for_sending();
  if (false) // XXX protocol_ver_to_send_if_needed != Protocol_negotiator::S_VER_UNKNOWN)
  {
    assert((m_protocol_negotiator.local_max_proto_ver_for_sending() == Protocol_negotiator::S_VER_UNKNOWN)
           && "Protocol_negotiator not properly marking the once-only sending-out of protocol version?");

    /* Haven't sent it yet.  (However now m_protocol_negotiator.local_max_proto_ver_for_sending() will in fact return
     * S_VER_UNKNOWN, so we're only doing this the one time.  The following won't infinitely recur, etc.)
     *
     * As discussed in m_protocol_negotiator doc header and class doc header "Protocol negotiation" section:
     * send a special CONTROL message; opposing side expects it as the first in-message.
     * By the way m_protocol_negotiator logged about the fact we're about to send it, so we can be pretty quiet. */
    sync_write_or_q_ctl_cmd_impl(-protocol_ver_to_send_if_needed);
    if (m_pending_err_code)
    {
      // New error.  Recursive call already nullified m_mq and all that; if would've returned true; just pass it up.
      return true;
    }
    /* else: Either it inline-sent it (very likely), or it got queued.
     *       Either way: no error; let's get on with queuing-or-sending the actual payload orig_blob!
     * P.S. There's only 1 protocol version as of this writing, so there's no ambiguity, and we can just get on with
     * sending stuff right away.  This could change in the future.  See m_protocol_negotiator doc header for more. */

    // Fall through.
  }
  // else { We've already sent protocol-negotiation msg before (i.e., this isn't the first payload going out. }

  if (m_pending_payloads_q.empty())
  {
    FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Want to send low-level payload: "
                   "blob of size [" << orig_blob.size() << "] "
                   "located @ [" << orig_blob.data() << "]; no write is pending so proceeding immediately.  "
                   "Will drop if would-block? = [" << bool(avoided_qing_or_null) << "].");

    const bool sent = m_mq->try_send(orig_blob, &m_pending_err_code);
    if (m_pending_err_code) // It will *not* emit would-block (will just return false but no error).
    {
      m_mq.reset();
      // Return resources.  (->try_send() logged WARNING sufficiently.)

      assert(!sent);
      avoided_qing_or_null && (*avoided_qing_or_null = false);
      return true; // Pipe-direction-ending error encountered; outgoing-direction pipe is finished forevermore.
    }
    // else

    if (sent)
    {
      // Awesome: Mainstream case: We wrote the whole thing synchronously.
      avoided_qing_or_null && (*avoided_qing_or_null = false);
      return true; // Outgoing-direction pipe flushed.
      // ^-- No error.  Logged about success in ->try_send().
    }
    // else if (!sent): Fall through.
  } // if (m_pending_payloads_q.empty())
  /* else if (!m_pending_payloads_q.empty())
   * { Other stuff is currently being asynchronously sent, so we can only queue our payload behind all that. } */

  /* At this point the payload could not be sent (either because another async write-op is in progress,
   * or not but it would-block if we tried to send now).
   * Per algorithm, we shall now have to queue it up to be sent (and if nothing is currently pending begin
   * asynchronously sending it ASAP).
   *
   * avoided_qing_or_null!=null, as of this writing used for auto-pings only, affects
   * the above as follows: As *all* of orig_blob would need to be queued (queue was already non-empty, or it
   * was empty, and ->try_send() yielded would-block, by definition for *all* of orig_blob.size()), then:
   * simply pretend like it was sent fine; and continue like nothing happened.  (See our doc header for
   * rationale.) */
  if (avoided_qing_or_null)
  {
    *avoided_qing_or_null = true;

    /* This would happen at most every few sec (with auto-pings) and is definitely a rather interesting
     * situation though not an error; INFO is suitable. */
    const auto q_size = m_pending_payloads_q.size();
    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Wanted to send low-level payload: "
                  "blob of size [" << orig_blob.size() << "] located @ [" << orig_blob.data() << "]; "
                  "result was would-block for all of its bytes (either because blocked-queue was non-empty "
                  "already, or it was empty, but payload-message would-block at this time).  "
                  "Therefore dropping payload (done for auto-pings at least).  Out-queue size remains "
                  "[" << q_size << "].");

    /* We won't enqueue it, so there's nothing more to do, but careful in deciding what to return:
     * If the queue is empty, we promised we would return true.  If the queue is not empty, we promised
     * we would return false.  Whether that's what we should do is subtly questionable, but as of this
     * writing what we return when avoided_qing_or_null!=null is immaterial (is ignored). */
    return q_size == 0;
  }
  // else if (!avoided_qing_or_null): Would-block, so queue it up.

  auto new_low_lvl_payload = boost::movelib::make_unique<Snd_low_lvl_payload>();

  /* Allocate N bytes; copy N bytes into that area.
   * This is the first and only place we copy the source blob (not counting the transmission into m_mq); we
   * have tried our best to synchronously send all of it, which would've avoided getting here and this copy.
   * Now we have no choice.  As discussed in the class doc header, probabilistically speaking we should rarely (if
   * ever) get here (and do this annoying alloc, and copy, and later dealloc) under normal operation of both sides. */

  auto& new_blob = new_low_lvl_payload->m_blob = Blob(get_logger());
  if (orig_blob.size() != 0)
  {
    new_blob.assign_copy(orig_blob);
  }

  FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Want to send pending-from-would-block low-level "
                 "payload: blob of size [" << new_low_lvl_payload->m_blob.size() << "] "
                 "located @ [" << orig_blob.data() << "]; "
                 "created blob copy @ [" << new_blob.const_buffer().data() << "]; "
                 "enqueued to out-queue which is now of size [" << (m_pending_payloads_q.size() + 1) << "].");

  m_pending_payloads_q.emplace(std::move(new_low_lvl_payload)); // Push a new Snd_low_lvl_payload::Ptr.

  if (m_pending_payloads_q.size() == 1)
  {
    /* Queue was empty; now it isn't; so start the chain of async send head=>dequeue=>async send head=>dequeue=>....
     * (In our case "async send head" means asking (via m_ev_wait_func) user to inform (via callback we pass
     * to m_ev_wait_func) us when would-block has cleared, call m_mq->try_send() again....
     * If we were operating directly as a boost.asio async loop then the first part would just be
     * m_mq->async_wait(); but we cannot do that; user does it for us, controlling what gets called when
     * synchronously.) */
    async_write_q_head_payload();
    // Queue has stuff in it now.  Return false.
  }
  // else { Queue has even more stuff in it now.  Return false. }

  return false;
} // Blob_stream_mq_sender_impl::sync_write_or_q_payload()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::sync_write_or_q_ctl_cmd(Control_cmd cmd)
{
  return sync_write_or_q_ctl_cmd_impl(static_cast<std::underlying_type_t<Control_cmd>>(cmd));
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::sync_write_or_q_ctl_cmd_impl
       (std::underlying_type_t<Control_cmd> raw_cmd)
{
  using util::Blob_const;

  /* Important: For PING avoided_qing != null; for reasons explained in its doc header.  Namely:
   * Consider S_PING.
   * If both payloads (empty msg, the enum) would-block, then there are already data that would
   * signal non-idleness sitting in the kernel buffer, so the auto-ping can be safely dropped in that case.
   * If 1 was flushed, while 2 would normally be queued, then dropping the latter would make the
   * protocol stream malformed from the opposing side's point of view; so we can't just drop it. */
  bool avoided_qing = false;
  bool* const avoided_qing_or_null = ((raw_cmd >= 0) && (Control_cmd(raw_cmd) == Control_cmd::S_PING))
                                       ? &avoided_qing : nullptr;

  // Payload 1 first.
  bool q_is_flushed = sync_write_or_q_payload(Blob_const(), avoided_qing_or_null);

  /* Returned true => out-queue flushed fully or dropped PING-payload-1 (avoided_qing)
   *                  or new error found (depending on m_pending_err_code).
   * Returned false => out-queue has stuff in it and will until transport writable. */
  if (!m_pending_err_code)
  {
    /* No error => flushed fully (so flush or queue payload 2); or queued (so queue payload 2);
     * or avoided_qing (dropped PING-payload-1). */
    if (!avoided_qing)
    {
      // No error, not avoided_quing => flushed fully (so flush or queue payload 2); or queued (so queue payload 2).
      q_is_flushed
        = sync_write_or_q_payload(Blob_const(&raw_cmd, sizeof(raw_cmd)), nullptr);
      assert(!(m_pending_err_code && (!q_is_flushed)));
    }
    /* else if (avoided_qing)
     * {
     *   No error, avoided_quing => dropped payload 1 due to would-block (so drop payload 2 too: do nothing).
     *   q_is_flushed is set to this or that depending on queue size (as of this writing PINGing code does not care).
     * } */
  }
  else // if (m_pending_err_code)
  {
    assert(q_is_flushed);
  }
  /* To summarize:
   *   - Found error via payload 1, or not but did find error via payload 2 => q_is_flushed.
   *   - Found no error, and flushed payload 1 and payload 2 => q_is_flushed.
   *   - Found no error, but had to queue payload 1 or both payloads => !q_is_flushed.
   *   - Dropped both payloads due to their comprising PING in a would-block situation when payload 1 was tried
   *     => depends on whether queue was (and thus remains) empty at the time payload 1 was tried. */

  return q_is_flushed;
} // Blob_stream_mq_sender_impl::sync_write_or_q_ctl_cmd_impl()

template<typename Persistent_mq_handle>
void Blob_stream_mq_sender_impl<Persistent_mq_handle>::async_write_q_head_payload()
{
  using util::Task;
  using flow::util::Lock_guard;
  using boost::asio::async_write;

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   * This one is actually fairly different from sync_io::Native_socket_stream::Impl::async_write_q_head_payload(),
   * as we are dealing with a message boundary-preserving low-level transport among other differences. */

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert((!m_pending_payloads_q.empty()) && "Contract is stuff is queued to be async-sent.  Bug?");

  /* Conceptually we'd like to do m_mq->async_wait(writable, F), where F() would perform
   * m_mq->try_send() (nb-send).  However this is the sync_io pattern, so
   * the user will be performing the conceptual async_wait() for us.  We must ask them to do so
   * via m_ev_wait_func(), giving them m_mq's FD -- m_ev_wait_hndl_mq -- to wait-on. */

  if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
  {
    /* In this case, though, m_mq has no FD; so we simulate it by awaiting writability in thread W
     * (which exists only for this purpose and is currently idle); and once that is detected we make the
     * pipe-read-end m_mq_ready_reader readable (by writing to write end):
     * *that* is watched by m_ev_wait_hndl_mq. */

    // So step 1 then; start the blocking wait in thread W.
    m_blocking_worker->post([this]()
    {
      // We are in thread W (the one and only code snippet that runs there).

      /* m_mq thread safety: See its doc header.  Spoiler alert: m_pending_payloads_q not empty, so by posting
       * this onto thread W, thread U promises not to touch m_mq until *we* tell it to via m_ev_wait_hndl_mq.
       * send_blob() will see queue is non-empty and enqueue more; similarly for *end_sending();
       * auto-ping-firing will no-op (since queue is non-empty).  The only other thing that would touch m_mq
       * (outside of dtor's m_mq->interrupt_sends()) is the async-wait-handler below -- but that will only run
       * if *we* signal it to.  So it's safe.  (That spoiler was very spoiler-y.  More of a restatement.) */

      Error_code err_code;
      m_mq->wait_sendable(&err_code); // This will TRACE-log plenty.
      if (err_code == error::Code::S_INTERRUPTED)
      {
        FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Blocking-worker was awaiting MQ transmissibility; "
                      "interrupted (presumably by dtor).  Bailing out.");
        return; // Dtor is shutting is down.  GTFO.
      }
      // else

      if (err_code)
      {
        FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Blocking-worker thread was awaiting MQ "
                         "transmissibility; yield error (not interrupted) -- details likely in WARNING above.  "
                         "We lack the means (well, the will mostly) to transmit the fact of the error "
                         "to user-land; so we will just report MQ-writability and let "
                         "user-land code uncover whatever is wrong with the MQ.  This is unusual generally.");
      }
      // else { No problem!  It logged enough; let us signal it. }
      FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Blocking-worker thread was awaiting MQ "
                     "transmissibility; success; now pinging user-land via IPC-pipe.");

      util::pipe_produce(get_logger(), &(*m_mq_ready_writer));
      // m_mq_read_reader now has a byte to read!  m_ev_wait_hndl_mq will be event-active.
    }); // m_blocking_worker->post()
  } // if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
  // else if (Mq::S_HAS_NATIVE_HANDLE) { m_ev_wait_hndl_mq has the MQ's kernel-waitable handle!  Sw33t! }

  bool snd_else_rcv;
  if constexpr(Mq::S_HAS_NATIVE_HANDLE)
  {
    snd_else_rcv = true; // Wait for write (actual MQ!).
  }
  else
  {
    snd_else_rcv = false; // Wait for read (pipe! not actual MQ!).
  }

  m_ev_wait_func(&m_ev_wait_hndl_mq,
                 snd_else_rcv,
                 // Once readable do this:
                 boost::make_shared<Task>
                   ([this]()
  {
    // We are back in *not* thread W!

    FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: User-performed wait-for-writable finished "
                   "(writable or error, we do not know which yet).  We endeavour to send->pop->send->... as much "
                   "of the queue as we can until would-block or total success.");

    assert((!m_pending_payloads_q.empty()) && "Send-queue shouldn't be touched while async-write of head is going.");
    assert((!m_pending_err_code) && "Send error would only be detected by us.  Bug?");

    if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
    {
      util::pipe_consume(get_logger(), &(*m_mq_ready_reader)); // Consume the byte to get to steady-state.
    }
    // else { No byte was written.  In fact there's no pipe even. }

    // Let's do as much as we can.
    bool would_block_or_error;
    do
    {
      auto& low_lvl_payload = *m_pending_payloads_q.front();
      auto& low_lvl_blob = low_lvl_payload.m_blob;
      auto low_lvl_blob_view = low_lvl_blob.const_buffer();

      FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: "
                     "Out-queue size is [" << m_pending_payloads_q.size() << "]; "
                     "want to send blob of size [" << low_lvl_blob_view.size() << "] "
                     "located @ [" << low_lvl_blob_view.data() << "].");
      would_block_or_error = !m_mq->try_send(low_lvl_blob_view, &m_pending_err_code);
      if (m_pending_err_code)
      {
        assert(would_block_or_error);
        assert((m_pending_err_code != error::Code::S_INTERRUPTED) &&
               "That should affect only blocking/timed-blocking MQ ops + is_*able(); not try_*().");
      }
      // else
      if (!would_block_or_error)
      {
        m_pending_payloads_q.pop(); // Nice; dealloc Blob into the aether.
      }
      // else { That's it: party's over.  Exit loop. }
    }
    while ((!m_pending_payloads_q.empty()) && (!would_block_or_error));

    /* Deal with continuing async-write if necessary.
     * Deal with possibly having to fire async_end_sending() completion handler. */

    bool invoke_on_done = false;
    if (m_pending_err_code)
    {
      invoke_on_done = !m_pending_on_last_send_done_func_or_empty.empty();
      FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: User-performed wait-for-writable reported "
                       "completion; wanted to nb-send any queued data and possibly initiated another "
                       "wait-for-writable; got error during an nb-send; TRACE details above.  "
                       "Error code details follow: "
                       "[" << m_pending_err_code << "] [" << m_pending_err_code.message() << "].  "
                       "Saved error code to return in next user send attempt if any.  "
                       "Will run graceful-sends-close completion handler? = ["  << invoke_on_done << "].");

      assert((!m_pending_payloads_q.empty()) && "Opportunistic sanity check.");
    }
    else if (m_pending_payloads_q.empty()) // && (!m_pending_err_code)
    {
      FLOW_LOG_TRACE("Out-queue has been emptied.");

      if (!m_pending_on_last_send_done_func_or_empty.empty())
      {
        // INFO-log is okay, as this occurs at most once per *this.
        FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: "
                      "We sent graceful-close and any preceding user messages with success.  Will now inform user via "
                      "graceful-sends-close completion handler.");
        invoke_on_done = true;
      }
    } // else if (m_pending_payloads_q.empty() && (!m_pending_err_code))
    else // if ((!m_pending_payloads_q.empty()) && (!m_pending_err_code))
    {
      FLOW_LOG_TRACE("Out-queue has not been emptied.  Must keep async-send chain going.");

      // Continue the chain (this guy "asynchronously" brought us here in the first place).
      async_write_q_head_payload();
      // To be clear: queue can now only become empty in "async" handler, not synchronously here.
    }

    if (invoke_on_done)
    {
      FLOW_LOG_TRACE("Blob_stream_mq_sender [" << *this << "]: Executing end-sending completion handler now.");
      auto on_done_func = std::move(m_pending_on_last_send_done_func_or_empty);
      m_pending_on_last_send_done_func_or_empty.clear(); // For cleanliness, in case move() didn't do it.

      on_done_func(m_pending_err_code);
      FLOW_LOG_TRACE("Handler completed.");
    }
  })); // m_ev_wait_func(): on_active_ev_func arg
} // Blob_stream_mq_sender_impl::async_write_q_head_payload()

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_sender_impl<Persistent_mq_handle>::send_blob_max_size() const
{
  return m_mq_max_msg_sz; // As promised in concept API: never changes in PEER state.
}

template<typename Persistent_mq_handle>
const Shared_name& Blob_stream_mq_sender_impl<Persistent_mq_handle>::absolute_name() const
{
  return m_absolute_name;
}

template<typename Persistent_mq_handle>
const std::string& Blob_stream_mq_sender_impl<Persistent_mq_handle>::nickname() const
{
  return m_nickname;
}

template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender_impl<Persistent_mq_handle>& val)
{
  /* Tempting to just print val.m_mq and all those goodies, which include val.absolute_name() too among others,
   * as opposed to sh_name[] only; however m_mq might get hosed during this call and become null; a lock would be
   * required; not worth it. */
  return
    os << "SIO["
       << val.nickname() << "]@" << static_cast<const void*>(&val) << " sh_name[" << val.absolute_name() << ']';
}

} // namespace ipc::transport::sync_io
