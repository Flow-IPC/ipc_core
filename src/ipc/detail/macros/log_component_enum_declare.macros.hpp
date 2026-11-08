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

/// @cond
// -^- Doxygen, please ignore the following.  This is wacky macro magic and not a regular `#pragma once` header.

/* This is modeled off the original, similarly-named, such file in Flow.  See that for docs.
 * The below should be self-explanatory; but if adding/editing it's best to refresh oneself on best practices. */

// Rarely used component corresponding to log call sites outside namespace `ipc::X`, for all X in ::ipc.
FLOW_LOG_CFG_COMPONENT_DEFINE(UNCAT, 0)
// Logging from namespace ipc::session.
FLOW_LOG_CFG_COMPONENT_DEFINE(SESSION, 1)
// Logging from namespace ipc::transport.
FLOW_LOG_CFG_COMPONENT_DEFINE(TRANSPORT, 2)
// Logging from namespace ipc::shm.
FLOW_LOG_CFG_COMPONENT_DEFINE(SHM, 3)
// Logging from namespace ipc::util.
FLOW_LOG_CFG_COMPONENT_DEFINE(UTIL, 4)
// Logging from namespace ipc::*::test.
FLOW_LOG_CFG_COMPONENT_DEFINE(TEST, 5)
// Logging from namespace ipc::*::shm::rpc.
FLOW_LOG_CFG_COMPONENT_DEFINE(RPC, 6)

// -v- Doxygen, please stop ignoring.
/// @endcond
