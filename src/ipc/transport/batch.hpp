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

#include <vector>
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport
{

// Types.

/**
 * Straightforward Msg_batch_in concept implementation (as taken by
 * API `{Blob|Native_handle}_receiver::async_receive_*_batch()` concept) usable by any receiver
 * engaging in *emulated batching*.
 *
 * @see Native_handle_receiver concept doc header for an explantion of receive-batching (including what
 *      emulated batching is; and how `Msg_resource_t` template-param relates to things).
 *
 * That is it features the following APIs as of this writing:
 *
 *   - For user (including internal user struc::sync_io::Channel): `size_t max_msg_count`-taking ctor,
 *     initialized(), n_used(), full(), clear_used(), prepare_target_payload(), target_payload_size(),
 *     result_payload_blob(), result_payload_hndl() (for Native_handle_receiver only).
 *   - For sync_io::async_receive_batch_emulation() or equivalents: next_target_blob(), next_target_hndl() (for
 *     Native_handle_receiver only), emulate_result().
 *     - Subtlety: While result_payload_blob() and result_payload_hndl(), where arg `idx >= this->n_used()`,
 *       formally results in undefined behavior, Generic_msg_batch_in intentionally relaxes this restriction;
 *       values up to (not including) `max_msg_count` are allowed.  The intention here is to allow a `*this`
 *       to be usable even without emulate_result(), or n_used() for that matter.  (The
 *       first use-case for this was an internal scenario in struc::sync_io::Channel, when batch-size is set to 1
 *       at compile-time.  Then a simplified code-path inside `Channel` stores a "batch" with `max_msg_count == 1`,
 *       then uses one-message-at-a-time receiving to target next_target_blob() and next_target_hndl() and always
 *       leaves `n_used() == 0`.  (A true-batch-handling code-path still relies on a batch being stored in that
 *       data structure but uses actual batched-receiving on it.)
 *
 * ### Why are `next_target_*()` and emulate_result() (and `idx >= n_used()`) public? ###
 * Please note that we have intentionally made the receiver-engine-facing API publicly accessible.  The idea is
 * that conceivably the Flow-IPC user could make use of it, as it is fairly general.  As of this writing, however,
 * only other Flow-IPC internal code uses this sub-API; but that need not be the case.  This also applies to
 * the aforementioned case where `idx >= this->n_used()` to `result_payload_*(idx)`.
 *
 * ### Impl notes ###
 * It's very straightforward.  The main thing to keep in mind for context is that Generic_msg_batch_in provides
 * value only in helping express batch-receives in terms of single-message-receives.  There is little to nothing
 * algorithmic going on; it just provides the expected batch API for the user (in terms of preparing slots and
 * examining/consuming results) while letting receiver impls straightforwardly record single-message-receives.
 *
 * @tparam Msg_resource_t
 *         See Msg_batch_in concept API.
 * @tparam NO_HNDLS
 *         See Msg_batch_in concept API.
 */
#ifdef IPC_DOXYGEN_ONLY // Mirror the transport_fwd.hpp fwd-declaration in the generated docs.

template<typename Msg_resource_t, bool NO_HNDLS = true>
class Generic_msg_batch_in

#else

template<typename Msg_resource_t, bool NO_HNDLS>
class Generic_msg_batch_in

#endif
  : private boost::noncopyable
{
public:
  // Types.

  /// See Msg_batch_in concept API.
  using Msg_resource = Msg_resource_t;

  /// See Msg_batch_in concept API.
  using Mutable_buffer_sequence = util::Blob_mutable;

  // Constants.

  /// See Msg_batch_in concept API.
  static constexpr bool S_NO_HNDLS = NO_HNDLS;

  // Constructors/destructor.

  /**
   * See Msg_batch_in concept API.
   *
   * @param max_msg_count
   *        See above.
   */
  explicit Generic_msg_batch_in(size_t max_msg_count);

#ifdef IPC_DOXYGEN_ONLY
  /// See Msg_batch_in concept API.
  ~Generic_msg_batch_in();
#endif

  // Methods.

  /**
   * See Msg_batch_in concept API.
   * @return See above.
   */
  bool initialized() const;

  /**
   * See Msg_batch_in concept API.
   * @return See above.
   */
  size_t n_used() const;

  /**
   * See Msg_batch_in concept API.
   * @return See above.
   */
  bool full() const;

  /// See Msg_batch_in concept API.
  void clear_used();

  /**
   * See Msg_batch_in concept API.
   *
   * @param target_blob
   *        See above.
   * @param msg_resource
   *        See above.
   * @param idx
   *        See above.
   */
  void prepare_target_payload(const Mutable_buffer_sequence& target_blob, Msg_resource&& msg_resource,
                              size_t idx = -1);

  /**
   * See Msg_batch_in concept API.
   * @return See above.
   */
  size_t target_payload_size() const;

  /**
   * See Msg_batch_in concept API; plus additional behavior for arg `idx` (allowed to be `>= n_used()`).
   *
   * @param idx
   *        Which slot?  This *may* `>= n_used()` for certain advanced use cases sans emulate_result();
   *        otherwise (informally) it should be `< n_used()` (but this is not checked).  Be careful.
   *        See class doc header for brief background.
   * @param msg_resource_ptr
   *        See above.
   * @return See above.
   */
  size_t result_payload_blob(size_t idx, Msg_resource** msg_resource_ptr = nullptr);

  /**
   * See Msg_batch_in concept API; plus additional behavior for arg `idx` (allowed to be `>= n_used()`).
   * Exists only if #S_NO_HNDLS is `false`.
   *
   * @param idx
   *        See result_payload_blob().
   * @return See above.
   */
  Native_handle result_payload_hndl(size_t idx) const;

  /**
   * For use by receiver-engine (not receiver-user), makes it so that n_used() would return the given value.  The
   * precipitating use case for this overload was the need for sync_io::async_receive_batch_emulation() to "undo"
   * successful receipt of 1+ messages upon encountering an exceptional error.
   *
   * @param new_n_used
   *        The value n_used() shall return.
   */
  void clear_used(size_t new_n_used);

  /**
   * For use by receiver-engine (not receiver-user), returns the location/size of the target memory area
   * from the last prepare_target_payload() for slot with index `== this->n_used()`.  full() must be `false`;
   * else behavior undefined/assertion may trip.
   *
   * @return See above.
   */
  util::Blob_mutable next_target_blob();

  /**
   * For use by receiver-engine (not receiver-user), returns pointer to the target `Native_handle`
   * for slot with index `== this->n_used()`.  full() must be `false`; else behavior undefined/assertion may trip.
   * Exists only if #S_NO_HNDLS is `false`.
   *
   * @return See above.
   */
  Native_handle* next_target_hndl();

  /**
   * For use by receiver-engine (not receiver-user), records the size of the successful receive at slot
   * with index `== this->n_used()` and increments n_used().  Therefore it registers that the read into
   * area next_target_blob() was successful and yielded `result_n_rcvd` bytes.  At entry full() must be `false`;
   * else behavior undefined/assertion may trip.
   *
   * @note To register a received Native_handle, simply assign to `*(next_target_hndl())` (before emulate_result()).
   *
   * @param result_n_rcvd
   *        How many bytes (it might be zero, depending) were successfully received.
   */
  void emulate_result(size_t result_n_rcvd);

  /**
   * Prints string representation to the given `ostream`.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

private:
  // Types.

  /// Data stored per slot.
  struct Mdt_per_payload
  {
    // Data.

    /// Per-slot resource moved from user's object via `prepare_target_payload()`.
    Msg_resource m_resource;

    /// The target memory location for where to receive-to for this slot.
    Mutable_buffer_sequence m_buf_seq;

    /**
     * The target/result `Native_handle` to receive-to for this slot; null originally; null or not upon
     * a receive op into the slot.
     *
     * Unused (left null) if #S_NO_HNDLS is true.  (It is so small that it didn't seem worthwhile to code actually
     * not having the member in memory in that case.)
     */
    Native_handle m_result_hndl_unless_no_hndls;

    /// The target/result containing the # of bytes successfully read into area #m_buf_seq for this slot.
    size_t m_result_n_rcvd;

    /// Constructors/destructor.

    /**
     * Ctor.
     * @param buf_seq
     *        See #m_buf_seq.
     * @param msg_resource
     *        Object to move-assign to #m_resource.
     */
    Mdt_per_payload(const Mutable_buffer_sequence& buf_seq, Msg_resource&& msg_resource);
  }; // struct Mdt_per_payload

  // Data.

  /// The slots.  `.capacity() == max_msg_count` (see ctor); `.size()` is same once initialized().
  std::vector<Mdt_per_payload> m_batch;

  /// See n_used().
  size_t m_n_used;
}; // class Generic_msg_batch_in

// Template implementations.

template<typename Msg_resource_t, bool NO_HNDLS>
Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::Mdt_per_payload::Mdt_per_payload
  (const Mutable_buffer_sequence& buf_seq, Msg_resource&& msg_resource) :
  m_resource(std::move(msg_resource)),
  m_buf_seq(buf_seq),
  m_result_n_rcvd(0) // Not strictly necessary but to avoid entropy and [sanitizer] warnings....
{
  // Done.
}

template<typename Msg_resource_t, bool NO_HNDLS>
Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::Generic_msg_batch_in(size_t max_msg_count) :
  m_n_used(0)
{
  m_batch.reserve(max_msg_count);
  // It's empty though.  Let them set up each guy via prepare_target_payload(-1) x N times.
  assert((m_batch.capacity() == max_msg_count) && "We use .capacity() to memorize `max_msg_count` for full(), etc.");
}

template<typename Msg_resource_t, bool NO_HNDLS>
void Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::prepare_target_payload
       (const util::Blob_mutable& target_blob, Msg_resource&& msg_resource, size_t idx)
{
  if (target_payload_size() != 0)
  {
    assert((target_payload_size() == target_blob.size())
           && "Msg_batch_in concept requires the max message size be the same across all slots.");
  }

  if (idx == size_t(-1))
  {
    // Ensure .emplace_back() wouldn't increase .capacity().
    assert((!initialized())
           && "At this time the fully-initialized batch size shall be permanently set via ctor; cannot grow.");

    m_batch.emplace_back(target_blob, std::move(msg_resource));
  }
  else if (idx < m_batch.size())
  {
    auto& mdt = m_batch[idx];
    mdt.m_buf_seq = target_blob;
    mdt.m_resource = std::move(msg_resource);
  }
  else // if (idx >= m_mdts.size()) && not -1
  {
    assert(false && "prepare_target_payload(x) must either replace an existing slot x or add slot via `x = -1`.");
  }
}

template<typename Msg_resource_t, bool NO_HNDLS>
size_t Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::target_payload_size() const
{
  return m_batch.empty() ? 0
                         : m_batch.front().m_buf_seq.size();
}

template<typename Msg_resource_t, bool NO_HNDLS>
size_t
  Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::result_payload_blob(size_t idx, Msg_resource** msg_resource_ptr)
{
  assert(idx < m_batch.size());
  // Note: We intentionally allow `idx >= m_n_used` due to advanced use cases; see our doc header.

  auto& mdt = m_batch[idx];
  if (msg_resource_ptr)
  {
    *msg_resource_ptr = &mdt.m_resource;
  }

  return mdt.m_result_n_rcvd;
}

template<typename Msg_resource_t, bool NO_HNDLS>
Native_handle Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::result_payload_hndl(size_t idx) const
{
  static_assert(!S_NO_HNDLS, "Do not invoke/instantiate this method if class tparam NO_HNDLS=true.");

  assert(idx < m_batch.size());
  // Note: We intentionally allow `idx >= m_n_used` due to advanced use cases; see our doc header.

  return m_batch[idx].m_result_hndl_unless_no_hndls;
}

template<typename Msg_resource_t, bool NO_HNDLS>
Native_handle* Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::next_target_hndl()
{
  static_assert(!S_NO_HNDLS, "Do not invoke/instantiate this method if class tparam NO_HNDLS=true.");

  assert(!full());
  return &(m_batch[m_n_used].m_result_hndl_unless_no_hndls);
}

template<typename Msg_resource_t, bool NO_HNDLS>
util::Blob_mutable Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::next_target_blob()
{
  assert(!full());
  return m_batch[m_n_used].m_buf_seq;
}

template<typename Msg_resource_t, bool NO_HNDLS>
bool Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::initialized() const
{
  return m_batch.size() == m_batch.capacity();
}

template<typename Msg_resource_t, bool NO_HNDLS>
size_t Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::n_used() const
{
  return m_n_used;
}

template<typename Msg_resource_t, bool NO_HNDLS>
bool Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::full() const
{
  return m_n_used == m_batch.size();
}

template<typename Msg_resource_t, bool NO_HNDLS>
void Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::clear_used()
{
  clear_used(0);
}

template<typename Msg_resource_t, bool NO_HNDLS>
void Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::clear_used(size_t new_n_used)
{
  m_n_used = new_n_used;
}

template<typename Msg_resource_t, bool NO_HNDLS>
void Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::emulate_result(size_t result_n_rcvd)
{
  assert(initialized() && "By contract must prepare_target_payload() to create all max_msg_count slots.  "
                            "Can check initialized() to ensure that is the case before calling us.");
  assert(!full());

  m_batch[m_n_used].m_result_n_rcvd = result_n_rcvd;
  ++m_n_used;
}

template<typename Msg_resource_t, bool NO_HNDLS>
void Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>::to_ostream(std::ostream* os_ptr) const
{
  auto& os = *os_ptr;
  os << "Gen[slots-total/rdy/rcvd [" << m_batch.size() << '/' << m_batch.capacity() << '/' << m_n_used << "] "
        "blob-sz[" << target_payload_size() << "]]@" << this;
  // We try to keep the various Msg_batch_in-ish variants have similar output format for easy log searching.
}

template<typename Msg_resource_t, bool NO_HNDLS>
std::ostream& operator<<(std::ostream& os,
                         const Generic_msg_batch_in<Msg_resource_t, NO_HNDLS>& val)
{
  val.to_ostream(&os);
  return os;
}

} // namespace ipc::transport
