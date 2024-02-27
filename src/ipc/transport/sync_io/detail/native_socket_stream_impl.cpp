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
#include "ipc/transport/sync_io/detail/native_socket_stream_impl.hpp"
#include "ipc/transport/error.hpp"
#include <boost/move/make_unique.hpp>
#include <cstddef>

namespace ipc::transport::sync_io
{

// Initializers.

const Native_socket_stream::Impl::low_lvl_payload_blob_length_t
  Native_socket_stream::Impl::S_META_BLOB_LENGTH_PING_SENTINEL
    = std::numeric_limits<low_lvl_payload_blob_length_t>::max();
const size_t Native_socket_stream::Impl::S_MAX_META_BLOB_LENGTH
  = S_META_BLOB_LENGTH_PING_SENTINEL - 1;

// Implementations (but main methods, ::rcv_*() and ::snd_*() + APIs are in diff .cpp file).

// Delegated ctor skips setting up m_peer_socket, not knowing whether we'll start in NULL or PEER state.
Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str, std::nullptr_t) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_nickname(nickname_str),
  m_state(State::S_NULL),
  m_protocol_negotiator(get_logger(), nickname(),
                        1, 1), // Initial protocol!  @todo Magic-number `const`(s), particularly if/when v2 exists.
  m_ev_wait_hndl_peer_socket(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still, at least.
  m_timer_worker(get_logger(), flow::util::ostream_op_string(*this)),

  m_snd_finished(false),
  m_snd_auto_ping_period(util::Fine_duration::zero()), // auto_ping() not yet called.
  m_snd_auto_ping_timer(m_timer_worker.create_timer()), // Inactive timer (auto_ping() not yet called).
  /* If it does become active, we'll use this readable-pipe-peer to get informed by m_timer_worker that
   * m_snd_auto_ping_timer has fired.  That way we can use m_snd_ev_wait_func() mechanism to have user
   * ferry timer firings to us. */
  m_snd_auto_ping_timer_fired_peer(m_timer_worker.create_timer_signal_pipe()),
  // And this is its watchee mirror for outside event loop (sync_io pattern).
  m_snd_ev_wait_hndl_auto_ping_timer_fired_peer
    (m_ev_hndl_task_engine_unused,
     Native_handle(m_snd_auto_ping_timer_fired_peer->native_handle())),

  m_rcv_idle_timeout(util::Fine_duration::zero()), // idle_timer_run() not yet called.
  m_rcv_idle_timer(m_timer_worker.create_timer()), // Inactive timer (idle_timer_run() not yet called).
  m_rcv_idle_timer_fired_peer(m_timer_worker.create_timer_signal_pipe()), // Analogous to above.
  m_rcv_ev_wait_hndl_idle_timer_fired_peer // Analogous to above.
    (m_ev_hndl_task_engine_unused,
     Native_handle(m_rcv_idle_timer_fired_peer->native_handle()))
{
  // m_*peer_socket essentially uninitialized for now; delegating ctor sets them up.
}

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  Impl(logger_ptr, nickname_str, nullptr)
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using std::string;

  FLOW_LOG_INFO("Socket stream [" << *this << "]: In NULL state: Started timer thread.  Otherwise inactive.");

  m_peer_socket
    = boost::movelib::make_unique<asio_local_stream_socket::Peer_socket>
        (m_nb_task_engine, // See its doc header if you're wondering about `_unused`.
         // Needs to be is_open() (hold an FD) -- see our doc header.  This arg makes it happen (omit => no FD).
         asio_local_stream_socket::local_ns::stream_protocol());
  // This "needs to be .assign()ed still."
  m_ev_wait_hndl_peer_socket.assign(m_peer_socket->native_handle());

  /* Last thing is, in NULL state, we need to prep the *connect() machinery.  As discussed in XXX class doc header,
   * the public API exposed is sync_connect(), whereas the following are private:
   *   -1- connect-ops counterpart of what replace_event_wait_handles() does for send-ops and receive-ops,
   *       without conflicting with user call to replace_event_wait_handles() which can happen anytime before
   *       start_send/receive_ops();
   *   -2- start_connect_ops();
   *   -3- async_connect().
   *
   * -1- and -2- can occur in any order really.  -3- of course happens last.
   *
   * We do -1- and -2- below.  -3- happens inside sync_connect() as needed.  */

  // Create the single-thread loop object, but do not .start() thread yet (see sync_connect() as to why not).
  m_conn_async_worker.emplace(get_logger(), string("conn-") + nickname());

  // -1- replace_event_wait_handles() counterpart.  To avoid conflict, use separate guy from m_ev_wait_hndl_peer_socket.
  m_conn_ev_wait_hndl_peer_socket.emplace(*(m_conn_async_worker->task_engine()),
                                          m_peer_socket->native_handle());

  /* -2- start_connect_ops(): We'll give it our .async_wait() machinery associated with m_conn_async_worker thread.
   * This is much like, e.g., Async_adapter_sender ctor does for send-ops. */
  start_connect_ops([this](Asio_waitable_native_handle* hndl_of_interest, bool ev_of_interest_snd_else_rcv,
                           Task_ptr&& on_active_ev_func)
  {
    FLOW_LOG_TRACE("Socket stream [" << *this << "]: Sync-IO connect-ops event-wait request: "
                   "descriptor [" << Native_handle(hndl_of_interest->native_handle()) << "], "
                   "writable-else-readable [" << ev_of_interest_snd_else_rcv << "].");

    // They want this async_wait().  Oblige.
    assert(hndl_of_interest);
    hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv
                                   ? Asio_waitable_native_handle::Base::wait_write
                                   : Asio_waitable_native_handle::Base::wait_read,
                                 [on_active_ev_func = std::move(on_active_ev_func)]
                                   (const Error_code& err_code)
    {
      // We are in m_conn_async_worker thread.  Nothing is locked.

      if (err_code == boost::asio::error::operation_aborted)
      {
        return; // Stuff is shutting down.  GTFO.
      }
      // else

      // They want to know about completed async_wait().  Oblige.

      /* Inform Impl *this of the event.  This can (indeed will for sure, in our case -- writable socket =>
       * connect completed) synchronously invoke handler we have registered via our this->async_connect() call
       * in sync_connect(). */

      (*on_active_ev_func)();
    }); // hndl_of_interest->async_wait()
  }); // start_connect_ops()
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 Native_handle&& native_peer_socket_moved) :
  Impl(logger_ptr, nickname_str, nullptr)
{
  using asio_local_stream_socket::Peer_socket;

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Immediately in PEER state: "
                "Taking over native peer socket [" << native_peer_socket_moved << "] which is connected "
                "and likely either just accepted or comes from local connect_pair().");

  m_state = State::S_PEER;

  /* Same deal as in NULL-state ctor above, except subsume the pre-connected Native_handle, and
   * m_ev_wait_hndl_peer_socket is already associated with m_ev_hndl_task_engine_unused, so
   * we just .assign() ("this needs to be .assign()ed still"). */

  m_peer_socket
    = boost::movelib::make_unique<asio_local_stream_socket::Peer_socket>
        (m_nb_task_engine,
         asio_local_stream_socket::local_ns::stream_protocol(),
         native_peer_socket_moved.m_native_handle);
  m_ev_wait_hndl_peer_socket.assign(native_peer_socket_moved.m_native_handle);

  // Clear it; we've eaten it.
  native_peer_socket_moved = Native_handle();
} // Native_socket_stream::Impl::Impl()

Native_socket_stream::Impl::~Impl()
{
  FLOW_LOG_INFO("Socket stream [" << *this << "]: Shutting down.  Next peer socket will close if open; "
                "and that is it.  They simply cannot advance our state machine via on_active_ev_func()s we "
                "handed out.");
}

bool Native_socket_stream::Impl::start_connect_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_ops<Op::S_CONN>(std::move(ev_wait_func));
} // Native_socket_stream::Impl::start_connect_ops()

bool Native_socket_stream::Impl::sync_connect(const Shared_name& absolute_name, Error_code* err_code)
{
  using util::sync_io::Asio_waitable_native_handle;
  using boost::promise;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Native_socket_stream::Impl::sync_connect,
                                     flow::util::bind_ns::cref(absolute_name), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (m_state != State::S_NULL)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to connect to [" << absolute_name << "] "
                     "but already not in NULL state.  Ignoring.");
    return false;
  }
  // else
  assert(op_started<Op::S_CONN>("sync_connect"));

  /* We don't m_conn_async_worker->start() in ctor, so we do it here -- and we will also .stop() it at the end.
   * Rationale: We'd like to keep ourselves as perf-light-weight as is reasonably possible; as in particular
   * the NULL-state ctor might be being invoked from transport::[sync_io::]Native_socket_stream default ctor,
   * as (and this is a common scenario) the target of Native_socket_stream_acceptor::async_accept(), which
   * will destroy *this Impl by move-assigning onto it via our pImpl setup.  Having a thread start/join occur
   * in an essentially blank-forever *this is unnecessarily heavy.  The trade-off is that sync_connect() is
   * framed by a thread stop/start, which is very confidently doable in under 1 millisecond total; and we
   * consider this a small price given the expected frequency of sync_connect()s in reality, plus the
   * operations that will run in this thread (not that those are heavy either).  Basically thread start/join
   * inside sync_connect() is unlikely to bother anyone, whereas a thread start/join in an object whose
   * purpose is to merely be overwritten via move-assign = obnoxious.
   *
   * Furthermore: All of the above holds, yes, but it's even nicer than that: async_connect(), per sync_io pattern,
   * can yield immediate, synchronous success or failure; then we do not even need any thread.  Not only that,
   * but if you look inside you'll see we say this is by far likelier than the converse.  (@todo Maybe we shouldn't
   * even create m_conn_async_worker at all, unless we know we need to start the thread?  It's probably OK as-is;
   * probably the thread start/join is the heaviest aspect of all this, and if we typically avoid that, we've done
   * fine.  Revisit sometime though.  It'd just make m_conn_ev_wait_hndl_peer_socket life-cycle more complicated.) */

  promise<void> done_promise;
  async_connect(absolute_name, err_code, [&](const Error_code& async_err_code)
  {
    // We are in m_conn_async_worker (short-lived) thread.

    /* async_connect() yielded SYNC_IO_WOULD_BLOCK after issuing m_conn_ev_wait_hndl_peer_socket->async_wait();
     * we detected that below and are now dutifully blocking (for a very short time) until the promise is fulfilled.
     * So fulfill it: */
    *err_code = async_err_code;
    done_promise.set_value();
  });

  if (*err_code == error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    /* m_conn_ev_wait_func() has been invoked, meaning there's an .async_wait() pending on m_conn_async_worker.
     * We have not yet started the actual thread though; this is 100% allowed by boost.asio and consequently
     * (and formally by its contract) Single_thread_task_loop.  Now we do need to start it though. */
    m_conn_async_worker->start();

    // And now we wait (for a very short time, at least in Linux; see class doc header discussion of sync_connect()).
    done_promise.get_future().wait();

    assert((*err_code != error::Code::S_SYNC_IO_WOULD_BLOCK)
           && "Handler to async_connect() should've set *err_code, and async_connect() must never issue would-block "
                "except synchronously.");

    m_conn_async_worker->stop(); // Stop/join thread until next sync_connect() attempt if any (PEER state? then never).
  }
  /* else if (*err_code == SYNC_IO_WOULD_BLOCK)
   * {
   *   Delightful (and very likely): It either sync-failed or sync-succeeded.  The handler won't be called.  Done:
   *   Fall-through.
   * } */

  if (*err_code)
  {
    assert((m_state == State::S_NULL)
           && "[A]synchronously-failed async_connect() should've gone back to NULL state by its contract.");
  }
  else
  {
    assert((m_state == State::S_PEER)
           && "[A]synchronously-successful async_connect() should've resulted in PEER state by its contract.");

    m_conn_ev_wait_func.clear(); // Might as well free some RAM (minor but why not).
  } // else if (!*err_code)

  return true;
} // Native_socket_stream::Impl::sync_connect()

void Native_socket_stream::Impl::async_connect(const Shared_name& absolute_name, Error_code* sync_err_code_ptr,
                                               flow::async::Task_asio_err&& on_done_func)
{
  namespace bind_ns = flow::util::bind_ns;
  using asio_local_stream_socket::Endpoint;
  using asio_local_stream_socket::Peer_socket;
  using asio_local_stream_socket::endpoint_at_shared_name;
  using flow::util::ostream_op_string;
  using flow::async::Task_asio_err;
  using util::Task;

  /* Maintenance note: If async_connect() becomes public at some point (alongside sync_connect() in some form),
   * it'll need to return bool and perform the m_state/NULL and op_started<CONN>() checks here, returning
   * false as appropriate, or continuing as appropriate.
   *
   * Maintenance note: sync_err_code_ptr can't be null now, but standard Flow-style error reporting semantics
   * will need to be implemented (throw exception on error, if it's indeed null). */

  assert(sync_err_code_ptr);
  auto& sync_err_code = *sync_err_code_ptr;

  m_state = State::S_CONNECTING;

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Will attempt connect to [" << absolute_name << "].");

  const auto remote_endpoint = endpoint_at_shared_name(get_logger(), absolute_name, &sync_err_code);
  assert((remote_endpoint == Endpoint()) == bool(sync_err_code)); // (By the way it WARNs on error.)

  if (!sync_err_code)
  {
    // Endpoint was fine; do the actual async connect attempt.

    /* Normally we'd do m_peer_socket->async_connect(F), but in the sync_io pattern we have to break it down into
     * some non-blocking operation(s) with an async-wait delegated to the user via m_conn_ev_wait_func().
     * (Internally that's what ->async_connect() does, but the wait is done by boost.asio.)
     * Namely that's:
     *   -# Set socket to non-blocking mode: ->non_blocking(true).
     *   -# Synchronous ->connect().
     *      -# This might succeed -- or fail -- immediately, as it's a local socket.  Done.  Otherwise:
     *   -# It will yield would-block indicating connection in-progress.
     *   -# Wait for writability of socket.
     *   -# Once that's ready, done.  (If it really failed, it'll be "writable" -- but as soon as they try using it,
     *      the true error shall be revealed.)
     *
     * We will do just that, for maximum resiliency in the face of who-knows-what.  In reality (as tested in Linux)
     * the situation is simultaneously simpler -- in practice -- and more complicated (the reasons for why it works
     * that way).  I explain... it's pretty messy.  This is with boost.asio from Boost 1.81.
     *
     * The trickiness is in ->connect().  If the opposing acceptor is listen()ing, and there is sufficient backlog
     * space, then it just immediately succeeds.  If it is not listen()ing, then it just immediately fails.
     * Now suppose it is listen()ing, but it ran out of backlog space.  Then: Whether ->non_blocking(true) or not
     * (but, like, yes, it's true for us), it internally does the following.
     *   -# ::connect().  If returns error.
     *   -# If that error is would-block (EWOULDBLOCK or EAGAIN or EINPROGRESS; but really-really EAGAIN is what happens
     *      in Linux with Unix domain sockets):
     *   -# ::poll(), awaiting (with infinite timeout) writability of socket.
     *      - Yes, it does this totally ignoring ->non_blocking().  That reads like a bug, and no comments explain
     *        it, but in practice it apparently is not a bug; at least it's not for Unix domain sockets in my
     *        (ygoldfel) testing.
     *   -# The ::poll() immediately returns 1 active FD, meaning it's writable.
     *      - I have no idea why, exactly.  In point of fact in blocking mode the ::connect() will actually sit there
     *        and wait for the backlog to clear.  It does make sense non-blocking ::connect() immediately yields
     *        EAGAIN.  But then ::poll() reports immediate writability?  That is rather strange.  Why wouldn't
     *        the connect() just return an error then?
     *   -# The code then tries to get SO_ERROR through a ::getsockopt().  This might be a TCP-stack thing more;
     *      in any case it yields no error in practice with Unix domain socket.  (I really traced through the code to
     *      confirm this; socket_ops.ipp is the file.)
     * So, ostensibly the ->connect() succeeds in this situation.  However, ->write_some() at that point just
     * hilariously yields ENOTCONN.  So all is well that ends... poorly?  Point is, that's what happens; at least
     * it doesn't just sit around blocking inside ->connect() despite non_blocking(true).  Is it okay behavior
     * given the situation (backlog full)?  Firsly backlog really shouldn't be full given a properly
     * operating opposing acceptor (by the way it default to like 4096).  But if it is, I'd say this behavior is
     * fine, and my standard is this: Even using a nice, civilized, normal m_peer_socket->async_connect(), the
     * resulting behavior is *exactly* the same: The ->async_connect() quickly "succeeds"; then
     * ->async_write() immediately ENOTCONNs.  If we do equally well as a properly operated ->async_connect(),
     * then what else can they ask for really?
     *
     * That said, again, out of a preponderance of caution and future-proofness (?) we don't rely on the above
     * always happening; and in fact have a clause for getting would_block from ->connect() in which case
     * we do the whole m_conn_ev_wait_func() thing.  For now it won't be exercised is all. */

    assert((!m_peer_socket->non_blocking()) && "New NULL-state socket should start as not non-blocking.");
    m_peer_socket->non_blocking(true, sync_err_code);
    if (!sync_err_code)
    {
      m_peer_socket->connect(remote_endpoint, sync_err_code);
      if (sync_err_code == boost::asio::error::would_block)
      {
        FLOW_LOG_INFO("Socket stream [" << *this << "]: boost::asio::connect() (non-blocking) got would-block; "
                      "while documented as possible this is actually rather surprising based on our testing "
                      "and understanding of internal boost.asio code.  Proceeding to wait for writability.");
        m_conn_ev_wait_func(&(m_conn_ev_wait_hndl_peer_socket.value()),
                            true, // Wait for write.
                            // Once writable do this.
                            boost::make_shared<Task>
                              ([this, on_done_func = std::move(on_done_func)]() mutable
        {
          conn_on_ev_peer_socket_writable(std::move(on_done_func));
        }));

        sync_err_code = error::Code::S_SYNC_IO_WOULD_BLOCK;
      }
      else if (sync_err_code)
      {
        FLOW_LOG_WARNING("Socket stream [" << *this << "]: boost::asio::connect() (non-blocking) completed "
                         "immediately but with error [" << sync_err_code << "] [" << sync_err_code.message() << "].  "
                         "Connect request failing.  Will emit error via sync-args.");
      }
      // else { Success.  Reminder: this and fatal error are both much likelier than would-block. }
    } // if (!err_code) [m_peer_socket->non_blocking(true)] (but it may have become truthy inside)
    else // if (err_code) [m_peer_socket->non_blocking(true)]
    {
      FLOW_LOG_WARNING("Socket stream [" << *this << "]: Trying to set non-blocking mode yielded error, "
                       "which is pretty crazy; details: [" << sync_err_code << "] "
                       "[" << sync_err_code.message() << "].  Connect request failing.  "
                       "Will emit error via sync-args.");
    }
  } // if (!sync_err_code) [endpoint construction] (but it may have become truthy inside)
  // else if (sync_err_code) [endpoint construction] { It logged. }

  // If got here, sync_err_code indicates immediate success or failure of async_connect().
  m_state = sync_err_code ? State::S_NULL : State::S_PEER;
} // Native_socket_stream::Impl::async_connect()

void Native_socket_stream::Impl::conn_on_ev_peer_socket_writable(flow::async::Task_asio_err&& on_done_func)
{
  assert((m_state == State::S_CONNECTING) && "Only we can get out of CONNECTING state in the first place.");

  /* The wait indicates it's writable... or "writable," meaning in error state.  While we could try some
   * trick like ::getsockopt(SO_ERROR), it's all academic anyway: in Linux at least, as noted in long
   * comment in async_connect(), ->connect() will either succeed or fail right away; and in the one
   * case where one can force a connectable-but-not-immediately situation -- hitting a backlog-full acceptor --
   * it'll still just succeed.  Anyway, if it weren't academic, then even if we falsely assume PEER state here,
   * when really m_peer_socket is hosed underneath, it'll just get exposed the moment we try to read or write.
   * So just relax. */

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Writable-wait upon would-blocked connect attempt: done.  "
                "Entering PEER state.  Will emit to completion handler.");
  m_state = State::S_PEER;
  on_done_func(Error_code());
  FLOW_LOG_TRACE("Handler completed.");
} // Native_socket_stream::Impl::conn_on_ev_peer_socket_writable()

bool Native_socket_stream::Impl::replace_event_wait_handles
       (const Function<util::sync_io::Asio_waitable_native_handle ()>& create_ev_wait_hndl_func)
{
  if ((!m_snd_ev_wait_func.empty()) || (!m_rcv_ev_wait_func.empty()))
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Replacing event-wait handles (probably to replace underlying "
                "execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl_peer_socket.is_open());
  assert(m_snd_ev_wait_hndl_auto_ping_timer_fired_peer.is_open());
  assert(m_rcv_ev_wait_hndl_idle_timer_fired_peer.is_open());

  Native_handle saved(m_ev_wait_hndl_peer_socket.release());
  m_ev_wait_hndl_peer_socket = create_ev_wait_hndl_func();
  m_ev_wait_hndl_peer_socket.assign(saved);

  saved.m_native_handle = m_snd_ev_wait_hndl_auto_ping_timer_fired_peer.release();
  m_snd_ev_wait_hndl_auto_ping_timer_fired_peer = create_ev_wait_hndl_func();
  m_snd_ev_wait_hndl_auto_ping_timer_fired_peer.assign(saved);

  saved.m_native_handle = m_rcv_ev_wait_hndl_idle_timer_fired_peer.release();
  m_rcv_ev_wait_hndl_idle_timer_fired_peer = create_ev_wait_hndl_func();
  m_rcv_ev_wait_hndl_idle_timer_fired_peer.assign(saved);

  return true;
} // Native_socket_stream::Impl::replace_event_wait_handles()

util::Process_credentials
  Native_socket_stream::Impl::remote_peer_process_credentials(Error_code* err_code) const
{
  using asio_local_stream_socket::Opt_peer_process_credentials;
  using util::Process_credentials;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Process_credentials, Native_socket_stream::Impl::remote_peer_process_credentials,
                                     _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!state_peer("remote_peer_process_credentials()"))
  {
    err_code->clear(); // As promised.
    return Process_credentials();
  }
  // else

  if (!m_peer_socket)
  {
    *err_code = error::Code::S_LOW_LVL_TRANSPORT_HOSED;
    return Process_credentials();
  }
  // else

  Opt_peer_process_credentials sock_opt; // Contains default-cted Process_credentials.
  m_peer_socket->get_option(sock_opt, *err_code);

  return sock_opt;
} // Native_socket_stream::Impl::remote_peer_process_credentials()

const std::string& Native_socket_stream::Impl::nickname() const
{
  return m_nickname;
}

bool Native_socket_stream::Impl::state_peer(util::String_view context) const
{
  if (m_state != State::S_PEER)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: In context [" << context << "] we must be in PEER state, "
                     "but we are not.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else
  return true;
}

std::ostream& operator<<(std::ostream& os, const Native_socket_stream::Impl& val)
{
  return os << "SIO[" << val.nickname() << "]@" << static_cast<const void*>(&val);
}

#if 0
void Native_socket_stream::Impl::reset_sync_io_setup()
{
  using util::sync_io::Asio_waitable_native_handle;

  /* Please see our doc header first.  That has important background.  Back here again?  Read on:
   *   - We're supposed to undo start_receive_*_ops(), if it has been called.
   *   - We're supposed to undo start_send_*_ops(), if it has been called.
   *     - However, do *not* undo its send of protocol-negotiation out-message.  Well, that's impossible anyway.
   *       More accurately: do *not* undo m_protocol_negotiator.local_max_proto_ver_for_sending() (i.e., do not
   *       .reset()).  The effect of leaving that alone => start_send_*_ops() will skip attempt to send it (again).
   *   - We're supposed to undo replace_event_wait_handles(), if it has been called.
   *   - We must leave *this in a coherent state (so one can use it, as-if it had just been cted in PEER state).
   *   - We can assume certain things that'll make the above (in aggregate) not hard.  Namely, none of these have
   *     been called: async_receive_*(), send_*(), *end_sending(), auto_ping(), idle_timer_run().
   *     That's very helpful, because it means no async-waits are currently in progress; which means undoing
   *     the sync-op-pattern-init methods (start_*_ops() and/or replace_event_wait_handles()) doesn't lead to
   *     an incoherent *this.
   *     - It'd be nice to assert() on that wherever practical.
   *     - HOWEVER!!!  A complicating factor is that, while no auto_ping() / send_*() / *end_sending() = helpful,
   *       nevertheless a send-op *will* have been invoked from start_send_*_ops().  As noted above, it would have
   *       sent the Protocol_negotiator out-message.  Thankfully, in actual fact, this can have caused one of
   *       exactly 2 state change sets, firstly m_protocol_negotiator.local_max_proto_ver_for_sending() now returns
   *       UNKNOWN plus secondly either
   *       - (success -- likely) no other state change; or
   *       - (failure -- unlikely) m_snd_pending_err_code is made truthy; m_peer_socket is nullified.
   *       Undoing start_*_ops() and replace_event_wait_handles() does *not* conflict with any of these eventualities.
   *       That is *this remains coherent.  To convince oneself of this, you only need to worry about the
   *       "(failure -- unlikely)" case, but in doing so you may have to go through other *this code to achieve it.
   *       Basically imagine *this after that scenario executes.
   *       - At first it's before any start_*_ops() or replace_event_wait_handles(), at which point essentially only
   *         those are callable.  Are they safe?  Yes:
   *         - start_*_ops(F): It'll just memorize move(F); and start_send_*_ops() will detect that Protocol_negotiator
   *           out-message was already sent and hence do nothing beyond memorizing move(F).
   *         - replace_event_wait_handles(): It does not access m_peer_socket or m_snd_pending_err_code at all.
   *       - Once they have been called: You can do other stuff, namely send_*(), async_receive_*(), and so on.
   *         Are they safe?  Yes, of course: It's just the vanilla an-error-has-hosed-*this-in-PEER-state situation. */

  // So first let's do our best to assert() the requirements.

  assert((m_state == State::S_PEER) && "Must be in PEER state by contract.");

  const bool hosed = !m_peer_socket;
  assert((hosed == bool(m_snd_pending_err_code))
         && "By contract we must not be in hosed state, unless due to internal initial-negotiation-send failing.");

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Releasing idle-state object to new socket-stream core object.  "
                 "To finish this: Undoing sync_io-pattern init steps to released core.  "
                 "Is it hosed due to protocol-negotiation out-message send having failed? = [" << hosed << "].");

  /* m_snd_pending_payloads_q being non-empty would mean there's an active async-wait; that'd be tough for us.
   * Slight subtlety: the Protocol_negotiator out-message send ostensibly could encounter would-block and hence
   * make this out-queue non-empty.  Except, no, it can't: there's no way the send buffer gets filled up by 1 small
   * message.  So if it's non-empty, they must have send_*()ed stuff against contract. */
  assert(m_snd_pending_payloads_q.empty()
         && "Did you send_*() against contract?  No way should initial-protocol-negotiation-send yield would-block.");

  assert((!m_snd_finished) && "Did you *end_sending() against contract?");
  assert(m_snd_pending_on_last_send_done_func_or_empty.empty() && "Did you *end_sending() against contract?");
  assert((m_snd_auto_ping_period == util::Fine_duration::zero()) && "Did you auto_ping() against contract?");
  assert((!m_rcv_user_request) && "Did you async_receive_*() against contract?");
  assert((!m_rcv_pending_err_code)
         && "Did you async_receive_*() or idle_timer_run() against contract?");
  assert((m_rcv_idle_timeout == util::Fine_duration::zero()) && "Did you idle_timer_run() against contract?");
  assert((m_protocol_negotiator.negotiated_proto_ver() == Protocol_negotiator::S_VER_UNKNOWN)
         && "Did you async_receive_*() or idle_timer_run() against contract?");

  // Time to undo stuff.

  /* start_*_ops() (excluding the m_protocol_negotiator out-message thing), in PEER state =
   *   - Memorize a user-supplied func into m_snd_ev_wait_func.
   *   - Ditto m_rcv_ev_wait_func.
   * Therefore just do this (possibly no-op): */
  m_snd_ev_wait_func.clear();
  m_rcv_ev_wait_func.clear();

  /* replace_event_wait_handles(), in PEER state =
   *   For the 3 watchable FDs in *this -- m_ev_wait_hndl_peer_socket, m_snd_ev_wait_hndl_auto_ping_timer_fired_peer,
   *   and m_rcv_ev_wait_hndl_idle_timer_fired_peer -- do this (for each guy S of those):
   *     - Starting point: S contains a particular raw FD, associated with Task_engine m_ev_hndl_task_engine_unused.
   *     - Do: Replace associated Task_engine m_ev_hndl_task_engine_unused with <user-supplied one via functor thing>.
   * So to undo it just do reverse it essentially (possibly no-op): */
  m_ev_wait_hndl_peer_socket
    = Asio_waitable_native_handle(m_ev_hndl_task_engine_unused,
                                  m_ev_wait_hndl_peer_socket.release());

  m_snd_ev_wait_hndl_auto_ping_timer_fired_peer
    = Asio_waitable_native_handle(m_ev_hndl_task_engine_unused,
                                  m_snd_ev_wait_hndl_auto_ping_timer_fired_peer.release());

  m_rcv_ev_wait_hndl_idle_timer_fired_peer
    = Asio_waitable_native_handle(m_ev_hndl_task_engine_unused,
                                  m_rcv_ev_wait_hndl_idle_timer_fired_peer.release());
} // Native_socket_stream::Impl::reset_sync_io_setup()
#endif

} // namespace ipc::transport::sync_io
