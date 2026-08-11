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

#include "ipc/transport/transport_fwd.hpp"

namespace ipc::transport
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Base_t>
struct Native_socket_stream_msg_batch_in_privileged;
class Native_socket_stream_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_base_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender_impl;

// Free functions.

/**
 * Prints string representation of the given Native_socket_stream_impl to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_impl& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver_impl` to the given `ostream`.
 *
 * @relatesalso Blob_stream_mq_receiver_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver_impl<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * @relatesalso Blob_stream_mq_sender_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender_impl<Persistent_mq_handle>& val);

} // namespace ipc::transport

namespace ipc::transport::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.

class Native_socket_stream_impl;
template<typename Core_t>
class Async_adapter_receiver;
template<typename Core_t>
class Async_adapter_sender;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender_impl;

// Free functions.

/**
 * Helper for async_receive_batch_emulation() that executes on that call's *initial* single-message async-read
 * (whether the result was immediate or following an async-wait due to initial would-block).  In short, then,
 *   - If `init_err_code`, it saves it to `*sync_err_code` and returns.  Otherwise:
 *   - It performs further single-message sync-reads, stopping once any of the following occurs (but in all cases
 *     setting `*sync_err_code` to success (falsy)):
 *     - `batch->full()` (might be the case at entry already);
 *     - would-block;
 *     - error.  (The error is not emitted, as the pre-condition at this stage is 1+ in-messages have already been
 *       received, and async-receive overall must not emit both messages and error, nor just the latter while eating
 *       the fotmer!  Hence `async_rcv_impl_func()` should cache it, if it is pipe-hosing,
 *       so the next async-receive immediately emits it.)
 *
 * @param logger_ptr
 *        See async_receive_batch_emulation().
 * @param batch
 *        See async_receive_batch_emulation().
 * @param sync_err_code
 *        See async_receive_batch_emulation().
 * @param async_rcv_impl_func
 *        See async_receive_batch_emulation().
 * @param init_err_code
 *        The result of the initial `async_rcv_impl_func()` call in this async_receive_batch_emulation().
 * @param init_sz
 *        If `init_err_code` is success: The # of bytes received by the initial `async_rcv_impl_func()` call in this
 *        async_receive_batch_emulation().
 */
template<bool NO_HNDLS, typename Batch, typename Async_rcv_impl_func>
void async_receive_batch_emulation_on_init_msg(flow::log::Logger* logger_ptr,
                                               Batch* batch, Error_code* sync_err_code,
                                               const Async_rcv_impl_func& async_rcv_impl_func,
                                               const Error_code init_err_code, size_t init_sz);

/**
 * Prints string representation of the given Native_socket_stream_impl to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_impl& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver_impl` to the given `ostream`.
 *
 * @relatesalso Blob_stream_mq_receiver_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver_impl<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * @relatesalso Blob_stream_mq_sender_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender_impl<Persistent_mq_handle>& val);

} // namespace ipc::transport::sync_io
