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

#include "ipc/transport/detail/blob_stream_mq_rcv_impl.hpp"
#include <experimental/propagate_const>

namespace ipc::transport
{

// Types.

/**
 * Implements Blob_receiver concept by using an adopted Persistent_mq_handle MQ handle to an MQ (message queue)
 * of that type, such as a POSIX or bipc MQ.  This allows for high-performance, potentially zero-copy (except
 * for copying out of the transport MQ) of discrete messages, each containing a binary blob.
 * This is a low-level (core) transport mechanism; higher-level (structured)
 * transport mechanisms may use Blob_stream_mq_receiver (and Blob_stream_mq_sender) to enable their work.
 *
 * Beyond that: see Blob_stream_mq_sender doc header.  We are just the other side of this.
 *
 * ### Cleanup; reusing same absolute_name() ###
 * See Blob_stream_mq_sender doc header.  The same applies symmetrically.
 *
 * ### Performance, responsiveness ###
 * See Blob_stream_mq_sender doc header.  The same applies symmetrically.  Be aware specifically that the same
 * responsiveness-of-destructor caveat applies to Blob_stream_mq_receiver.
 *
 * ### Thread safety ###
 * We add no more thread safety guarantees than those mandated by the main concepts.  To wit:
 *   - Concurrent access to 2 separate Blob_stream_mq_receiver objects is safe.
 *   - After construction, concurrent access to the main transmission API methods and the dtor is not safe for a given
 *     `*this`.
 *
 * Methods and situations not covered by that text should have their thread safety explained in their individual doc
 * headers.
 *
 * @internal
 * ### Implementation design/rationale ###
 * See pImpl-lite notes in Blob_stream_mq_sender doc header.  The exact same applies here.
 *
 * @see Blob_stream_mq_receiver_impl doc header.
 *
 * @endinternal
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 *
 * @see Blob_receiver: implemented concept.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver : public Blob_stream_mq_base<Persistent_mq_handle>
{
public:
  // Types.

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = typename Blob_stream_mq_receiver_impl<Persistent_mq_handle>::Mq;

  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Blob_stream_mq_receiver<Mq>;
  /// You may disregard.
  using Async_io_obj = Null_peer;

  // Constants.

  /// Implements concept API.  Equals `Mq::S_RESOURCE_TYPE_ID`.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /**
   * Implements concept API; namely it is `true`.  Notes for transport::Native_socket_stream apply.
   *
   * @see Native_handle_receiver::S_BLOB_UNDERFLOW_ALLOWED: implemented concept.  Accordingly also see
   *      "Blob underflow semantics" in transport::Native_handle_receiver doc header.
   */
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = false;

  // Constructors/destructor.

  /**
   * Constructs the receiver by taking over an already-opened MQ handle.
   * Note that this op does not implement any concept; Blob_receiver concept does not define how a Blob_receiver
   * is created in this explicit fashion.
   *
   * No traffic must have occurred on `mq_moved` up to this call.  Otherwise behavior is undefined.
   *
   * If this fails (sets `*err_code` to truthy if not null; throws if null), all transmission calls on `*this`
   * will fail with the post-value in `*err_code` emitted.  In particular, error::Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS
   * is that code, if the reason for failure was that another Blob_stream_mq_receiver to
   * `mq_moved.absolute_name()` has already been created in this or other process.  See Blob_stream_mq_sender class
   * doc header for discussion of the relationship between Blob_stream_mq_sender, Blob_stream_mq_receiver, the
   * underlying MQ at a given Shared_name, and that Shared_name as registered in the OS.
   * In short: there is to be up to 1 Blob_stream_mq_sender and up to 1 Blob_stream_mq_receiver for a given
   * named persistent MQ.  In this way, it is one single-direction pipe with 2 peers, like half of
   * Native_socket_stream pipe: it is not a MQ with
   * back-and-forth traffic nor multiple senders or multiple receivers.  The underlying MQ supports such things;
   * but that is not what the Blob_sender/Blob_receiver concepts model.
   *
   * Along those same lines note that the dtor (at the latest -- which happens if no fatal error occurs throughout)
   * will not only close the MQ handle acquired from `mq_moved` but will execute `Mq::remove_persistent(name)`,
   * where `name == mq_moved.absolute_name()` pre-this-ctor.
   *
   * ### Leaks of persistent resources ###
   * Notes from Blob_stream_mq_sender ctor doc header apply here.
   *
   * ### Performance ###
   * The taking over of `mq_moved` should be thought of as light-weight.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param mq_moved
   *        An MQ handle to an MQ with no traffic on it so far.  Unless an error is emitted, `mq_moved` becomes
   *        nullified upon return from this ctor.  `*this` owns the MQ handle from this point on and is reponsible
   *        for closing it.
   * @param nickname_str
   *        Human-readable nickname of the new object, as of this writing for use in `operator<<(ostream)` and
   *        logging only.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS (another Blob_stream_mq_receiver exists),
   *        system codes (other errors, all to do with the creation of a separate internally used tiny SHM pool
   *        used to prevent duplicate Blob_stream_mq_receiver in the system).
   */
  explicit Blob_stream_mq_receiver(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                   Mq&& mq_moved, Error_code* err_code = 0);

  /**
   * Implements Blob_receiver API, per its concept contract.
   * All the notes for that concept's core-adopting ctor apply.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See above.
   *
   * @see Blob_receiver::Blob_receiver(): implemented concept.
   */
  explicit Blob_stream_mq_receiver(Sync_io_obj&& sync_io_core_in_peer_state_moved);

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

  /**
   * Implements Blob_receiver API.  All the notes for the concept's destructor apply but as a reminder:
   *
   * Destroys this peer endpoint which will end the one-direction pipe and cancel any pending
   * completion handlers by invoking it ASAP with error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   * As of this writing these are the completion handlers that would therefore be called:
   *   - Any handler passed to async_receive_blob() that has not yet been invoked.
   *     There can be 0 or more of these.
   *
   * ### Fate of underlying MQ and its `.absolute_name()` ###
   * Notes from Blob_stream_mq_sender dtor doc header apply here symmetrically.
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
   * @return `*this`.
   *
   * @see Blob_receiver move assignment: implemented concept.
   */
  Blob_stream_mq_receiver& operator=(Blob_stream_mq_receiver&& src);

  /// Copy assignment is disallowed.
  Blob_stream_mq_receiver& operator=(const Blob_stream_mq_receiver&) = delete;

  /**
   * Implements Blob_receiver API per contract.
   *
   * @return See above.
   *
   * @see Blob_receiver::receive_blob_max_size(): implemented concept.
   */
  size_t receive_blob_max_size() const;

  /**
   * Implements Blob_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier async_receive_blob().  Reminder: It's not thread-safe
   * to call this concurrently with other transmission methods or destructor on the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   * error::Code::S_INVALID_ARGUMENT (non-pipe-hosing error: `target_blob.size()` is smaller than
   * Persistent_mq_handle::max_msg_size()),
   * error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE (peer gracefully closed pipe via `*end_sending()`),
   * other system codes, indicating the underlying transport is hosed for that specific reason, as detected during
   * incoming-direction processing.
   *
   * error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE shall never be emitted.  See
   * Blob_stream_mq_receiver::S_BLOB_UNDERFLOW_ALLOWED.
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_blob
   *        See above.  Reminder: The memory area described by this arg must be valid up to
   *        completion handler entry.
   * @param on_done_func
   *        See above.  Reminder: any moved/copied version of this callback's associated captured state will
   *        be freed soon after it returns.
   * @return See above.
   *
   * @see Blob_receiver::async_receive_blob(): implemented concept.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          Task_err_sz&& on_done_func);

  /**
   * Implements Blob_receiver API per contract.  Reminder: You may call this directly from within a
   * completion handler you supplied to an earlier async_receive_blob().
   *
   * @param timeout
   *        See above.
   * @return See above.
   *
   * @see Blob_receiver::idle_timer_run(): implemented concept.
   */
  bool idle_timer_run(util::Fine_duration timeout = boost::chrono::seconds(5));

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
   * @return See above.  Always the same value.
   */
  const Shared_name& absolute_name() const;

private:
  // Types.

  /// Short-hand for `const`-respecting wrapper around Blob_stream_mq_sender_impl for the pImpl idiom.
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Blob_stream_mq_receiver_impl<Mq>>>;

  // Friends.

  /// Friend of Blob_stream_mq_sender.
  template<typename Persistent_mq_handle2>
  friend std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle2>& val);

  // Methods.

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

template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::Blob_stream_mq_receiver(Sync_io_obj&& sync_io_core_in_peer_state_moved) :

  m_impl(boost::movelib::make_unique<Blob_stream_mq_receiver_impl<Mq>>
           (std::move(sync_io_core_in_peer_state_moved)))
{
  // Yay.
}

// It's only explicitly defined to formally document it.
template<typename Persistent_mq_handle>
Blob_stream_mq_receiver<Persistent_mq_handle>::~Blob_stream_mq_receiver() = default;

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_receiver<Persistent_mq_handle>::receive_blob_max_size() const
{
  return m_impl ? m_impl->receive_blob_max_size() : 0;
}

template<typename Persistent_mq_handle>
template<typename Task_err_sz>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::async_receive_blob
       (const util::Blob_mutable& target_blob, Task_err_sz&& on_done_func)
{
  return m_impl ? (m_impl->async_receive_blob(target_blob, std::move(on_done_func)), true)
                : false;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_receiver<Persistent_mq_handle>::idle_timer_run(util::Fine_duration timeout)
{
  return m_impl ? m_impl->idle_timer_run(timeout)
                : false;
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

} // namespace ipc::transport
