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
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include "ipc/common.hpp"

namespace ipc::util::sync_io
{

// Implementations.

Asio_waitable_native_handle::Asio_waitable_native_handle(const Base::executor_type& ex, Native_handle hndl) :
  Base(ex, hndl.m_native_handle)
{
  /* This might be too late; I do know that ->assign() throws "Bad descriptor" in Linux on .null() (-1) handle.
   * It (assign()), as of Boost-1.81 anyway, immediately does an epoll_ctl(ADD) on it which yields bad-descriptor
   * error. */
  assert(!hndl.null());
}

Asio_waitable_native_handle::Asio_waitable_native_handle(const Base::executor_type& ex) :
  Base(ex)
{
  // Yeah.
}

Asio_waitable_native_handle::Asio_waitable_native_handle(Asio_waitable_native_handle&& src) = default;

Asio_waitable_native_handle& Asio_waitable_native_handle::operator=(Asio_waitable_native_handle&& src) = default;

Asio_waitable_native_handle::~Asio_waitable_native_handle()
{
  /* boost.asio documentation is slightly ambiguous -- well, terse as usual -- as to whether `Base::~Base()`
   * (posix::basic_descriptor dtor), which is `protected` by the way, will act as-if .close() was called,
   * in the way that certainly (e.g.) ipc::tcp::socket::~socket() does.  Of course we could test it; but
   * various signs and common sense indicate that it would in fact ::close() the FD.  Even if not, the following
   * would do no harm though.  To be clear: We do *not* want to close it.  We exist for watchability only. */
  release();

  // Probably any async_wait() would get operation_aborted because of that.  They should be ready for that anyway.

  // Now Base::~Base() will do its usual thing, but on a (perfectly valid) as-if-default-cted *this.
}

Native_handle Asio_waitable_native_handle::native_handle()
{
  return Base::is_open() ? Native_handle(Base::native_handle()) : Native_handle();
}

void Asio_waitable_native_handle::assign(Native_handle hndl)
{
  assert(!hndl.null()); // See comment in 2+-arg ctor above.

  if (is_open())
  {
    release();
  }

  Error_code err_code_ok;
  Base::assign(hndl.m_native_handle, err_code_ok);

  if (err_code_ok)
  {
    assert(false && "This can fail at least with bad-descriptor error in Linux; but if you followed "
                      "contract (passed valid descriptor), then it will not.  Bug?  Blowing up.");
    std::abort();
  }
}

} // namespace ipc::util::sync_io
