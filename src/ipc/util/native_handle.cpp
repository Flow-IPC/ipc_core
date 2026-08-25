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

// Native_handle implementations.

void Native_handle::close() noexcept
{
  if (null()) // As promised no-op in this case.
  {
    return;
  }
  // else

#ifndef FLOW_OS_LINUX
  static_assert(false, "Native ::close() tested in Linux only in this context, though anything POSIXy should be OK.");
#endif
  ::close(m_native_handle);

  operator=({}); // As promised nullify it.

  /* Discussion of ::close() versus alternatives:
   *
   * Firstly, at least in Linux (likely POSIX, but we haven't rigorously looked into it), ::close() does what we want:
   * the FD (file descriptOR) no longer refers to the file descriptION; so in particular if it's the last FD to have
   * been referring to it, then the descriptION goes away.  So once all FDs (::dup()ed ones, socket-transmitted
   * ones, original) are thus ::close()d, then the resource can really go away (as opposed to leaking, in addition to
   * FD(s) leaking).  So, at least it's correct/correct enough.
   *
   * Secondly, is there an alternative in our style?  Yes but no.  Yes in that we'd like to use the "portable"
   * boost.asio for this job.  Indeed, probably, making a posix_descriptor S from hndl_or_null.m_native_handle
   * and calling S.close() would likely do the job.  However, posix::descriptor's dtor is protected, so one cannot
   * really instantiate a posix::descriptor; must instantiate a sub-class: perhaps stream::descriptor which
   * adds generic read/write ops like write_some() and read_some().  One might be tempted to try
   * Peer_socket<Protocol_byte_stream> (stream_protocol::socket) or Peer_socket<Protocol_byte_stream>
   * (seq_packet_protocol::socket).  Does it matter that m_native_handle might not refer to a stream-style
   * protocol at all (if using stream_protocol::socket or stream::descriptor) or a SEQPACKET-style one (if using
   * seq_packet_protocol::socket)?  One might guess, "no, it's just close it anyway, whatever."  Turns out, actually,
   * no: some kind of machinery in boost.asio (@todo Might be fun/educational to find out what exact machinery)
   * seems to detect a mismatch at least with {stream|seq_packet}_protocol::socket, leading to a quite-stealthy
   * failure to actually .close() successfully... and a leak (total failure of this function to fulfill its contract).
   * So, in closing, no... not that we could find --
   *
   * whereas POSIX ::close() gets the job done.
   *
   * @todo Maybe revisit for boost.asio education at least. */
} // Native_handle::close()

void Native_handle::static_close(Native_handle& hndl) noexcept // Static.
{
  hndl.close();
}

Native_handle disowned_native_handle(Own_native_handle&& src) noexcept
{
  Native_handle ret{src.get()};
  src.release();
  return ret;
}

size_t hash_value(Native_handle val) noexcept
{
  using boost::hash;

  return hash<Native_handle::handle_t>()(val.m_native_handle);
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

} // namespace ipc::util
