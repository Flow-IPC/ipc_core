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

#include "ipc/transport/native_socket_stream.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/util/util_fwd.hpp>
#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/thread/future.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <utility>
#include <vector>

/* Native_socket_stream is exercised heavily by higher-layer tests and demos; here we target corners not
 * reachable from there. */

namespace ipc::transport::test
{

namespace
{

using Peer_socket = Native_socket_stream_cfg::Protocol::socket;
using boost::promise;
using std::vector;
using std::pair;

// Two Native_socket_streams connected to each other (via a socket pair), in PEER state.
pair<Native_socket_stream, Native_socket_stream> make_connected_pair(flow::log::Logger* logger)
{
  flow::util::Task_engine io;
  Peer_socket sock_a{io};
  Peer_socket sock_b{io};

  boost::asio::local::connect_pair(sock_a, sock_b);

  return { Native_socket_stream{logger, "a", Native_handle{sock_a.release()}},
           Native_socket_stream{logger, "b", Native_handle{sock_b.release()}} };
}

} // namespace (anon)

/* send_native_handle() contract: the user's handle need only stay valid until the call returns -- even if the
 * message had to be queued internally due to would-block -- because the impl duplicates it in that case.
 * Fill the kernel send buffer (idle receiver), send a handle-bearing message so it gets queued, close the
 * original handle at once (and recycle its number), drain: the received handle must still be the real thing. */
TEST(Native_socket_stream_test, send_native_handle_under_would_block)
{
#ifndef FLOW_OS_LINUX
static_assert(false,
              "Lots of native-Linux calls in this test; should work in POSIX but not yet tried; Windows TBD.");
#endif

  // DATA-level logging would dump every (large) filler payload; take it down a notch.
  ipc::test::Test_logger logger{flow::log::Sev::S_TRACE};
  auto peers = make_connected_pair(&logger);
  auto& snd = peers.first;
  auto& rcv = peers.second;
  Error_code err_code;

  // Fill the send buffer: with nobody receiving, sends shall eventually would-block (and queue internally).
  const size_t max_sz = snd.send_meta_blob_max_size();
  const vector<uint8_t> filler(max_sz, 0x42);
  size_t n_fill = 0;
  while (snd.native_handle_send_stats().m_would_block_count == 0)
  {
    ASSERT_TRUE(snd.send_blob(util::Blob_const{filler.data(), filler.size()}, &err_code));
    ASSERT_FALSE(err_code) << err_code.message();
    ++n_fill;
    ASSERT_LT(n_fill, size_t(100000)) << "Send buffer never filled up?  Test premise is off.";
  }

  /* Now the handle-bearing message; it shall be queued behind the would-block.  The handle is a pipe's write end;
   * we keep the read end to later prove the received handle refers to that very pipe. */
  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  ASSERT_NE(::fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK), -1); // So that a wrong received handle => failed read, not hang.
  const uint8_t tag = 0x77;
  ASSERT_TRUE(snd.send_native_handle(Native_handle{pipe_fds[1]}, util::Blob_const{&tag, 1}, &err_code));
  ASSERT_FALSE(err_code) << err_code.message();
  EXPECT_NE(snd.native_handle_send_stats().m_snd_q_depth, 0u); // Indeed queued.

  /* Per contract we may now close our handle at once.  Also recycle its descriptor number with an unrelated FD
   * (the lowest free number gets reused), so that any lingering use of the raw number would be maximally visible:
   * it would ship the decoy. */
  ASSERT_EQ(::close(pipe_fds[1]), 0);
  const int decoy_fd = ::open("/dev/null", O_WRONLY);
  ASSERT_NE(decoy_fd, -1);

  // Drain.  The filler messages arrive first; then ours.
  vector<uint8_t> rcv_buf(rcv.receive_meta_blob_max_size());
  Native_handle rcvd_hndl;
  for (size_t idx = 0; idx != (n_fill + 1); ++idx)
  {
    promise<void> done;
    Error_code done_err_code;
    size_t done_sz;
    rcvd_hndl = Native_handle{};
    ASSERT_TRUE(rcv.async_receive_native_handle(&rcvd_hndl, util::Blob_mutable{rcv_buf.data(), rcv_buf.size()},
                                                [&](const Error_code& async_err_code, size_t sz)
                                                  { done_err_code = async_err_code; done_sz = sz; done.set_value(); }));
    done.get_future().wait();
    ASSERT_FALSE(done_err_code) << "Receive [" << idx << "] failed: " << done_err_code.message();
    if (idx != n_fill)
    {
      EXPECT_EQ(done_sz, max_sz);
      EXPECT_TRUE(rcvd_hndl.null());
    }
    else
    {
      EXPECT_EQ(done_sz, size_t(1));
      EXPECT_EQ(rcv_buf[0], tag);
    }
  }

  // The key check: the received handle is a live duplicate of the pipe's write end -- not garbage, not the decoy.
  ASSERT_FALSE(rcvd_hndl.null());
  EXPECT_TRUE(rcvd_hndl.is_open());
  const uint8_t probe = 0x99;
  EXPECT_EQ(::write(rcvd_hndl.m_native_handle, &probe, 1), 1);
  uint8_t got = 0;
  EXPECT_EQ(::read(pipe_fds[0], &got, 1), 1);
  EXPECT_EQ(got, probe);

  const auto stats = snd.native_handle_send_stats();
  EXPECT_EQ(stats.m_total_msgs, n_fill + 1);
  EXPECT_EQ(stats.m_msgs_with_hndls, size_t(1));
  EXPECT_NE(stats.m_would_block_count, size_t(0));

  rcvd_hndl.close();
  Native_handle{pipe_fds[0]}.close();
  Native_handle{decoy_fd}.close();
}

} // namespace ipc::transport::test
