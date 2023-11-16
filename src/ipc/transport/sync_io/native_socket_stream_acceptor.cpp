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
#include "ipc/transport/sync_io/native_socket_stream_acceptor.hpp"
#include <flow/error/error.hpp>

namespace ipc::transport::sync_io
{

// Initializers.

const Shared_name Native_socket_stream_acceptor::S_RESOURCE_TYPE_ID = Shared_name::ct("sockStmAcc");

// Implementations.

Native_socket_stream_acceptor::Native_socket_stream_acceptor(flow::log::Logger* logger_ptr,
                                                             const Shared_name& absolute_name_arg,
                                                             Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_ready_reader(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  // The Core (starring Hilary Swank).
  m_async_io(get_logger(), absolute_name_arg, err_code)
{
  using flow::error::Runtime_error;
  using boost::asio::connect_pipe;

  // If !err_code and m_async_io exploded, then it threw.  If not null, and it failed then --
  if (err_code && *err_code)
  {
    return; // Further use of *this => undefined behavior.  @todo Make the semantics nicer than that.
  }
  // else

  Error_code sys_err_code;

  connect_pipe(m_ready_reader, m_ready_writer, sys_err_code);
  if (sys_err_code)
  {
    FLOW_LOG_WARNING("Acceptor [" << *this << "]: Constructing: connect-pipe failed.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    if (err_code)
    {
      *err_code = sys_err_code;
      return;
    }
    // else
    throw Runtime_error(sys_err_code, "sync_io::Native_socket_stream_acceptor::ctor");
  }
  // else: No futher errors.

  m_ev_wait_hndl.assign(Native_handle(m_ready_reader.native_handle()));

  FLOW_LOG_WARNING("Acceptor [" << *this << "]: Constructed.  Async-IO core started above.");
} // Native_socket_stream_acceptor::Native_socket_stream_acceptor()

Native_socket_stream_acceptor::~Native_socket_stream_acceptor() = default; // @todo Maybe TRACE-log something?

const Shared_name& Native_socket_stream_acceptor::absolute_name() const
{
  return m_async_io.absolute_name();
}

std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val)
{
  return os << "SIO/sh_name[" << val.absolute_name() << "]@" << static_cast<const void*>(&val);
}

} // namespace ipc::transport::sync_io
