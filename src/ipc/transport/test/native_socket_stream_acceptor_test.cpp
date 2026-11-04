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

#include "ipc/transport/sync_io/native_socket_stream_acceptor.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <gtest/gtest.h>
#include <future>
#include <utility>

/* This exercises sync_io::Native_socket_stream_acceptor -- notably the only sync_io-pattern API otherwise
 * (as of this writing) not instantiated by any internal code, test, or demo; they all use the async-I/O
 * Native_socket_stream_acceptor which is easier (and which the sync_io guy wraps, in a reversal of the
 * usual X-wraps-sync_io::X relationship).  We satisfy its async-waits with a boost.asio-based
 * flow.async loop, the canonical sync_io-pattern integration (see util::sync_io docs). */

namespace ipc::transport::test
{

namespace
{

using Acceptor = sync_io::Native_socket_stream_acceptor;
using Peer = Acceptor::Peer; // A/k/a sync_io::Native_socket_stream.
using util::sync_io::Asio_waitable_native_handle;
using util::sync_io::Task_ptr;
using flow::async::Single_thread_task_loop;

// Generate an acceptor Shared_name unique to this process + call site (abstract namespace => no cleanup worries).
Shared_name unique_name(util::String_view suffix)
{
  return Shared_name::ct(flow::util::ostream_op_string("/nssaUnitTest/", util::Process_credentials::own_process_id(),
                                                       '/', suffix));
}

// The canonical sync_io-pattern hookup: satisfy `acc`s async-wait requests via the given boost.asio-based loop.
void start_ops(Acceptor* acc, Single_thread_task_loop* loop)
{
  EXPECT_TRUE(acc->replace_event_wait_handles([loop]()
                                                { return Asio_waitable_native_handle{*(loop->task_engine())}; }));
  EXPECT_TRUE(acc->start_accept_ops([](Asio_waitable_native_handle* hndl_of_interest,
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
  }));
}

} // namespace (anon)

// Ctor/accessor; the various please-use-the-API-properly guards; dtor-fires-no-handlers contract.
TEST(Sync_io_native_socket_stream_acceptor_test, lifecycle_and_guards)
{
  ipc::test::Test_logger logger;
  Single_thread_task_loop loop{&logger, "nssaUtG"};
  loop.start();

  bool handler_ran = false;
  {
    const auto srv_name = unique_name("guards");
    Acceptor acc{&logger, srv_name}; // (Would throw on failure; should not.)
    EXPECT_EQ(acc.absolute_name(), srv_name);

    Peer target{&logger, "nssaUtG_target"};

    // Before start_accept_ops(): promised no-op/false.
    EXPECT_FALSE(acc.async_accept(&target, [&](const Error_code&) { handler_ran = true; }));

    start_ops(&acc, &loop);

    // Dupe-start and post-start-replace: refused.
    EXPECT_FALSE(acc.start_accept_ops([](Asio_waitable_native_handle*, bool, Task_ptr&&) {}));
    EXPECT_FALSE(acc.replace_event_wait_handles([&loop]()
                                                  { return Asio_waitable_native_handle{*(loop.task_engine())}; }));

    // One async_accept() is fine; a concurrent second one is refused.
    EXPECT_TRUE(acc.async_accept(&target, [&](const Error_code&) { handler_ran = true; }));
    EXPECT_FALSE(acc.async_accept(&target, [&](const Error_code&) { handler_ran = true; }));

    loop.stop(); // (Just so the no-handlers-fire check below is fully deterministic.)
  } // acc dtor runs here, with the accept outstanding: by contract it fires no completion handlers.
  EXPECT_FALSE(handler_ran);
}

// Mainstream flow: async_accept() awaits; client connects; handler fires with success; repeat for 2nd client.
TEST(Sync_io_native_socket_stream_acceptor_test, accept_then_connect)
{
  ipc::test::Test_logger logger;
  Single_thread_task_loop loop{&logger, "nssaUtA"};
  loop.start();

  const auto srv_name = unique_name("accThenConn");
  Acceptor acc{&logger, srv_name};
  start_ops(&acc, &loop);

  Peer target;
  std::promise<Error_code> accepted_promise;
  ASSERT_TRUE(acc.async_accept(&target, [&](const Error_code& err_code) { accepted_promise.set_value(err_code); }));

  Peer cli{&logger, "nssaUtA_cli"};
  Error_code sync_err_code;
  EXPECT_TRUE(cli.sync_connect(srv_name, &sync_err_code));
  EXPECT_FALSE(sync_err_code) << "sync_connect: [" << sync_err_code.message() << "].";

  const auto err_code = accepted_promise.get_future().get();
  EXPECT_FALSE(err_code) << "async_accept: [" << err_code.message() << "].";

  /* target should now be a PEER-state core.  E.g., its opposing-peer-creds should have been saved at accept time;
   * and the opposing (client) process is us. */
  Error_code creds_err_code;
  const auto creds = target.remote_peer_process_credentials(&creds_err_code);
  EXPECT_FALSE(creds_err_code);
  EXPECT_EQ(creds.process_id(), util::Process_credentials::own_process_id());

  // The accept chain should have re-armed: a 2nd client/accept round works identically.
  Peer target2;
  std::promise<Error_code> accepted_promise2;
  ASSERT_TRUE(acc.async_accept(&target2,
                               [&](const Error_code& err_code2) { accepted_promise2.set_value(err_code2); }));
  Peer cli2{&logger, "nssaUtA_cli2"};
  EXPECT_TRUE(cli2.sync_connect(srv_name, &sync_err_code));
  EXPECT_FALSE(sync_err_code);
  EXPECT_FALSE(accepted_promise2.get_future().get());

  loop.stop();
}

/* Reverse order: client connects first (the async-I/O core inside shall bank the resulting peer as surplus,
 * on its own schedule); async_accept() then completes -- still via the mandatory async-wait, per contract
 * (there is no synchronous-completion mode). */
TEST(Sync_io_native_socket_stream_acceptor_test, connect_then_accept)
{
  ipc::test::Test_logger logger;
  Single_thread_task_loop loop{&logger, "nssaUtC"};
  loop.start();

  const auto srv_name = unique_name("connThenAcc");
  Acceptor acc{&logger, srv_name};
  start_ops(&acc, &loop);

  Peer cli{&logger, "nssaUtC_cli"};
  Error_code sync_err_code;
  EXPECT_TRUE(cli.sync_connect(srv_name, &sync_err_code));
  EXPECT_FALSE(sync_err_code) << "sync_connect: [" << sync_err_code.message() << "].";

  Peer target;
  std::promise<Error_code> accepted_promise;
  ASSERT_TRUE(acc.async_accept(&target, [&](const Error_code& err_code) { accepted_promise.set_value(err_code); }));

  const auto err_code = accepted_promise.get_future().get();
  EXPECT_FALSE(err_code) << "async_accept: [" << err_code.message() << "].";

  Error_code creds_err_code;
  EXPECT_EQ(target.remote_peer_process_credentials(&creds_err_code).process_id(),
            util::Process_credentials::own_process_id());
  EXPECT_FALSE(creds_err_code);

  loop.stop();
}

} // namespace ipc::transport::test
