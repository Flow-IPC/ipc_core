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

#include "ipc/transport/sync_io/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/detail/native_socket_stream_batch.hpp"
#include "ipc/transport/native_socket_stream_cfg.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <flow/util/util_fwd.hpp>

namespace ipc::transport::sync_io
{

// Native_socket_stream_impl template implementations (::rcv_*() and receive-API methods only).

template<typename Msg_resource>
bool Native_socket_stream_impl::async_receive_native_handle_batch(Native_handle_batch_in<Msg_resource>* batch,
                                                                  bool assume_would_block,
                                                                  Error_code* sync_err_code,
                                                                  flow::async::Task_asio_err&& on_done_func)
{
  return async_receive_batch_impl<Native_handle_batch_in<Msg_resource>, false>
           (batch, assume_would_block, sync_err_code, std::move(on_done_func));
}

template<typename Msg_resource>
bool Native_socket_stream_impl::async_receive_blob_batch(Blob_batch_in<Msg_resource>* batch,
                                                         bool assume_would_block,
                                                         Error_code* sync_err_code,
                                                         flow::async::Task_asio_err&& on_done_func)
{
  return async_receive_batch_impl<Blob_batch_in<Msg_resource>, true>
           (batch, assume_would_block, sync_err_code, std::move(on_done_func));
}

template<typename Batch, bool NO_HNDLS>
bool Native_socket_stream_impl::async_receive_batch_impl(Batch* batch,
                                                         bool assume_would_block,
                                                         Error_code* err_code,
                                                         flow::async::Task_asio_err&& on_done_func)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool,
                                     (async_receive_batch_impl<Batch, NO_HNDLS>),
                                     batch, assume_would_block, _1, std::move(on_done_func));
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.
  auto& sync_err_code = *err_code;

  assert(!on_done_func.empty());

  // Do these checks one time, similarly to the user-facing single-message async_receive_*().
  if ((!op_started<Op::S_RCV>("async_receive_batch_impl()"))
      || (!state_peer("async_receive_batch_impl()")))
  {
    return false;
  }
  // else
  if (m_rcv_user_request || m_rcv_user_batch_request)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Async-receive-batch for in-batch [" << *batch << "] "
                     "requested, but a preceding request is still in progress; the message has not arrived yet.  "
                     "Likely a user error, but who are we to judge?  Ignoring.");
    return false;
  }
  // else
  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: "
                     "User async-receive-batch for in-batch [" << *batch << "] requested: "
                     "Error already encountered earlier.  Emitting via sync-args.  Note this is not necessarily "
                     "the user acting oddly; in batch-receive case it is normal to detect an error like "
                     "graceful-close just after 1+ user in-messages -- in which case we cache the error but "
                     "emit the message(s) plus success; so we might be emitting the cached error now.");

    sync_err_code = m_rcv_pending_err_code;
    return true;
  }
  // else

  if (!batch->initialized())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Async-receive-batch for in-batch [" << *batch << "] "
                     "requested, but batch object is not initialized() (not all slots have been "
                     "prepare_target_payload()ed); emitting INVALID_ARGUMENT.");
    sync_err_code = error::Code::S_INVALID_ARGUMENT;
    return true;
  }
  // else
  if (batch->full())
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Async-receive-batch for in-batch [" << *batch << "] "
                     "requested, but batch object is already full; emitting INVALID_ARGUMENT.");
    sync_err_code = error::Code::S_INVALID_ARGUMENT;
    return true;
  }
  // else

  if constexpr(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
               && Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: User async-receive-batch (natively) request "
                   "for in-batch [" << *batch << "]; native-handles allowed? = [" << (!NO_HNDLS) << "]; "
                   "assume initial would-block? = [" << assume_would_block << "].");

    m_rcv_user_batch_request.emplace();
    m_rcv_user_batch_request->m_no_hndls = NO_HNDLS;
    m_rcv_user_batch_request->m_on_done_func = std::move(on_done_func);

    rcv_read_batch_from_pkt_stream<Batch>(batch, assume_would_block, &sync_err_code);

    if ((!sync_err_code)
        || (sync_err_code != error::Code::S_SYNC_IO_WOULD_BLOCK)) // Would-block continues op; other error ends it.
    {
      FLOW_LOG_TRACE("Async-request for in-batch [" << *batch << "] completed synchronously (result "
                     "[" << sync_err_code << "] [" << sync_err_code.message() << "]); emitting synchronously and "
                     "disregarding handler.");
      m_rcv_user_batch_request.reset();
    }
    // else { Other stuff logged enough. }
  }
  else // if constexpr(!(USE_OS_DGRAM_SUPPORT && USE_OS_DGRAM_BATCH_SUPPORT))
  {
    async_receive_batch_emulation<NO_HNDLS>(get_logger(), batch, assume_would_block,
                                            &sync_err_code, std::move(on_done_func),
                                            [this](auto&&... args)
    {
      if constexpr(NO_HNDLS)
      {
        async_receive_core(nullptr, std::forward<decltype(args)>(args)...);
      }
      else
      {
        async_receive_core(std::forward<decltype(args)>(args)...);
      }
    });
  } // else // if constexpr(!(USE_OS_DGRAM_SUPPORT && USE_OS_DGRAM_BATCH_SUPPORT))

  /* See log_stats() doc header for basic background behind the logic here.
   * (Per the "subtlety" in said doc header which we should avoid: on_done_func() won't be called anywhere above;
   * that would be later (if ever) and only if we hit would-block now.) */
  if (m_rcv_pending_err_code) // Note we would've returned already had it been already truthy at the start.
  {
    rcv_log_stats("async_receive_batch_impl(): while sync-processing rcv-pipe hosed");
  }

  return true;
} // Native_socket_stream_impl::async_receive_batch_impl()

template<typename Batch>
void Native_socket_stream_impl::rcv_read_batch_from_pkt_stream(Batch* batch,
                                                               bool assume_would_block,
                                                               Error_code* sync_err_code)
{
  using util::Task;
  using util::Blob_mutable;
  using flow::util::Lock_guard;

  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");
  assert(!m_rcv_pending_err_code);

  /* The below might look surprisingly brief and relatively simple given all the many subleties
   * in reading up-to-*batch-size messages essentially in one fell swoop (plus all the auto-pings and graceful-closes
   * and ... the user has no interest in).  That's really because Native_socket_stream_msg_batch_in::nb_read() does
   * all of that for us!  Well, really, it's more that that whole class a part of our impl -- we aren't really
   * separable -- while also giving the user a standard interface for loading target resources (buffers and such)
   * and checking the results (including batch->n_used(), the # of messages received).
   *
   * So get right to it, namely batch->nb_read(..., m_peer_socket.get(), ...) which will obtain any messages,
   * report any error including graceful-close, mark not-idleness if appropriate, even perform m_protocol_negotiator
   * negotiation if needed -- at least!  We just need to do the usual thing of locking the m_peer_socket-protecting
   * mutex and nullify that guy (details below obv) on fatal error. */

  // m_*peer_socket = (the only) shared data between our- and opposite-direction code.  Must lock (see class docs).
  {
    Lock_guard<decltype(m_peer_socket_mutex)> peer_socket_lock{m_peer_socket_mutex};

    if (m_peer_socket)
    {
      if (assume_would_block)
      {
        /* The following is identical to what would happen below if !assume_would_block, and ->nb_read() returned
         * would-block.  Note: It's easy enough, and good for correctness/maintainability, to instead only
         * check assume_would_block nearer the ->nb_read() and "simulate" its would-block by rigging
         * n_rcvd_or_zero, not_idle_on_would_block, etc.  We choose to forego that approach in favor of little perf
         * gains -- as assume_would_block=true exists for that reason; granted skipping the sys-call is easily the
         * biggest gain, but let's just go all the way.  @todo Reconsider.
         *
         * Comments light; see the assume_would_block=false clause below for comments, particularly w/r/t the
         * m_rcv_ev_wait_func() invocation. */
        FLOW_LOG_TRACE("Got nothing but would-block (pre-assumed) for in-batch [" << *batch << "].  "
                       "Awaiting readability.");

        m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket, false, boost::make_shared<Task>([this, batch]()
        {
          rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error<Batch>(batch);
        }));
        *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      } // if (assume_would_block)
      else // if (!assume_would_block)
      {
        bool not_idle_on_would_block;
        auto n_rcvd_or_zero = batch->n_used(); // This is subtracted just below.
#ifndef NDEBUG
        const bool ok =
#endif
        Native_socket_stream_msg_batch_in_privileged<Batch>{ *batch }
          .nb_read(get_logger(),
                   m_peer_socket.get(), &m_protocol_negotiator, m_rcv_user_batch_request->m_no_hndls,
                   &not_idle_on_would_block, &m_rcv_pending_err_code, 0, &m_rcv_stats);
        // That should have TRACE-logged (at least) much stuff, so we won't (it's our function).
        assert(ok && "By contract, do not pass us an already-full batch object; we should have already emitted "
                       "INVALID_ARGUMENT without calling this.");
        n_rcvd_or_zero = batch->n_used() - n_rcvd_or_zero;

        assert((m_rcv_pending_err_code != error::Code::S_INVALID_ARGUMENT)
               && "By contract, do not pass us an un-initialized() batch object; we should have already emitted "
                    "INVALID_ARGUMENT without calling this.");
        /* Caution!  batch->nb_read() has unique (versus single-message-transmitting APIs) semantics:
         *   - would_block: n_rcvd_or_zero = 0 (no messages received).  That's normal.
         *   - <other serious errors, including our own graceful-close S_RECEIVES_FINISHED_CANNOT_RECEIVE and
         *      low-level graceful-close `eof`>:
         *     - Possible: n_rcvd_or_zero = 0 (no messages received, only error [illegal dgram, etc.] or
         *                                     "error" namely one of 2 graceful-close types).  That's normal.
         *     - Possible: n_rcvd_or_zero > 0 (1+ messages received, then "error," as of this writing only
         *                                     one of 2 graceful-close types possible).
         *       That's exotic!  I.e., this can't happen with aforementioned typical APIs... but it can happen
         *       for us.  Actually it can *totally* happen for us!
         *       - Reminder: Our contract says that *we* will never emit 1+ messages *and* an error.
         *         To achieve this, we below cache the error (as usual) in m_rcv_pending_err_code... but
         *         we emit success *this* time.  Next time, though, they'll get the cached error. */

        if (m_rcv_pending_err_code == boost::asio::error::would_block)
        {
          assert(n_rcvd_or_zero == 0);

          FLOW_LOG_TRACE("Got nothing but would-block for in-batch [" << *batch << "].  Awaiting readability.");

          /* Subtlety: We pass `batch` through a capture to rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error()
           * which (on readability later) passes it to ourselves -- rcv_read_batch_from_pkt_stream() --
           * to try all this again.  We'd *like* to just shove it in m_rcv_user_batch_request instead, but
           * we can't; see struct Rcv_user_batch_request doc header which explains how/why + suggests possible stylistic
           * alternative(s).  (Spoiler alert: basic reason is, `batch` has a template-parameterized type.) */
          m_rcv_ev_wait_func(&m_ev_wait_hndl_peer_socket,
                             false, // Wait for read.
                             // Once readable do this:
                             boost::make_shared<Task>([this, batch]()
          {
            rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error<Batch>(batch);
          }));

          m_rcv_pending_err_code.clear(); // m_rcv_pending_err_code is falsy; n_rcvd_or_zero == 0; cool.
          // *We*, however, emit our advertised would-block thing.
          *sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;

          /* Subtlety possible with would-block: though we got 0 user messages, we may well have gotten auto-ping(s);
           * at least that's what it would be as of this writing; but in any case batch->nb_read() reports a thing
           * we can use for this specific case. */
          if (not_idle_on_would_block)
          {
            rcv_not_idle(); // Register activity <= got at least 1 in-message of any kind, no (fatal) error.
          }
        }
        else // if (m_rcv_pending_err_code != would_block)
        {
          if (m_rcv_pending_err_code)
          {
            if (m_rcv_pending_err_code != error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE)
            {
              /* True-blue error.  Kill off *m_peer_socket (connection hosed).  We could simply nullify it, which'd
               * give it back to the system (it's a resource), but see m_peer_socket_hosed doc header for explanation as
               * to why we cannot, and why instead we transfer it to "death row" until dtor executes, at which
               * point ::close(m_peer_socket_hosed->native_handle()) will actually happen. */

              assert((!m_peer_socket_hosed)
                     && "m_peer_socket_hosed must start as null and only become non-null once.  Bug?");
              m_peer_socket_hosed = std::move(m_peer_socket);
              assert((!m_peer_socket) && "Shocking unique_ptr misbehavior!");
            }
            /* else if (m_rcv_pending_err_code == ...EXCEEDS_USER_STORAGE)
             * { m_rcv_pending_err_code is truthy; and to our user the *in*-direction pipe is likely hosed (or
             *   will be on next receive attempt) -- see just below for all that.  However, we choose *not*
             *   to hose m_peer_socket, and therefore the *out*-direction pipe continues to operate if desired. } */

            // m_rcv_pending_err_code is truthy; n_rcvd_or_zero >= 0; now emit the proper thing per comment higher-up.
            if (n_rcvd_or_zero == 0)
            {
              *sync_err_code = m_rcv_pending_err_code;
            }
            else
            {
              FLOW_LOG_TRACE("Socket stream [" << *this << "]: User async-receive-batch request "
                             "for in-batch [" << *batch << "]; native-handles allowed? = "
                             "[" << (!m_rcv_user_batch_request->m_no_hndls) << "]: "
                             "Batch-read encountered error "
                             "[" << m_rcv_pending_err_code << "] [" << m_rcv_pending_err_code.message() << "] after "
                             "[" << n_rcvd_or_zero << "] user in-dgrams; as advertised will emit success + "
                             "the messages but cache the error to immediately emit to the next async-receive.");
              sync_err_code->clear();

              /* Should we rcv_not_idle() here?  On one hand messages did arrive; on the other the pipe is hosed
               * anyway.  Going with "don't rock the boat"; if the timer does have a chance to fire, it'll just
               * see m_rcv_pending_err_code and no-op.  We won't add entropy by canceling/restarting it.  Also it's
               * similar to the mainstream error-and-0-messages case (we don't rcv_not_idle() there). */
            }
          }
          else // if (!m_rcv_pending_err_code)
          {
            assert(n_rcvd_or_zero != 0);

            // m_rcv_pending_err_code is falsy; n_rcvd_or_zero >= 1; cool.
            sync_err_code->clear();
            rcv_not_idle(); // Register activity <= got at least 1 in-message of any kind, no error.
          }
        } // if (m_rcv_pending_err_code != would_block)
      } // else // if (!assume_would_block)
    } // if (m_peer_socket)
    else // if (!m_peer_socket)
    {
      FLOW_LOG_WARNING("Socket stream [" << *this << "]: "
                       "User async-receive-batch request for in-batch [" << *batch << "]: "
                       "was about to batch-read but discovered opposite-direction socket-hosing error; "
                       "emitting error.");
      *sync_err_code = m_rcv_pending_err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE;
    }
  } // Lock_guard peer_socket_lock{m_peer_socket_mutex}
} // Native_socket_stream_impl::rcv_read_batch_from_pkt_stream()

template<typename Batch>
void Native_socket_stream_impl::rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error(Batch* batch)
{
  assert(Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT
         && Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT
         && "@todo Really this function should be made to not compile in this case, and this then should be "
              "static_assert() rather than run-time.");

  if (m_rcv_pending_err_code)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: User's wait-for-readable finished (readable or error, "
                     "we do not know which yet); would resume processing depending on what we were doing before; "
                     "however an error was detected in the meantime (as of this writing: idle timeout).  "
                     "Stopping read chain (batch); in-batch [" << *batch << "]");
    assert((!m_rcv_user_batch_request)
           && "If rcv-error emitted during low-level async-wait, we should have fed it to any pending async-receive.");
    return;
  }
  // else

  assert(m_rcv_user_batch_request);

  // Will potentially emit this (if and only if message-read completes due to this successful async-wait).
  Error_code sync_err_code;

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: User-performed wait-for-readable finished (readable or error, "
                 "we do not know which yet).  Retrying to resume read chain (batch); in-batch [" << *batch << "]");

  rcv_read_batch_from_pkt_stream<Batch>(batch, false, &sync_err_code);

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
    rcv_log_stats("rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error(): "
                  "while processing ev-ready rcv-pipe hosed");
  }

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Async-op (batch) result for in-batch [" << *batch << "] "
                 "ready after successful async-wait.  Executing handler now.");

  // Prevent stepping on our own toes: move/clear it first / invoke handler second.
  const auto on_done_func = std::move(m_rcv_user_batch_request->m_on_done_func);
  m_rcv_user_batch_request.reset();

  on_done_func(sync_err_code);
  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Handler completed for in-batch [" << *batch << "].");
} // Native_socket_stream_impl::rcv_on_ev_peer_socket_pkt_stream_batch_readable_or_error()

} // namespace ipc::transport::sync_io
