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

#pragma once

#include "ipc/util/util_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <flow/log/log.hpp>
#include <flow/common.hpp>
#include <gtest/gtest.h>
#include <boost/thread/future.hpp>
#include <boost/filesystem.hpp>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

/* Templated (on Persistent_mq_handle impl: Bipc_mq_handle, Posix_mq_handle) unit test of the concept API,
 * exercised directly -- as opposed to through Blob_stream_mq_sender/receiver, which transport_test [scripted]
 * and others exercise plenty but which (1) uses only a subset of the concept API and (2) for
 * S_HAS_NATIVE_HANDLE MQs (Posix_mq_handle) bypasses the {is|wait|timed_wait}_{sendable|receivable}() +
 * interrupt/allow facility entirely, awaiting the MQ's native handle instead.  Hence the present tests, with
 * particular attention to the wait/interrupt family.
 *
 * The instantiating TUs (one per MQ type) invoke the below `*_test<Mq>()` guys from vanilla TEST()s.
 *
 * Deliberately not covered (hard to arrange deterministically): the timed_send()/timed_receive() remaining-timeout
 * arithmetic across 2+ wait cycles (requires competing same-direction handles stealing wakeups at just the right
 * times); Bipc_mq_handle's cross-handle condition-variable interplay (an interrupt_*() waking an innocent other-handle
 * waiter, which must re-enter its wait).  Also native_handle() gets a mere sanity check: the various
 * Blob_stream_mq_*er testing exercises it in realistic fashion. */

namespace ipc::transport::test
{

// The Linux/POSIX-y bits are concentrated here, for the future porting guy.
#ifndef FLOW_OS_LINUX
static_assert(false, "The FD-counting leak-check below relies on Linux /proc semantics; "
                       "look into it (and any other gaps) when porting.");
#endif

/* Count of open FDs in this process (via /proc/self/fd).  Used to detect FD leaks: take a baseline, do stuff that
 * should net-zero-out, compare.  (The count includes the transient FD used to list the directory itself; that is
 * fine, as it is equally present in any 2 counts being compared.) */
inline size_t n_open_fds()
{
  namespace fs = boost::filesystem;
  size_t n = 0;
  for ([[maybe_unused]] const auto& dir_entry : fs::directory_iterator{"/proc/self/fd"})
  {
    ++n;
  }
  return n;
}

// Logger passed to the Mq objects themselves.  Flip to `&test_logger()` instead, if their detailed logs are desired.
inline flow::log::Logger* obj_logger()
{
  return nullptr;
}

// Console logger for the tests' own light progress-reporting (and optionally for obj_logger()).
inline flow::log::Logger* test_logger()
{
  static ipc::test::Test_logger s_logger;
  return &s_logger;
}

/* RAII-ish scoped name for a test MQ: pre-removes any leftover MQ at that name (just in case a prior run aborted);
 * removes again at scope exit, so nothing persistent is left behind by us.  (The pre-remove also incidentally
 * exercises remove_persistent()'s remove-nonexistent-is-an-error semantics, in the typical no-leftover case.) */
template<typename Mq>
class Scoped_mq_name
{
public:
  explicit Scoped_mq_name(util::String_view suffix) :
    m_name(util::Shared_name::ct(flow::util::ostream_op_string("ipcMqHndlTest_",
                                                               Mq::S_RESOURCE_TYPE_ID.str(), '_', suffix)))
  {
    remove();
  }

  ~Scoped_mq_name()
  {
    remove();
  }

  const util::Shared_name& name() const
  {
    return m_name;
  }

private:
  void remove()
  {
    Error_code sink;
    Mq::remove_persistent(obj_logger(), m_name, &sink); // Error (typically nonexistent) OK: best-effort.
  }

  const util::Shared_name m_name;
}; // class Scoped_mq_name

// Little helper: the pattern byte for index `idx`, for payload-integrity checks.
inline uint8_t pattern_byte(size_t idx)
{
  return uint8_t((idx * 7) & 0xFF);
}

/* Creation/opening/removal/listing/accessors; and the native_handle() sanity check.
 * Queue geometry note (applies below tests too): we request small values; impls may only increase them
 * (concept: "at least approximately"); so tests use the reported max_msg_size()/max_n_msgs() where it matters. */
template<typename Mq>
void persistent_mq_handle_lifecycle_test()
{
  using util::Shared_name;
  using util::Permissions_level;
  using util::shared_resource_permissions;

  FLOW_LOG_SET_CONTEXT(test_logger(), Log_component::S_TRANSPORT);
  const auto PERMS = shared_resource_permissions(Permissions_level::S_USER_ACCESS);

  Scoped_mq_name<Mq> scoped_name{"lifecycle"};
  const auto& name = scoped_name.name();
  Error_code err_code;

  FLOW_LOG_INFO("lifecycle: create-only; accessors.");
  std::optional<Mq> mq{std::in_place, obj_logger(), name, util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  EXPECT_EQ(mq->absolute_name(), name);
  EXPECT_GE(mq->max_msg_size(), 16u);
  EXPECT_GE(mq->max_n_msgs(), 4u);

  if constexpr(Mq::S_HAS_NATIVE_HANDLE)
  {
    // Sanity only: Blob_stream_mq_*er testing exercises the native handle in realistic fashion.
    EXPECT_FALSE(mq->native_handle().null());
  }

  FLOW_LOG_INFO("lifecycle: dupe create fails; open-only coexists; same underlying queue.");
  Mq mq_dupe{obj_logger(), name, util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  EXPECT_TRUE(bool(err_code)); // Already exists.

  Mq mq2{obj_logger(), name, util::OPEN_ONLY, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  // Prove the 2 handles refer to one queue: push through one; pop through the other.
  const uint8_t BYTE = 0x5A;
  EXPECT_TRUE(mq->try_send(util::Blob_const{&BYTE, 1}, &err_code));
  EXPECT_FALSE(err_code);
  std::vector<uint8_t> buf(mq2.max_msg_size()); // (Reminder: rcv buffer must be >= max_msg_size().)
  auto blob = util::Blob_mutable{buf.data(), buf.size()};
  EXPECT_TRUE(mq2.try_receive(&blob, &err_code));
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blob.size(), 1u);
  EXPECT_EQ(*static_cast<const uint8_t*>(blob.data()), BYTE);

  FLOW_LOG_INFO("lifecycle: handle-death does not kill queue; open-or-create opens-else-creates.");
  mq.reset(); // Close the original handle.
  Mq mq3{obj_logger(), name, util::OPEN_ONLY, &err_code}; // Still openable (mq2 also still open by the way).
  EXPECT_FALSE(err_code) << err_code.message();

  Mq mq4{obj_logger(), name, util::OPEN_OR_CREATE, 4, 16, PERMS, &err_code}; // Existing => opens.
  EXPECT_FALSE(err_code) << err_code.message();

  FLOW_LOG_INFO("lifecycle: for_each_persistent() lists us; remove_persistent() semantics.");
  const auto name_listed_func = [&]() -> bool
  {
    bool found = false;
    Mq::for_each_persistent([&](const util::Shared_name& a_name) { found = found || (a_name == name); });
    return found;
  };
  EXPECT_TRUE(name_listed_func());

  Mq::remove_persistent(obj_logger(), name, &err_code);
  EXPECT_FALSE(err_code) << err_code.message();
  EXPECT_FALSE(name_listed_func());
  // Handles to the removed queue keep working (kernel persistence): pushing via mq2 must still work.
  EXPECT_TRUE(mq2.try_send(util::Blob_const{&BYTE, 1}, &err_code));
  EXPECT_FALSE(err_code);

  Mq mq5{obj_logger(), name, util::OPEN_ONLY, &err_code}; // Name is gone though.
  EXPECT_TRUE(bool(err_code));
  Mq::remove_persistent(obj_logger(), name, &err_code); // Removing a non-existent name *is* an error, per concept.
  EXPECT_TRUE(bool(err_code));

  Mq mq6{obj_logger(), name, util::OPEN_OR_CREATE, 4, 16, PERMS, &err_code}; // Non-existing => creates.
  EXPECT_FALSE(err_code) << err_code.message();
  // (Scoped_mq_name dtor shall remove the name mq6's creation just re-added.)
} // persistent_mq_handle_lifecycle_test()

// Move ctor/assignment and swap(); including an FD-leak regression check around move-assignment-onto-open-handle.
template<typename Mq>
void persistent_mq_handle_move_and_swap_test()
{
  using util::Shared_name;
  using util::Permissions_level;
  using util::shared_resource_permissions;
  using std::swap;

  FLOW_LOG_SET_CONTEXT(test_logger(), Log_component::S_TRANSPORT);
  const auto PERMS = shared_resource_permissions(Permissions_level::S_USER_ACCESS);

  Scoped_mq_name<Mq> scoped_name_a{"moveA"};
  Scoped_mq_name<Mq> scoped_name_b{"moveB"};
  Error_code err_code;

  const auto probe_send_func = [&](Mq* mq)
  {
    const uint8_t BYTE = 0x77;
    EXPECT_TRUE(mq->try_send(util::Blob_const{&BYTE, 1}, &err_code));
    EXPECT_FALSE(err_code);
  };
  const auto probe_receive_func = [&](Mq* mq)
  {
    std::vector<uint8_t> buf(mq->max_msg_size());
    auto blob = util::Blob_mutable{buf.data(), buf.size()};
    EXPECT_TRUE(mq->try_receive(&blob, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_EQ(blob.size(), 1u);
    EXPECT_EQ(buf[0], 0x77);
  };

  FLOW_LOG_INFO("move_and_swap: move ctor; move assignment onto open handle; swap.");
  Mq mq_a{obj_logger(), scoped_name_a.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  probe_send_func(&mq_a);

  Mq mq_moved{std::move(mq_a)}; // Move ct.
  EXPECT_EQ(mq_moved.absolute_name(), scoped_name_a.name());
  probe_receive_func(&mq_moved); // The probe message came along.

  Mq mq_b{obj_logger(), scoped_name_b.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  probe_send_func(&mq_moved);
  mq_b = std::move(mq_moved); // Move assignment onto an *open* handle (regression: used to leak FDs in Posix_ impl).
  EXPECT_EQ(mq_b.absolute_name(), scoped_name_a.name());
  probe_receive_func(&mq_b);

  Mq mq_default;
  swap(mq_b, mq_default); // Now mq_default is the real one; mq_b is the null one.
  EXPECT_EQ(mq_default.absolute_name(), scoped_name_a.name());
  probe_send_func(&mq_default);
  probe_receive_func(&mq_default);
  swap(mq_b, mq_default); // Put it back (for max readability below).

  /* FD-leak regression proper: net FD count across create+move-assign+destroy cycles must be zero.
   * Trick: remove_persistent() right after creation makes the queue anonymous -- handles keep working -- so the
   * loop needs no per-iteration name cleanup, and aborting mid-loop cannot leak names either. */
  FLOW_LOG_INFO("move_and_swap: FD-leak regression loop.");
  Scoped_mq_name<Mq> scoped_name_c{"moveC"};
  Scoped_mq_name<Mq> scoped_name_d{"moveD"};
  const auto one_cycle_func = [&]()
  {
    Error_code sink;
    Mq mq_c{obj_logger(), scoped_name_c.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
    ASSERT_FALSE(err_code) << err_code.message();
    Mq::remove_persistent(obj_logger(), scoped_name_c.name(), &sink);
    Mq mq_d{obj_logger(), scoped_name_d.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
    ASSERT_FALSE(err_code) << err_code.message();
    Mq::remove_persistent(obj_logger(), scoped_name_d.name(), &sink);

    mq_d = std::move(mq_c); // The formerly-leaky op.
    probe_send_func(&mq_d);
  };
  one_cycle_func(); // Warm-up (in case anything lazily-initializes on first use).
  const auto n_fds_baseline = n_open_fds();
  for (size_t idx = 0; idx != 20; ++idx)
  {
    one_cycle_func();
  }
  EXPECT_EQ(n_open_fds(), n_fds_baseline);
} // persistent_mq_handle_move_and_swap_test()

// Non-blocking transmission: round-trips, empty messages, would-block both ways, size-error semantics.
template<typename Mq>
void persistent_mq_handle_transmission_nb_test()
{
  using util::Permissions_level;
  using util::shared_resource_permissions;

  FLOW_LOG_SET_CONTEXT(test_logger(), Log_component::S_TRANSPORT);
  const auto PERMS = shared_resource_permissions(Permissions_level::S_USER_ACCESS);

  Scoped_mq_name<Mq> scoped_name{"nb"};
  Error_code err_code;

  Mq mq{obj_logger(), scoped_name.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  const auto max_msg_sz = mq.max_msg_size();
  const auto max_n_msg = mq.max_n_msgs();
  std::vector<uint8_t> rcv_buf(max_msg_sz);

  FLOW_LOG_INFO("nb: empty-message round trip.");
  EXPECT_TRUE(mq.try_send(util::Blob_const{}, &err_code));
  EXPECT_FALSE(err_code);
  auto blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.try_receive(&blob, &err_code));
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blob.size(), 0u);

  FLOW_LOG_INFO("nb: payload-integrity round trip.");
  std::vector<uint8_t> snd_buf(max_msg_sz);
  for (size_t idx = 0; idx != snd_buf.size(); ++idx)
  {
    snd_buf[idx] = pattern_byte(idx);
  }
  EXPECT_TRUE(mq.try_send(util::Blob_const{snd_buf.data(), snd_buf.size()}, &err_code));
  EXPECT_FALSE(err_code);
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.try_receive(&blob, &err_code));
  EXPECT_FALSE(err_code);
  ASSERT_EQ(blob.size(), snd_buf.size());
  EXPECT_EQ(0, std::memcmp(blob.data(), snd_buf.data(), blob.size()));

  FLOW_LOG_INFO("nb: fill to capacity => would-block; drain => would-block.");
  for (size_t idx = 0; idx != max_n_msg; ++idx)
  {
    EXPECT_TRUE(mq.try_send(util::Blob_const{snd_buf.data(), 1}, &err_code));
    EXPECT_FALSE(err_code);
  }
  EXPECT_FALSE(mq.try_send(util::Blob_const{snd_buf.data(), 1}, &err_code)); // Full.
  EXPECT_FALSE(err_code); // Would-block is not an error.
  for (size_t idx = 0; idx != max_n_msg; ++idx)
  {
    blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
    EXPECT_TRUE(mq.try_receive(&blob, &err_code));
    EXPECT_FALSE(err_code);
  }
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_FALSE(mq.try_receive(&blob, &err_code)); // Empty.
  EXPECT_FALSE(err_code); // Would-block is not an error.

  FLOW_LOG_INFO("nb: size errors (non-fatal per concept).");
  std::vector<uint8_t> big_buf(max_msg_sz + 1);
  EXPECT_FALSE(mq.try_send(util::Blob_const{big_buf.data(), big_buf.size()}, &err_code));
  EXPECT_EQ(err_code, error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW);

  EXPECT_TRUE(mq.try_send(util::Blob_const{snd_buf.data(), 1}, &err_code)); // Still works (non-fatal).
  EXPECT_FALSE(err_code);
  // Receive-buffer under max_msg_size() => underflow error; message *not* consumed.
  blob = util::Blob_mutable{rcv_buf.data(), max_msg_sz - 1};
  EXPECT_FALSE(mq.try_receive(&blob, &err_code));
  EXPECT_EQ(err_code, error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW);
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.try_receive(&blob, &err_code)); // Still there; still works.
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blob.size(), 1u);
} // persistent_mq_handle_transmission_nb_test()

/* Blocking and timed transmission + poll/wait/timed-wait family: timeouts actually time out (and take nonzero time);
 * blocked ops get unblocked by a counterpart op scheduled on a worker loop (via a 2nd handle to the same queue --
 * per concept, same-`*this` concurrency is not allowed, but different-handles concurrency is). */
template<typename Mq>
void persistent_mq_handle_blocking_test()
{
  using util::Permissions_level;
  using util::shared_resource_permissions;
  using util::Fine_duration;
  using flow::Fine_clock;
  using flow::async::Single_thread_task_loop;
  using boost::chrono::milliseconds;

  FLOW_LOG_SET_CONTEXT(test_logger(), Log_component::S_TRANSPORT);
  const auto PERMS = shared_resource_permissions(Permissions_level::S_USER_ACCESS);
  constexpr auto SHORT_TIMEOUT = milliseconds{100};
  constexpr auto LONG_TIMEOUT = milliseconds{10 * 1000}; // Test fails via would-block... rather, hangs; so, generous.
  constexpr auto COUNTERPART_DELAY = milliseconds{50};

  Scoped_mq_name<Mq> scoped_name{"blocking"};
  Error_code err_code;

  Mq mq{obj_logger(), scoped_name.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  Mq mq_peer{obj_logger(), scoped_name.name(), util::OPEN_ONLY, &err_code}; // The loop thread shall use only this one.
  ASSERT_FALSE(err_code) << err_code.message();
  const auto max_n_msg = mq.max_n_msgs();
  std::vector<uint8_t> rcv_buf(mq.max_msg_size());
  const uint8_t BYTE = 0x33;

  Single_thread_task_loop loop{obj_logger(), "mqUtBlk"}; // (Scaffolding, not subject: quiet like the Mq objects.)
  loop.start();

  const auto fill_func = [&]() // Fill the queue to capacity (via nb-pushes).
  {
    for (size_t idx = 0; idx != max_n_msg; ++idx)
    {
      EXPECT_TRUE(mq.try_send(util::Blob_const{&BYTE, 1}, &err_code));
      EXPECT_FALSE(err_code);
    }
  };
  const auto drain_func = [&]() // Empty it.
  {
    auto blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
    while (mq.try_receive(&blob, &err_code))
    {
      EXPECT_FALSE(err_code);
      blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
    }
    EXPECT_FALSE(err_code);
  };

  FLOW_LOG_INFO("blocking: is_*() polls in various queue states.");
  EXPECT_TRUE(mq.is_sendable(&err_code)); // Empty queue: sendable...
  EXPECT_FALSE(err_code);
  EXPECT_FALSE(mq.is_receivable(&err_code)); // ...not receivable.
  EXPECT_FALSE(err_code);
  fill_func();
  EXPECT_FALSE(mq.is_sendable(&err_code)); // Full queue: not sendable...
  EXPECT_FALSE(err_code);
  EXPECT_TRUE(mq.is_receivable(&err_code)); // ...receivable.
  EXPECT_FALSE(err_code);
  drain_func();

  FLOW_LOG_INFO("blocking: timed ops time out in bounded fashion on starved queue.");
  const auto expect_took_awhile_func = [&](const Fine_clock::time_point& since)
  {
    /* Lower bound only, and a lenient one at that (concept explicitly disclaims timing precision; and, e.g.,
     * epoll_wait() has milliseconds granularity).  Upper bound would be flake-bait on a loaded CI machine; a hang
     * is caught by the overall test timeout regardless. */
    EXPECT_GE(Fine_clock::now() - since, Fine_duration{SHORT_TIMEOUT} / 2);
  };
  auto since = Fine_clock::now();
  auto blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_FALSE(mq.timed_receive(&blob, SHORT_TIMEOUT, &err_code)); // Empty.
  EXPECT_FALSE(err_code); // Timeout is not an error.
  expect_took_awhile_func(since);

  since = Fine_clock::now();
  EXPECT_FALSE(mq.timed_wait_receivable(SHORT_TIMEOUT, &err_code));
  EXPECT_FALSE(err_code);
  expect_took_awhile_func(since);

  fill_func();
  since = Fine_clock::now();
  EXPECT_FALSE(mq.timed_send(util::Blob_const{&BYTE, 1}, SHORT_TIMEOUT, &err_code)); // Full.
  EXPECT_FALSE(err_code);
  expect_took_awhile_func(since);

  since = Fine_clock::now();
  EXPECT_FALSE(mq.timed_wait_sendable(SHORT_TIMEOUT, &err_code));
  EXPECT_FALSE(err_code);
  expect_took_awhile_func(since);
  drain_func();

  FLOW_LOG_INFO("blocking: receive()/wait_receivable()/timed_receive() unblocked by delayed counterpart send.");
  const auto delayed_peer_send_func = [&]()
  {
    loop.schedule_from_now(Fine_duration{COUNTERPART_DELAY}, [&](bool) // Delay to ~ensure main thread blocks first.
    {
      Error_code peer_err_code;
      EXPECT_TRUE(mq_peer.try_send(util::Blob_const{&BYTE, 1}, &peer_err_code));
      EXPECT_FALSE(peer_err_code);
    });
  };
  delayed_peer_send_func();
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  mq.receive(&blob, &err_code);
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blob.size(), 1u);

  delayed_peer_send_func();
  mq.wait_receivable(&err_code);
  EXPECT_FALSE(err_code);
  drain_func();

  delayed_peer_send_func();
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.timed_receive(&blob, LONG_TIMEOUT, &err_code));
  EXPECT_FALSE(err_code);

  FLOW_LOG_INFO("blocking: send()/wait_sendable()/timed_send() unblocked by delayed counterpart receive.");
  const auto delayed_peer_receive_func = [&]()
  {
    loop.schedule_from_now(Fine_duration{COUNTERPART_DELAY}, [&](bool)
    {
      Error_code peer_err_code;
      std::vector<uint8_t> peer_buf(mq_peer.max_msg_size());
      auto peer_blob = util::Blob_mutable{peer_buf.data(), peer_buf.size()};
      EXPECT_TRUE(mq_peer.try_receive(&peer_blob, &peer_err_code));
      EXPECT_FALSE(peer_err_code);
    });
  };
  fill_func();
  delayed_peer_receive_func();
  mq.send(util::Blob_const{&BYTE, 1}, &err_code);
  EXPECT_FALSE(err_code);

  // Queue is full again at this point (was full; 1 popped; 1 pushed)... continue similarly:
  delayed_peer_receive_func();
  mq.wait_sendable(&err_code);
  EXPECT_FALSE(err_code);
  EXPECT_TRUE(mq.try_send(util::Blob_const{&BYTE, 1}, &err_code));
  EXPECT_FALSE(err_code);

  delayed_peer_receive_func();
  EXPECT_TRUE(mq.timed_send(util::Blob_const{&BYTE, 1}, LONG_TIMEOUT, &err_code));
  EXPECT_FALSE(err_code);

  loop.stop();
} // persistent_mq_handle_blocking_test()

/* The interrupt/allow facility.  Notably includes the interrupt-while-transmissible checks: deterministic,
 * threadless verification that an engaged interrupt mode preempts polls/waits *even when* the MQ is concurrently
 * transmissible in the watched direction.  (Regression: in the Posix_ impl, an epoll_wait() max-events bug
 * used to cause exactly that case to report transmissibility instead of S_INTERRUPTED.) */
template<typename Mq>
void persistent_mq_handle_interruption_test()
{
  using util::Permissions_level;
  using util::shared_resource_permissions;
  using util::Fine_duration;
  using flow::async::Single_thread_task_loop;
  namespace this_thread = flow::util::this_thread;
  using boost::chrono::milliseconds;
  using boost::promise;

  FLOW_LOG_SET_CONTEXT(test_logger(), Log_component::S_TRANSPORT);
  const auto PERMS = shared_resource_permissions(Permissions_level::S_USER_ACCESS);

  Scoped_mq_name<Mq> scoped_name{"interrupt"};
  Error_code err_code;

  Mq mq{obj_logger(), scoped_name.name(), util::CREATE_ONLY, 4, 16, PERMS, &err_code};
  ASSERT_FALSE(err_code) << err_code.message();
  std::vector<uint8_t> rcv_buf(mq.max_msg_size());
  const uint8_t BYTE = 0x44;

  FLOW_LOG_INFO("interruption: rcv-side, with queue non-empty (transmissible) the whole time.");
  EXPECT_TRUE(mq.try_send(util::Blob_const{&BYTE, 1}, &err_code)); // Make it *receivable*; and:
  EXPECT_FALSE(err_code);
  EXPECT_TRUE(mq.interrupt_receives());
  EXPECT_FALSE(mq.interrupt_receives()); // Dupe => false.
  // Interruption must win over transmissibility, in all 3 wait sorts:
  EXPECT_FALSE(mq.is_receivable(&err_code));
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  EXPECT_FALSE(mq.timed_wait_receivable(milliseconds{100}, &err_code));
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  mq.wait_receivable(&err_code);
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  /* The blocking-transmit ops (receive()/timed_receive() -- send()/timed_send() likewise) are interrupted only in
   * their blocking/waiting aspect: they try the nb-op first, and with a message available it simply succeeds --
   * interrupted mode or no.  (See interrupt_receives() concept doc.)  Demonstrate: */
  auto blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.timed_receive(&blob, milliseconds{100}, &err_code));
  EXPECT_FALSE(err_code);
  EXPECT_EQ(blob.size(), 1u);
  EXPECT_TRUE(mq.try_send(util::Blob_const{&BYTE, 1}, &err_code)); // Restore the 1-queued-message state for below.
  EXPECT_FALSE(err_code);

  EXPECT_TRUE(mq.allow_receives());
  EXPECT_FALSE(mq.allow_receives()); // Dupe => false.
  EXPECT_TRUE(mq.is_receivable(&err_code)); // Back in business (message still queued).
  EXPECT_FALSE(err_code);

  FLOW_LOG_INFO("interruption: snd-side, with queue non-full (transmissible -- the normal state) the whole time.");
  EXPECT_TRUE(mq.interrupt_sends());
  EXPECT_FALSE(mq.interrupt_sends());
  EXPECT_FALSE(mq.is_sendable(&err_code));
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  EXPECT_FALSE(mq.timed_wait_sendable(milliseconds{100}, &err_code));
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  mq.wait_sendable(&err_code);
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  EXPECT_TRUE(mq.allow_sends());
  EXPECT_FALSE(mq.allow_sends());
  EXPECT_TRUE(mq.is_sendable(&err_code));
  EXPECT_FALSE(err_code);

  FLOW_LOG_INFO("interruption: preemptive, on starved queue (detector-only path).");
  // Drain the 1 queued message, so the queue is empty.
  blob = util::Blob_mutable{rcv_buf.data(), rcv_buf.size()};
  EXPECT_TRUE(mq.try_receive(&blob, &err_code));
  EXPECT_FALSE(err_code);
  EXPECT_TRUE(mq.interrupt_receives());
  EXPECT_FALSE(mq.is_receivable(&err_code)); // Empty *and* interrupted; must report the latter.
  EXPECT_EQ(err_code, error::Code::S_INTERRUPTED);
  EXPECT_TRUE(mq.allow_receives());

  FLOW_LOG_INFO("interruption: concurrent -- interrupt breaks an ongoing wait (both directions).");
  Single_thread_task_loop loop{obj_logger(), "mqUtIntr"}; // (Scaffolding, not subject: quiet like the Mq objects.)
  loop.start();
  {
    promise<Error_code> done_promise;
    loop.post([&]()
    {
      Error_code wait_err_code;
      mq.wait_receivable(&wait_err_code); // Queue is empty: this shall block until interrupted.
      done_promise.set_value(wait_err_code);
    });
    this_thread::sleep_for(milliseconds{100}); // ~Ensure the wait is entered (correct either way).
    EXPECT_TRUE(mq.interrupt_receives()); // (interrupt_*() is concurrency-safe against the ongoing wait, per concept.)
    EXPECT_EQ(done_promise.get_future().get(), error::Code::S_INTERRUPTED);
    EXPECT_TRUE(mq.allow_receives());
  }
  {
    // Same for snd side; must first fill the queue so the wait actually blocks.
    for (size_t idx = 0; idx != mq.max_n_msgs(); ++idx)
    {
      EXPECT_TRUE(mq.try_send(util::Blob_const{&BYTE, 1}, &err_code));
      EXPECT_FALSE(err_code);
    }
    promise<Error_code> done_promise;
    loop.post([&]()
    {
      Error_code wait_err_code;
      mq.wait_sendable(&wait_err_code);
      done_promise.set_value(wait_err_code);
    });
    this_thread::sleep_for(milliseconds{100});
    EXPECT_TRUE(mq.interrupt_sends());
    EXPECT_EQ(done_promise.get_future().get(), error::Code::S_INTERRUPTED);
    EXPECT_TRUE(mq.allow_sends());
  }
  loop.stop();
} // persistent_mq_handle_interruption_test()

} // namespace ipc::transport::test
