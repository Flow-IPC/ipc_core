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

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE // Needed at least to get access to `struct ::ucred` (Linux).
#endif

#include "ipc/util/util_fwd.hpp"
#include "ipc/util/process_credentials_fwd.hpp"

namespace ipc::util
{

// Types.

/**
 * A process's credentials (PID, UID, GID as of this writing).
 *
 * @see Opt_peer_process_credentials, a sub-class.
 */
class Process_credentials
{
public:
  // Constructors/destructor.

  /// Default ctor: each value is initialized to zero or equivalent.
  Process_credentials();

  /**
   * Ctor that sets the values explicitly.
   * @param process_id_init
   *        See process_id().
   * @param user_id_init
   *        See user_id().
   * @param group_id_init
   *        See group_id().
   */
  explicit Process_credentials(process_id_t process_id_init, user_id_t user_id_init, group_id_t group_id_init);

  /**
   * Boring copy ctor.
   * @param src
   *        Source object.
   */
  Process_credentials(const Process_credentials& src);

  // Methods.

  /**
   * Boring copy assignment.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Process_credentials& operator=(const Process_credentials& src);

  /**
   * The process ID (PID).
   * @return See above.
   */
  process_id_t process_id() const;

  /**
   * The user ID (UID).
   * @return See above.
   */
  user_id_t user_id() const;

  /**
   * The group user ID (GID).
   * @return See above.
   */
  group_id_t group_id() const;

  /**
   * Obtains, from the OS, information as to the binary name via which process process_id() was started, per its
   * command line, namely `argv[0]`.  In simplified terms this is the executable of `*this` process; however
   * there are important properties to consider about this information:
   *   - The process must be currently executing.  If it is not, a no-such-file error is emitted.
   *   - The executable name is as-it-appeared on the command line; e.g., it might be a relative and/or
   *     un-normalized and/or a symlink.  It is only the normalized, absolute path if that is how it was invoked.
   *   - It is possible (not common but not non-existent either) for a process to change its own command line
   *     upon execution; in this case there's almost no limit to what string might result.
   *     Note!  Even an empty returned string is possible; and this is not emitted as an error.
   *
   * Because of these facts we feel it is, informally, best to use this information for one of these use cases:
   *   - logging;
   *   - verification against an expected value, especially when safety-checking against an expected policy
   *     (e.g., against session::App::m_exec_path) -- namely that, say, the given process was (1) an instance
   *     of a specific application and (2) was invoked via its normalized, absolute path.
   *
   * For these cases it should be reasonable to use.
   *
   * ### Implementation/rationale for it ###
   * (Normally we omit impl details in public doc headers, but in this case the utility is low-level, and it may
   * benefit you to know it.)
   *
   * This builds in Linux only; it reads /proc/(pid)/cmdline (if it exists) and obtains the first NUL-terminated
   * entry there.  This is `argv[0]`.  `argv` space may be overwritten by the process, so it could be something
   * other than the original invocation.  Ignoring that, it'll be whatever the command line included; could be
   * absolute or not, normalized or not, the symlink or not... and so on.  The tool `ps` uses the same technique.
   *
   * One technique was considered: read Linux's /proc/(pid)/exe symlink which points to the absolute, normalized
   * executable path actually invoked.  That would have been perfect; however it appears to require
   * admin privileges on our part, and we do not want to require this.  On balance, since the original use case
   * prompting this feature (session::App::m_exec_path verification) is acceptably served by the less-demanding
   * /proc/.../cmdline method, we chose that.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        some system error code; but most likely no-such-file error of some kind, indicating the process
   *        is not executing (any longer?).
   * @return The first entry -- the executable -- according to the process's command line (see above),
   *         if no #Error_code is emitted.  If one *is* emitted, without an exception (`err_code` not null),
   *         empty string.  Caution: If error is *not* emitted, returned value *might* still be empty,
   *         if the process overrode its own command line (see above).
   */
  std::string process_invoked_as(Error_code* err_code = 0) const;

  /**
   * Obtains the calling process's process_id().  This value will never change.
   * @return See above.
   */
  static process_id_t own_process_id();

  /**
   * Obtains the calling process's effective user_id().  This value can be changed via OS calls.
   * @return See above.
   */
  static user_id_t own_user_id();

  /**
   * Obtains the calling process's effective group_id().  This value can be changed via OS calls.
   * @return See above.
   */
  static group_id_t own_group_id();

  /**
   * Constructs and returns Process_credentials containing values pertaining to the calling process at this
   * time.  Note that certain values therein may become incorrect over time (see own_user_id(), own_group_id()).
   *
   * @return `Process_credentials(own_process_id(), own_user_id(), own_group_id())`.
   */
  static Process_credentials own_process_credentials();

private:
  // Data.

  /// The raw data.  By using Linux `ucred` directly, we can reuse this as base for Opt_peer_process_credentials.
  ::ucred m_val;
}; // class Process_credentials

// Free functions: in *_fwd.hpp.

} // namespace ipc::util
