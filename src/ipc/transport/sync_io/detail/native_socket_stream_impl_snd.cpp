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
#include <boost/move/make_unique.hpp>

namespace ipc::transport::sync_io
{

// Native_socket_stream::Impl implementations (::snd_*() and send-API methods only).

bool Native_socket_stream::Impl::start_send_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_ops<Op::S_SND>(std::move(ev_wait_func));
}

bool Native_socket_stream::Impl::start_send_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_send_native_handle_ops(std::move(ev_wait_func));
}

bool Native_socket_stream::Impl::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  // Just use this in degraded fashion.
  return send_native_handle(Native_handle(), blob, err_code);
}

bool Native_socket_stream::Impl::send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                                                    Error_code* err_code)
{
  using util::Fine_duration;
  using util::Blob_const;
  using flow::util::buffers_dump_string;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Native_socket_stream::Impl::send_native_handle,
                                     hndl_or_null, flow::util::bind_ns::cref(meta_blob), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  if ((!op_started<Op::S_SND>("send_native_handle()")) || (!state_peer("send_native_handle()")))
  {
    err_code->clear();
    return false;
  }
  // else

  const size_t meta_size = meta_blob.size();
  assert(((!hndl_or_null.null()) || (meta_size != 0))
         && "Native_socket_stream::send_blob() blob must have length 1+; "
              "Native_socket_stream::send_native_handle() must have same or non-null hndl_or_null or both.");

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Will send handle [" << hndl_or_null << "] with "
                 "meta-blob of size [" << meta_blob.size() << "].");
  if (meta_size != 0)
  {
    // Verbose and slow (100% skipped unless log filter passes).
    FLOW_LOG_DATA("Socket stream [" << *this << "]: Meta-blob contents are "
                  "[\n" << buffers_dump_string(meta_blob, "  ") << "].");
  }

  if (m_snd_finished)
  {
    /* If they called *end_sending() before, then by definition (see doc header impl discussion)
     * any future send attempt is to be ignored with this error.  Even though previously queued stuff can and should
     * keep being sent, once that's done this clause will prevent any more from being initiated.
     *
     * Corner case: If those queued sends indeed exist (are ongoing) then user should have a way of knowing when it's
     * done.  That isn't the case for regular send_native_handle() calls which are silently queued up as-needed,
     * which is why I mention it here.  So that's why in *end_sending() there's a way for
     * user to be informed (via sync callback) when everything has been sent through, or if an error stops it
     * from happening.  None of our business here though: we just refuse to do anything and emit this error. */
    *err_code = error::Code::S_SENDS_FINISHED_CANNOT_SEND;
    // Note that this clause will always be reached subsequently also.
  }
  else if (meta_blob.size() > S_MAX_META_BLOB_LENGTH)
  {
    *err_code = error::Code::S_INVALID_ARGUMENT;
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Send: User argument length [" << meta_blob.size() << "] "
                     "exceeds limit [" << S_MAX_META_BLOB_LENGTH << "].");
    // More WARNING logging below; just wanted to show those particular details also.
  }
  else if (m_snd_pending_err_code) // && (!m_snd_finished) && (meta_blob.size() OK)
  {
    /* This --^ holds either the last inline-completed send_native_handle() call's emitted Error_code, or (rarely) one
     * that was found while attempting to dequeue previously-would-blocked queued-up (due to incomplete s_n_h())
     * payload(s).  This may seem odd, but that's part of the design of the send interface which we wanted to be
     * as close to looking like a series of synchronous always-inline-completed send_*() calls as humanly possible,
     * especially given that 99.9999% of the time that will indeed occur given proper behavior by the opposing
     * receiver. */

    FLOW_LOG_INFO("Socket stream [" << *this << "]: An error was detected earlier and saved for any subsequent "
                  "send attempts like this.  Will not proceed with send.  More info in WARNING below.");
    *err_code = m_snd_pending_err_code;
  }
  else // if (!m_snd_finished) && (meta_blob.size() OK) && (!m_snd_pending_err_code)
  {
    /* As seen in protocol definition in class doc header, payload 1 contains handle, if any, and the length of
     * the blob in payload 2 (or 0 if no payload 2).  Set up the meta-length thing on the stack before entering
     * critical section.  Endianness stays constant on the machine, so don't worry about that.
     * @todo Actually it would be (1) more forward-compatible and (2) consistent to do the same as how
     * the structured layer encodes UUIDs -- mandating a pre-send conversion native->little-endian, post-send
     * conversion backwards.  The forward-compatibility is for when this mechanism is expanded to inter-machine IPC;
     * while noting that the conversion is actually a no-op given our known hardware, so no real perf penalty. */
    const auto meta_length_raw = low_lvl_payload_blob_length_t(meta_size);
    // ^-- must stay on stack while snd_sync_write_or_q_payload() executes, and it will.  Will be copied if must queue.
    const Blob_const meta_length_blob(&meta_length_raw, sizeof(meta_length_raw));

    // Payload 2, if any, consists of stuff we already have ready, namely simply meta_blob itself.  Nothing to do now.

    // Little helper for below; handles each `(handle or none, blob)` payload.  Sets m_snd_pending_err_code.
    const auto send_low_lvl_payload
      = [&](unsigned int idx, Native_handle payload_hndl, const Blob_const& payload_blob)
    {
      FLOW_LOG_TRACE("Socket stream [" << *this << "]: Wanted to send handle [" << hndl_or_null << "] with "
                     "meta-blob of size [" << meta_blob.size() << "].  "
                     "About to send payload index [" << idx << "] of "
                     "[" << ((meta_length_raw == 0) ? 1 : 2) << "] new low-level payloads; "
                     "includes handle [" << payload_hndl << "] and "
                     "low-level blob of size [" << payload_blob.size() << "] "
                     "located @ [" << payload_blob.data() << "].");

      /* As the name indicates this may synchronously finish it or queue up any parts instead to be done once
       * a would-block clears, when user informs of this past the present function's return. */
      snd_sync_write_or_q_payload(payload_hndl, payload_blob, false);
      /* That may have returned `true` indicating everything (up to and including our payload) was synchronously
       * given to kernel successfuly; or this will never occur, because outgoing-pipe-ending error was encountered.
       * Since this is send_native_handle(), we do not care: there is no on-done
       * callback to invoke, as m_snd_finished is false, as *end_sending() has not been called yet. */
    }; // const auto send_low_lvl_payload =

    /* Send-or-queue each payload of which we spoke above.  There's a very high chance all of this is done inline;
     * but there's a small chance either there's either stuff queued already (we've delegated waiting for would-block
     * to clear to user), or not but this can't fully complete (encountered would-block).  We don't care here per
     * se; I am just saying for context, to clarify what "send-or-queue" means. */
    send_low_lvl_payload(1, hndl_or_null, meta_length_blob); // It sets m_snd_pending_err_code.
    if ((meta_length_raw != 0) && (!m_snd_pending_err_code))
    {
      send_low_lvl_payload(2, Native_handle(), meta_blob); // It sets m_snd_pending_err_code.
    }

    *err_code = m_snd_pending_err_code; // Emit the new error.

    // Did either thing generate a new error?
    if (*err_code)
    {
      FLOW_LOG_TRACE("Socket stream [" << *this << "]: Wanted to send user message but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << *err_code << "] [" << err_code->message() << "].  "
                     "Saved error code to return in next user send attempt if any, after this attempt also "
                     "returns that error code synchronously first.");
    }
    else if (m_snd_auto_ping_period != Fine_duration::zero()) // && (!*err_code)
    {
      /* Send requested, and there was no error; that represents non-idleness.  If auto_ping() has been called
       * (the feature is engaged), idleness shall occur at worst in m_snd_auto_ping_period; hence reschedule
       * snd_on_ev_auto_ping_now_timer_fired(). */

      const size_t n_canceled = m_snd_auto_ping_timer.expires_after(m_snd_auto_ping_period);

      FLOW_LOG_TRACE("Socket stream [" << *this << "]: Send request from user; hence rescheduled "
                     "auto-ping to occur in "
                     "[" << round<milliseconds>(m_snd_auto_ping_period) << "] (will re-reschedule "
                     "again upon any other outgoing traffic that might be requested before then).  As a result "
                     "[" << n_canceled << "] previously scheduled auto-pings have been canceled; 1 is most likely; "
                     "0 means an auto-ping is *just* about to fire (we lost the race -- which is fine).");
      if (n_canceled == 1)
      {
        /* m_timer_worker will m_snd_auto_ping_timer.async_wait(F), where F() will signal through pipe,
         * making *m_snd_auto_ping_timer_fired_peer readable.  We've already used m_snd_ev_wait_func() to start
         * wait on it being readable and invoke snd_on_ev_auto_ping_now_timer_fired() in that case; but we've
         * canceled the previous .async_wait() that would make it readable; so just redo that part. */
        m_timer_worker.timer_async_wait(&m_snd_auto_ping_timer, m_snd_auto_ping_timer_fired_peer);
      }
      else
      {
        assert((n_canceled == 0) && "We only invoke one timer async_wait() at a time.");

        /* Too late to cancel snd_on_ev_auto_ping_now_timer_fired(), so it'll just schedule next one itself.
         * Note that in practice the effect is about the same. */
      }
    } // else if (m_snd_auto_ping_period != zero) && (!*err_code)
    // else if (m_snd_auto_ping_period == zero) && (!*err_code) { Auto-ping feature not engaged. }
  } /* else if (!m_snd_finished) && (meta_blob.size() OK) && (!m_snd_pending_err_code)
     *         (but m_snd_pending_err_code may have become truthy inside) */

  if (*err_code)
  {
    // At the end try to categorize nature of error.
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send handle [" << hndl_or_null << "] with "
                     "meta-blob of size [" << meta_blob.size() << "], but an error (not necessarily new error) "
                     "encountered on pipe or in user API args.  Error code details follow: "
                     "[" << *err_code << "] [" << err_code->message() << "];  "
                     "pipe hosed (sys/protocol error)? = "
                     "[" << ((*err_code != error::Code::S_INVALID_ARGUMENT)
                             && (*err_code != error::Code::S_SENDS_FINISHED_CANNOT_SEND)) << "]; "
                     "sending disabled by user? = "
                     "[" << (*err_code == error::Code::S_SENDS_FINISHED_CANNOT_SEND) << "].");
  }

  return true;
} // Native_socket_stream::Impl::send_native_handle()

bool Native_socket_stream::Impl::end_sending()
{
  return async_end_sending_impl(nullptr, flow::async::Task_asio_err());
}

bool Native_socket_stream::Impl::async_end_sending(Error_code* sync_err_code_ptr,
                                                   flow::async::Task_asio_err&& on_done_func)
{
  Error_code sync_err_code;
  // This guy either takes null/.empty(), or non-null/non-.empty(): it doesn't do the Flow-style error emission itself.
  const bool ok = async_end_sending_impl(&sync_err_code, std::move(on_done_func));

  if (!ok)
  {
    return false; // False start.
  }
  // else

  // Standard error-reporting semantics.
  if ((!sync_err_code_ptr) && sync_err_code)
  {
    throw flow::error::Runtime_error(sync_err_code, "Native_socket_stream::Impl::async_end_sending()");
  }
  // else
  sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
  // And if (!sync_err_code_ptr) + no error => no throw.

  return true;
} // Native_socket_stream::Impl::async_end_sending()

bool Native_socket_stream::Impl::async_end_sending_impl(Error_code* sync_err_code_ptr_or_null,
                                                        flow::async::Task_asio_err&& on_done_func_or_empty)
{
  using util::Blob_const;
  using flow::async::Task_asio_err;

  assert(bool(sync_err_code_ptr_or_null) == (!on_done_func_or_empty.empty()));

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  if ((!op_started<Op::S_SND>("async_end_sending()")) || (!state_peer("async_end_sending()")))
  {
    return false;
  }
  // else

  /* Went back and forth on this semantic.  Choices, if m_snd_finished==true already, were:
   *   -1- Just return.  Never call on_done_func_or_empty() either.
   *   -2- on_done_func_or_empty().
   *     -a- Could pass success Error_code.
   *     -b- Could pass some new special Error_code for doubly-ending sends.
   *   -3- Just assert(false) -- undefined behavior.
   *   -4- Just return... but return false or something.
   *
   * 1 is out, because then they might be expecting it to be called, and it's never called; and since they can't solve
   * the halting problem, they could never know that it won't be called; of course they shouldn't be calling us in the
   * first place... but if they DID call us then presumably it's because it was a mistake, or they want to find out.
   * In the latter case, they're messed over; so 1 is out.
   *
   * (Note there is NO other way m_snd_finished becomes true.)
   *
   * 2b seems annoying -- an entirely new error code for something that's most likely an easily avoidable mistake
   * (though could reuse S_SENDS_FINISHED_CANNOT_SEND or S_INVALID_ARGUMENT...); and if they're trying to determine
   * *if* they'd already called it, then doing it via async handler is annoying from user's PoV and much better done
   * with an accessor synchronously.  Most importantly it breaks the normal pattern, wherein asynchronously reported
   * errors are asynchronously encountered (system) conditions, which this isn't; it's just meh.  Not awful but meh.
   *
   * 2a is pretty good for the user.  Though it won't indicate there was a problem... but on the other hand who cares?
   * However, internally it creates another async flow which would require some reasoning to ensure it doesn't interact
   * in some way with the rest of the outgoing direction (and incoming for that matter).  It'd be annoying to have
   * to think hard for such a dinky scenario.
   *
   * 3 is pretty good.  Implementation-wise it's by far the easiest.  Usage-wise, the user just needs to not make the
   * obvious error of calling it twice.  This can be done with their own flag if desired.  This seems sufficient, though
   * through experience we may determine otherwise ultimately which would require an API change, and that'd suck a bit.
   *
   * 4 is pretty good.  The interface is a little less clean, but if the user wouldn't cause the assert() in 3, then
   * they can exactly equally ignore the return value in 4 or assert() on it themselves.  It also allows the user to
   * detect the mistake easily.
   *
   * So 3 and 4 seem the best, and 4 is more robust at the cost of a small diff in interface complexity. */
  if (m_snd_finished)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wants to end sending, but we're in sends-finished "
                     "state already.  Ignoring.");
    return false;
  }
  // else
  m_snd_finished = true; // Cause future send_native_handle() to emit S_SENDS_FINISHED_CANNOT_SEND and return.

  bool qd; // Set to false to report results *now*: basically true <=> stuff is still queued to send.
  if (m_snd_pending_err_code)
  {
    qd = false; // There was outgoing-pipe-ending error detected before us; so should immediately report.
  }
  else
  {
    // Save this to emit once everything (including the thing we just made) has been sent off.  Or not if empty.
    assert(m_snd_pending_on_last_send_done_func_or_empty.empty());
    m_snd_pending_on_last_send_done_func_or_empty = std::move(on_done_func_or_empty);
    // on_done_func_or_empty is potentially hosed now.

    /* Prepare/send the payload per aforementioned (class doc header) strategy: no handle, and a 0x0000 integer.
     * Keeping comments light, as this is essentially a much simplified version of send_native_handle(). */

    const auto ZERO_SIZE_RAW = low_lvl_payload_blob_length_t(0);
    const Blob_const blob_with_0(&ZERO_SIZE_RAW, sizeof(ZERO_SIZE_RAW));

    /* snd_sync_write_or_q_payload():
     * Returns true => out-queue flushed successfully; or error detected.
     *   => report synchronously now.  (Unless they don't care about any such report.)
     * Returns false => out-queue has stuff in it and will continue to, until transport is writable.
     *   => cannot report completion yet. */

    qd = !snd_sync_write_or_q_payload(Native_handle(), blob_with_0, false);
    if (qd && sync_err_code_ptr_or_null)
    {
      /* It has not been flushed (we will return would-block).
       * Save this to emit once everything (including the thing we just made) has been sent off, since
       * they care about completion (sync_err_code_ptr_or_null not null). */
      assert(m_snd_pending_on_last_send_done_func_or_empty.empty());
      m_snd_pending_on_last_send_done_func_or_empty = std::move(on_done_func_or_empty);
      // on_done_func_or_empty is potentially hosed now.
    }
    /* else if (qd && (!sync_err_code_ptr_or_null))
     *   { on_done_func_or_empty is .empty() anyway.  Anyway they don't care about emitting result.  Done:
     *     It'll be async-sent when/if possible. }
     * else if (!qd)
     *   { All flushed synchronously.  We will emit it synchronously, if they're interested in that. } */
  } // if (!m_snd_pending_err_code) (but it may have become truthy inside)

  // Log the error, if any; report the result synchronously if applicable.

  if (m_snd_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wanted to end sending, but an error (not necessarily "
                     "new error) encountered on pipe synchronously when trying to send graceful-close.  "
                     "Nevertheless locally sends-finished state is now active.  Will report completion via "
                     "sync-args (if any).  Error code details follow: "
                     "[" << m_snd_pending_err_code << "] [" << m_snd_pending_err_code.message() << "].");
    assert(!qd);
  }
  else if (qd)
  {
    FLOW_LOG_INFO("Socket stream [" << *this << "]: User wanted to end sending.  Success so far but out-queue "
                  "has payloads -- at least the graceful-close payload -- still pending while waiting for "
                  "writability.  Locally sends-finished state is now active, and the other side will be informed "
                  "of this barring subsequent system errors.  "
                  "We cannot report completion via sync-args (if any).");
  }
  else // if ((!m_snd_pending_err_code) && (!qd))
  {
    FLOW_LOG_INFO("Socket stream [" << *this << "]: User wanted to end sending.  Immediate success: out-queue "
                  "flushed permanently.  "
                  "Locally sends-finished state is now active, and the other side will be informed of this.  "
                  "Locally will report completion via sync-args (if any).");
  }

  if (sync_err_code_ptr_or_null)
  {
    *sync_err_code_ptr_or_null = qd ? error::Code::S_SYNC_IO_WOULD_BLOCK
                                    : m_snd_pending_err_code; // Could be falsy (probably is usually).
  }
  // else { Don't care about completion. }

  return true;
} // Native_socket_stream::Impl::async_end_sending_impl()

bool Native_socket_stream::Impl::auto_ping(util::Fine_duration period)
{
  using util::Blob_const;
  using util::Fine_duration;
  using util::Task;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  if ((!op_started<Op::S_SND>("auto_ping()")) || (!state_peer("auto_ping()")))
  {
    return false;
  }
  // else

  assert(period.count() > 0);

  if (m_snd_finished)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wants to start auto-pings, but we're in "
                     "sends-finished state already.  Ignoring.");
    return false;
  }
  // else

  /* By concept requirements we are to do 2 things: send an auto-ping now as a baseline; and then make it so
   * *some* message (auto-ping or otherwise) is sent at least every `period` until *end_sending() or error. */

  /* Prepare the payload per class doc header strategy: no handle, and a 0xFFFF... integer.
   * Keeping comments somewhat light, as this is essentially a much simplified version of send_native_handle()
   * and is very similar to what async_end_sending() does in this spot. */

  if (m_snd_auto_ping_period != Fine_duration::zero())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wants to start auto-pings, but this "
                     "has already been engaged earlier.  Ignoring.");
    return false;
  }
  // else
  m_snd_auto_ping_period = period; // Remember this, both as flag (non-zero()) and to know how often to reschedule it.

  FLOW_LOG_INFO("Socket stream [" << *this << "]: User wants to start auto-pings so that there are "
                "outgoing messages at least as frequently as every "
                "[" << round<milliseconds>(m_snd_auto_ping_period) << "].  Sending baseline auto-ping and scheduling "
                "first subsequent auto-ping; it may be rescheduled if more user traffic occurs before then.");

  if (m_snd_pending_err_code)
  {
    /* Concept does not require us to report any error via auto_ping() itself.  It's for receiver's benefit anyway.
     * The local user will discover it, assuming they have interest, via the next send_*() or *end_sending(). */
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User wanted to start auto-pings, but an error was "
                     "previously encountered on pipe; so will not auto-ping.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].");
    return true;
  }
  // else

  const Blob_const blob_with_ff(&S_META_BLOB_LENGTH_PING_SENTINEL, sizeof(S_META_BLOB_LENGTH_PING_SENTINEL));

  /* Important: avoid_qing=true for reasons explained in its doc header.  Namely:
   * If blob_with_ff would-block entirely, then there are already data that would signal-non-idleness sitting
   * in the kernel buffer, so the auto-ping can be safely dropped in that case. */
  snd_sync_write_or_q_payload(Native_handle(), blob_with_ff, true);
  if (m_snd_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send initial auto-ping but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not schedule periodic auto-pings.");
    return true;
  }
  // else

  // Initial auto-ping partially- or fully-sent fine (anything unsent was queued).  Now schedule next one per above.

  /* Now we can schedule it, similarly to send_native_handle() and snd_on_ev_auto_ping_now_timer_fired() itself.
   * Recall that we can't simply .async_wait(F) on some timer and have the desired code --
   * snd_on_ev_auto_ping_now_timer_fired() which sends the ping -- be the handler F.  We have to use the sync_io
   * pattern, where the user waits on events for us... but there's no timer FD, so we do it in the separate thread and
   * then ping via a pipe.  (This is all discussed elsewhere, but yes, timerfd_*() is available; the boost.asio
   * timer API is much nicer however -- and while it appears to use timerfd_*() at least optionally, this
   * functionality is not exposed via .native_handle() or something.  But I digress!!!  Discussed elsewhere.) */

  m_snd_ev_wait_func(&m_snd_ev_wait_hndl_auto_ping_timer_fired_peer,
                     false, // Wait for read.
                     // Once readable do this: pop pipe; send ping; schedule again.
                     boost::make_shared<Task>
                       ([this]() { snd_on_ev_auto_ping_now_timer_fired(); }));
  /* Reminder: That has no effect (other than user recording stuff) until this method returns.
   * So it cannot execute concurrently or anything.  They'd need to do their poll()/epoll_wait() or do so
   * indirectly by returning from the present boost.asio task (if they're running a boost.asio event loop). */

  /* Set up the actual timer.  The second call really does m_snd_auto_ping_timer.async_wait().
   * Note that could fire immediately, even concurrently (if m_snd_auto_ping_period is somehow insanely short,
   * and the timer resolution is amazing)... but it would only cause snd_on_ev_auto_ping_now_timer_fired()
   * once they detect the pipe-readable event, which (again) can only happen after we return. */
  m_snd_auto_ping_timer.expires_after(m_snd_auto_ping_period);
  m_timer_worker.timer_async_wait(&m_snd_auto_ping_timer, m_snd_auto_ping_timer_fired_peer);

  return true;
} // Native_socket_stream::Impl::auto_ping()

void Native_socket_stream::Impl::snd_on_ev_auto_ping_now_timer_fired()
{
  using util::Blob_const;
  using util::Task;

  /* This is an event handler!  Specifically for the *m_snd_auto_ping_timer_fired_peer pipe reader being
   * readable.  To avoid infinite-loopiness, we'd best pop the thing that was written there. */
  m_timer_worker.consume_timer_firing_signal(m_snd_auto_ping_timer_fired_peer);

  // Now do the auto-ping itself.

  if (m_snd_pending_err_code)
  {
    /* Concept does not require us to report any error via auto_ping() itself.  It's for receiver's benefit anyway.
     * The local user will discover it, assuming they have interest, via the next send_*() or *end_sending(). */
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Auto-ping timer fired, but an error was "
                     "previously encountered in 2-way pipe; so will neither auto-ping nor schedule next auto-ping.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].");
    return;
  }
  // else

  if (m_snd_finished)
  {
    // This is liable to be quite common and not of much interest at the INFO level; though it's not that verbose.
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: "
                   "Auto-ping timer fired; but graceful-close API earlier instructed us to no-op.  No-op.");
    return;
  }
  // else

  // This may be of some interest sufficient for INFO.  @todo Reconsider due to non-trivial verbosity possibly.
  FLOW_LOG_INFO("Socket stream [" << *this << "]: "
                "Auto-ping timer fired; sending/queueing auto-ping; scheduling for next time; it may be "
                "rescheduled if more user traffic occurs before then.");

  // The next code is similar to the initial auto_ping().  Keeping comments light.

  const Blob_const blob_with_ff{&S_META_BLOB_LENGTH_PING_SENTINEL, sizeof(S_META_BLOB_LENGTH_PING_SENTINEL)};

  snd_sync_write_or_q_payload(Native_handle(), blob_with_ff, true);
  if (m_snd_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send non-initial auto-ping but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not continue scheduling periodic auto-pings.");
    return;
  }
  // else

  m_snd_ev_wait_func(&m_snd_ev_wait_hndl_auto_ping_timer_fired_peer,
                     false, // Wait for read.
                     boost::make_shared<Task>
                       ([this]() { snd_on_ev_auto_ping_now_timer_fired(); }));
  m_snd_auto_ping_timer.expires_after(m_snd_auto_ping_period);
  m_timer_worker.timer_async_wait(&m_snd_auto_ping_timer, m_snd_auto_ping_timer_fired_peer);
} // Native_socket_stream::Impl::snd_on_ev_auto_ping_now_timer_fired()

bool Native_socket_stream::Impl::snd_sync_write_or_q_payload(Native_handle hndl_or_null,
                                                             const util::Blob_const& orig_blob, bool avoid_qing)
{
  using flow::util::Blob;
  using util::Blob_const;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert((!m_snd_pending_err_code) && "Pipe must not be pre-hosed by contract.");

  size_t n_sent_or_zero;
  if (m_snd_pending_payloads_q.empty())
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Want to send low-level payload: "
                   "handle [" << hndl_or_null << "] with blob of size [" << orig_blob.size() << "] "
                   "located @ [" << orig_blob.data() << "]; no write is pending so proceeding immediately.  "
                   "Will drop if all of it would-block? = [" << avoid_qing << "].");

    n_sent_or_zero = snd_nb_write_low_lvl_payload(hndl_or_null, orig_blob, &m_snd_pending_err_code);
    if (m_snd_pending_err_code) // It will *not* emit would-block (will just return 0 but no error).
    {
      assert(n_sent_or_zero == 0);
      return true; // Pipe-direction-ending error encountered; outgoing-direction pipe is finished forevermore.
    }
    // else

    if (n_sent_or_zero == orig_blob.size())
    {
      // Awesome: Mainstream case: We wrote the whole thing synchronously.
      return true; // Outgoing-direction pipe flushed.
      // ^-- No error.  Logged about success in snd_nb_write_low_lvl_payload().
    }
    // else if (n_sent_or_zero < orig_blob.size()) { Fall through.  n_sent_or_zero is significant. }
  } // if (m_snd_pending_payloads_q.empty())
  else // if (!m_snd_pending_payloads_q.empty())
  {
    // Other stuff is currently being asynchronously sent, so we can only queue our payload behind all that.
    n_sent_or_zero = 0;
  }

  /* At this point some or all of the payload could not be sent (either because another async write-op is in progress,
   * or not but it would-block if we tried to send the rest now).
   * Per algorithm, we shall now have to queue it up to be sent (and if nothing is currently pending begin
   * asynchronously sending it ASAP).  The question now is what is "it" exactly: it needs to be exactly the payload
   * except the parts that *were* just sent (if any).
   *
   * avoid_qing==true, as of this writing used for auto-pings only, affects
   * the above as follows: If *all* of orig_blob would need to be queued (queue was already non-empty, or it
   * was empty, and snd_nb_write_low_lvl_payload() yielded would-block for *all* of orig_blob.size()), then:
   * simply pretend like it was sent fine; and continue like nothing happened.  (See our doc header for
   * rationale.) */

  if (avoid_qing)
  {
    assert(hndl_or_null.null()
           && "Internal bug?  Do not ask to drop a payload with a native handle inside under any circumstances.");
    if (n_sent_or_zero == 0)
    {
      /* This would happen at most every few sec (with auto-pings) and is definitely a rather interesting
       * situation though not an error; INFO is suitable. */
      const auto q_size = m_snd_pending_payloads_q.size();
      FLOW_LOG_INFO("Socket stream [" << *this << "]: Wanted to send low-level payload: "
                    "blob of size [" << orig_blob.size() << "] located @ [" << orig_blob.data() << "]; "
                    "result was would-block for all of its bytes (either because blocked-queue was non-empty "
                    "already, or it was empty, but all of payload's bytes would-block at this time).  "
                    "Therefore dropping payload (done for auto-pings at least).  Out-queue size remains "
                    "[" << q_size << "].");

      /* We won't enqueue it, so there's nothing more to do, but careful in deciding what to return:
       * If the queue is empty, we promised we would return true.  If the queue is not empty, we promised
       * we would return false.  Whether that's what we should do is subtly questionable, but as of this
       * writing what we return when avoid_qing==true is immaterial (is ignored). */
      return q_size == 0;
    }
    // else if (n_sent_or_zero > 0) (but not == orig_blob.size())

    // This is even more interesting; definitely INFO as well.
    FLOW_LOG_INFO("Socket stream [" << *this << "]: Wanted to send low-level payload: "
                  "blob of size [" << orig_blob.size() << "] located @ [" << orig_blob.data() << "]; "
                  "result was would-block for all but [" << n_sent_or_zero << "] of its bytes (blocked-queue "
                  "was empty, so nb-send was attmpted, and some -- but not all -- of payload's bytes "
                  "would-block at this time).  We cannot \"get back\" the sent bytes and thus are forced "
                  "to queue the remaining ones (would have dropped payload if all the bytes would-block).");
    // Fall-through.
  } // if (avoid_qing)
  // else if (!avoid_qing) { Fall through. }

  auto new_low_lvl_payload = boost::movelib::make_unique<Snd_low_lvl_payload>();
  if (n_sent_or_zero == 0)
  {
    new_low_lvl_payload->m_hndl_or_null = hndl_or_null;
  }
  // else { Leave it as null.  Even if (!hndl_or_null.null()): 1+ bytes were sent OK => so was hndl_or_null. }

  new_low_lvl_payload->m_blob = Blob(get_logger());

  /* Allocate N bytes; copy N bytes from area referred to by new_blob.  Start at 1st unsent byte (possibly 1st byte).
   * This is the first and only place we copy the source blob (not counting the transmission into kernel buffer); we
   * have tried our best to synchronously send all of `new_blob`, which would've avoided getting here and this copy.
   * Now we have no choice.  As discussed in the class doc header, probabilistically speaking we should rarely (if
   * ever) get here (and do this annoying alloc, and copy, and later dealloc) under normal operation of both sides. */
  const auto& new_blob = orig_blob + n_sent_or_zero;
  new_low_lvl_payload->m_blob.assign_copy(new_blob);

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Want to send pending-from-would-block low-level payload: "
                 "handle [" << new_low_lvl_payload->m_hndl_or_null << "] with "
                 "blob of size [" << new_low_lvl_payload->m_blob.size() << "] "
                 "located @ [" << new_blob.data() << "]; "
                 "created blob copy @ [" << new_low_lvl_payload->m_blob.const_buffer().data() << "]; "
                 "enqueued to out-queue which is now of size [" << (m_snd_pending_payloads_q.size() + 1) << "].");

  m_snd_pending_payloads_q.emplace(std::move(new_low_lvl_payload)); // Push a new Snd_low_lvl_payload::Ptr.

  if (m_snd_pending_payloads_q.size() == 1)
  {
    /* Queue was empty; now it isn't; so start the chain of async send head=>dequeue=>async send head=>dequeue=>....
     * (In our case "async send head" means asking (via m_snd_ev_wait_func) user to inform (via callback we pass
     * to m_snd_ev_wait_func) us when would-block has cleared, call snd_nb_write_low_lvl_payload() again....
     * If we were operating directly as a boost.asio async loop then the first part would just be
     * m_peer_socket->async_wait(); but we cannot do that; user does it for us, controlling what gets called when
     * synchronously.) */
    snd_async_write_q_head_payload();

    /* That immediately failed => m_snd_pending_err_code is truthy
     *   => Pipe-direction-ending error encountered; outgoing-direction pipe is finished forevermore. => return true;
     * That did not fail ("async"-wait for writable begins) => m_snd_pending_err_code is falsy
     *   => Outgoing-direction pipe has pending queued stuff. => return false; */
    return bool(m_snd_pending_err_code);
  }
  // else
  assert(!m_snd_pending_err_code);
  return false; // Outgoing-direction pipe has (even more) pending queued stuff; nothing to do about it for now.
} // Native_socket_stream::Impl::snd_sync_write_or_q_payload()

size_t Native_socket_stream::Impl::snd_nb_write_low_lvl_payload(Native_handle hndl_or_null,
                                                                const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::Lock_guard;
  using asio_local_stream_socket::nb_write_some_with_native_handle;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  /* Result semantics (reminder of what we promised in contract):
   *   Partial or total success in non-blockingly sending blob+handle: >= 1; falsy *err_code.
   *   No success in sending blob+handle, because it would-block (not fatal): == 0; falsy *err_code.
   *   No success in sending blob+handle, because fatal error: == 0, truthy *err_code. */
  size_t n_sent_or_zero = 0;

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

    if (m_peer_socket)
    {
      if (hndl_or_null.null())
      {
        /* hndl_or_null is in fact null, so we can just use boost.asio's normal non-blocking send (non_blocking(true),
         * write_some()). */

        /* First set non-blocking mode.  (Subtlety: We could use Linux extension per-call MSG_DONTWAIT flag instead;
         * but this way is fully portable.  For posterity: to do that, use m_peer_socket->send(), which is identical to
         * write_some() but has an overload accepting flags to OS send() call, where one could supply MSG_DONTWAIT.
         * boost.asio lacks a constant for it, but we could just use actual MSG_DONTWAIT; of course stylistically that's
         * not as nice and suggests lesser portability.) */
        if (!m_peer_socket->non_blocking()) // This is fast (it doesn't run system calls but uses a cached value).
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

          FLOW_LOG_TRACE("Writing low-level blob directly via boost.asio (blob details logged above hopefully).");

          n_sent_or_zero = m_peer_socket->write_some(blob, *err_code);
        }
        // else if (*err_code) { *err_code is truthy; n_sent_or_zero == 0; cool. }
      } // if (hndl_or_null.null())
      else // if (!hndl_or_null.null())
      {
        /* Per contract, nb_write_some_with_native_handle() is identical to setting non_blocking(true) and attempting
         * write_some(orig_blob) -- except if it's able to send even 1 byte it'll also have sent through hndl_or_null.
         * When we say identical we mean identical result semantics along with everything else. */
        n_sent_or_zero = nb_write_some_with_native_handle(get_logger(), m_peer_socket.get(),
                                                          hndl_or_null, blob, err_code);
        // That should have TRACE-logged stuff, so we won't (it's our function).
      }

      /* Almost home free; but our result semantics are a little different from the low-level-write functions'.
       *
       * Plus, if we just discovered the connection is hosed, do whatever's needed with that. */
      assert(((!*err_code) && (n_sent_or_zero != 0))
             || (*err_code && (n_sent_or_zero == 0)));
      if (*err_code == boost::asio::error::would_block)
      {
        err_code->clear();
        // *err_code is falsy; n_sent_or_zero == 0; cool.
      }
      else if (*err_code)
      {
        // True-blue system error.  Kill off *m_peer_socket: might as well give it back to the system (it's a resource).

        // Closes peer socket to the (hosed anyway) connection; including ::close(m_peer_socket->native_handle()).
        m_peer_socket.reset();

        // *err_code is truthy; n_sent_or_zero == 0; cool.
      }
      // else if (!*err_code) { *err_code is falsy; n_sent_or_zero >= 1; cool. }
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND;
    }
  } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

  assert((!*err_code)
         || (n_sent_or_zero == 0)); // && *err_code

  if (*err_code)
  {
    FLOW_LOG_TRACE("Sent nothing due to error [" << *err_code << "] [" << err_code->message() << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Send: no error.  Was able to send [" << n_sent_or_zero << "] of [" << blob.size() << "] bytes.");
    if (!hndl_or_null.null())
    {
      FLOW_LOG_TRACE("Able to send the native handle? = [" << (n_sent_or_zero != 0) << "].");
    }
  }

  return n_sent_or_zero;
} // Native_socket_stream::Impl::snd_nb_write_low_lvl_payload()

void Native_socket_stream::Impl::snd_async_write_q_head_payload()
{
  using asio_local_stream_socket::async_write_with_native_handle;
  using util::Task;
  using flow::util::Lock_guard;
  using boost::asio::async_write;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert((!m_snd_pending_payloads_q.empty()) && "Contract is stuff is queued to be async-sent.  Bug?");
  assert((!m_snd_pending_err_code) && "Pipe must not be pre-hosed by contract.");

  /* Conceptually we'd like to do m_peer_socket->async_wait(writable, F), where F() would perform
   * snd_nb_write_low_lvl_payload() (nb-send over m_peer_socket).  However this is the sync_io pattern, so
   * the user will be performing the conceptual async_wait() for us.  We must ask them to do so
   * via m_snd_ev_wait_func(), giving them m_peer_socket's FD -- m_snd_ev_wait_hndl_peer_socket -- to wait-on. */

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock(m_peer_socket_mutex);

    if (m_peer_socket)
    {
      /* @todo Previously, when we ran our own boost.asio loop in a separate thread mandatorily (non-sync_io pattern),
       * we did either boost::asio::async_write() (stubborn write, no handle to send) or
       * asio_local_stream_socket::async_write_with_native_handle (same but with handle to send).
       * Either one would do the async_wait()->nb-write->async_wait()->... chain for us.  Now with sync_io we
       * manually split it up into our version of "async"-wait->nb-write->....  However other than the
       * "async-wait" mechanism itself, the logic is the same.  The to-do would be to perhaps provide
       * those functions in such a way as to work with the sync_io way of "async"-waiting, without getting rid
       * of the publicly useful non-sync_io API (some layered design, confined to
       * asio_local_stream_socket::*).  The logic would not need to be repeated and maintained in two places;
       * as of this writing the non-sync_io version has become for direct public use (by user) only anyway;
       * two birds one stone to have both use cases use the same core logic code. */

      m_snd_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                         true, // Wait for write.
                         // Once writable do this:
                         boost::make_shared<Task>
                           ([this]() { snd_on_ev_peer_socket_writable_or_error(); }));
      return;
    }
    // else:
  } // Lock_guard peer_socket_lock(m_peer_socket_mutex)

  m_snd_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND;

  /* Style note: I (ygoldfel) was tempted to make snd_on_ev_peer_socket_writable_or_error() a closure right in here,
   * which saves boiler-plate lines, but subjectively the reading flow seemed too gnarly here.  Typically I would have
   * done it that other way though; in fact originally that's how I had it, but it proved too gnarly over time.
   * Among other things it's nice to have the on-sent handler appear right *below* the async-op that takes it as
   * an arg; *above* is dicey to read. */
} // Native_socket_stream::Impl::snd_async_write_q_head_payload()

void Native_socket_stream::Impl::snd_on_ev_peer_socket_writable_or_error()
{
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: User-performed wait-for-writable finished (writable or error, "
                 "we do not know which yet).  We endeavour to send->pop->send->... as much of the queue as we "
                 "can until would-block or total success.");

  assert((!m_snd_pending_payloads_q.empty()) && "Send-queue should not be touched while async-write of head is going.");
  assert((!m_snd_pending_err_code) && "Send error would only be detected by us.  Bug?");

  // Let's do as much as we can.
  bool would_block = false;
  do
  {
    auto& low_lvl_payload = *m_snd_pending_payloads_q.front();
    auto& hndl_or_null = low_lvl_payload.m_hndl_or_null;
    auto& low_lvl_blob = low_lvl_payload.m_blob;
    auto low_lvl_blob_view = low_lvl_blob.const_buffer();

    FLOW_LOG_TRACE("Socket stream [" << *this << "]: "
                   "Out-queue size is [" << m_snd_pending_payloads_q.size() << "]; "
                   "want to send handle [" << hndl_or_null << "] with "
                   "low-level blob of size [" << low_lvl_blob_view.size() << "] "
                   "located @ [" << low_lvl_blob_view.data() << "].");

    const auto n_sent_or_zero
      = snd_nb_write_low_lvl_payload(low_lvl_payload.m_hndl_or_null, low_lvl_blob_view, &m_snd_pending_err_code);
    if (m_snd_pending_err_code)
    {
      continue; // Get out of the loop.
    }
    // else

    if (n_sent_or_zero == low_lvl_blob_view.size())
    {
      // Everything was sent nicely!
      m_snd_pending_payloads_q.pop(); // This should dealloc low_lvl_payload.m_blob in particular.
    }
    else // if (n_sent_or_zero != low_lvl_payload.m_blob.size())
    {
      /* Some or all of the payload could not be sent (it would-block if we tried to send the rest now).
       * This is similar to snd_sync_write_or_q_payload(), but we needn't enqueue it, as it's already enqueued;
       * just "edit" it in-place as needed. */
      if (n_sent_or_zero != 0)
      {
        // Even if (!m_hndl_or_null.null()): 1 bytes were sent => so was the native handle.
        low_lvl_payload.m_hndl_or_null = Native_handle();
        /* Slide its .begin() to the right by n_sent_or_zero (might be no-op if was not writable after all).
         * Note internally it's just a size_t +=; no realloc or anything. */
        low_lvl_payload.m_blob.start_past_prefix_inc(n_sent_or_zero);
      }
      // else if (n_sent_or_zero == 0) { Nothing was sent, so no edits needed to low_lvl_payload. }
      would_block = true; // Stop; would-block if we tried more.
    }
  }
  while ((!m_snd_pending_payloads_q.empty()) && (!would_block) && (!m_snd_pending_err_code));

  // Careful!  This must be done before the `if` sequence below, as it can make m_snd_pending_err_code truthy after all.
  if ((!m_snd_pending_err_code) && (!m_snd_pending_payloads_q.empty()))
  {
    FLOW_LOG_TRACE("Out-queue has not been emptied.  Must keep async-send chain going.");

    // Continue the chain (this guy "asynchronously" brought us here in the first place).
    snd_async_write_q_head_payload();
    /* To be clear: queue can now only become empty in "async" handler, not synchronously here.
     *
     * Reasoning sanity check: How could m_snd_pending_err_code become truthy here yet falsy (success)
     * through the do/while() loop above?  Answer: The writes could work; then after the last such write,
     * but before snd_async_write_q_head_payload() -- which needs an m_*peer_socket member to indicate
     * connection is still healthy, to initiate the user-executed wait-for-writable -- incoming-direction processing
     * could have hosed m_*peer_socket.  E.g., it tried to nb-receive and exposed newly-arrived error condition. */
  }
  /* ^-- if ((!m_snd_pending_err_code) && (!m_snd_pending_payloads_q.empty()))
   * (but m_snd_pending_err_code may have become truthy inside). */

  // Lastly deal with possibly having to fire async_end_sending() completion handler.

  bool invoke_on_done = false;
  if (m_snd_pending_err_code)
  {
    invoke_on_done = !m_snd_pending_on_last_send_done_func_or_empty.empty();
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User-performed wait-for-writable reported completion; "
                     "wanted to nb-send any queued data and possibly initiated another wait-for-writable; "
                     "got error during an nb-send or when initiating wait; TRACE details above.  "
                     "Error code details follow: "
                     "[" << m_snd_pending_err_code << "] [" << m_snd_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any.  "
                     "Will run graceful-sends-close completion handler? = ["  << invoke_on_done << "].");

    assert((!m_snd_pending_payloads_q.empty()) && "Opportunistic sanity check.");
  }
  else if (m_snd_pending_payloads_q.empty()) // && (!m_snd_pending_err_code)
  {
    FLOW_LOG_TRACE("Out-queue has been emptied.");

    if (!m_snd_pending_on_last_send_done_func_or_empty.empty())
    {
      // INFO-log is okay, as this occurs at most once per *this.
      FLOW_LOG_INFO("Socket stream [" << *this << "]: "
                    "We sent graceful-close and any preceding user messages with success.  Will now inform user via "
                    "graceful-sends-close completion handler.");
      invoke_on_done = true;
    }
  } // else if (m_snd_pending_payloads_q.empty() && (!m_snd_pending_err_code))
  // else if ((!m_snd_pending_payloads_q.empty()) && (!m_snd_pending_err_code)) { Async-wait started. }

  if (invoke_on_done)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Executing end-sending completion handler now.");
    auto on_done_func = std::move(m_snd_pending_on_last_send_done_func_or_empty);
    m_snd_pending_on_last_send_done_func_or_empty.clear(); // For cleanliness, in case move() didn't do it.

    on_done_func(m_snd_pending_err_code);
    FLOW_LOG_TRACE("Handler completed.");
  }
} // Native_socket_stream::Impl::snd_on_ev_peer_socket_writable_or_error()

size_t Native_socket_stream::Impl::send_meta_blob_max_size() const
{
  return state_peer("send_meta_blob_max_size()") ? S_MAX_META_BLOB_LENGTH : 0;
}

size_t Native_socket_stream::Impl::send_blob_max_size() const
{
  return send_meta_blob_max_size();
}

} // namespace ipc::transport::sync_io
