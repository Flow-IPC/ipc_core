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
#include "ipc/transport/sync_io/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/native_socket_stream.hpp"

namespace ipc::transport::sync_io
{

// Native_socket_stream_impl implementations (::rcv_*() and receive-API methods only).

bool Native_socket_stream_impl::start_receive_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_ops<Op::S_RCV>(std::move(ev_wait_func));
  /* We are a public API impl; but we don't try any I/O; so m_rcv_pending_err_code cannot become truthy.
   * So no need to check for that / log_stats() (see its doc header for background as to what we mean here). */
}

bool Native_socket_stream_impl::start_receive_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_receive_native_handle_ops(std::move(ev_wait_func));
}

bool Native_socket_stream_impl::async_receive_native_handle(Native_handle* target_hndl,
                                                            const util::Blob_mutable& target_meta_blob,
                                                            Error_code* sync_err_code, size_t* sync_sz,
                                                            flow::async::Task_asio_err_sz&& on_done_func)
{
  assert(!on_done_func.empty());

  /* Subtlety: async_receive_blob() will pass target_hndl==nullptr to _impl(); but we do not allow this.
   * When using us as a Native_handle_receiver they must be ready to receive a Native_handle, even if the other
   * side chooses to not send one (then *target_hndl shall be set to equal Native_handle{}, a null handle).
   *
   * So now internally our code can tell whether this is from a Blob_receiver or a Native_handle_receiver role
   * and emit error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB if target_hndl==0, yet the other side sent an actual handle. */
  assert(target_hndl && "Native_socket_stream::async_receive_native_handle() must take non-null Native_handle ptr.");

  return async_receive_impl(target_hndl, target_meta_blob, sync_err_code, sync_sz,
                            std::move(on_done_func));
}

bool Native_socket_stream_impl::async_receive_blob(const util::Blob_mutable& target_blob,
                                                   Error_code* sync_err_code, size_t* sync_sz,
                                                   flow::async::Task_asio_err_sz&& on_done_func)
{
  assert(!on_done_func.empty());
  return async_receive_impl(nullptr, target_blob, sync_err_code, sync_sz, std::move(on_done_func));
}

bool Native_socket_stream_impl::async_receive_impl(Native_handle* target_hndl_or_null,
                                                   const util::Blob_mutable& target_meta_blob,
                                                   Error_code* err_code, size_t* sync_sz,
                                                   flow::async::Task_asio_err_sz&& on_done_func)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool,
                                     async_receive_impl,
                                     target_hndl_or_null, target_meta_blob, _1, sync_sz, std::move(on_done_func));
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.
  auto& sync_err_code = *err_code;

  if ((!op_started<Op::S_RCV>("async_receive_impl()"))
      || (!state_peer("async_receive_impl()")))
  {
    return false;
  }
  // else

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  if (m_rcv_user_request || m_rcv_user_batch_request)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Async-receive requested, but the preceding such request "
                     "is still in progress; the message has not arrived yet.  "
                     "Likely a user error, but who are we to judge?  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: User async-receive request for "
                 "possible native handle and meta-blob (located @ [" << target_meta_blob.data() << "] of "
                 "max size [" << target_meta_blob.size() << "]).");

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request for "
                     "possible native handle and meta-blob (located @ [" << target_meta_blob.data() << "] of "
                     "max size [" << target_meta_blob.size() << "]): Error already encountered earlier.  Emitting "
                     "via sync-args.");

    sync_err_code = m_rcv_pending_err_code;
    *sync_sz = 0;
  }
  else
  {
    async_receive_core(target_hndl_or_null, false, target_meta_blob, &sync_err_code, sync_sz, std::move(on_done_func));
    // ^-- on_done_func() won't be called in there; that would be later (if ever) and only if we hit would-block now.

    // See log_stats() doc header for basic background behind the logic here.
    if (m_rcv_pending_err_code) // Note we wouldn't be in this branch had it been already truthy at the start.
    {
      rcv_log_stats("async_receive_impl(): while sync-processing rcv-pipe hosed");
    }
  }

  return true;
} // Native_socket_stream_impl::async_receive_impl()

void Native_socket_stream_impl::async_receive_core(Native_handle* target_hndl_or_null,
                                                   bool assume_would_block,
                                                   const util::Blob_mutable& target_meta_blob,
                                                   Error_code* sync_err_code, size_t* sync_sz,
                                                   flow::async::Task_asio_err_sz&& on_done_func_or_none)
{
  assert(sync_err_code);

  /* Background can be found by following the comment on this concept constant (S_BLOB_UNDERFLOW_ALLOWED).
   * In this context what is interesting for us, though, is that its value is `true`.  Let's explore:
   * If we operate with SOCK_SEQPACKET semantics then:
   *   - When we make the OS-read call that shall read into m_rcv_user_request->m_target_meta_blob, we do *not*
   *     know the length of what will be returned; but the OS will be able to detect if a particular in-dgram
   *     is too long, and our API(s) wrapping this will emit MESSAGE_SIZE_EXCEEDS_USER_STORAGE directly in
   *     asio_local_stream_socket-land (not here in *this).  (It is detectable in Linux via the MSG_TRUNC out-flag.
   *     See nb_read_some_with_native_handle() doc header for details.)
   *   - So we rely on nb_read_some_with_native_handle(), etc., to throw the aforementioned civilized error
   *     as needed, upon the OS-call failing due to having to truncate an in-dgram.
   *
   * However if we operate with SOCK_STREAM (which is more typical/portable but has inferiorities in other ways):
   *   - When we make that OS-read call, we do know the length and read just that many bytes (we have the length
   *     from a preceding OS-read call that obtained the length frame-header).
   *   - So we can throw a civilized error right then, before attempting any OS-read call into the buffer; as
   *     by then we know the length in the in-message *and* the buffer's length m_target_meta_blob.size().
   *
   * That's the background, but the practical result is simply that we don't do any pre-check here;
   * if an in-dgram or in-message is too long, we'll throw the appropriate error at that time, first detecting
   * the situation via OS or ourselves respectively.
   *
   * Note that in some cases, as of this writing, higher layers (namely struc::Channel) might rely on the ability
   * to intentionally provide smaller-than-MAX_META_BLOB_LENGTH in-buffers (so as to not need to allocate larger
   * buffers which can be costlier in some cases depending on the allocator used), when both sides have agreed on
   * a smaller-than-MAX_META_BLOB_LENGTH size limit.  So through the protocol above ours, they guarantee no overflow;
   * but if they (or someone else using us) messes it up, we will throw the necessary error at that time. */

  static_assert(Native_socket_stream::S_BLOB_UNDERFLOW_ALLOWED,
                "See large-ish comment above this place in the code.");

  m_rcv_user_request.emplace();
  m_rcv_user_request->m_target_hndl_ptr = target_hndl_or_null;
  m_rcv_user_request->m_target_meta_blob = target_meta_blob;
  if (!on_done_func_or_none.empty())
  {
    m_rcv_user_request->m_on_done_func = std::move(on_done_func_or_none);
  }
  // else { We must be part of the async_receive_batch_emulation() code path. }

  if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
  {
    rcv_read_msg_from_pkt_stream(assume_would_block, sync_err_code, sync_sz);
  }
  else // if constexpr(!USE_OS_DGRAM_SUPPORT)
  {
    if (m_rcv_resume_incomplete_msg_processing_func.empty())
    {
      // This is the mainstream path.
      rcv_read_msg_from_byte_stream(assume_would_block, sync_err_code, sync_sz);
    }
    else
    {
      // Not an error but rare and interesting: INFO log-level.
      FLOW_LOG_INFO("Socket stream [" << *this << "]: User async-receive request for "
                    "possible native handle and meta-blob (located @ [" << target_meta_blob.data() << "] of "
                    "max size [" << target_meta_blob.size() << "]): Encountered corner-case situation wherein "
                    "a batch-receive-emulation op earlier encountered a would-block in the middle of "
                    "reading a message having already received 1+ complete in-messages; which meant it had to "
                    "finish the batch-receive op and leave the state machine to resume at that point in the next "
                    "async-receive op (that is we).  Executing the steps to place the state machine in that "
                    "state again and resume.");

      /* Pre-nullify m_rcv_resume_incomplete_msg_processing_func just in case the saved function will itself need to set
       * m_rcv_resume_incomplete_msg_processing_func again. */
      const auto resume_func = std::move(m_rcv_resume_incomplete_msg_processing_func);
      m_rcv_resume_incomplete_msg_processing_func.clear(); // Just in case the move-assign didn't do it.

      /* This can be thought of as rcv_read_msg_from_byte_stream()... but it "fast-forwards" through part of the
       * first in-message and resumes from after that point.
       * @todo In this corner case we just ignore assume_would_block being true, if it's true.  The idea is to do it
       * for perf when user knows there's would-block; so this would at worst waste some cycles/whatever but get the
       * right result.  It is OK in this corner case of a corner case, but for full correctness it shouldn't be ignored;
       * it's just fairly painful (as is everything to do with this resume-func corner case really) for
       * m_rcv_resume_incomplete_msg_processing_func to take and propagate assume_would_block to its innards. */
      resume_func(sync_err_code, sync_sz);
    }
  } // else // if constexpr(!USE_OS_DGRAM_SUPPORT)

  if (m_rcv_user_request->m_on_done_func.empty() // Would-block ends op, if no on-done func provided (as advertised).
      || (*sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK)) // Success or any other error always ends op.
  {
    FLOW_LOG_TRACE("Async-request completed synchronously (result "
                   "[" << *sync_err_code << "] [" << sync_err_code->message() << "]); emitting synchronously and "
                   "disregarding handler.");
    m_rcv_user_request.reset(); // Might be a no-op.
  }
  // else { Other stuff logged enough. }
} // Native_socket_stream_impl::async_receive_core()

bool Native_socket_stream_impl::idle_timer_run(util::Fine_duration timeout)
{
  using util::Fine_duration;
  using util::Task;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert(timeout.count() > 0);

  if ((!op_started<Op::S_RCV>("idle_timer_run()")) || (!state_peer("idle_timer_run()")))
  {
    return false;
  }
  // else

  // According to concept requirements we shall no-op/return false if duplicately called.
  if (m_rcv_idle_timeout != Fine_duration::zero())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wants to start idle timer, but they have already "
                     "started it before.  Therefore ignoring.");
    return false;
  }
  // else

  m_rcv_idle_timeout = timeout; // Remember this, both as flag (non-zero()) and to know to when to schedule it.
  // Now we will definitely return true (even if an error is already pending).  This matches concept requirements.

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_INFO("Socket stream [" << *this << "]: User wants to start idle timer, but an error has already "
                  "been found and emitted earlier.  It's moot; ignoring.");
    return true;
  }
  // else

  FLOW_LOG_INFO("Socket stream [" << *this << "]: User wants to start idle-timer with timeout "
                "[" << round<milliseconds>(m_rcv_idle_timeout) << "].  Scheduling (will be rescheduled as needed).");

  /* Per requirements in concept, start it now; reschedule similarly each time there is activity.
   *
   * The mechanics of timer-scheduling are identical to those in auto_ping() and are explained there;
   * keeping comments light here. */

  m_rcv_ev_wait_func(&m_rcv_ev_wait_hndl_idle_timer_fired_peer,
                     false, // Wait for read.
                     boost::make_shared<Task>
                       ([this]() { rcv_on_ev_idle_timer_fired(); }));

  m_rcv_idle_timer.expires_after(m_rcv_idle_timeout);
  m_timer_worker.timer_async_wait(&m_rcv_idle_timer, m_rcv_idle_timer_fired_peer);

  /* We are a public API impl; but we don't try any I/O; so m_rcv_pending_err_code cannot become truthy.
   * So no need to check for that / log_stats() (see its doc header for background as to what we mean here). */

  return true;
} // Native_socket_stream_impl::idle_timer_run()

void Native_socket_stream_impl::rcv_on_ev_idle_timer_fired()
{
  /* This is an event handler!  Specifically for the *m_rcv_idle_timer_fired_peer pipe reader being
   * readable.  To avoid infinite-loopiness, we'd best pop the thing that was written there. */
  m_timer_worker.consume_timer_firing_signal(m_rcv_idle_timer_fired_peer);

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Idle timer fired: There's been 0 traffic past idle timeout.  "
                   "However an error has already been found and emitted earlier.  Therefore ignoring.");
    return;
  }
  // else

  m_rcv_pending_err_code = error::Code::S_RECEIVER_IDLE_TIMEOUT;
  ++m_rcv_stats.m_idle_timeouts;

  FLOW_LOG_WARNING("Socket stream [" << *this << "]: Idle timer fired: There's been 0 traffic past idle timeout.  "
                   "Will not proceed with any further low-level receiving.  If a user async-receive request is "
                   "pending (is it? = [" << (m_rcv_user_request || m_rcv_user_batch_request) << "]) "
                   "will emit to completion handler.");

  /* See log_stats() doc header for basic background behind the logic here.
   * Note we put this ahead of any handler-call to avoid reentrant hellishness. */
  rcv_log_stats("rcv_on_ev_idle_timer_fired(): idle timeout fired, rcv-pipe hosed");

  if (m_rcv_user_request)
  {
    assert(!m_rcv_user_batch_request);
    // Prevent stepping on our own toes: move/clear it first / invoke handler second.
    const auto on_done_func = std::move(m_rcv_user_request->m_on_done_func);
    m_rcv_user_request.reset();

    assert((!on_done_func.empty())
           && "Empty m_on_done_func => that m_rcv_user_request should have been nullified before returning "
                "from the async-receive API, hence before the timer fired and we were called.  Bug.");

    on_done_func(m_rcv_pending_err_code, 0);
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
  }
  else if (m_rcv_user_batch_request)
  {
    assert(!m_rcv_user_request);
    const auto on_done_func = std::move(m_rcv_user_batch_request->m_on_done_func);
    m_rcv_user_batch_request.reset();
    assert((!on_done_func.empty())
           && "Empty m_on_done_func => that m_rcv_user_batch_request should have been nullified before returning "
                "from the async-receive API, hence before the timer fired and we were called.  Bug.");
    on_done_func(m_rcv_pending_err_code);
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
  }

  /* That's it.  If m_rcv_user_request (we've just nullified it) then async read chain will be finished forever as
   * soon as the user informs us of readability (if it ever does) -- we will detect there's an error
   * in m_rcv_pending_err_code already (and hence no m_rcv_user_request).
   * Or same deal with m_rcv_user_batch_request. */
} // Native_socket_stream_impl::rcv_on_ev_idle_timer_fired()

void Native_socket_stream_impl::rcv_not_idle()
{
  using util::Fine_duration;

  if (m_rcv_idle_timeout == Fine_duration::zero())
  {
    return;
  }
  // else

  /* idle_timer_run() has enabled the idle timer feature, and we've been called indicating we just read something,
   * and therefore it is time to reschedule the idle timer. */

  const auto n_canceled = m_rcv_idle_timer.expires_after(m_rcv_idle_timeout);

  if (n_canceled == 0)
  {
    // This is a fun, rare coincidence that is worth an INFO message.
    FLOW_LOG_INFO("Socket stream [" << *this << "]: Finished reading a message, which means "
                  "we just received traffic, which means the idle timer should be rescheduled.  However "
                  "when trying to reschedule it, we found we were too late: it was very recently queued to "
                  "be invoked in the near future.  An idle timeout error shall be emitted very soon.");
  }
  else // if (n_canceled >= 1)
  {
    assert((n_canceled == 1) && "We only issue 1 timer async_wait() at a time.");

    /* m_timer_worker will m_rcv_idle_timer.async_wait(F), where F() will signal through pipe,
     * making *m_rcv_idle_timer_fired_peer readable.  We've already used m_rcv_ev_wait_func() to start
     * wait on it being readable and invoke rcv_on_ev_idle_timer_fired() in that case; but we've
     * canceled the previous .async_wait() that would make it readable; so just redo that part. */
    m_timer_worker.timer_async_wait(&m_rcv_idle_timer, m_rcv_idle_timer_fired_peer);
  }
} // Native_socket_stream_impl::rcv_not_idle()

void Native_socket_stream_impl::rcv_read_msg_from_pkt_stream(bool assume_would_block,
                                                             Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using util::Blob_mutable;
  using flow::util::Lock_guard;

  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(!m_rcv_pending_err_code);

  if (assume_would_block)
  {
    rcv_read_msg_from_stream_having_assumed_would_block(sync_err_code, sync_sz);
    return;
  }
  // else:

  const bool proto_negotiating
    = m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-receive: "
                 "Trying nb-read of in-dgram synchronously; proto-negotiating? = [" << proto_negotiating << "].");

  Native_handle target_hndl; // Target this even if target_hndl_or_null is null (to check for a certain error).
  const auto n_rcvd_or_zero
    = rcv_nb_read_low_lvl_payload_from_pkt_stream
        (&target_hndl,
         Blob_mutable{&m_rcv_target_meta_length, sizeof(m_rcv_target_meta_length)},
         proto_negotiating ? Blob_mutable{}
                           : m_rcv_user_request->m_target_meta_blob,
         &m_rcv_pending_err_code);
  if (!m_rcv_pending_err_code)
  {
    if (n_rcvd_or_zero != 0)
    {
      FLOW_LOG_TRACE("Got in-dgram.");

      rcv_on_dgram(target_hndl, n_rcvd_or_zero, sync_err_code, sync_sz);
      return;
    }
    // else

    if (m_rcv_user_request->m_on_done_func.empty())
    {
      FLOW_LOG_TRACE("Got nothing but would-block.  This is part of a sub-op (as of this writing when emulating "
                     "batch-receive using single-receives) in which would-block means we end the op and do *not* "
                     "await readability; so we emit would-block and stop the overall receive op.");
      // This is like the below case minus the considerably more complex steps of continuing the overall op.
      *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      *sync_sz = 0;
      return;
    }
    // else

    FLOW_LOG_TRACE("Got nothing but would-block.  Awaiting readability.");

    /* Conceptually we'd like to do m_peer_socket->async_wait(readable, F), where F() would perform
     * rcv_nb_read_low_lvl_payload_from_pkt_stream() (nb-receive over m_peer_socket).  However this is the sync_io
     * pattern, so the user will be performing the conceptual async_wait() for us.  We must ask them to do so
     * via m_rcv_ev_wait_func(), giving them m_peer_socket's FD -- m_ev_wait_hndl_peer_socket -- to wait-on. */

    // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see their docs).
    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once readable do this:
                           boost::make_shared<Task>([this]()
        {
          rcv_on_ev_peer_socket_pkt_stream_readable_or_error();
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request: "
                     "was about to await readability but discovered opposite-direction socket-hosing error; "
                     "emitting error via completion handler (or via sync-args).");

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (!m_rcv_pending_err_code) (but may have become truthy inside, and has unless we `return`ed inside)

  assert(m_rcv_pending_err_code);

  FLOW_LOG_TRACE("Nb-read, or async-read following nb-read encountering would-block, detected error (details "
                 "above); will emit via completion handler (or via sync-args).  Most errors hose the pipe "
                 "in both directions; however as of this writing MESSAGE_SIZE_EXCEEDS_USER_STORAGE "
                 "hoses only in in-direction while the out-direction pipe is usable.");

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_read_msg_from_pkt_stream()

void Native_socket_stream_impl::rcv_on_dgram(Native_handle hndl_or_null, size_t n_rcvd,
                                             Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Blob_mutable;
  using util::Task;

  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.  Anyway... bug; we should never be called because "
              "!USE_OS_DGRAM_SUPPORT.");
  assert(m_rcv_user_request);
  assert(!m_rcv_pending_err_code);
  assert(n_rcvd != 0);

  bool proto_negotiating
    = m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN;

  if (proto_negotiating && (!hndl_or_null.null()))
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Expecting protocol-negotiation (first) in-dgram "
                     "to contain *only* a meta-blob: but received Native_handle is non-null which is "
                     "unexpected; emitting error via completion handler (or via sync-args).");
#ifndef NDEBUG
    const bool ok =
#endif
    m_protocol_negotiator.compute_negotiated_proto_ver(Protocol_negotiator::S_VER_UNKNOWN, &m_rcv_pending_err_code);
    assert(ok && "Protocol_negotiator breaking contract?  Bug?");
    assert(m_rcv_pending_err_code
           && "Protocol_negotiator should have emitted error given intentionally bad version.");
  }
  else if ((!hndl_or_null.null()) && (!m_rcv_user_request->m_target_hndl_ptr))
       // && (!proto_negotiating)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request for "
                     "*only* a meta-blob: but received Native_handle is non-null which is "
                     "unexpected; emitting error via completion handler (or via sync-args).");
    m_rcv_pending_err_code = error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB;
  } // if (hndl_or_null && (!m_rcv_user_request->m_target_hndl_ptr))
  else // if (no prob with hndl_or_null or m_target_hndl_ptr)
  {
    // Finalize the user's Native_handle target variable if applicable.
    if (m_rcv_user_request->m_target_hndl_ptr
        // If proto_negotiating, hndl_or_null is null; and anyway m_target_hndl_ptr is not yet in play.
        && (!proto_negotiating))
    {
      *m_rcv_user_request->m_target_hndl_ptr = hndl_or_null;
    }

    /* Decode the situation, which mainly flows from m_rcv_target_meta_length, though despite the name in
     * our (dgram-based) case it is actually:
     *   - (if proto_negotiating) Protocol_negotiator-consumed value; or
     *   - (otherwise) basically an enum indicating message type.
     * (Re-recommend here to read ### Protocol with `Protocol_pkt_stream` (OS maintains meessage boundaries) ###
     * in class doc header.)
     *
     * In any case m_rcv_target_meta_length must have been fully received; so check that first. */

    if (n_rcvd < sizeof(m_rcv_target_meta_length))
    {
      FLOW_LOG_WARNING("Socket stream [" << *this << "]: Received in-dgram contains invalid header (opposing "
                       "side misbehaved/bug?): header length is [" << sizeof(m_rcv_target_meta_length) << "]; "
                       "but the entire in-dgram has size only [" << n_rcvd << "]; "
                       "emitting error via completion handler (or via sync-args).");
      m_rcv_pending_err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
    }
    else // if (n_rcvd >= sizeof(m_rcv_target_meta_length))
    {
      const bool no_data = n_rcvd == sizeof(m_rcv_target_meta_length);

      if (proto_negotiating)
      {
        if (!no_data)
        {
          FLOW_LOG_WARNING("Socket stream [" << *this << "]: Expecting protocol-negotiation (first) in-dgram "
                           "to contain *only* the header: but received more bytes "
                           "([" << (n_rcvd - sizeof(m_rcv_target_meta_length)) << "] on top of header); "
                           "unexpected; emitting error via completion handler (or via sync-args).");
          m_rcv_pending_err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
        }
        else // if (no_data) // && proto_negotiating
        {
          /* Protocol_negotiator handles everything (invalid value, incompatible range...); we just know
           * the encoding is to shove the version number into what is normally the length field. */
#ifndef NDEBUG
          const bool ok =
#endif
          m_protocol_negotiator.compute_negotiated_proto_ver
            (static_cast<Protocol_negotiator::proto_ver_t>(m_rcv_target_meta_length),
             &m_rcv_pending_err_code);
          assert(ok && "Protocol_negotiator breaking contract?  Bug?");
          proto_negotiating = false; // Just in case (maintainability).

          if (!m_rcv_pending_err_code)
          {
            // Succeeded; do what we'd do due to, say, receiving auto-ping below.

            FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of negotiation payload; passed.  "
                           "Ignoring other than registering non-idle activity.  Proceeding with the next dgram read.");

            rcv_not_idle(); // Register activity <= end of complete message, no error.

            m_rcv_stats.m_total_low_lvl_bytes += n_rcvd;
            rcv_read_msg_from_pkt_stream(false, sync_err_code, sync_sz);
            return;
          }
          // else if (m_rcv_pending_err_code) { Fall through as for various other error cases above and below. }
        } // else if (!no_data) // && proto_negotiating
      } // if (proto_negotiating)
      else // if (!proto_negotiating) // Typical path.
      {
        if (m_rcv_target_meta_length == 0)
        {
          if (!no_data)
          {
            sync_err_code->clear();
            *sync_sz = (n_rcvd - sizeof(m_rcv_target_meta_length));

            FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received in-dgram with user message sized "
                           "[" << *sync_sz << "].  Will register non-idle activity; "
                           "and invoke handler (or report via sync-args).");

            rcv_not_idle(); // Register activity <= end of complete message, no error.

            ++m_rcv_stats.m_total_msgs;
            m_rcv_stats.m_total_bytes += *sync_sz;
            m_rcv_stats.m_total_low_lvl_bytes += n_rcvd;
            m_rcv_stats.m_histo_payload_sz.record_value(*sync_sz);
            if (!hndl_or_null.null()) { ++m_rcv_stats.m_msgs_with_hndls; }
            return;
          }
          // else
          if (no_data && (!hndl_or_null.null()))
          {
            sync_err_code->clear();
            *sync_sz = 0;

            FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received in-dgram; header=0 + non-null handle + "
                           "no data => handle received with no meta-blob.  Will register non-idle activity; "
                           "and invoke handler (success).");

            rcv_not_idle(); // Register activity <= end of complete message, no error.

            ++m_rcv_stats.m_total_msgs;
            m_rcv_stats.m_total_low_lvl_bytes += n_rcvd;
            m_rcv_stats.m_histo_payload_sz.record_value(0); // Handle-only: 0-byte user payload.
            ++m_rcv_stats.m_msgs_with_hndls; // Always a handle here (that's the point).
            return;
          }
          // else if (no_data && hndl_or_null.null()):

          // Once per connection at most, so INFO log level is OK.
          FLOW_LOG_INFO("Socket stream [" << *this << "]: Received in-dgram: Graceful-close-of-incoming-pipe "
                        "message.  Will not proceed with any further low-level receiving.  "
                        "Will invoke handler (graceful-close error).");
          m_rcv_pending_err_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE;

          m_rcv_stats.m_total_low_lvl_bytes += n_rcvd;
        } // if (m_rcv_target_meta_length == 0)
        else if (m_rcv_target_meta_length == Native_socket_stream_cfg::S_PING_SENTINEL)
        {
          if (no_data)
          {
            FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received in-dgram; header "
                           "contains special value indicating a ping.  Ignoring other than registering non-idle "
                           "activity.  Proceeding with the next dgram read.");

            rcv_not_idle(); // Register activity <= end of complete message, no error.

            ++m_rcv_stats.m_auto_pings;
            m_rcv_stats.m_total_low_lvl_bytes += n_rcvd;
            rcv_read_msg_from_pkt_stream(false, sync_err_code, sync_sz);
            return;
          }
          // else if (!no_data):
          FLOW_LOG_WARNING("Socket stream [" << *this << "]: Received in-dgram contains invalid header (opposing "
                           "side misbehaved/bug?): it contains PING_SENTINEL, but a ping shall contain no "
                           "further data, yet we received more bytes "
                           "([" << (n_rcvd - sizeof(m_rcv_target_meta_length)) << "] on top of header); "
                           "emitting error via completion handler (or via sync-args).");
          m_rcv_pending_err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
        } // else if (m_rcv_target_meta_length == PING)
        else // if (m_rcv_target_meta_length != 0 or PING)
        {
          FLOW_LOG_WARNING("Socket stream [" << *this << "]: Received in-dgram contains invalid header (opposing "
                           "side misbehaved/bug?): it contains the value [" << m_rcv_target_meta_length << " "
                           "(decimal)] but only the values 0 and [" << Native_socket_stream_cfg::S_PING_SENTINEL << "] "
                           "are allowed; we also possibly received more bytes "
                           "([" << (n_rcvd - sizeof(m_rcv_target_meta_length)) << "] on top of header); "
                           "emitting error via completion handler (or via sync-args).");
          m_rcv_pending_err_code = error::Code::S_LOW_LVL_INTERNAL_PROTOCOL_INVALID_HEADER;
        } // else if (m_rcv_target_meta_length != 0 or PING)
      } // else if (!proto_negotiating)
    } // else if (n_rcvd >= sizeof(m_rcv_target_meta_length))
  } // else if (no prob with hndl_or_null or m_target_hndl_ptr)

  assert(m_rcv_pending_err_code);

  // WARNINGs above are enough; no TRACE here.

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_on_dgram()

void Native_socket_stream_impl::rcv_on_ev_peer_socket_pkt_stream_readable_or_error()
{
  /* Our task is the USE_OS_DGRAM_SUPPORT version of rcv_on_ev_peer_socket_byte_stream_readable_or_error()
   * but much simpler: The would-block (which has now been resolved) can only by definition occur between
   * in-dgrams and not partway through one.  So basically we just do
   *   rcv_read_msg_from_pkt_stream()
   * again, with some pre-checks (same as in that other guy) as-to something having happened during the wait +
   * invoking the on-done handler on successful in-message read. */

  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User's wait-for-readable finished (readable or error, "
                     "we do not know which yet); would resume processing depending on what we were doing before; "
                     "however an error was detected in the meantime (as of this writing: idle timeout).  "
                     "Stopping read chain.");
    assert((!m_rcv_user_request)
           && "If rcv-error emitted during low-level async-wait, we should have fed it to any pending async-receive.");
    return;
  }
  // else

  assert(m_rcv_user_request);

  // Will potentially emit these (if and only if message-read completes due to this successful async-wait).
  Error_code sync_err_code;
  size_t sync_sz;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: User-performed wait-for-readable finished (readable or error, "
                 "we do not know which yet).  Retrying to resume read chain.");

  rcv_read_msg_from_pkt_stream(false, &sync_err_code, &sync_sz);

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Another async-wait is pending now.  We've logged enough.  Live to fight another day.
    return;
  }
  // else

  /* See log_stats() doc header for basic background behind the logic here.
   * Note we put this ahead of any handler-call to avoid reentrant hellishness. */
  if (m_rcv_pending_err_code) // Note we would've returned already had it been already truthy at the start.
  {
    rcv_log_stats("rcv_on_ev_peer_socket_pkt_stream_readable_or_error(): while processing ev-ready rcv-pipe hosed");
  }

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-op result ready after successful async-wait.  "
                 "Executing handler now.");

  // Prevent stepping on our own toes: move/clear it first / invoke handler second.
  const auto on_done_func = std::move(m_rcv_user_request->m_on_done_func);
  m_rcv_user_request.reset();

  assert((!on_done_func.empty())
         && "Empty m_on_done_func => that m_rcv_user_request should have been nullified without issuing async-wait, "
              "so we should not have been called.  Bug.");

  on_done_func(sync_err_code, sync_sz);
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
} // Native_socket_stream_impl::rcv_on_ev_peer_socket_pkt_stream_readable_or_error()

void Native_socket_stream_impl::rcv_read_msg_from_byte_stream(bool assume_would_block,
                                                              Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using util::Blob_mutable;
  using flow::util::Lock_guard;

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-receive: Start of payload 1: "
                 "Trying nb-read of payload 1 (handle if any, meta-blob-length) synchronously.");
  assert(!m_rcv_pending_err_code);

  if (assume_would_block)
  {
    rcv_read_msg_from_stream_having_assumed_would_block(sync_err_code, sync_sz);
    return;
  }
  // else:

  Native_handle target_hndl; // Target this even if target_hndl_or_null is null (to check for a certain error).
  const auto n_rcvd_or_zero
    = rcv_nb_read_low_lvl_payload_from_byte_stream(&target_hndl,
                                                   Blob_mutable{&m_rcv_target_meta_length,
                                                                sizeof(m_rcv_target_meta_length)},
                                                   &m_rcv_pending_err_code);
  if (!m_rcv_pending_err_code)
  {
    if (n_rcvd_or_zero != 0)
    {
      FLOW_LOG_TRACE("Got some or all of payload 1.");
      rcv_on_handle_finalized(target_hndl, n_rcvd_or_zero, sync_err_code, sync_sz);
      return;
    }
    // else

    if (m_rcv_user_request->m_on_done_func.empty())
    {
      FLOW_LOG_TRACE("Got nothing but would-block.  This is part of a sub-op (as of this writing when emulating "
                     "batch-receive using single-receives) in which would-block means we end the op and do *not* "
                     "await readability; so we emit would-block and stop the overall receive op.");
      // This is like the below case minus the considerably more complex steps of continuing the overall op.
      *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      *sync_sz = 0;
      return;
    }
    // else

    FLOW_LOG_TRACE("Got nothing but would-block.  Awaiting readability.");

    /* Conceptually we'd like to do m_peer_socket->async_wait(readable, F), where F() would perform
     * rcv_nb_read_low_lvl_payload_from_byte_stream() (nb-receive over m_peer_socket).  However this is the sync_io
     * pattern, so the user will be performing the conceptual async_wait() for us.  We must ask them to do so
     * via m_rcv_ev_wait_func(), giving them m_peer_socket's FD -- m_ev_wait_hndl_peer_socket -- to wait-on. */

    // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see their docs).
    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once readable do this:
                           boost::make_shared<Task>([this]()
        {
          rcv_on_ev_peer_socket_byte_stream_readable_or_error(Rcv_msg_state::S_MSG_START,
                                                              0 /* ignored for S_MSG_START */);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request: "
                     "was about to await readability but discovered opposite-direction socket-hosing error; "
                     "emitting error via completion handler (or via sync-args).");

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (!m_rcv_pending_err_code) (but may have become truthy inside, and has unless we `return`ed inside)

  assert(m_rcv_pending_err_code);

  FLOW_LOG_TRACE("Nb-read, or async-read following nb-read encountering would-block, detected error (details "
                 "above); will emit via completion handler (or via sync-args).");

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_read_msg_from_byte_stream()

void Native_socket_stream_impl::rcv_on_handle_finalized(Native_handle hndl_or_null, size_t n_rcvd,
                                                        Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Blob_mutable;
  using util::Task;
  using flow::util::Lock_guard;

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(m_rcv_user_request);
  assert(!m_rcv_pending_err_code);
  assert(n_rcvd != 0);

  const bool proto_negotiating
    = m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN;

  if (proto_negotiating && (!hndl_or_null.null()))
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Expecting protocol-negotiation (first) in-message "
                     "to contain *only* a meta-blob: but received Native_handle is non-null which is "
                     "unexpected; emitting error via completion handler (or via sync-args).");
#ifndef NDEBUG
    const bool ok =
#endif
    m_protocol_negotiator.compute_negotiated_proto_ver(Protocol_negotiator::S_VER_UNKNOWN, &m_rcv_pending_err_code);
    assert(ok && "Protocol_negotiator breaking contract?  Bug?");
    assert(m_rcv_pending_err_code
           && "Protocol_negotiator should have emitted error given intentionally bad version.");
  }
  else if ((!hndl_or_null.null()) && (!m_rcv_user_request->m_target_hndl_ptr))
       // && (!proto_negotiating)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request for "
                     "*only* a meta-blob: but received Native_handle is non-null which is "
                     "unexpected; emitting error via completion handler (or via sync-args).");
    m_rcv_pending_err_code = error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB;
  } // if (hndl_or_null && (!m_rcv_user_request->m_target_hndl_ptr))
  else // if (no prob with hndl_or_null or m_target_hndl_ptr)
  {
    // Finalize the user's Native_handle target variable if applicable.
    if (m_rcv_user_request->m_target_hndl_ptr
        // If proto_negotiating, hndl_or_null is null; and anyway m_target_hndl_ptr is not yet in play.
        && (!proto_negotiating))
    {
      *m_rcv_user_request->m_target_hndl_ptr = hndl_or_null;
    }

    if (n_rcvd == sizeof(m_rcv_target_meta_length))
    {
      // Got the entire payload 1, not just some of it including handle-if-any.
      rcv_on_head_payload(sync_err_code, sync_sz);
      return;
    }
    // else

    // Still have to finish reading into m_rcv_target_meta_length.  We've already hit would-block though.
    if (m_rcv_user_request->m_on_done_func.empty())
    {
      // Corner case.
      FLOW_LOG_TRACE("Got would-block after only partially reading the meta-length/msg-type from the byte stream.  "
                     "This is part of a sub-op (as of this writing when emulating "
                     "batch-receive using single-receives) in which would-block means we end the op and do *not* "
                     "await readability; so we emit would-block and stop the overall receive op.  However saving "
                     "the partial-message info, and any subsequent receive-op will resume from here.");

      *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      *sync_sz = 0;

      /* To resume the state machine below, we have to in a sense "replay" what has happened so far this time around
       * (which we shall abandon this time around).  To do this sufficiently succinctly/buglessly it is hugely helpful
       * to avoid various branches/corner cases w/r/t what may have happened for this incomplete in-message so far.
       * This is helpful at least: */
      assert((!proto_negotiating) && "This corner case should only happen after already banking 1+ user in-message, "
                                       "so protocol negotiation should have already succeeded.");
      assert(m_rcv_resume_incomplete_msg_processing_func.empty()
             && "This should have been nullified at the start of the async-receive.");

      m_rcv_resume_incomplete_msg_processing_func
        = [this, hndl_or_null,
           target_meta_length_incomplete = m_rcv_target_meta_length,
           n_rcvd]
            (Error_code* err_code, size_t* sz)
      {
        rcv_resume_incomplete_msg_processing(err_code, sz, hndl_or_null, target_meta_length_incomplete, n_rcvd, {});
      };

      return;
    }
    // else if (m_rcv_user_request->m_on_done_func.empty()):

    /* Mainstream case: Await readability; then resume once we think socket readable.  So
     * much like in rcv_read_msg_from_byte_stream() (keeping comments light): */

    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once readable do this:
                           boost::make_shared<Task>([this, n_rcvd]()
        {
          rcv_on_ev_peer_socket_byte_stream_readable_or_error(Rcv_msg_state::S_HEAD_PAYLOAD,
                                                              sizeof(m_rcv_target_meta_length) - n_rcvd);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request: "
                     "was about to await readability but discovered opposite-direction socket-hosing error; "
                     "emitting error via completion handler (or via sync-args).");

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (no prob with hndl_or_null or m_target_hndl_ptr) (but another problem may have occurred inside)

  assert(m_rcv_pending_err_code);

  // WARNINGs above are enough; no TRACE here.

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_on_handle_finalized()

void Native_socket_stream_impl::rcv_on_head_payload(Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Blob_mutable;

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(m_rcv_user_request);
  assert(!m_rcv_pending_err_code);

  bool proto_negotiating
    = m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN;

  if (proto_negotiating)
  {
    /* Protocol_negotiator handles everything (invalid value, incompatible range...); we just know
     * the encoding is to shove the version number into what is normally the length field. */
#ifndef NDEBUG
    const bool ok =
#endif
    m_protocol_negotiator.compute_negotiated_proto_ver
      (static_cast<Protocol_negotiator::proto_ver_t>(m_rcv_target_meta_length),
       &m_rcv_pending_err_code);
    assert(ok && "Protocol_negotiator breaking contract?  Bug?");
    proto_negotiating = false; // Just in case (maintainability).

    if (m_rcv_pending_err_code)
    {
      // Protocol negotiation failed.  Do what we'd do due to, say, graceful-close below.
      *sync_err_code = m_rcv_pending_err_code;
      *sync_sz = 0;
      return;
    }
    // else: Succeeded; do what we'd do due to, say, receiving auto-ping below.

    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of negotiation payload; passed.  "
                   "Ignoring other than registering non-idle activity.  Proceeding with the next message read.");

    rcv_not_idle(); // Register activity <= end of complete message, no error.

    m_rcv_stats.m_total_low_lvl_bytes += sizeof(m_rcv_target_meta_length);
    rcv_read_msg_from_byte_stream(false, sync_err_code, sync_sz);
    return;
  }
  // else if (!proto_negotiating): Normal payload 1 handling.

  if (m_rcv_target_meta_length == Native_socket_stream_cfg::S_PING_SENTINEL)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of payload 1; length prefix "
                   "contains special value indicating a ping.  Ignoring other than registering non-idle "
                   "activity.  Proceeding with the next message read.");

    rcv_not_idle(); // Register activity <= end of complete message, no error.

    ++m_rcv_stats.m_auto_pings;
    m_rcv_stats.m_total_low_lvl_bytes += sizeof(m_rcv_target_meta_length);
    rcv_read_msg_from_byte_stream(false, sync_err_code, sync_sz);
    return;
  }
  // else

  const auto user_target_size = m_rcv_user_request->m_target_meta_blob.size();
  if (m_rcv_target_meta_length != 0) // && (not ping)
  {
    if (m_rcv_target_meta_length <= user_target_size)
    {
      FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of payload 1; length prefix "
                     "[" << m_rcv_target_meta_length <<"] is positive (and not indicative of ping).  "
                     "Reading payload 2.");
      rcv_read_blob(Rcv_msg_state::S_META_BLOB_PAYLOAD,
                    Blob_mutable{m_rcv_user_request->m_target_meta_blob.data(),
                                 size_t{m_rcv_target_meta_length}},
                    sync_err_code, sync_sz);
      return;
    }
    // else if (m_rcv_target_meta_length > user_target_size):

    FLOW_LOG_WARNING("Received all of payload 1; length prefix "
                     "[" << m_rcv_target_meta_length <<"] is positive (and not indicative of ping); "
                     "however it exceeds user target blob size [" << user_target_size << "] and would "
                     "overflow.  Treating similarly to a graceful-close but with a bad error code and "
                     "this warning.  Will not proceed with any further low-level receiving; will invoke "
                     "handler (failure).");
    m_rcv_pending_err_code = error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE;
  }
  else // if (m_rcv_target_meta_length == 0)
  {
    if (m_rcv_user_request->m_target_hndl_ptr && (!m_rcv_user_request->m_target_hndl_ptr->null()))
    {
      FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of payload 1; length 0 + non-null handle => "
                     "handle received with no meta-blob.  Will register non-idle activity; "
                     "and invoke handler (success).");
      // m_rcv_pending_err_code remains falsy.

      rcv_not_idle(); // Register activity <= end of complete message, no error.

      ++m_rcv_stats.m_total_msgs;
      m_rcv_stats.m_total_low_lvl_bytes += sizeof(m_rcv_target_meta_length);
      m_rcv_stats.m_histo_payload_sz.record_value(0); // Handle-only: 0-byte user payload.
      ++m_rcv_stats.m_msgs_with_hndls; // Always a handle here (that's the point).
    }
    else
    {
      // Once per connection at most, so INFO log level is OK.
      FLOW_LOG_INFO("Socket stream [" << *this << "]: User message received: Graceful-close-of-incoming-pipe "
                    "message.  Will not proceed with any further low-level receiving.  "
                    "Will invoke handler (graceful-close error).");
      m_rcv_pending_err_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE;

      m_rcv_stats.m_total_low_lvl_bytes += sizeof(m_rcv_target_meta_length);
    }
  } // else if (m_rcv_target_meta_length == 0)

  *sync_err_code = m_rcv_pending_err_code; // Truthy (graceful-close) or falsy (got handle + no meta-blob).
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_on_head_payload()

void Native_socket_stream_impl::rcv_on_ev_peer_socket_byte_stream_readable_or_error(Rcv_msg_state msg_state,
                                                                                    size_t n_left)
{
  using util::Blob_mutable;

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User's wait-for-readable finished (readable or error, "
                     "we do not know which yet); would resume processing depending on what we were doing before; "
                     "however an error was detected in the meantime (as of this writing: idle timeout).  "
                     "Stopping read chain.");
    assert((!m_rcv_user_request)
           && "If rcv-error emitted during low-level async-wait, we should have fed it to any pending async-receive.");
    return;
  }
  // else

  assert(m_rcv_user_request);

  // Will potentially emit these (if and only if message-read completes due to this successful async-wait).
  Error_code sync_err_code;
  size_t sync_sz;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: User-performed wait-for-readable finished (readable or error, "
                 "we do not know which yet).  Resuming processing depending on what we were doing before.");

  switch (msg_state)
  {
  case Rcv_msg_state::S_MSG_START:
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: In state MSG_START: Reading from byte 0/handle if any.");
    rcv_read_msg_from_byte_stream(false, &sync_err_code, &sync_sz);
    break;

  case Rcv_msg_state::S_HEAD_PAYLOAD:
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: In state HEAD_PAYLOAD: "
                   "Reading meta-length/ping/graceful-close specifier: [" << n_left << "] bytes left.");
    rcv_read_blob(Rcv_msg_state::S_HEAD_PAYLOAD,
                  Blob_mutable{static_cast<uint8_t*>(static_cast<void*>(&m_rcv_target_meta_length))
                                 + sizeof(m_rcv_target_meta_length) - n_left,
                               n_left},
                  &sync_err_code, &sync_sz);
    break;

  case Rcv_msg_state::S_META_BLOB_PAYLOAD:
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: In state META_BLOB_PAYLOAD: "
                   "Reading meta-blob: [" << n_left << "] bytes left.");
    rcv_read_blob(Rcv_msg_state::S_META_BLOB_PAYLOAD,
                  Blob_mutable{static_cast<uint8_t*>(m_rcv_user_request->m_target_meta_blob.data())
                                 + size_t(m_rcv_target_meta_length) - n_left,
                               n_left},
                  &sync_err_code, &sync_sz);
  } // switch (msg_state) (Compiler should catch any missed enum value.)

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Another async-wait is pending now.  We've logged enough.  Live to fight another day.
    return;
  }
  // else

  /* See log_stats() doc header for basic background behind the logic here.
   * Note we put this ahead of any handler-call to avoid reentrant hellishness. */
  if (m_rcv_pending_err_code) // Note we would've returned already had it been already truthy at the start.
  {
    rcv_log_stats("rcv_on_ev_peer_socket_byte_stream_readable_or_error(): while processing ev-ready rcv-pipe hosed");
  }

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-op result ready after successful async-wait.  "
                 "Executing handler now.");

  // Prevent stepping on our own toes: move/clear it first / invoke handler second.
  const auto on_done_func = std::move(m_rcv_user_request->m_on_done_func);
  m_rcv_user_request.reset();

  assert((!on_done_func.empty())
         && "Empty m_on_done_func => that m_rcv_user_request should have been nullified without issuing async-wait, "
              "so we should not have been called.  Bug.");

  on_done_func(sync_err_code, sync_sz);
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
} // Native_socket_stream_impl::rcv_on_ev_peer_socket_byte_stream_readable_or_error()

void Native_socket_stream_impl::rcv_read_blob(Rcv_msg_state msg_state, const util::Blob_mutable& target_blob,
                                              Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using util::Blob_const;
  using flow::util::Lock_guard;
  using flow::util::Blob;

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(!m_rcv_pending_err_code);
  assert(m_rcv_user_request);

  const auto n_rcvd_or_zero = rcv_nb_read_low_lvl_payload_from_byte_stream(nullptr, target_blob,
                                                                           &m_rcv_pending_err_code);
  if (!m_rcv_pending_err_code)
  {
    if (n_rcvd_or_zero == target_blob.size())
    {
      switch (msg_state)
      {
      case Rcv_msg_state::S_HEAD_PAYLOAD:
        rcv_on_head_payload(sync_err_code, sync_sz);
        return;
      case Rcv_msg_state::S_META_BLOB_PAYLOAD:
      {
        FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of payload 2 (meta-blob of length "
                       "[" << m_rcv_target_meta_length << "]).  Will register non-idle activity; "
                       "and invoke handler (or report via sync-args).");

        rcv_not_idle(); // Register activity <= end of complete message, no error.

        const auto len = size_t(m_rcv_target_meta_length);

        assert(!*sync_err_code);
        *sync_sz = len;

        ++m_rcv_stats.m_total_msgs;
        m_rcv_stats.m_total_bytes += len;
        m_rcv_stats.m_total_low_lvl_bytes += (sizeof(m_rcv_target_meta_length) + len);
        m_rcv_stats.m_histo_payload_sz.record_value(len);
        const auto hndl_ptr = m_rcv_user_request->m_target_hndl_ptr;
        if (hndl_ptr && (!hndl_ptr->null())) { ++m_rcv_stats.m_msgs_with_hndls; }
        return;
      }
      case Rcv_msg_state::S_MSG_START:
        assert(false && "rcv_read_blob() shall be used only for S_*_PAYLOAD phases.");
      }
      assert(false && "Should not get here.");
    } // if (n_rcvd_or_zero == target_blob.size())
    // else if (n_rcvd_or_zero != target_blob.size()):

    // Still have to finish reading into target_blob.  We've already hit would-block though.
    if (m_rcv_user_request->m_on_done_func.empty())
    {
      /* Corner case.  They want us to report the would-block synchronously (as normal) but abandon the
       * async-read (async_receive_batch_emulation() code-path only).  So any *following* async-read will
       * need to start the state machine already mid-message. */

      FLOW_LOG_TRACE("Got would-block after only partially reading the requested blob.  "
                     "This is part of a sub-op (as of this writing when emulating "
                     "batch-receive using single-receives) in which would-block means we end the op and do *not* "
                     "await readability; so we emit would-block and stop the overall receive op.  However saving "
                     "the partial-message info, and any subsequent receive-op will resume from here.");

      *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      *sync_sz = 0;

      /* To resume the state machine later, we have to in a sense "replay" what has happened so far this time around
       * (which we shall abandon this time around).  It might help here to look at rcv_on_handle_finalized(), in a
       * similar spot -- but in our case depending on (at least) msg_state we possibly have more to replay than
       * just that.  Anyway rcv_resume_incomplete_msg_processing() will do the right thing; we just have to tell
       * it where we had to stop mid-message. */

      assert((m_protocol_negotiator.negotiated_proto_ver() != Protocol_negotiator::S_VER_UNKNOWN)
             && "This corner case should only happen after already banking 1+ user in-message, "
                  "so protocol negotiation should have already succeeded.");
      assert(m_rcv_resume_incomplete_msg_processing_func.empty()
             && "This should have been nullified at the start of the async-receive.");

      if (msg_state == Rcv_msg_state::S_HEAD_PAYLOAD)
      {
        // We stopped in the middle of reading into m_rcv_target_meta_length.
        m_rcv_resume_incomplete_msg_processing_func
          = [this,
             hndl_or_null = m_rcv_user_request->m_target_hndl_ptr
                              ? *m_rcv_user_request->m_target_hndl_ptr
                              : Native_handle{},
             target_meta_length_incomplete = m_rcv_target_meta_length,
             target_meta_length_incomplete_n_rcvd = sizeof(m_rcv_target_meta_length)
                                                    - target_blob.size() + n_rcvd_or_zero]
              (Error_code* err_code, size_t* sz)
        {
          rcv_resume_incomplete_msg_processing(err_code, sz, hndl_or_null, target_meta_length_incomplete,
                                               target_meta_length_incomplete_n_rcvd, {});
        };

        return;
      } // if (msg_state == HEAD_PAYLOAD)
      // else if (msg_state == META_BLOB_PAYLOAD):

      /* We got all of m_rcv_target_meta_length, but only N (not all m_rcv_target_meta_length) of the following
       * meta-blob.  N may be 0. */

      Blob target_blob_incomplete{get_logger()};
      // This is N.
      const auto target_blob_incomplete_sz = m_rcv_user_request->m_target_meta_blob.size()
                                             - target_blob.size() + n_rcvd_or_zero;
      if (target_blob_incomplete_sz != 0)
      {
        /* Rare (in fact only, as of this writing) receive-direction meta-blob copy.
         * (Well, also, m_rcv_resume_incomplete_msg_processing_func() will copy again -- out of it into the next
         * async-receive's user target buffer.)
         * We expect this entire situation to be quite rare, so the perf impact is likely negligible.
         * There really is no choice; this is the user's buffer, and we're about to report we received no data
         * into it, and the user is allowed to destroy/repurpose that memory area. */
        target_blob_incomplete.assign_copy(Blob_const{m_rcv_user_request->m_target_meta_blob.data(),
                                                      target_blob_incomplete_sz});
      }
      m_rcv_resume_incomplete_msg_processing_func
        = [this,
           hndl_or_null = m_rcv_user_request->m_target_hndl_ptr
                            ? *m_rcv_user_request->m_target_hndl_ptr
                            : Native_handle{},
           target_meta_length = m_rcv_target_meta_length,
           target_blob_incomplete = std::move(target_blob_incomplete)] // Move-capture it (don't copy again).
            (Error_code* err_code, size_t* sz)
      {
        rcv_resume_incomplete_msg_processing(err_code, sz,
                                             hndl_or_null, target_meta_length, 0, target_blob_incomplete);
      };
      return;
    } // if (m_rcv_user_request->m_on_done_func.empty())
    /* else if (!m_rcv_user_request->m_on_done_func.empty()):
     *
     * Mainstream case: Report would-block synchronously (as normal) but don't abandon the async-receive.
     * Await readability; then resume once we think socket readable.  So,
     * much like in rcv_read_msg_from_byte_stream() (keeping comments light): */

    FLOW_LOG_TRACE("Do not have all of requested payload; got would-block.  Awaiting readability.");

    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once readable do this:
                           boost::make_shared<Task>([this, msg_state,
                                                     n_left = target_blob.size() - n_rcvd_or_zero]()
        {
          rcv_on_ev_peer_socket_byte_stream_readable_or_error(msg_state, n_left);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (!m_rcv_pending_err_code) (but may have become truthy inside, and has unless we `return`ed inside)

  assert(m_rcv_pending_err_code);

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_read_blob()

void Native_socket_stream_impl::rcv_resume_incomplete_msg_processing
       (Error_code* err_code, size_t* sz, Native_handle hndl_or_null,
        Native_socket_stream_cfg::low_lvl_payload_blob_length_t target_meta_length_possibly_incomplete,
        size_t target_meta_length_incomplete_n_rcvd_or_zero_if_complete,
        const flow::util::Blob& target_blob_incomplete)
{
  using util::Blob_mutable;

  assert(m_rcv_user_request && "We should be called from top-level-ish async-receive....");
  assert((!m_rcv_pending_err_code) && "Any cached error should have been emitted already.");
  assert(m_rcv_resume_incomplete_msg_processing_func.empty() && "It should be a new day!");

  // Replay the fact we may have gotten a handle.  Same deal as in rcv_on_handle_finalized(); keeping comments light.
  if ((!hndl_or_null.null()) && (!m_rcv_user_request->m_target_hndl_ptr))
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: While replaying: User async-receive request for "
                     "*only* a meta-blob: but earlier-received Native_handle is non-null which is "
                     "unexpected; emitting error.");
    *err_code = m_rcv_pending_err_code = error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB;
    *sz = 0;
    return;
  } // if (hndl_or_null && (!m_rcv_user_request->m_target_hndl_ptr))
  // else
  if (m_rcv_user_request->m_target_hndl_ptr)
  {
    *m_rcv_user_request->m_target_hndl_ptr = hndl_or_null;
  }

  /* In target_meta_length_possibly_incomplete, we have either all of m_rcv_target_meta_length; or just 1+ but not
   * all of the bytes of it.  Either way copy it over, including the possibly-garbage trailing bytes. */
  m_rcv_target_meta_length = target_meta_length_possibly_incomplete;

  if (target_meta_length_incomplete_n_rcvd_or_zero_if_complete != 0)
  {
    /* We have only some of it.  Replayed.  Now resume from there.
     * This is the core of what rcv_on_ev_peer_socket_byte_stream_readable_or_error(HEAD_PAYLOAD) would do.
     * @todo Some code reuse might be nice... though it might also just increase code verbostiy. */
    rcv_read_blob(Rcv_msg_state::S_HEAD_PAYLOAD,
                  Blob_mutable{static_cast<uint8_t*>(static_cast<void*>(&m_rcv_target_meta_length))
                                 + target_meta_length_incomplete_n_rcvd_or_zero_if_complete,
                               sizeof(m_rcv_target_meta_length)
                                 - target_meta_length_incomplete_n_rcvd_or_zero_if_complete},
                  err_code, sz);
    return;
  }
  /* else if (target_meta_length_incomplete_n_rcvd_or_zero_if_complete == 0): We have all of it.  Continue replaying.
   *
   * We have saved a copy of the completed part of the meta-blob.  (It might be empty; that is fine.) */

  assert((m_rcv_target_meta_length != 0)
         && "Got all of m_rcv_target_meta_length, and it is zero, yet instructed to finish reading meta-blob... "
              "but there should have been none; the message should have been completed; and we should not have "
              "been invoked at all.  Bug on someone's part?");

  if (!target_blob_incomplete.empty())
  {
    target_blob_incomplete.sub_copy(target_blob_incomplete.begin(),
                                    Blob_mutable{m_rcv_user_request->m_target_meta_blob.data(),
                                                 target_blob_incomplete.size()});
  }

  /* Replayed.  Now resume from there.
   * This is the core of what rcv_on_ev_peer_socket_byte_stream_readable_or_error(META_BLOB_PAYLOAD) would do.
   * @todo Some code reuse might be nice... though it might also just increase code verbostiy. */
  rcv_read_blob(Rcv_msg_state::S_META_BLOB_PAYLOAD,
                Blob_mutable{static_cast<uint8_t*>(m_rcv_user_request->m_target_meta_blob.data())
                               + target_blob_incomplete.size(),
                             size_t(m_rcv_target_meta_length)
                               - target_blob_incomplete.size()},
                err_code, sz);
} // Native_socket_stream_impl::rcv_resume_incomplete_msg_processing()

void Native_socket_stream_impl::rcv_read_msg_from_stream_having_assumed_would_block(Error_code* sync_err_code,
                                                                                    size_t* sync_sz)
{
  using flow::util::Lock_guard;
  using util::Task;

  /* Special mode; it's invoked only for perf reasons so fast-track through the various possibilities of what would
   * happen if !assume_would_block => actual nb-read => would-block.  This decreases maintainability but is nice for
   * perf.  Keeping comments light; see the !assume_would_block paths for those.  @todo Revisit.
   *
   * A nice thing is that particular path is nearly identical whether we're using Protocol_pkt_stream or the
   * generally more annoying Protocol_byte_stream.  So we can code this common-to-both helper which just
   * diverges in one `if constexpr()` below. */

  Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

  if (m_peer_socket)
  {
    if (!m_rcv_user_request->m_on_done_func.empty())
    {
      FLOW_LOG_TRACE("Got nothing but would-block (pre-assumed).  Awaiting readability.");

      m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket, false, boost::make_shared<Task>([this]()
      {
        if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
        {
          rcv_on_ev_peer_socket_pkt_stream_readable_or_error();
        }
        else
        {
          rcv_on_ev_peer_socket_byte_stream_readable_or_error(Rcv_msg_state::S_MSG_START,
                                                              0 /* ignored for S_MSG_START */);
        }
      }));
    }
    // else { Corner case. }

    *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
  }
  else
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request (pre-assumed would-block): "
                     "was about to await readability but discovered opposite-direction socket-hosing error; "
                     "emitting error via completion handler (or via sync-args).");
    *sync_err_code = m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  }
  *sync_sz = 0;
} // Native_socket_stream_impl::rcv_read_msg_from_stream_having_assumed_would_block()

size_t Native_socket_stream_impl::rcv_nb_read_low_lvl_payload_from_pkt_stream
         (Native_handle* target_payload_hndl,
          const util::Blob_mutable& target_payload_blob1, const util::Blob_mutable& target_payload_blob2_or_none,
          Error_code* err_code)
{
  using asio_local_stream_socket::nb_read_some_with_native_handle;
  using util::Blob_mutable;
  using flow::util::Lock_guard;
  using boost::array;

  /* @todo This is somewhat similar to rcv_nb_read_low_lvl_payload_from_byte_stream(), but only in little pieces.
   * Try to code-reuse without sacrificing any perf; or maybe avoid duplicating comments (though, the dupe comments
   * are not long). */

  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(err_code);
  assert(target_payload_hndl && "Our contract is we must have a target Native_handle; rationale is in our doc header.");

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  /* Result semantics (reminder of what we promised in contract):
   *   Success in non-blockingly receiving blob(s) and handle-or-none: > 0; falsy *err_code.
   *     USE_OS_DGRAM_SUPPORT => Either we got that, or we got nothing; no "partial" success possible.
   *   No success in receiving blob(s)+handle, because it would-block (not fatal): == 0; falsy *err_code.
   *   No success in receiving blob(s)+handle, because fatal error: == 0, truthy *err_code. */
  size_t n_rcvd_or_zero = 0;

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

    if (m_peer_socket)
    {
      /* Per contract, nb_read_some_with_native_handle() is identical to setting non_blocking(true) and attempting
       * .receive(<buffer sequence below>) -- except if it's able to receive an in-dgram, it'll also have set the
       * target handle to either null (none present) or non-null; and it detects having to truncate the in-dgram
       * due to target_payload_blob1 + target_payload_blob2_or_none being in sum too small (it'll emit
       * S_BLOB_RECEIVER_GOT_NON_BLOB in that case, as we advertised).
       *
       * Perf subtlety: If target_payload_blob2_or_none is "none" (empty), then specifying target_payload_blob1
       * alone (as opposed to a 2-container with an empty 2nd element) subtly chooses a compile-time-faster template
       * impl of nb_read_some_with_native_handle().  So do that if relevant, even though the code is less
       * elegant-looking here.  Also the best choice (for another compile-time-decided optimization) for the
       * 2-container (if relevant) is {std|boost}::array<2>.  So use that if relevant. */
      if (target_payload_blob2_or_none.size() == 0)
      {
        n_rcvd_or_zero = nb_read_some_with_native_handle<Native_socket_stream_cfg::Protocol>
                           (get_logger(), m_peer_socket.get(), target_payload_hndl,
                            target_payload_blob1, err_code);
      }
      else
      {
        array<Blob_mutable, 2> target_buf_seq = { target_payload_blob1, target_payload_blob2_or_none };
        n_rcvd_or_zero = nb_read_some_with_native_handle<Native_socket_stream_cfg::Protocol>
                           (get_logger(), m_peer_socket.get(), target_payload_hndl,
                            target_buf_seq, err_code);
      }
      // That should have TRACE-logged stuff, so we won't (it's our function).

      /* Almost home free; but our result semantics are a little different from the low-level-read functions'.
       *
       * Plus, if we just discovered the connection is hosed, do whatever's needed with that. */
      assert(((!*err_code) && (n_rcvd_or_zero != 0))
             || (*err_code && (n_rcvd_or_zero == 0)));
      if (*err_code == boost::asio::error::would_block)
      {
        err_code->clear();
        // *err_code is falsy; n_rcvd_or_zero == 0; cool.
      }
      else if (*err_code && (*err_code != error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE))
      {
        /* True-blue system error.  Kill off *m_peer_socket (connection hosed).  We could simply nullify it, which would
         * give it back to the system (it's a resource), but see m_peer_socket_hosed doc header for explanation as
         * to why we cannot, and why instead we transfer it to "death row" until dtor executes, at which
         * point ::close(m_peer_socket_hosed->native_handle()) will actually happen. */

        assert((!m_peer_socket_hosed) && "m_peer_socket_hosed must start as null and only become non-null once.  Bug?");
        m_peer_socket_hosed = std::move(m_peer_socket);
        assert((!m_peer_socket) && "Shocking unique_ptr misbehavior!");

        // *err_code is truthy; n_rcvd_or_zero == 0; cool.
      }
      /* else if (!*err_code) { *err_code is falsy; n_rcvd_or_zero >= 1; cool. }
       * else if (*err_code == MESSAGE_SIZE_EXCEEDS_USER_STORAGE)
       * { *err_code is truthy; n_rcvd_or_zero == 0; and to our user the *in*-direction pipe is likely hosed --
       *   our caller shall set m_rcv_pending_err_code accordingly, and that's that.  However, we choose *not*
       *   to hose m_peer_socket, and therefore the *out*-direction pipe continues to operate if desired. } */
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
    }
  } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

  assert((!*err_code)
         || (n_rcvd_or_zero == 0)); // && *err_code

  if (*err_code)
  {
    FLOW_LOG_TRACE("Received nothing due to error [" << *err_code << "] [" << err_code->message() << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Receive: no error.  Was able to receive [" << n_rcvd_or_zero << "] of "
                   "[" << (target_payload_blob1.size() + target_payload_blob2_or_none.size()) << "] bytes; "
                   "interest in native handle: got [" << *target_payload_hndl << "].");
  } // else if (!*err_code)

  return n_rcvd_or_zero;
} // Native_socket_stream_impl::rcv_nb_read_low_lvl_payload_from_pkt_stream()

template<typename Ignored> // See below.  Technicalities therein aside -- feel free to ignore this line.
size_t Native_socket_stream_impl::rcv_nb_read_low_lvl_payload_from_byte_stream
         (Native_handle* target_payload_hndl_or_null,
          const util::Blob_mutable& target_payload_blob, Error_code* err_code)
{
  using asio_local_stream_socket::nb_read_some_with_native_handle;
  using flow::util::Lock_guard;

  /* @todo This is somewhat similar to rcv_nb_read_low_lvl_payload_from_pkt_stream(), but only in little pieces.
   * Try to code-reuse without sacrificing any perf; or maybe avoid duplicating comments (though, the dupe comments
   * are not long). */

  assert((!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
         && "Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.  There is a @todo lower-down in this function to that effect.");
  assert(err_code);

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  /* Result semantics (reminder of what we promised in contract):
   *   Partial or total success in non-blockingly receiving blob and handle-or-none: > 0; falsy *err_code.
   *   No success in receiving blob+handle, because it would-block (not fatal): == 0; falsy *err_code.
   *   No success in receiving blob+handle, because fatal error: == 0, truthy *err_code. */
  size_t n_rcvd_or_zero = 0;

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

    if (m_peer_socket)
    {
      if (target_payload_hndl_or_null)
      {
        /* Per contract, nb_read_some_with_native_handle() is identical to setting non_blocking(true) and attempting
         * m_peer_socket->receive(target_payload_blob) -- except if it's able to receive even 1 byte it'll also
         * have set the target handle to either null (none present) or non-null.
         * When we say identical we mean identical result semantics along with everything else. */
        n_rcvd_or_zero = nb_read_some_with_native_handle<Native_socket_stream_cfg::Protocol>
                           (get_logger(), m_peer_socket.get(),
                            target_payload_hndl_or_null, target_payload_blob, err_code);
        // That should have TRACE-logged stuff, so we won't (it's our function).
      } // if (target_payload_hndl_or_null)
      else // if (!target_payload_hndl_or_null)
      {
        /* No interest in receiving a handle, so we can just use boost.asio's normal non-blocking read
         * (non_blocking(true), receive()). */

        // First set non-blocking mode... same deal as in snd_nb_write_low_lvl_payload(); keeping comments light.
        if (!m_peer_socket->non_blocking())
        {
          FLOW_LOG_TRACE("Socket stream [" << *this << "]: Setting boost.asio peer socket non-blocking mode.");
          m_peer_socket->non_blocking(true, *err_code); // Sets *err_code to success or the triggering error.
        }
        else // if (already non-blocking)
        {
          err_code->clear();
        }

        if (!*err_code)
        {
          assert(m_peer_socket->non_blocking());

          FLOW_LOG_TRACE("Reading low-level blob directly via boost.asio (blob details logged above hopefully).");

          /* (Without the `if constexpr()` it would try to compile a call to
           *   Protocol_pkt_stream::socket::receive
           *     (<signature that exists only for Protocol_byte_stream::socket::receive()>) => compile error.
           * even though the present method would never be called.) */
          if constexpr(!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
          {
            /* Here we want to just:
             *   n_rcvd_or_zero = m_peer_socket->receive(target_payload_blob, 0, *err_code);
             * Unfortunately due to extremely difficult rules about when a discarded branch of `if constexpr()`
             * is still semantically-checked, and to what extent, that would still lead to the compile error.
             * By placing the present code in a fake template (parameterized on `typename Ignored = void`) *and*
             * wrapping the call in a template-specialization-via-auto-taking-lambda, we can get the compiler
             * (clang, gcc, various versions each) to forego instantiating the "problem" code when semantically
             * checking it.  Note that any decent optimizer will in the end still generate the equivalent of
             * the above desired statement, inlined.
             *
             * The following would be probably more "direct," but at least some compilers will "see through it"
             * and give a compile error still:
             *   n_rcvd_zero = invoke(&Protocol_byte_stream::sock::receive, m_peer_socket.get(),
             *                        target_payload_blob, 0, *err_code);
             * So we'll settle for the funky lambda-with-auto-arg thing. */
            n_rcvd_or_zero =
              ([&](auto& sock) -> auto { return sock->receive(target_payload_blob, 0, *err_code); })
                (m_peer_socket);
          }
          else
          {
            assert(false && "We are not a template so would have been compiled even though USE_OS_DGRAM_SUPPORT; "
                              "but we should not have been *called* ever.");
            /* @todo There is surely a way to structure our compile-time-diverged code-paths using certain C++
             * techniques (templates would be surely involved) so that the code in this method is never checked
             * by the compiler (beyond syntax), unless !USE_OS_DGRAM_SUPPORT.  The same applies to a few other
             * places in Flow-IPC with similar properties.  It would not be an algorithmic improvement, but the
             * code would be perhaps cleaner; with unnecessary code paths not being instantiated at all.
             * Warning: Getting that to work requires a keen understanding of some hairiness such as
             *   [ https://en.cppreference.com/w/cpp/language/if.html#Constexpr_if ]
             * which I (ygoldfel) assure you is even tougher than it looks. */
          }
        }
        // else if (*err_code) { *err_code is truthy; n_rcvd_or_zero == 0; cool. }
      } // else if (!target_payload_hndl_or_null)

      /* Almost home free; but our result semantics are a little different from the low-level-read functions'.
       *
       * Plus, if we just discovered the connection is hosed, do whatever's needed with that. */
      assert(((!*err_code) && (n_rcvd_or_zero != 0))
             || (*err_code && (n_rcvd_or_zero == 0)));
      if (*err_code == boost::asio::error::would_block)
      {
        err_code->clear();
        // *err_code is falsy; n_rcvd_or_zero == 0; cool.
      }
      else if (*err_code)
      {
        /* True-blue system error.  Kill off *m_peer_socket (connection hosed).  We could simply nullify it, which would
         * give it back to the system (it's a resource), but see m_peer_socket_hosed doc header for explanation as
         * to why we cannot, and why instead we transfer it to "death row" until dtor executes, at which
         * point ::close(m_peer_socket_hosed->native_handle()) will actually happen. */

        assert((!m_peer_socket_hosed) && "m_peer_socket_hosed must start as null and only become non-null once.  Bug?");
        m_peer_socket_hosed = std::move(m_peer_socket);
        assert((!m_peer_socket) && "Shocking unique_ptr misbehavior!");

        // *err_code is truthy; n_rcvd_or_zero == 0; cool.
      }
      // else if (!*err_code) { *err_code is falsy; n_rcvd_or_zero >= 1; cool. }
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
    }
  } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

  assert((!*err_code)
         || (n_rcvd_or_zero == 0)); // && *err_code

  if (*err_code)
  {
    FLOW_LOG_TRACE("Received nothing due to error [" << *err_code << "] [" << err_code->message() << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Receive: no error.  Was able to receive [" << n_rcvd_or_zero << "] of "
                   "[" << target_payload_blob.size() << "] bytes.");
    if (target_payload_hndl_or_null)
    {
      if (n_rcvd_or_zero != 0)
      {
        FLOW_LOG_TRACE("Interest in native handle; got [" << *target_payload_hndl_or_null << "].");
      }
    } // if (target_payload_hndl_or_null)
    else
    {
      FLOW_LOG_TRACE("No interest in native handle.");
    }
  } // else if (!*err_code)

  return n_rcvd_or_zero;
} // Native_socket_stream_impl::rcv_nb_read_low_lvl_payload_from_byte_stream()

void Native_socket_stream_impl::rcv_log_stats(util::String_view context) const
{
  using flow::util::stat::print;

  if (m_state == State::S_PEER)
  {
    FLOW_LOG_INFO("Socket stream [" << *this << "]: In context [" << context << "]: Stats: "
                  "rcv[" << print(m_rcv_stats) << "].");
  }
  // else { It'd all be zeroes anyway. }
}

size_t Native_socket_stream_impl::receive_meta_blob_max_size() const
{
  return state_peer("receive_meta_blob_max_size()") ? Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH : 0;
}

size_t Native_socket_stream_impl::receive_blob_max_size() const
{
  return receive_meta_blob_max_size();
}

stat::Blob_rcv_stats Native_socket_stream_impl::blob_receive_stats() const
{
  return m_rcv_stats;
}

void Native_socket_stream_impl::blob_receive_stats_reset()
{
  flow::util::stat::stats_reset(&m_rcv_stats, Blob_rcv_stats{Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH});
}

stat::Blob_rcv_stats Native_socket_stream_impl::native_handle_receive_stats() const
{
  return blob_receive_stats();
}

void Native_socket_stream_impl::native_handle_receive_stats_reset()
{
  blob_receive_stats_reset();
}

} // namespace ipc::transport::sync_io
