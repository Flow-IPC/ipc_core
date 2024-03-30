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
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <sys/stat.h>

namespace ipc::util
{

// Initializations.

const Open_or_create OPEN_OR_CREATE;
const Open_only OPEN_ONLY;
const Create_only CREATE_ONLY;
const std::string EMPTY_STRING;

// Implementations.

Permissions shared_resource_permissions(Permissions_level permissions_lvl)
{
  const auto raw_lvl = size_t(permissions_lvl);
  assert((raw_lvl < size_t(Permissions_level::S_END_SENTINEL))
         && "Seems the sentinel enum value was specified, or there is an internal maintenance bug.");

  return SHARED_RESOURCE_PERMISSIONS_LVL_MAP[raw_lvl];
}

void set_resource_permissions(flow::log::Logger* logger_ptr, const fs::path& path,
                              const Permissions& perms, Error_code* err_code)
{
  using boost::system::system_category;
  using ::open;
  using ::close;
  // using ::O_PATH; // It's a macro apparently.
  // using ::errno; // It's a macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { set_resource_permissions(logger_ptr, path, perms, actual_err_code); },
         err_code, "util::set_resource_permissions(1)"))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

  /* We can either reuse the handle-based overload, or we can do fs::permissions.  The latter is perhaps more
   * portable and certainly a few lines easier and logically simpler.  Nevertheless I (ygoldfel) chose the former,
   * and that is for consistency (and not just in a cosmetic sense; I'd like both routines to work and fail in
   * similar ways with fewer degrees of freedom on how to differ based on some 3rd-party developer's whims).
   * (@todo It is certainly arguable.)  Thing is, there's an `fs` way of doing fs::permissions(path), but
   * there's no fs::permissions(<FD>).  So we'd have to use a native call for the latter but `fs` for the other
   * former?  Nah.  Let's just do it native all the way but build one on top of the other.  Also this repeats
   * the technique in session::ensure_resource_owner_is_app(). */

  int native_handle = open(path.c_str(), O_RDONLY); // Read-only is sufficient for ::fchmod().
  if (native_handle == -1)
  {
    *err_code = Error_code(errno, system_category());
    FLOW_LOG_WARNING("Tried to set permissions of resource at [" << path << "] but while obtaining info-only handle "
                     "encountered error [" << *err_code << "] [" << err_code->message() << "]; unable to proceed.");
    return;
  }
  // else
  Native_handle handle(native_handle);

  // For nicer messaging add some more logging on error.  A little code duplication, but it's OK.
  set_resource_permissions(logger_ptr, handle, perms, err_code);
  if (*err_code)
  {
    FLOW_LOG_WARNING("Tried to set permissions of resource at [" << path << "], upon successfully opening probe-only "
                     "descriptor/handle, but this resulted in error; see preceding WARNING referencing all "
                     "other details.");
  }

  close(native_handle);
} // set_resource_permissions(1)

void set_resource_permissions(flow::log::Logger* logger_ptr, Native_handle handle,
                              const Permissions& perms, Error_code* err_code)
{
  using boost::system::system_category;
  using ::fchmod;
  // using ::errno; // It's a macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { set_resource_permissions(logger_ptr, handle, perms, actual_err_code); },
         err_code, "util::set_resource_permissions(2)"))
  {
    return;
  }
  // else

  assert((!handle.null()) && "Disallowed per contract.");

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

  // There's no Boost.filesystem or STL way to set mode, that I know of (via FD); so use POSIX-y thing.
  const auto rc = fchmod(handle.m_native_handle, perms.get_permissions());
  if (rc == -1)
  {
    *err_code = Error_code(errno, system_category());
    FLOW_LOG_WARNING("Tried to set permissions of resources via descriptor/handle [" << handle << "] but encountered "
                     "error [" << *err_code << "] [" << err_code->message() << "]; unable to check.");
  }
  else
  {
    err_code->clear();
  }

  // As promised don't log anything else (not even TRACE) and leave that to the caller if desired.
} // set_resource_permissions(2)

bool process_running(process_id_t process_id)
{
  using boost::system::system_category;
  using ::kill;
  // using ::errno; // It's a macro apparently.

  // The below, down to the impl, is described in the doc header.  Note these are POSIX semantics.
#ifndef FLOW_OS_LINUX
  static_assert(false, "Using POSIX ::kill(pid, 0) semantics to check whether process running.  "
                         "Have not coded Windows equivalent yet.  Should work in non-Linux POSIX OS but "
                         "must be checked/tested.");
#endif

  if (kill(process_id, 0) == 0)
  {
    return true;
  }
  // else
  assert((Error_code(errno, system_category()) == boost::system::errc::no_such_process)
         && "The fake signal zero is valid and is not actually sent so no permission problem possible; hence "
              "ESRCH or equivalent must be the only possible error.");
  return false;
} // process_running()

const uint8_t* blob_data(const Blob_const& blob)
{
  return static_cast<const uint8_t*>(blob.data());
}

uint8_t* blob_data(const Blob_mutable& blob)
{
  return static_cast<uint8_t*>(blob.data());
}

} // namespace ipc::util
