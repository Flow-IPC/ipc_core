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

#include "ipc/util/native_handle.hpp"
#include "ipc/common.hpp"
#include <flow/log/log.hpp>
#include <flow/async/util.hpp>
#include <boost/asio.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/interprocess/creation_tags.hpp>

/**
 * Flow-IPC module containing miscellaneous general-use facilities that ubiquitously used by ~all Flow-IPC modules
 * and/or do not fit into any other Flow-IPC module.
 *
 * Each symbol therein is typically used by at least 1 other Flow-IPC module; but all public symbols (except ones
 * under a detail/ subdirectory) are intended for use by Flow-IPC user as well.  Some particulars to note:
 *
 * ipc::util::Shared_name is a universally used shared-resource name class, conceptually similar to
 * `boost::filesystem::path`.  It is used in ipc::transport APIs -- namely to name endpoints when establishing channels,
 * and more -- but also in other Flow-IPC modules.  For example `Shared_name` is used to name certain SHM entities
 * in ipc::shm and in ipc::session when connecting to conversation partner process(es).
 *
 * ipc::util::Native_handle is a commonly used native-handle (FD in POSIX parlance) wrapper (so thin it doesn't add
 * a single bit on top of the actul FD).  These are ~always passed around by value (copied), as they are very small
 * in practice.
 *
 * There are of course various other things; just check them out as they are referenced, or just look around.
 * Note there are free functions providing various niceties; and more types including classes and scoped `enum`s.
 */
namespace ipc::util
{

// Types.

// Find doc headers near the bodies of these compound types.

template <typename T, typename Allocator = std::allocator<T>>
class Default_init_allocator;

class Use_counted_object;

/* Normally we'd try to forward-declare the following only, but doing so means (1) having to specify `: size_t`
 * in two places, here and the definition; and (2) ::S_END_SENTINEL cannot be used in _fwd.hpp to, say,
 * set a compile-time array<> size.  So just make an exception and define the enum fully here.  It's not
 * "really" an aggregate type (class), so it's all right. */

#ifdef FLOW_OS_WIN
static_assert(false, "Design of Permissions_level assumes a POSIX-y security model with users and groups; "
                       "we have not yet considered whether it can apply to Windows in its current form.");
#endif

/**
 * Simple specifier of desired access permissions, usually but not necessarily translated into
 * a `Permissions` value (though even then different value in different contexts).  May be used to map, say,
 * from a Permissions_level to a #Permissions value in an
 * `array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>`.
 *
 * While, unlike #Permissions, this `enum` intends not be overtly based on a POSIX RWXRWXRWX model, it does
 * still assume the 3 user groupings are "user themselves," "user's group," and "everyone."  The 1st and 3rd
 * are likely universal, but the 2nd may not apply to all OS -- through probably all POSIX/Unix ones --
 * and even for something like Linux there could be different groupings such as ones based on OS ACL.
 * As of this writing it's fine, as this is a POSIX-targeted library at least (in fact, Linux, as of this writing,
 * but that could change to include, say, macOS/similar).
 *
 * @internal
 * ### Maintenance ###
 * Do *not* change the order of these constants unless absolutely necessary.  In general any change here
 * means updating any `array<>`s (etc.) that map implicitly from a `size_t` representing
 * Permissions_level.  By convention they are named `*_PERMISSIONS_LVL_MAP` as of this writing.
 *
 * The `array<>` mapping scheme is for max performance; if one uses an `unordered_map` or similar, then there is no
 * similar issue.
 */
enum class Permissions_level : size_t
{
  /// Forbids all access, even by the creator's user.  Most likely this would be useful for testing or debugging.
  S_NO_ACCESS,

  /// Allows access by resource-owning user (in POSIX/Unix identified by UID) and no one else.
  S_USER_ACCESS,

  /**
   * Allows access by resource-owning user's containing group(s) (in POSIX/Unix identified by GID) and no one else.
   * This implies, as well, at least as much access as `S_USER_ACCESS`.
   */
  S_GROUP_ACCESS,

  /// Allows access by all.  Implies, as well, at least as much access as `S_GROUP_ACCESS` and thus `S_USER_ACCESS`.
  S_UNRESTRICTED,

  /// Sentinel: not a valid value.  May be used to, e.g., size an `array<>` mapping from Permissions_level.
  S_END_SENTINEL
}; // enum class Permissions_level

/// Short-hand for Flow's `String_view`.
using String_view = flow::util::String_view;
/// Short-hand for Flow's `Fine_duration`.
using Fine_duration = flow::Fine_duration;
/// Short-hand for Flow's `Fine_time_pt`.
using Fine_time_pt = flow::Fine_time_pt;

/// Short-hand for polymorphic function (a-la `std::function<>`) that takes no arguments and returns nothing.
using Task = flow::async::Task;

/**
 * Short-hand for an immutable blob somewhere in memory, stored as exactly a `void const *` and a `size_t`.
 *
 * ### How to use ###
 * We provide this alias as a stylistic short-hand, as it better suits various interfaces especially in
 * ipc::transport.  Nevertheless it's not meant to more than that; it's an attempt to abstract it away.
 *
 * That is to say, to work with these (create them, access them, etc.), do use the highly convenient
 * boost.asio buffer APIs which are well documented in boost.asio's docs.
 */
using Blob_const = boost::asio::const_buffer;

/**
 * Short-hand for an mutable blob somewhere in memory, stored as exactly a `void*` and a `size_t`.
 * @see ipc::util::Blob_const; usability notes in that doc header apply similarly here.
 */
using Blob_mutable = boost::asio::mutable_buffer;

/// Syntactic-sugary type for POSIX process ID (integer).
using process_id_t = ::pid_t;

/// Syntactic-sugary type for POSIX user ID (integer).
using user_id_t = ::uid_t;

/// Syntactic-sugary type for POSIX group ID (integer).
using group_id_t = ::gid_t;

/// Tag type indicating an atomic open-if-exists-else-create operation.  @see #OPEN_OR_CREATE.
using Open_or_create = bipc::open_or_create_t;

/// Tag type indicating an ideally-atomic open-if-exists-else-fail operation.  @see #OPEN_ONLY.
using Open_only = bipc::open_only_t;

/// Tag type indicating a create-unless-exists-else-fail operation.  @see #CREATE_ONLY.
using Create_only = bipc::create_only_t;

/// Short-hand for Unix (POSIX) permissions class.
using Permissions = bipc::permissions;

// Constants.

/// Tag value indicating an open-if-exists-else-create operation.
extern const Open_or_create OPEN_OR_CREATE;

/// Tag value indicating an atomic open-if-exists-else-fail operation.
extern const Open_only OPEN_ONLY;

/// Tag value indicating an atomic create-unless-exists-else-fail operation.
extern const Create_only CREATE_ONLY;

/// A (default-cted) string.  May be useful for functions returning `const std::string&`.
extern const std::string EMPTY_STRING;

// Free functions.

/**
 * Maps general Permissions_level specifier to low-level #Permissions value, when the underlying resource
 * is in the file-system and is either accessible (read-write in terms of file system) or inaccessible.
 * Examples of such resources are SHM pools (e.g., shm::classic::Pool_arena), bipc MQs (transport::Bipc_mq_handle),
 * POSIX MQs (transport::Posix_mq_handle).
 *
 * Please do not confuse this setting with the read-only/read-write dichotomy potentially specified each time such
 * a resource is opened for access (as is the case for SHM pools): the present mapping applies to a persistent
 * protection in the file system, not at runtime at the code writer's discretion.  The present permissions check
 * is performed at opening time; the runtime writability check each time a datum is written into the resource.
 *
 * @param permissions_lvl
 *        The value to translate.
 * @return The result.
 */
Permissions shared_resource_permissions(Permissions_level permissions_lvl);

/**
 * Utility that sets the permissions of the given resource (at the supplied file system path) to specified
 * POSIX value.  If the resource cannot be accessed (not found, permissions...) that system Error_code shall be
 * emitted.
 *
 * ### Rationale ###
 * It may seem unnecessary, particularly given that it sometimes (in our internal code, but I mention it
 * publicly for exposition purposes) is placed right after the
 * creation of the resource (file, SHM pool, POSIX MQ, shared mutex, etc.) -- where the same `perms` is supplied
 * to the creation-API, whichever is applicable.  The reason is that those APIs tend to make the corresponding OS
 * call (e.g., `open()`) which is bound by the "process umask" in POSIX/Linux; so for example if it's set to
 * the typical 022 (octal), then it's impossible to make the resource group- or all-writable, regardless of
 * `perms`.  set_resource_permissions() uses a technique that bypasses the umask thing.  Note that it does not make
 * any calls to change the umask to accomplish this.
 *
 * Note 1: Sometimes there is not even the creation-API argument for `perms`; in which case the rationale is even
 * more straightforward.
 *
 * Note 2: Sometimes there *is* that API... and (namely in boost.ipc at least) they actually took care to do this
 * (what we do here) themselves (via `fchmod()` and such)... so we don't need to; in fact I (ygoldfel) treated
 * it as valuable confirmation of the correctness of this maneuver.
 *
 * @param logger_ptr
 *        Logger to use for logging (WARNING, on error only).  Caller can themselves log further info if desired.
 * @param path
 *        Path to resource.  Symlinks are followed, and the target is the resource in question (not the symlink).
 * @param perms
 *        See other overload.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        see other overload; note that in addition file-not-found and the like are possible errors (in fact
 *        arguably the likeliest).
 */
void set_resource_permissions(flow::log::Logger* logger_ptr, const fs::path& path,
                              const Permissions& perms, Error_code* err_code = 0);

/**
 * Identical to the other set_resource_permissions() overload but operates on a pre-opened Native_handle
 * (a/k/a handle, socket, file descriptor) to the resource in question.
 *
 * @param logger_ptr
 *        See other overload.
 * @param handle
 *        See above.  `handle.null() == true` causes undefined behavior (assertion may trip).
 *        Closed/invalid/etc. handle will yield civilized Error_code emission.
 * @param perms
 *        See other overload.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        system error codes if permissions cannot be set (invalid descriptor, un-opened descriptor, etc.).
 */
void set_resource_permissions(flow::log::Logger* logger_ptr, Native_handle handle,
                              const Permissions& perms, Error_code* err_code = 0);

/**
 * Returns `true` if and only if the given process (by PID) is reported as running by the OS.
 * Caution: It may be running, but it may be a zombie; and/or it may be running now but dead shortly after
 * this function returns.  Use defensively.
 *
 * Implementation: It invokes POSIX `kill()` with the fake zero signal; this indicates the process *can*
 * be signaled and therefore exists.
 *
 * @param process_id
 *        The process ID of the process in question.
 * @return See above.
 */
bool process_running(process_id_t process_id);

/**
 * Syntactic-sugary helper that returns pointer to first byte in an immutable buffer, as `const uint8_t*`.
 *
 * @param blob
 *        The buffer.
 * @return See above.
 */
const uint8_t* blob_data(const Blob_const& blob);

/**
 * Syntactic-sugary helper that returns pointer to first byte in a mutable buffer, as `uint8_t*`.
 *
 * @param blob
 *        The buffer.
 * @return See above.
 */
uint8_t* blob_data(const Blob_mutable& blob);

} // namespace ipc::util
