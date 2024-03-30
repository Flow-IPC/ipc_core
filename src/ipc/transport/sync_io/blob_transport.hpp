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

// Not compiled: for documentation only.  Contains concept docs as of this writing.
#ifndef IPC_DOXYGEN_ONLY
static_assert(false, "As of this writing this is a documentation-only \"header\" "
                       "(the \"source\" is for humans and Doxygen only).");
#else // ifdef IPC_DOXYGEN_ONLY

namespace ipc::transport::sync_io
{

// Types.

/**
 * A documentation-only *concept*: what transport::Blob_sender is to transport::Native_handle_sender (namely a
 * degenerate version thereof), this is to sync_io::Native_handle_sender.
 *
 * The concept is exactly identical to sync_io::Native_handle_sender except that in the latter each message consists
 * of 0-1 native handles and 0-1 binary blobs; whereas here it is always exactly 1 of the latter.  More precisely,
 * each message contains 1 binary blob, whose length must be at least 1 byte.
 *
 * Therefore we keep the documentation very light, only pointing out the differences against
 * sync_io::Native_handle_sender -- which are summarized in the preceding paragraph.  All other notes from
 * sync_io::Native_handle_sender apply here.
 *
 * @see sync_io::Native_handle_sender, an identical concept with 1 feature (native handle transmission) added.
 */
class Blob_sender
{
public:
  // Constants.

  /// Same notes as for transport::Blob_sender.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   *
   * All notes from sync_io::Native_handle_sender apply.
   */
  Blob_sender();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @param src
   *        See above.
   */
  Blob_sender(Blob_sender&& src);

  /// Disallow copying.
  Blob_sender(const Blob_sender&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it's
   * still active) and return resources to OS as applicable.
   *
   * All notes from sync_io::Native_handle_sender apply.
   */
  ~Blob_sender();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @param src
   *        See above.
   * @return `*this`.
   */
  Blob_sender& operator=(Blob_sender&& src);
  // Methods.

  /// Disallow copying.
  Blob_sender& operator=(const Blob_sender&) = delete;

  // `sync_io`-pattern API.

  /**
   * Coincides with sync_io::Native_handle_sender concept counterpart.  All notes apply.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * All notes from sync_io::Native_handle_sender::start_send_native_handle_ops() apply.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  template<typename Event_wait_func_t>
  bool start_send_blob_ops(Event_wait_func_t&& ev_wait_func);

  // General API (mirrors transport:: counterpart).

  /**
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @return See above.
   */
  size_t send_blob_max_size() const;

  /**
   * In PEER state: Synchronously, non-blockingly sends one discrete message, reliably and in-order, to the opposing
   * peer; the message consists of the provided binary blob (of length at least 1 byte).  Providing a null blob
   * results in undefined behavior (assertion may trip).
   *
   * Otherwise all notes from sync_io::Native_handle_sender::send_native_handle() apply.
   *
   * @param blob
   *        A binary blob to send; behavior undefined unless `blob.size() >= 1`.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Equivalent to send_blob() but sends a graceful-close message instead of the usual payload.
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @tparam Task_err
   *         See above.
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Equivalent to `async_end_sending(F)` wherein `F()` does nothing.
   *
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @return See above.
   */
  bool end_sending();

  /**
   * In PEER state: Irreversibly enables periodic auto-pinging of opposing receiver with low-level messages that
   * are ignored except that they reset any opposing idle timer.
   *
   * All notes from sync_io::Native_handle_sender apply.
   *
   * @param period
   *        See above.
   * @return See above.
   */
  bool auto_ping();
}; // class Blob_sender

/**
 * A documentation-only *concept*: what transport::Blob_receiver is to transport::Native_handle_receiver (namely a
 * degenerate version thereof), this is to sync_io::Native_handle_receiver.
 *
 * The concept is exactly identical to Native_handle_receiver except that in the latter each message consists
 * of 0-1 native handles and 0-1 binary blobs; whereas here it is always exactly 1 of the latter.  More precisely,
 * each message contains 1 binary blob, whose length must be at least 1 byte.
 *
 * Therefore we keep the documentation very light, only pointing out the differences against
 * sync_io::Native_handle_receiver -- which are summarized in the preceding paragraph.  All other notes from
 * sync_io::Native_handle_receiver apply here.
 *
 * @see sync_io::Native_handle_receiver, an identical concept with 1 feature (native handle transmission) added.
 */
class Blob_receiver
{
public:
  // Constants.

  /// Same notes as for transport::Blob_receiver.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// Same notes as for transport::Blob_receiver.
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = value;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   *
   * All notes from sync_io::Native_handle_receiver apply.
   */
  Blob_receiver();

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   * All notes from sync_io::Native_handle_receiver apply.
   *
   * @param src
   *        See above.
   */
  Blob_receiver(Blob_receiver&& src);

  /// Disallow copying.
  Blob_receiver(const Blob_receiver&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it's
   * still active) and return resources to OS as applicable.
   *
   * All notes from sync_io::Native_handle_receiver apply.
   */
  ~Blob_receiver();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * All notes from sync_io::Native_handle_receiver apply.
   *
   * @param src
   *        See above.
   * @return `*this`.
   */
  Blob_receiver& operator=(Blob_receiver&& src);

  /// Disallow copying.
  Blob_receiver& operator=(const Blob_receiver&) = delete;

  // `sync_io`-pattern API.

  /**
   * Coincides with sync_io::Native_handle_receiver concept counterpart.  All notes apply.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * All notes from sync_io::Native_handle_receiver::start_receive_native_handle_ops() apply.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  template<typename Event_wait_func_t>
  bool start_receive_blob_ops(Event_wait_func_t&& ev_wait_func);

  // General API (mirrors transport:: counterpart).

  /**
   * All notes from sync_io::Native_handle_receiver::receive_meta_blob_max_size() apply.
   *
   * @return See above.
   */
  size_t receive_blob_max_size() const;

  /**
   * In PEER state: Possibly-asynchronously awaits one discrete message -- as sent by the opposing peer -- and
   * receives it into the given target locations, reliably and in-order.
   *
   * Otherwise all notes from sync_io::Native_handle_receiver::async_receive_native_handle() apply.
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_blob
   *        `target_blob.data()...` shall be written to before `on_done_func()` is executed with a falsy
   *        code, bytes numbering `N`, where `N` is passed to that callback.  `N` shall not exceed
   *        `target_blob.size()`.  See "Error semantics" (there is an `Error_code` regarding
   *        overflowing the user's memory area).
   * @param sync_err_code
   *        See above.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param sync_sz
   *        See above.  If null behavior undefined (assertion may trip).
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          Error_code* sync_err_code,  size_t* sync_sz,
                          Task_err_sz&& on_done_func);

  /**
   * In PEER state: Irreversibly enables a conceptual idle timer whose potential side effect is, once at least
   * the specified time has passed since the last received low-level traffic (or this call, whichever most
   * recently occurred), to emit the pipe-hosing error error::Code::S_RECEIVER_IDLE_TIMEOUT.
   *
   * All notes from sync_io::Native_handle_receiver apply.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout);
}; // class Blob_receiver

} // namespace ipc::transport::sync_io
