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
#include "ipc/transport/detail/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/error/error.hpp>

namespace ipc::transport::asio_local_stream_socket
{

// Implementations.

Endpoint endpoint_at_shared_name(flow::log::Logger* logger_ptr,
                                 const Shared_name& absolute_name, Error_code* err_code)
{
  namespace bind_ns = flow::util::bind_ns;
  using boost::system::system_error;
  using std::string;

  FLOW_ERROR_EXEC_FUNC_AND_THROW_ON_ERROR(Endpoint, endpoint_at_shared_name,
                                          logger_ptr, bind_ns::cref(absolute_name), _1);
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

  Endpoint endpoint;
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
