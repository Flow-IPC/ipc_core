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
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <boost/interprocess/exceptions.hpp>

namespace ipc::util
{

// Template implementations.

template<typename Func>
void op_with_possible_bipc_exception(flow::log::Logger* logger_ptr, Error_code* err_code,
                                     const Error_code& misc_bipc_lib_error,
                                     String_view context,
                                     const Func& func)
{
  using flow::error::Runtime_error;
  using bipc::interprocess_exception;
  using boost::system::system_category;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { op_with_possible_bipc_exception(logger_ptr, actual_err_code, misc_bipc_lib_error, context, func); },
         err_code, context))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

  try
  {
    func();
  }
  catch (const interprocess_exception& exc)
  {
    /* This code is based off Bipc_mq_handle::op_with_possible_bipc_mq_exception().
     * It is somewhat simpler, due to lacking the special case to do with MQs.
     * @todo Unify the 2 somehow, for code reuse.
     *
     * Keeping comments light because of that.  See that method. */

    const auto native_code_raw = exc.get_native_error();
    const auto bipc_err_code_enum = exc.get_error_code();
    FLOW_LOG_WARNING("bipc threw interprocess_exception; will emit some hopefully suitable Flow-IPC Error_code; "
                     "but here are all the details of the original exception: native code int "
                     "[" << native_code_raw << "]; bipc error_code_t enum->int "
                     "[" << int(bipc_err_code_enum) << "]; message = [" << exc.what() << "]; "
                     "context = [" << context << "].");
    // else
    if (native_code_raw != 0)
    {
      const auto& sys_err_code = *err_code = Error_code(native_code_raw, system_category());
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      return;
    }
    // else

    *err_code = misc_bipc_lib_error; // The earlier WARNING is good enough.
    return;
  }
  // Got here: all good.
  err_code->clear();
} // op_with_possible_bipc_exception()

template<typename Handle_name_func>
void for_each_persistent_impl(const fs::path& persistent_obj_dev_dir_path, const Handle_name_func& handle_name_func)
{
  using fs::directory_iterator;

  assert(persistent_obj_dev_dir_path.is_absolute() && "Expecting absolute path like /dev/shm (by contract).");

  /* We could use directory_options::skip_permission_denied => no error if lack permissions for `ls D`; but other
   * errors are still possible, and we promised to ignore them.  So simply do ignore them: eat the Error_code
   * and let persistent_obj_dev_dir = <the end iterator>.  E.g., if there is no error but no contents either,
   * it'll equal <the end interator> equivalently. */
  Error_code sink;
  for (const auto& dir_entry : directory_iterator(persistent_obj_dev_dir_path, sink))
  {
    /* In Linux, e.g., /dev/shm/<some pool> is (despite the odd nature of /dev/shm) to be listed as a regular file.
     * We skip anything else including:
     *   - directories (should not exist but who knows?);
     *   - symlinks (ditto);
     *   - anything else like sockets or other odd things (ditto);
     *   - situation where it could not be determined for some error reason.  (Subtlety: directory_entry is, for
     *     optimization, supposed to already store the status before we call is_regular_file(), hence no new
     *     system call occurs therein, but the stored status itself could contain an error.  Anyway we simply skip
     *     any non-vanilla situation, so whatever.)  (Subtlety 2: std::filesystem::directory_entry specifies
     *     its own .is_regular_file(), but boost.fs lacks this; though the latter is described in terms of
     *     of the free function anyway; so meh.) */
#ifndef FLOW_OS_LINUX
#  error "We rely on Linux /dev/... (etc.) semantics, where the object-mapped files are classified as regular files."
#endif
    Error_code err_code;
    const auto dir_entry_status = dir_entry.status(err_code);
    if (err_code || !fs::is_regular_file(dir_entry_status))
    {
      continue;
    }
    // else

    handle_name_func(Shared_name::ct(dir_entry.path().filename().native()));
  } // for (dir_entry)
} // for_each_persistent_impl()

template<typename Handle_name_func>
void for_each_persistent_shm_pool(const Handle_name_func& handle_name_func)
{
#ifndef FLOW_OS_LINUX
#  error "This method relies on/has been tested only with Linux /dev/shm semantics."
#endif
  for_each_persistent_impl("/dev/shm", handle_name_func);
}

} // namespace ipc::util
