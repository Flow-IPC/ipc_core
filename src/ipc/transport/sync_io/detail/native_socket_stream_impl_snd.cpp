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
#include "ipc/transport/native_socket_stream_cfg.hpp"
#include "ipc/transport/error.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::transport::sync_io
{

// Native_socket_stream_impl implementations (::snd_*() and send-API methods only).

bool Native_socket_stream_impl::start_send_native_handle_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  using util::Blob_const;

  if (!start_ops<Op::S_SND>(std::move(ev_wait_func)))
  {
    return false;
  }
  // else

  const auto protocol_ver_to_send_if_needed = m_protocol_negotiator.local_max_proto_ver_for_sending();
  assert(protocol_ver_to_send_if_needed != Protocol_negotiator::S_VER_UNKNOWN);

  assert((m_protocol_negotiator.local_max_proto_ver_for_sending() == Protocol_negotiator::S_VER_UNKNOWN)
         && "Protocol_negotiator not properly marking the once-only sending-out of protocol version?");
  assert((!m_snd_pending_err_code) && "We should be the first send-related transmission code possible.");

  /* As discussed in m_protocol_negotiator doc header and class doc header "Protocol negotiation" section:
   * send a special as-if-payload 1 (and no payload 2): no Native_handle; no meta-blob; and the "length"
   * field in payload 1 instead of any length stores protocol_ver_to_send_if_needed (sized appropriately).
   * By the way m_protocol_negotiator logged about the fact we're about to send it, so we can be pretty quiet.
   *
   * The mechanics here are very similar to how send_native_handle() invokes snd_sync_write_or_q_payload().
   * Keeping comments light, except where something different applies (as of this writing that's just: the
   * meaning of snd_sync_write_or_q_payload() return value). */

  const auto fake_meta_length_raw
    = static_cast<Native_socket_stream_cfg::low_lvl_payload_blob_length_t>(protocol_ver_to_send_if_needed);
  const Blob_const payload_blob{&fake_meta_length_raw, sizeof(fake_meta_length_raw)};

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Want to send protocol-negotiation info.  "
                 "About to send payload 1 of 1; "
                 "contains low-level blob of size [" << payload_blob.size() << "] "
                 "located @ [" << payload_blob.data() << "].");

  snd_sync_write_or_q_payload({}, payload_blob, {}, false);
  m_snd_stats.m_total_low_lvl_bytes += sizeof(fake_meta_length_raw);
  /* m_send_pending_err_code may have become truthy; just means next send_*()/whatever will emit that error.
   *
   * Otherwise: Either it inline-sent it (very likely), or it got queued.
   *            Either way: no error; let's get on with queuing-or-sending real stuff like send_*() payloads.
   * P.S. There's only 1 protocol version as of this writing, so there's no ambiguity, and we can just get on with
   * sending stuff right away.  This could change in the future.  See m_protocol_negotiator doc header for more. */

  /* If we bring back transport::Native_socket_stream::release() (currently that code path if `#if 0`d out;
   * like see the `#if 0`d reset_sync_io_setup()), then instead of assert()ing
   * that `protocol_ver_to_send_if_needed != Protocol_negotiator::S_VER_UNKNOWN` above, it would become an `if`,
   * and if that isn't the case then we'd just log the following and no-op. */
#if 0
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Wanted to send protocol-negotiation info; "
                 "but we've marked it as already-sent, even though we are in start_*_ops() in PEER state.  "
                 "Probably we come from a .release()d Native_socket_stream which has already done it; cool.");
#endif

  // See log_stats() doc header for basic background behind the logic here.
  if (m_snd_pending_err_code) // Note we've asserted it was not already truthy at the start.
  {
    snd_log_stats("start_send_native_handle_ops(): while sync-processing: proto-neg-send => snd-pipe hosed");
  }

  return true;
} // Native_socket_stream_impl::start_send_native_handle_ops()

bool Native_socket_stream_impl::start_send_blob_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_send_native_handle_ops(std::move(ev_wait_func));
}

bool Native_socket_stream_impl::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  return send_native_handle({}, blob, err_code);
}

bool Native_socket_stream_impl::send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                                                   Error_code* err_code)
{
  using util::Fine_duration;
  using util::Blob_const;
  using flow::util::buffers_dump_string;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, send_native_handle, hndl_or_null, meta_blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  if ((!op_started<Op::S_SND>("send_native_handle()")) || (!state_peer("send_native_handle()")))
  {
    err_code->clear();
    return false;
  }
  // else

  const bool was_hosed_already = bool(m_snd_pending_err_code);
  const size_t meta_size = meta_blob.size();
  assert(((!hndl_or_null.null()) || (meta_size != 0))
         && "Native_socket_stream::send_blob() blob must have length 1+; "
              "Native_socket_stream::send_native_handle() must have same or non-null hndl_or_null or both.");

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Will send handle [" << hndl_or_null << "] with "
                 "meta-blob of size [" << meta_size << "].");
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
  else if (meta_size > Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH)
  {
    *err_code = error::Code::S_INVALID_ARGUMENT;
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Send: User argument length [" << meta_size << "] "
                     "exceeds limit [" << Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH << "].");
    // More WARNING logging below; just wanted to show those particular details also.
  }
  else if (was_hosed_already) // && (!m_snd_finished) && (meta_size OK)
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
  else // if (!m_snd_finished) && (meta_size OK) && (!was_hosed_already)
  {
    /* For Protocol_byte_stream:
     *   As seen in protocol definition in class doc header, payload 1 contains handle, if any, and the length of
     *   the blob in payload 2 (or 0 if no payload 2).  Set up the meta-length thing on the stack before entering
     *   critical section.  Endianness stays constant on the machine, so don't worry about that.
     *   @todo Actually it would be (1) more forward-compatible and (2) consistent to do the same as how
     *   the structured layer encodes UUIDs -- mandating a pre-send conversion native->little-endian, post-send
     *   conversion backwards.  The forward-compatibility is for when this mechanism is expanded to inter-machine IPC;
     *   while noting that the conversion is actually a no-op given our known hardware, so no real perf penalty.
     * For Protocol_pkt_stream:
     *   Actually it is all almost the same (on the send side; receipt algorithm is more different between the two).
     *   The differences:
     *     - Payload 1 and payload 2 (if any) *must* be in the same OS-write call.  Since we don't want to first copy
     *       meta_blob's contents into some temp buffer, we must use scatter/gather semantics.  Happily, though,
     *       the exact same thing can be done for Protocol_byte_stream; it does not have to be, but it is better, as
     *       the kernel locking involved in making syscalls can be surprisingly expensive under load.  So we can
     *       use the same OS-write call in both cases -- mandatory here but optional-but-faster for
     *       Protocol_byte_stream.
     *     - Since the OS maintains dgram boundaries for us, when payload 2 has 1+ bytes we still need not encode
     *       any length; we encode zero still. */
    using len_t = Native_socket_stream_cfg::low_lvl_payload_blob_length_t;
    len_t meta_length_raw;
    if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
    {
      meta_length_raw = 0;
    }
    else
    {
      meta_length_raw = static_cast<len_t>(meta_size);
    }
    // ^-- must stay on stack while snd_sync_write_or_q_payload() executes, and it will.  Will be copied if must queue.
    const Blob_const meta_length_blob{&meta_length_raw, sizeof(meta_length_raw)};

    // Payload 2, if any, consists of stuff we already have ready, namely simply meta_blob itself.  Nothing to do now.

    const auto logger_ptr = get_logger();
    if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_TRACE, get_log_component()))
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Socket stream [" << *this << "]: Wanted to send handle [" << hndl_or_null << "] with "
         "meta-blob of size [" << meta_size << "].  About to send payload 1 (includes handle above and "
         "low-level sub-blob of size [" << sizeof(len_t) << "] located @ [" << &meta_length_raw << "]).");
      if (meta_size != 0)
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Socket stream [" << *this << "]: ...plus: "
           "About to send payload 2 (low-level sub-blob of size [" << meta_size << "] "
           "located @ [" << meta_blob.data() << "]).");
      }
    }

    /* As the name indicates, this may synchronously finish it or queue up any parts instead to be done once
     * a would-block clears, when user informs of this past the present function's return.  If fatal error occurs
     * instead, then m_snd_pending_err_code shall become truthy.
     * (If meta_size is 0, meta_blob is basically as-if-default-cted Blob_const{} and will be ignored.) */
    snd_sync_write_or_q_payload(hndl_or_null, meta_length_blob, meta_blob, false);
    /* That may have returned `true` indicating everything (up to and including our 1-2 payloads) was synchronously
     * given to kernel successfuly; or this will never occur, because outgoing-pipe-ending error was encountered.
     * Since this is send_native_handle(), we do not care: there is no on-done
     * callback to invoke, as m_snd_finished is false, as *end_sending() has not been called yet. */

    *err_code = m_snd_pending_err_code; // Emit the new error if any.

    if (!*err_code)
    {
      // Successful user send.
      ++m_snd_stats.m_total_msgs;
      m_snd_stats.m_total_bytes += meta_size;
      m_snd_stats.m_total_low_lvl_bytes += (sizeof(len_t) + meta_size);
      m_snd_stats.m_histo_payload_sz.record_value(meta_size);
      if (!hndl_or_null.null()) { ++m_snd_stats.m_msgs_with_hndls; }
    }

    // Did it generate a new error?
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
  } /* else if (!m_snd_finished) && (meta_size OK) && (!m_snd_pending_err_code)
     *         (but m_snd_pending_err_code may have become truthy inside) */

  if (*err_code)
  {
    // At the end try to categorize nature of error.
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send handle [" << hndl_or_null << "] with "
                     "meta-blob of size [" << meta_size << "], but an error (not necessarily new error) "
                     "encountered on pipe or in user API args.  Error code details follow: "
                     "[" << *err_code << "] [" << err_code->message() << "];  "
                     "pipe hosed (sys/protocol error)? = "
                     "[" << ((*err_code != error::Code::S_INVALID_ARGUMENT)
                             && (*err_code != error::Code::S_SENDS_FINISHED_CANNOT_SEND)) << "]; "
                     "sending disabled by user? = "
                     "[" << (*err_code == error::Code::S_SENDS_FINISHED_CANNOT_SEND) << "].");
  }

  // See log_stats() doc header for basic background behind the logic here.
  if ((!was_hosed_already) && m_snd_pending_err_code)
  {
    snd_log_stats("send_native_handle(): while sync-processing snd-pipe hosed");
  }

  return true;
} // Native_socket_stream_impl::send_native_handle()

bool Native_socket_stream_impl::end_sending()
{
  return async_end_sending_impl(nullptr, {});
}

bool Native_socket_stream_impl::async_end_sending(Error_code* err_code,
                                                  flow::async::Task_asio_err&& on_done_func)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, async_end_sending, _1, std::move(on_done_func));
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // This guy either takes null/.empty(), or non-null/non-.empty(): it doesn't do the Flow-style error emission itself.
  return async_end_sending_impl(err_code, std::move(on_done_func));
} // Native_socket_stream_impl::async_end_sending()

bool Native_socket_stream_impl::async_end_sending_impl(Error_code* sync_err_code_ptr_or_null,
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

  bool became_hosed = false;
  m_snd_finished = true; // Cause future send_native_handle() to emit S_SENDS_FINISHED_CANNOT_SEND and return.

  bool qd; // Set to false to report results *now*: basically true <=> stuff is still queued to send.
  if (m_snd_pending_err_code)
  {
    qd = false; // There was outgoing-pipe-ending error detected before us; so should immediately report.
  }
  else
  {
    /* Prepare/send the payload per aforementioned (class doc header) strategy: no handle, and a 0x0000 integer.
     * Keeping comments light, as this is essentially a simplified version of send_native_handle(). */

    const auto ZERO_SIZE_RAW = static_cast<Native_socket_stream_cfg::low_lvl_payload_blob_length_t>(0);
    const Blob_const blob_with_0{&ZERO_SIZE_RAW, sizeof(ZERO_SIZE_RAW)};

    /* snd_sync_write_or_q_payload():
     * Returns true => out-queue flushed successfully; or error detected.
     *   => report synchronously now.  (Unless they don't care about any such report.)
     * Returns false => out-queue has stuff in it and will continue to, until transport is writable.
     *   => cannot report completion yet. */

    qd = !snd_sync_write_or_q_payload({}, blob_with_0, {}, false);
    became_hosed = bool(m_snd_pending_err_code);

    m_snd_stats.m_total_low_lvl_bytes += sizeof(ZERO_SIZE_RAW);
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

  // See log_stats() doc header for basic background behind the logic here.
  if (became_hosed)
  {
    snd_log_stats("async_end_sending_impl(): while sync-processing snd-pipe hosed");
  }

  return true;
} // Native_socket_stream_impl::async_end_sending_impl()

bool Native_socket_stream_impl::auto_ping(util::Fine_duration period)
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
   * Keeping comments somewhat light, as this is essentially a simplified version of send_native_handle()
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

  const Blob_const blob_with_ff{&Native_socket_stream_cfg::S_PING_SENTINEL,
                                sizeof(Native_socket_stream_cfg::S_PING_SENTINEL)};

  /* Important: avoid_qing=true for reasons explained in its doc header.  Namely:
   * If blob_with_ff would-block entirely, then there are already data that would signal-non-idleness sitting
   * in the kernel buffer, so the auto-ping can be safely dropped in that case. */
  snd_sync_write_or_q_payload({}, blob_with_ff, {}, true);

  ++m_snd_stats.m_auto_pings; // Count even if dropped due to avoid_qing or error below.
  m_snd_stats.m_total_low_lvl_bytes += sizeof(Native_socket_stream_cfg::S_PING_SENTINEL);

  if (m_snd_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send initial auto-ping but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not schedule periodic auto-pings.");

    /* See log_stats() doc header for basic background behind the logic here.
     * Note we would've returned already had m_snd_pending_err_code been already truthy at the start. */
    snd_log_stats("auto_ping(): while sync-processing: auto-ping-send => snd-pipe hosed");

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
} // Native_socket_stream_impl::auto_ping()

void Native_socket_stream_impl::snd_on_ev_auto_ping_now_timer_fired()
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

  const Blob_const blob_with_ff{&Native_socket_stream_cfg::S_PING_SENTINEL,
                                sizeof(Native_socket_stream_cfg::S_PING_SENTINEL)};

  snd_sync_write_or_q_payload({}, blob_with_ff, {}, true);

  ++m_snd_stats.m_auto_pings;
  m_snd_stats.m_total_low_lvl_bytes += sizeof(Native_socket_stream_cfg::S_PING_SENTINEL);

  if (m_snd_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to send non-initial auto-ping but detected error "
                     "synchronously.  "
                     "Error code details follow: [" << m_snd_pending_err_code << "] "
                     "[" << m_snd_pending_err_code.message() << "].  "
                     "Saved error code to return in next user send attempt if any; otherwise ignoring; "
                     "will not continue scheduling periodic auto-pings.");

    /* See log_stats() doc header for basic background behind the logic here.
     * Note we would've returned already had m_snd_pending_err_code been already truthy at the start. */
    snd_log_stats("snd_on_ev_auto_ping_now_timer_fired(): while attempting to send auto-ping snd-pipe hosed");
    return;
  }
  // else

  m_snd_ev_wait_func(&m_snd_ev_wait_hndl_auto_ping_timer_fired_peer,
                     false, // Wait for read.
                     boost::make_shared<Task>
                       ([this]() { snd_on_ev_auto_ping_now_timer_fired(); }));
  m_snd_auto_ping_timer.expires_after(m_snd_auto_ping_period);
  m_timer_worker.timer_async_wait(&m_snd_auto_ping_timer, m_snd_auto_ping_timer_fired_peer);
} // Native_socket_stream_impl::snd_on_ev_auto_ping_now_timer_fired()

bool Native_socket_stream_impl::snd_sync_write_or_q_payload(Native_handle hndl_or_null, const util::Blob_const& blob1,
                                                            const util::Blob_const& blob2_or_none, bool avoid_qing)
{
  using flow::util::Blob;
  using util::Blob_const;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert((!m_snd_pending_err_code) && "Pipe must not be pre-hosed by contract.");

  size_t n_sent_or_zero;
  if (m_snd_pending_payloads_q.empty())
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Want to send low-level payload(s): "
                   "handle [" << hndl_or_null << "]; "
                   "payload 1 sized [" << blob1.size() << "] @ [" << blob1.data() << "]; "
                   "payload 2 sized [" << blob2_or_none.size() << "] @ "
                   "[" << ((blob2_or_none.size() != 0) ? blob2_or_none.data() : nullptr) << "].  "
                   "No write is pending so proceeding immediately.  "
                   "Will drop if all of it would-block? = [" << avoid_qing << "].");

    n_sent_or_zero = snd_nb_write_low_lvl_payload(hndl_or_null, blob1, blob2_or_none, &m_snd_pending_err_code);
    if (m_snd_pending_err_code) // It will *not* emit would-block (will just return 0 but no error).
    {
      assert(n_sent_or_zero == 0);
      return true; // Pipe-direction-ending error encountered; outgoing-direction pipe is finished forevermore.
    }
    // else

    if (n_sent_or_zero == (blob1.size() + blob2_or_none.size()))
    {
      // Awesome: Mainstream case: We wrote the whole thing synchronously.
      return true; // Outgoing-direction pipe flushed.
      // ^-- No error.  Logged about success in snd_nb_write_low_lvl_payload().
    }
    // else if (n_sent_or_zero < [blob1+2 size]) { Fall through.  n_sent_or_zero is significant. }
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
   * the above as follows: If *all* of blob1+2 would need to be queued (queue was already non-empty, or it
   * was empty, and snd_nb_write_low_lvl_payload() yielded would-block for *all* of blob+2), then:
   * simply pretend like it was sent fine; and continue like nothing happened.  (See our doc header for
   * rationale.) */

  bool sent_none;
  if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
  {
    assert((n_sent_or_zero == 0)
           && "In message-boundary-respecting mode nevertheless OS-write indicated a partially successful "
                "write; this is thought to be impossible.  Bug?");
    sent_none = true;
  }
  else
  {
    sent_none = (n_sent_or_zero == 0);
  }

  if (avoid_qing)
  {
    assert(hndl_or_null.null()
           && "Internal bug?  Do not ask to drop a payload with a native handle inside under any circumstances.");
    if (sent_none) // Always true if S_USE_OS_DGRAM_SUPPORT; hopefully optimizer will pick up on that.
    {
      /* This would happen at most every few sec (with auto-pings) and is definitely a rather interesting
       * situation though not an error; INFO is suitable. */
      const auto q_size = m_snd_pending_payloads_q.size();
      FLOW_LOG_INFO("Socket stream [" << *this << "]: Want to send low-level payload(s): "
                    "handle [" << hndl_or_null << "]; "
                    "payload 1 sized [" << blob1.size() << "] @ [" << blob1.data() << "]; "
                    "payload 2 sized [" << blob2_or_none.size() << "] @ "
                    "[" << ((blob2_or_none.size() != 0) ? blob2_or_none.data() : nullptr) << "]; "
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
    // else if (n_sent_or_zero > 0) (but not == blob1.size + blob2_or_none.size()):

    if constexpr(!Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
    {
      // This is even more interesting that what would've led to the preceding INFO msg; definitely INFO as well.
      FLOW_LOG_INFO("Socket stream [" << *this << "]: Want to send low-level payload(s): "
                    "handle [" << hndl_or_null << "]; "
                    "payload 1 sized [" << blob1.size() << "] @ [" << blob1.data() << "]; "
                    "payload 2 sized [" << blob2_or_none.size() << "] @ "
                    "[" << ((blob2_or_none.size() != 0) ? blob2_or_none.data() : nullptr) << "]; "
                    "result was would-block for all but [" << n_sent_or_zero << "] of its bytes (blocked-queue "
                    "was empty, so nb-send was attmpted, and some -- but not all -- of payload's bytes "
                    "would-block at this time).  We cannot \"get back\" the sent bytes and thus are forced "
                    "to queue the remaining ones (would have dropped payload if all the bytes would-block).");
      // Fall-through.
    }
    // else if constexpr(S_USE_OS_DGRAM_SUPPORT) { Cannot have reached here; see assert() earlier. }
  } // if (avoid_qing)
  // else if (!avoid_qing) { Fall through. }

  auto new_low_lvl_payload = boost::movelib::make_unique<Snd_low_lvl_payload>();
  if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
  {
    // `sent_none` has already been assert()ed; so:
    new_low_lvl_payload->m_hndl_or_null = hndl_or_null;
  }
  else // if constexpr(!S_USE_OS_DGRAM_SUPPORT)
  {
    if (sent_none)
    {
      new_low_lvl_payload->m_hndl_or_null = hndl_or_null;
    }
    // else { Leave it as null.  Even if (!hndl_or_null.null()): 1+ bytes were sent OK => so was hndl_or_null. }
  }

  /* Allocate N bytes; copy N bytes into there from blob1 and/or blob2_or_none.  Start at 1st unsent byte (possibly 1st
   * byte).  This is the first and only place we copy the source blob (not counting the transmission into kernel
   * buffer); we have tried our best to synchronously send all of N, which would've avoided getting here and this copy.
   * Now we have no choice.  As discussed in the class doc header, probabilistically speaking we should rarely (if
   * ever) get here (and do this annoying alloc, and copy, and later dealloc) under normal operation of both sides. */

  if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
  {
    // In dgram mode, partial writes are not possible; so simply copy blob1 and blob2_or_none (if any) into new blob.
    auto& tgt_blob = new_low_lvl_payload->m_blob = Blob{get_logger(), blob1.size() + blob2_or_none.size()};
    tgt_blob.emplace_copy
      (tgt_blob.emplace_copy(tgt_blob.begin(), blob1), // Returns 1-after last-written byte.
       blob2_or_none); // If blob2_or_none.size() == 0, this outer .emplace_copy() is a no-op.
  }
  else
  {
    /* In byte-stream mode, partial writes are possible; so this is a bit trickier.
     * Find the spot in blob1 or blob2_or_none past a total of n_sent_or_zero bytes starting with blob1.begin().
     * Then copy-over the rest of that blob; and if that blob was blob1 then also all of blob2_or_none.
     *
     * Reminder: Blob_const B + size_t N = all of B except without the first N bytes (shifts-right .data() += N,
     * decrements .size() -= N). */
    const auto n_unsent = static_cast<size_t>(blob1.size() + blob2_or_none.size() - n_sent_or_zero);
    auto& tgt_blob = new_low_lvl_payload->m_blob = Blob{get_logger(), n_unsent};
    if (n_sent_or_zero < blob1.size())
    {
      tgt_blob.emplace_copy
        (tgt_blob.emplace_copy(tgt_blob.begin(), blob1 + n_sent_or_zero), // Returns 1-after last-written byte.
         blob2_or_none); // If blob2_or_none.size() == 0, this outer .emplace_copy() is a no-op.

      // Sanity check: If (n_sent_or_zero == 0), then this reduces to exactly the `if (S_USE_OS_DGRAM_SUPPORT)` code.
    }
    else // if (n_sent_or_zero >= blob1.size()) [All of blob1 was sent; and possibly some of blob2_or_none was too.]
    {
      tgt_blob.emplace_copy(tgt_blob.begin(), blob2_or_none + (blob2_or_none.size() - n_unsent));
    }
  } // if constexpr(!S_USE_OS_DGRAM_SUPPORT)

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Want to send pending-from-would-block low-level payload: "
                 "handle [" << new_low_lvl_payload->m_hndl_or_null << "] with "
                 "new blob of size [" << new_low_lvl_payload->m_blob.size() << "] "
                 "located @ [" << new_low_lvl_payload->m_blob.const_buffer().data() << "]; "
                 "enqueued to out-queue which is now of size [" << (m_snd_pending_payloads_q.size() + 1) << "].");

  m_snd_pending_payloads_q.emplace(std::move(new_low_lvl_payload)); // Push a new Snd_low_lvl_payload::Ptr.

  ++m_snd_stats.m_would_block_count;
  const auto q_size = m_snd_pending_payloads_q.size();
  m_snd_stats.m_snd_q_depth = q_size;
  flow::util::stat::update_hi_wmark(&m_snd_stats.m_snd_q_hi_wmark, q_size);

  if (q_size == 1)
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
} // Native_socket_stream_impl::snd_sync_write_or_q_payload()

size_t Native_socket_stream_impl::snd_nb_write_low_lvl_payload(Native_handle hndl_or_null,
                                                               const util::Blob_const& blob1,
                                                               const util::Blob_const& blob2_or_none,
                                                               Error_code* err_code)
{
  using util::Blob_const;
  using asio_local_stream_socket::nb_write_some_with_native_handle;
  using flow::util::Lock_guard;
  using boost::array;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  /* Result semantics (reminder of what we promised in contract):
   *   Partial or total success in non-blockingly sending blob+handle: >= 1; falsy *err_code.
   *   No success in sending blob+handle, because it would-block (not fatal): == 0; falsy *err_code.
   *   No success in sending blob+handle, because fatal error: == 0, truthy *err_code. */
  size_t n_sent_or_zero = 0;

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

    if (m_peer_socket)
    {
      if (hndl_or_null.null())
      {
        /* hndl_or_null is in fact null, so we can just use boost.asio's normal non-blocking send (non_blocking(true),
         * send()). */

        /* First set non-blocking mode.  (Subtlety: We could use Linux extension per-call MSG_DONTWAIT flag instead;
         * but this way is fully portable.  For posterity: to do that, flag MSG_DONTWAIT to ->send().
         * boost.asio lacks a constant for it, but we could just use actual MSG_DONTWAIT; of course stylistically that's
         * not as nice and suggests lesser portability.)  (Subtlety: Peer_socket<Protocol_pkt_stream> lacks
         * .write_some(), while Peer_socket<Protocol_byte_stream> has it; but they both have
         * .send(bufs, flags, err_code); so use that.) */
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

          /* Perf subtlety: If blob2_or_none is "none" (empty), then specifying blob1
           * alone (as opposed to a 2-container with an empty 2nd element) subtly chooses a compile-time-faster template
           * impl of ->send().  So do that if relevant, even though the code is less
           * elegant-looking here.  Also the best choice (for another compile-time-decided optimization) for the
           * 2-container (if relevant) is {std|boost}::array<2>.  So use that if relevant. */

          if (blob2_or_none.size() == 0)
          {
            n_sent_or_zero = m_peer_socket->send(blob1, 0, *err_code);
          }
          else
          {
            array<Blob_const, 2> buf_seq = { blob1, blob2_or_none };
            n_sent_or_zero = m_peer_socket->send(buf_seq, 0, *err_code);
          }
        }
        // else if (*err_code) { *err_code is truthy; n_sent_or_zero == 0; cool. }
      } // if (hndl_or_null.null())
      else // if (!hndl_or_null.null())
      {
        /* Per contract, nb_write_some_with_native_handle() is identical to setting non_blocking(true) and attempting
         * send(<buffer sequence below>) -- except if it's able to send even 1 byte it'll also have sent
         * through hndl_or_null.  When we say identical we mean identical result semantics along with everything else.
         *
         * Perf subtlety: same as above with m_peer_socket->send(). */
        if (blob2_or_none.size() == 0)
        {
          n_sent_or_zero = nb_write_some_with_native_handle<Native_socket_stream_cfg::Protocol>
                             (get_logger(), m_peer_socket.get(), hndl_or_null, blob1, err_code);
        }
        else
        {
          array<Blob_const, 2> buf_seq = { blob1, blob2_or_none };
          n_sent_or_zero = nb_write_some_with_native_handle<Native_socket_stream_cfg::Protocol>
                             (get_logger(), m_peer_socket.get(), hndl_or_null, buf_seq, err_code);
        }
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
        /* True-blue system error.  Kill off *m_peer_socket (connection hosed).  We could simply nullify it, which would
         * give it back to the system (it's a resource), but see m_peer_socket_hosed doc header for explanation as
         * to why we cannot, and why instead we transfer it to "death row" until dtor executes, at which
         * point ::close(m_peer_socket_hosed->native_handle()) will actually happen. */

        assert((!m_peer_socket_hosed) && "m_peer_socket_hosed must start as null and only become non-null once.  Bug?");
        m_peer_socket_hosed = std::move(m_peer_socket);
        assert((!m_peer_socket) && "Shocking unique_ptr misbehavior!");

        // *err_code is truthy; n_sent_or_zero == 0; cool.
      }
      // else if (!*err_code) { *err_code is falsy; n_sent_or_zero >= 1; cool. }
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND;
    }
  } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

  assert((!*err_code)
         || (n_sent_or_zero == 0)); // && *err_code

  if (*err_code)
  {
    FLOW_LOG_TRACE("Sent nothing due to error [" << *err_code << "] [" << err_code->message() << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Send: no error.  Was able to send [" << n_sent_or_zero << "] of "
                   "[" << (blob1.size() + blob2_or_none.size()) << "] bytes.");
    if (!hndl_or_null.null())
    {
      FLOW_LOG_TRACE("Able to send the native handle? = [" << (n_sent_or_zero != 0) << "].");
    }
  }

  return n_sent_or_zero;
} // Native_socket_stream_impl::snd_nb_write_low_lvl_payload()

void Native_socket_stream_impl::snd_async_write_q_head_payload()
{
  using util::Task;
  using flow::util::Lock_guard;

  // We comment liberally, but tactically, inline; but please read the strategy in the class doc header's impl section.

  assert((!m_snd_pending_payloads_q.empty()) && "Contract is stuff is queued to be async-sent.  Bug?");
  assert((!m_snd_pending_err_code) && "Pipe must not be pre-hosed by contract.");

  /* Conceptually we'd like to do m_peer_socket->async_wait(writable, F), where F() would perform
   * snd_nb_write_low_lvl_payload() (nb-send over m_peer_socket).  However this is the sync_io pattern, so
   * the user will be performing the conceptual async_wait() for us.  We must ask them to do so
   * via m_snd_ev_wait_func(), giving them m_peer_socket's FD -- m_snd_ev_wait_hndl_peer_socket -- to wait-on. */

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

    if (m_peer_socket)
    {
      m_snd_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                         true, // Wait for write.
                         // Once writable do this:
                         boost::make_shared<Task>
                           ([this]() { snd_on_ev_peer_socket_writable_or_error(); }));
      return;
    }
    // else:
  } // Lock_guard peer_socket_lock{m_peer_socket_mutex}

  m_snd_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND;

  /* Style note: I (ygoldfel) was tempted to make snd_on_ev_peer_socket_writable_or_error() a closure right in here,
   * which saves boiler-plate lines, but subjectively the reading flow seemed too gnarly here.  Typically I would have
   * done it that other way though; in fact originally that's how I had it, but it proved too gnarly over time.
   * Among other things it's nice to have the on-sent handler appear right *below* the async-op that takes it as
   * an arg; *above* is dicey to read. */
} // Native_socket_stream_impl::snd_async_write_q_head_payload()

void Native_socket_stream_impl::snd_on_ev_peer_socket_writable_or_error()
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
      = snd_nb_write_low_lvl_payload(low_lvl_payload.m_hndl_or_null, low_lvl_blob_view, {}, &m_snd_pending_err_code);
    if (m_snd_pending_err_code)
    {
      continue; // Get out of the loop.
    }
    // else

    if (n_sent_or_zero == low_lvl_blob_view.size())
    {
      // Everything was sent nicely!
      m_snd_pending_payloads_q.pop(); // This should dealloc low_lvl_payload.m_blob in particular.
      m_snd_stats.m_snd_q_depth = m_snd_pending_payloads_q.size();
    }
    else // if (n_sent_or_zero != low_lvl_payload.m_blob.size())
    {
      if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT)
      {
        assert((n_sent_or_zero == 0)
               && "In message-boundary-respecting mode nevertheless OS-write indicated a partially successful "
                    "write; this is thought to be impossible.  Bug?");
        // Could not send (any of) blob; will just have to retry the whole thing later.
      }
      else // if constexpr(!S_USE_OS_DGRAM_SUPPORT)
      {
        /* Some or all of the payload could not be sent (it would-block if we tried to send the rest now).
         * This is similar to snd_sync_write_or_q_payload(), but we needn't enqueue it, as it's already enqueued;
         * just "edit" it in-place as needed. */
        if (n_sent_or_zero != 0)
        {
          // Even if (!m_hndl_or_null.null()): 1 bytes were sent => so was the native handle.
          low_lvl_payload.m_hndl_or_null = {};
          /* Slide its .begin() to the right by n_sent_or_zero (might be no-op if was not writable after all).
           * Note internally it's just a size_t +=; no realloc or anything. */
          low_lvl_payload.m_blob.start_past_prefix_inc(n_sent_or_zero);
        }
        // else if (n_sent_or_zero == 0) { Nothing was sent, so no edits needed to low_lvl_payload. }
      } // else if constexpr(!S_USE_OS_DGRAM_SUPPORT)

      would_block = true; // Stop; would-block if we tried more.
    } // else if (n_sent_or_zero != low_lvl_blob_view.size())
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

  /* See log_stats() doc header for basic background behind the logic here.
   * Note we put this ahead of any handler-call to avoid reentrant hellishness. */
  if (m_snd_pending_err_code) // Note we've asserted it was not already truthy at the start.
  {
    snd_log_stats("snd_on_ev_peer_socket_writable_or_error(): while processing ev-ready snd-pipe hosed");
  }

  if (invoke_on_done)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Executing end-sending completion handler now.");
    auto on_done_func = std::move(m_snd_pending_on_last_send_done_func_or_empty);
    m_snd_pending_on_last_send_done_func_or_empty.clear(); // For cleanliness, in case move() didn't do it.

    on_done_func(m_snd_pending_err_code);
    FLOW_LOG_TRACE("Handler completed.");
  }
} // Native_socket_stream_impl::snd_on_ev_peer_socket_writable_or_error()

void Native_socket_stream_impl::snd_log_stats(util::String_view context) const
{
  using flow::util::stat::print;

  if (m_state == State::S_PEER)
  {
    FLOW_LOG_INFO("Socket stream [" << *this << "]: In context [" << context << "]: Stats: "
                  "snd[" << print(m_snd_stats) << "].");
  }
  // else { It'd all be zeroes anyway. }
}

size_t Native_socket_stream_impl::send_meta_blob_max_size() const
{
  return state_peer("send_meta_blob_max_size()") ? Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH : 0;
}

size_t Native_socket_stream_impl::send_blob_max_size() const
{
  return send_meta_blob_max_size();
}

stat::Blob_snd_stats Native_socket_stream_impl::blob_send_stats() const
{
  return m_snd_stats;
}

void Native_socket_stream_impl::blob_send_stats_reset()
{
  flow::util::stat::stats_reset(&m_snd_stats, Blob_snd_stats{Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH});
}

stat::Blob_snd_stats Native_socket_stream_impl::native_handle_send_stats() const
{
  return blob_send_stats();
}

void Native_socket_stream_impl::native_handle_send_stats_reset()
{
  blob_send_stats_reset();
}

} // namespace ipc::transport::sync_io
