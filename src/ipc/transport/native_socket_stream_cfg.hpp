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

#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include <flow/log/log_fwd.hpp>
#include <type_traits>

namespace ipc::transport
{

// Types.

/**
 * Grouping of compile-type config/knobs (such as type aliases, constants) governing some of the behavior of
 * Native_socket_stream and sync_io::Native_socket_stream, the latter in particular being a core IPC transport
 * in Flow-IPC.
 *
 * ### Rationale ###
 * Some or all of these definitions can be viewed as implementation details, which would normally imply
 * class Native_socket_stream_cfg and everything in it should be in `detail/` and by Flow convention not
 * for public use.  Nevertheless we've kept it all public.  We feel it is harmless to expose such things,
 * and it could be helpful for logging/reporting, as much of it concerns key performance characteristics of
 * arguably *the* core low-level IPC transport in Flow-IPC.
 */
class Native_socket_stream_cfg
{
public:
  // Types.

  /**
   * The type used to encode the internal meta-blob length/type-indicator for sync_io::Native_socket_stream and its
   * derivative(s).
   *
   * @todo Native_socket_stream_cfg::low_lvl_payload_blob_length_t should arguably be renamed, since it represents
   * a length (when not representing a message-type) only if #S_USE_OS_DGRAM_SUPPORT is `false`; otherwise
   * it always represents a message-type only.  We have kept the existing name arguably form legacy inertia and/or
   * because a better name was not immediately forthcoming.
   *
   * If #S_USE_OS_DGRAM_SUPPORT is `false`: this puts a cap on how long the meta-blobs can be, and as of this writing
   * it width and sign happen to be chosen (at 2-bytes, unsigned), because ~64Ki is a pretty reasonable max
   * meta-blob size; arguably users should not be sending larger messages than that.
   *
   * If `S_USE_OS_DGRAM_SUPPORT == true`: this no longer dictates any cap and can be really any unsigned type.
   * As of this writing we've decided to use the same type, basically, for simplicity/consistency.  Further: a larger
   * width seems an unnecessary use of space; while using only 1 byte "feels" like it could constrain future
   * extensions.  (Though, perhaps something equalling a native word, for some definition of word, would yield
   * somewhat better perf somewhere?  Look into it?  Not an official to-do yet.)
   */
  using low_lvl_payload_blob_length_t = uint16_t;
  static_assert(std::numeric_limits<low_lvl_payload_blob_length_t>::is_integer
                  && (!std::numeric_limits<low_lvl_payload_blob_length_t>::is_signed),
                "low_lvl_payload_blob_length_t is at least in some cases used as a length type, "
                  "so it must be an unsigned integer of some width.");

  // (Contant-dependent type(s) below.)

  // Constants.

#ifndef FLOW_OS_LINUX
static_assert(false, "Native_socket_stream uses AF_LOCAL/SOCK_SEQPACKET and relies on Linux semantics for it; "
                       "this may be available in other OS with alleged spotty support, and we have not tested it "
                       "elsewhere.  For now build in Linux only.");
#endif
  /**
   * Whether the OS provides, and we shall use for sync_io::Native_socket_stream and derivative(s),
   * Unix domain sockets of sub-type `SOCK_SEQPACKET` (connection-oriented, message boundary-preserving);
   * if `true` then yes; if `false` then only `SOCK_STREAM` (connection-oriented, message boundary-non-preserving)
   * shall be used instead.
   *
   * For Linux of any reasonably recent vintage this is `true`.
   *
   * @todo Native_socket_stream_cfg::S_USE_OS_DGRAM_SUPPORT and Native_socket_stream_cfg::S_USE_OS_DGRAM_BATCH_SUPPORT
   * is each currently determined based on availability in the *compiling* host OS; but for various
   * reasons (both flexibility/testing and compatibility with other target platforms
   * and versions) it might be helpful to make each configurable via the Flow-IPC build script(s).
   */
  static constexpr bool S_USE_OS_DGRAM_SUPPORT = true;

  /**
   * Whether the OS provides, and we shall use for sync_io::Native_socket_stream and derivative(s),
   * batch-receiving of multiple Unix domain socket *datagrams* in a single native OS-call.
   * If `true` then yes; if `false` then only single-message OS-calls shall be used instead, even if
   * one uses sync_io::Native_socket_stream::async_receive_blob_batch() and similar/derived APIs.  In the latter
   * case Flow-IPC code shall emulate batch-transmission via potentially multiple regular one-message OS-calls.
   *
   * Meaningful only if #S_USE_OS_DGRAM_SUPPORT is `true`.  (Code internally shall check #S_USE_OS_DGRAM_SUPPORT
   * and only if `true` then check this value.)
   *
   * For Linux of any reasonably recent vintage this is `true`.
   */
  static constexpr bool S_USE_OS_DGRAM_BATCH_SUPPORT = true;

  /**
   * For sync_io::Native_socket_stream and derivative(s):
   * Value for the length/type-indicator field in each internal message's header (internally known as payload 1)
   * that means "not a length; indicating this is a ping message" (if #S_USE_OS_DGRAM_SUPPORT is `false`) or
   * just "this is a ping message" (otherwise).
   *
   * The other special value is 0 which indicates "graceful close, or no meta-blob" (depending on whether
   * a native handle is attached).
   */
  static constexpr low_lvl_payload_blob_length_t S_PING_SENTINEL
    = std::numeric_limits<low_lvl_payload_blob_length_t>::max();

  /**
   * The maximum length of a blob that can be sent by the protocol internally used by sync_io::Native_socket_stream
   * and derivative(s).  Native_socket_stream::send_native_handle() (and similar) shall synchronously emit a
   * particular error, if `[meta_]blob.size()` exceeds this.
   *
   * As alluded to in #low_lvl_payload_blob_length_t doc header: The choice of value follows somewhat divergent
   * reasoning depending on #S_USE_OS_DGRAM_SUPPORT.  If `false` then we essentially use the highest possible value;
   * since #S_PING_SENTINEL exists, that eliminates the highest possible value, so we use the next-highest.
   *
   * If #S_USE_OS_DGRAM_SUPPORT is `true`, though, we can use anything here that the OS shall accept without
   * yielding a message-too-large (`EMSGSIZE` in Linux) in sending or receiving.  As of this writing modern
   * Linuxes happen to accept that same value (roughly), namely 64Ki, and since we chose #low_lvl_payload_blob_length_t
   * partially because its max-value is a reasonable limit on message size (subjectively), we shall use the same
   * value here (actually a couple bytes more for roundness).
   *
   * @warning I (ygoldfel) tested out `SOCK_SEQPACKET` (also `SOCK_DGRAM` which behaved identically) with a fairly
   * modern Linux distro configured normally; this showed the limit was well above 128Ki but below 256Ki.
   * This suggests our chosen value here may even be larger than what is allowed in reality depending on the host OS.
   * If this occurs, it shall exhibit as follows: Our APIs won't throw an error when pre-checking the receive-buffer
   * size in `async_receive_*()`; but then later the given Native_socket_stream shall get hosed due to an OS
   * message-too-large, if indeed a too-large message manifests in your application.
   */
  static constexpr size_t S_MAX_META_BLOB_LENGTH = S_USE_OS_DGRAM_SUPPORT
                                                     ? 0x10000
                                                     : (S_PING_SENTINEL - 1);

  /**
   * Whether to periodically log various statistics about Native_socket_stream.  Stats are always collected
   * regardless of this setting; this controls only whether they are logged.  See #S_STATS_LOG_SEV and
   * #S_STATS_LOG_PERIOD for further knobs.
   */
  static constexpr bool S_STATS_LOG_ENABLED = false;

  /// If #S_STATS_LOG_ENABLED: the log severity for stats output.
  static constexpr flow::log::Sev S_STATS_LOG_SEV = flow::log::Sev::S_INFO;

  /// If #S_STATS_LOG_ENABLED: how often to output stats via logs.
  static constexpr util::Fine_duration S_STATS_LOG_PERIOD = boost::chrono::seconds{5};

  /**
   * For batch-receiving event-count histogram: the width of each event-count bucket.
   *
   * By example: consider the histogram for how many messages were received in each batch-receive.  Suppose
   * max batch size is 64.  For each batch-receive, we can get either 0 (would-block) messages or 1 or 2 or ...
   * or 64.  There is always a bucket for 0, and then a bunch of evenly-sized buckets.  Then, suppose
   * `S_STATS_HISTO_MSG_CT_BUCKET_SZ == 2`.  The next bucket would be for getting 1 or 2 messages; then for 3 or 4;
   * etc., up to the bucket including 64.
   *
   * The value 1 is pretty good, as each possible outcome is separately tracked.  Plus it is interesting when
   * *any* batching (2+) occurred versus none (1).
   */
  static constexpr size_t S_STATS_HISTO_MSG_CT_BUCKET_SZ = 1;

  // Constant-dependent types.

  /**
   * sync_io::Native_socket_stream and derivative(s) shall internally use a boost.asio socket type of this
   * boost.asio protocol, as determined by #S_USE_OS_DGRAM_SUPPORT.
   */
  using Protocol = std::conditional_t<S_USE_OS_DGRAM_SUPPORT, asio_local_stream_socket::Protocol_pkt_stream,
                                                              asio_local_stream_socket::Protocol_byte_stream>;

private:
  // Constructors/destructor.

  /// Forbid instantiation, for it is pointless.
  Native_socket_stream_cfg() = delete;
}; // class Native_socket_stream_cfg

} // namespace ipc::transport
