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

#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/shared_name_fwd.hpp"
#include <flow/util/util.hpp>

namespace ipc::transport::asio_local_stream_socket
{

// Free functions.

/**
 * Returns an #Endpoint corresponding to the given absolute Shared_name, so that an #Acceptor or #Peer_socket could
 * be bound to it.  TRACE logging possible; WARNING on error.
 *
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param absolute_name
 *        Name.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        `boost::asio::error::invalid_argument` (endpoint failed to initialize specifically
 *        because the given or computed address/name ended up too long to fit into natively-mandated data structures;
 *        or because there are invalid characters therein, most likely forward-slash).
 * @return If it did not throw (which cannot happen if `err_code` non-null): default-cted `Endpoint` on error
 *         (if and only if `*err_code` truthy); the actual `Endpoint` otherwise (on success).
 */
Endpoint endpoint_at_shared_name(flow::log::Logger* logger_ptr,
                                 const Shared_name& absolute_name, Error_code* err_code = 0);

/**
 * Helper of async_write_with_native_handle() used as the callback executed when waiting for writability of
 * the given connected local peer socket, with the idea to execute a native `sendmsg()` call and all further
 * parts of the async send op started by async_write_with_native_handle().  This is used as the callback either
 * by async_write_with_native_handle() or by itself upon `sendmsg()` encountering would-block, in which case
 * it must wait again and invoke itself asynchronously (and so on).
 *
 * It is *not* used once at least 1 byte (and therefore also the native handle) have been successfuly sent.
 *
 * @tparam Task_err
 *         See async_write_with_native_handle().
 * @param logger_ptr
 *        See async_write_with_native_handle().
 * @param sys_err_code
 *        The error code yielded by the `Peer_socket::async_wait()` async call for which we are the handler arg.
 *        This can be success or error but not a would-block error.
 * @param peer_socket
 *        See async_write_with_native_handle().
 * @param payload_hndl
 *        See async_write_with_native_handle().
 * @param payload_blob
 *        See async_write_with_native_handle().
 * @param on_sent_or_error
 *        See async_write_with_native_handle().  Note that we, too, will likely blow it away via move semantics.
 */
template<typename Task_err>
void on_wait_writable_or_error(flow::log::Logger* logger_ptr,
                               const Error_code& sys_err_code,
                               Native_handle payload_hndl,
                               const util::Blob_const& payload_blob,
                               Peer_socket* peer_socket,
                               Task_err&& on_sent_or_error);

/**
 * Helper of async_read_with_target_func() containing its core (asynchronously) recursive implementation.
 * Call with template param `TARGET_TBD == true` to start the wait/async-read chain; then it asynchronously
 * triggers the `TARGET_TBD == false` form which then repats that as many times as needed to complete
 * the target buffer.
 *
 * It is also used directly in `TARGET_TBD == false` mode by async_read_interruptible() which is a simpler
 * version of async_read_with_target_func() that knows the target buffer from the start.
 *
 * @tparam TARGET_TBD
 *         Must be `true` when async_read_with_target_func() async-invokes it, `false` when it async-invokes
 *         itself, or if async_read_interruptible() invokes it.
 * @tparam Task_err_blob
 *         See async_read_with_target_func().
 * @tparam Target_payload_blob_func
 *         See async_read_with_target_func().  However, it can be anything (suggest `int`),
 *         if `TARGET_TBD == false` and is ignored in that case.
 * @tparam Should_interrupt_func
 *         See async_read_with_target_func().
 * @param logger_ptr
 *        See async_read_with_target_func().
 * @param sys_err_code
 *        The error code yielded by the `Peer_socket::async_wait()` async call for which we are the handler arg.
 *        This can be success or error but not a would-block error.
 * @param peer_socket
 *        See async_read_with_target_func().
 * @param should_interrupt_func
 *        See async_read_with_target_func().
 * @param on_rcvd_or_error
 *        See async_read_with_target_func().
 * @param target_payload_blob_func
 *        See async_read_with_target_func().  However, it can be anything (suggest `0`),
 *        if `TARGET_TBD == false` and is ignored in that case.
 * @param target_payload_blob
 *        Ignored if `TARGET_TBD == true`; otherwise it must be the result of
 *        `target_payload_blob_func()` during the `TARGET_TBD == false` invocation in this operation.
 * @param n_rcvd_so_far
 *        Must be 0 if `TARGET_TBD == true`; else it must equal the number of leading bytes of `target_payload_blob`
 *        that have already been filled out by reading from socket.  (Note this may be 0 even so, though it's
 *        unlikely.)
 */
template<bool TARGET_TBD,
         typename Task_err_blob, typename Target_payload_blob_func, typename Should_interrupt_func>
void on_wait_readable_or_error(flow::log::Logger* logger_ptr, const Error_code& sys_err_code,
                               Peer_socket* peer_socket,
                               Should_interrupt_func&& should_interrupt_func,
                               Task_err_blob&& on_rcvd_or_error,
                               Target_payload_blob_func&& target_payload_blob_func,
                               util::Blob_mutable target_payload_blob,
                               size_t n_rcvd_so_far);

} // namespace ipc::transport::asio_local_stream_socket
