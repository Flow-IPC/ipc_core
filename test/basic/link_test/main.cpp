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

#include <ipc/transport/blob_stream_mq_snd.hpp>
#include <ipc/transport/blob_stream_mq_rcv.hpp>
#include <ipc/transport/bipc_mq_handle.hpp>
#include <flow/log/simple_ostream_logger.hpp>
#include <flow/log/async_file_logger.hpp>

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main(int argc, char const * const * argv)
{
  using ipc::util::Shared_name;
  using ipc::util::Blob_const;
  using ipc::util::Blob_mutable;

  using flow::util::Blob;
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::Error_code;
  using flow::Flow_log_component;

  using std::string;
  using std::exception;

  const string LOG_FILE = "ipc_core_link_test.log";
  const int BAD_EXIT = 1;
  const size_t SZ = 1000;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  Config std_log_config;
  std_log_config.init_component_to_union_idx_mapping<Flow_log_component>(1000, 999);
  std_log_config.init_component_names<Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "link_test-");

  Simple_ostream_logger std_logger(&std_log_config);
  FLOW_LOG_SET_CONTEXT(&std_logger, Flow_log_component::S_UNCAT);

  // This is separate: the IPC/Flow logging will go into this file.
  string log_file((argc >= 2) > string(argv[1]) : LOG_FILE);
  FLOW_LOG_INFO("Opening log file [" << log_file << "] for IPC/Flow logs only.");
  Config log_config = std_log_config;
  log_config.configure_default_verbosity(Sev::S_DATA, true); // High-verbosity.  Use S_INFO in production.
  /* First arg: could use &std_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(nullptr, &log_config, log_file, false /* No rotation; we're no serious business. */);

  try
  {
    /* Use the templates Blob_stream_mq_sender/receiver and some other peripheral things.
     * As a reminder we're not trying to demo the library here; just to access certain things -- probably
     * most users would do something higher-level and more impressive than this.  We're ensuring stuff built OK
     * more or less. */
    auto mq_name = Shared_name::ct("/cool_mq");
    mq_name.sanitize();

    using Mq_handle = ipc::transport::Bipc_mq_handle;
    Error_code dummy; // Do not throw if does not exist (pre-cleanup is just in case).
    Mq_handle::remove_persistent(&log_logger, mq_name, &dummy);
    Mq_handle mq_snd(&log_logger, mq_name, ipc::util::CREATE_ONLY, 10, SZ);
    Mq_handle mq_rcv(&log_logger, mq_name, ipc::util::OPEN_ONLY);

    ipc::transport::Blob_stream_mq_sender<Mq_handle>
      mq_snd_stream(&log_logger, "qsnd", std::move(mq_snd));
    ipc::transport::Blob_stream_mq_receiver<Mq_handle>
      mq_rcv_stream(&log_logger, "qrcv", std::move(mq_rcv));

    const string PAYLOAD = "Hello, world!";
    Blob target(&log_logger, SZ);

    mq_snd_stream.send_blob(Blob_const(static_cast<const void*>(&PAYLOAD[0]), PAYLOAD.size()));
    FLOW_LOG_INFO("Send message over MQ: [" << PAYLOAD << "].");

    mq_rcv_stream.async_receive_blob(Blob_mutable(static_cast<void*>(target.begin()), target.size()),
                                     [&](const Error_code& err_code, size_t n_rcvd)
    {
      if (err_code)
      {
        FLOW_LOG_WARNING("Problem receiving; unexpected!  Error: [" << err_code << "] [" << err_code.message() << "].");
        return;
      }
      // else

      const string str(reinterpret_cast<const char*>(target.begin()), n_rcvd);
      FLOW_LOG_INFO("Received message we sent: [" << str << "].");
    });

    // Don't judge us.  Again, we aren't demo-ing best practices here!
    FLOW_LOG_INFO("Sleeping for a few sec; then will exit (message should arrive very shortly from now).");
    flow::util::this_thread::sleep_for(boost::chrono::seconds(1));
    FLOW_LOG_INFO("Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
} // main()
