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
#include "ipc/util/sync_io/detail/timer_ev_emitter.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/error/error.hpp>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Internal, non-movable pImpl-lite implementation of sync_io::Blob_stream_mq_receiver class template.
 * In and of itself it would have been directly and publicly usable; however Blob_stream_mq_receiver adds move semantics
 * which are essential to cooperation with the rest of the API, Channel in particular.
 *
 * @see All discussion of the public API is in Blob_stream_mq_receiver doc header; that class template forwards to this
 *      one.  All discussion of pImpl-lite-related notions is also there.  See that doc header first please.  Then come
 *      back here.
 *
 * ### Impl design ###
 * See sync_io::Blob_stream_mq_sender_impl.  The same notes apply.  That isn't to say our algorithm is the same
 * at all: we are receiving; they are sending.  However, obviously, they speak the same protocol.  And beyond that
 * it is again about (1) understanding the `sync_io` pattern (util::sync_io doc header) and (2) jumping into the code.
 *
 * Again, for that last part, we recommend starting with the data member doc headers.
 *
 * ### Protocol negotiation ###
 * Do see the same-named section in sync_io::Blob_stream_mq_sender_impl doc header.  Naturally again we're mirroring
 * the same protocol; but we're receiving; they're sending.  Due to the latter difference, though, a couple additional
 * notes:
 *   - We are the guy receiving the preceding-all-messages message containing the opposing side's preferred (highest)
 *     protocol version.  As of this writing we only speak v1, but they might speak a range -- including or
 *     excluding it.  However our Protocol_negotiator takes care of all that; and we only need to add logic to
 *     emit fatal error, if Protocol_negotiator reports that we're incompatible with opposing side.
 *     - Having gotten past that without error, we simply then speak the only version we know how (v1).
 *   - Since only protocol v1 exists as of this writing, there's no need yet for an API to signal a partner
 *     MQ-sender object of the negotiated protocol version, once we have it.  As noted in
 *     sync_io::Blob_stream_mq_sender_impl doc header, that may change in the future.
 *
 * @note We suggest that if version 2, etc., is/are added, then the above notes be kept more or less intact; then
 *       add updates to show changes.  This will provide a good overview of how the protocol evolved; and our
 *       backwards-compatibility story (even if we decide to have no backwards-compatibility).
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver_impl :
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
   * See Blob_stream_mq_receiver counterpart.
   *
   * @param logger_ptr
   *        See Blob_stream_mq_receiver counterpart.
   * @param mq_moved
   *        See Blob_stream_mq_receiver counterpart.
   * @param nickname_str
   *        See Blob_stream_mq_receiver counterpart.
   * @param err_code
   *        See Blob_stream_mq_receiver counterpart.
   */
  explicit Blob_stream_mq_receiver_impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                        Mq&& mq_moved, Error_code* err_code);

  /// See Blob_stream_mq_receiver counterpart.
  ~Blob_stream_mq_receiver_impl();

  // Methods.

  /**
   * See Blob_stream_mq_receiver counterpart.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See Blob_stream_mq_receiver counterpart.
   * @param create_ev_wait_hndl_func
   *        See Blob_stream_mq_receiver counterpart.
   * @return See Blob_stream_mq_receiver counterpart.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * See Blob_stream_mq_receiver counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_receiver counterpart.
   */
  size_t receive_blob_max_size() const;

  /**
   * See Blob_stream_mq_receiver counterpart.
   *
   * @param ev_wait_func
   *        See Blob_stream_mq_receiver counterpart.
   * @return See Blob_stream_mq_receiver counterpart.
   */
  bool start_receive_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func);

  /**
   * See Blob_stream_mq_receiver counterpart, but assuming PEER state.
   *
   * @param target_blob
   *        See Blob_stream_mq_receiver counterpart.
   * @param sync_err_code
   *        See Blob_stream_mq_receiver counterpart.
   * @param sync_sz
   *        See Blob_stream_mq_receiver counterpart.
   * @param on_done_func
   *        See Blob_stream_mq_receiver counterpart.
   * @return See Blob_stream_mq_receiver counterpart.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          Error_code* sync_err_code, size_t* sync_sz,
                          Task_err_sz&& on_done_func);

  /**
   * See Blob_stream_mq_receiver counterpart, but assuming PEER state.
   *
   * @param timeout
   *        See Blob_stream_mq_receiver counterpart.
   * @return See Blob_stream_mq_receiver counterpart.
   */
  bool idle_timer_run(util::Fine_duration timeout);

  /**
   * See Blob_stream_mq_receiver counterpart, but assuming PEER state.
   * @return See Blob_stream_mq_receiver counterpart.
   */
  const std::string& nickname() const;

  /**
   * See Blob_stream_mq_receiver counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_receiver counterpart.
   */
  const Shared_name& absolute_name() const;

private:
  // Types.

  /**
   * Identical to sync_io::Async_adapter_receiver::User_request, except we only keep at most 1 of these
   * and thus don't need a `Ptr` alias inside; and we need not worry about transmitting `Native_handle`s.
   * As in that other class, this records the args from an async_receive_blob() call.
   */
  struct User_request
  {
    // Data.

    /// Same as in sync_io::Async_adapter_receiver::User_request.
    util::Blob_mutable m_target_blob;
    /// Same as in sync_io::Async_adapter_receiver::User_request.
    flow::async::Task_asio_err_sz m_on_done_func;
  }; // struct User_request

  /// Short-hand for Blob_stream_mq_base_impl::Control_cmd.
  using Control_cmd = typename Base::Control_cmd;

  // Methods.

  /**
   * Handler for the async-wait, via util::sync_io::Timer_event_emitter, of the idle timer firing;
   * if still relevant it records the idle-timeout error in #m_pending_err_code; and if
   * an async_receive_blob() is in progress (awaiting data via async-wait), it completes
   * that operation with the appropriate idle-timeout error (completion handler in #m_user_request runs
   * synchronously).  If not still relevant -- #m_pending_err_code already is truthy -- then no-ops.
   */
  void on_ev_idle_timer_fired();

  /**
   * No-ops if idle_timer_run() is not engaged; otherwise reacts to non-idleness of the in-pipe by
   * rescheduling idle timer to occur in #m_idle_timeout again.
   * (Other code calls this, as of this writing, on receipt of a complete message.)
   *
   * Note that this can only occur while an async_receive_blob() is in progress; as otherwise
   * we will not be reading the low-level pipe at all.  This is a requirement for using
   * idle_timer_run(), so it's not our fault, if they don't do it and get timed-out.
   */
  void not_idle();

  /**
   * Begins read chain (completing it as synchronously as possible, async-completing the rest) for the next
   * in-message.
   *
   * Given the pre-condition that (1) async_receive_blob() is oustanding (#m_user_request not null),
   * (2) in the pipe we expect payload 1 (of 1 or 2) of the next in-message next, (3) there is no known pipe error
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
  void read_msg(Error_code* sync_err_code, size_t* sync_sz);

  /**
   * Helper that returns `true` silently if `start_*_ops()` has been called; else
   * logs WARNING and returns `false`.
   *
   * @param context
   *        For logging: the algorithmic context (function name or whatever).
   * @return See above.
   */
  bool op_started(util::String_view context) const;

  // Data.

  /// See nickname().
  const std::string m_nickname;

  /**
   * Handles the protocol negotiation at the start of the pipe and subsequently stores the result of that negotiation.
   *
   * @see Protocol_negotiator doc header for key background on the topic.
   * @see Blob_stream_mq_sender_impl::m_protocol_negotiator doc header which contains notes relevant to possible future
   *      development of the present class regarding speaking multiple protocol versions.
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
   * The MQ handle adopted by the ctor, through which nb-receives and blocking-waits are executed.
   * It is not directly protected by any mutex; however it is accessed exclusively as follows:
   *   - By thread W, when #m_user_request is non-null (would-block state), thus thread U has tasked
   *     it with awaiting readability -- which it is doing and is not yet done doing.
   *   - By thread U, when #m_user_request is null (no would-block at the moment); or
   *     it is not null, but thread W has signaled it is done awaiting non-would-block, so thread U
   *     is trying to nullify it via nb-reads.
   *
   * Exception: `m_mq->interrupt_receives()` is done from thread U (dtor), potentially *while* thread W
   * is inside `m_mq->wait_receivable()`.  However that specific action is allowed by Persistent_mq_handle contract
   * (in fact that is the main utility of Persistent_mq_handle::interrupt_receives()).  It makes the
   * Persistent_mq_handle::wait_receivable() immediately return.
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
   * by the `sync_io`-pattern-using outside event loop.  In our case we (optionally) maintain the idle timer.
   *
   * @see util::sync_io::Timer_event_emitter doc header for design/rationale discussion.
   */
  util::sync_io::Timer_event_emitter m_timer_worker;

  /**
   * Null if no async_receive_blob()` is currently pending; else describes the arguments to that pending
   * async_receive_blob().
   *
   * ### Rationale ###
   * It exists for a hopefully obvious reasons: At least a non-immediately-completed async_receive_blob() needs
   * to keep track of the request so as to know where to place results and what completion handler to invoke.
   *
   * As for it being nullable: this is used to guard against `async_receive_*()` being invoked while another
   * is already outstanding.  We do not queue pending requests per
   * sync_io::Blob_receiver concept.  (However the non-`sync_io` a/k/a async-I/O
   * Blob_receiver transport::Blob_stream_mq_receiver does.  Therefore
   * the latter class does internally implement a `User_request` queue.)
   */
  std::optional<User_request> m_user_request;

  /**
   * At steady-state `false`, becomes `true` if a low-level "escape" payload (empty message) was last received
   * meaning the next message will be the encoding of a #Control_cmd enumeration value, receiving which shall
   * reset this to `false`.
   */
  bool m_control_state;

  /**
   * Used only when #m_control_state is `true`, this is where payload 2 (the #Control_cmd) is placed.
   * Though in reality it should only require `sizeof(Control_cmd)` bytes, #m_mq receive will emit an error
   * if it cannot hold #m_mq_max_msg_sz bytes (that is just how Persistent_mq_handle works; though we do not care
   * why here the reason is that's how both ipc and POSIX MQ APIs work).
   *
   * We could also abuse #m_user_request `m_target_blob`; but that seems uncool.
   *
   * @todo Maybe we should indeed use `m_user_request->m_target_blob` (instead of locally stored
   *       #m_target_control_blob) to save RAM/a few cycles?  Technically at least user should not care if some garbage
   *       is temporarily placed there (after PING a real message should arrive and replace it; or else on error
   *       or graceful-close who cares?).
   */
  flow::util::Blob m_target_control_blob;

  /**
   * The first and only MQ-hosing error condition detected when attempting to low-level-read on
   * #m_mq; or falsy if no such error has yet been detected.  Among possible other uses, it is emitted
   * to the ongoing-at-the-time async_receive_blob()'s completion handler (if one is indeed outstanding)
   * and immediately to any subsequent async_receive_blob().
   */
  Error_code m_pending_err_code;

  /**
   * `timeout` from idle_timer_run() args; or `zero()` if not yet called.  #m_idle_timer stays inactive
   * until this becomes not-`zero()`.
   */
  util::Fine_duration m_idle_timeout;

  /**
   * Timer that fires on_ev_idle_timer_fired() (which hoses the pipe with idle timeour error) and is
   * (re)scheduled to fire in #m_idle_timeout each time `*this` receives a complete message
   * on #m_mq.  If it does fire, without being preempted by some error to have occurred since then,
   * the pipe is hosed with a particular error indicating idle-timeout (so that `Error_code` is saved
   * to #m_pending_err_code).
   *
   * Since we implement `sync_io` pattern, the timer is obtained from, and used via, util::sync_io::Timer_event_emitter
   * #m_timer_worker.  See that member's doc header for more info.
   */
  flow::util::Timer m_idle_timer;

  /**
   * Read-end of IPC-mechanism used by #m_timer_worker to ferry timer-fired events from #m_idle_timer
   * to `sync_io` outside async-wait to our actual on-timer-fired handler logic.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Timer_event_emitter::Timer_fired_read_end* m_idle_timer_fired_peer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_idle_timer_fired_peer.
   *
   * @see #m_timer_worker for more info.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_idle_timer_fired_peer;

  /**
   * Function (set forever in start_receive_blob_ops()) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to our ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_ev_wait_func;

  /**
   * Worker thread W always in one of 2 states: idle; or (when #m_mq is in would-block condition) executing an
   * indefinite, interrupting blocking wait for transmissibility of #m_mq.  When thread U wants to receive
   * a payload but gets would-block, it issues the wait on this thread W and a `sync_io`-pattern async-wait
   * for #m_ev_wait_hndl_mq; once that wait completes in thread W, it writes a byte to an internal IPC-pipe.
   * #m_ev_wait_hndl_mq becomes readable, the outside event loop lets `*this` know, which completes the async-wait.
   *
   * In dtor we stop thread W, including using Persistent_mq_handle::interrupt_receives() to abort the indefinite
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
}; // class Blob_stream_mq_receiver_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Persistent_mq_handle>
Blob_stream_mq_receiver_impl<Persistent_mq_handle>::Blob_stream_mq_receiver_impl
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq,
   Error_code* err_code) :

  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_nickname(nickname_str),
  m_protocol_negotiator(get_logger(), nickname(),
                        2, 1), // XXX Initial protocol!  @todo Magic-number `const`(s), particularly if/when v2 exists.
  m_absolute_name(mq.absolute_name()),
  // m_mq null for now but is set up below.
  m_mq_max_msg_sz(mq.max_msg_size()), // Just grab this now though.
  m_ev_wait_hndl_mq(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_timer_worker(get_logger(), flow::util::ostream_op_string(*this)),
  m_control_state(false),
  m_target_control_blob(get_logger(), receive_blob_max_size()), // See its doc header.
  m_idle_timeout(util::Fine_duration::zero()), // idle_timer_run() not yet called.
  m_idle_timer(m_timer_worker.create_timer()), // Inactive timer (idle_timer_run() not yet called).
  /* If it does become active, we'll use this readable-pipe-peer to get informed by m_timer_worker that
   * m_idle_timer has fired.  That way we can use m_ev_wait_func() mechanism to have user
   * ferry timer firings to us. */
  m_idle_timer_fired_peer(m_timer_worker.create_timer_signal_pipe()),
  // And this is its watchee mirror for outside event loop (sync_io pattern).
  m_ev_wait_hndl_idle_timer_fired_peer
    (m_ev_hndl_task_engine_unused,
     Native_handle(m_idle_timer_fired_peer->native_handle()))
{
  using flow::error::Runtime_error;
  using flow::util::ostream_op_string;
  using boost::asio::connect_pipe;

  // This is near-identical to Blob_stream_mq_sender_impl ctor.  Keeping comments light.  @todo Code reuse.

  auto& sys_err_code = m_pending_err_code;

  m_mq = Base::ensure_unique_peer(get_logger(),
                                  std::move(mq), false /* receiver */, &sys_err_code); // Does not throw.
  assert(bool(m_mq) == (!sys_err_code));
  // On error *err_code will be truthy, and it will have returned null.

  if (!sys_err_code)
  {
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

      // Start thread W, for when (and if) we need to use it to async-wait for transmissibility.
      m_blocking_worker.emplace(get_logger(),
                                ostream_op_string("mq_rcv-", nickname())); // Thread W started just below.
      m_blocking_worker->start();

      // For now we just set up the IPC-pipe and the sync_io-watched FD mirror.  First things first... err, second....
      connect_pipe(*m_mq_ready_reader, *m_mq_ready_writer, sys_err_code);
      if (sys_err_code)
      {
        FLOW_LOG_WARNING
          ("Blob_stream_mq_receiver [" << *this << "]: Constructing: connect-pipe failed.  Details follow.");
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
    // `mq` is untouched.  m_pending_err_code will cause that to be emitted on async_receive_blob() and such.
    assert(sys_err_code);

    if (err_code)
    {
      *err_code = sys_err_code;
      return;
    }
    // else
    throw Runtime_error(sys_err_code, "Blob_stream_mq_receiver(): ensure_unique_peer()");
  }
  // else: took over `mq` ownership.
  assert(!sys_err_code);

  FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: MQ-handle-watching apparatus ready including running "
                "blocking worker thread for would-block situations if necessary "
                "(is it? = [" << (!Mq::S_HAS_NATIVE_HANDLE) << "]).");
} // Blob_stream_mq_receiver_impl::Blob_stream_mq_receiver()

template<typename Persistent_mq_handle>
Blob_stream_mq_receiver_impl<Persistent_mq_handle>::~Blob_stream_mq_receiver_impl()
{
  // This is near-identical to Blob_stream_mq_sender_impl ctor.  Keeping comments light.  @todo Code reuse.

  FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: Shutting down.  Async-wait worker thread and timer "
                "thread will shut down/be joined shortly; but we must interrupt any currently-running async-wait to "
                "ensure async-wait worker thread actually exits.");

  if (m_mq)
  {
    m_mq->interrupt_receives();
  }
}

template<typename Persistent_mq_handle>
template<typename Create_ev_wait_hndl_func>
bool Blob_stream_mq_receiver_impl<Persistent_mq_handle>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  if (m_pending_err_code)
  {
    /* Ctor failed (without throwing, meaning they used the non-null-err_code semantic).
     * It is tempting to... <see comment in _sender_impl same place.  @todo Code reuse. */
    FLOW_LOG_WARNING("Blob_stream_mq_sender [" << *this << "]: Cannot replace event-wait handles as requested: "
                     "ctor failed earlier ([" << m_pending_err_code << "] [" << m_pending_err_code.message() << "].  "
                     "Any transmission attempts will fail in civilized fashion, so we will just no-op here.");
    return true;
  }
  // else

  FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: Replacing event-wait handles (probably to replace "
                "underlying execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl_mq.is_open());
  assert(m_ev_wait_hndl_idle_timer_fired_peer.is_open());

  Native_handle saved(m_ev_wait_hndl_mq.release());
  m_ev_wait_hndl_mq = create_ev_wait_hndl_func();
  m_ev_wait_hndl_mq.assign(saved);

  saved.m_native_handle = m_ev_wait_hndl_idle_timer_fired_peer.release();
  m_ev_wait_hndl_idle_timer_fired_peer = create_ev_wait_hndl_func();
  m_ev_wait_hndl_idle_timer_fired_peer.assign(saved);

  return true;
} // Blob_stream_mq_receiver_impl::replace_event_wait_handles()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_receiver_impl<Persistent_mq_handle>::
       start_receive_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  m_ev_wait_func = std::move(ev_wait_func);

  FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: Start-ops requested.  Done.");
  return true;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_receiver_impl<Persistent_mq_handle>::op_started(util::String_view context) const
{
  if (m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: "
                     "In context [" << context << "] we must be start_...()ed, "
                     "but we are not.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else
  return true;
}

template<typename Persistent_mq_handle>
template<typename Task_err_sz>
bool Blob_stream_mq_receiver_impl<Persistent_mq_handle>::async_receive_blob
       (const util::Blob_mutable& target_blob, Error_code* sync_err_code_ptr, size_t* sync_sz,
        Task_err_sz&& on_done_func)
{
  using util::Blob_mutable;

  if (!op_started("async_receive_blob()"))
  {
    return false;
  }
  // else

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   *
   * Briefly though: If you understand opposing send_blob() impl, then this will be easy in comparison:
   * no more than one async_receive_blob() can be outstanding at a time; they are not allowed to
   * invoke it until we invoke their completion handler.  (Rationale is shown in detail elsewhere.)
   * So there is no queuing of requests (deficit); nor reading more messages beyond what has been requested
   * (surplus).
   *
   * That said, we must inform them of completion via on_done_func(), whereas send_blob() has no
   * such requirement; easy enough -- we just save it as needed.  In that sense it is much like
   * opposing async_end_sending().
   *
   * ...Well, no, there's one more complicating matter -- though it's nowhere near as hairy as what rcv-side of
   * Native_socket_stream::Impl has to deal with:  Sure, there is no queuing of requests, but the outgoing-direction
   * algorithm makes the low-level payloads and then just sends them out -- what's inside doesn't matter (almost).
   * Incoming-direction is harder, because we need to read payload 1, interpret it, possibly read payload 2.
   * So there is a bit of tactical nonsense about async-waiting and then resuming from the same spot and so on.
   * However, unlike with Native_socket_stream::Impl our message boundaries are always preserved, so it is a ton
   * simpler.  Basically it's just a matter of keeping track of the flag m_control_state and acting differently
   * depending on whether it's true or not (when analyzing an incoming low-level payload). */

  if (m_user_request)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: Async-receive requested, but the preceding such "
                     "request is still in progress; the message has not arrived yet (we are awaiting "
                     "readability, before we can get the rest of it).  Likely a user error, but who are "
                     "we to judge?  Ignoring.");
    return false;
  }
  // else

  Error_code sync_err_code;

  FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: User async-receive request for "
                 "blob (located @ [" << target_blob.data() << "] of max size [" << target_blob.size() << "]).");

  if (m_pending_err_code)
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: User async-receive request for "
                     "blob (located @ [" << target_blob.data() << "] of "
                     "max size [" << target_blob.size() << "]): Error already encountered earlier.  Emitting "
                     "via sync-args.");

    sync_err_code = m_pending_err_code;
    *sync_sz = 0;
  }
  else // if (!m_pending_err_code)
  {
    // This next check: background can be found by following the comment on this concept constant.
    static_assert(!Blob_stream_mq_receiver<Mq>::S_BLOB_UNDERFLOW_ALLOWED,
                   "MQs disallow even trying to receive into a buffer that could underflow "
                     "with the largest *possible* message -- even if the actual message "
                     "happens to be small enough to fit.");
    if (target_blob.size() < receive_blob_max_size())
    {
      FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: User async-receive request for "
                       "blob (located @ [" << target_blob.data() << "] of "
                       "max size [" << target_blob.size() << "]): "
                       "that size underflows MQ size limit [" << receive_blob_max_size() << "].  "
                       "Emitting error immediately; but pipe continues.  Note that if we don't do this, "
                       "then the low-level MQ will behave this way anyway.");
      // As mandated by concept *do not* hose pipe by setting m_pending_err_code given INVALID_ARGUMENT.  Just emit.

      sync_err_code = error::Code::S_INVALID_ARGUMENT;
      *sync_sz = 0;
    }
    else
    {
      m_user_request.emplace();
      m_user_request->m_target_blob = target_blob;
      m_user_request->m_on_done_func = std::move(on_done_func);

      read_msg(&sync_err_code, sync_sz);
    } // else if (target_blob.size() is fine)
  } // else if (!m_pending_err_code)

  if ((!sync_err_code) || (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK))
  {
    FLOW_LOG_TRACE("Async-request completed synchronously (result "
                    "[" << sync_err_code << "] [" << sync_err_code.message() << "]); emitting synchronously and "
                    "disregarding handler.");
    m_user_request.reset(); // No-op if we didn't set it up due to early error being detected above.
  }
  // else { Other stuff logged enough. }

  // Standard error-reporting semantics.
  if ((!sync_err_code_ptr) && sync_err_code)
  {
    throw flow::error::Runtime_error(sync_err_code, "Blob_stream_mq_receiver_impl::async_receive_blob()");
  }
  // else
  sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
  // And if (!sync_err_code_ptr) + no error => no throw.

  return true;
} // Blob_stream_mq_receiver_impl::async_receive_blob()

template<typename Persistent_mq_handle>
bool Blob_stream_mq_receiver_impl<Persistent_mq_handle>::idle_timer_run(util::Fine_duration timeout)
{
  using util::Fine_duration;
  using util::Task;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert(timeout.count() > 0);

  if (!op_started("idle_timer_run()"))
  {
    return false;
  }
  // else

  // According to concept requirements we shall no-op/return false if duplicately called.
  if (m_idle_timeout != Fine_duration::zero())
  {
    FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: User wants to start idle timer, but they have "
                     "already started it before.  Therefore ignoring.");
    return false;
  }
  // else

  m_idle_timeout = timeout; // Remember this, both as flag (non-zero()) and to know to when to schedule it.
  // Now we will definitely return true (even if an error is already pending).  This matches concept requirements.

  if (m_pending_err_code)
  {
    FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: User wants to start idle timer, but an error has "
                  "already been found and emitted earlier.  It's moot; ignoring.");
    return true;
  }
  // else

  FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: User wants to start idle-timer with timeout "
                "[" << round<milliseconds>(m_idle_timeout) << "].  Scheduling (will be rescheduled as needed).");

  /* Per requirements in concept, start it now; reschedule similarly each time there is activity.
   *
   * The mechanics of timer-scheduling are identical to those in opposing auto_ping() and are explained there;
   * keeping comments light here. */

  m_ev_wait_func(&m_ev_wait_hndl_idle_timer_fired_peer,
                 false, // Wait for read.
                 boost::make_shared<Task>
                   ([this]() { on_ev_idle_timer_fired(); }));

  m_idle_timer.expires_after(m_idle_timeout);
  m_timer_worker.timer_async_wait(&m_idle_timer, m_idle_timer_fired_peer);

  return true;
} // Blob_stream_mq_receiver_impl::idle_timer_run()

template<typename Persistent_mq_handle>
void Blob_stream_mq_receiver_impl<Persistent_mq_handle>::on_ev_idle_timer_fired()
{
  /* This is an event handler!  Specifically for the *m_idle_timer_fired_peer pipe reader being
   * readable.  To avoid infinite-loopiness, we'd best pop the thing that was written there. */
  m_timer_worker.consume_timer_firing_signal(m_idle_timer_fired_peer);

  if (m_pending_err_code)
  {
    FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Idle timer fired: There's been 0 traffic past idle "
                   "timeout.  However an error has already been found and emitted earlier.  Therefore ignoring.");
    return;
  }
  // else

  m_pending_err_code = error::Code::S_RECEIVER_IDLE_TIMEOUT;

  FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: Idle timer fired: There's been 0 traffic past idle "
                   "timeout.  Will not proceed with any further low-level receiving.  If a user async-receive request "
                   "is pending (is it? = [" << bool(m_user_request) << "]) will emit to completion handler.");

  if (m_user_request)
  {
    // Prevent stepping on our own toes: move/clear it first / invoke handler second.
    const auto on_done_func = std::move(m_user_request->m_on_done_func);
    m_user_request.reset();
    on_done_func(m_pending_err_code, 0);
    FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Handler completed.");
  }

  /* That's it.  If m_user_request (we've just nullified it) then async read chain will be finished forever as
   * soon as the user informs us of readability (if it ever does) -- we will detect there's an error
   * in m_pending_err_code already (and hence no m_user_request). */
} // Blob_stream_mq_receiver_impl::on_ev_idle_timer_fired()

template<typename Persistent_mq_handle>
void Blob_stream_mq_receiver_impl<Persistent_mq_handle>::not_idle()
{
  using util::Fine_duration;

  if (m_idle_timeout == Fine_duration::zero())
  {
    return;
  }
  // else

  /* idle_timer_run() has enabled the idle timer feature, and we've been called indicating we just read something,
   * and therefore it is time to reschedule the idle timer. */

  const auto n_canceled = m_idle_timer.expires_after(m_idle_timeout);

  if (n_canceled == 0)
  {
    // This is a fun, rare coincidence that is worth an INFO message.
    FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: Finished reading a message, which means "
                  "we just received traffic, which means the idle timer should be rescheduled.  However "
                  "when trying to reschedule it, we found we were too late: it was very recently queued to "
                  "be invoked in the near future.  An idle timeout error shall be emitted very soon.");
  }
  else // if (n_canceled >= 1)
  {
    assert((n_canceled == 1) && "We only issue 1 timer async_wait() at a time.");

    /* m_timer_worker will m_idle_timer.async_wait(F), where F() will signal through pipe,
     * making *m_idle_timer_fired_peer readable.  We've already used m_ev_wait_func() to start
     * wait on it being readable and invoke on_ev_idle_timer_fired() in that case; but we've
     * canceled the previous .async_wait() that would make it readable; so just redo that part. */
    m_timer_worker.timer_async_wait(&m_idle_timer, m_idle_timer_fired_peer);
  }
} // Blob_stream_mq_receiver_impl::not_idle()

template<typename Persistent_mq_handle>
void Blob_stream_mq_receiver_impl<Persistent_mq_handle>::read_msg(Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using util::Blob_mutable;
  using flow::util::Lock_guard;
  using raw_ctl_cmd_enum_t = std::underlying_type_t<Control_cmd>;

  assert(!m_pending_err_code);
  assert(m_user_request);

  /* We just entered possibly-not-would-block state of m_mq; and are about to nb-read message (which might would-block
   * and other possibilities).  Pre-condition is m_user_request is truthy -- has not been satisfied.  Lastly
   * m_control_state may be true or false depending on the last payload (if any).
   *
   *   -# MQ error or protocol error (invalid ctl msg) or graceful-close ctl msg => m_pending_err_code becomes true;
   *      emit to m_user_request; GTFO.
   *   -# Would-block => async-wait for readability, when ready read_msg() (us) again (unless
   *                     idle-timeout in the meantime, so be sure to not read_msg() in that case).  GTFO here
   *                     (algorithm continues asynchronously).
   *   -# Valid msg:
   *      -# If m_control_state already: So then it's PING => m_control_state=false; register non-idleness; go again*.
   *      -# Else:
   *         -# If empty msg => m_control_state=true; go again*.
   *         -# Else: regular user msg => emit to m_user_request; GTFO.
   *
   * (*) - What is "go again"?  It means repeat the above steps.  One way to structure this would be for read_msg()
   *       to then call read_msg().  That is algorithmically sound conceptually, but for example suppose there are
   *       100 PINGs queued up; we do not want 100+ stack frames.  So we avoid this recursion; instead it's a
   *       do-while loop, with the above steps being one iteration.
   *
   * Let us consider the possible outcomes of an iteration as listed above.
   *   -# m_pending_err_code truthy, emit-to-user, do not reiterate.
   *   -# m_pending_err_code falsy, emit-to-user, do not reiterate.
   *   -# Async-wait started, do not emit-to-user, do not reiterate.
   *   -# Reiterate.  (m_control_state would have changes to true or to false.)
   *
   * Here is how we structure the flow control then.  Firstly, outcome (3) stylistically we normally handle in various
   * places by `return`ing after starting async-wait.  It's nice visually to show that when the algorithm continues
   * asynchronously, then we don't do anything at the tail end (after the async-wait).  So, that's that for outcome (3).
   *
   * That leaves 2 possibilities if end of iteration is reached:
   *   - emit-to-user, stop loop;
   *   - do not emit-to-user, continue loop.
   *
   * Hence track it with `bool emit`; and end the do-while() once it becomes true.
   */
  bool emit = false;
  Blob_mutable target_blob;

  do
  {
    FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Async-receive: Start of payload: "
                   "Either user message or CONTROL-state escape message; unless already in CONTROL state "
                   "(are we? = [" << m_control_state << "]) -- then the CONTROL command enum value.");

    target_blob = m_control_state ? Blob_mutable(m_target_control_blob.data(),
                                                 m_target_control_blob.size())
                                  : m_user_request->m_target_blob;

    const bool rcvd_else_would_block_or_error
      = m_mq->try_receive(&target_blob, &m_pending_err_code);

    if (!m_pending_err_code)
    {
      if (rcvd_else_would_block_or_error)
      {
        // Not error, not would block: got low-level message.  Interpret it.

        /* Let's discuss protocol negotiation, as it applies to us.  Simply, the first thing we must receive
         * (though there's no requirement as to how soon it happens -- it just needs to precede anything else)
         * is a CONTROL message, whose payload (the 2nd MQ-message) contains a negative number (unlike regular
         * CONTROL messages, wherein it's a non-negative number encoding Control_cmd enum: 0, 1, ...), the
         * negation of which is the opposing side's preferred (highest supported) protocol version to pass-to
         * m_protocol_negotiator.compute_negotiated_proto_ver() -- which completes the negotiation.
         *
         * Since m_protocol_negotiator offers ability to track "first message" or "not first message," we don't
         * need to keep our own m_ state about that.  Thus: */
        bool proto_negotiating
          = m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN;
         /* If false, and we haven't errored-out of the loop, then negotiation is behind us.
          * If true, then in a couple places below we'll need to zealously handle the negotiation.
          * Generally speaking, the tactics we follow (when `true`) are consistent with Protocol_negotiator doc
          * header convention which mandates that we interpret as little as we can, until proto_negotiating
          * becomes true, or we error out due to negotiation fail. */

        if (m_control_state)
        {
          // Do not forget!
          m_control_state = false;

          if (target_blob.size() == sizeof(Control_cmd))
          {
            const auto& cmd = *(reinterpret_cast<const Control_cmd*>(target_blob.data()));
            const auto raw_cmd = raw_ctl_cmd_enum_t(cmd);

            if (proto_negotiating)
            {
              /* Protocol_negotiator handles everything (invalid value, incompatible range...); we just know
               * the encoding is to be the negation of the version. */
#ifndef NDEBUG
              const bool ok =
#endif
              m_protocol_negotiator.compute_negotiated_proto_ver(-raw_cmd, &m_pending_err_code);
              assert(ok && "Protocol_negotiator breaking contract?  Bug?");
              proto_negotiating = false; // Just in case (maintainability).

              /* Cool, so now either all is cool, and we should keep reading; or m_pending_err_code contains how
               * negotiation failed, and loop will exit.  The general code before the } in `if (m_control) {}` will
               * handle it either way. */
            } // if (proto_negotiating)
            else if (raw_cmd >= raw_ctl_cmd_enum_t(Control_cmd::S_END_SENTINEL)) // && !proto_negotiating
            {
              FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: In CONTROL state expecting encoding "
                               "enum value < int[" << raw_ctl_cmd_enum_t(Control_cmd::S_END_SENTINEL) << "] but got "
                               "[" << raw_cmd << "]; emitting protocol error (bug on sender side?).");
              m_pending_err_code = error::Code::S_BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD;
            }
            else // if ((!proto_negotiating) && (cmd is valid))
            {
              switch (cmd)
              {
              case Control_cmd::S_END_SENDING:
                m_pending_err_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE;
                break;
              case Control_cmd::S_PING:
                // As of this writing also INFO-level on sender side.   @todo Reconsider that and this too.
                FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: In CONTROL state got PING.  Ignoring "
                              "other than registering non-idle activity.  Back in non-CONTROL state.");
                not_idle();

                // Like it said -- ignore it.  Just go for the next message.
                break;
              case Control_cmd::S_END_SENTINEL:
                assert(false && "Should've been handled above already.");
              } // switch (cmd)
            } // else if ((!proto_negotiating) && (cmd is valid))
          } // if (target_blob.size() == sizeof(Control_cmd))
          else // if (target_blob.size() != sizeof(Control_cmd))
          {
            FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: In CONTROL state expecting msg of "
                             "exactly size [" << sizeof(Control_cmd) << "] "
                             "but got [" << target_blob.size() << "]; emitting protocol error (bug on sender side?).");
            m_pending_err_code = error::Code::S_BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD;
          }

          // Close (graceful or error) => emit to user; end loop.  Ping => continue loop.
          assert(!emit);
          m_pending_err_code && (emit = true);
        } // if (m_control_state)
        else // if (!m_control_state) // && (rcvd_else_would_block_or_error) (so, got message, in non-CONTROL state)
        {
          assert(!emit);
          if (target_blob.size() == 0)
          {
            FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: In non-CONTROL state got escape message.  "
                           "Entering CONTROL state; reading again if possible.");
            m_control_state = true;

            /* if (proto_negotiating) { Good: In non-CONTROL state -- at start -- first message should do just this. }
             * else { Neither good nor bad; just apparently it's a CONTROL message coming next.  Err, good. } */
          }
          else // if (target_blob.size() != 0)
          {
            if (proto_negotiating)
            {
              /* In non-CONTROL state -- at start -- before protocol has been negotiated, the first message *must*
               * be a CONTROL message.  Formally speaking we'll never be able to parse the negotiated version, because
               * it is not coming first according to the protocol.  So, per it API, we can just give
               * Protocol_negotiator an invalid version, so it'll emit the proper error. */
              FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: In non-CONTROL state got regular "
                               "user message.  However this is the start of the comm-pathway, and by protocol "
                               "the first thing must be a CONTROL message containing the protocol version; so "
                               "we shall now emit error accordingly.");
#ifndef NDEBUG
              const bool ok =
#endif
              m_protocol_negotiator.compute_negotiated_proto_ver(Protocol_negotiator::S_VER_UNKNOWN,
                                                                 &m_pending_err_code);
              assert(ok && "Protocol_negotiator breaking contract?  Bug?");
              assert(m_pending_err_code
                     && "Protocol_negotiator should have emitted error given intentionally bad version.");
              proto_negotiating = false; // Just in case (maintainability).
            }
            else // if (!proto_negotiating)
            {
              // Regular user message in regular operation, post any negotiation.
              FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: In non-CONTROL state got regular "
                             "user message.  Will emit to user (possibly via sync-args, namely if this is "
                             "synchronously within async-receive API).  Also registering non-idle activity.");
              not_idle();
            }

            // Emit message (much more likely); or emit error.  Either way:
            emit = true;
          }
        } // else if (!m_control_state)
      } // if (rcvd_else_would_block_or_error) (and !m_pending_err_code, but it may have become truthy inside)
      else // if (!rcvd_else_would_block_or_error) (and !m_pending_err_code, so in fact would-block)
      {
        FLOW_LOG_TRACE("Got would-block.  Awaiting readability.");

        /* Conceptually we'd like to do m_mq->async_wait(readable, F), where F() would perform
         * read_msg() (the present method: nb-receive over m_mq).  However this is the sync_io pattern, so
         * the user will be performing the conceptual async_wait() for us.  We must ask them to do so
         * via m_ev_wait_func(), giving them m_mq's FD -- m_ev_wait_hndl_mq -- to wait-on.  */

        if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
        {
          /* In this case, though, m_mq has no FD; so we simulate it by awaiting readability in thread W
           * (which exists only for this purpose and is currently idle); and once that is detected we make the
           * pipe-read-end m_mq_ready_reader readable (by writing to write end):
           * *that* is watched by m_ev_wait_hndl_mq. */

          // So step 1 then; start the blocking wait in thread W.
          m_blocking_worker->post([this]()
          {
            // We are in thread W (the one and only code snippet that runs there).

            /* m_mq thread safety: See its doc header.  Spoiler alert: m_user_request not empty, so by posting
             * this onto thread W, thread U promises not to touch m_mq until *we* tell it to via m_ev_wait_hndl_mq.
             * async_receive_blob() will see m_user_request is non-empty and refuse to proceed.
             * The only other thing that would touch m_mq (outside of dtor's m_mq->interrupt_receives()) is the
             * async-wait-handler below -- but that will only run if *we* signal it to.  So it's safe.  (That spoiler
             * was very spoiler-y.  More of a restatement.) */

            Error_code err_code;
            m_mq->wait_receivable(&err_code); // This will TRACE-log plenty.
            if (err_code == error::Code::S_INTERRUPTED)
            {
              FLOW_LOG_INFO("Blob_stream_mq_receiver [" << *this << "]: Blocking-worker was awaiting MQ "
                            "transmissibility; interrupted (presumably by dtor).  Bailing out.");
              return; // Dtor is shutting is down.  GTFO.
            }
            // else

            if (err_code)
            {
              FLOW_LOG_WARNING("Blob_stream_mq_receiver [" << *this << "]: Blocking-worker thread was awaiting MQ "
                               "transmissibility; yield error (not interrupted) -- details likely in WARNING above.  "
                               "We lack the means (well, the will mostly) to transmit the fact of the error "
                               "to user-land; so we will just report MQ-readability and let "
                               "user-land code uncover whatever is wrong with the MQ.  This is unusual generally.");
            }
            // else { No problem!  It logged enough; let us signal it. }
            FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Blocking-worker thread was awaiting MQ "
                           "transmissibility; success; now pinging user-land via IPC-pipe.");

            util::pipe_produce(get_logger(), &(*m_mq_ready_writer));
            // m_mq_read_reader now has a byte to read!  m_ev_wait_hndl_mq will be event-active.
          }); // m_blocking_worker->post()
        } // if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
        // else if (Mq::S_HAS_NATIVE_HANDLE) { m_ev_wait_hndl_mq has the MQ's kernel-waitable handle!  Sw33t! }

        m_ev_wait_func(&m_ev_wait_hndl_mq,
                       false, // Wait for read (whether pipe or actual MQ!).
                       // Once readable do this:
                       boost::make_shared<Task>
                         ([this]()
        {
          // We are back in *not* thread W!

          if constexpr(!Mq::S_HAS_NATIVE_HANDLE)
          {
            util::pipe_consume(get_logger(), &(*m_mq_ready_reader)); // Consume the byte to get to steady-state.
          }
          // else { No byte was written.  In fact there's no pipe even. }

          FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: User-performed wait-for-readable finished "
                         "(readable or error, we do not know which yet).  We endeavour to receive->pop->receive->... "
                         "as much of the MQ as we can until would-block, or we have a full message (so either "
                         "1 message, or if in CONTROL state then 2 messages).");
          if (m_pending_err_code)
          {
            FLOW_LOG_TRACE("However error (presumably idle-timeout) occurred in the meantime; "
                           "hence stopping read-chain forever.");

            assert((!m_user_request) // Sanity-check.
                   && "If rcv-error emitted during low-level async-wait, we should have fed it to any "
                        "pending async-receive.");
            return;
          }
          // else if (!m_pending_err_code)
          assert(m_user_request && "Only an error, or we ourselves here, can satisfy user request."); // Sanity check.

          // Will potentially emit these (if and only if message-read completes due to this successful async-wait).
          Error_code sync_err_code;
          size_t sync_sz;

          // Would-not-block, as far as we know.  Try again.
          read_msg(&sync_err_code, &sync_sz);

          if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
          {
            // Another async-wait is pending now.  We've logged enough.  Live to fight another day.
            return;
          }
          // else: Message completed/error!

          FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Async-op result ready after successful "
                         "async-wait.  Executing handler now.");

          // Prevent stepping on our own toes: move/clear it first / invoke handler second.
          const auto on_done_func = std::move(m_user_request->m_on_done_func);
          m_user_request.reset();
          on_done_func(sync_err_code, sync_sz);
          FLOW_LOG_TRACE("Blob_stream_mq_receiver [" << *this << "]: Handler completed.");
        })); // m_ev_wait_func(): on_active_ev_func arg

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return; // Async read chain started/continuing.  GTFO.
      } // else if (!rcvd_else_would_block_or_error) (and !m_pending_err_code, so in fact would-block)
    } // if (!m_pending_err_code) [from m_mq->try_receive()] (but it may have become truthy inside)
    else // if (m_pending_err_code) [from m_mq->try_receive()]
    {
      assert(!emit);
      emit = true; // Error => emit to user, end loop.  The try_*() WARNING+TRACE-logged enough.
    }
  }
  while (!emit);

  // Passed the gauntlet.  Emit whatever happened to user.

  assert(m_pending_err_code || // Sanity-check: Either error/closed, or we got a nice non-empty message.
         ((!m_control_state) && (target_blob.size() != 0)));

  *sync_err_code = m_pending_err_code;
  *sync_sz = m_pending_err_code ? 0 : target_blob.size();
} // Blob_stream_mq_receiver_impl::read_msg()

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_receiver_impl<Persistent_mq_handle>::receive_blob_max_size() const
{
  return m_mq_max_msg_sz; // As promised in concept API: never changes in PEER state.
}

template<typename Persistent_mq_handle>
const Shared_name& Blob_stream_mq_receiver_impl<Persistent_mq_handle>::absolute_name() const
{
  return m_absolute_name;
}

template<typename Persistent_mq_handle>
const std::string& Blob_stream_mq_receiver_impl<Persistent_mq_handle>::nickname() const
{
  return m_nickname;
}

template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver_impl<Persistent_mq_handle>& val)
{
  /* Tempting to just print val.m_mq and all those goodies, which include val.absolute_name() too among others,
   * as opposed to sh_name[] only; however m_mq might get hosed during this call and become null; a lock would be
   * required; not worth it. */
  return
    os << "SIO["
       << val.nickname() << "]@" << static_cast<const void*>(&val) << " sh_name[" << val.absolute_name() << ']';
}

} // namespace ipc::transport::sync_io
