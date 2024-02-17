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
  m_ev_wait_hndl_peer_socket(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
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
  // Keep in sync with release()!

  // m_*peer_socket essentially uninitialized for now; delegating ctor sets them up.
}

Native_socket_stream::Impl::Impl(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  Impl(logger_ptr, nickname_str, nullptr)
{
  // Keep in sync with release()!

  FLOW_LOG_INFO("Socket stream [" << *this << "]: In NULL state: Started timer thread.  Otherwise inactive.");

  m_peer_socket
    = boost::movelib::make_unique<asio_local_stream_socket::Peer_socket>
        (m_nb_task_engine, // See its doc header if you're wondering about `_unused`.
         // Needs to be is_open() (hold an FD) -- see our doc header.  This arg makes it happen (omit => no FD).
         asio_local_stream_socket::local_ns::stream_protocol());
  // Lastly load up the same FD into the watchee mirror.  See also replace_event_wait_handles().
  m_ev_wait_hndl_peer_socket.assign(Native_handle(m_peer_socket->native_handle()));
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

  // Same deal as above ctor, except subsume the pre-connected Native_handle instead of making a new one.

  m_peer_socket
    = boost::movelib::make_unique<asio_local_stream_socket::Peer_socket>
        (m_nb_task_engine,
         asio_local_stream_socket::local_ns::stream_protocol(),
         native_peer_socket_moved.m_native_handle);
  m_ev_wait_hndl_peer_socket.assign(native_peer_socket_moved.m_native_handle);

  // Clear it; we've eaten it.
  native_peer_socket_moved = Native_handle();
} // Native_socket_stream::Impl::Impl()

Native_socket_stream Native_socket_stream::Impl::release()
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::String_view;
  using asio_local_stream_socket::Peer_socket;
  using flow::log::Log_context;

  // Keep in sync with NULL-state ctor!

  Error_code sys_err_code;
  const auto abort_on_fail = [&](String_view context) // Little helper.
  {
    if (sys_err_code)
    {
      FLOW_LOG_FATAL("Socket stream [" << *this << "]: "
                     "Socket op [" << context << "] failed; this should never happen.  Details follow.  Look into it.");
      FLOW_ERROR_SYS_ERROR_LOG_FATAL();
      assert(false && "Socket op failed; this should never happen.  Look into it.");
      std::abort();
    }
  };

  /* Our mission is to (1) create a fresh guy like *this managing the same Native_handle; and (2) make us
   * as-if we were just cted in NULL state (with null Logger and empty nickname).
   *
   * (1) is the easy (and important) part: we just need to invoke our ctor feeding it the raw Native_handle.
   * (2) is the hard/brittle (and not super-important, but hey, we did promise to make it work a certain way) part:
   * we need to act like the NULL-state ctor path on existing *this.
   *
   * It does help that by contract we have to be in nothing-has-happened-since-entering-PEER-state state.  So
   * our expected state is pretty rigid; so it is reasonably easy to reason about it.
   *
   * Maybe it's self-evident but to be clear -- things like m_nb_task_engine, m_ev_hndl_task_engine_unused,
   * m_snd_auto_ping_timer, m_rcv_idle_timer can simply be left alone; nothing wrong with them.
   * The timers haven't been used (by contract, no auto_ping() or idle_timer_run()); the `Task_engine`s
   * are dissociated/re-associated with their asio objects' FDs in civilized fashion via .release() + reassignment. */

  FLOW_LOG_TRACE("Socket stream [" << *this << "]: Releasing idle-state object to new socket-stream core object.");

  assert((m_state == State::S_PEER) && "By contract must be in PEER state."); // This is just important.
  assert(m_peer_socket && "By contract must be not be in hosed state."); // Ditto.
  // These following ones mean we won't have to reset them to be as-if-default-cted.
  assert(m_snd_pending_payloads_q.empty() && "By contract must be in idle state.");
  assert((!m_snd_pending_err_code) && "By contract must be in idle state.");
  assert((!m_snd_finished) && "By contract must be in idle state.");
  assert(m_snd_pending_on_last_send_done_func_or_empty.empty() && "By contract must be in idle state.");
  assert(m_snd_auto_ping_period == util::Fine_duration::zero());
  assert((!m_rcv_user_request) && "By contract must be in idle state.");
  assert((!m_rcv_pending_err_code) && "By contract must not be in a hosed state.");
  assert(m_rcv_idle_timeout == util::Fine_duration::zero());

  // As-if-default-cted means these will all be empty.
  m_conn_ev_wait_func.clear();
  m_snd_ev_wait_func.clear();
  m_rcv_ev_wait_func.clear();

  // And this guy.
  m_state = State::S_NULL;

  // We'll need this key FD; get it out of m_peer_socket in civilized fashion; it becomes FD-less (!.is_open()).
  auto peer_socket_raw_hndl = m_peer_socket->release(sys_err_code);
  abort_on_fail("release");

  /* Now we're ready to set up m_*peer_socket to as-if-default-cted state.
   * Follow what ctor does.  Note that, indeed, we generate a new FD. */
  assert((!m_peer_socket->is_open()) && "We ->release()d it above.");
  m_peer_socket->open(asio_local_stream_socket::local_ns::stream_protocol(),
                      sys_err_code);
  abort_on_fail("open");

  m_ev_wait_hndl_peer_socket.assign(Native_handle(m_peer_socket->native_handle()));

  // Logging done; can blow up Log_context super-object to as-if-default-cted state.
  const auto logger_ptr = get_logger();
  *(static_cast<Log_context*>(this)) = Log_context(nullptr, Log_component::S_TRANSPORT);
  // This no longer matters for logging; might as well empty it here.  Obv save it for the new guy.
  auto nickname_str = std::move(m_nickname);
  m_nickname.clear(); // Just in case move() didn't do it.

  // The easy+important part is to finally make a fresh core like ex-*this.
  return Native_socket_stream(logger_ptr, nickname_str, Native_handle(peer_socket_raw_hndl));
} // Native_socket_stream::Impl::release()

Native_socket_stream::Impl::~Impl()
{
  FLOW_LOG_INFO("Socket stream [" << *this << "]: Shutting down.  Next peer socket will close if open; "
                "and that is it.  They simply cannot advance our state machine via on_active_ev_func()s we "
                "handed out.");
}

bool Native_socket_stream::Impl::replace_event_wait_handles
       (const Function<util::sync_io::Asio_waitable_native_handle ()>& create_ev_wait_hndl_func)
{
  if ((!m_conn_ev_wait_func.empty()) || (!m_snd_ev_wait_func.empty()) || (!m_rcv_ev_wait_func.empty()))
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

bool Native_socket_stream::Impl::start_connect_ops(util::sync_io::Event_wait_func&& ev_wait_func)
{
  return start_ops<Op::S_CONN>(std::move(ev_wait_func));
} // Native_socket_stream::Impl::start_connect_ops()

bool Native_socket_stream::Impl::async_connect(const Shared_name& absolute_name, Error_code* sync_err_code_ptr,
                                               flow::async::Task_asio_err&& on_done_func)
{
  namespace bind_ns = flow::util::bind_ns;
  using asio_local_stream_socket::Endpoint;
  using asio_local_stream_socket::Peer_socket;
  using asio_local_stream_socket::endpoint_at_shared_name;
  using flow::util::ostream_op_string;
  using flow::async::Task_asio_err;
  using util::Task;

  if (m_state != State::S_NULL)
  {
    FLOW_LOG_WARNING("Socket stream [" << *this << "]: Wanted to connect to [" << absolute_name << "] "
                     "but already not in NULL state.  Ignoring.");
    return false;
  }
  // else
  if (!op_started<Op::S_CONN>("async_connect"))
  {
    return false;
  }
  // else

  m_state = State::S_CONNECTING;

  FLOW_LOG_INFO("Socket stream [" << *this << "]: Will attempt connect to [" << absolute_name << "].");

  Error_code sync_err_code;
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
        m_conn_ev_wait_func(&m_ev_wait_hndl_peer_socket,
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

  // Standard error-reporting semantics.
  if ((!sync_err_code_ptr) && sync_err_code)
  {
    throw flow::error::Runtime_error(sync_err_code, "Native_socket_stream::Impl::async_connect()");
  }
  // else
  sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
  // And if (!sync_err_code_ptr) + no error => no throw.

  return true;
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

} // namespace ipc::transport::sync_io
