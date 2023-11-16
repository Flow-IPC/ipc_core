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
#include "ipc/util/native_handle.hpp"
#include <boost/functional/hash/hash.hpp>

namespace ipc::util
{

// Static initializers.

// Reminder: We've assured via #error that this is being built in POSIX.  (Windows has a special value too though.)
const Native_handle::handle_t Native_handle::S_NULL_HANDLE = -1;

// Native_handle implementations.

Native_handle::Native_handle(handle_t native_handle) :
  m_native_handle(native_handle)
{
  // Nope.
}

Native_handle::Native_handle(Native_handle&& src)
{
  // It's why we exist: prevent duplication of src.m_native_handle.
  operator=(std::move(src));
}

Native_handle::Native_handle(const Native_handle&) = default;

Native_handle& Native_handle::operator=(Native_handle&& src)
{
  using std::swap;

  if (*this != src)
  {
    m_native_handle = S_NULL_HANDLE;
    swap(m_native_handle, src.m_native_handle); // No-op if `&src == this`.
  }
  return *this;
}

Native_handle& Native_handle::operator=(const Native_handle&) = default;

bool Native_handle::null() const
{
  return m_native_handle == S_NULL_HANDLE;
}

std::ostream& operator<<(std::ostream& os, const Native_handle& val)
{
  os << "native_hndl[";
  if (val.null())
  {
    os << "NONE";
  }
  else
  {
    os << val.m_native_handle;
  }
  return os << ']';
}

bool operator==(Native_handle val1, Native_handle val2)
{
  return val1.m_native_handle == val2.m_native_handle;
}

bool operator!=(Native_handle val1, Native_handle val2)
{
  return !operator==(val1, val2);
}

size_t hash_value(Native_handle val)
{
  using boost::hash;

  return hash<Native_handle::handle_t>()(val.m_native_handle);
}

bool operator<(Native_handle val1, Native_handle val2)
{
  /* In POSIX and Windows this is valid.  So don't worry about #ifdef'ing for OS, #error, etc.
   * I did check that even in Windows they are formally integers still. */
  return val1.m_native_handle < val2.m_native_handle;
}

} // namespace ipc::util
