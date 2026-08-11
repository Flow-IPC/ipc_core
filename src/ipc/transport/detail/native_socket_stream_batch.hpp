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
#include "ipc/transport/detail/native_socket_stream_batch.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include <flow/log/log.hpp>
#include <utility>

namespace ipc::transport
{

// Types.

/**
 * This is the `friend` facade of Native_socket_stream_msg_batch_in
 * that exposes `private` APIs hidden from public user by providing public access to them; this is used internally
 * by at least sync_io::Native_socket_stream impl.  The background is briefly explained in the impl section of
 * Native_socket_stream_msg_batch_in doc header.
 *
 * @tparam Server_session_t
 *         The type of object whose specific `private` API to expose:
 *         A concrete instance of class template Native_socket_stream_msg_batch_in.
 */
template<typename Base_t>
struct Native_socket_stream_msg_batch_in_privileged
{
  // Types.

  /// Alias for the wrapped object's type.
  using Base = Base_t;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; `private` API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * Forwards to same-named (private) priviliged-API (private) of #Base.
   * @tparam Args
   *        See above.
   * @param args
   *        See above.
   * @return See above.
   */
  template<typename... Args>
  bool nb_read(Args&&... args);
}; // struct Native_socket_stream_msg_batch_in_privileged

// Template implementations.

template<typename Base_t>
template<typename... Args>
bool Native_socket_stream_msg_batch_in_privileged<Base_t>::nb_read(Args&&... args)
{
  return m_base.nb_read(std::forward<Args>(args)...);
}

} // namespace ipc::transport
