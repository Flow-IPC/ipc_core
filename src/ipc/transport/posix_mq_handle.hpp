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

#include "ipc/util/util_fwd.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/native_handle.hpp"
#include <flow/log/log.hpp>
#include <flow/common.hpp>
#include <mqueue.h>

namespace ipc::transport
{

// Types.

#ifndef FLOW_OS_LINUX
#  error "Posix_mq_handle relies on Linux semantics and has not been tested in other Unix; cannot exist in Windows."
#endif

/**
 * Implements the Persistent_mq_handle concept by wrapping the POSIX message queue API (see `man mq_overview`).
 *
 * @see Persistent_mq_handle: implemented concept.
 *
 * Reminder: This is available publicly in case it is useful; but it is more likely one would use a
 * Blob_stream_mq_sender or Blob_stream_mq_receiver which provides a far more convenient boost.asio-like async-capable
 * API.  It uses class(es) like this one in its impl.
 *
 * native_handle() returns the underlying MQ descriptor; in Linux (the only OS supported as of this writing)
 * this happens to be an FD; and as an FD it can (according to `man mq_overview`) participate in `epoll/poll/select()`.
 * It is simple to wrap this descriptor in a boost.asio `posix::descriptor`.  Having done that, one can
 * `async_wait()` on it, awaiting writability and readability in async fashion not supported by the
 * Persistent_mq_handle concept.  Accordingly Posix_mq_handle::S_HAS_NATIVE_HANDLE is `true`.
 *
 * @internal
 * ### Implementation ###
 * I (ygoldfel) wrote this immediately after Bipc_mq_handle.  Notably the thing that thinly wraps,
 * `bipc::message_queue`, appears to be heavily influenced by the POSIX MQ API in terms of its API and functionality.
 * So the implementation here is conceptually similar.
 */
class Posix_mq_handle : // Note: movable but not copyable.
  public flow::log::Log_context
{
public:
  // Constants.

  /// Implements concept API.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// Implements concept API.  Contrast this value with Bipc_mq_handle::S_HAS_NATIVE_HANDLE.
  static constexpr bool S_HAS_NATIVE_HANDLE = true;

  // Constructors/destructor.

  /// Implements Persistent_mq_handle API: Construct null handle.
  Posix_mq_handle();

  /**
   * Implements Persistent_mq_handle API: Construct handle to non-existing named MQ, creating it first.  If it already
   * exists, it is an error.
   *
   * @see Persistent_mq_handle::Persistent_mq_handle(): implemented concept.
   *
   * `max_n_msg` and `max_msg_sz` are subject to certain OS limits, according to `man mq_overview`.  Watch out for
   * those: we have no control over them here.  The `man` page should give you the necessary information.
   *
   * @param logger_ptr
   *        See above.
   * @param absolute_name
   *        See above.
   * @param mode_tag
   *        See above.
   * @param perms
   *        See above.
   *        Reminder: Suggest the use of util::shared_resource_permissions() to translate
   *        from one of a small handful of levels of access; these apply almost always in practice.
   * @param max_n_msg
   *        See above.
   * @param max_msg_sz
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                           util::Create_only mode_tag, size_t max_n_msg, size_t max_msg_sz,
                           const util::Permissions& perms = util::Permissions(),
                           Error_code* err_code = 0);
  /**
   * Implements Persistent_mq_handle API: Construct handle to existing named MQ, or else if it does not exist creates
   * it first and opens it (atomically).
   *
   * @see Persistent_mq_handle::Persistent_mq_handle(): implemented concept.
   *
   * @param logger_ptr
   *        See above.
   * @param absolute_name
   *        See above.
   * @param mode_tag
   *        See above.
   * @param perms_on_create
   *        See above.
   *        Reminder: Suggest the use of util::shared_resource_permissions() to translate
   *        from one of a small handful of levels of access; these apply almost always in practice.
   * @param max_n_msg_on_create
   *        See above.
   * @param max_msg_sz_on_create
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                           util::Open_or_create mode_tag, size_t max_n_msg_on_create, size_t max_msg_sz_on_create,
                           const util::Permissions& perms_on_create = util::Permissions(),
                           Error_code* err_code = 0);
  /**
   * Implements Persistent_mq_handle API: Construct handle to existing named MQ.  If it does not exist, it is an error.
   *
   * @param logger_ptr
   *        See above.
   * @param absolute_name
   *        See above.
   * @param mode_tag
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                           util::Open_only mode_tag, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Constructs handle from the source handle while making the latter as-if
   * default-cted.  Reminder, informally: This is a light-weight op.
   *
   * @see Persistent_mq_handle::Persistent_mq_handle(): implemented concept.
   *
   * @param src
   *        See above.
   */
  Posix_mq_handle(Posix_mq_handle&& src);

  /// Copying of handles is prohibited, per Persistent_mq_handle concept.
  Posix_mq_handle(const Posix_mq_handle&) = delete;

  /**
   * Implements Persistent_mq_handle API: Destroys this handle (or no-op if no handle was successfully constructed, or
   * if it's a moved-from or default-cted handle).  Reminder: The underlying MQ (if any) is *not* destroyed and can
   * be attached-to by another handle.
   *
   * @see Persistent_mq_handle::~Persistent_mq_handle(): implemented concept.
   */
  ~Posix_mq_handle();

  // Methods.

  /**
   * Implements Persistent_mq_handle API: Replaces handle with the source handle while making the latter invalid as-if
   * default-cted.  Reminder, informally: this is a light-weight op.
   *
   * @param src
   *        See above.
   * @return `*this`.
   */
  Posix_mq_handle& operator=(Posix_mq_handle&& src);

  /// Copying of handles is prohibited, per Persistent_mq_handle concept.
  Posix_mq_handle& operator=(const Posix_mq_handle&) = delete;

  /**
   * Implements Persistent_mq_handle API: Removes the named persistent MQ.  Reminder: name is removed immediately
   * (if present -- otherwise error), but underlying MQ continues to exist until all system-wide handles to it
   * are closed.
   *
   * @see Persistent_mq_handle::remove_persistent(): implemented concept.
   *
   * @see Reminder: see also `util::remove_each_persistent_*()`.
   *
   * @param logger_ptr
   *        See above.
   * @param name
   *        See above.
   * @param err_code
   *        See above.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& name, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API.  Impl note for exposition: we use the fact that, e.g., in Linux
   * the POSIX MQ devices are listed in flat fashion in /dev/mqueue.
   *
   * @see Persistent_mq_handle::for_each_persistent(): implemented concept.
   *
   * @tparam Handle_name_func
   *         See above.
   * @param handle_name_func
   *        See above.
   */
  template<typename Handle_name_func>
  static void for_each_persistent(const Handle_name_func& handle_name_func);

  /**
   * Implements Persistent_mq_handle API: Non-blocking send: pushes copy of message to queue and returns `true`;
   * if queue is full then no-op and returns `false`.
   *
   * @see Persistent_mq_handle::try_send(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool try_send(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Blocking send: pushes copy of message to queue; if queue is full blocks
   * until it is not.
   *
   * @see Persistent_mq_handle::send(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()`
   * to temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param err_code
   *        See above.
   */
  void send(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Blocking timed send: pushes copy of message to queue; if queue is full
   * blocks until it is not, or the specified time passes, whichever happens first.
   *
   * @see Persistent_mq_handle::timed_send(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error or timed out.
   * (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param timeout_from_now
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool timed_send(const util::Blob_const& blob, util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like try_send() but without the actual pushing of a message.
   *
   * @see Persistent_mq_handle::is_sendable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool is_sendable(Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like send() but without the actual pushing of a message.
   *
   * @see Persistent_mq_handle::wait_sendable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   */
  void wait_sendable(Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like timed_send() but without the actual pushing of a message.
   *
   * @see Persistent_mq_handle::timed_wait_sendable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   * @param timeout_from_now
   *        See above.
   * @return See above.
   */
  bool timed_wait_sendable(util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Non-blocking receive: pops copy of message from queue into buffer and
   * returns `true`; if queue is empty then no-op and returns `false`.
   *
   * @see Persistent_mq_handle::try_receive(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool try_receive(util::Blob_mutable* blob, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Blocking receive: pops copy of message from queue into buffer; if queue
   * is empty blocks until it is not.
   *
   * @see Persistent_mq_handle::receive(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param err_code
   *        See above.
   */
  void receive(util::Blob_mutable* blob, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Blocking timed receive: pops copy of message from queue into buffer;
   * if queue is empty blocks until it is not, or the specified time passes, whichever happens first.
   *
   * @see Persistent_mq_handle::timed_receive(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error or timed out.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param blob
   *        See above.
   * @param timeout_from_now
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool timed_receive(util::Blob_mutable* blob, util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like try_receive() but without the actual popping of a message.
   *
   * @see Persistent_mq_handle::is_receivable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool is_receivable(Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like receive() but without the actual popping of a message.
   *
   * @see Persistent_mq_handle::wait_receivable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   */
  void wait_receivable(Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like timed_receive() but without the actual popping of a message.
   *
   * @see Persistent_mq_handle::timed_wait_receivable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.
   *
   * @param err_code
   *        See above.
   * @param timeout_from_now
   *        See above.
   * @return See above.
   */
  bool timed_wait_receivable(util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API:
   * Turn on preemptive/concurrent interruption of blocking-sends and sendable-waits/polls.
   *
   * @see Persistent_mq_handle::interrupt_sends(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on duplicate use, INFO otherwise.
   *
   * @return See above.
   */
  bool interrupt_sends();

  /**
   * Implements Persistent_mq_handle API:
   * Turn off preemptive/concurrent interruption of blocking-sends and sendable-waits/polls.
   *
   * @see Persistent_mq_handle::allow_sends(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on duplicate use, INFO otherwise.
   *
   * @return See above.
   */
  bool allow_sends();

  /**
   * Implements Persistent_mq_handle API:
   * Turn on preemptive/concurrent interruption of blocking-receives and receivable-waits/polls.
   *
   * @see Persistent_mq_handle::interrupt_receives(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on duplicate use, INFO otherwise.
   *
   * @return See above.
   */
  bool interrupt_receives();

  /**
   * Implements Persistent_mq_handle API:
   * Turn off preemptive/concurrent interruption of blocking-receives and receivable-waits/polls.
   *
   * @see Persistent_mq_handle::allow_receives(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on duplicate use, INFO otherwise.
   *
   * @return See above.
   */
  bool allow_receives();

  /**
   * Implements Persistent_mq_handle API: Returns name equal to `absolute_name` passed to ctor.
   * @return See above.
   * @see Persistent_mq_handle::absolute_name(): implemented concept.
   */
  const Shared_name& absolute_name() const;

  /**
   * Implements Persistent_mq_handle API: Returns the max message size of the underlying queue.  Reminder:
   * This is not required to match was was passed to `Create_only` or `Open_or_create` ctor.
   *
   * @return See above.
   * @see Persistent_mq_handle::max_msg_size(): implemented concept.
   */
  size_t max_msg_size() const;

  /**
   * Implements Persistent_mq_handle API: Returns the max message count of the underlying queue.  Reminder:
   * This is not required to match was was passed to `Create_only` or `Open_or_create` ctor.
   *
   * @return See above.
   * @see Persistent_mq_handle::max_n_msgs(): implemented concept.
   */
  size_t max_n_msgs() const;

  /**
   * Implements Persistent_mq_handle API: Returns the stored native MQ handle; null if not open.
   *
   * @return See above.
   * @see Persistent_mq_handle::native_handle(): implemented concept.
   */
  Native_handle native_handle() const;

private:
  // Types.

  /// Short-hand for anonymous pipe write end.
  using Pipe_writer = util::Pipe_writer;

  /// Short-hand for anonymous pipe read end.
  using Pipe_reader = util::Pipe_reader;

  // Friends.

  // Friend of Posix_mq_handle.
  friend void swap(Posix_mq_handle& val1, Posix_mq_handle& val2);

  // Constructors/destructor.

  /**
   * Helper ctor delegated by the 2 `public` ctors that take `Open_or_create` or `Create_only` mode.
   *
   * @tparam Mode_tag
   *         Either util::Open_or_create or util::Create_only.
   * @param logger_ptr
   *        See `public` ctors.
   * @param absolute_name
   *        See `public` ctors.
   * @param mode_tag
   *        See `public` ctors.
   * @param max_n_msg_on_create
   *        See `public` ctors.
   * @param max_msg_sz_on_create
   *        See `public` ctors.
   * @param perms_on_create
   *        See `public` ctors.
   * @param err_code
   *        See `public` ctors.
   */
  template<typename Mode_tag>
  explicit Posix_mq_handle(Mode_tag mode_tag, flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                           size_t max_n_msg_on_create, size_t max_msg_sz_on_create,
                           const util::Permissions& perms_on_create,
                           Error_code* err_code);

  // Methods.

  /**
   * Ctor helper that sets up `m_interrupt*` pipe items.  If it fails it returns truthy code
   * and cleans up what it did.  It ignores everything else like #m_mq.
   *
   * @return See above.
   */
  Error_code pipe_setup();

  /**
   * Ctor helper that sets up #m_epoll_hndl_snd and #m_epoll_hndl_rcv.  If it fails it returns truthy code
   * and puts everything back to as-if-ctor-failed state, including #m_mq being null.
   *
   * @return See above.
   */
  Error_code epoll_setup();

  /**
   * Sets #m_mq to blocking or non-blocking and returns `true` on success and clears `*err_code`; otherwise returns
   * `false` and sets truthy `*err_code`.
   *
   * @param err_code
   *        To set.  If null behavior is undefined (assertion may trip).
   * @param nb
   *        Non-blocking if `true`, else blocking.
   * @return `true` <=> success.
   */
  bool set_non_blocking(bool nb, Error_code* err_code);

  /**
   * Helper that handles the result of an `mq_*()` call by logging WARNING(s) on error; setting `*err_code` on error;
   * clearing it on success.  `errno` is presumably set by the native API on error going into this.
   *
   * @param result
   *        What the native API returned.
   * @param err_code
   *        To set.  If null behavior is undefined (assertion may trip).
   * @param context
   *        See Bipc_mq_handle::op_with_possible_bipc_mq_exception().
   * @return `true` if `*err_code` was set to success (falsy); else `false` (it was set to truthy).
   */
  bool handle_mq_api_result(int result, Error_code* err_code, util::String_view context) const;

  /**
   * Impl body for `interrupt_*()`.
   *
   * @tparam SND_ELSE_RCV
   *         True for `*_sends()`, else `*_receives()`.
   * @return See callers.
   */
  template<bool SND_ELSE_RCV>
  bool interrupt_impl();

  /**
   * Impl body for `allow_*()`.
   *
   * @tparam SND_ELSE_RCV
   *         True for `*_sends()`, else `*_receives()`.
   * @return See callers.
   */
  template<bool SND_ELSE_RCV>
  bool allow_impl();

  /**
   * Impl body for `*_sendable()` and `*_receivable()`.
   *
   * @param timeout_from_now_or_none
   *        `timeout_from_now`; or 0 for `is_*()`, or `Fine_duration::max()` for non-timed-blocking variant.
   * @param snd_else_rcv
   *        True for `*_sendable()`, else `*_receivable()`.
   * @param err_code
   *        See callers.
   * @return See callers.
   */
  bool wait_impl(util::Fine_duration timeout_from_now_or_none, bool snd_else_rcv, Error_code* err_code);

  // Data.

  /**
   * Underlying MQ handle.  We are a thin wrapper around this really.  `.null()` if creation
   * fails in ctor, or if `*this` was moved-from.  This is very light-weight; probably `int`.
   */
  Native_handle m_mq;

  /// See absolute_name().
  Shared_name m_absolute_name;

  /**
   * `epoll_*()` handle (`.null()` if and only if #m_mq is null) that is level-triggered to be active
   * (with only 1 event registered) if and only if #m_mq is currently capable of sending at least 1 message
   * (we could push 1+ messages right now).  Used by wait_impl() and its various public `*_sendable()` forms.
   */
  Native_handle m_epoll_hndl_snd;

  /**
   * `epoll_*()` handle (`.null()` if and only if #m_mq is null) that is level-triggered to be active
   * (with only 1 event registered) if and only if #m_mq is currently storing at least 1 message
   * (we could pop 1+ messages right now).  Used by wait_impl() and its various public `*_receivable()` forms.
   */
  Native_handle m_epoll_hndl_rcv;

  /**
   * Starting at `false`, this is made `true` via interrupt_sends(), and back by allow_sends(); acts as a guard
   * against doing it when already in effect.  Note that that is its only purpose; as wait_impl() never checks it;
   * instead it relies on `epoll_wait()` detecting a readable #m_interrupt_detector_rcv.
   */
  bool m_interrupting_snd;

  /// Other-direction counterpart to #m_interrupting_snd.
  bool m_interrupting_rcv;

  /// Never used for `.run()` or `.async()` -- just so we can construct #Pipe_reader, #Pipe_writer.
  flow::util::Task_engine m_nb_task_engine;

  /**
   * A byte is written to this end by interrupt_sends() to make it readable for the poll-wait in wait_impl()
   * indicating that #m_interrupting_snd mode is on.
   */
  Pipe_writer m_interrupter_snd;

  /**
   * A byte is read from this end by allow_sends() to make it not-readable for the poll-wait in wait_impl()
   * indicating that #m_interrupting_snd mode is off; and wait_impl() poll-waits on this along with
   * #m_mq -- if it is readable, then the mode is on.
   */
  Pipe_reader m_interrupt_detector_snd;

  /// Other-direction counterpart to #m_interrupter_snd.
  Pipe_writer m_interrupter_rcv;

  /// Other-direction counterpart to #m_interrupt_detector_snd.
  Pipe_reader m_interrupt_detector_rcv;
}; // class Posix_mq_handle

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Handle_name_func>
void Posix_mq_handle::for_each_persistent(const Handle_name_func& handle_name_func) // Static.
{
#ifndef FLOW_OS_LINUX
#  error "This method relies on/has been tested only with Linux /dev/mqueue semantics."
#endif
  util::for_each_persistent_impl("/dev/mqueue", handle_name_func);
}

} // namespace ipc::transport
