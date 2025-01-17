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
#include "ipc/util/process_credentials.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>

namespace ipc::util
{

// Process_credentials implementations.

Process_credentials::Process_credentials() :
  Process_credentials(0, 0, 0)
{
  // That's it.
}

Process_credentials::Process_credentials(process_id_t process_id_init, user_id_t user_id_init, group_id_t group_id_init)
{
  /* I don't really want to rely on the ordering of this annoying little Linux/whatever structure,
   * so just assign it manually instead of in the initializer above.
   * @todo In C++20 can use the { .x = ..., .y = ... } syntax. */
  m_val.pid = process_id_init;
  m_val.uid = user_id_init;
  m_val.gid = group_id_init;
}

Process_credentials::Process_credentials(const Process_credentials&) = default;
Process_credentials& Process_credentials::operator=(const Process_credentials&) = default;

process_id_t Process_credentials::process_id() const
{
  return m_val.pid;
}

user_id_t Process_credentials::user_id() const
{
  return m_val.uid;
}

group_id_t Process_credentials::group_id() const
{
  return m_val.gid;
}

std::string Process_credentials::process_invoked_as(Error_code* err_code) const
{
  using boost::lexical_cast;
  using boost::system::system_category;
  using fs::path;
  using fs::ifstream;
  using std::ios_base;
  using std::string;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(string, process_invoked_as, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

#ifndef FLOW_OS_LINUX
  static_assert(false, "process_invoked_as() depends on Linux /proc semantics.");
#endif

  path cmd_line_path("/proc");
  cmd_line_path /= lexical_cast<path>(process_id());
  cmd_line_path /= "cmdline";

  /* This weird "file" (I believe it's really the kernel just pumping out these characters, as we read, as opposed
   * to anything actually being stored on a drive) is argv[0], argv[1], ..., right in a row, each one NUL-terminated.
   * So open in binary mode and just read up to NUL or end-of-file (EOF). */
  ifstream cmd_line_file(cmd_line_path, ios_base::binary);
  if (!cmd_line_file.good())
  {
    *err_code = Error_code(errno, system_category());
    return string();
  }
  // else

  /* From this point on we are quite tolerant; we only emit an error if we see the file in non-.good() state --
   * but only if it's not end-of-file.  Beyond that we make zero assumptions about what might be in here (spaces?
   * EOF without NUL? whatever!); see background in our doc header impl section.  Even an empty return value
   * is not, in itself, an error (see our contract).
   *
   * Detail: We could use .getline(), but I don't want to deal with assumptions of max-length.
   * We could use string<<, but isspace() has locale implications, and I just don't wanna get into all that.
   * Reading one char at a time from a buffered stream should be fine perf-wise in this context. */
  string result;
  bool done = false;
  do
  {
    const int ch_or_none = cmd_line_file.get();
    if (!(done = (!cmd_line_file.good()) || (char(ch_or_none) == '\0')))
    {
      result += ch_or_none;
    }
    // else { We break out of loop. }
  }
  while (!done);

  if ((!cmd_line_file.good()) && (!cmd_line_file.eof()))
  {
    assert((errno != 0) && "There had to be *some* reason file reading failed, and it wasn't EOF.");
    *err_code = Error_code(errno, system_category());
    return string();
  }
  // else

  return result; // Might still be empty (unlikely though)... but as noted -- that's OK by us.
} // Process_credentials::process_invoked_as()

process_id_t Process_credentials::own_process_id() // Static.
{
  return ::getpid();
}

user_id_t Process_credentials::own_user_id() // Static.
{
  return ::geteuid();
}

group_id_t Process_credentials::own_group_id() // Static.
{
  return ::getegid();
}

Process_credentials Process_credentials::own_process_credentials() // Static.
{
  return Process_credentials(own_process_id(), own_user_id(), own_group_id());
}

bool operator==(const Process_credentials& val1, const Process_credentials& val2)
{
  return (val1.process_id() == val2.process_id())
         && (val1.user_id() == val2.user_id()) && (val1.group_id() == val2.group_id());
}

bool operator!=(const Process_credentials& val1, const Process_credentials& val2)
{
  return !operator==(val1, val2);
}

std::ostream& operator<<(std::ostream& os, const Process_credentials& val)
{
  return os << "pid[" << val.process_id() << "], user[" << val.user_id() << ':' << val.group_id() << ']';
}

} // namespace ipc::util
