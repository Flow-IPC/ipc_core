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
#pragma once

#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/native_socket_stream_acceptor.hpp"
#include "ipc/transport/detail/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include "ipc/util/sync_io/asio_waitable_native_hndl.hpp"
#include "ipc/transport/error.hpp"

namespace ipc::transport::sync_io
{

// Types.

/**
 * `sync_io`-pattern counterpart to async-I/O-pattern transport::Native_socket_stream_acceptor.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 * @see transport::Native_socket_stream_acceptor -- generally all notes, other than the mechanism of how
 *      async_accept() reports readiness, in that doc header apply here.
 *
 * As is generally the case when choosing `sync_io::X` versus `X`, we recommend using `X` due to it being easier.
 * In this particular case (see below) there is no perf benefit to using `sync_io::X`, either, so the only reason
 * to use `sync_io::X` in this case would be because you've got an old-school reactor event loop with
 * a `poll()` or `epoll_wait()`, in which case the `sync_io` API may be easier to integrate.
 *
 * ### Internal implementation ###
 * Normally this would not be in the public docs for this public-use class, but indulge us.
 *
 * In perf-critical situations, such as the various transport::Blob_sender / transport::Blob_receiver / etc.
 * impls, typically `sync_io::X` contains the core implementation, then `X` adapts a `sync_io::X` *core*
 * to provide background-thread-driven work and signaling of completion.  We do not consider
 * the present `X = Native_socket_stream_acceptor` to be perf-critical; as such as of this writing
 * transport::Native_socket_stream_acceptor has the logic, while sync_io::Native_socket_stream_acceptor
 * adapts it into the `sync_io` pattern.  Internally this is accomplished using an unnamed IPC-pipe, where
 * an internal background thread W tickles said IPC-pipe which is waited-on by the user of
 * sync_io::Native_socket_stream_acceptor.
 */
class Native_socket_stream_acceptor :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for type of target peer-socket objects targeted by async_accept().
  using Peer = transport::Native_socket_stream::Sync_io_obj;

  /// You may disregard.
  using Sync_io_obj = Null_peer;
  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::Native_socket_stream_acceptor;

  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * All notes from #Async_io_obj API counterpart API apply.  However, additionally, per `sync_io` pattern,
   * you'll need to invoke start_accept_ops() (possibly preceded by replace_event_wait_handles())
   * before further real work.
   *
   * If `err_code` is not null, and this emits truthy `*err_code`, then any use of `*this` other
   * than destruction => undefined behavior.
   *
   * @param logger_ptr
   *        See #Async_io_obj API.
   * @param absolute_name
   *        See #Async_io_obj API.
   * @param err_code
   *        See #Async_io_obj API.
   */
  explicit Native_socket_stream_acceptor(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                         Error_code* err_code = 0);

  /**
   * All notes from #Async_io_obj API counterpart API apply, except that no completion handler(s) are fired,
   * even if an async-accept is currently outstanding.
   */
  ~Native_socket_stream_acceptor();

  // Methods.

  /**
   * All notes from #Async_io_obj API counterpart API apply.
   *
   * @return See above.
   */
  const Shared_name& absolute_name() const;

  /**
   * Sets up the `sync_io`-pattern interaction between `*this` and the user's event loop; required before
   * async_accept() will work (as opposed to no-op/return `false`).
   *
   * `ev_wait_func()` -- with signature matching util::sync_io::Event_wait_func -- is a key function memorized
   * by `*this`.  It shall be invoked by `*this` operations when some op cannot complete synchronously and requires
   * a certain event (readable/writable) to be active on a certain native-handle.
   *
   * @see util::sync_io::Event_wait_func doc header for useful and complete instructions on how to write an
   *      `ev_wait_func()` properly.  Doing so correctly is the crux of using the `sync_io` pattern.
   *
   * This is a standard `sync_io`-pattern API per util::sync_io doc header.
   *
   * @tparam Event_wait_func_t
   *         Function type matching util::sync_io::Event_wait_func.
   * @param ev_wait_func
   *        See above.
   * @return `false` if this has already been invoked; no-op logging aside.  `true` otherwise.
   */
  template<typename Event_wait_func_t>
  bool start_accept_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Analogous to transport::sync_io::Native_handle_sender::replace_event_wait_handles().
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Starts background attempt at a peer connection being established, contingent on the user reporting
   * (via `sync_io` pattern) the necessary native handle event(s); completion handler `on_done_func()` shall
   * execute synchronously during such a future report.  Or perhaps in plainer English:
   *   -# Call this.
   *      -# It will synchronously invoke util::sync_io::Event_wait_func from start_accept_ops(),
   *         causing you to synchronously begin an async-wait for some status of some native handle (FD).
   *   -# Once that async-wait completes, call `(*on_active_ev_func)()` to report that fact to `*this`.
   *      -# It will synchronously invoke `on_done_func()` saved from the present call,
   *         indicating via the `Error_code` arg success or failure.  The semantics of this are equivalent
   *         to those of #Async_io_obj `async_accept()`.
   *
   * @note There is no mode of async_accept() wherein `*target_peer` is synchronously (immediately) available.
   *       Completion shall always be reported via `on_done_func()` after a mandatory async-wait.
   *       This is in contrast to perf-critical operations such as, say, Native_socket_stream::async_receive_blob().
   *       (Native_socket_stream::async_connect() also behaves in that dual manner, for consistency with the
   *       other APIs of that type, even though async-connect is not usually thought of as perf-critical.)
   *
   * @tparam Task_err
   *         See #Async_io_obj API.
   * @param target_peer
   *        See #Async_io_obj API.
   * @param on_done_func
   *        See #Async_io_obj API.
   * @return `true` if op started; `false` if one is already in progress, and therefore this one is ignored (no-op).
   */
  template<typename Task_err>
  bool async_accept(Peer* target_peer, Task_err&& on_done_func);

private:
  // Types.

  /// Short-hand for callback called on new peer-to-peer connection; or on unrecoverable error.
  using On_peer_accepted_func = flow::async::Task_asio_err;

  // Data.

  /**
   * The `Task_engine` for `m_ready_*`.  It is necessary to construct those pipe-end objects, but we never
   * use that guy's `->async_*()` APIs -- only non-blocking operations, essentially leveraging boost.asio's
   * portable transmission APIs but not its actual, um, async-I/O abilities in this case.  Accordingly we
   * never load any tasks onto #m_nb_task_engine and certainly never `.run()` (or `.poll()` or ...) it.
   *
   * In the `sync_io` pattern the user's outside event loop is responsible for awaiting readability/writability
   * of a guy like #m_ready_reader via our exporting of its `.native_handle()`.
   */
  flow::util::Task_engine m_nb_task_engine;

  /**
   * The `Task_engine` for #m_ev_wait_hndl, unless it is replaced via replace_event_wait_handles().
   *
   * This is to fulfill the `sync_io` pattern.
   */
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /**
   * Read-end of IPC-pipe used by `*this` user do detect that an acceptability-wait has completed.  The signal byte
   * is read out of #m_ready_reader, making it empty again (the steady-state before the next time acceptability-wait
   * begins, and a byte is written to it making it non-empty).
   *
   * @see #m_ready_writer.
   */
  util::Pipe_reader m_ready_reader;

  /**
   * Write-end of IPC-pipe together with #m_ready_reader.
   * @see #m_ready_reader.
   */
  util::Pipe_writer m_ready_writer;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_ready_reader.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl;

  /**
   * Function (set forever in start_accept_ops()) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to our ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_ev_wait_func;

  /**
   * `on_done_func` from async_accept() if one is pending; otherwise `.empty()`.  We maybe could have kept it as
   * a lambda capture only, but this way it can double as a guard against allowing 2+ async-accepts (which goes
   * against the spirit of what `sync_io`-pattern objects provide).
   */
  On_peer_accepted_func m_on_done_func_or_empty;

  /// Result of last `m_async_io.async_accept()`.
  Error_code m_target_err_code;

  /// This guy does all the work.  We merely report async_accept() completion via #m_ready_reader and #m_ready_writer.
  Async_io_obj m_async_io;
}; // class Native_socket_stream_acceptor

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Task_err>
bool Native_socket_stream_acceptor::async_accept(Peer* target_peer, Task_err&& on_done_func)
{
  using util::Task;

  if (!m_on_done_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Acceptor [" << *this << "]: Async-accept requested, but one is already in progress.  Ignoring.");
    return false;
  }
  // else

  m_on_done_func_or_empty = std::move(on_done_func);

  m_async_io.async_accept(target_peer, [this](const Error_code& err_code)
  {
    if (err_code == error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
    {
      return; // Stuff is shutting down.  GTFO.
    }
    // else

    FLOW_LOG_TRACE("Acceptor [" << *this << "]: Async-IO core async-accept done; tickling IPC-pipe to inform user.");
    m_target_err_code = err_code;

    util::pipe_produce(get_logger(), &m_ready_writer);
  }); // m_async_io.async_accept()

  m_ev_wait_func(&m_ev_wait_hndl,
                 false, // Wait for read.
                 boost::make_shared<Task>
                   ([this]()
  {
    FLOW_LOG_INFO("Acceptor [" << *this << "]: Async-IO core async-accept finished: informed via IPC-pipe; "
                  "invoking handler.");
    util::pipe_consume(get_logger(), &m_ready_reader);

    auto on_done_func = std::move(m_on_done_func_or_empty);
    m_on_done_func_or_empty.clear(); // In case move() didn't do it.

    on_done_func(m_target_err_code);
    FLOW_LOG_TRACE("Handler completed.");
  }));

  return true;
} // Native_socket_stream_acceptor::async_accept()

template<typename Event_wait_func_t>
bool Native_socket_stream_acceptor::start_accept_ops(Event_wait_func_t&& ev_wait_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Acceptor [" << *this << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  m_ev_wait_func = std::move(ev_wait_func);
  FLOW_LOG_INFO("Acceptor [" << *this << "]: Start-ops requested.  Done.");
  return true;
}

template<typename Create_ev_wait_hndl_func>
bool Native_socket_stream_acceptor::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Acceptor [" << *this << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Acceptor [" << *this << "]: Replacing event-wait handles (probably to replace underlying "
                "execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl.is_open());

  Native_handle saved(m_ev_wait_hndl.release());
  m_ev_wait_hndl = create_ev_wait_hndl_func();
  m_ev_wait_hndl.assign(saved);

  return true;
} // Native_socket_stream::Impl::replace_event_wait_handles()

} // namespace ipc::transport::sync_io
