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

#include "ipc/transport/detail/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>

namespace ipc::transport::asio_local_stream_socket
{

// Template implementations.

template<typename Task_err>
void on_wait_writable_or_error(flow::log::Logger* logger_ptr,
                               const Error_code& sys_err_code,
                               Native_handle payload_hndl,
                               const util::Blob_const& payload_blob_ref,
                               Peer_socket* peer_socket_ptr,
                               Task_err&& on_sent_or_error)
{
  using util::blob_data;
  using flow::util::buffers_dump_string;
  using boost::asio::async_write;
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;
  using boost::asio::post;

  assert(peer_socket_ptr);
  auto& peer_socket = *peer_socket_ptr;
  util::Blob_const payload_blob(payload_blob_ref); // So we can modify it.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Connected local peer socket was waiting to write from location @ [" << payload_blob.data() << "] "
                 "plus native handle [" << payload_hndl << "]; "
                 "handler called, either ready or error; will try to send if appropriate.");

  if (sys_err_code)
  {
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    FLOW_LOG_WARNING("Connected local peer socket was waiting to write from "
                     "location @ [" << payload_blob.data() << "] plus native handle [" << payload_hndl << "]; "
                     "but an unrecoverable error occurred; will not retry; posting handler.");

    const auto executor = get_associated_executor(on_sent_or_error);
    post(peer_socket.get_executor(),
         bind_executor(executor,
                       [sys_err_code, on_sent_or_error = std::move(on_sent_or_error)]()
    {
      on_sent_or_error(sys_err_code);
    }));

    // And that's it.
    return;
  }
  // else

  assert (!sys_err_code);
  // No error.  Let us send as much as possible of remaining payload_blob; and the native handle payload_hndl.

  Error_code nb_err_code;
  const size_t n_sent_or_zero
    = nb_write_some_with_native_handle(logger_ptr, peer_socket_ptr, payload_hndl, payload_blob, &nb_err_code);

  if (n_sent_or_zero == 0)
  {
    // Not even 1 byte of the blob was sent; and hence nor was payload_hndl (per contract of that function).

    if (nb_err_code == boost::asio::error::would_block)
    {
      FLOW_LOG_TRACE("Async wait indicates writability, yet write attempt indicated would-block; unusual but not "
                     "an error condition; we will try again.");

      // Just like the initial wait (omitting comments for brevity; see async_wait() in main function above).
      peer_socket.async_wait
        (Peer_socket::wait_write,
         [logger_ptr, payload_hndl, payload_blob, peer_socket_ptr,
          on_sent_or_error = std::move(on_sent_or_error)]
         (const Error_code& sys_err_code) mutable
      {
        on_wait_writable_or_error(logger_ptr, sys_err_code, payload_hndl, payload_blob, peer_socket_ptr,
                                  std::move(on_sent_or_error));
      });
      return;
    }
    // else

    assert(nb_err_code);

    // All other errors are fatal.  Retrying makes no sense.  Report the error and however much we'd sent of blob.
    FLOW_LOG_WARNING("Connected local peer socket tried to write from "
                     "location @ [" << payload_blob.data() << "] plus native handle [" << payload_hndl << "]; "
                     "but an unrecoverable error occurred; will not retry; posting handler with error.");

    const auto executor = get_associated_executor(on_sent_or_error);
    post(peer_socket.get_executor(),
         bind_executor(executor,
                       [get_logger, get_log_component, nb_err_code, on_sent_or_error = std::move(on_sent_or_error)]()
    {
      FLOW_LOG_TRACE("Handler started.");
      on_sent_or_error(nb_err_code);
      FLOW_LOG_TRACE("Handler finished.");
      // And that's it.  Async op finished.
    }));
    return;
  } // if (n_sent_or_zero == 0)
  // else if (n_sent_or_zero > 0)

  assert (n_sent_or_zero > 0);

  const size_t orig_blob_size = payload_blob.size();
  payload_blob += n_sent_or_zero; // Shift the buffer (size goes down, start goes right).
  if (payload_blob.size() == 0)
  {
    FLOW_LOG_TRACE("Blob fully sent; hence posting handler with success code.");

    const auto executor = get_associated_executor(on_sent_or_error);
    post(peer_socket.get_executor(),
         bind_executor(executor,
                       [get_logger, get_log_component, on_sent_or_error = std::move(on_sent_or_error)]()
    {
      FLOW_LOG_TRACE("Handler started.");
      on_sent_or_error(Error_code());
      FLOW_LOG_TRACE("Handler finished.");
      // And that's it.  Async op finished.
    }));
    return;
  }
  // else

  // Log much like when about to perform the original handle-and-blob wait; except handle is now sent off.
  FLOW_LOG_TRACE("Continuing: Via connected local peer socket, will *only* send remaining "
                 "blob of size [" << payload_blob.size() << "] located @ [" << payload_blob.data() << "].");

  // Verbose and slow (100% skipped unless log filter passes).
  FLOW_LOG_DATA("Continuing: Blob contents are "
                "[\n" << buffers_dump_string(payload_blob, "  ") << "].");

  /* Subtlety: Here we could easily retry the async_wait() again, then nb_write_some_with_native_handle(), etc.;
   * as in the would-block case above.  Why do we choose to use boost.asio
   * instead?  Answer: Well, A, we can, since the handle has been delivered.
   * And on_wait_writable_or_error() formally expects that payload_hndl is not null, meaning it lacks a mode to
   * nb_write_some_with_native_handle() only the blob.  Well, why not add that ability?
   * (It wouldn't be hard; sendmsg() -- the core of nb_write_some_with_native_handle() --
   * works just fine without using the ancillary-data
   * feature at all.)  Answer: It's fine either way really.  I (ygoldfel) feel a tiny
   * bit better doing native/non-portable code only when necessary and reducing complexity of the native/non-portable
   * code itself; in other words the extra branching could either go deeper into the native code flow, or a
   * a bit higher up, and the latter appeals to me more.  On the other hand this does increase the number of different
   * types of transmission APIs we use; instead of sendmsg() used in 2 ways, we use sendmsg() in 1 way and boost.asio
   * async_write() in 1 way.  Potato, potahto....
   *
   * Side note: See how easy the following call is, not worrying about having to keep trying if it could only send
   * a partial amount?  async_write_with_native_handle() -- the thing we are implementing -- has those same semantics,
   * which is why we're doing all that stuff in here, so our caller need not. */

  async_write(peer_socket, payload_blob,
              [get_logger, get_log_component, // Then we needn't do FLOW_LOG_SET_CONTEXT() inside before logging.
               payload_blob, orig_blob_size, peer_socket_ptr,
               on_sent_or_error = std::move(on_sent_or_error)]
                (const Error_code& sys_err_code, size_t)
  {
    FLOW_LOG_TRACE("Connected local peer socket tried to write from location @ [" << payload_blob.data() << "] "
                   "plus NO native handle; handler called, either with success or error.");

    if (sys_err_code)
    {
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      FLOW_LOG_WARNING("Connected local peer socket tried to write *only* blob "
                       "located @ [" << payload_blob.data() << "]; "
                       "but an unrecoverable error occurred; will not retry; posting handler with error.");
    }

    /* We must pass N to them, where N is the number of bytes in total, out of the *original* payload_blob, the entire
     * async op -- not just this async_write() -- has been able to send.
     *   - We got here because the one and only non-would-block sendmsg() already was able to send
     *     `orig_blob_size - payload_blob.size()` bytes.  Recall that payload_blob now is the original payload_blob
     *     shifted (`+`ed) by that many bytes.
     *   - Then we tried to do the async_write(), and that sent a further `n_sent` bytes (possibly 0).
     *   - Hence sum those 2 quantities to yield N. */

    const auto executor = get_associated_executor(on_sent_or_error);
    post(peer_socket_ptr->get_executor(),
         bind_executor(executor,
                       [get_logger, get_log_component,
                        sys_err_code, on_sent_or_error = std::move(on_sent_or_error)]()
    {
      FLOW_LOG_TRACE("Handler started.");
      on_sent_or_error(sys_err_code);
      FLOW_LOG_TRACE("Handler finished.");
      // And that's it.  Async op finished.
    }));
  }); // async_write()
} // on_wait_writable_or_error()

template<bool TARGET_TBD,
         typename Task_err_blob, typename Target_payload_blob_func, typename Should_interrupt_func>
void on_wait_readable_or_error(flow::log::Logger* logger_ptr, const Error_code& async_err_code,
                               Peer_socket* peer_socket_ptr,
                               Should_interrupt_func&& should_interrupt_func,
                               Task_err_blob&& on_rcvd_or_error,
                               Target_payload_blob_func&& target_payload_blob_func,
                               util::Blob_mutable target_payload_blob,
                               size_t n_rcvd_so_far)
{
  using util::Blob_mutable;
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;
  using boost::asio::post;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  if constexpr(TARGET_TBD)
  {
    FLOW_LOG_TRACE("Connected local peer socket was waiting to read (1st time -- target buffer undetermined); "
                   "handler called, either ready or error; will try to obtain target blob/receive if appropriate.");
    assert(n_rcvd_so_far == 0);
  }
  else
  {
    FLOW_LOG_TRACE("Connected local peer socket was waiting to read (target buffer known); "
                   "handler called, either ready or error; will try to receive if appropriate.  "
                   "Buffer location: @ [" << target_payload_blob.data() << "], "
                   "size [" << target_payload_blob.size() << "] (already received [" << n_rcvd_so_far << "]).");
  }

  auto& peer_socket = *peer_socket_ptr;

  /* Note: As promised we disregard any executor bound to the callbacks target_payload_blob_func()
   * and should_interrupt_func().  This could be a feature added later, though the perf implications would
   * be questionable:  Unlike on_rcvd_or_error() neither is the completion handler, so we'd probably need
   * to (1) post() a wrapper of either; then (2) within that wrapper at the end post() the subsequent
   * work (the code that currently executes synchronously right after these calls at the moment). */

  /* Always, before placing anything into target buffer or anything else that would lead to completion handler,
   * we must check for the operation being interrupted/canceled by user.  Hence it's too early to even check
   * async_err_code which could lead to completion handler. */
  if (should_interrupt_func())
  {
    FLOW_LOG_TRACE("Interrupted locally.  Not proceeding further; not invoking completion handler.");
    return;
  }
  // else OK, not interrupted during async-gap.  The floor is ours.

  // Little helper to invoke completion handler.
  const auto finish = [&](const Error_code& err_code, util::Blob_mutable blob)
  {
    const auto executor = get_associated_executor(on_rcvd_or_error);
    post(peer_socket.get_executor(),
         bind_executor(executor,
                       [err_code, blob, on_rcvd_or_error = std::move(on_rcvd_or_error)]()
    {
      on_rcvd_or_error(err_code, blob);
    }));
  };

  auto sys_err_code = async_err_code; // Just so we can tactically modify it.
  if (sys_err_code)
  {
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    finish(sys_err_code, Blob_mutable());
    return;
  }
  // else wait results in no error; should be readable.

  if constexpr(TARGET_TBD)
  {
    // First time we need to ask user's callback for the target buffer.
    assert(target_payload_blob.size() == 0);
    target_payload_blob = target_payload_blob_func();

    // Handle corner case wherein they've decided not to read anything after all.
    if (target_payload_blob.size() == 0)
    {
      FLOW_LOG_TRACE("Target blob has been determined: no read should proceed after all; degenerate case.  "
                     "Posting handler.");

      finish(Error_code(), target_payload_blob);
      return;
    }
    // else

    FLOW_LOG_TRACE("Target blob has been determined: location @ [" << target_payload_blob.data() << "], "
                   "size [" << target_payload_blob.size() << "].");

    /* Finally can read non-blockingly -- since we know there are probably bytes to read, and where to read them,
     * and we haven't been canceled.  Though, we'll have to run non_blocking() first to set that mode if needed.
     * (All of this is thread-unsafe in various ways, but we talked about all that in our doc header.) */
    if (!peer_socket.non_blocking()) // This is an ultra-fast flag check (I verified): no sys call, no throwing.
    {
      peer_socket.non_blocking(true, sys_err_code);
      if (sys_err_code)
      {
        FLOW_LOG_WARNING("Wanted to nb-read to location @ [" << target_payload_blob.data() << "], "
                         "size [" << target_payload_blob.size() << "], and 1+ bytes are reportedly available, but "
                         "had to set non-blocking mode, and that op failed.");
        FLOW_ERROR_SYS_ERROR_LOG_WARNING();

        finish(sys_err_code, target_payload_blob);
        return;
      }
      // else
      assert(peer_socket.non_blocking());
      // We will *not* "undo" for reasons explained in our doc header as of this writing.
    }
  } // if constexpr(TARGET_TBD)
  // else if constexpr(!TARGET_TBD) { Don't touch target_payload_blob_func in this instance of the template. }

  assert(target_payload_blob.size() != 0);
  // Okay, can really read now.

  const auto remaining_target_payload_blob = target_payload_blob + n_rcvd_so_far; // We still need the original.
  assert(remaining_target_payload_blob.size() != 0);

  const size_t n_rcvd = peer_socket.read_some(remaining_target_payload_blob, sys_err_code);
  if (sys_err_code && (sys_err_code != boost::asio::error::would_block))
  {
    assert(n_rcvd == 0);
    FLOW_LOG_WARNING("Wanted to nb-read to location @ [" << remaining_target_payload_blob.data() << "], "
                     "size [" << remaining_target_payload_blob.size() << "], and 1+ bytes are reportedly "
                     "available, but the nb-read failed (could be graceful disconnect; details below).");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    finish(sys_err_code, target_payload_blob);
    return;
  }
  // else no error, and we've read [0, N] bytes.

  if (n_rcvd == remaining_target_payload_blob.size()) // Then done and done.
  {
    assert(!sys_err_code);
    FLOW_LOG_TRACE("Successfully nb-read all expected data to location "
                   "@ [" << remaining_target_payload_blob.data() << "], "
                   "size [" << remaining_target_payload_blob.size() << "].  Posting handler.");

    finish(Error_code(), target_payload_blob);
    return;
  }
  // else we've read [0, N - 1] bytes; must async-read the rest, because else would likely get would-block right now.

  FLOW_LOG_TRACE("Successfully nb-read some (not all) expected data to "
                 "location @ [" << remaining_target_payload_blob.data() << "], "
                 "size [" << remaining_target_payload_blob.size() << "].  "
                 "Remaining: [" << (remaining_target_payload_blob.size() - n_rcvd) << "] bytes.  "
                 "Will now async-read this (async-wait being first part of that).");

  n_rcvd_so_far += n_rcvd; // See below.
  peer_socket.async_wait
    (Peer_socket::wait_read, [logger_ptr, peer_socket_ptr, target_payload_blob, n_rcvd_so_far,
                              on_rcvd_or_error = std::move(on_rcvd_or_error),
                              should_interrupt_func = std::move(should_interrupt_func)]
                               (const Error_code& async_err_code) mutable
  {
    on_wait_readable_or_error<false> // false => See just below for meaning.
      (logger_ptr, async_err_code, peer_socket_ptr, std::move(should_interrupt_func), std::move(on_rcvd_or_error),
       0, // false => Ignore this.
       target_payload_blob, // false => Read into this place (past the bytes in next arg).
       n_rcvd_so_far); // Keep making progress -- this was incremented above.
  });
} // on_wait_readable_or_error()

} // namespace ipc::transport::asio_local_stream_socket
