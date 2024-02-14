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

#include "ipc/transport/sync_io/detail/blob_stream_mq_rcv_impl.hpp"
#include "ipc/transport/blob_stream_mq.hpp"
#include <boost/move/make_unique.hpp>
#include <experimental/propagate_const>

namespace ipc::transport::sync_io
{

// Types.

/**
 * Implements sync_io::Blob_receiver concept by using an adopted Persistent_mq_handle MQ handle to an MQ (message queue)
 * of that type, such as a POSIX or bipc MQ.  This allows for high-performance, potentially zero-copy (except
 * for copying into the transport MQ) of discrete messages, each containing a binary blob.
 * This is the `sync_io`-pattern counterpart to transport::Blob_stream_mq_receiver -- and in fact the latter use an
 * instance of the present class as its core.
 *
 * @see transport::Blob_stream_mq_receiver and util::sync_io doc headers.  The latter describes the general pattern
 *      which we implement here; it also contrasts it with the async-I/O pattern, which the former implements.
 *      In general we recommend you use a transport::Blob_stream_mq_receiver rather than a `*this` --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * ### Informal comparison to other core transport mechanisms ###
 * Notes for transport::Blob_stream_mq_receiver apply.
 *
 * ### Relationship between `Blob_stream_mq_*`, the underlying MQ resource, and its Shared_name ###
 * Notes for transport::Blob_stream_mq_receiver apply.
 *
 * ### Cleanup ###
 * Notes for transport::Blob_stream_mq_receiver apply.
 *
 * ### Reusing the same absolute_name() for a new MQ and one-direction pipe ###
 * Notes for transport::Blob_stream_mq_receiver apply.
 *
 * ### Thread safety ###
 * Notes for transport::Blob_stream_mq_receiver apply.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Notes for transport::Blob_stream_mq_receiver apply: the pImpl stuff; and the fact that:
 *
 * The rest of the implementation is inside sync_io::Blob_stream_mq_receiver_impl and is discussed in that class's
 * doc header.
 *
 * @see sync_io::Blob_stream_mq_receiver_impl doc header.
 *
 * @endinternal
 *
 * @see sync_io::Blob_receiver: implemented concept.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver : public Blob_stream_mq_base<Persistent_mq_handle>
{
public:
  // Types.

  /// Short-hand for our base with `static` goodies at least.
  using Base = Blob_stream_mq_base<Persistent_mq_handle>;

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = typename Blob_stream_mq_receiver_impl<Persistent_mq_handle>::Mq;

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::Blob_stream_mq_receiver<Mq>;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  // Constants.

  /// Implements concept API.  Equals `Mq::S_RESOURCE_TYPE_ID`.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /**
   * Implements concept API; namely it is `false`: async_receive_blob() with smaller-than-`receive_blob_max_size()`
   * size shall lead to error::Code::S_INVALID_ARGUMENT; hence error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE
   * is impossible.
   *
   * @see Blob_receiver::S_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in sister concept class's doc header for potentially important discussion.
   */
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = false;

  /// Useful for generic programming: to `false` to indicate a `*this` has no `.async_receive_native_handle()`.
  static constexpr bool S_TRANSMIT_NATIVE_HANDLES = false;

  // Constructors/destructor.

  /**
   * Constructs the receiver by taking over an already-opened MQ handle.
   * Note that this op does not implement any concept; Blob_receiver concept does not define how a Blob_receiver
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
  explicit Blob_stream_mq_receiver(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                   Mq&& mq_moved, Error_code* err_code = 0);

  /**
   * Implements Blob_receiver API, per its concept contract.
   * All the notes for that concept's default ctor apply.
   *
   * @see Blob_receiver::Blob_receiver(): implemented concept.
   */
  Blob_stream_mq_receiver();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * Implements Blob_receiver API, per its concept contract.
   *
   * @param src
   *        See above.
   *
   * @see Blob_receiver::Blob_receiver(): implemented concept.
   */
  Blob_stream_mq_receiver(Blob_stream_mq_receiver&& src);

  /// Copy construction is disallowed.
  Blob_stream_mq_receiver(const Blob_stream_mq_receiver&) = delete;

  /**
   * Implements Blob_receiver API.  All notes from `transport::` counterpart apply.
   *
   * @see Blob_receiver::~Blob_receiver(): implemented concept.
   */
  ~Blob_stream_mq_receiver();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   * Implements Blob_receiver API, per its concept contract.
   *
   * @see ~Blob_stream_mq_receiver().
   *
   * @param src
   *        See above.
   * @return `*this` (see concept API).
   *
   * @see Blob_receiver move assignment: implemented concept.
   */
  Blob_stream_mq_receiver& operator=(Blob_stream_mq_receiver&& src);

  /// Copy assignment is disallowed.
  Blob_stream_mq_receiver& operator=(const Blob_stream_mq_receiver&) = delete;

  /**
   * Returns logger (possibly null).
   * @return See above.
   */
  flow::log::Logger* get_logger() const;

  /**
   * Implements Blob_receiver API per contract.
   *
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::replace_event_wait_handles(): implemented concept.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Implements Blob_receiver API per contract.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::start_receive_blob_ops(): implemented concept.
   */
  template<typename Event_wait_func_t>
  bool start_receive_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Implements Blob_receiver API per contract.  Note this value equals "remote" peer's value for the same call
   * at any given time which is *not* a concept requirement and may be untrue of other concept co-implementing classes.
   *
   * @return See above.
   *
   * @see Blob_receiver::receive_blob_max_size(): implemented concept.
   */
  size_t receive_blob_max_size() const;

  /**
   * Implements Blob_receiver API per contract.
   *
   * #Error_code generated and passed to `on_done_func()` or emitted synchronously:
   * See #Async_io_obj counterpart doc header (but not `S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`).
   *
   * @param target_blob
   *        See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::async_receive_blob(): implemented concept.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob, Error_code* sync_err_code, size_t* sync_sz,
                          Task_err_sz&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.
   *
   * @param timeout
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::idle_timer_run(): implemented concept.
   */
  bool idle_timer_run(util::Fine_duration timeout);

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

  /// Short-hand for `const`-respecting wrapper around Blob_stream_mq_receiver_impl for the pImpl idiom.
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Blob_stream_mq_receiver_impl<Mq>>>;

  // Friends.

  /// Friend of Blob_stream_mq_receiver.
  template<typename Persistent_mq_handle2>
  friend std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle2>& val);

  // Data.

  /// The true implementation of this class.  See also our class doc header.
  Impl_ptr m_impl;
}; // class Blob_stream_mq_receiver

// Free functions: in *_fwd.hpp.

// Template initializers.

template<typename Persistent_mq_handle>
const Shared_name Blob_stream_mq_receiver<Persistent_mq_handle>::S_RESOURCE_TYPE_ID = Mq::S_RESOURCE_TYPE_ID;

// Template implementations (strict pImpl-idiom style (albeit pImpl-lite due to template-ness)).

// The performant move semantics we get delightfully free with pImpl; they'll just move-to/from the unique_ptr m_impl.

template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::Blob_stream_mq_receiver(Blob_stream_mq_receiver&&) = default;
template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>& Blob_stream_mq_receiver<Persistent_mq_handle>::operator=
                                                 (Blob_stream_mq_receiver&&) = default;

// The NULL state ctor comports with how null m_impl is treated all over below.
template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::Blob_stream_mq_receiver() = default;

// The rest is strict forwarding to m_impl, once PEER state is established (non-null m_impl).

template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::Blob_stream_mq_receiver
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq, Error_code* err_code) :
  m_impl(boost::movelib::make_unique<Blob_stream_mq_receiver_impl<Mq>>
           (logger_ptr, nickname_str, std::move(mq), err_code))
{
  // Yay.
}

// It's only explicitly defined to formally document it.
template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::~Blob_stream_mq_receiver() = default;

template<typename Persistent_mq_handle>
template<typename Create_ev_wait_hndl_func>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  return m_impl ? m_impl->replace_event_wait_handles(create_ev_wait_hndl_func)
                : false;
}

template<typename Persistent_mq_handle>
template<typename Event_wait_func_t>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::start_receive_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  return m_impl ? m_impl->start_receive_blob_ops(std::move(ev_wait_func))
                : false;
}

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_receiver<Persistent_mq_handle>::receive_blob_max_size() const
{
  return m_impl ? m_impl->receive_blob_max_size() : 0;
}

template<typename Persistent_mq_handle>
template<typename Task_err_sz>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::async_receive_blob
       (const util::Blob_mutable& target_blob, Error_code* sync_err_code, size_t* sync_sz, Task_err_sz&& on_done_func)
{
  return m_impl ? m_impl->async_receive_blob(target_blob, sync_err_code, sync_sz, std::move(on_done_func)) : false;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::idle_timer_run(util::Fine_duration timeout)
{
  return m_impl ? m_impl->idle_timer_run(timeout) : false;
}

template<typename Persistent_mq_handle>
const Shared_name& Blob_stream_mq_receiver<Persistent_mq_handle>::absolute_name() const
{
  return m_impl ? m_impl->absolute_name() : Shared_name::S_EMPTY;
}

template<typename Persistent_mq_handle>
const std::string& Blob_stream_mq_receiver<Persistent_mq_handle>::nickname() const
{
  return m_impl ? m_impl->nickname() : util::EMPTY_STRING;
}

template<typename Persistent_mq_handle>
flow::log::Logger* Blob_stream_mq_receiver<Persistent_mq_handle>::get_logger() const
{
  return m_impl ? m_impl->get_logger() : nullptr;
}

// `friend`ship needed for this "non-method method":

template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle>& val)
{
  if (val.m_impl)
  {
    return os << *val.m_impl;
  }
  // else
  return os << "null";
}

} // namespace ipc::transport::sync_io
