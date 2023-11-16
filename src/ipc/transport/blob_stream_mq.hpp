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

#include "ipc/transport/detail/blob_stream_mq_impl.hpp"

namespace ipc::transport
{

// Types.

/**
 * Base of Blob_stream_mq_sender and Blob_stream_mq_receiver containing certain `static` facilities, particularly
 * for post-abort persistent resource cleanup.  As of this writing this type stores no data.
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_base
{
public:
  // Methods.

  /**
   * Removes the Blob_stream_mq_sender and/or Blob_stream_mq_receiver persistent resources associated with
   * a Persistent_mq_handle with the given name.  Please see Cleanup in Blob_stream_mq_sender doc header for
   * context; and note it applies equally to Blob_stream_mq_receiver.  In short: Those classes gracefully
   * clean up by themselves, via their dtors; but an abort (or two) may prevent this from occurring -- in which
   * case one should do one of the following:
   *   - If the name of the potentially-leaked MQ is known: Call this function, remove_persistent().
   *   - If not, but you used a name-prefix-based scheme: Call
   *     `remove_each_persistent_with_name_prefix<Blob_stream_mq_base>()`, providing the applicable name prefix.
   *     It will invoke remove_persistent() (and for_each_persistent()) internally as needed.
   *
   * Trying to remove a non-existent name *is* an error; but typically one should ignore any error, since the
   * function is meant for best-effort cleanup in case a leak occurred earlier due to abort(s).
   *
   * Logs INFO message.
   *
   * ### Rationale ###
   * Why does this exist, when Persistent_mq_handle::remove_persistent() is available?  Answer:
   * Blob_stream_mq_base sub-classes require certain internal resources (tiny SHM pools as of this writing)
   * to ensure a single sender and single receiver per MQ (a limitation not imposed by the lower-level
   * Persistent_mq_handle construct).
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param name
   *        Absolute name at which the persistent MQ (fed to Blob_stream_mq_sender and/or
   *        Blob_stream_mq_receiver ctor) lives.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& name,
                                Error_code* err_code = 0);

  /**
   * Forwards to Persistent_mq_handle::for_each_persistent().  In practice this exists merely to enable
   * templates remove_each_persistent_with_name_prefix() and remove_each_persistent_if() to work with
   * Blob_stream_mq_base.
   *
   * @tparam Handle_name_func
   *         See Persistent_mq_handle::for_each_persistent().
   * @param handle_name_func
   *        See Persistent_mq_handle::for_each_persistent().
   */
  template<typename Handle_name_func>
  static void for_each_persistent(const Handle_name_func& handle_name_func);
}; // class Blob_stream_mq_base

// Template implementations.

template<typename Persistent_mq_handle>
void Blob_stream_mq_base<Persistent_mq_handle>::remove_persistent(flow::log::Logger* logger_ptr, // Static.
                                                                  const Shared_name& name, Error_code* err_code)
{
  Blob_stream_mq_base_impl<Persistent_mq_handle>::remove_persistent(logger_ptr, name, err_code);
}

template<typename Persistent_mq_handle>
template<typename Handle_name_func>
void // Static.
  Blob_stream_mq_base<Persistent_mq_handle>::for_each_persistent(const Handle_name_func& handle_name_func)
{
  Blob_stream_mq_base_impl<Persistent_mq_handle>::for_each_persistent(handle_name_func);
}

} // namespace ipc::transport
