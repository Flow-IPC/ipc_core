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

#include "ipc/transport/sync_io/detail/blob_stream_mq_snd_impl.hpp"
#include "ipc/transport/blob_stream_mq.hpp"
#include <experimental/propagate_const>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Implements sync_io::Blob_sender concept by using an adopted Persistent_mq_handle MQ handle to an MQ (message queue)
 * of that type, such as a POSIX or bipc MQ.  This allows for high-performance, potentially zero-copy (except
 * for copying into the transport MQ) of discrete messages, each containing a binary blob.
 * This is the `sync_io`-pattern counterpart to transport::Blob_stream_mq_sender -- and in fact the latter use an
 * instance of the present class as its core.
 *
 * @see transport::Blob_stream_mq_sender and util::sync_io doc headers.  The latter describes the general pattern which
 *      we implement here; it also contrasts it with the async-I/O pattern, which the former implements.
 *      In general we recommend you use a transport::Blob_stream_mq_sender rather than a `*this` --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * ### Informal comparison to other core transport mechanisms ###
 * Notes for transport::Blob_stream_mq_sender apply.
 *
 * ### Relationship between `Blob_stream_mq_*`, the underlying MQ resource, and its Shared_name ###
 * Notes for transport::Blob_stream_mq_sender apply.
 *
 * ### Cleanup ###
 * Notes for transport::Blob_stream_mq_sender apply.
 *
 * ### Reusing the same absolute_name() for a new MQ and one-direction pipe ###
 * Notes for transport::Blob_stream_mq_sender apply.
 *
 * ### Thread safety ###
 * Notes for transport::Blob_stream_mq_sender apply.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Notes for transport::Blob_stream_mq_sender apply: the pImpl stuff; and the fact that:
 *
 * The rest of the implementation is inside sync_io::Blob_stream_mq_sender_impl and is discussed in that class's
 * doc header.
 *
 * @see sync_io::Blob_stream_mq_sender_impl doc header.
 *
 * @endinternal
 *
 * @see sync_io::Blob_sender: implemented concept.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender : public Blob_stream_mq_base<Persistent_mq_handle>
{
public:
  // Types.

  /// Short-hand for our base with `static` goodies at least.
  using Base = Blob_stream_mq_base<Persistent_mq_handle>;

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = typename Blob_stream_mq_sender_impl<Persistent_mq_handle>::Mq;

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::Blob_stream_mq_sender<Mq>;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  // Constants.

  /// Implements concept API.  Equals `Mq::S_RESOURCE_TYPE_ID`.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// Useful for generic programming: to `false` to indicate a `*this` has no `.send_native_handle()`.
  static constexpr bool S_TRANSMIT_NATIVE_HANDLES = false;

  // Constructors/destructor.

  /**
   * Constructs the sender by taking over an already-opened MQ handle.
   * Note that this op does not implement any concept; Blob_sender concept does not define how a Blob_sender
   * is created in this explicit fashion.
   *
   * All notes from `transport::` counterpart apply.
   *
   * @param logger_ptr
   *        See above.
   * @param mq_moved
   *        See above.
   * @param nickname_str
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Blob_stream_mq_sender(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 Mq&& mq_moved, Error_code* err_code = 0);

  /**
   * Implements Blob_sender API, per its concept contract.
   * All the notes for that concept's default ctor apply.
   *
   * @see Blob_sender::Blob_sender(): implemented concept.
   */
  Blob_stream_mq_sender();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Implements Blob_sender API, per its concept contract.
   *
   * @param src
   *        See above.
   *
   * @see Blob_sender::Blob_sender(): implemented concept.
   */
  Blob_stream_mq_sender(Blob_stream_mq_sender&& src);

  /// Copy construction is disallowed.
  Blob_stream_mq_sender(const Blob_stream_mq_sender&) = delete;

  /**
   * Implements Blob_sender API.  All notes from `transport::` counterpart apply.
   *
   * @see Blob_sender::~Blob_sender(): implemented concept.
   */
  ~Blob_stream_mq_sender();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   * Implements Blob_sender API, per its concept contract.
   *
   * @see ~Blob_stream_mq_sender().
   *
   * @param src
   *        See above.
   * @return `*this` (see concept API).
   *
   * @see Blob_sender move assignment: implemented concept.
   */
  Blob_stream_mq_sender& operator=(Blob_stream_mq_sender&& src);

  /// Copy assignment is disallowed.
  Blob_stream_mq_sender& operator=(const Blob_stream_mq_sender&) = delete;

  /**
   * Returns logger (possibly null).
   * @return See above.
   */
  flow::log::Logger* get_logger() const;

  /**
   * Implements Blob_sender API per contract.
   *
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.  See above.
   *
   * @see Blob_sender::replace_event_wait_handles(): implemented concept.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Implements Blob_sender API per contract.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   *
   * @see Blob_sender::start_send_blob_ops(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_send_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Blob_sender API per contract.  Note this value equals "remote" peer's value for the same call
   * at any given time which is *not* a concept requirement and may be untrue of other concept co-implementing classes.
   *
   * @return See above.
   *
   * @see Blob_sender::send_blob_max_size(): implemented concept.
   */
  size_t send_blob_max_size() const;

  /**
   * Implements Blob_sender API per contract.  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods or destructor on the same `*this`.
   *
   * Reminder: `blob.size() == 0` results in undefined behavior (assertion may trip).
   *
   * @param blob
   *        See above.  Reminder: The memory area described by this arg need only be valid until this
   *        method returns.  Perf reminder: That area will not be copied except for rare circumstances.
   * @param err_code
   *        See above.  Reminder: In rare circumstances, an error emitted here may represent something
   *        detected during handling of a *preceding* send_blob() call but after it returned.
   *        #Error_code generated: See #Async_io_obj counterpart doc header.
   * @return See above.
   *
   * @see Blob_sender::send_blob(): implemented concept.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Implements Blob_sender API per contract.
   * Reminder: It's not thread-safe to call this concurrently with other transmission methods or destructor on
   * the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See #Async_io_obj counterpart doc header (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`).
   *
   * Reminder: In rare circumstances, an error emitted there may represent something
   * detected during handling of a preceding send_blob() call but after it returned.
   *
   * @tparam Task_err
   *         See above.
   * @param sync_err_code
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.  Reminder: If and only if it returns `false`, we're in NULL state, or `*end_sending()` has
   *         already been called; and `on_done_func()` will never be called, nor will an error be emitted.
   *
   * @see Blob_sender::async_end_sending(): implemented concept.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Implements Blob_sender API per contract.  Reminder: It is equivalent to async_end_sending()
   * but with a no-op `on_done_func`.
   *
   * @return See above.  Reminder: If and only if it returns `false`, we're in NULL state, or `*end_sending()` has
   *         already been called.
   *
   * @see Blob_sender::end_sending(): implemented concept.
   */
  bool end_sending();

  /**
   * Implements Blob_sender API per contract.
   *
   * @param period
   *        See above.
   * @return See above.
   *
   * @see Blob_sender::auto_ping(): implemented concept.
   */
  bool auto_ping(util::Fine_duration period = boost::chrono::seconds(2));

  /**
   * Returns nickname, a brief string suitable for logging.  This is included in the output by the `ostream<<`
   * operator as well.  This method is thread-safe in that it always returns the same value.
   *
   * If this object is default-cted (or moved-from), this will return a value equal to "".
   *
   * @return See above.
   */
  const std::string& nickname() const;

  /**
   * Returns name equal to `mq.absolute_name()`, where `mq` was passed to ctor, at the time it was passed to ctor.
   *
   * If this object is default-cted (or moved-from), this will return Shared_name::S_EMPTY.
   *
   * @return See above.  Always the same value except across move-assignment.
   */
  const Shared_name& absolute_name() const;

private:
  // Types.

  /// Short-hand for `const`-respecting wrapper around Blob_stream_mq_sender_impl for the pImpl idiom.
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Blob_stream_mq_sender_impl<Mq>>>;

  // Friends.

  /// Friend of Blob_stream_mq_sender.
  template<typename Persistent_mq_handle2>
  friend std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle2>& val);

  // Data.

  /// The true implementation of this class.  See also our class doc header.
  Impl_ptr m_impl;
}; // class Blob_stream_mq_sender

// Free functions: in *_fwd.hpp.

// Template initializers.

template<typename Persistent_mq_handle>
const Shared_name Blob_stream_mq_sender<Persistent_mq_handle>::S_RESOURCE_TYPE_ID = Mq::S_RESOURCE_TYPE_ID;

// Template implementations (strict pImpl-idiom style (albeit pImpl-lite due to template-ness)).

// The performant move semantics we get delightfully free with pImpl; they'll just move-to/from the unique_ptr m_impl.

template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::Blob_stream_mq_sender(Blob_stream_mq_sender&&) = default;
template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>& Blob_stream_mq_sender<Persistent_mq_handle>::operator=
                                               (Blob_stream_mq_sender&&) = default;

// The NULL state ctor comports with how null m_impl is treated all over below.
template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::Blob_stream_mq_sender() = default;

// The rest is strict forwarding to m_impl, once PEER state is established (non-null m_impl).

template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::Blob_stream_mq_sender
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq, Error_code* err_code) :
  m_impl(boost::movelib::make_unique<Blob_stream_mq_sender_impl<Mq>>
           (logger_ptr, nickname_str, std::move(mq), err_code))
{
  // Yay.
}

// It's only explicitly defined to formally document it.
template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::~Blob_stream_mq_sender() = default;

template<typename Persistent_mq_handle>
template<typename Create_ev_wait_hndl_func>
bool Blob_stream_mq_sender<Persistent_mq_handle>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  return m_impl ? m_impl->replace_event_wait_handles(create_ev_wait_hndl_func)
                : false;
}

template<typename Persistent_mq_handle>
template<typename Event_wait_func_t>
bool Blob_stream_mq_sender<Persistent_mq_handle>::start_send_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  return m_impl ? m_impl->start_send_blob_ops(std::move(ev_wait_func))
                : false;
}

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_sender<Persistent_mq_handle>::send_blob_max_size() const
{
  return m_impl ? m_impl->send_blob_max_size() : 0;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender<Persistent_mq_handle>::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  return m_impl ? m_impl->send_blob(blob, err_code) : false;
}

template<typename Persistent_mq_handle>
template<typename Task_err>
bool Blob_stream_mq_sender<Persistent_mq_handle>::async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func)
{
  return m_impl ? m_impl->async_end_sending(sync_err_code, std::move(on_done_func)) : false;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender<Persistent_mq_handle>::end_sending()
{
  return m_impl ? m_impl->end_sending() : false;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender<Persistent_mq_handle>::auto_ping(util::Fine_duration period)
{
  return m_impl ? m_impl->auto_ping(period) : false;
}

template<typename Persistent_mq_handle>
const Shared_name& Blob_stream_mq_sender<Persistent_mq_handle>::absolute_name() const
{
  return m_impl ? m_impl->absolute_name() : Shared_name::S_EMPTY;
}

template<typename Persistent_mq_handle>
const std::string& Blob_stream_mq_sender<Persistent_mq_handle>::nickname() const
{
  return m_impl ? m_impl->nickname() : util::EMPTY_STRING;
}

template<typename Persistent_mq_handle>
flow::log::Logger* Blob_stream_mq_sender<Persistent_mq_handle>::get_logger() const
{
  return m_impl ? m_impl->get_logger() : nullptr;
}

// `friend`ship needed for this "non-method method":

template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle>& val)
{
  if (val.m_impl)
  {
    return os << *val.m_impl;
  }
  // else
  return os << "null";
}

} // namespace ipc::transport::sync_io
