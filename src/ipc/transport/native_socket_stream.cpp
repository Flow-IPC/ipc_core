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

// Include Native_socket_stream::Impl class body to complete that type and enable pImpl forwarding.
#include "ipc/transport/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::transport
{

// Initializers.

const Shared_name& Native_socket_stream::S_RESOURCE_TYPE_ID = Sync_io_obj::S_RESOURCE_TYPE_ID;

// Implementations (strict pImpl-idiom style).

// The performant move semantics we get delightfully free with pImpl; they'll just move-to/from the unique_ptr m_impl.

Native_socket_stream::Native_socket_stream(Native_socket_stream&&) = default;
Native_socket_stream& Native_socket_stream::operator=(Native_socket_stream&&) = default;

// Oh and this helper is needed; just see its doc header for rationale.

Native_socket_stream::Impl_ptr& Native_socket_stream::impl() const
{
  using boost::movelib::make_unique;

  if (!m_impl)
  {
    m_impl = make_unique<Impl>(nullptr, "");
  }

  return m_impl;
}

// Provide specific default ctor for documentation elegance:

Native_socket_stream::Native_socket_stream() :
  Native_socket_stream(nullptr, "")
{
  // Yay.
}

// The rest is strict forwarding to impl() (essentially m_impl).

Native_socket_stream::Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  m_impl(boost::movelib::make_unique<Impl>(logger_ptr, nickname_str))
{
  // Yay.
}

Native_socket_stream::Native_socket_stream(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                           Native_handle&& native_peer_socket_moved) :
  m_impl(boost::movelib::make_unique<Impl>(logger_ptr, nickname_str, std::move(native_peer_socket_moved)))
{
  // Yay.
}

Native_socket_stream::Native_socket_stream(Sync_io_obj&& sync_io_core_in_peer_state_moved) :
  m_impl(boost::movelib::make_unique<Impl>(std::move(sync_io_core_in_peer_state_moved)))
{
  // Yay.
}

Native_socket_stream::~Native_socket_stream() = default; // It's only explicitly defined to formally document it.

Native_socket_stream::Sync_io_obj Native_socket_stream::release()
{
  return impl()->release();
}

const std::string& Native_socket_stream::nickname() const
{
  return impl()->nickname();
}

util::Process_credentials Native_socket_stream::remote_peer_process_credentials(Error_code* err_code) const
{
  return impl()->remote_peer_process_credentials(err_code);
}

size_t Native_socket_stream::send_meta_blob_max_size() const
{
  return impl()->send_meta_blob_max_size();
}

size_t Native_socket_stream::send_blob_max_size() const
{
  return impl()->send_blob_max_size();
}

bool Native_socket_stream::send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                                              Error_code* err_code)
{
  return impl()->send_native_handle(hndl_or_null, meta_blob, err_code);
}

bool Native_socket_stream::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  return impl()->send_blob(blob, err_code);
}

bool Native_socket_stream::end_sending()
{
  return impl()->end_sending();
}

bool Native_socket_stream::auto_ping(util::Fine_duration period)
{
  return impl()->auto_ping(period);
}

size_t Native_socket_stream::receive_meta_blob_max_size() const
{
  return impl()->receive_meta_blob_max_size();
}

size_t Native_socket_stream::receive_blob_max_size() const
{
  return impl()->receive_blob_max_size();
}

bool Native_socket_stream::idle_timer_run(util::Fine_duration timeout)
{
  return impl()->idle_timer_run(timeout);
}

// `friend`ship needed for this "non-method method":

std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val)
{
  return os << *(val.impl());
}

// Though, some of them (the templates) had to be written as _fwd() non-templated helpers that take various Function<>s.

bool Native_socket_stream::async_connect_fwd(const Shared_name& absolute_name,
                                             flow::async::Task_asio_err&& on_done_func)
{
  return impl()->async_connect(absolute_name, std::move(on_done_func));
}

bool Native_socket_stream::async_end_sending_fwd(flow::async::Task_asio_err&& on_done_func)
{
  return impl()->async_end_sending(std::move(on_done_func));
}

bool Native_socket_stream::async_receive_native_handle_fwd(Native_handle* target_hndl,
                                                           const util::Blob_mutable& target_meta_blob,
                                                           flow::async::Task_asio_err_sz&& on_done_func)
{
  return impl()->async_receive_native_handle(target_hndl, target_meta_blob, std::move(on_done_func));
}

bool Native_socket_stream::async_receive_blob_fwd(const util::Blob_mutable& target_blob,
                                                  flow::async::Task_asio_err_sz&& on_done_func)
{
  return impl()->async_receive_blob(target_blob, std::move(on_done_func));
}

} // namespace ipc::transport
