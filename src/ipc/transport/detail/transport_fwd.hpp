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

template<typename Persistent_mq_handle>
class Blob_stream_mq_base_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver_impl;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender_impl;

// Free functions.

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
