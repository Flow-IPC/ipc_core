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
#include "ipc/transport/native_socket_stream_acceptor.hpp"
#include "ipc/transport/sync_io/native_socket_stream_acceptor.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <boost/move/make_unique.hpp>

namespace ipc::transport
{

// Initializers.

// It's a reference due to `static` things being initialized in unknown order before main().
const Shared_name& Native_socket_stream_acceptor::S_RESOURCE_TYPE_ID = Sync_io_obj::S_RESOURCE_TYPE_ID;

// Implementations.

Native_socket_stream_acceptor::Native_socket_stream_acceptor(flow::log::Logger* logger_ptr,
                                                             const Shared_name& absolute_name_arg,
                                                             Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_absolute_name(absolute_name_arg),
  m_worker(get_logger(), // Start the 1 thread.
           /* (Linux) OS thread name will truncate m_absolute_name to 15-5=10 chars here; high chance that'll include
            * something decently useful; probably not everything though.  It's a decent attempt. */
           flow::util::ostream_op_string("NSSA-", m_absolute_name.str())),
  m_next_peer_socket(*(m_worker.task_engine())) // Steady state: start it as empty, per doc header.
{
  using asio_local_stream_socket::Acceptor;
  using asio_local_stream_socket::Endpoint;
  using asio_local_stream_socket::endpoint_at_shared_name;
  using util::String_view;
  using flow::error::Runtime_error;
  using flow::async::reset_thread_pinning;
  using boost::system::system_error;

  /* For simplicity we'll just do all the work in thread W we're about to start.  Whether to do some initial stuff
   * in this thread or not doesn't matter much.  We have promised that,
   * upon our return -- no later -- the Native_socket_stream_acceptor
   * will be listening (assuming no errors).  So wait for that (the startup) to finish using start() arg. */
  Error_code sys_err_code;

  FLOW_LOG_TRACE("Acceptor [" << *this << "]: Awaiting initial setup/listening in worker thread.");
  m_worker.start([&]() // Execute all this synchronously in the thread.
  {
    reset_thread_pinning(get_logger()); // Don't inherit any strange core-affinity!  Worker must float free.

    auto const asio_engine = m_worker.task_engine();

    FLOW_LOG_INFO("Acceptor [" << *this << "]: Starting (am in worker thread).");

    const auto local_endpoint = endpoint_at_shared_name(get_logger(), m_absolute_name, &sys_err_code);
    assert((local_endpoint == Endpoint()) == bool(sys_err_code));
    if (sys_err_code) // It logged.
    {
      return; // Escape the start() callback, that is.
    }
    // else

    // Start a listening acceptor socket at that endpoint!

    try
    {
      // Throws on error.  (It's annoying there's no error-code-returning API; but it's normal in boost.asio ctors.)
      m_acceptor.reset(new Acceptor(*asio_engine, local_endpoint));
      // @todo Is reuse_addr appropriate?  Do we run into the already-exists error in practice?  Revisit.
    }
    catch (const system_error& exc)
    {
      assert(!m_acceptor);
      FLOW_LOG_WARNING("Acceptor [" << *this << "]: Unable to open/bind/listen native local stream socket; could "
                       "be due to address/name clash; details logged below.");
      sys_err_code = exc.code();
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      return; // Escape the start() callback, that is.
    }

    FLOW_LOG_INFO("Acceptor [" << *this << "]: "
                  "Successfully made endpoint and open/bind/listen-ed on it.  Ready for connections.");

    /* OK!  Background-wait for the first incoming connection.  As explained in m_next_peer_socket doc header
     * m_next_peer_socket is empty/unconnected at entry to each such background-wait; set to connected state
     * by boost.asio as it launches our callback; and moved/emptied again by that callback, as it starts the next
     * background-wait.  We have the just the one thread, so it's just a simple serial sequence. */

    m_acceptor->async_accept(m_next_peer_socket,
                             [this](const Error_code& async_err_code)
    {
      // We are in thread W.
      on_next_peer_socket_or_error(async_err_code);
    });

    assert(!sys_err_code); // Success.
  }); // m_worker.start()

  if (sys_err_code)
  {
    /* Just keep the thread going; even though it's not gonna be doing any listening.
     * @todo We could stop it here no problem.  And it could help something, somewhere, marginally to not have an
     * extra thread around.  But then this has to be coded for all over the place;
     * didn't seem essential, particularly since these error conditions are highly irregular and likely to be very
     * bad anyway. */

    if (err_code)
    {
      *err_code = sys_err_code;
      return;
    }
    // else
    throw Runtime_error(sys_err_code, FLOW_UTIL_WHERE_AM_I_STR());
  }
  // else
  assert(!sys_err_code);

  FLOW_LOG_INFO("Acceptor [" << *this << "]: Ready for incoming connections.");
} // Native_socket_stream_acceptor::Native_socket_stream_acceptor()

Native_socket_stream_acceptor::~Native_socket_stream_acceptor()
{
  using flow::async::Single_thread_task_loop;
  using flow::async::reset_thread_pinning;
  using flow::util::ostream_op_string;

  // We are in thread U.  By contract in doc header, they must not call us from a completion handler (thread W).

  FLOW_LOG_INFO("Acceptor [" << *this << "]: Shutting down.  Next acceptor socket will close; all our internal "
                "async handlers will be canceled; and worker thread thread will be joined.");

  // stop() logic is similar to what happens in Native_socket_stream::Impl dtor.  Keeping cmnts light.
  m_worker.stop();
  // Thread W is (synchronously!) no more.

  // Post-stop() poll() logic is similar to what happens in Native_socket_stream::Impl dtor.  Keeping cmnts light.

  FLOW_LOG_INFO("Acceptor [" << *this << "]: Continuing shutdown.  Next we will run pending handlers from some "
                "other thread.  In this user thread we will await those handlers' completion and then return.");
  Single_thread_task_loop one_thread(get_logger(), ostream_op_string("NSSADeinit-", m_absolute_name.str()));

  one_thread.start([&]()
  {
    reset_thread_pinning(get_logger()); // Don't inherit any strange core-affinity.  Float free.

    FLOW_LOG_INFO("Acceptor [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending internal handlers (typically none).");

    const auto task_engine = m_worker.task_engine();
    task_engine->restart();
    const auto count = task_engine->poll();
    if (count != 0)
    {
      FLOW_LOG_INFO("Acceptor [" << *this << "]: "
                    "In transient finisher thread: Ran [" << count << "] internal handlers after all.");
    }
    task_engine->stop();

    FLOW_LOG_INFO("Acceptor [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending user handlers (feeding operation-aborted).");

    while (!m_pending_user_requests_q.empty())
    {
      FLOW_LOG_TRACE("Running a queued async-accept completion handler.");
      m_pending_user_requests_q.front()
        ->m_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
      m_pending_user_requests_q.pop();
      FLOW_LOG_TRACE("User accept handler finished.  Popped from user request deficit queue.");
    } // while (!m_pending_user_requests_q.empty())

    FLOW_LOG_INFO("Transient finisher exiting.");
  }); // one_thread.start()
  // Here thread exits/joins synchronously.
} // Native_socket_stream_acceptor::~Native_socket_stream_acceptor()

void Native_socket_stream_acceptor::on_next_peer_socket_or_error(const Error_code& sys_err_code)
{
  using asio_local_stream_socket::Peer_socket;
  using flow::util::ostream_op_string;
  using std::holds_alternative;

  // We are in thread W.
  if (sys_err_code == boost::asio::error::operation_aborted)
  {
    return; // Stuff is shutting down.  GTFO.
  }
  // else
  assert(sys_err_code != boost::asio::error::would_block); // Not possible for async handlers.

  FLOW_LOG_TRACE("Acceptor [" << *this << "]: Incoming connection, or error when trying to accept one.");
  if (sys_err_code)
  {
    // Close/empty the potentially-almost-kinda-accepted socket.  Probably unnecessary but can't hurt.
    Error_code dummy;
    m_next_peer_socket.close(dummy);

    if (sys_err_code == boost::asio::error::connection_aborted)
    {
      FLOW_LOG_WARNING("Incoming connection aborted halfway during connection; this is quite weird but "
                       "should not be fatal.  Ignoring.  Still listening.");
      // Fall through.
    }
    else
    {
      FLOW_LOG_WARNING("Acceptor [" << *this << "]: The background accept failed fatally.  "
                       "Closing acceptor; no longer listening.  Details follow.");
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();

      // Close/destroy the acceptor.  And of course don't call async_accept() on it again as we normally would below.
      m_acceptor->close(dummy);

      // Queue up and handle result.

      /* Shouldn't have gotten here if any other error had been emitted.  We would've closed m_acceptor and certainly
       * not run m_acceptor->async_accept() again. */
      assert(m_pending_results_q.empty() || (!holds_alternative<Error_code>(m_pending_results_q.back())));

      // We are in steady state.  Make this one change...
      m_pending_results_q.push(sys_err_code);
      // ...and immediately handle it appropriately to get back into steady state.
      finalize_q_surplus_on_error();

      // Do *not* m_acceptor->async_accept() again.  (It's closed actually, so we couldn't anyway.)
      return;
    }
  } // if (sys_err_code)
  else // if (!sys_err_code)
  {
    /* We'll enqueue the resulting peer socket handle from the new socket.  There are a few subtleties:
     *   - We need to make a new Peer, which must be passed the boost.asio Peer_socket, or similar,
     *     to wrap.  On that guy, they will internally call things like Peer_socket::async_read_some().
     *   - m_next_peer_socket, basically, stores two pieces of data: the raw native socket handle (of new peer socket),
     *     and the Task_engine that is to be used to execute the logic of all async_ calls on it (e.g.,
     *     Peer_socket::async_read_some()) (basically 1-1 to our m_worker).
     *     - Can we pass m_next_peer_socket into the Native_socket_stream ctor then?  Well, no.  Why?  Answer:
     *       Suppose we did.  Now suppose Native_socket_stream calls async_read_some() on it.  Suppose some bytes are
     *       indeed read from the opposing peer.  Now some callback internal to the Native_socket_stream must be called.
     *       On what thread would it be called?  Answer: per above, on m_worker.  Do we want that?  Well, no, because
     *       that class sets up its own thread for that.  (Briefly: Actually, why not use one thread?  Answer: Even
     *       if we thought that was a good design for perf or something, consider that m_worker goes away in
     *       our dtor, so we'd have to actively share m_worker (maybe via shared_ptr) with the offshoot
     *       Native_socket_stream.  Simply, we just aren't designing it that way.  We want them to run an independent
     *       thread, though this is not where I will justify that.)
     *     - So what to do?  I was hoping there'd be a move-constructor-ish thing in Peer_socket,
     *       but there isn't one that lets one also specify the Task_engine; and it isn't apparently possible to change
     *       a Peer_socket's Task_engine (thing returned by .get_executor()) after construction.
     *       Worst-case, we could suck out the native socket handle (see just above) and then Native_socket_stream
     *       ctor could construct and take over that guy.  It should be entirely safe, since it is a new socket, and
     *       we haven't started any async ops on it yet, but it still feels dodgy.  Turns out the safe way to do it
     *       is basically that, but one can Peer_socket::release() to "officially" safely "eject" an open
     *       socket.  So we do that, and Peer_socket can take over the "ejected" native socket handle in
     *       its ctor.
     *   - We promise in our contract to propagate get_logger() to any child peer sockets.  This is where it happens. */

    /* .release() won't throw except in Windows <8.1, where it'll always throw (unsupported; per boost.asio docs).
     * We don't worry about Windows generally; and anyway in .hpp somewhere we already should've ensured Linux in
     * particular.  Just re-check that for sanity for now.  (This is a bit of future-proofing, so that the problem is
     * obvious if porting the code.) */
#ifndef FLOW_OS_LINUX
    static_assert(false, "Should not have gotten to this line; should have required Linux; "
                           "the next thing assumes not-Win-<8.1.");
#endif
    // Could store a raw handle too, but this is exactly as fast and adds some logging niceties.
    Native_handle native_peer_socket(m_next_peer_socket.release());
    assert(!m_next_peer_socket.is_open()); // Non-exhaustive sanity check that it's back in empty/unconnected state.
    FLOW_LOG_TRACE("Acceptor [" << *this << "]: "
                   "Ejected ownership of new incoming peer socket [" << native_peer_socket << "].");

    auto new_peer
      = boost::movelib::make_unique<Peer>
          (get_logger(),
           // Nickname is, like, "_pathName_of_this_acceptor=>native_hndl[35]" (as I write this).
           ostream_op_string(m_absolute_name.str(), "=>", native_peer_socket),
           std::move(native_peer_socket));
    // Caution: native_peer_socket is now invalid.
    assert(native_peer_socket.null());

    // Queue up and handle result.

    // As above -- on error we wouldn't have kept trying to accept more.
    assert(m_pending_results_q.empty() || (!holds_alternative<Error_code>(m_pending_results_q.back())));

    // We are in steady state.  Make this one change...
    m_pending_results_q.emplace(std::move(new_peer)); // (new_peer may now be hosed.)
    // ...and immediately handle it appropriately to get back into steady state.
    finalize_q_surplus_on_success();
  } // else if (!sys_err_code)

  // Either there was success (!sys_err_code), or a non-fatal error (otherwise).  Keep the async chain going.

  /* @todo Does it help perf-wise to spin through non-blocking accepts here (in case more incoming peers have been
   * queued up by OS) until would-block?  I (ygoldfel) have done it in the past when doing TCP/UDP reads, but I never
   * really checked whether it's beneficial, and anyway this situation is not really the same (incoming load
   * should be much less intense here). */

  FLOW_LOG_TRACE("Acceptor [" << *this << "]: Starting the next background accept.");
  m_acceptor->async_accept(m_next_peer_socket,
                           [this](const Error_code& async_err_code)
  {
    // We are in thread W.
    on_next_peer_socket_or_error(async_err_code);
  });
} // Native_socket_stream_acceptor::on_next_peer_socket_or_error()

void Native_socket_stream_acceptor::async_accept_impl(Peer* target_peer, On_peer_accepted_func&& on_done_func)
{
  using boost::movelib::make_unique;
  using std::get;
  using std::holds_alternative;

  // We are in thread U/W.  (They *are* allowed to invoke async_accept() from within their completion handler.)

  /* We don't lock our state, hence we do everything in thread W.
   *
   * If we are in thread U:  Post on thread W.
   *
   * If we are in thread W already (being invoked from earlier user completion handler):  Still post on thread W.
   * Otherwise we may well invoke handler synchronously (if surplus is available at the moment) which would
   * mean nested handler invocation, which we promised not to do (for good reason: if, say, their handler
   * is bracketed by a non-recursive lock, then they would get a deadlock trying to acquire the lock in
   * the 2nd -- inner -- handler execution). */
  m_worker.post([this, target_peer, on_done_func = std::move(on_done_func)]
                  () mutable // To allow for the on_done_func to be move()d again.
  {
    // We are in thread W.

    FLOW_LOG_TRACE("Acceptor [" << *this << "]: Handling async-accept request.");

    auto new_req = make_unique<User_request>();
    new_req->m_target_peer = target_peer;
    new_req->m_on_done_func = std::move(on_done_func);

    // We are in steady state.  Make this one change....
    m_pending_user_requests_q.emplace(std::move(new_req)); // (new_req may now be hosed.)
    // ...and immediately handle it appropriately to get back into steady state:

    if (m_pending_results_q.empty())
    {
      FLOW_LOG_TRACE("Acceptor [" << *this << "]: New async-accept request pushed onto deficit queue; "
                     "but there is no surplus (no pending results).  Will await results.");
      return;
    }
    // else if (!m_pending_results_q.empty())

    /* If deficit existed *before* the request was pushed, and there's surplus too, then it wasn't steady state
     * pre-push.  Violates our invariant (see data member doc headers). */
    assert(m_pending_user_requests_q.size() == 1);

    auto& peer_or_err_code = m_pending_results_q.front();
    if (holds_alternative<Error_code>(peer_or_err_code))
    {
      assert(m_pending_results_q.size() == 1); // An error always caps the queue (and never leaves it).
      FLOW_LOG_TRACE("Acceptor [" << *this << "]: New async-request pushed onto deficit queue; "
                     "and there is surplus in the form of a fatal error code.  Will feed error to the request "
                     "*without* popping it from surplus queue (size remains 1).");
      feed_error_result_to_deficit(get<Error_code>(peer_or_err_code));
    }
    else
    {
      assert(holds_alternative<Peer_ptr>(peer_or_err_code));

      FLOW_LOG_TRACE("Acceptor [" << *this << "]: New async-request pushed onto deficit queue; "
                     "and there is surplus in the form of a new peer handle.  Will feed handle to the request.  "
                     "Queue size will become [" << (m_pending_results_q.size() - 1) << "].");

      Peer_ptr peer(std::move(get<Peer_ptr>(peer_or_err_code)));
      m_pending_results_q.pop();
      feed_success_result_to_deficit(std::move(peer));
    }
  }); // m_worker.post()
} // Native_socket_stream_acceptor::async_accept_impl()

void Native_socket_stream_acceptor::finalize_q_surplus_on_error()
{
  using std::get;
  using std::holds_alternative;

  // We are in thread W.

  assert((!m_pending_results_q.empty()) && holds_alternative<Error_code>(m_pending_results_q.back()));

  if (m_pending_user_requests_q.empty())
  {
    FLOW_LOG_TRACE("Acceptor [" << *this << "]: Fatal error pushed onto surplus queue; "
                   "but there is no deficit (no pending requests).  Will await async-accept request(s).");
    return;
  }
  // else if (!m_pending_user_requests_q.empty())

  /* If surplus existed *before* the error was pushed, and there's deficit too, then it wasn't steady state pre-push.
   * Violates our pre-condition. */
  assert(m_pending_results_q.size() == 1);

  const auto err_code = get<Error_code>(m_pending_results_q.front());
  FLOW_LOG_TRACE("Acceptor [" << *this << "]: Fatal error pushed onto surplus queue; "
                 "and there is deficit (1+ pending requests).  Will feed error to all pending requests *without* "
                 "popping surplus queue, whose size remains 1.");
  feed_error_result_to_deficit(err_code);
} // Native_socket_stream_acceptor::finalize_q_surplus_on_error()

void Native_socket_stream_acceptor::finalize_q_surplus_on_success()
{
  using std::get;
  using std::holds_alternative;

  // We are in thread W.

  assert((!m_pending_results_q.empty()) && holds_alternative<Peer_ptr>(m_pending_results_q.back()));

  if (m_pending_user_requests_q.empty())
  {
    FLOW_LOG_TRACE("Acceptor [" << *this << "]: New peer socket handle pushed onto surplus queue; "
                   "but there is no deficit (no pending requests).  Will await async-accept request(s).");
    return;
  }
  // else if (!m_pending_user_requests_q.empty())

  /* If surplus existed *before* the handle was pushed, and there's deficit too, then it wasn't steady state pre-push.
   * Violates our pre-condition. */
  assert(m_pending_results_q.size() == 1);

  Peer_ptr peer(std::move(get<Peer_ptr>(m_pending_results_q.front())));
  m_pending_results_q.pop();
  FLOW_LOG_TRACE("Acceptor [" << *this << "]: New peer socket handle pushed onto surplus queue; "
                 "and there is deficit (1+ pending requests).  Will feed to next pending request, having "
                 "popped it from surplus queue (size is now 0).");
  feed_success_result_to_deficit(std::move(peer));
} // Native_socket_stream_acceptor::finalize_q_surplus_on_success()

void Native_socket_stream_acceptor::feed_error_result_to_deficit(const Error_code& err_code)
{
  assert(!m_pending_user_requests_q.empty());

  size_t idx = 0;
  do // while (!empty())
  {
    FLOW_LOG_TRACE("Acceptor [" << *this << "]: Feeding to user async-accept request handler [" << idx << "]: "
                   "Error code [" << err_code << "] [" << err_code.message() << "].");
    m_pending_user_requests_q.front()->m_on_done_func(err_code);
    m_pending_user_requests_q.pop();

    ++idx;
  }
  while (!m_pending_user_requests_q.empty());
} // Native_socket_stream_acceptor::feed_error_result_to_deficit()

void Native_socket_stream_acceptor::feed_success_result_to_deficit(Peer_ptr&& peer)
{
  assert(!m_pending_user_requests_q.empty());

  FLOW_LOG_TRACE("Acceptor [" << *this << "]: Feeding to user async-accept request handler: "
                 "Socket stream [" << *peer << "].  User request queue size post-pop is "
                 "[" << (m_pending_user_requests_q.size() - 1) << "].");
  auto& head_request = m_pending_user_requests_q.front();
  *head_request->m_target_peer = std::move(*peer);
  head_request->m_on_done_func(Error_code());
  m_pending_user_requests_q.pop();
} // Native_socket_stream_acceptor::feed_success_result_to_deficit()

const Shared_name& Native_socket_stream_acceptor::absolute_name() const
{
  return m_absolute_name;
}

std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val)
{
  return os << "sh_name[" << val.absolute_name() << "]@" << static_cast<const void*>(&val);
}

} // namespace ipc::transport
