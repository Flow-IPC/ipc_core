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
#include "ipc/transport/blob_transport_stats.hpp"
#include <cassert>

namespace ipc::transport::stat
{

// Local helpers.

namespace
{

/* (File-local/internal-use) Constructs a payload-size histogram whose last bucket isolates exactly max_payload_size.
 * Bucket 0 absorbs the remainder when max_payload_size is not divisible by bucket_sz,
 * so that subsequent bucket boundaries align and one falls exactly on max_payload_size.
 * When divisible, bucket 0 is the same width as the rest (uniform). */
flow::util::stat::Histogram_counter make_payload_sz_histo(size_t max_payload_size, size_t bucket_sz)
{
  assert((max_payload_size >= 1) && "max_payload_size must be >= 1.");

  const auto remainder = max_payload_size % bucket_sz;
  const auto bucket0_sz = (remainder == 0) ? bucket_sz : remainder;

  return flow::util::stat::Histogram_counter{size_t(2) + ((max_payload_size - size_t(1)) / bucket_sz),
                                             int64_t(bucket0_sz),
                                             int64_t(bucket_sz), 0};
}

} // namespace (anon)

// Implementations.

Blob_snd_stats::Blob_snd_stats(size_t max_payload_size) :
  m_histo_payload_sz(make_payload_sz_histo(max_payload_size, S_PAYLOAD_SZ_HISTO_BUCKET_SZ))
{
  // Yep.
}

const Blob_snd_stats& blob_snd_stats(const Blob_snd_stats& stats)
{
  return stats;
}

Blob_rcv_stats::Blob_rcv_stats(size_t max_payload_size) :
  m_histo_payload_sz(make_payload_sz_histo(max_payload_size, S_PAYLOAD_SZ_HISTO_BUCKET_SZ))
{
  // Yep.
}

const Blob_rcv_stats& blob_rcv_stats(const Blob_rcv_stats& stats)
{
  return stats;
}


} // namespace ipc::transport::stat
