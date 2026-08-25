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
#include "ipc/transport/blob_transport_stats.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/common.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/log/log.hpp>
#include <gtest/gtest.h>
#include <boost/thread/future.hpp>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

// In this file: NSS = Native_socket_stream.

namespace ipc::transport::test
{

namespace
{

using ipc::transport::stat::blob_snd_stats; // Proper way to do ADL for this pattern (not mere syntactic sugar).
using ipc::transport::stat::blob_rcv_stats; // Ditto.

using flow::log::Logger;
using Raw_blob = flow::util::Blob_sans_log_context;

// Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
// Survives across all TESTs in this TU; object internals use `logger` (toggleable) instead.
ipc::test::Test_logger g_logger_obj;
Logger* const g_logger_console = &g_logger_obj;

/* In this test's variations, T is either an NSS (Native_socket_stream: low-level transport that uses Unix domain
 * sockets) -- being used a receiver or sender depending on the context; or a
 * Blob_stream_mq_{send|receiv}er<> (call it B_s + B_r temporarily: it uses POSIX or bipc MQs);
 * *or* Channel<Null_peer, Null_peer, NSS, NSS>; *or* Channel<B_s, B_r, Null_peer, Null_peer>.
 * NSS can transmit blobs and native handles (in Linux et al a/k/a FDs); hence the Channel<-, -, NSS, NSS>
 * has those exact capabilities also; we therefore want to exercise (to some extent) both things.
 * B_s + B_r can transmit only blobs; hence Channel<B_s, B_r, -, -> ditto; hence we exercise only that part.
 * Trying to, even if at runtime we skip that path, send native-handles there would not compile.
 *
 * To recap: In addition to testing the send/receive/receive-batch actions and specifically that they are reflected
 * in the resulting Blob_{snd|rcv}_stats values returned by various accessors of the actual low-level transport
 * concept impls (NSS and B_s/B_r), we also test that all of this is properly forwarded by the Channel<> bundler
 * class to the objects being bundled (in our case, just one bidirectional pipe of either type: NSS/NSS
 * which can transmit handles and blob; or B_s/B_r which can transmit blobs only).
 *
 * We don't want `if constexpr()` about handle-transmitting ability of the pipe object(s) being tested strewn
 * all across the test code, so the following contains ~all the ops that depend on whether it's a
 * handle-transmitting T or not and wraps them in such a way as to do the proper thing at compile-time.
 *
 * How to do it?  Well, T here is either a receiver or a sender peer object (doesn't matter); and as discussed
 * it is either NSS or B_{s|r} or Channel<NSS> or Channel<B_{s|r}>.  So S_USE_NH_API<T> shall be true if and only
 * if it's NSS or Channel<NSS>; hence obv false otherwise.  Based on this for each op (e.g., send) we
 * check S_USE_NH_API<T> and compile-time do the handles-and-blobs-involving thing or the blobs-only-involving thing. */

/* @todo Once NSS + B_s + B_r also have ::S_HAS_BLOB_PIPE et al, to determine whether it's a Blob_{sender|receiver} or
 * Native_handle_{sender|receiver}, we can just us that to assign S_USE_NH_API.
 *
 * Until then we do a bit of haxoring: We use this truth table to compute USE_NH_API<T>:
 *   Is NSS => true // Well, duh.  NSS can transmit `Native_handle`s.
 *   Is not NSS; is Channel and !Channel::S_HAS_BLOB_PIPE => true // It's Channel<NSS, NSS>.  Hence ditto.
 *   Is not NSS; is Channel and Channel::S_HAS_BLOB_PIPE => false // It's Channel<B_s, B_r>.
 *   Is not NSS; is not Channel => false // It's B_s or B_r.  Cannot transmit handles.
 * We could also check directly against B_s or B_r, but they are actually templates, each with at least 2 different
 * possible template parameters (POSIX versus bipc MQ type), so that'd be really wordy and error-prone.
 *
 * Is it dirty to check against the NSS type specifically?  Well, not really.  The test tests what it tests, and
 * NSS is what (in vanilla Flow-IPC, which we're testing -- not user extensions) can transmit handles.
 *
 * We don't mean for this to be complex; the @todo being done would really make this all go away and make it
 * fully generic. */
template<typename T, typename = void>
struct Is_nss_channel : std::false_type {};
template<typename T>
struct Is_nss_channel<T, std::void_t<decltype(T::S_HAS_BLOB_PIPE)>> : std::bool_constant<!T::S_HAS_BLOB_PIPE> {};
// This thing answers the q "Is it a Channel<Null_peer, Null_peer, NSS, NSS>?".

template<typename T>
constexpr bool S_USE_NH_API = std::is_same_v<T, Native_socket_stream> || Is_nss_channel<T>::value;

template<typename Snd_t>
bool do_send_blob(Snd_t& snd, const util::Blob_const& blob, Error_code* err)
{
  if constexpr(S_USE_NH_API<Snd_t>) { return snd.send_native_handle(Native_handle{}, blob, err); }
  else { return snd.send_blob(blob, err); }
}

template<typename T>
auto do_send_stats(const T& obj)
{
  if constexpr(S_USE_NH_API<T>) { return obj.native_handle_send_stats(); }
  else { return obj.blob_send_stats(); }
}

template<typename T>
void do_send_stats_reset(T& obj)
{
  if constexpr(S_USE_NH_API<T>) { obj.native_handle_send_stats_reset(); }
  else { obj.blob_send_stats_reset(); }
}

template<typename T>
auto do_receive_stats(const T& obj)
{
  if constexpr(S_USE_NH_API<T>) { return obj.native_handle_receive_stats(); }
  else { return obj.blob_receive_stats(); }
}

template<typename T>
void do_receive_stats_reset(T& obj)
{
  if constexpr(S_USE_NH_API<T>) { obj.native_handle_receive_stats_reset(); }
  else { obj.blob_receive_stats_reset(); }
}

template<typename T>
size_t do_receive_max_size(const T& obj)
{
  if constexpr(S_USE_NH_API<T>) { return obj.receive_meta_blob_max_size(); }
  else { return obj.receive_blob_max_size(); }
}

// A few more uses of S_USE_NH_API, which are repeated less, are in the code below; but the idea is the same.

// State shared across async drain callbacks.
struct Drain_state
{
  Raw_blob m_buf;
  Native_handle m_hndl;
  size_t m_remaining;

  explicit Drain_state(size_t n) :
    m_buf(0x20000), // Larger than S_MAX_META_BLOB_LENGTH.
    m_remaining(n)
  {
    assert(m_buf.size() > Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH);
  }

  void buf_resize(size_t n)
  {
    if (n > m_buf.capacity())
    {
      m_buf.make_zero(); // Otherwise Raw_blob shall refuse to do it (never deallocs except via make_zero() or move).
    }
    m_buf.resize(n);
  }
};

// Send N blobs of the given size (works for any Blob_sender, Channel, or mixed).
template<typename Snd_t>
void send_blobs(Snd_t& snd, size_t n, size_t sz)
{
  const std::vector<uint8_t> buf(sz, 0xAB);
  const util::Blob_const blob{buf.data(), buf.size()};
  for (size_t i = 0; i < n; ++i)
  {
    Error_code err;
    const bool ok = do_send_blob(snd, blob, &err);
    ASSERT_TRUE(ok) << "send_blob() returned false.";
    ASSERT_FALSE(err) << "send_blob() err: [" << err << "] [" << err.message() << "].";
  }
}

// Send N native-handle-bearing messages (NSS-only; works with raw NSS or NSS Channel).
template<typename Snd_t>
void send_native_handle_msgs(Snd_t& snd, size_t n, size_t meta_sz)
{
  const std::vector<uint8_t> buf(meta_sz, 0xCD);
  const util::Blob_const meta_blob{buf.data(), buf.size()};
  for (size_t i = 0; i < n; ++i)
  {
    Native_handle hndl{::dup(STDOUT_FILENO)};
    ASSERT_FALSE(hndl.null());
    Error_code err;
    const bool ok = snd.send_native_handle(hndl, meta_blob, &err);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(err) << "send_native_handle() err: [" << err << "] [" << err.message() << "].";
  }
}

/* Asynchronously drain N messages.  Uses async_receive_blob() or async_receive_native_handle()
 * depending on whether Rcv_t requires the NH API (e.g., NSS-only Channel). */
template<typename Rcv_t>
void drain_one(Rcv_t& rcv, std::shared_ptr<Drain_state> state,
               flow::async::Single_thread_task_loop& loop,
               Function<void()> on_done)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  if (state->m_remaining == 0)
  {
    FLOW_LOG_INFO("drain_one(): All drained; invoking on_done().");
    on_done();
    return;
  }
  // else

  FLOW_LOG_INFO("drain_one(): Issuing async_receive; [" << state->m_remaining << "] remaining.");

  const auto buf = state->m_buf.mutable_buffer();
  auto handler = [&rcv, state, &loop, on_done = std::move(on_done)]
                   (const Error_code& err, size_t) mutable
  {
    loop.post([&rcv, state, &loop, on_done = std::move(on_done), err]() mutable
    {
      FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
      FLOW_LOG_INFO("drain_one(): Received; err=[" << err << "]; "
                    "[" << state->m_remaining << "] were remaining.");
      EXPECT_FALSE(err) << "async_receive err: [" << err << "] [" << err.message() << "].";
      // Clean up any received native handle (no-op for blob-only path).
      state->m_hndl.close(); // No-op if .null().
      --state->m_remaining;
      drain_one(rcv, std::move(state), loop, std::move(on_done));
    });
  };

  if constexpr(S_USE_NH_API<Rcv_t>)
  {
    rcv.async_receive_native_handle(&state->m_hndl, buf, std::move(handler));
  }
  else
  {
    rcv.async_receive_blob(buf, std::move(handler));
  }
}

template<typename Rcv_t>
void drain_receives(Rcv_t& rcv, size_t n, size_t buf_sz,
                    flow::async::Single_thread_task_loop& loop,
                    Function<void()> on_done)
{
  auto state = std::make_shared<Drain_state>(n);
  state->buf_resize(buf_sz);
  drain_one(rcv, state, loop, std::move(on_done));
}

// Create a prepared batch with n_slots slots, each with a buf_sz-byte receive buffer.
template<typename Batch_t>
std::shared_ptr<Batch_t> make_batch(size_t n_slots, size_t buf_sz)
{
  auto batch = std::make_shared<Batch_t>(n_slots);
  do
  {
    Raw_blob blob{buf_sz};
    batch->prepare_target_payload(blob.mutable_buffer(), std::move(blob));
  }
  while (!batch->initialized());
  return batch;
}

/* Asynchronously drain `remaining` messages via batch-receives.
 * Dispatches between blob and native-handle batch API based on S_USE_NH_API. */
template<typename Rcv_t, typename Batch_t>
void batch_drain_one(Rcv_t& rcv, std::shared_ptr<Batch_t> batch,
                     size_t remaining,
                     flow::async::Single_thread_task_loop& loop,
                     Function<void()> on_done)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  if (remaining == 0)
  {
    FLOW_LOG_INFO("batch_drain_one(): All drained; invoking on_done().");
    on_done();
    return;
  }
  // else

  FLOW_LOG_INFO("batch_drain_one(): Issuing async_receive_*_batch; "
                "[" << remaining << "] remaining.");

  auto handler = [&rcv, batch, remaining, &loop, on_done = std::move(on_done)]
                   (const Error_code& err) mutable
  {
    loop.post([&rcv, batch, remaining, &loop, on_done = std::move(on_done), err]() mutable
    {
      FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

      const auto n = batch->n_used();
      FLOW_LOG_INFO("batch_drain_one(): Received [" << n << "] in batch; err=[" << err << "]; "
                    "[" << remaining << "] were remaining.");
      EXPECT_FALSE(err) << "async_receive_*_batch err: "
                           "[" << err << "] [" << err.message() << "].";

      remaining -= n;
      batch->clear_used();
      batch_drain_one(rcv, std::move(batch), remaining, loop, std::move(on_done));
    });
  };

  if constexpr(S_USE_NH_API<Rcv_t>)
  {
    rcv.template async_receive_native_handle_batch<Raw_blob>(batch.get(), false, std::move(handler));
  }
  else
  {
    rcv.template async_receive_blob_batch<Raw_blob>(batch.get(), false, std::move(handler));
  }
}

template<typename Rcv_t>
void batch_drain_receives(Rcv_t& rcv, size_t n_slots, size_t buf_sz, size_t total,
                          flow::async::Single_thread_task_loop& loop,
                          Function<void()> on_done)
{
  if constexpr(S_USE_NH_API<Rcv_t>)
  {
    using Batch_t = typename Rcv_t::template Native_handle_batch_in<Raw_blob>;
    batch_drain_one(rcv, make_batch<Batch_t>(n_slots, buf_sz), total, loop, std::move(on_done));
  }
  else
  {
    using Batch_t = typename Rcv_t::template Blob_batch_in<Raw_blob>;
    batch_drain_one(rcv, make_batch<Batch_t>(n_slots, buf_sz), total, loop, std::move(on_done));
  }
}

/* Unified send/receive stats test, parameterized on MqType and VIA_CHANNEL.
 * MqType: NONE = NSS (use Native_socket_stream), BIPC/POSIX = MQ (use Blob_stream_mq_*er of that MQ-type).
 * VIA_CHANNEL: false = operate on extracted pipe objects; true = operate on Channel directly; forward to
 * the pipe objects bundled inside.  Hence, when exercised, Channel is being used in its capacity as implementing
 * up to all four of the concepts: {Native_handle|Blob}_{sender|receiver}.
 *
 * Once the forwarding or lack thereof is achieved, both paths exercise the same test phases.
 *
 * Please see commentary near definition of S_USE_NH_API.  Then come back here.
 *
 * Note: We operate on these exclusively as async-I/O-pattern objects, not sync_io-pattern.  So whether it's Channel
 * or NSS, we use it in the mode where it'll create background thread(s), perform async I/O in the background,
 * and invoke completion handlers we supply, there.  Why so?  Answer:
 *   - It directly tests the stats-accessor/resetter APIs at the async-I/O concept level.
 *     - It indirectly still tests the same APIs at the sync_io concept level, because in reality these perf-sensitive
 *       classes are implemented as follows: sync_io core wrapped in async-I/O core wrangler that starts thread(s)
 *       that'll do all the required asynchrony if any.  Hence, e.g., x.blob_send_stats_reset() (where
 *       x is a transport::X) will test both that and indirectly xx.blob_send_stats_reset() (where xx is
 *       transport::sync_io::X).
 *   - It is easier to work with it.  That's really the appeal of the async-I/O guys; they take care of
 *     asynchrony and such for you at the cost of fine control of the thread structure and the event loop.
 *   - @todo Nevertheless an *exhaustive* test would test sync_io directly as well.
 *
 * Last (but not least), ignoring the above question(s) of who/when/how to set up the basic I/O objects:
 * once they are set up: We do actual transmission and stats-tracking.  How exhaustive is this part?  Answer:
 * It's pretty good, but as of this writing it is not (yet?) meant to be exhaustive.  Generally we tickle all or
 * most of the stats and sanity-check all the basics.
 *
 * @todo An *exhaustive* test would explore every stat and corner case and ensure each of those is exercised.
 * This could be done in this test case generator or separately.  In the meantime what's already here capably
 * exercises a cross-section. */
template<session::schema::MqType MQ_TYPE, bool VIA_CHANNEL>
void test_send_receive_stats()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::stat::print;

  constexpr bool IS_NSS = MQ_TYPE == session::schema::MqType::NONE;

#if 1
  Logger* const logger = nullptr; // Normally avoid massive log spam, so we can see our test printouts.
#else
  Logger* const logger = g_logger_console; // But when debugging it can be quite helpful.
#endif
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  Logger::this_thread_set_logged_nickname("U", g_logger_console); // Since we call it U in comments....

  /* Create a session in-process and produce a channel pair.
   * For NSS (MqType::NONE): TRANSMIT_NATIVE_HANDLES = true; channel stores NSS in hndl_snd/rcv().
   * For MQ (BIPC/POSIX): TRANSMIT_NATIVE_HANDLES = false; channel stores MQ senders/receivers in blob_snd/rcv(). */
  auto sync_io_pair = struc::test::make_session_channel_pair<MQ_TYPE, IS_NSS>(logger);
  /* sync_io_pair.m_..._channels[0] can be thought of, conceptually, as essentially a set of OS-handles (FDs even)
   * to the appropriate IPC peers.  On these one can do synchronous ops like, conceptually speaking,
   * send(fd), receive(fd).  We however -- as noted in the header comment of the present function -- want
   * Flow-IPC to operate an entire event loop + thread for it.  So: X.async_io_obj() creates and returns
   * an object representing this entire system, and it feeds move(X) into the returned object. */
  /* Declared before the channels below on purpose: their worker-thread handlers post() onto this loop, so it
   * must outlive them.  (A post() onto a stopped loop belays harmlessly; onto a destroyed one = race on freed
   * innards.) */
  Single_thread_task_loop loop{logger, "U1"};
  loop.start();

  auto cli_channel = sync_io_pair.m_cli_channels.front().async_io_obj();
  auto srv_channel = sync_io_pair.m_srv_channels.front().async_io_obj();

  /* Get the sender and receiver.
   * VIA_CHANNEL = true: use the Channel objects directly (tests Channel forwarding).
   * VIA_CHANNEL = false: extract the underlying pipe objects (tests direct pipe-object API).
   *
   * This technique is kinda like a compile-time ternary (?:) that makes snd and rcv refer to the appropriately-typed
   * reference to the appropriate object. The `return (xyz)` parens cause lambda to return & instead of by-value (which
   * probably would move-construct a temp Channel or Native_socket_stream or...; probably fine too but no need). */
  auto& snd = [&]() -> decltype(auto)
  {
    if constexpr(VIA_CHANNEL) { return (cli_channel); }
    else if constexpr(IS_NSS) { return (*cli_channel.hndl_snd()); }
    else { return (*cli_channel.blob_snd()); }
  }();
  auto& rcv = [&]() -> decltype(auto)
  {
    if constexpr(VIA_CHANNEL) { return (srv_channel); }
    else if constexpr(IS_NSS) { return (*srv_channel.hndl_rcv()); }
    else { return (*srv_channel.blob_rcv()); }
  }();

  using Rcv_t = std::remove_reference_t<decltype(rcv)>;

  boost::promise<void> test_done;

  constexpr size_t N_PLAIN = 5;
  constexpr size_t PLAIN_SZ = 100;
  constexpr size_t N_HNDL = 3;
  constexpr size_t HNDL_META_SZ = 50;
  constexpr size_t N_POST_RESET = 2;
  constexpr size_t POST_RESET_SZ = 200;
  constexpr size_t N_BATCH_1 = 3;
  constexpr size_t BATCH_1_SZ = 80;
  constexpr size_t N_BATCH_MULTI = 5;
  constexpr size_t BATCH_MULTI_SLOTS = 4;
  constexpr size_t BATCH_MULTI_SZ = 120;

  const auto rcv_buf_sz = do_receive_max_size(rcv);

  // Everything below runs on U1.  U just waits for test_done at the bottom.
  loop.post([&]()
  {
    FLOW_LOG_INFO("Test body starting.");

    /* Start auto_ping on the sender.  This triggers the initial ping immediately, so the send-stats
     * m_auto_pings should be >= 1 by the time we look at them. */
    ASSERT_TRUE(snd.auto_ping());
    FLOW_LOG_INFO("auto_ping() started.");

    // --- Phase 1: send some plain blobs, check send stats ---
    FLOW_LOG_INFO("Phase 1: sending [" << N_PLAIN << "] plain blobs of [" << PLAIN_SZ << "] bytes.");
    { FLOW_TEST_TRACE(); send_blobs(snd, N_PLAIN, PLAIN_SZ); }

    {
      const auto stats = do_send_stats(snd);
      FLOW_LOG_INFO("Phase 1 send stats: " << print(stats));

      EXPECT_GE(stats.m_total_msgs, N_PLAIN);
      EXPECT_GE(stats.m_total_bytes, N_PLAIN * PLAIN_SZ);
      // Low-level bytes include framing overhead (NSS) or protocol messages (MQ); at least user bytes.
      EXPECT_GE(stats.m_total_low_lvl_bytes, stats.m_total_bytes);
      EXPECT_EQ(stats.m_msgs_with_hndls, 0u);
      // The by-size histogram saw each of those payloads.
      EXPECT_GE(stats.m_histo_payload_sz.count_for_bucket_containing_outcome(PLAIN_SZ), N_PLAIN);
      // auto_ping() was called; at least the initial ping should have been sent.
      EXPECT_GE(stats.m_auto_pings, 1u);
    }

    // Verify ADL core-stats extraction (identity for stat::Blob_snd_stats; meaningful for higher-layer types).
    {
      const auto stats_c = do_send_stats(snd);
      EXPECT_EQ(&blob_snd_stats(stats_c), &stats_c);
      auto stats_m = do_send_stats(snd);
      EXPECT_EQ(&blob_snd_stats_mutable(stats_m), &stats_m);
    }

    // --- Phase 2 (NSS only): send some handle-bearing messages ---
    if constexpr(IS_NSS)
    {
      FLOW_LOG_INFO("Phase 2: sending [" << N_HNDL << "] handle-bearing messages.");
      { FLOW_TEST_TRACE(); send_native_handle_msgs(snd, N_HNDL, HNDL_META_SZ); }

      {
        const auto stats = do_send_stats(snd);
        FLOW_LOG_INFO("Phase 2 send stats: " << print(stats));

        EXPECT_GE(stats.m_total_msgs, N_PLAIN + N_HNDL);
        EXPECT_GE(stats.m_msgs_with_hndls, N_HNDL);
      }
    }

    /* Drain initial messages, then continue with common post-drain code.
     * NSS: drain via async_receive_native_handle (handles both blob-only and handle-bearing msgs).
     * MQ: drain via async_receive_blob. */
    auto after_initial_drain = [&]()
    {
      FLOW_LOG_INFO("Initial drain complete.");

      {
        const auto stats = do_receive_stats(rcv);
        FLOW_LOG_INFO("Post-drain receive stats: " << print(stats));

        if constexpr(IS_NSS)
        {
          EXPECT_GE(stats.m_total_msgs, N_PLAIN + N_HNDL);
          EXPECT_GE(stats.m_total_bytes, (N_PLAIN * PLAIN_SZ) + (N_HNDL * HNDL_META_SZ));
          EXPECT_GE(stats.m_msgs_with_hndls, N_HNDL);
        }
        else
        {
          EXPECT_GE(stats.m_total_msgs, N_PLAIN);
          EXPECT_GE(stats.m_total_bytes, N_PLAIN * PLAIN_SZ);
          EXPECT_EQ(stats.m_msgs_with_hndls, 0u);
        }
        EXPECT_GE(stats.m_total_low_lvl_bytes, stats.m_total_bytes);
        // The by-size histogram saw each of those payloads.
        EXPECT_GE(stats.m_histo_payload_sz.count_for_bucket_containing_outcome(PLAIN_SZ), N_PLAIN);
        // The sender did auto_ping(); receiver should see at least 1 auto-ping.
        EXPECT_GE(stats.m_auto_pings, 1u);
      }

      // Verify ADL core-stats extraction for receive stats.
      {
        const auto stats_c = do_receive_stats(rcv);
        EXPECT_EQ(&blob_rcv_stats(stats_c), &stats_c);
        auto stats_m = do_receive_stats(rcv);
        EXPECT_EQ(&blob_rcv_stats_mutable(stats_m), &stats_m);
      }

      // --- Reset stats ---
      FLOW_LOG_INFO("Resetting stats.");
      do_send_stats_reset(snd);
      do_receive_stats_reset(rcv);

      {
        const auto snd_s = do_send_stats(snd);
        const auto rcv_s = do_receive_stats(rcv);
        FLOW_LOG_INFO("Post-reset send stats: " << print(snd_s));
        FLOW_LOG_INFO("Post-reset receive stats: " << print(rcv_s));

        EXPECT_EQ(snd_s.m_total_msgs, 0u);
        EXPECT_EQ(snd_s.m_total_bytes, 0u);
        EXPECT_EQ(rcv_s.m_total_msgs, 0u);
        EXPECT_EQ(rcv_s.m_total_bytes, 0u);
      }

      // --- Send more after reset; verify counters resume from 0 ---
      FLOW_LOG_INFO("Sending [" << N_POST_RESET << "] post-reset blobs.");
      { FLOW_TEST_TRACE(); send_blobs(snd, N_POST_RESET, POST_RESET_SZ); }

      FLOW_LOG_INFO("Draining [" << N_POST_RESET << "] post-reset messages.");
      {
        FLOW_TEST_TRACE();
        drain_receives(rcv, N_POST_RESET, rcv_buf_sz, loop, [&]()
        {
          FLOW_LOG_INFO("Post-reset drain complete.");

          {
            const auto snd_s = do_send_stats(snd);
            const auto rcv_s = do_receive_stats(rcv);
            FLOW_LOG_INFO("Post-reset send stats: " << print(snd_s));
            FLOW_LOG_INFO("Post-reset receive stats: " << print(rcv_s));

            EXPECT_GE(snd_s.m_total_msgs, N_POST_RESET);
            EXPECT_GE(snd_s.m_total_bytes, N_POST_RESET * POST_RESET_SZ);
            EXPECT_GE(rcv_s.m_total_msgs, N_POST_RESET);
            EXPECT_GE(rcv_s.m_total_bytes, N_POST_RESET * POST_RESET_SZ);
          }

          // --- Batch-receive phases ---
          FLOW_LOG_INFO("Resetting stats for batch testing.");
          do_send_stats_reset(snd);
          do_receive_stats_reset(rcv);

          FLOW_LOG_INFO("Sending [" << N_BATCH_1 << "] blobs of ["
                        << BATCH_1_SZ << "] bytes.");
          { FLOW_TEST_TRACE(); send_blobs(snd, N_BATCH_1, BATCH_1_SZ); }

          FLOW_LOG_INFO("Batch-draining [" << N_BATCH_1 << "] via 1-slot batch.");
          {
            FLOW_TEST_TRACE();
            batch_drain_receives(rcv, 1, rcv_buf_sz, N_BATCH_1, loop, [&]()
            {
              FLOW_LOG_INFO("Batch drain (1-slot) complete.");

              {
                const auto rcv_s = do_receive_stats(rcv);
                FLOW_LOG_INFO("Batch (1-slot) receive stats: " << print(rcv_s));

                EXPECT_GE(rcv_s.m_total_msgs, N_BATCH_1);
                EXPECT_GE(rcv_s.m_total_bytes, N_BATCH_1 * BATCH_1_SZ);
              }

              // Multi-slot batch.
              FLOW_LOG_INFO("Sending [" << N_BATCH_MULTI << "] blobs of ["
                            << BATCH_MULTI_SZ << "] bytes.");
              { FLOW_TEST_TRACE(); send_blobs(snd, N_BATCH_MULTI, BATCH_MULTI_SZ); }

              FLOW_LOG_INFO("Batch-draining [" << N_BATCH_MULTI << "] via ["
                            << BATCH_MULTI_SLOTS << "]-slot batch.");
              {
                FLOW_TEST_TRACE();
                batch_drain_receives(rcv, BATCH_MULTI_SLOTS, rcv_buf_sz, N_BATCH_MULTI,
                                     loop, [&]()
                {
                  FLOW_LOG_INFO("Batch drain (multi-slot) complete.");

                  {
                    const auto rcv_s = do_receive_stats(rcv);
                    FLOW_LOG_INFO("Batch (multi-slot) receive stats: " << print(rcv_s));

                    EXPECT_GE(rcv_s.m_total_msgs, N_BATCH_1 + N_BATCH_MULTI);
                    EXPECT_GE(rcv_s.m_total_bytes,
                              (N_BATCH_1 * BATCH_1_SZ) + (N_BATCH_MULTI * BATCH_MULTI_SZ));
                  }

                  // --- Graceful close via end_sending() ---
                  FLOW_LOG_INFO("end_sending().");
                  ASSERT_TRUE(snd.end_sending());

                  FLOW_LOG_INFO("Issuing final async_receive for graceful-close.");
                  auto close_state = std::make_shared<Drain_state>(0);
                  close_state->buf_resize(rcv_buf_sz);

                  const auto close_buf = close_state->m_buf.mutable_buffer();
                  auto close_handler = [&, close_state](const Error_code& err, size_t)
                  {
                    loop.post([&, err]()
                    {
                      FLOW_LOG_INFO("Graceful-close receive err=[" << err << "] "
                                    "[" << err.message() << "].");
                      EXPECT_TRUE(err) << "Expected an error code for graceful-close.";

                      const auto snd_s = do_send_stats(snd);
                      const auto rcv_s = do_receive_stats(rcv);
                      FLOW_LOG_INFO("FINAL send stats: " << print(snd_s));
                      FLOW_LOG_INFO("FINAL receive stats: " << print(rcv_s));

                      FLOW_LOG_INFO("Test complete; signaling U.");
                      test_done.set_value();
                    });
                  };

                  if constexpr(S_USE_NH_API<Rcv_t>)
                  {
                    rcv.async_receive_native_handle(&close_state->m_hndl, close_buf,
                                                    std::move(close_handler));
                  }
                  else
                  {
                    rcv.async_receive_blob(close_buf, std::move(close_handler));
                  }
                });
              } // Multi-slot batch drain + FLOW_TEST_TRACE scope.
            });
          } // 1-slot batch drain + FLOW_TEST_TRACE scope.
        });
      } // Post-reset drain + FLOW_TEST_TRACE scope.
    }; // after_initial_drain

    // Kick off initial drain.
    constexpr size_t N_INITIAL_DRAIN = IS_NSS ? (N_PLAIN + N_HNDL) : N_PLAIN;

    FLOW_LOG_INFO("Draining [" << N_INITIAL_DRAIN << "] messages.");
    {
      FLOW_TEST_TRACE();
      drain_receives(rcv, N_INITIAL_DRAIN, rcv_buf_sz, loop,
                     std::move(after_initial_drain));
    }

    FLOW_LOG_INFO("Initial post() returning; async drain chain in progress.");
  }); // loop.post() -- end of U1 work.

  // U waits here for the entire test to finish on U1.
  auto fut = test_done.get_future();
  ASSERT_EQ(fut.wait_for(boost::chrono::seconds{10}), boost::future_status::ready)
    << "Test timed out waiting for U1 to complete.";

  FLOW_LOG_INFO("Thread U1 completed; stopping loop.");
  loop.stop();
} // test_send_receive_stats()

} // namespace (anon)

#define STATS_COMBO_TEST(test_name) \
  TEST(Blob_transport_stats_test, test_name##_Nss_Pipe) \
    { test_##test_name<session::schema::MqType::NONE, false>(); }  \
  TEST(Blob_transport_stats_test, test_name##_Nss_Channel) \
    { test_##test_name<session::schema::MqType::NONE, true>(); }   \
  TEST(Blob_transport_stats_test, test_name##_Bipc_Pipe) \
    { test_##test_name<session::schema::MqType::BIPC, false>(); }  \
  TEST(Blob_transport_stats_test, test_name##_Bipc_Channel) \
    { test_##test_name<session::schema::MqType::BIPC, true>(); }   \
  TEST(Blob_transport_stats_test, test_name##_Posix_Pipe) \
    { test_##test_name<session::schema::MqType::POSIX, false>(); } \
  TEST(Blob_transport_stats_test, test_name##_Posix_Channel) \
    { test_##test_name<session::schema::MqType::POSIX, true>(); }

/* Field-coverage manifests.  For each Stat_set type that this suite is responsible for testing:
 * stats_field_names() reflects the type's full declared field list, and every field must appear in exactly
 * one of the two lists below -- covered (asserted somewhere in this suite) or skipped (deliberately
 * unasserted, with its why).  A newly-added struct field fails this test until someone classifies it here:
 * that is the point.  (The covered-claims themselves are maintained by review; this enforces completeness
 * of the classification, not the existence of the asserts.) */
TEST(Blob_transport_stats_test, field_coverage_manifests)
{
  using flow::util::stat::stats_field_names;
  using std::sort;
  using std::string;
  using std::vector;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto check_manifest = [](util::String_view stat_set_name, vector<string> declared,
                                 vector<string> covered, const vector<string>& skipped)
  {
    // The table itself (in declaration order), for the eyeball:
    const auto in = [](const vector<string>& vec, const string& name)
                      { return std::find(vec.begin(), vec.end(), name) != vec.end(); };
    std::cout << "Field-coverage manifest [" << stat_set_name << "] "
                 "(" << covered.size() << " covered, " << skipped.size() << " skipped):\n";
    for (const auto& name : declared)
    {
      std::cout << "  " << (in(skipped, name) ? "SKIPPED: "
                                              : in(covered, name) ? "covered: "
                                                                  : "*** UNCLASSIFIED: ")
                << name << '\n';
    }

    auto& classified = covered;
    classified.insert(classified.end(), skipped.begin(), skipped.end());
    sort(classified.begin(), classified.end());
    sort(declared.begin(), declared.end());
    EXPECT_EQ(declared, classified) << "Unclassified/stale stat-field classification for "
                                       "[" << stat_set_name << "].";
  };

  check_manifest
    ("Blob_snd_stats", stats_field_names<stat::Blob_snd_stats>(size_t(1)),
     { // Covered: asserted in this suite (across the transport-flavor combo instantiations).
       "total_msgs", "total_bytes", "total_low_lvl_bytes", "msgs_with_hndls", "auto_pings",
       "histo_payload_sz" },
     { /* Skipped deliberately: these require provoking a full low-level pipe (backpressure), which this
        * suite's modest scripted traffic never does.  @todo transport_test could throw in some stat-checks. */
       "would_block_count", "snd_q_depth", "snd_q_hi_wmark" });

  check_manifest
    ("Blob_rcv_stats", stats_field_names<stat::Blob_rcv_stats>(size_t(1)),
     { // Covered: asserted in this suite (across the transport-flavor combo instantiations).
       "total_msgs", "total_bytes", "total_low_lvl_bytes", "msgs_with_hndls", "auto_pings",
       "histo_payload_sz" },
     { // Skipped deliberately: requires a provoked idle-timeout (wall-clock waiting); not exercised here.  @todo.
       "idle_timeouts" });
} // TEST(Blob_transport_stats_test, field_coverage_manifests)

STATS_COMBO_TEST(send_receive_stats)

} // namespace ipc::transport::test
