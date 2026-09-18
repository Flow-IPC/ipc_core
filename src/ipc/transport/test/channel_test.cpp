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

#include "ipc/transport/channel.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <gtest/gtest.h>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/thread/future.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* transport::Channel is the bundler of 1-2 pipes' unstructured-IPC peer objects that higher layers (struc::Channel,
 * ipc::session) routinely handle; those layers' tests exercise it constantly but mainly in its steady state, already
 * initialized and handed over by ipc::session.  Here we cover the Channel API itself: the init_*_pipe()
 * sequence and initialized(), the alias classes' ctors (including their error paths), move semantics,
 * async_io_obj(), the forwarding methods, and the bundle-wide methods that must treat both pipes as one.
 * All of that happens before or instead of anything ipc::session does, so channels are built by hand here:
 * socket pairs for Native_socket_stream and small named MQs for Blob_stream_mq_*er.
 *
 * Most of the code (transport/channel.hpp) is boiler-plate/forwarding and very simple logic; but there is ample
 * opportunity for typos and copy-paste bugs; and/or doing not exactly what docs advertise (e.g. supposed to reject
 * API misuse so-and-so but instead it is accepted).  Tests may be simple, but it's still surprisingly helpful.
 *
 * There is *some* non-trivial logic; Channel::async_end_sending() (2 forms) comes to mind.  As always such things are
 * good to test also. */

namespace ipc::transport::test
{

namespace
{

using Sio_nss = sync_io::Native_socket_stream;
using util::sync_io::Asio_waitable_native_handle;
using util::sync_io::Task_ptr;
using util::Blob_const;
using util::Blob_mutable;
using flow::async::Single_thread_task_loop;
using boost::promise;
using std::string;
using std::vector;
using std::pair;

// Logger for the peer objects, loops, etc.: null (they are chatty).  Flip to console when a failing test needs them.
flow::log::Logger* obj_logger()
{
#if 1
  return nullptr;
#else
  static ipc::test::Test_logger s_logger;
  return &s_logger;
#endif
}

/* Logger for the `Channel`s themselves: a real one, so that the checks that a logger is carried over (by move,
 * by async_io_obj()) are meaningful; but a quiet one, as their INFO chatter is not useful here, while the few
 * expected WARNINGs (dupe-init refusals) are. */
flow::log::Logger* channel_logger()
{
  static ipc::test::Test_logger s_logger{flow::log::Sev::S_WARNING};
  return &s_logger;
}

// A message payload distinct enough that receiving it proves the intended pipe carried it.
const vector<uint8_t> BLOB_PAYLOAD{0x10, 0x20, 0x30, 0x40, 0x50};
const vector<uint8_t> HNDL_PAYLOAD{0x77, 0x66, 0x55};

Blob_const as_blob(const vector<uint8_t>& payload)
{
  return Blob_const{payload.data(), payload.size()};
}

// Two sync_io::Native_socket_streams connected to each other (via a socket pair), in PEER state.
pair<Sio_nss, Sio_nss> make_sio_nss_pair(util::String_view nickname_a, util::String_view nickname_b)
{
  using Peer_socket = Native_socket_stream_cfg::Protocol::socket;

  flow::util::Task_engine io;
  Peer_socket sock_a{io};
  Peer_socket sock_b{io};
  boost::asio::local::connect_pair(sock_a, sock_b);

  return { Sio_nss{obj_logger(), nickname_a, Native_handle{sock_a.release()}},
           Sio_nss{obj_logger(), nickname_b, Native_handle{sock_b.release()}} };
}

/* A uniquely named MQ, created at construction and removed from the OS at scope exit.  (Blob_stream_mq_sender's
 * dtor also removes it, but not every test gets as far as constructing one.)  open() yields a fresh handle to it.
 * The name is absolute (leading separator), as Blob_stream_mq_*er require of the MQs handed to them. */
template<typename Mq>
class Test_mq
{
public:
  explicit Test_mq(util::String_view suffix) :
    m_name(Shared_name::ct(flow::util::ostream_op_string(Shared_name::S_SEPARATOR, "ipcChannelTest_",
                                                         Mq::S_RESOURCE_TYPE_ID.str(), '_',
                                                         util::Process_credentials::own_process_id(), '_', suffix)))
  {
    remove();
    /* Small queue, so that filling it (to force would-block) is quick; but not too small: Posix_mq_handle is
     * subject to modest OS limits (msg_max = 10 by default), and Blob_stream_mq_sender's control commands
     * (protocol version at start-ops, graceful-close) cost 2 low-level messages each, so a test that sends
     * 1 user message and then ends sending, expecting no would-block, needs 5 messages' room. */
    Mq mq{obj_logger(), m_name, util::CREATE_ONLY, 8, 64,
          util::shared_resource_permissions(util::Permissions_level::S_USER_ACCESS)}; // Throws on failure.
  }

  ~Test_mq()
  {
    remove();
  }

  Mq open() const
  {
    return Mq{obj_logger(), m_name, util::OPEN_ONLY}; // Throws on failure.
  }

private:
  void remove()
  {
    Error_code sink;
    Mq::remove_persistent(obj_logger(), m_name, &sink); // Error (typically nonexistent) OK: best-effort.
  }

  const Shared_name m_name;
}; // class Test_mq

/* Both ends of a blobs pipe over 2 MQs (one per direction) and of a handles pipe over a socket pair, as
 * sync_io cores ready to be bundled into a Channel of any shape.  Side A sends over m_a2b and m_sock_a;
 * side B over m_b2a and m_sock_b. */
template<typename Mq>
struct Pipe_ends
{
  Test_mq<Mq> m_a2b{"a2b"};
  Test_mq<Mq> m_b2a{"b2a"};
  pair<Sio_nss, Sio_nss> m_socks = make_sio_nss_pair("sockA", "sockB");

  // Sync_io peer objects for a blob pipe on side A / side B (out-MQ, in-MQ).
  pair<Mq, Mq> mqs_a() { return { m_a2b.open(), m_b2a.open() }; }
  pair<Mq, Mq> mqs_b() { return { m_b2a.open(), m_a2b.open() }; }
};

// The canonical sync_io-pattern hookup: satisfy a sync_io Channel's async-wait requests via the given loop.
template<typename Channel_t>
void start_sio_ops(Channel_t* channel, Single_thread_task_loop* loop)
{
  EXPECT_TRUE(channel->replace_event_wait_handles([loop]()
                                                    { return Asio_waitable_native_handle{*(loop->task_engine())}; }));
  const auto ev_wait_func = [](Asio_waitable_native_handle* hndl_of_interest,
                               bool ev_of_interest_snd_else_rcv, Task_ptr&& on_active_ev_func)
  {
    hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv ? Asio_waitable_native_handle::Base::wait_write
                                                             : Asio_waitable_native_handle::Base::wait_read,
                                 [on_active_ev_func = std::move(on_active_ev_func)](const Error_code& err_code)
    {
      if (err_code != boost::asio::error::operation_aborted)
      {
        (*on_active_ev_func)();
      }
    });
  };

  if constexpr(Channel_t::S_HAS_BLOB_PIPE)
  {
    EXPECT_TRUE(channel->start_send_blob_ops(ev_wait_func));
    EXPECT_TRUE(channel->start_receive_blob_ops(ev_wait_func));
    EXPECT_FALSE(channel->start_send_blob_ops(ev_wait_func)); // Dupes are refused (forwarded verbatim).
    EXPECT_FALSE(channel->start_receive_blob_ops(ev_wait_func));
  }
  if constexpr(Channel_t::S_HAS_NATIVE_HANDLE_PIPE)
  {
    EXPECT_TRUE(channel->start_send_native_handle_ops(ev_wait_func));
    EXPECT_TRUE(channel->start_receive_native_handle_ops(ev_wait_func));
    EXPECT_FALSE(channel->start_send_native_handle_ops(ev_wait_func));
    EXPECT_FALSE(channel->start_receive_native_handle_ops(ev_wait_func));
  }
  // Once any start_*_ops() succeeded, replacing the wait-handles is refused; the Channel ANDs the results.
  EXPECT_FALSE(channel->replace_event_wait_handles([loop]()
                                                     { return Asio_waitable_native_handle{*(loop->task_engine())}; }));
}

// What one async-I/O receive yields.  m_hndl is meaningful for handle-pipe receives only.
struct Rcv_result
{
  Error_code m_err_code;
  vector<uint8_t> m_payload;
  Native_handle m_hndl;
};

// Receives one message over the async-I/O `rcv`s blob pipe (a Channel or a Blob_receiver), synchronously.
template<typename Rcv>
Rcv_result receive_blob(Rcv* rcv)
{
  Rcv_result result;
  result.m_payload.resize(rcv->receive_blob_max_size());
  promise<void> done;
  EXPECT_TRUE(rcv->async_receive_blob(Blob_mutable{result.m_payload.data(), result.m_payload.size()},
                                      [&](const Error_code& err_code, size_t sz)
  {
    result.m_err_code = err_code;
    result.m_payload.resize(sz);
    done.set_value();
  }));
  done.get_future().wait();
  return result;
}

// Receives one message over the async-I/O `rcv`s handles pipe (a Channel or a Native_handle_receiver), synchronously.
template<typename Rcv>
Rcv_result receive_hndl(Rcv* rcv)
{
  Rcv_result result;
  result.m_payload.resize(rcv->receive_meta_blob_max_size());
  promise<void> done;
  EXPECT_TRUE(rcv->async_receive_native_handle(&result.m_hndl,
                                               Blob_mutable{result.m_payload.data(), result.m_payload.size()},
                                               [&](const Error_code& err_code, size_t sz)
  {
    result.m_err_code = err_code;
    result.m_payload.resize(sz);
    done.set_value();
  }));
  done.get_future().wait();
  return result;
}

// Sends one message over each of `snd`s pipes; receives each over `rcv`s (async-I/O) and checks payloads.
template<typename Snd_channel, typename Rcv_channel>
void send_and_receive_over_each_pipe(Snd_channel* snd, Rcv_channel* rcv)
{
  FLOW_TEST_TRACE_CTX("Sender [", *snd, "]; receiver [", *rcv, "].");
  Error_code err_code;
  if constexpr(Snd_channel::S_HAS_BLOB_PIPE)
  {
    ASSERT_TRUE(snd->send_blob(as_blob(BLOB_PAYLOAD), &err_code));
    ASSERT_FALSE(err_code) << err_code.message();
    const auto result = receive_blob(rcv);
    ASSERT_FALSE(result.m_err_code) << result.m_err_code.message();
    EXPECT_EQ(result.m_payload, BLOB_PAYLOAD);
  }
  if constexpr(Snd_channel::S_HAS_NATIVE_HANDLE_PIPE)
  {
    ASSERT_TRUE(snd->send_native_handle(Native_handle{}, as_blob(HNDL_PAYLOAD), &err_code));
    ASSERT_FALSE(err_code) << err_code.message();
    const auto result = receive_hndl(rcv);
    ASSERT_FALSE(result.m_err_code) << result.m_err_code.message();
    EXPECT_EQ(result.m_payload, HNDL_PAYLOAD);
    EXPECT_TRUE(result.m_hndl.null());
  }
}

// The as-if-default-cted state (initial, moved-from, or post-async_io_obj()) as visible through the API.
template<typename Channel_t>
void expect_as_if_default_cted(const Channel_t& channel)
{
  EXPECT_FALSE(channel.initialized(true)); // (Do not log a WARNING about it; the point is it is expected.)
  EXPECT_TRUE(channel.nickname().empty());
  if constexpr(Channel_t::S_HAS_BLOB_PIPE)
  {
    EXPECT_EQ(channel.blob_snd(), nullptr);
    EXPECT_EQ(channel.blob_rcv(), nullptr);
  }
  if constexpr(Channel_t::S_HAS_NATIVE_HANDLE_PIPE)
  {
    EXPECT_EQ(channel.hndl_snd(), nullptr);
    EXPECT_EQ(channel.hndl_rcv(), nullptr);
  }
  EXPECT_EQ(flow::util::ostream_op_string(channel).rfind("[null]@", 0), 0u);
}

// The accessors of an initialized Channel: non-null; const and mutable overloads agree.
template<typename Channel_t>
void expect_accessors_live(Channel_t* channel)
{
  FLOW_TEST_TRACE_CTX("Channel [", *channel, "].");
  const auto& const_channel = *channel;
  if constexpr(Channel_t::S_HAS_BLOB_PIPE)
  {
    ASSERT_TRUE(channel->blob_snd());
    ASSERT_TRUE(channel->blob_rcv());
    EXPECT_EQ(const_channel.blob_snd(), channel->blob_snd());
    EXPECT_EQ(const_channel.blob_rcv(), channel->blob_rcv());
  }
  if constexpr(Channel_t::S_HAS_NATIVE_HANDLE_PIPE)
  {
    ASSERT_TRUE(channel->hndl_snd());
    ASSERT_TRUE(channel->hndl_rcv());
    EXPECT_EQ(const_channel.hndl_snd(), channel->hndl_snd());
    EXPECT_EQ(const_channel.hndl_rcv(), channel->hndl_rcv());
  }
}

} // namespace (anon)

/* The compile-time traits every generic user of Channel meta-programs against, checked for each pipe shape
 * in both sync_io and async-I/O form.  (The static_asserts are the test; the TEST body just makes it visible.) */
TEST(Channel_test, compile_time_traits)
{
  using Blobs_1_obj = Socket_stream_channel_of_blobs<true>;
  using Blobs_2_objs = sync_io::Posix_mqs_channel_of_blobs;
  using Hndls = Socket_stream_channel<true>;
  using Both = sync_io::Bipc_mqs_socket_stream_channel;

  static_assert(Blobs_1_obj::S_HAS_BLOB_PIPE_ONLY && Blobs_1_obj::S_HAS_BLOB_PIPE
                  && !Blobs_1_obj::S_HAS_NATIVE_HANDLE_PIPE && !Blobs_1_obj::S_HAS_NATIVE_HANDLE_PIPE_ONLY
                  && !Blobs_1_obj::S_HAS_2_PIPES,
                "Blobs-only shape traits.");
  static_assert(Blobs_2_objs::S_HAS_BLOB_PIPE_ONLY && !Blobs_2_objs::S_HAS_2_PIPES, "Blobs-only (2 objects) traits.");
  static_assert(Hndls::S_HAS_NATIVE_HANDLE_PIPE_ONLY && Hndls::S_HAS_NATIVE_HANDLE_PIPE
                  && !Hndls::S_HAS_BLOB_PIPE && !Hndls::S_HAS_BLOB_PIPE_ONLY && !Hndls::S_HAS_2_PIPES,
                "Handles-only shape traits.");
  static_assert(Both::S_HAS_2_PIPES && Both::S_HAS_BLOB_PIPE && Both::S_HAS_NATIVE_HANDLE_PIPE
                  && !Both::S_HAS_BLOB_PIPE_ONLY && !Both::S_HAS_NATIVE_HANDLE_PIPE_ONLY,
                "Both-pipes shape traits.");

  static_assert(Blobs_1_obj::S_IS_SYNC_IO_OBJ && !Blobs_1_obj::S_IS_ASYNC_IO_OBJ, "sync_io form.");
  static_assert(Both::S_IS_SYNC_IO_OBJ && !Both::S_IS_ASYNC_IO_OBJ, "sync_io form.");
  static_assert(Socket_stream_channel<false>::S_IS_ASYNC_IO_OBJ && !Socket_stream_channel<false>::S_IS_SYNC_IO_OBJ,
                "async-I/O form.");
  static_assert(Posix_mqs_socket_stream_channel::S_IS_ASYNC_IO_OBJ, "async-I/O form.");

  // Async_io_obj/Sync_io_obj map between the forms; Channel-of-Null_peers marks the dead end in each direction.
  static_assert(std::is_same_v<Both::Async_io_obj, Bipc_mqs_socket_stream_channel::Base::Base>,
                "sync_io -> async-I/O counterpart.");
  static_assert(std::is_same_v<Both::Async_io_obj::Sync_io_obj, Both::Base::Base>, "...and back.");
  static_assert(std::is_same_v<Both::Sync_io_obj, Channel<Null_peer, Null_peer, Null_peer, Null_peer>>,
                "sync_io form has no sync_io counterpart.");
  static_assert(std::is_same_v<Hndls::Async_io_obj::Async_io_obj, Channel<Null_peer, Null_peer, Null_peer, Null_peer>>,
                "async-I/O form has no async-I/O counterpart.");

  // The concept-API bits forwarded from the stored peers' types.
  static_assert(Both::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION
                  == sync_io::Bipc_mq_receiver::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION, "Batch-size forwarding.");
  static_assert(Both::S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION
                  == Sio_nss::S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION, "Batch-size forwarding.");
  static_assert(Hndls::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION == 0, "Disabled pipe: Null_peer's dummy value.");
  static_assert(std::is_same_v<Both::Blob_snd_stats, sync_io::Bipc_mq_sender::Blob_snd_stats>,
                "Stats-type forwarding.");
  static_assert(std::is_same_v<Both::Native_handle_rcv_stats, Sio_nss::Native_handle_rcv_stats>,
                "Stats-type forwarding.");

  SUCCEED();
}

// Default-cted channels of each alias class: ??? state as documented, including the credentials accessor's null result.
TEST(Channel_test, default_cted)
{
  Socket_stream_channel<true> hndls_sio;
  Socket_stream_channel_of_blobs<false> blobs_aio;
  Posix_mqs_channel_of_blobs mqs_aio;
  sync_io::Bipc_mqs_socket_stream_channel both_sio;

  { FLOW_TEST_TRACE_CTX("Handles-only sync_io."); expect_as_if_default_cted(hndls_sio); }
  { FLOW_TEST_TRACE_CTX("Blobs-only (1 object) async-I/O."); expect_as_if_default_cted(blobs_aio); }
  { FLOW_TEST_TRACE_CTX("Blobs-only (2 objects) async-I/O."); expect_as_if_default_cted(mqs_aio); }
  { FLOW_TEST_TRACE_CTX("Both pipes sync_io."); expect_as_if_default_cted(both_sio); }

  Error_code err_code = error::Code::S_TIMEOUT; // Any truthy value; should be cleared.
  EXPECT_EQ(hndls_sio.remote_peer_process_credentials(&err_code), util::NULL_PROCESS_CREDENTIALS);
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blobs_aio.remote_peer_process_credentials(), util::NULL_PROCESS_CREDENTIALS);
  EXPECT_EQ(both_sio.remote_peer_process_credentials(), util::NULL_PROCESS_CREDENTIALS);
}

/* The by-hand initialization sequence on a raw Channel: initialized() flips only once every enabled pipe has been
 * loaded; each init_*_pipe() succeeds once and refuses dupes; the 1-arg form makes sender and receiver one object. */
TEST(Channel_test, manual_init_sequence)
{
  auto socks = make_sio_nss_pair("a", "b");
  auto& sock_a = socks.first;

  // 1 pipe, 1 object for both directions.
  {
    Channel<Sio_nss, Sio_nss, Null_peer, Null_peer> channel{channel_logger(), "oneObj"};
    EXPECT_EQ(channel.nickname(), "oneObj");
    EXPECT_FALSE(channel.initialized(true));
    EXPECT_EQ(channel.blob_snd(), nullptr);

    EXPECT_TRUE(channel.init_blob_pipe(std::move(sock_a)));
    EXPECT_TRUE(channel.initialized());
    expect_accessors_live(&channel);
    EXPECT_EQ(static_cast<const void*>(channel.blob_snd()), static_cast<const void*>(channel.blob_rcv()));
    EXPECT_EQ(channel.blob_snd()->nickname(), "a"); // The very object we moved in.

    EXPECT_FALSE(channel.init_blob_pipe(Sio_nss{obj_logger(), "dupe"})); // Refused: already loaded.
    EXPECT_TRUE(channel.initialized());
    EXPECT_EQ(channel.blob_snd()->nickname(), "a"); // And the original stays.
    EXPECT_NE(flow::util::ostream_op_string(channel).find(" blob_pipes[snd_rcv["), string::npos);
  }

  // 2 pipes; the blobs one with a separate object per direction.
  {
    Pipe_ends<Posix_mq_handle> ends;
    auto mqs = ends.mqs_a();

    Channel<sync_io::Posix_mq_sender, sync_io::Posix_mq_receiver, Sio_nss, Sio_nss>
      channel{channel_logger(), "twoPipes"};
    EXPECT_FALSE(channel.initialized(true));

    EXPECT_TRUE(channel.init_blob_pipe(sync_io::Posix_mq_sender{obj_logger(), "mqSnd", std::move(mqs.first)},
                                       sync_io::Posix_mq_receiver{obj_logger(), "mqRcv", std::move(mqs.second)}));
    EXPECT_FALSE(channel.initialized(true)); // Still missing the handles pipe.
    EXPECT_NE(channel.blob_snd(), nullptr);
    EXPECT_EQ(channel.hndl_snd(), nullptr);

    EXPECT_TRUE(channel.init_native_handle_pipe(std::move(ends.m_socks.first)));
    EXPECT_TRUE(channel.initialized());
    expect_accessors_live(&channel);
    EXPECT_NE(static_cast<const void*>(channel.blob_snd()), static_cast<const void*>(channel.blob_rcv()));
    EXPECT_EQ(static_cast<const void*>(channel.hndl_snd()), static_cast<const void*>(channel.hndl_rcv()));
    EXPECT_EQ(channel.blob_snd()->nickname(), "mqSnd");
    EXPECT_EQ(channel.blob_rcv()->nickname(), "mqRcv");

    EXPECT_FALSE(channel.init_native_handle_pipe(Sio_nss{obj_logger(), "dupe"}));
    EXPECT_FALSE(channel.init_blob_pipe(sync_io::Posix_mq_sender{}, sync_io::Posix_mq_receiver{}));
    EXPECT_TRUE(channel.initialized());
    const auto str = flow::util::ostream_op_string(channel);
    EXPECT_NE(str.find(" blob_pipes[snd_out["), string::npos);
    EXPECT_NE(str.find(" hndl_pipes[snd_rcv["), string::npos);
  }
}

// The alias classes' PEER-state ctors do the init_*_pipe() work; the socket-bearing ones report the peer's credentials.
TEST(Channel_test, alias_ctors)
{
  const auto own_pid = util::Process_credentials::own_process_id(); // Both ends of every pipe are in this process.

  Pipe_ends<Bipc_mq_handle> ends;
  /* Open all MQ handles up-front: a Blob_stream_mq_sender's dtor removes its MQ's name from the OS (by contract),
   * so once the 1st MQ channel below is gone, its MQs can no longer be opened by name; open handles keep working. */
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  {
    Socket_stream_channel<true> channel{channel_logger(), "hndls", std::move(ends.m_socks.first)};
    EXPECT_TRUE(channel.initialized());
    EXPECT_EQ(channel.nickname(), "hndls");
    EXPECT_EQ(channel.remote_peer_process_credentials().process_id(), own_pid);
  }
  {
    Socket_stream_channel_of_blobs<true> channel{channel_logger(), "blobs", std::move(ends.m_socks.second)};
    EXPECT_TRUE(channel.initialized());
    EXPECT_EQ(channel.remote_peer_process_credentials().process_id(), own_pid);
  }
  {
    sync_io::Bipc_mqs_channel_of_blobs channel{channel_logger(), "mqs",
                                               std::move(mqs_a.first), std::move(mqs_a.second)};
    EXPECT_TRUE(channel.initialized());
    expect_accessors_live(&channel);
  }
  {
    auto socks = make_sio_nss_pair("a", "b");
    Error_code err_code;
    sync_io::Bipc_mqs_socket_stream_channel channel{channel_logger(), "both",
                                                    std::move(mqs_b.first), std::move(mqs_b.second),
                                                    std::move(socks.first), &err_code};
    EXPECT_FALSE(err_code) << err_code.message();
    EXPECT_TRUE(channel.initialized());
    expect_accessors_live(&channel);
    EXPECT_EQ(channel.remote_peer_process_credentials().process_id(), own_pid);
  }
}

/* Mqs_channel ctor error semantics: a failing MQ peer ctor yields a truthy *err_code (or a throw), and *this
 * stays uninitialized; Mqs_socket_stream_channel then leaves its handles pipe alone too.  The failure we can
 * provoke without OS trickery: a 2nd Blob_stream_mq_sender (or _receiver) to an MQ that already has one. */
TEST(Channel_test, mqs_channel_ctor_errors)
{
  using flow::error::Runtime_error;

  Pipe_ends<Posix_mq_handle> ends;
  Error_code err_code;

  // Occupy the sender slot of a2b; then a channel wanting a2b as its out-MQ must fail at the sender step.
  const sync_io::Posix_mq_sender snd_taken{obj_logger(), "sndTaken", ends.m_a2b.open()};
  {
    auto mqs = ends.mqs_a();
    sync_io::Posix_mqs_channel_of_blobs channel{channel_logger(), "mqsErr",
                                                std::move(mqs.first), std::move(mqs.second), &err_code};
    EXPECT_EQ(err_code, error::Code::S_BLOB_STREAM_MQ_SENDER_EXISTS);
    EXPECT_FALSE(channel.initialized(true));
    EXPECT_EQ(channel.blob_snd(), nullptr);
  }
  {
    auto mqs = ends.mqs_a();
    bool threw = false;
    try
    {
      sync_io::Posix_mqs_channel_of_blobs channel{channel_logger(), "mqsThrow",
                                                  std::move(mqs.first), std::move(mqs.second)};
    }
    catch (const Runtime_error& exc)
    {
      threw = true;
      EXPECT_EQ(exc.code(), error::Code::S_BLOB_STREAM_MQ_SENDER_EXISTS);
    }
    EXPECT_TRUE(threw);
  }
  // Ditto but at the receiver step: side B's in-MQ is a2b, whose sender slot is free but receiver slot we now take.
  const sync_io::Posix_mq_receiver rcv_taken{obj_logger(), "rcvTaken", ends.m_a2b.open()};
  {
    auto mqs = ends.mqs_b();
    auto socks = make_sio_nss_pair("a", "b");
    sync_io::Posix_mqs_socket_stream_channel channel{channel_logger(), "bothErr",
                                                     std::move(mqs.first), std::move(mqs.second),
                                                     std::move(socks.first), &err_code};
    EXPECT_EQ(err_code, error::Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS);
    EXPECT_FALSE(channel.initialized(true));
    EXPECT_EQ(channel.blob_snd(), nullptr);
    EXPECT_EQ(channel.hndl_snd(), nullptr); // The handles pipe was skipped, not half-loaded.
  }
}

// Move ctor and move assignment: the target takes over everything; the source becomes as-if default-cted.
TEST(Channel_test, move_ops)
{
  Pipe_ends<Posix_mq_handle> ends;
  auto mqs = ends.mqs_a();
  sync_io::Posix_mqs_socket_stream_channel src{channel_logger(), "orig", std::move(mqs.first), std::move(mqs.second),
                                               std::move(ends.m_socks.first)};
  ASSERT_TRUE(src.initialized());

  sync_io::Posix_mqs_socket_stream_channel via_ctor{std::move(src)};
  {
    FLOW_TEST_TRACE_CTX("Move ctor.");
    expect_as_if_default_cted(src);
    EXPECT_TRUE(via_ctor.initialized());
    EXPECT_EQ(via_ctor.nickname(), "orig");
    EXPECT_EQ(via_ctor.get_logger(), channel_logger());
    expect_accessors_live(&via_ctor);
    EXPECT_EQ(via_ctor.hndl_snd()->nickname(), "sockA");
  }

  sync_io::Posix_mqs_socket_stream_channel via_assign;
  via_assign = std::move(via_ctor);
  {
    FLOW_TEST_TRACE_CTX("Move-assignment into default-cted.");
    expect_as_if_default_cted(via_ctor);
    EXPECT_TRUE(via_assign.initialized());
    EXPECT_EQ(via_assign.nickname(), "orig");
    expect_accessors_live(&via_assign);
    EXPECT_EQ(via_assign.hndl_snd()->nickname(), "sockA");
  }

  // Moving into an initialized channel: its old contents are destroyed (its pipes' MQs, hence the names, released).
  auto mqs_b = ends.mqs_b();
  sync_io::Posix_mqs_socket_stream_channel other{channel_logger(), "other",
                                                 std::move(mqs_b.first), std::move(mqs_b.second),
                                                 std::move(ends.m_socks.second)};
  other = std::move(via_assign);
  FLOW_TEST_TRACE_CTX("Move-assignment into initialized.");
  expect_as_if_default_cted(via_assign);
  EXPECT_EQ(other.nickname(), "orig");
  EXPECT_EQ(other.hndl_snd()->nickname(), "sockA");
}

/* async_io_obj(): each sync_io shape yields its async-I/O counterpart, initialized, with the same nickname and
 * logger, while the source becomes as-if default-cted; and the new channel actually transmits over every pipe. */
namespace
{

template<typename Sio_channel>
void async_io_obj_test(Sio_channel&& sio_a, Sio_channel&& sio_b)
{
  ASSERT_TRUE(sio_a.initialized());
  ASSERT_TRUE(sio_b.initialized());
  const auto nickname_a = sio_a.nickname();

  auto aio_a = sio_a.async_io_obj();
  expect_as_if_default_cted(sio_a);
  static_assert(decltype(aio_a)::S_IS_ASYNC_IO_OBJ, "The point of async_io_obj().");
  EXPECT_TRUE(aio_a.initialized());
  EXPECT_EQ(aio_a.nickname(), nickname_a);
  EXPECT_EQ(aio_a.get_logger(), channel_logger());
  expect_accessors_live(&aio_a);

  auto aio_b = sio_b.async_io_obj();
  { FLOW_TEST_TRACE_CTX("A -> B."); send_and_receive_over_each_pipe(&aio_a, &aio_b); }
  { FLOW_TEST_TRACE_CTX("B -> A."); send_and_receive_over_each_pipe(&aio_b, &aio_a); }
}

} // namespace (anon)

TEST(Channel_test, async_io_obj_blobs_one_obj)
{
  auto socks = make_sio_nss_pair("a", "b");
  async_io_obj_test(Socket_stream_channel_of_blobs<true>{channel_logger(), "blobsA", std::move(socks.first)},
                    Socket_stream_channel_of_blobs<true>{channel_logger(), "blobsB", std::move(socks.second)});
}

TEST(Channel_test, async_io_obj_blobs_two_objs)
{
  Pipe_ends<Posix_mq_handle> ends;
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  async_io_obj_test(sync_io::Posix_mqs_channel_of_blobs{channel_logger(), "mqsA",
                                                        std::move(mqs_a.first), std::move(mqs_a.second)},
                    sync_io::Posix_mqs_channel_of_blobs{channel_logger(), "mqsB",
                                                        std::move(mqs_b.first), std::move(mqs_b.second)});
}

TEST(Channel_test, async_io_obj_hndls)
{
  auto socks = make_sio_nss_pair("a", "b");
  async_io_obj_test(Socket_stream_channel<true>{channel_logger(), "hndlsA", std::move(socks.first)},
                    Socket_stream_channel<true>{channel_logger(), "hndlsB", std::move(socks.second)});
}

TEST(Channel_test, async_io_obj_both)
{
  Pipe_ends<Bipc_mq_handle> ends;
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  async_io_obj_test(sync_io::Bipc_mqs_socket_stream_channel{channel_logger(), "bothA", std::move(mqs_a.first),
                                                            std::move(mqs_a.second), std::move(ends.m_socks.first)},
                    sync_io::Bipc_mqs_socket_stream_channel{channel_logger(), "bothB", std::move(mqs_b.first),
                                                            std::move(mqs_b.second), std::move(ends.m_socks.second)});
}

/* The per-pipe forwarders (max-sizes, stats, stats-reset, send/receive) yield exactly what the stored peer would;
 * and the bundle-wide once-only methods (auto_ping(), idle_timer_run(), end_sending()) hit every pipe, so that
 * a dupe call is refused both by the Channel and by each peer individually; after end_sending() the opposing
 * side sees graceful-close on each pipe. */
namespace
{

template<typename Channel_t>
void bundle_methods_test(Channel_t* a, Channel_t* b)
{
  static_assert(Channel_t::S_IS_ASYNC_IO_OBJ, "This exercises the async-I/O transmission API.");

  send_and_receive_over_each_pipe(a, b);

  if constexpr(Channel_t::S_HAS_BLOB_PIPE)
  {
    EXPECT_EQ(a->send_blob_max_size(), a->blob_snd()->send_blob_max_size());
    EXPECT_EQ(b->receive_blob_max_size(), b->blob_rcv()->receive_blob_max_size());
    EXPECT_EQ(a->blob_send_stats().m_total_msgs, 1u);
    EXPECT_EQ(a->blob_send_stats().m_total_bytes, a->blob_snd()->blob_send_stats().m_total_bytes);
    EXPECT_EQ(b->blob_receive_stats().m_total_msgs, 1u);
    EXPECT_EQ(b->blob_receive_stats().m_total_bytes, b->blob_rcv()->blob_receive_stats().m_total_bytes);
    a->blob_send_stats_reset();
    b->blob_receive_stats_reset();
    EXPECT_EQ(a->blob_snd()->blob_send_stats().m_total_msgs, 0u);
    EXPECT_EQ(b->blob_rcv()->blob_receive_stats().m_total_msgs, 0u);
  }
  if constexpr(Channel_t::S_HAS_NATIVE_HANDLE_PIPE)
  {
    EXPECT_EQ(a->send_meta_blob_max_size(), a->hndl_snd()->send_meta_blob_max_size());
    EXPECT_EQ(b->receive_meta_blob_max_size(), b->hndl_rcv()->receive_meta_blob_max_size());
    EXPECT_EQ(a->native_handle_send_stats().m_total_msgs, 1u);
    EXPECT_EQ(a->native_handle_send_stats().m_total_bytes, a->hndl_snd()->native_handle_send_stats().m_total_bytes);
    EXPECT_EQ(b->native_handle_receive_stats().m_total_msgs, 1u);
    a->native_handle_send_stats_reset();
    b->native_handle_receive_stats_reset();
    EXPECT_EQ(a->hndl_snd()->native_handle_send_stats().m_total_msgs, 0u);
    EXPECT_EQ(b->hndl_rcv()->native_handle_receive_stats().m_total_msgs, 0u);
  }

  // Once-only bundle-wide methods.  (Periods/timeouts are long: nothing should fire during the test.)
  const auto long_time = boost::chrono::hours{1};
  EXPECT_TRUE(a->auto_ping(long_time));
  EXPECT_FALSE(a->auto_ping(long_time));
  EXPECT_TRUE(b->idle_timer_run(long_time));
  EXPECT_FALSE(b->idle_timer_run(long_time));
  EXPECT_TRUE(a->end_sending());
  EXPECT_FALSE(a->end_sending());
  if constexpr(Channel_t::S_HAS_BLOB_PIPE)
  {
    EXPECT_FALSE(a->blob_snd()->auto_ping(long_time));
    EXPECT_FALSE(b->blob_rcv()->idle_timer_run(long_time));
    EXPECT_FALSE(a->blob_snd()->end_sending());
    EXPECT_EQ(receive_blob(b).m_err_code, error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
  }
  if constexpr(Channel_t::S_HAS_NATIVE_HANDLE_PIPE)
  {
    EXPECT_FALSE(a->hndl_snd()->auto_ping(long_time));
    EXPECT_FALSE(b->hndl_rcv()->idle_timer_run(long_time));
    EXPECT_FALSE(a->hndl_snd()->end_sending());
    EXPECT_EQ(receive_hndl(b).m_err_code, error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
  }
}

} // namespace (anon)

TEST(Channel_test, bundle_methods_blobs)
{
  auto socks = make_sio_nss_pair("a", "b");
  Socket_stream_channel_of_blobs<false> a{channel_logger(), "blobsA", Native_socket_stream{std::move(socks.first)}};
  Socket_stream_channel_of_blobs<false> b{channel_logger(), "blobsB", Native_socket_stream{std::move(socks.second)}};
  bundle_methods_test(&a, &b);
}

TEST(Channel_test, bundle_methods_hndls)
{
  auto socks = make_sio_nss_pair("a", "b");
  Socket_stream_channel<false> a{channel_logger(), "hndlsA", Native_socket_stream{std::move(socks.first)}};
  Socket_stream_channel<false> b{channel_logger(), "hndlsB", Native_socket_stream{std::move(socks.second)}};
  bundle_methods_test(&a, &b);
}

TEST(Channel_test, bundle_methods_both)
{
  Pipe_ends<Posix_mq_handle> ends;
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  Posix_mqs_socket_stream_channel a{channel_logger(), "bothA", std::move(mqs_a.first), std::move(mqs_a.second),
                                    Native_socket_stream{std::move(ends.m_socks.first)}};
  Posix_mqs_socket_stream_channel b{channel_logger(), "bothB", std::move(mqs_b.first), std::move(mqs_b.second),
                                    Native_socket_stream{std::move(ends.m_socks.second)}};
  bundle_methods_test(&a, &b);
}

/* The sync_io-only forwarders: replace_event_wait_handles() and start_*_ops() reach every stored peer, so
 * dupes are refused; and a sync_io Channel so started transmits over each pipe (to an async-I/O opposing side). */
TEST(Channel_test, sync_io_forwarders)
{
  // Declared before the sync_io channel whose wait-handles will refer to its Task_engine: it must outlive them.
  Single_thread_task_loop loop{obj_logger(), "sioLoop"};
  loop.start();

  Pipe_ends<Bipc_mq_handle> ends;
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  sync_io::Bipc_mqs_socket_stream_channel a{channel_logger(), "sioA", std::move(mqs_a.first), std::move(mqs_a.second),
                                            std::move(ends.m_socks.first)};
  Bipc_mqs_socket_stream_channel b{channel_logger(), "aioB", std::move(mqs_b.first), std::move(mqs_b.second),
                                   Native_socket_stream{std::move(ends.m_socks.second)}};

  start_sio_ops(&a, &loop);

  send_and_receive_over_each_pipe(&a, &b);
  // sync_io sends complete synchronously, so the stats forwarders can be checked right away, per pipe.
  EXPECT_EQ(a.blob_send_stats().m_total_msgs, 1u);
  EXPECT_EQ(a.native_handle_send_stats().m_total_msgs, 1u);

  EXPECT_TRUE(a.end_sending());
  EXPECT_EQ(receive_blob(&b).m_err_code, error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
  EXPECT_EQ(receive_hndl(&b).m_err_code, error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
}

/* async_end_sending(): the one place Channel has logic of its own.  With 2 pipes it must combine the 2 peers'
 * completions into one, and in the sync_io form each peer can either complete synchronously or report would-block
 * and complete later via its handler.  We drive each combination by filling a pipe's low-level buffer while the
 * receiver is idle, so that pipe's graceful-close message has to queue; then drain the receiver, which must get
 * every pre-end message and then the graceful-close, while the completion handler fires exactly once (if at all). */
namespace
{

// Sends one max-size message, every byte = `idx` (so the receiver can check ordering), over the given pipe.
template<bool BLOB_ELSE_HNDL, typename Snd_channel>
void send_marked_msg(Snd_channel* snd, size_t idx)
{
  Error_code err_code;
  if constexpr(BLOB_ELSE_HNDL)
  {
    const vector<uint8_t> payload(snd->send_blob_max_size(), uint8_t(idx));
    EXPECT_TRUE(snd->send_blob(as_blob(payload), &err_code));
  }
  else
  {
    const vector<uint8_t> payload(snd->send_meta_blob_max_size(), uint8_t(idx));
    EXPECT_TRUE(snd->send_native_handle(Native_handle{}, as_blob(payload), &err_code));
  }
  EXPECT_FALSE(err_code) << "Message [" << idx << "]: " << err_code.message();
}

template<bool BLOB_ELSE_HNDL, typename Snd_channel>
uint64_t would_block_count(const Snd_channel& snd)
{
  if constexpr(BLOB_ELSE_HNDL) { return snd.blob_send_stats().m_would_block_count; }
  else { return snd.native_handle_send_stats().m_would_block_count; }
}

/* Sends marked messages over the given pipe (whose receiver must be idle) until one has to be queued due to
 * would-block; returns how many were sent, or 1 if `fill` is `false` (just the one message, no would-block). */
template<bool BLOB_ELSE_HNDL, typename Snd_channel>
size_t send_marked_msgs(Snd_channel* snd, bool fill)
{
  size_t n_sent = 0;
  do
  {
    send_marked_msg<BLOB_ELSE_HNDL>(snd, n_sent);
    ++n_sent;
    if (n_sent == 100000)
    {
      ADD_FAILURE() << "Low-level send buffer never filled up?  Test premise is off.";
      break;
    }
  }
  while (fill && (would_block_count<BLOB_ELSE_HNDL>(*snd) == 0));
  return n_sent;
}

template<bool BLOB_ELSE_HNDL, typename Rcv_channel>
Rcv_result receive_one(Rcv_channel* rcv)
{
  if constexpr(BLOB_ELSE_HNDL) { return receive_blob(rcv); }
  else { return receive_hndl(rcv); }
}

// Receives `n_msgs` marked messages over the given pipe, checking each; then expects the graceful-close.
template<bool BLOB_ELSE_HNDL, typename Rcv_channel>
void drain_pipe(Rcv_channel* rcv, size_t n_msgs)
{
  FLOW_TEST_TRACE_CTX(BLOB_ELSE_HNDL ? "Blob pipe." : "Handles pipe.");
  for (size_t idx = 0; idx != n_msgs; ++idx)
  {
    const auto result = receive_one<BLOB_ELSE_HNDL>(rcv);
    ASSERT_FALSE(result.m_err_code) << "Message [" << idx << "]: " << result.m_err_code.message();
    ASSERT_FALSE(result.m_payload.empty());
    EXPECT_EQ(result.m_payload.front(), uint8_t(idx)) << "Message [" << idx << "].";
  }
  EXPECT_EQ(receive_one<BLOB_ELSE_HNDL>(rcv).m_err_code, error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
}

// Bookkeeping for an async_end_sending() completion handler, which may run on some background thread.
struct End_sending_done
{
  unsigned int m_n_calls = 0; // Read by the test only after wait() (or once nothing can be running anymore).
  Error_code m_err_code;
  promise<void> m_done;

  auto handler()
  {
    return [this](const Error_code& err_code)
    {
      m_err_code = err_code;
      if (++m_n_calls == 1) { m_done.set_value(); }
    };
  }

  void wait()
  {
    m_done.get_future().wait();
  }
};

/* The sync_io overload, over `a` (sync_io; ops started) with the opposing async-I/O `b` idle: fill the pipe(s)
 * indicated, or send 1 message over each pipe otherwise; end sending; check the synchronous result; check that
 * dupes are refused; drain `b`; check the completion handler fired exactly once, if and only if would-block. */
template<typename Sio_channel, typename Aio_channel>
void end_sending_sio_test(Sio_channel* a, Aio_channel* b, bool fill_blob_pipe, bool fill_hndl_pipe)
{
  [[maybe_unused]] size_t n_blobs = 0;
  [[maybe_unused]] size_t n_hndls = 0;
  if constexpr(Sio_channel::S_HAS_BLOB_PIPE) { n_blobs = send_marked_msgs<true>(a, fill_blob_pipe); }
  if constexpr(Sio_channel::S_HAS_NATIVE_HANDLE_PIPE) { n_hndls = send_marked_msgs<false>(a, fill_hndl_pipe); }

  End_sending_done done;
  Error_code sync_err_code;
  EXPECT_TRUE(a->async_end_sending(&sync_err_code, done.handler()));
  const bool would_block = fill_blob_pipe || fill_hndl_pipe;
  if (would_block)
  {
    EXPECT_EQ(sync_err_code, error::Code::S_SYNC_IO_WOULD_BLOCK);
  }
  else
  {
    EXPECT_FALSE(sync_err_code) << sync_err_code.message();
  }

  // Dupe calls are refused, whether the first one is still pending or not.
  End_sending_done dupe;
  Error_code dupe_err_code;
  EXPECT_FALSE(a->async_end_sending(&dupe_err_code, dupe.handler()));
  EXPECT_FALSE(a->end_sending());

  if constexpr(Sio_channel::S_HAS_BLOB_PIPE) { drain_pipe<true>(b, n_blobs); }
  if constexpr(Sio_channel::S_HAS_NATIVE_HANDLE_PIPE) { drain_pipe<false>(b, n_hndls); }

  if (would_block)
  {
    done.wait();
    EXPECT_FALSE(done.m_err_code) << done.m_err_code.message();
  }
  EXPECT_EQ(done.m_n_calls, would_block ? 1u : 0u);
  EXPECT_EQ(dupe.m_n_calls, 0u);
}

// The async-I/O overload: same idea, except its handler fires in all cases, once both pipes' sends are flushed.
template<typename Aio_channel>
void end_sending_aio_test(Aio_channel* a, Aio_channel* b, bool fill_blob_pipe, bool fill_hndl_pipe)
{
  size_t n_blobs = 0;
  size_t n_hndls = 0;
  if constexpr(Aio_channel::S_HAS_BLOB_PIPE) { n_blobs = send_marked_msgs<true>(a, fill_blob_pipe); }
  if constexpr(Aio_channel::S_HAS_NATIVE_HANDLE_PIPE) { n_hndls = send_marked_msgs<false>(a, fill_hndl_pipe); }

  End_sending_done done;
  EXPECT_TRUE(a->async_end_sending(done.handler()));
  End_sending_done dupe;
  EXPECT_FALSE(a->async_end_sending(dupe.handler()));
  EXPECT_FALSE(a->end_sending());

  if constexpr(Aio_channel::S_HAS_BLOB_PIPE) { drain_pipe<true>(b, n_blobs); }
  if constexpr(Aio_channel::S_HAS_NATIVE_HANDLE_PIPE) { drain_pipe<false>(b, n_hndls); }

  done.wait();
  EXPECT_FALSE(done.m_err_code) << done.m_err_code.message();
  EXPECT_EQ(done.m_n_calls, 1u);
  EXPECT_EQ(dupe.m_n_calls, 0u);
}

} // namespace (anon)

// 1 pipe (blobs): the forward-only arm; immediate completion, then would-block.
TEST(Channel_test, async_end_sending_sio_blobs_only)
{
  for (const bool fill : {false, true})
  {
    FLOW_TEST_TRACE_CTX("Fill pipe (would-block)? = [", fill, "].");
    Single_thread_task_loop loop{obj_logger(), "sioLoop"};
    loop.start();
    auto socks = make_sio_nss_pair("a", "b");
    Socket_stream_channel_of_blobs<true> a{channel_logger(), "sioA", std::move(socks.first)};
    Socket_stream_channel_of_blobs<false> b{channel_logger(), "aioB", Native_socket_stream{std::move(socks.second)}};
    start_sio_ops(&a, &loop);
    end_sending_sio_test(&a, &b, fill, false);
  }
}

// 1 pipe (handles): ditto.
TEST(Channel_test, async_end_sending_sio_hndls_only)
{
  for (const bool fill : {false, true})
  {
    FLOW_TEST_TRACE_CTX("Fill pipe (would-block)? = [", fill, "].");
    Single_thread_task_loop loop{obj_logger(), "sioLoop"};
    loop.start();
    auto socks = make_sio_nss_pair("a", "b");
    Socket_stream_channel<true> a{channel_logger(), "sioA", std::move(socks.first)};
    Socket_stream_channel<false> b{channel_logger(), "aioB", Native_socket_stream{std::move(socks.second)}};
    start_sio_ops(&a, &loop);
    end_sending_sio_test(&a, &b, false, fill);
  }
}

// 2 pipes: every combination of which pipe(s) would-block.  (Posix MQs: they have an FD, so no worker thread.)
TEST(Channel_test, async_end_sending_sio_both)
{
  for (const bool fill_blob_pipe : {false, true})
  {
    for (const bool fill_hndl_pipe : {false, true})
    {
      FLOW_TEST_TRACE_CTX("Fill blob pipe? = [", fill_blob_pipe, "]; fill handles pipe? = [", fill_hndl_pipe, "].");
      Single_thread_task_loop loop{obj_logger(), "sioLoop"};
      loop.start();
      Pipe_ends<Posix_mq_handle> ends;
      auto mqs_a = ends.mqs_a();
      auto mqs_b = ends.mqs_b();
      sync_io::Posix_mqs_socket_stream_channel a{channel_logger(), "sioA",
                                                 std::move(mqs_a.first), std::move(mqs_a.second),
                                                 std::move(ends.m_socks.first)};
      Posix_mqs_socket_stream_channel b{channel_logger(), "aioB", std::move(mqs_b.first), std::move(mqs_b.second),
                                        Native_socket_stream{std::move(ends.m_socks.second)}};
      start_sio_ops(&a, &loop);
      end_sending_sio_test(&a, &b, fill_blob_pipe, fill_hndl_pipe);
    }
  }
}

// The async-I/O overload with 2 pipes: nothing queued; then both pipes queued.
TEST(Channel_test, async_end_sending_aio_both)
{
  for (const bool fill : {false, true})
  {
    FLOW_TEST_TRACE_CTX("Fill both pipes (would-block)? = [", fill, "].");
    Pipe_ends<Posix_mq_handle> ends;
    auto mqs_a = ends.mqs_a();
    auto mqs_b = ends.mqs_b();
    Posix_mqs_socket_stream_channel a{channel_logger(), "aioA", std::move(mqs_a.first), std::move(mqs_a.second),
                                      Native_socket_stream{std::move(ends.m_socks.first)}};
    Posix_mqs_socket_stream_channel b{channel_logger(), "aioB", std::move(mqs_b.first), std::move(mqs_b.second),
                                      Native_socket_stream{std::move(ends.m_socks.second)}};
    end_sending_aio_test(&a, &b, fill, fill);
  }
}

/* A hosed pipe: async_end_sending() still returns true but completes synchronously with that pipe's error and never
 * invokes the handler; with 2 pipes the Channel reports the one error even though the other pipe ended fine.
 * The throwing form (null Error_code*) throws it.  We hose the handles pipe by destroying the opposing side. */
TEST(Channel_test, async_end_sending_sio_hosed)
{
  using flow::error::Runtime_error;

  Single_thread_task_loop loop{obj_logger(), "sioLoop"};
  loop.start();
  Pipe_ends<Posix_mq_handle> ends;
  auto mqs_a = ends.mqs_a();
  auto mqs_b = ends.mqs_b();
  sync_io::Posix_mqs_socket_stream_channel a{channel_logger(), "sioA", std::move(mqs_a.first), std::move(mqs_a.second),
                                             std::move(ends.m_socks.first)};
  std::optional<Posix_mqs_socket_stream_channel> b{std::in_place, channel_logger(), "aioB",
                                                   std::move(mqs_b.first), std::move(mqs_b.second),
                                                   Native_socket_stream{std::move(ends.m_socks.second)}};
  start_sio_ops(&a, &loop);

  b.reset(); // Opposing socket closed: the next send over the handles pipe fails, hosing it.
  Error_code hose_err_code;
  EXPECT_TRUE(a.send_native_handle(Native_handle{}, as_blob(HNDL_PAYLOAD), &hose_err_code));
  ASSERT_TRUE(bool(hose_err_code)) << "Expected the handles pipe to be hosed by now.";
  EXPECT_NE(hose_err_code, error::Code::S_SYNC_IO_WOULD_BLOCK);

  End_sending_done done;
  bool threw = false;
  try
  {
    a.async_end_sending(nullptr, done.handler());
  }
  catch (const Runtime_error& exc)
  {
    threw = true;
    EXPECT_EQ(exc.code(), hose_err_code);
  }
  EXPECT_TRUE(threw);

  // Sends-finished state took effect regardless: dupes refused, even by the throwing form (nothing left to throw).
  Error_code dupe_err_code;
  EXPECT_FALSE(a.async_end_sending(&dupe_err_code, done.handler()));
  EXPECT_FALSE(a.async_end_sending(nullptr, done.handler()));
  EXPECT_FALSE(a.end_sending());

  loop.stop(); // So that the no-handler-fired check is fully deterministic.
  EXPECT_EQ(done.m_n_calls, 0u);
}

// Transmission and conversion on an uninitialized Channel: the documented assertion trips.
TEST(Channel_DeathTest, uninitialized_use)
{
#ifdef NDEBUG
  GTEST_SKIP() << "Death tests rely on assert()s which are disabled in this (NDEBUG) build.";
#endif
  Socket_stream_channel_of_blobs<true> sio;
  Socket_stream_channel<false> aio;
  flow::util::Task_engine io;

  EXPECT_DEATH(sio.send_blob(as_blob(BLOB_PAYLOAD)), "initialized");
  EXPECT_DEATH(aio.send_native_handle(Native_handle{}, as_blob(HNDL_PAYLOAD)), "initialized");
  EXPECT_DEATH(aio.end_sending(), "initialized");
  EXPECT_DEATH(sio.async_io_obj(), "initialized");
  EXPECT_DEATH(sio.replace_event_wait_handles([&]() { return Asio_waitable_native_handle{io}; }), "initialized");
}

} // namespace ipc::transport::test
