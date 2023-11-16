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
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/log/log.hpp>

namespace ipc::transport
{

// Types.

/**
 * Implements the Persistent_mq_handle concept by thinly wrapping `bipc::message_queue`, which is boost.interprocess's
 * persistent message queue API.  Internally `bipc::message_queue` maintains a boost.interprocess (classic)
 * SHM pool which itself is a portable wrapper around a standard POSIX shared memory object.
 *
 * @see Persistent_mq_handle: implemented concept.  Its doc header provides plenty of background for the present
 *      concrete class, since essentially that concept was inspired by `bipc::message_queue` in the first place.
 *      Therefore there is essentially no added documentation needed here.
 *
 * Reminder: This is available publicly in case it is useful; but it is more likely one would use a Blob_stream
 * which provides a far more convenient boost.asio-like async-capable API.  It uses class(es) like this one in
 * its impl.
 *
 * @internal
 * ### Implementation ###
 * It is really a thin wrapper around `bipc::message_queue` as discussed above/in Persistent_mq_handle doc header.
 * To the extent it adds its own intelligence, it's only about normalizing error reporting and adding logging.
 */
class Bipc_mq_handle : // Note: movable but not copyable.
  public flow::log::Log_context
{
public:
  // Constants.

  /// Implements concept API.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// Implements concept API.
  static constexpr bool S_HAS_NATIVE_HANDLE = false;

  // Constructors/destructor.

  /// Implements Persistent_mq_handle API: Construct null handle.
  Bipc_mq_handle();

  /**
   * Implements Persistent_mq_handle API: Construct handle to non-existing named MQ, creating it first.  If it already
   * exists, it is an error.
   *
   * @see Persistent_mq_handle::Persistent_mq_handle(): implemented concept.
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
  explicit Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
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
  explicit Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
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
  explicit Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
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
  Bipc_mq_handle(Bipc_mq_handle&& src);

  /// Copying of handles is prohibited, per Persistent_mq_handle concept.
  Bipc_mq_handle(const Bipc_mq_handle&) = delete;

  /**
   * Implements Persistent_mq_handle API: Destroys this handle (or no-op if no handle was successfully constructed, or
   * if it's a moved-from or default-cted handle).  Reminder: The underlying MQ (if any) is *not* destroyed and can
   * be attached-to by another handle.
   *
   * @see Persistent_mq_handle::~Persistent_mq_handle(): implemented concept.
   */
  ~Bipc_mq_handle();

  // Methods.

  /**
   * Implements Persistent_mq_handle API: Replaces handle with the source handle while making the latter invalid as-if
   * default-cted.  Reminder, informally: this is a light-weight op.
   *
   * @param src
   *        See above.
   * @return `*this`.
   */
  Bipc_mq_handle& operator=(Bipc_mq_handle&& src);

  /// Copying of handles is prohibited, per Persistent_mq_handle concept.
  Bipc_mq_handle& operator=(const Bipc_mq_handle&) = delete;

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
   * @param absolute_name
   *        See above.
   * @param err_code
   *        See above.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                Error_code* err_code = 0);

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
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
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
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
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
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
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
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param timeout_from_now
   *        See above.
   * @param err_code
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
  bool timed_receive(util::Blob_mutable* blob, util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Implements Persistent_mq_handle API: Like try_receive() but without the actual popping of a message.
   *
   * @see Persistent_mq_handle::is_receivable(): implemented concept.
   *
   * ### INFO+ logging ###
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
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
   * WARNING on error.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
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
   * WARNING on error or timed out.  (You may use the `flow::log::Config::this_thread_verbosity_override_auto()` to
   * temporarily, in that thread only, disable/reduce logging.  This is quite easy and performant.)
   *
   * @param timeout_from_now
   *        See above.
   * @param err_code
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

private:
  // Types.

  /// Specifies a variation of each set (sends, receives) of operations.
  enum class Wait_type
  {
    /// Poll-type (non-blocking).
    S_POLL,
    /// Wait-type (blocking indefinitely).
    S_WAIT,
    /// Timed-wait-type (blocking until timeout).
    S_TIMED_WAIT
  };

  // Friends.

  // Friend of Bipc_mq_handle.
  friend void swap(Bipc_mq_handle& val1, Bipc_mq_handle& val2);

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
  explicit Bipc_mq_handle(Mode_tag mode_tag, flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                          size_t max_n_msg_on_create, size_t max_msg_sz_on_create,
                          const util::Permissions& perms_on_create,
                          Error_code* err_code);

  // Methods.

  /**
   * Error helper: Run `func()` which will perform a `bipc::message_queue` API that might throw; if it does
   * emit an error in the way our Flow-style APIs must (set #Error_code if not null, else throw... etc.).
   *
   * @tparam Func
   *         Functor that takes no args and returns nothing.
   * @param func
   *        `func()` shall be executed synchronously.
   * @param err_code
   *        `err_code` as-if passed to our API.  `*err_code` shall be set if not null depending on success/failure;
   *        if null and no problem, won't throw; if null and problem will throw `Runtime_error` containing a truthy
   *        #Error_code.  That's the usual Flow-style error emission semantics.
   * @param context
   *        Description for logging of the op being attempted, in case there is an error.  Compile-time-known strings
   *        are best, as this is not protected by a filtering macro and hence will be evaluated no matter what.
   */
  template<typename Func>
  void op_with_possible_bipc_mq_exception(Error_code* err_code, util::String_view context, const Func& func);

  /**
   * Impl body for `interrupt_*()` and `allow_*()`.
   *
   * @tparam SND_ELSE_RCV
   *         True for `*_sendable()`, else `*_receivable()`.
   * @tparam ON_ELSE_OFF
   *         True for `interrupt_*()`, else `allow_*()`.
   * @return See callers.
   */
  template<bool SND_ELSE_RCV, bool ON_ELSE_OFF>
  bool interrupt_allow_impl();

  /**
   * Impl body for `*_sendable()` and `*_receivable()`.
   *
   * @tparam WAIT_TYPE
   *         See Wait_type.
   * @tparam SND_ELSE_RCV
   *         True for `*_sendable()`, else `*_receivable()`.
   * @param timeout_from_now
   *        `timeout_from_now` for the `timed_*()` variations; else ignored.
   * @param err_code
   *        See callers.
   * @return See callers.
   */
  template<Wait_type WAIT_TYPE, bool SND_ELSE_RCV>
  bool wait_impl(util::Fine_duration timeout_from_now, Error_code* err_code);

  // Data.

  /**
   * Underlying MQ handle.  We are a thin wrapper around this really.  Null if creation fails in ctor, or if
   * `*this` was moved-from.
   */
  boost::movelib::unique_ptr<bipc::message_queue> m_mq;

  /// See absolute_name().
  Shared_name m_absolute_name;

  /**
   * Starting at `false`, this is made `true` via interrupt_sends(), and back by allow_sends(); when `true`
   * wait_impl() will return with error::Code::S_INTERRUPTED, as checked at
   *   - the start of its wait critical section;
   *   - on being awoken via condition variable during said critical section.  Said condition variable is
   *     notified by the following code:
   *     - `bipc::message_queue` opposite-direction (receiving) code, when it pops a message; and
   *     - interrupt_sends() itself when transitioning #m_interrupting_snd to `true` (also sometimes spuriously
   *       for performance).
   *
   * Protected by the mutex associated with the aforementioned `bipc::message_queue` condition variable.
   * See wait_impl() internals for more detail about this mutex and all of that.
   *
   * (Naturally it also acts as a guard against duplicately calling interrupt_sends() or allow_sends().)
   */
  bool m_interrupting_snd;

  /// Other-direction counterpart to #m_interrupting_snd.
  bool m_interrupting_rcv;
}; // class Bipc_mq_handle

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Handle_name_func>
void Bipc_mq_handle::for_each_persistent(const Handle_name_func& handle_name_func) // Static.
{
  /* bipc does not expose what we want; but we use the knowledge (from perusing its internal source code) that
   * it simply stores a given MQ N's stuff in a SHM pool named also N.  @todo Check Boost version or something?
   * They could change this, although it is unlikely in my opinion (ygoldfel). */
  util::for_each_persistent_shm_pool(handle_name_func);
  // (See that guy's doc header for why we use him and not shm::classic::Pool_arena::for_each_persistent().)
}

} // namespace ipc::transport
