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

#include "ipc/transport/sync_io/blob_stream_mq_snd.hpp"
#include "ipc/transport/detail/blob_stream_mq_impl.hpp"
#include "ipc/transport/sync_io/detail/async_adapter_snd.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include <flow/async/single_thread_task_loop.hpp>

namespace ipc::transport
{

// Types.

/**
 * Internal, non-movable pImpl-lite implementation of Blob_stream_mq_sender class template.
 * In and of itself it would have been directly and publicly usable; however Blob_stream_mq_sender adds move semantics
 * which are essential to cooperation with the rest of the API, Channel in particular.
 *
 * @see All discussion of the public API is in Blob_stream_mq_sender doc header; that class template forwards to this
 *      one.  All discussion of pImpl-lite-related notions is also there.  See that doc header first please.  Then come
 *      back here.
 *
 * ### Impl design ###
 * The actual logic in this class is very simple, because (1) it's either in PEER state or NULL state, and there
 * is no CONNECTING to speak of -- the only way to go between them is move-assignment; and (2) while in PEER state
 * it is only a matter of adapting a sync_io::Blob_sender -- namely sync_io::Blob_stream_mq_sender -- *core* into
 * the requirements of async-I/O-pattern Blob_sender concept.  PEER state has 100% of the logic; and
 * 99% of that logic is adapting which is accomplished by class template sync_io::Async_adapter_sender.
 *
 * Therefore:
 *
 * @see sync_io::Blob_stream_mq_sender for the core logic having to do with managing MQs.
 * @see sync_io::Async_adapter_sender for the transport-agnostic adapting of the latter (as a sync_io::Blob_sender)
 *      into a `*this`.  We just forward to it.
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender_impl :
  public Blob_stream_mq_base_impl<Persistent_mq_handle>,
  public flow::log::Log_context,
  private boost::noncopyable // And non-movable.
{
public:
  // Types.

  /// Short-hand for our base with `static` goodies at least.
  using Base = Blob_stream_mq_base_impl<Persistent_mq_handle>;

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = typename Base::Mq;

  // Constructors/destructor.

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * @param logger_ptr
   *        See Blob_stream_mq_sender counterpart.
   * @param mq_moved
   *        See Blob_stream_mq_sender counterpart.
   * @param nickname_str
   *        See Blob_stream_mq_sender counterpart.
   * @param err_code
   *        See Blob_stream_mq_sender counterpart.
   */
  explicit Blob_stream_mq_sender_impl(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                      Mq&& mq_moved, Error_code* err_code);

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See Blob_stream_mq_sender counterpart.
   */
  explicit Blob_stream_mq_sender_impl(sync_io::Blob_stream_mq_sender<Mq>&& sync_io_core_in_peer_state_moved);

  /**
   * See Blob_stream_mq_sender counterpart.
   *
   * Impl note:
   * The unspecified thread, as of this writing, is not thread W.  There is rationale discussion and detail
   * in the body, but it seemed prudent to point it out here.
   */
  ~Blob_stream_mq_sender_impl();

  // Methods.

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  size_t send_blob_max_size() const;

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param blob
   *        See Blob_stream_mq_sender counterpart.
   * @param err_code
   *        See Blob_stream_mq_sender counterpart.
   */
  void send_blob(const util::Blob_const& blob, Error_code* err_code);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param on_done_func
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool end_sending();

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @param period
   *        See Blob_stream_mq_sender counterpart.
   * @return See Blob_stream_mq_sender counterpart.
   */
  bool auto_ping(util::Fine_duration period);

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   * @return See Blob_stream_mq_sender counterpart.
   */
  const std::string& nickname() const;

  /**
   * See Blob_stream_mq_sender counterpart, but assuming PEER state.
   *
   * @return See Blob_stream_mq_sender counterpart.
   */
  const Shared_name& absolute_name() const;

private:
  // Data.

  /**
   * Single-thread worker pool for all internal async work.  Referred to as thread W in comments.
   *
   * Ordering: Must be either declared after mutex(es), or `.stop()`ed explicitly in dtor: Thread must be joined,
   * before mutex possibly-locked-in-it destructs.
   */
  flow::async::Single_thread_task_loop m_worker;

  /**
   * The core `Blob_stream_mq_sender` engine, implementing the `sync_io` pattern (see util::sync_io doc header).
   * See our class doc header for overview of how we use it (the aforementioned `sync_io` doc header talks about
   * the `sync_io` pattern generally).
   *
   * Thus, #m_sync_io is the synchronous engine that we use to perform our work in our asynchronous boost.asio
   * loop running in thread W (#m_worker) while collaborating with user thread(s) a/k/a thread U.
   * (Recall that the user may choose to set up their own event loop/thread(s) --
   * boost.asio-based or otherwise -- and use their own equivalent of an #m_sync_io instead.)
   *
   * ### Order subtlety versus `m_worker` ###
   * When constructing #m_sync_io, we need the `Task_engine` from #m_worker.  On the other hand tasks operating
   * in #m_worker access #m_sync_io.  So in destructor it is important to `m_worker.stop()` explicitly, so that
   * the latter is no longer a factor.  Then when automatic destruction occurs in the opposite order of
   * creation, the fact that #m_sync_io is destroyed before #m_worker has no bad effect.
   */
  sync_io::Blob_stream_mq_sender<Mq> m_sync_io;

  /**
   * This handles ~all logic in that state.  sync_io::Async_adapter_sender adapts
   * any sync_io::Blob_sender and makes available ~all necessary async-I/O Blob_sender APIs.
   * So we forward ~everything to this guy.
   *
   * ### Creation ###
   * By its contract, this guy's ctor will handle what it needs to, as long as #m_worker (to which it stores a pointer)
   * has been `.start()`ed by that time, and #m_sync_io (to which it stores... ditto) has been
   * `.replace_event_wait_handles()`ed as required.
   *
   * ### Destruction ###
   * By its contract, this guy's dtor will handle what it needs to, as long as #m_worker (to which it stores a pointer)
   * has been `.stop()`ed by that time, and any queued-up (ready to execute) handlers on it have been
   * `Task_enginer::poll()`ed-through by that time as well.
   *
   * ### Why `optional`? ###
   * The only reason is we have to invoke a certain method on #m_sync_io before we can construct this guy.
   */
  std::optional<sync_io::Async_adapter_sender<decltype(m_sync_io)>> m_sync_io_adapter;
}; // class Blob_stream_mq_sender

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Persistent_mq_handle>
Blob_stream_mq_sender_impl<Persistent_mq_handle>::Blob_stream_mq_sender_impl
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq, Error_code* err_code) :
  // Create core ourselves; then delegate to other ctor.
  Blob_stream_mq_sender_impl
    (sync_io::Blob_stream_mq_sender<Mq>(logger_ptr, nickname_str, std::move(mq), err_code))
{
  /* That's all.  Note, though, sync_io::Blob_stream_mq_sender ctor may have thrown.  Then we threw; no problem.
   * If err_code not null, it may have set *err_code to true -- but still memorized the poorly cted m_sync_io.
   * That is okay. */
}

// Delegated-to or public ctor.
template<typename Persistent_mq_handle>
Blob_stream_mq_sender_impl<Persistent_mq_handle>::Blob_stream_mq_sender_impl
  (sync_io::Blob_stream_mq_sender<Mq>&& sync_io_core_in_peer_state_moved) :
  flow::log::Log_context(sync_io_core_in_peer_state_moved.get_logger(), Log_component::S_TRANSPORT),
  m_worker(sync_io_core_in_peer_state_moved.get_logger(), sync_io_core_in_peer_state_moved.nickname()),
  // Adopt the just-cted, idle sync_io:: core.
  m_sync_io(std::move(sync_io_core_in_peer_state_moved))
  // m_sync_io_adapter is null but is set-up shortly below.
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::util::ostream_op_string;

  m_worker.start();

  // We're using a boost.asio event loop, so we need to base the async-waited-on handles on our Task_engine.
#ifndef NDEBUG
  bool ok =
#endif
  m_sync_io.replace_event_wait_handles([this]() -> Asio_waitable_native_handle
                                         { return Asio_waitable_native_handle(*(m_worker.task_engine())); });
  assert(ok && "Did you break contract by passing-in a non-fresh sync_io core object to ctor?");

  /* Have to do this after .replace_event_wait_handles() by the adapter's ctor's contract.
   * As of this writing that's the only reason m_sync_io_adapter is optional<>. */
  m_sync_io_adapter.emplace(get_logger(), ostream_op_string("Blob_stream_mq_sender [", *this, ']'),
                            &m_worker, &m_sync_io);
} // Blob_stream_mq_sender_impl::Blob_stream_mq_sender_impl()

template<typename Persistent_mq_handle>
Blob_stream_mq_sender_impl<Persistent_mq_handle>::~Blob_stream_mq_sender_impl()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;

  // We are in thread U.  By contract in doc header, they must not call us from a completion handler (thread W).

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Shutting down.  All our "
                "internal async handlers will be canceled; and worker thread will be joined.");

  /* This (1) stop()s the Task_engine thus possibly
   * preventing any more handlers from running at all (any handler possibly running now is the last one to run); (2)
   * at that point Task_engine::run() exits, hence thread W exits; (3) joins thread W (waits for it to
   * exit); (4) returns.  That's a lot, but it's non-blocking. */
  m_worker.stop();
  // Thread W is (synchronously!) no more.

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Continuing shutdown.  "
                "Next we will run user handler (if any) from some other thread.  "
                "In this user thread we will await those handlers' completion and then return.");

  // See comment in similar spot in Native_socket_stream::~Impl() regarding the following.

  FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: Continuing shutdown.  Next we will run pending handlers "
                "from some other thread.  In this user thread we will await those handlers' completion and then "
                "return.");

  Single_thread_task_loop one_thread(get_logger(), ostream_op_string(nickname(), "-temp_deinit"));
  one_thread.start([&]()
  {
    FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending internal handlers (typically none).");

    const auto task_engine = m_worker.task_engine();
    task_engine->restart();
    const auto count = task_engine->poll();
    if (count != 0)
    {
      FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: "
                    "In transient finisher thread: Ran [" << count << "] internal handlers after all.");
    }
    task_engine->stop();

    FLOW_LOG_INFO("Transient finisher exiting.  (Send-ops de-init may follow.)");
  });
  // Here thread exits/joins synchronously.
} // Blob_stream_mq_sender_impl::~Blob_stream_mq_sender_impl()

template<typename Persistent_mq_handle>
void Blob_stream_mq_sender_impl<Persistent_mq_handle>::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  m_sync_io_adapter->send_blob(blob, err_code);
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::end_sending()
{
  using flow::async::Task_asio_err;

  return async_end_sending(Task_asio_err());
}

template<typename Persistent_mq_handle>
template<typename Task_err>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::async_end_sending(Task_err&& on_done_func)
{
  return m_sync_io_adapter->async_end_sending(std::move(on_done_func));
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender_impl<Persistent_mq_handle>::auto_ping(util::Fine_duration period)
{
  return m_sync_io_adapter->auto_ping(period);
}

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_sender_impl<Persistent_mq_handle>::send_blob_max_size() const
{
  /* Never changes (always in PEER state); no need to lock.  Contrast with transport::Native_socket_stream::Impl
   * which has to rationalize somewhat harder... but also locks nothing here. */
  return m_sync_io.send_blob_max_size();
}

template<typename Persistent_mq_handle>
const Shared_name& Blob_stream_mq_sender_impl<Persistent_mq_handle>::absolute_name() const
{
  return m_sync_io.absolute_name();
}

template<typename Persistent_mq_handle>
const std::string& Blob_stream_mq_sender_impl<Persistent_mq_handle>::nickname() const
{
  return m_sync_io.nickname();
}

template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender_impl<Persistent_mq_handle>& val)
{
  return
    os << '[' << val.nickname() << "]@" << static_cast<const void*>(&val) << " sh_name[" << val.absolute_name() << ']';
}

} // namespace ipc::transport
