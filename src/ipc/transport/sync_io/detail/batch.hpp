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
#include "ipc/transport/error.hpp"
#include <flow/log/log.hpp>

namespace ipc::transport::sync_io
{

// Template implementations.

template<bool NO_HNDLS, typename Batch, typename Async_rcv_impl_func>
void async_receive_batch_emulation_on_init_msg(flow::log::Logger* logger_ptr,
                                               Batch* batch, Error_code* sync_err_code,
                                               const Async_rcv_impl_func& async_rcv_impl_func,
                                               const Error_code init_err_code, size_t init_sz)
{
  using flow::async::Task_asio_err_sz;

  /* This is a continuation of async_receive_batch_emulation(), and it should be looked at together with that one,
   * including the big comment therein.  Really we are only a (helper) function, because we might be invoked
   * synchronously from there, or asynchronously after a would-block and async-wait. */

  if (init_err_code)
  {
    *sync_err_code = init_err_code; // Init read led to fatal error.
    return;
  }
  // else: Got 1 message!  So now read 0+ more messages until would-block or other error or all-slots-filled.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  /* Though first don't forget to "bank" that initial message.  The meta-blob and (possibly) Native_handle are
   * already written; so we just tell it the size of the message to record.  It'll also increment batch->n_used().
   * Note that the latter could make batch->full().  Also note that earlier we ensured the requirement that
   * batch->initialized() -- notably, *before* reading anything off the pipe. */
  const auto init_n_used = batch->n_used();
  batch->emulate_result(init_sz);

  if constexpr(NO_HNDLS)
  {
    // By the way *batch output includes key stuff like .n_used(), so things should be clear.
    FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = yes); "
                   "just recorded init-result into the last slot: n_rcvd [" << init_sz << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = no); "
                   "just recorded init-result into the last slot: "
                   "n_rcvd [" << init_sz << "]; hndl [" << batch->result_payload_hndl(batch->n_used() - 1) << "].");
  }

  sync_err_code->clear();

  Error_code err_code;
  while ((!err_code) && (!batch->full())) // First time err_code is falsy; but batch->full() may be true.
  {
    size_t sz;

    // This time, there's no async continuation on would-block (hence {} for the on-done handler).
    if constexpr(NO_HNDLS)
    {
      async_rcv_impl_func(false, batch->next_target_blob(), &err_code, &sz,
                          Task_asio_err_sz{});
    }
    else // if constexpr(!NO_HNDLS)
    {
      async_rcv_impl_func(batch->next_target_hndl(), false, batch->next_target_blob(), &err_code, &sz,
                          Task_asio_err_sz{});
    }

    if (err_code && (err_code != error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE)
                 && (err_code != boost::asio::error::eof)
                 && (err_code != error::Code::S_SYNC_IO_WOULD_BLOCK))
    {
      /* It is an exceptional error which, by the combination of the async_receive_..._batch() concept requirements
       * and our explicit promise to treat `boost::asio::error::eof` as the sole transport-level graceful-close
       * error, means that we are obliged to emit the error immediately -- not ignore it (while async_rcv_impl_func()
       * caches it for next async-receive if any) and simply emit success/the 1+ already-received-OK in-messages.
       * In the concept doc this is called the *instant error* outcome.
       *
       * Mechanically for us that means two things:
       *   - Emit the truthy err_code via *sync_err_code (obviously).
       *   - As required when doing the latter, return batch->n_used() to its original value, thus "eating" the
       *     already received 1+ messages.
       *     - But!  Let us not leak any handles that may have been previously banked.  (This is promised
       *       formally: see async_receive_batch_emulation() doc header's *instant error* discussion; also
       *       Native_handle_receiver::async_receive_native_handle_batch() concept requires it of impls.) */

      if constexpr(!NO_HNDLS)
      {
        for (auto idx = init_n_used; idx != batch->n_used(); ++idx)
        {
          /* If there's a handle in there, un-leak it.
           * (Stored value in *batch is not touched; but it remains in [n_used(), ...) <=> meaningless.
           * @todo For cleanliness/defensiveness it would be good to also nullify it; *batch lacks the required
           * receiver-engine-facing API at the moment; could add it (see to-do on
           * asio_local_stream_socket::Msg_batch_in::result_payload_hndl()).) */
          batch->result_payload_hndl(idx).close();
        }
      }

      batch->clear_used(init_n_used);
      *sync_err_code = err_code;

      FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Due to non-would-block, non-graceful-close "
                     "error [" << err_code << "] [" << err_code.message() << "] emitting overall batch-receive "
                     "error having rewound the Msg_batch to its pre-op state.");
      // Loop ends because err_code.
    }
    else if (!err_code)
    {
      // Again: increments batch->n_used(); may reach batch->full().
      batch->emulate_result(sz);

      if constexpr(NO_HNDLS)
      {
        FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = yes); "
                       "just recorded tacked-on-result into the last slot: n_rcvd [" << sz << "].");
      }
      else
      {
        FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = no); "
                       "just recorded tacked-on-result into the last slot: "
                       "n_rcvd [" << sz << "]; hndl [" << batch->result_payload_hndl(batch->n_used() - 1) << "].");
      }
      // Loop continues unless batch->full().
    }
    /* else // if (err_code is one of {graceful-close, native-graceful-close, would-block}):
     *
     * Do nothing: leave *sync_err_code at success; emit the 1+ in-messages from before.  Loop ends because err_code.
     * Now let's consider the possibilities and show this is proper behavior:
     *   - err_code is would-block: Not a "real" error, and since we have 1+ in-messages already, we certainly
     *     should not be emitting would-block.  The 1+ in-messages have been emitted into *batch.  All good.
     *   - err_code is one of the graceful-closes delineated by
     *     Native_handle_receiver::async_receive_native_handle_batch() and our contract as necessitating
     *     the *delayed error* outcome.  In this outcome: async_rcv_impl_func() is to cache err_code, so that
     *     any further async-receive yields that error immediately (this is a requirement for our use; not up to us);
     *     while we are to emit success this time, emitting any 1+ in-messages that had been obtained to that point.
     *     Indeed: *sync_err_code is falsy, and the 1+ in-messages have been emitted into *batch.  All good. */
  } // while (!err_code && !batch->full())
  // Reminder: err_code, even if truthy (very possible: would-block), is eaten.

  /* That's it!  To recap:
   *   - 1 (init) message is banked in *batch (batch->n_used() incremented 1x).
   *   - 0+ further messages are banked (batch->n_used() incremented 1x per msg).
   *     - If, while trying to get these, a (non-would-block) error was detected, it will be emitted next
   *       time user tries async-receive (but, per next bullet, not this time).  It is the object's responsibility
   *       to save such deferred errors.
   *   - Emitting success *sync_err_code.
   *   - batch->full() may have been reached.  If it has not, it is "implied would-block".
   *     If it has been reached, it is indeterminate; could be would-block, could be not.
   *     User can do what they wish (spoiler alert: on implied would-block, they shouldn't try a read right now;
   *     in the other case they should). */
} // async_receive_batch_emulation_on_init_msg()

} // namespace ipc::transport::sync_io
