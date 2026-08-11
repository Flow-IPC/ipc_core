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

#include "ipc/transport/sync_io/detail/batch.hpp"
#include "ipc/transport/error.hpp"
#include <flow/log/log.hpp>

namespace ipc::transport::sync_io
{

// Template implementations.

template<bool NO_HNDLS, typename Batch, typename Task_err, typename Async_rcv_impl_func>
void async_receive_batch_emulation(flow::log::Logger* logger_ptr,
                                   Batch* batch, bool assume_would_block, Error_code* sync_err_code,
                                   Task_err&& on_done_func,
                                   Async_rcv_impl_func&& async_rcv_impl_func)
{
  assert(sync_err_code);

  /* We are emulating a batch-receive by expressing it in terms of, mainly, the ability to do a single-target "classic"
   * receive op (plus some other little needed hooks).  Conceptually one can think of a batch-receive as similar to
   * a single-target receive.  To recap the latter:
   *
   *  - We try to get a message immediately (synchronously); and
   *    - we might succeed; or else
   *    - we might get would-block, effect a sync_io-pattern async-wait on certain FD(s), then try again
   *      again once informed via sync_io pattern; rinse/repeat until indeed we do succeed in getting that 1 message.
   *      (To be clear-ish for this recap -- the stuff about async-waiting and rinse/repeat, that's all handled
   *      by the single-target-receive black box.  The bottom line is, either it yields the 1 message synchronously
   *      or later via on_done_func().  The async-wait machinery has all been set-up already via start_ops(), etc.)
   *  - In the first case async_rcv_impl_func() (which implements the single-target receive) reports it
   *    synchronously via *sync_sz.
   *  - In the second case it later reports it via on_done_func(..., size_t).
   *  - Either way an error might occur; then we report that via *sync_err_code or on_done_func(Error_code, ...)
   *    respectively.
   *
   * The difference when adding batching is, conceptually, not that big.  It is simply that, upon reading that
   * 1 in-message successfully (either path), we then read as many other messages as we synchronously can, until
   * either would-block happens (possibly not the 1st time, depending on the earlier path to initial read success),
   * or batch->full() (all slots have in-messages).  So one can think of it as reading a message consisting of
   * 1+ sub-messages.  Oh and also there's no "overall msg count" separate analogous to sync_sz, as that is state
   * recorded in *batch.
   *
   * There are various tactical caveats, generally commented-upon as needed closer to the fact; but the following
   * are somewhat more strategic:
   *   - Would-block is not like other error conditions, even though API-wise it is reported similarly via
   *     *sync_err_code.  It needs to be handled specially depending on the context.
   *     - If it occurs during that initial single-target receive (async_rcv_impl_func()) attempt, then we report it
   *       as normal via *sync_err_code but must remember that whatever Task_err_sz we give to async_rcv_impl_func()
   *       will still likely trigger later, continuing the overall operation.
   *     - If it occurs while trying to tack-on more messages having gotten the initial in-message, then it has a very
   *       different meaning.  It means the end of the batch -- so at that point we do *not* report would-block
   *       via *sync_err_code or on_done_func(); rather we report a successful read of 1+ in-messages.  This introduces
   *       tactical challenge: async_rcv_impl_func() yielding would-block, by contract, means it has initiated
   *       async-wait(s) and is trying to read more messages; but in fact we do *not* want that anymore; we want
   *       to be all-done.  Hence async_rcv_impl_func() takes an *optional* done-handler; we *do* pass one in here
   *       but do *not* in the prev bullet.
   *   - Other error conditons, including (but not limited to; e.g. for Unix-domain-sockets there could also be
   *     ECONNRESET/etc.) graceful-close (RECEIVES_FINISHED_CANNOT_RECEIVE), might occur during the
   *     tacking-onto-initial-good-message phase.  We advertise that -- much like Linux's ::recvmmsg() as of this
   *     writing -- in this case we:
   *     - report full success *this time*; but
   *     - any relevant call *next time* will yield the (deferred) truthy Error_code.
   *     Mechanically this is easy; any pipe-hosing error is by contract to be cached by async_rcv_impl_func(), so
   *     bullet point 2 is taken care of for us; and as for bullet point 1, we simply do not forward the error
   *     reported by async_rcv_impl_func() back to the user. */

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  if (!batch->initialized())
  {
    FLOW_LOG_WARNING("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = [" << NO_HNDLS << "]; "
                     "assume-would-block? = [" << assume_would_block << "]): "
                     "By contract emitting invalid-argument error, because the user- or library-supplied "
                     "batch object is not initialized() (not all slots have been prepare_target_payload()ed).");
    *sync_err_code = error::Code::S_INVALID_ARGUMENT;
    return; // Notable before reading anything off the pipe (as advertised).  Pipe is not hosed by this.
  }
  // else

  FLOW_LOG_TRACE("Msg_batch [" << *batch << "]: Emulating async-receive (no-hndls? = [" << NO_HNDLS << "]; "
                 "assume-would-block? = [" << assume_would_block << "]).");

  size_t sync_init_sz;
  if constexpr(NO_HNDLS)
  {
    async_rcv_impl_func(assume_would_block, batch->next_target_blob(), sync_err_code, &sync_init_sz,
                        [logger_ptr, batch, on_done_func = std::move(on_done_func),
                         async_rcv_impl_func] // No choice but to copy it.  @todo Use member func ptrs instead?
                          (const Error_code& init_err_code, size_t init_sz) mutable
    {
      // Got 1 message, or (real) error, asynchronously.
      Error_code err_code;

      async_receive_batch_emulation_on_init_msg<NO_HNDLS>
        (logger_ptr, batch, &err_code, async_rcv_impl_func, init_err_code, init_sz);

      on_done_func(err_code);
    });
  }
  else // if constexpr(!NO_HNDLS)
  {
    async_rcv_impl_func(batch->next_target_hndl(), assume_would_block, batch->next_target_blob(),
                        sync_err_code, &sync_init_sz,
                        [logger_ptr, batch, on_done_func = std::move(on_done_func), async_rcv_impl_func]
                          (const Error_code& init_err_code, size_t init_sz) mutable
    { // Same exact thing.  Code reuse doesn't seem worthwhile here.
      Error_code err_code;
      async_receive_batch_emulation_on_init_msg<NO_HNDLS>
        (logger_ptr, batch, &err_code, async_rcv_impl_func, init_err_code, init_sz);
      on_done_func(err_code);
    });
  }

  if (*sync_err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    // We'll (likely) later continue from the handler we gave just above.  Emit this synchronously and GTFO.
    return;
  }
  // else: Got 1 message, or (real) error, synchronously.

  async_receive_batch_emulation_on_init_msg<NO_HNDLS>
    (logger_ptr, batch, sync_err_code, async_rcv_impl_func, *sync_err_code, sync_init_sz);
} // async_receive_batch_emulation()

} // namespace ipc::transport::sync_io
