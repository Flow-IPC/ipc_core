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

#include "ipc/util/util_fwd.hpp"
#include "ipc/util/shared_name_fwd.hpp"
#include <flow/common.hpp>

namespace ipc::util
{

// Types.

/// Short-hand for anonymous pipe write end.
using Pipe_writer = boost::asio::writable_pipe;

/// Short-hand for anonymous pipe read end.
using Pipe_reader = boost::asio::readable_pipe;

// Constants.

/**
 * Maps general Permissions_level specifier to low-level #Permissions value, when the underlying resource
 * is in the file-system and is either accessible (read-write in terms of file system) or inaccessible.
 * Examples of such resources are SHM pools (in Linux living in "/dev/shm") and POSIX MQs ("/dev/mqueue").
 *
 * See any additional user-facing notes in shared_resource_permissions() doc header.
 */
extern const boost::array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>
  SHARED_RESOURCE_PERMISSIONS_LVL_MAP;

/**
 * Maps general Permissions_level specifier to low-level #Permissions value, when the underlying resource
 * is in the file-system (e.g., a file) and is *produced* (and therefore always writable) by the owning (creating,
 * updating) user; but *consumed* (and therefore at most readable, never writable) by potentially other processes,
 * and therefore possibly other users (e.g., group access or unrestricted access -- but only for reading either way).
 * For example a PID file is typically only writable by the daemon's user but may be readable or inaccessible by
 * other users.
 */
extern const boost::array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>
  PRODUCER_CONSUMER_RESOURCE_PERMISSIONS_LVL_MAP;

#ifndef FLOW_OS_LINUX
#  error "IPC_KERNEL_PERSISTENT_RUN_DIR (/var/run) semantics require Unix; tested in Linux specifically only."
#endif
/**
 * Absolute path to the directory (without trailing separator) in the file system where kernel-persistent
 * runtime, but not temporary, information shall be placed.  Kernel-persistent means that it'll disappear
 * at reboot; runtime, but not temporary, means it's... not the designated temporary-data directory (informally:
 * not /tmp).  Informally: just know that it is /var/run, and that it stores things such as process-ID (PID)
 * files.
 *
 * It is a Unix path, absolute (starts with forward-slash), and lexically normal (and lacks a root-name).
 */
extern const fs::path IPC_KERNEL_PERSISTENT_RUN_DIR;

// Free functions.

/**
 * Internal (to ::ipc) utility that invokes the given function that invokes a boost.interprocess operation
 * that is documented to throw `bipc::interprocess_exception` on failure; if indeed it throws the utility
 * emits an error in the Flow error-reporting style.  On error it logs WARNING with all available details.
 *
 * ### Background ###
 * boost.interprocess has a number of operations that are documented to throw the aforementioned exception
 * on error -- without any alternate way of reporting an error such as through an out-arg.  Moreover the
 * exception itself is not any standard runtime-exception type but rather a custom thing.  This is somewhat
 * annoying; and various parts of ::ipc want to emit errors in a consistent way.  That does *not* mean
 * we must only emit some `transport::error::Code::*` or whatever, but we should emit a native-based #Error_code
 * if applicable, either as an out-arg or as a `flow::error::Runtime_error` wrapping it, or indeed one of our
 * own codes otherwise (or if it is more applicable).
 *
 * So this helper standardizes that stuff.
 *
 * @tparam Func
 *         Functor that takes no args and returns nothing.
 * @param logger_ptr
 *        Logger to use for logging subsequently.
 * @param err_code
 *        `err_code` as-if passed to our API.  `*err_code` shall be set if not null depending on success/failure;
 *        if null and no problem, won't throw; if null and problem will throw `Runtime_error` containing a truthy
 *        #Error_code.  That's the usual Flow-style error emission semantics.
 * @param misc_bipc_lib_error
 *        If `func` throws, and we are unable to suss out the system error that caused it, then emit
 *        this value.  E.g.: transport::Code::S_MQ_BIPC_MISC_LIBRARY_ERROR makes sense if creating
 *        a `bipc::message_queue`; or a similar one for SHM-related parts of `bipc`; etc.
 * @param context
 *        Description for logging of the op being attempted, in case there is an error.  Compile-time-known strings
 *        are best, as this is not protected by a filtering macro and hence will be evaluated no matter what.
 * @param func
 *        `func()` shall be executed synchronously.
 */
template<typename Func>
void op_with_possible_bipc_exception(flow::log::Logger* logger_ptr, Error_code* err_code,
                                     const Error_code& misc_bipc_lib_error,
                                     String_view context,
                                     const Func& func);

/**
 * Writes a byte to the given pipe writer.  If any error occurs => undefined behavior (assert may trip).
 * If it would-block... it will block.  So: only use if your algorithm has made sure you will never reach the
 * pipe's capacity.
 *
 * @param logger_ptr
 *        Logger to use for logging subsequently.
 * @param pipe
 *        The pipe.
 */
void pipe_produce(flow::log::Logger* logger_ptr, Pipe_writer* pipe);

/**
 * Reads a byte via the given pipe reader.  If any error occurs => undefined behavior (assert may trip).
 * If it would-block... it will block.  So: only use if your algorithm has made sure you have a byte in there.
 *
 * @param logger_ptr
 *        Logger to use for logging subsequently.
 * @param pipe
 *        The pipe.
 */
void pipe_consume(flow::log::Logger* logger_ptr, Pipe_reader* pipe);

/**
 * Builds an absolute name according to the path convention explained in Shared_name class doc header;
 * this overload applies to resources *outside* the ipc::session paradigm.  Typically one
 * would then add a Shared_name::S_SEPARATOR and then purpose-specific path component(s) (via `/=` or similar).
 *
 * Behavior is undefined if a path fragment argument is empty (assertion may trip).
 *
 * @param resource_type
 *        Resource type constant.  Typically it is available as a `S_RESOURCE_TYPE_ID` constant in some class;
 *        otherwise as a `Shared_name::S_RESOURCE_TYPE_ID_*` constant.
 * @return Absolute path not ending in `S_SEPARATOR`, typically to be appended with a `S_SEPARATOR` and more
 *         component(s) by the caller.
 */
Shared_name build_conventional_non_session_based_shared_name(const Shared_name& resource_type);

/**
 * Implementation of `Persistent_object::for_each_persistent()`; for example see
 * shm::classic::Pool_arena::for_each_persistent().  It relies on the OS feature wherein each particular
 * `Persistent_object` (kernel-persistent object) type is listed in flat fashion in a particular
 * directory; `persistent_obj_dev_dir_path` must specify this directory.
 *
 * Conceptually, we shall perform `ls D/`, where `D` is `persistent_obj_dev_dir_path`.  Then for each
 * resulting `D/N`, we shall call `handle_name_func(N)`.
 *
 * ### Errors (or lack thereof) ###
 * There is no error condition.  If `persistent_obj_dev_dir_path` does not exist, or we lack permissions to
 * look therein, it merely means it cannot contain any relevant items; hence `handle_name_func()` is not invoked.
 *
 * @tparam Handle_name_func
 *         See, e.g., shm::classic::Pool_arena::for_each_persistent().
 * @param persistent_obj_dev_dir_path
 *        This must be an absolute path, or behavior is undefined (assertion may trip).  See above.
 * @param handle_name_func
 *        See `Handle_name_func`.
 */
template<typename Handle_name_func>
void for_each_persistent_impl(const fs::path& persistent_obj_dev_dir_path, const Handle_name_func& handle_name_func);

/**
 * Equivalent to shm::classic::Pool_arena::for_each_persistent().  Provided here (for internal purposes as of this
 * writing) to avoid a little circular-inter-component-dependent problem: Posix_mq_handle::for_each_persistent()
 * needs us; but accessing `Pool_arena` would mean `ipc_core` Flow-IPC sub-component needing `ipc_shm`
 * sub-component as a dependency... but the reverse is already naturally the case.  See ::ipc namespace doc header
 * for discussion of the component hierarchy and how we prefer to avoid circular dependencies therein.
 *
 * @tparam Handle_name_func
 *         See above.
 * @param handle_name_func
 *        See above.
 */
template<typename Handle_name_func>
void for_each_persistent_shm_pool(const Handle_name_func& handle_name_func);

/**
 * Equivalent to shm::classic::Pool_arena::remove_persistent().  Provided for similar reason to that described
 * in for_each_persistent_shm_pool() doc header nearby.
 *
 * @param logger_ptr
 *        See above.
 * @param name
 *        See above.
 * @param err_code
 *        See above.
 */
void remove_persistent_shm_pool(flow::log::Logger* logger_ptr, const Shared_name& name, Error_code* err_code);

} // namespace ipc::util

namespace ipc::util::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.

class Timer_event_emitter;

// Free functions.

/**
 * Prints string representation of the given `Timer_event_emitter` to the given `ostream`.
 *
 * @relatesalso Timer_event_emitter
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Timer_event_emitter& val);

} // namespace ipc::util::sync_io
