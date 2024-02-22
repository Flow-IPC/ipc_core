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
#include "ipc/transport/error.hpp"

namespace ipc::transport::sync_io
{

// Native_socket_stream::Impl implementations (::rcv_*() and receive-API methods only).

bool Native_socket_stream::Impl::start_receive_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_ops<Op::S_RCV>(std::move(ev_wait_func));
}

bool Native_socket_stream::Impl::start_receive_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_receive_native_handle_ops(std::move(ev_wait_func));
}

bool Native_socket_stream::Impl::async_receive_native_handle(Native_handle* target_hndl,
                                                             const util::Blob_mutable& target_meta_blob,
                                                             Error_code* sync_err_code, size_t* sync_sz,
                                                             flow::async::Task_asio_err_sz&& on_done_func)
{
  /* Subtlety: async_receive_blob() will pass target_hndl==nullptr to _impl(); but we do not allow this.
   * When using us as a Native_handle_receiver they must be ready to receive a Native_handle, even if the other
   * side chooses to not send one (then *target_hndl shall be set to equal Native_handle(), a null handle).
   *
   * So now internally our code can tell whether this is from a Blob_receiver or a Native_handle_receiver role
   * and emit error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB if target_hndl==0, yet the other side sent an actual handle. */
  assert(target_hndl && "Native_socket_stream::async_receive_native_handle() must take non-null Native_handle ptr.");

  return async_receive_native_handle_impl(target_hndl, target_meta_blob, sync_err_code, sync_sz,
                                          std::move(on_done_func));
}

bool Native_socket_stream::Impl::async_receive_blob(const util::Blob_mutable& target_blob,
                                                    Error_code* sync_err_code, size_t* sync_sz,
                                                    flow::async::Task_asio_err_sz&& on_done_func)
{
  return async_receive_native_handle_impl(nullptr, target_blob, sync_err_code, sync_sz, std::move(on_done_func));
}

bool Native_socket_stream::Impl::async_receive_native_handle_impl(Native_handle* target_hndl_or_null,
                                                                  const util::Blob_mutable& target_meta_blob,
                                                                  Error_code* sync_err_code_ptr, size_t* sync_sz,
                                                                  flow::async::Task_asio_err_sz&& on_done_func)
{
  using util::Blob_mutable;

  if ((!op_started<Op::S_RCV>("async_receive_native_handle()")) || (!state_peer("async_receive_native_handle()")))
  {
    return false;
  }
  // else

  /* We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.
   *
   * Briefly though: If you understand send_native_handle() impl, then this will be easy in comparison:
   * no more than one async_receive_native_handle_impl() can be outstanding at a time; they are not allowed to
   * invoke it until we invoke their completion handler.  (Rationale is shown in detail elsewhere.)
   * So there is no queuing of requests (deficit); nor reading more messages beyond what has been requested
   * (surplus).
   *
   * That said, we must inform them of completion via on_done_func(), whereas send_native_handle() has no
   * such requirement; easy enough -- we just save it as needed.  In that sense it is much like
   * async_end_sending().
   *
   * ...No, it's not easy in comparison actually.  Sure, there is no queuing of requests, but the outgoing-direction
   * algorithm makes the low-level payloads and then just sends them out -- what's inside doesn't matter.
   * Incoming-direction is harder, because we need to read payload 1, interpret it, possibly read payload 2.
   * So there is a bunch of tactical nonsense about async-waiting and then resuming from the same spot and so on. */

  if (m_rcv_user_request)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Async-receive requested, but the preceding such request "
                     "is still in progress; the message has not arrived yet.  "
                     "Likely a user error, but who are we to judge?  Ignoring.");
    return false;
  }
  // else

  Error_code sync_err_code;

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
  else // if (!m_rcv_pending_err_code)
  {
    // Background can be found by following the comment on this concept constant.
    static_assert(Native_socket_stream::S_BLOB_UNDERFLOW_ALLOWED,
                  "Socket streams allow trying to receive into a buffer that could underflow "
                    "with the largest *possible* message -- as long as the actual message "
                    "(if any) happens to be small enough to fit.  Otherwise a pipe-hosing "
                    "error shall occur at receipt time.");

    m_rcv_user_request.emplace();
    m_rcv_user_request->m_target_hndl_ptr = target_hndl_or_null;
    m_rcv_user_request->m_target_meta_blob = target_meta_blob;
    m_rcv_user_request->m_on_done_func = std::move(on_done_func);

    rcv_read_msg(&sync_err_code, sync_sz);
  } // else if (!m_rcv_pending_err_code)

  if ((!sync_err_code) || (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK))
  {
    FLOW_LOG_TRACE("Async-request completed synchronously (result "
                    "[" << sync_err_code << "] [" << sync_err_code.message() << "]); emitting synchronously and "
                    "disregarding handler.");
    m_rcv_user_request.reset(); // No-op if we didn't set it up due to m_rcv_pending_err_code being truthy already.
  }
  // else { Other stuff logged enough. }

  // Standard error-reporting semantics.
  if ((!sync_err_code_ptr) && sync_err_code)
  {
    throw flow::error::Runtime_error(sync_err_code, "Native_socket_stream::Impl::async_receive_native_handle_impl()");
  }
  // else
  sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
  // And if (!sync_err_code_ptr) + no error => no throw.

  return true;
} // Native_socket_stream::Impl::async_receive_native_handle_impl()

bool Native_socket_stream::Impl::idle_timer_run(util::Fine_duration timeout)
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

  return true;
} // Native_socket_stream::Impl::idle_timer_run()

void Native_socket_stream::Impl::rcv_on_ev_idle_timer_fired()
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

  FLOW_LOG_WARNING("Socket stream [" << *this << "]: Idle timer fired: There's been 0 traffic past idle timeout.  "
                   "Will not proceed with any further low-level receiving.  If a user async-receive request is "
                   "pending (is it? = [" << bool(m_rcv_user_request) << "]) will emit to completion handler.");

  if (m_rcv_user_request)
  {
    // Prevent stepping on our own toes: move/clear it first / invoke handler second.
    const auto on_done_func = std::move(m_rcv_user_request->m_on_done_func);
    m_rcv_user_request.reset();
    on_done_func(m_rcv_pending_err_code, 0);
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
  }

  /* That's it.  If m_rcv_user_request (we've just nullified it) then async read chain will be finished forever as
   * soon as the user informs us of readability (if it ever does) -- we will detect there's an error
   * in m_rcv_pending_err_code already (and hence no m_rcv_user_request). */
} // Native_socket_stream::Impl::rcv_on_ev_idle_timer_fired()

void Native_socket_stream::Impl::rcv_not_idle()
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
} // Native_socket_stream::Impl::rcv_not_idle()

void Native_socket_stream::Impl::rcv_read_msg(Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using util::Blob_mutable;
  using flow::util::Lock_guard;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-receive: Start of payload 1: "
                 "Trying nb-read of payload 1 (handle if any, meta-blob-length) synchronously.");
  assert(!m_rcv_pending_err_code);

  Native_handle target_hndl; // Target this even if target_hndl_or_null is null (to check for a certain error).
  const auto n_rcvd_or_zero
    = rcv_nb_read_low_lvl_payload(&target_hndl,
                                  Blob_mutable(&m_rcv_target_meta_length, sizeof(m_rcv_target_meta_length)),
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

    FLOW_LOG_TRACE("Got nothing but would-block.  Awaiting readability.");

    /* Conceptually we'd like to do m_peer_socket->async_wait(readable, F), where F() would perform
     * rcv_nb_read_low_lvl_payload() (nb-receive over m_peer_socket).  However this is the sync_io pattern, so
     * the user will be performing the conceptual async_wait() for us.  We must ask them to do so
     * via m_rcv_ev_wait_func(), giving them m_peer_socket's FD -- m_ev_wait_hndl_peer_socket -- to wait-on. */

    // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see their docs).
    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once writable do this:
                           boost::make_shared<Task>([this]()
        {
          rcv_on_ev_peer_socket_readable_or_error(Rcv_msg_state::S_MSG_START, 0 /* ignored for S_MSG_START */);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

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
} // Native_socket_stream::Impl::rcv_read_msg()

void Native_socket_stream::Impl::rcv_on_handle_finalized(Native_handle hndl_or_null, size_t n_rcvd,
                                                         Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Blob_mutable;
  using util::Task;
  using flow::util::Lock_guard;

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

    /* Still have to finish reading into m_rcv_target_meta_length.  We've already hit would-block so,
     * much like in rcv_read_msg() (keeping comments light): */

    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once writable do this:
                           boost::make_shared<Task>([this, n_rcvd]()
        {
          rcv_on_ev_peer_socket_readable_or_error(Rcv_msg_state::S_HEAD_PAYLOAD,
                                                  sizeof(m_rcv_target_meta_length) - n_rcvd);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User async-receive request: "
                     "was about to await readability but discovered opposite-direction socket-hosing error; "
                     "emitting error via completion handler (or via sync-args).");

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (no prob with hndl_or_null or m_target_hndl_ptr) (but another problem may have occurred inside)

  assert(m_rcv_pending_err_code);

  // WARNINGs above are enough; no TRACE here.

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream::Impl::rcv_on_handle_finalized()

void Native_socket_stream::Impl::rcv_on_head_payload(Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Blob_mutable;

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
    rcv_read_msg(sync_err_code, sync_sz);
    return;
  }
  // else if (!proto_negotiating): Normal payload 1 handling.

  if (m_rcv_target_meta_length == S_META_BLOB_LENGTH_PING_SENTINEL)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Received all of payload 1; length prefix "
                   "contains special value indicating a ping.  Ignoring other than registering non-idle "
                   "activity.  Proceeding with the next message read.");

    rcv_not_idle(); // Register activity <= end of complete message, no error.
    rcv_read_msg(sync_err_code, sync_sz);
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
                    Blob_mutable(m_rcv_user_request->m_target_meta_blob.data(),
                                 size_t(m_rcv_target_meta_length)),
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
    }
    else
    {
      // Once per connection at most, so INFO log level is OK.
      FLOW_LOG_INFO("Socket stream [" << *this << "]: User message received: Graceful-close-of-incoming-pipe "
                    "message.  Will not proceed with any further low-level receiving.  "
                    "Will invoke handler (graceful-close error).");
      m_rcv_pending_err_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE;
    }
  } // else if (m_rcv_target_meta_length == 0)

  *sync_err_code = m_rcv_pending_err_code; // Truthy (graceful-close) or falsy (got handle + no meta-blob).
  *sync_sz = 0;
} // Native_socket_stream::Impl::rcv_on_head_payload()

void Native_socket_stream::Impl::rcv_on_ev_peer_socket_readable_or_error(Rcv_msg_state msg_state, size_t n_left)
{
  using util::Blob_mutable;

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
    rcv_read_msg(&sync_err_code, &sync_sz);
    break;

  case Rcv_msg_state::S_HEAD_PAYLOAD:
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: In state HEAD_PAYLOAD: "
                   "Reading meta-length/ping/graceful-close specifier: [" << n_left << "] bytes left.");
    rcv_read_blob(Rcv_msg_state::S_HEAD_PAYLOAD,
                  Blob_mutable(static_cast<uint8_t*>(static_cast<void*>(&m_rcv_target_meta_length))
                                 + sizeof(m_rcv_target_meta_length) - n_left,
                               n_left),
                  &sync_err_code, &sync_sz);
    break;

  case Rcv_msg_state::S_META_BLOB_PAYLOAD:
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: In state META_BLOB_PAYLOAD: "
                   "Reading meta-blob: [" << n_left << "] bytes left.");
    rcv_read_blob(Rcv_msg_state::S_META_BLOB_PAYLOAD,
                  Blob_mutable(static_cast<uint8_t*>(m_rcv_user_request->m_target_meta_blob.data())
                                 + size_t(m_rcv_target_meta_length) - n_left,
                               n_left),
                  &sync_err_code, &sync_sz);
  } // switch (msg_state) (Compiler should catch any missed enum value.)

  if (sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // Another async-wait is pending now.  We've logged enough.  Live to fight another day.
    return;
  }
  // else

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-op result ready after successful async-wait.  "
                 "Executing handler now.");

  // Prevent stepping on our own toes: move/clear it first / invoke handler second.
  const auto on_done_func = std::move(m_rcv_user_request->m_on_done_func);
  m_rcv_user_request.reset();
  on_done_func(sync_err_code, sync_sz);
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed.");
} // Native_socket_stream::Impl::rcv_on_ev_peer_socket_readable_or_error()

void Native_socket_stream::Impl::rcv_read_blob(Rcv_msg_state msg_state, const util::Blob_mutable& target_blob,
                                               Error_code* sync_err_code, size_t* sync_sz)
{
  using util::Task;
  using flow::util::Lock_guard;

  assert(!m_rcv_pending_err_code);
  assert(m_rcv_user_request);

  const auto n_rcvd_or_zero = rcv_nb_read_low_lvl_payload(nullptr, target_blob, &m_rcv_pending_err_code);
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

        assert(!*sync_err_code);
        *sync_sz = size_t(m_rcv_target_meta_length);

        return;
      }
      case Rcv_msg_state::S_MSG_START:
        assert(false && "rcv_read_blob() shall be used only for S_*_PAYLOAD phases.");
      }
      assert(false && "Should not get here.");
    } // if (n_rcvd_or_zero == target_blob.size())
    // else if (n_rcvd_or_zero != target_blob.size())

    FLOW_LOG_TRACE("Do not have all of requested payload; got would-block.  Awaiting readability.");

    // So, much like in rcv_read_msg() (keeping comments light): */
    {
      Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

      if (m_peer_socket)
      {
        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                           false, // Wait for read.
                           // Once writable do this:
                           boost::make_shared<Task>([this, msg_state,
                                                     n_left = target_blob.size() - n_rcvd_or_zero]()
        {
          rcv_on_ev_peer_socket_readable_or_error(msg_state, n_left);
        }));

        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
        *sync_sz = 0;
        return;
      }
      // else:
    } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

    m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
  } // if (!m_rcv_pending_err_code) (but may have become truthy inside, and has unless we `return`ed inside)

  assert(m_rcv_pending_err_code);

  *sync_err_code = m_rcv_pending_err_code;
  *sync_sz = 0;
} // Native_socket_stream::Impl::rcv_read_blob()

size_t Native_socket_stream::Impl::rcv_nb_read_low_lvl_payload(Native_handle* target_payload_hndl_or_null,
                                                               const util::Blob_mutable& target_payload_blob,
                                                               Error_code* err_code)
{
  using flow::util::Lock_guard;
  using asio_local_stream_socket::nb_read_some_with_native_handle;

  assert(err_code);

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  /* Result semantics (reminder of what we promised in contract):
   *   Partial or total success in non-blockingly receiving blob and handle-or-none: > 0; falsy *err_code.
   *   No success in receiving blob+handle, because it would-block (not fatal): == 0; falsy *err_code.
   *   No success in receiving blob+handle, because fatal error: == 0, truthy *err_code. */
  size_t n_rcvd_or_zero = 0;

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

    if (m_peer_socket)
    {
      if (target_payload_hndl_or_null)
      {
        /* Per contract, nb_read_some_with_native_handle() is identical to setting non_blocking(true) and attempting
         * read_some(orig_blob) -- except if it's able to receive even 1 byte it'll also have set the target handle
         * to either null (none present) or non-null.
         * When we say identical we mean identical result semantics along with everything else. */
        n_rcvd_or_zero = nb_read_some_with_native_handle(get_logger(), m_peer_socket.get(),
                                                         target_payload_hndl_or_null, target_payload_blob, err_code);
        // That should have TRACE-logged stuff, so we won't (it's our function).
      } // if (target_payload_hndl_or_null)
      else // if (!target_payload_hndl_or_null)
      {
        /* No interest in receiving a handle, so we can just use boost.asio's normal non-blocking read
         * (non_blocking(true), read_some()). */

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
          n_rcvd_or_zero = m_peer_socket->read_some(target_payload_blob, *err_code);
        }
        // else if (*err_code) { *err_code is truthy; n_rcvd_or_zero == 0; cool. }
      }

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
        // True-blue system error.  Kill off *m_peer_socket: might as well give it back to the system (it's a resource).

        // Closes peer socket to the (hosed anyway) connection; including ::close(m_peer_socket->native_handle()).
        m_peer_socket.reset();

        // *err_code is truthy; n_rcvd_or_zero == 0; cool.
      }
      // else if (!*err_code) { *err_code is falsy; n_rcvd_or_zero >= 1; cool. }
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
    }
  } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

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
        FLOW_LOG_TRACE("Interest in native handle; was able to establish its presence or absence; "
                       "present? = [" << (!target_payload_hndl_or_null->null()) << "].");
      }
    } // if (target_payload_hndl_or_null)
    else
    {
      FLOW_LOG_TRACE("No interest in native handle.");
    }
  } // else if (!*err_code)

  return n_rcvd_or_zero;
} // Native_socket_stream::Impl::rcv_nb_read_low_lvl_payload()

size_t Native_socket_stream::Impl::receive_meta_blob_max_size() const
{
  return state_peer("receive_meta_blob_max_size()") ? S_MAX_META_BLOB_LENGTH : 0;
}

size_t Native_socket_stream::Impl::receive_blob_max_size() const
{
  return receive_meta_blob_max_size();
}

} // namespace ipc::transport::sync_io
