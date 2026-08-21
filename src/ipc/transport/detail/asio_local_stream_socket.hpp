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

#include "ipc/transport/detail/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/log/log.hpp>
#include <flow/error/error.hpp>
#include <boost/array.hpp>
#include <string>
#include <sys/socket.h>

namespace ipc::transport::asio_local_stream_socket
{

// Types.

/**
 * Ancillary data (for `recv[m]msg()` and `send[m]msg()`) buffer, used in our case for storing transmitted
 * native handle(s), properly sized to store the max supported # of handles and wrapped in a union in
 * order to ensure it is suitably aligned.
 *
 * ...If I (ygoldfel) understand correctly, and I think I do, the idea is that:
 *   - `msg_control` points to a contiguous data area (ancillary data); which is split up into variable-length
 *     sub-areas, each of which starts with a header, in the form of a `cmsghdr`, which contains a couple of
 *     `enum`-like `int`s, a `cmsg_len` specifying the length of the rest of that sub-area (within the sequence),
 *     and then that area which contains the stuff to be transmitted (whose exact size/meaning depends on the
 *     `enum`s; in our case it'll be native handles a/k/a FDs).
 *   - A key caveat is that in order to be properly interpreted, the header -- probably cast by the kernel/something
 *     to `cmsghdr` -- must begin on an aligned address.  So, in particular, if whatever comes before it
 *     (could be the preceding ancillary data sub-area, depending) ends just before an odd address, then
 *     the data sub-area must be a byte or a few to the right.
 *   - So this trick explicitly puts a `cmsghdr` there; so that its co-inhabitant #m_buf (the actual area where we'll
 *     shove handles) will also begin on the same address/byte.
 *   - `CMSG_ALIGN()` is a Linux extension that might be usable to avoid having to use the trick, but seeing as
 *     how `man cmsg` in my Linux says it's not portable and itself foregoes it in favor of the trick in the
 *     example, I'll just also use the trick.
 */
union Msg_control_as_union
{
  /// The ancillary-data buffer.
  boost::array<uint8_t, CMSG_SPACE(sizeof(Native_handle::handle_t) * N_PAYLOAD_FDS)> m_buf;
  /// The alignment-ensuring thing.
  ::cmsghdr m_align;
};

// Template implementations.

template<typename Protocol>
Endpoint<Protocol> endpoint_at_shared_name(flow::log::Logger* logger_ptr,
                                           const Shared_name& absolute_name, Error_code* err_code)
{
  using boost::system::system_error;
  using std::string;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Endpoint<Protocol>, endpoint_at_shared_name<Protocol>,
                                     logger_ptr, absolute_name, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  /* Make the name "string" as required to construct the endpoint.  This is more subtle than one might expect.
   * POSIX-y Unix domain sockets are either anonymous (not OK for us), or the endpoints live as files in the
   * file system.  Linux adds the delightful extension, "abstract namespace," wherein instead the endpoints are
   * named in a cosmically present namespace unconnected to the file system.  This avoids the file system issues,
   * such as permissions and directories and ..., so we want to use that.  How to specify it, though?
   * Well, more or less, at the native level, the sockaddr_un member sun_path (a `char` array) is used either way;
   * to specify an abstract name one sets the first byte to NUL (which wouldn't be a valid value otherwise); then
   * the rest of sun_path[] (up to the specified size of the sockaddr_un) is taken as the abstract-namespace name.
   * Fine... but we use boost.asio; how to get its `endpoint` to behave that way?  Bad news is it's not explicitly
   * documented all the way.  Good news is it's quite doable.  Firstly, endpoint::data() lets one manually set
   * the whole sockaddr_un structure ourselves -- that part being no prettier than coding this stuff sans boost.asio
   * but overall still nicer than all-native code.  However, turns out we need not do that either (though this is
   * the part that isn't explicitly documented but clearly intended if one checks the Boost header code).
   * Namely: `endpoint` ctor, and the parallel counterpart endpoint::path() mutator overloads, have 2 forms:
   * one taking `const char*`; and one taking `string[_view]`.  `const char*` is clearly insufficient (proof left
   * as exercise to comment reader).  Fascinatingly, string[_view] *is* sufficient: A string[_view] can perfectly
   * safely store multiple NUL characters; and indeed one can see in the Boost header code that it *specifically*
   * supports the weird NUL-led abstract-namespace use case (again, I leave details for you to verify). */

  string abstract_namespace_name(size_t(1), '\0'); // Start with NUL!
  abstract_namespace_name += absolute_name.str(); // After the NUL append the real deal!
  FLOW_LOG_TRACE("Abstract-namespace (Linux extension) name above consists of 1 NUL + the name "
                 "[" << absolute_name << "]; total of [" << abstract_namespace_name.size() << "] bytes.");

  Endpoint<Protocol> endpoint;
  auto& sys_err_code = *err_code;
  try
  {
    /* Throws on error.  (It's annoying there's no error-code-returning API; a bit odd actually.)
     * In practice it'll throw only on too-long name (as of this writing; Boost 1.76), but we don't rely on this. */
    endpoint.path(abstract_namespace_name);
    sys_err_code.clear(); // If it didn't throw.
  }
  catch (const system_error& exc)
  {
    FLOW_LOG_WARNING("Unable to set up native local stream endpoint structure; "
                     "could be due to name length; details logged below.");
    sys_err_code = exc.code();
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
  }

  return endpoint; // sys_err_code is set; `endpoint` might be empty.
} // endpoint_at_shared_name()

} // namespace ipc::transport::asio_local_stream_socket
