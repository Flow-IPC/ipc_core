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

#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::transport::sync_io
{

// Initializers.

const Shared_name Native_socket_stream::S_RESOURCE_TYPE_ID = Shared_name::ct("lclSock");

// Implementations (strict pImpl-idiom style).

// The performant move semantics we get delightfully free with pImpl; they'll just move-to/from the unique_ptr m_impl.

Native_socket_stream::Native_socket_stream(Native_socket_stream&&) = default;
Native_socket_stream& Native_socket_stream::operator=(Native_socket_stream&&) = default;

// Oh and this helper is needed; just see its doc header for rationale.

Native_socket_stream::Impl_ptr& Native_socket_stream::impl() const
{
  return m_impl ? m_impl
                : (m_impl = boost::movelib::make_unique<Impl>(nullptr, ""));
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

Native_socket_stream::~Native_socket_stream() = default; // It's only explicitly defined to formally document it.

const std::string& Native_socket_stream::nickname() const
{
  return impl()->nickname();
}

bool Native_socket_stream::sync_connect(const Shared_name& absolute_name, Error_code* err_code)
{
  return impl()->sync_connect(absolute_name, err_code);
}

#if 0 // See the declaration in class { body }; explains why `if 0` yet still here.
Native_socket_stream Native_socket_stream::release()
{
  auto sock = std::move(*this);
  sock.impl()->reset_sync_io_setup();
  return sock;
}
#endif

const util::Process_credentials& Native_socket_stream::remote_peer_process_credentials(Error_code* err_code) const
{
  return impl()->remote_peer_process_credentials(err_code);
}

bool Native_socket_stream::remote_peer_process_credentials(const util::Process_credentials& creds)
{
  return impl()->remote_peer_process_credentials(creds);
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

stat::Blob_snd_stats Native_socket_stream::blob_send_stats() const
{
  return impl()->blob_send_stats();
}

void Native_socket_stream::blob_send_stats_reset()
{
  impl()->blob_send_stats_reset();
}

stat::Blob_snd_stats Native_socket_stream::native_handle_send_stats() const
{
  return impl()->native_handle_send_stats();
}

void Native_socket_stream::native_handle_send_stats_reset()
{
  impl()->native_handle_send_stats_reset();
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

stat::Blob_rcv_stats Native_socket_stream::blob_receive_stats() const
{
  return impl()->blob_receive_stats();
}

void Native_socket_stream::blob_receive_stats_reset()
{
  impl()->blob_receive_stats_reset();
}

stat::Blob_rcv_stats Native_socket_stream::native_handle_receive_stats() const
{
  return impl()->native_handle_receive_stats();
}

void Native_socket_stream::native_handle_receive_stats_reset()
{
  impl()->native_handle_receive_stats_reset();
}

flow::log::Logger* Native_socket_stream::get_logger() const
{
  return impl()->get_logger();
}

// `friend`ship needed for this "non-method method":

std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val)
{
  return os << *(val.impl());
}

} // namespace ipc::transport::sync_io
