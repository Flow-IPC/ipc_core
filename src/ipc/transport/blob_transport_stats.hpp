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
#include <flow/util/stat/histo.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <cstdint>
#include <string>

namespace ipc::transport::stat
{

// Types.

/**
 * Cumulative stats from the outgoing-direction (send) pipe of a Blob_sender or Native_handle_sender
 * concept implementation.  Applicable to any such implementation; also counts native-handle sends if applicable.
 *
 * @note While internally a payload-to-be-sent may have to be queued on would-block from the low-level transport
 *       (e.g., kernel send buffer full in `Native_socket_stream`), counters like #m_total_msgs and #m_total_bytes
 *       and #m_msgs_with_hndls count the full payload ASAP without waiting for all of it to be "really sent."
 *       Meanwhile #m_would_block_count and #m_snd_q_hi_wmark can give a sense of how often the internal queuing
 *       had to be invoked.
 */
struct Blob_snd_stats
{
  // Constants.

  /// Payload-size histogram bucket width (bytes).  The last bucket isolates the max payload size.
  static constexpr size_t S_PAYLOAD_SZ_HISTO_BUCKET_SZ = 1024;

  // Data.

  /// Total user messages successfully passed to the low-level send path via `send_*()`.
  uint64_t m_total_msgs = 0;

  /// Total user blob bytes sent (sum of blob sizes from `send_*()`).
  uint64_t m_total_bytes = 0;

  /// Total low-level bytes sent (user blob bytes plus protocol headers and framing).
  uint64_t m_total_low_lvl_bytes = 0;

  /// Number of messages sent that carried at least one native handle.  Currently at most 1 handle per message.
  uint64_t m_msgs_with_hndls = 0;

  /// Number of times a would-block condition required payload queueing (low-level transport's internal buffer full).
  uint64_t m_would_block_count = 0;

  /// Number of auto-pings sent (initial from `auto_ping()` plus periodic timer-fired pings).
  uint64_t m_auto_pings = 0;

  /// Current depth of the pending-payloads send queue (queue added-to when would-block occurs).
  size_t m_snd_q_depth = 0;

  /// Max of #m_snd_q_depth observed so far.
  size_t m_snd_q_hi_wmark = 0;

  /**
   * Payload-size histogram for sent user messages.  Maxed-out size gets its own bucket.
   *
   * @todo Consider replacing #m_histo_payload_sz's uniform low end with log-scale buckets (64, 128, ...,
   * 1Ki), if field experience keeps wanting better resolution among small messages: bucket 0 currently
   * swallows all sub-1Ki traffic.  (The latter includes ~all outer messages of SHM-backed *structured* channels;
   * conceivably it would be nice to capture those, for applications that use that feature; which to be clear
   * means (1) using struc::Channel or struc::sync_io::Channel in ther first place; and given that also (2) using
   * the SHM-backed variety as opposed to merely heap-backed.)
   */
  flow::util::stat::Histogram_counter m_histo_payload_sz;

  // Constructors.

  /**
   * Constructs stats with all counters zeroed and a payload-size histogram sized for the given max payload.
   * The last bucket isolates exactly `max_payload_size`.  If `max_payload_size` is not divisible by
   * #S_PAYLOAD_SZ_HISTO_BUCKET_SZ, bucket 0 absorbs the remainder so that subsequent bucket boundaries
   * align and one falls exactly on `max_payload_size`.
   *
   * @param max_payload_size
   *        Maximum blob payload size for this transport (e.g., `Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH`).
   *        Must be >= 1.
   */
  explicit Blob_snd_stats(size_t max_payload_size);
}; // struct Blob_snd_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Blob_snd_stats* src_stats, Blob_snd_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_total_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_total_bytes, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_total_low_lvl_bytes, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_msgs_with_hndls, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_would_block_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_auto_pings, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd_q_depth, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_snd_q_hi_wmark, m_snd_q_depth);
  FLOW_UTIL_STAT_DECLARE(m_histo_payload_sz, ACCUMULATOR);
}

template<typename Stats_t>
Blob_snd_stats& blob_snd_stats_mutable(Stats_t& stats)
{
  return const_cast<Blob_snd_stats&>(blob_snd_stats(static_cast<const Stats_t&>(stats)));
}

// Types.

/**
 * Cumulative stats from the incoming-direction (receive) pipe of a Blob_receiver or Native_handle_receiver
 * concept implementation.  Applicable to any such implementation; also counts native-handle receives if applicable.
 */
struct Blob_rcv_stats
{
  // Constants.

  /// Payload-size histogram bucket width (bytes).  The last bucket isolates the max payload size.
  static constexpr size_t S_PAYLOAD_SZ_HISTO_BUCKET_SZ = 1024;

  // Data.

  /// Total user messages fully received and delivered.
  uint64_t m_total_msgs = 0;

  /// Total user blob bytes received (sum of in-message blob sizes).
  uint64_t m_total_bytes = 0;

  /// Total low-level bytes received (user blob bytes plus protocol headers and framing).
  uint64_t m_total_low_lvl_bytes = 0;

  /// Number of messages received that carried at least one native handle.  Currently at most 1 handle per message.
  uint64_t m_msgs_with_hndls = 0;

  /// Number of auto-pings received from the opposing sender.
  uint64_t m_auto_pings = 0;

  /// Number of idle-timeout events.  For a {Blob|Native}_handle_receiver as of this writing this cannot exceed 1.
  uint64_t m_idle_timeouts = 0;

  /**
   * Payload-size histogram for received user messages.  Maxed-out size gets its own bucket.
   * The to-do on Blob_snd_stats::m_histo_payload_sz applies equally here.
   */
  flow::util::stat::Histogram_counter m_histo_payload_sz;

  // Constructors.

  /**
   * Constructs stats with all counters zeroed and a payload-size histogram sized for the given max payload.
   * The last bucket isolates exactly `max_payload_size`.  If `max_payload_size` is not divisible by
   * #S_PAYLOAD_SZ_HISTO_BUCKET_SZ, bucket 0 absorbs the remainder so that subsequent bucket boundaries
   * align and one falls exactly on `max_payload_size`.
   *
   * @param max_payload_size
   *        Maximum blob payload size for this transport (e.g., `Native_socket_stream_cfg::S_MAX_META_BLOB_LENGTH`).
   *        Must be >= 1.
   */
  explicit Blob_rcv_stats(size_t max_payload_size);
}; // struct Blob_rcv_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Blob_rcv_stats* src_stats, Blob_rcv_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_total_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_total_bytes, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_total_low_lvl_bytes, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_msgs_with_hndls, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_auto_pings, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_idle_timeouts, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_histo_payload_sz, ACCUMULATOR);
}

template<typename Stats_t>
Blob_rcv_stats& blob_rcv_stats_mutable(Stats_t& stats)
{
  return const_cast<Blob_rcv_stats&>(blob_rcv_stats(static_cast<const Stats_t&>(stats)));
}

} // namespace ipc::transport::stat
