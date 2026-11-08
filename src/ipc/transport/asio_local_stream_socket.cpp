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
#include "ipc/transport/asio_local_stream_socket.hpp"
#include "ipc/transport/error.hpp"
#include <unistd.h>

namespace ipc::transport::asio_local_stream_socket
{

// Opt_peer_process_credentials implementations.

Opt_peer_process_credentials::Opt_peer_process_credentials() = default;
Opt_peer_process_credentials::Opt_peer_process_credentials(const Opt_peer_process_credentials&) = default;
Opt_peer_process_credentials& Opt_peer_process_credentials::operator=(const Opt_peer_process_credentials&) = default;

} // namespace ipc::transport::asio_local_stream_socket
