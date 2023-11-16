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
#  error "As of this writing this is a documentation-only "header" (the "source" is for humans and Doxygen only)."
#else // ifdef IPC_DOXYGEN_ONLY

namespace ipc::transport
{

// Types.

/**
 * A documentation-only *concept* defining the behavior of an object capable of reliably/in-order *sending* of
 * discrete messages, each containing a binary blob.  This is paired with
 * the Blob_receiver concept which defines reception of such messages.
 *
 * The concept is exactly identical to Native_handle_sender except that in the latter each message consists
 * of 0-1 native handles and 0-1 binary blobs; whereas here it is always exactly 1 of the latter.  More precisely,
 * each message contains 1 binary blob, whose length must be at least 1 byte.  You may interpret notes in common-sense
 * fashion; for example send_blob_max_size() here corresponds to `send_meta_blob_max_size()` there.
 *
 * Therefore we keep the documentation very light, only pointing out the differences against Native_handle_sender --
 * which are summarized in the preceding paragraph.  All other notes from Native_handle_sender apply here.
 *
 * @see Native_handle_sender, an identical concept with 1 feature (native handle transmission) added.
 */
class Blob_sender
{
public:
  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.  Equals `_receiver`'s.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   *
   * All notes from Native_handle_sender() default ctor doc header apply.
   */
  Blob_sender();

  /**
   * `sync_io`-core-adopting ctor: Creates a peer object in PEER state by subsuming a `sync_io` core in that state.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See Native_handle_sender concept.
   */
  Blob_sender(sync_io::Blob_sender&& sync_io_core_in_peer_state_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   */
  Blob_sender(Blob_sender&& src);

  /// Disallow copying.
  Blob_sender(const Blob_sender&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it
   * is still active) and cancels any pending completion handlers by invoking them ASAP with
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   *
   * All notes from ~Native_handle_sender() doc header apply.
   */
  ~Blob_sender();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * @see ~Blob_sender().
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   * @return `*this`.
   */
  Blob_sender& operator=(Blob_sender&& src);
  // Methods.

  /// Disallow copying.
  Blob_sender& operator=(const Blob_sender&) = delete;

  /**
   * In PEER state: Returns max `blob.size()` such that send_blob() shall not fail due to too-long
   * payload with error::Code::S_INVALID_ARGUMENT.  Always the same value once in PEER state.  The opposing
   * Native_handle_receiver::receive_blob_max_size()` shall return the same value (in the opposing object potentially
   * in a different process).
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns zero; else
   * a positive value.
   *
   * @return See above.
   */
  size_t send_blob_max_size() const;

  /**
   * In PEER state: Synchronously, non-blockingly sends one discrete message, reliably and in-order, to the opposing
   * peer; the message consists of the provided binary blob (of length at least 1 byte).  Providing a null blob
   * results in undefined behavior (assertion may trip).
   *
   * All notes from Native_handle_sender::send_native_handle() doc header apply.
   *
   * @param blob
   *        A binary blob to send; behavior undefined unless `blob.size() >= 1`.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Equivalent to send_blob() but sends a graceful-close message instead of the usual payload; the opposing
   * peer's Blob_receiver shall receive it reliably and in-order via Blob_receiver::async_receive_blob() in the form of
   * #Error_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE.
   *
   * All notes from Native_handle_sender::async_end_sending() doc header apply.
   *
   * @tparam Task_err
   *         See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * Equivalent to `async_end_sending(F)` wherein `F()` does nothing.
   *
   * All notes from Native_handle_sender::end_sending() doc header apply.
   *
   * @return See above.
   */
  bool end_sending();

  /**
   * In PEER state: Irreversibly enables periodic auto-pinging of opposing receiver with low-level messages that
   * are ignored except that they reset any idle timer as enabled via Blob_receiver::idle_timer_run()
   * (or similar).
   *
   * All notes from Native_handle_receiver::auto_ping() doc header apply.
   *
   * @param period
   *        See above.
   * @return See above.
   */
  bool auto_ping();
}; // class Blob_sender

/**
 * A documentation-only *concept* defining the behavior of an object capable of reliably/in-order *receiving* of
 * discrete messages, each containing a binary blob.  This is paired with
 * the Blob_sender concept which defines sending of such messages.
 *
 * The concept is exactly identical to Native_handle_receiver except that in the latter each message consists
 * of 0-1 native handles and 0-1 binary blobs; whereas here it is always exactly 1 of the latter.  More precisely,
 * each message contains 1 binary blob, whose length must be at least 1 byte.  You may interpret notes in common-sense
 * fashion; for example receive_blob_max_size() here corresponds to `receive_meta_blob_max_size()` there.
 *
 * Therefore we keep the documentation very light, only pointing out the differences against Native_handle_receiver --
 * which are summarized in the preceding paragraph.  All other notes from Native_handle_receiver apply here.
 *
 * @see Native_handle_receiver, an identical concept with 1 feature (native handle transmission) added.
 */
class Blob_receiver
{
public:
  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.  Equals `_sender`'s.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /**
   * If `false` then `blob.size() > receive_blob_max_size()` in PEER-state async_receive_blob()
   * shall yield non-pipe-hosing error::Code::INVALID_ARGUMENT, and it shall never yield
   * pipe-hosing error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE; else the latter may occur, while the former
   * shall never occur for that reason.
   *
   * @see "Blob underflow semantics" in Native_handle_sender concept doc header; they apply equally here.
   */
  static constexpr bool S_BLOB_UNDERFLOW_ALLOWED = value;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.
   *
   * All notes from Native_handle_receiver() default ctor doc header apply.
   */
  Blob_receiver();

  /**
   * `sync_io`-core-adopting ctor: Creates a peer object in PEER state by subsuming a `sync_io` core in that state.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See Native_handle_sender concept.
   */
  Blob_receiver(sync_io::Blob_receiver&& sync_io_core_in_peer_state_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   */
  Blob_receiver(Blob_receiver&& src);

  /// Disallow copying.
  Blob_receiver(const Blob_receiver&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual incoming-direction pipe (in PEER state, and if it
   * is still active) and cancels any pending completion handlers by invoking them ASAP with
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   *
   * All notes from ~Native_handle_receiver() doc header apply.
   */
  ~Blob_receiver();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * @see ~Blob_receiver().
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   * @return `*this`.
   */
  Blob_receiver& operator=(Blob_receiver&& src);

  /// Disallow copying.
  Blob_receiver& operator=(const Blob_receiver&) = delete;

  /**
   * In PEER state: Returns min `target_blob.size()` such that (1) async_receive_blob() shall not fail
   * with error::Code::S_INVALID_ARGUMENT (only if #S_BLOB_UNDERFLOW_ALLOWED is `false`; otherwise not relevant),
   * and (2) it shall *never* fail with error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE.  Please see
   * "Blob underflow semantics" (for Native_handle_receiver, but applied in common-sense fashion to us) for
   * explanation of these semantics.
   *
   * Always the same value once in PEER state.  The opposing
   * Native_handle_sender::send_blob_max_size() shall return the same value (in the opposing object potentially
   * in a different process).
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns zero; else
   * a positive value.
   *
   * @return See above.
   */
  size_t receive_blob_max_size() const;

  /**
   * In PEER state: Asynchronously awaits one discrete message -- as sent by the opposing peer via
   * Blob_sender::send_blob() or `"Blob_sender::*end_sending()"` -- and
   * receives it into the given target locations, reliably and in-order.
   *
   * All notes from Native_handle_receiver::async_receive_native_handle() doc header apply.  In particular
   * watch out for the semantics regarding `target_blob.size()` and potential resulting errors.
   *
   * @tparam Task_err_sz
   *         See above.
   * @param target_blob
   *        `target_blob.data()...` shall be written to by the time `on_done_func()` is executed with a falsy
   *        code, bytes numbering `N`, where `N` is passed to that callback.  `N` shall not exceed
   *        `target_blob.size()`.  See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err_sz>
  bool async_receive_blob(const util::Blob_mutable& target_blob,
                          Task_err_sz&& on_done_func);

  /**
   * In PEER state: Irreversibly enables a conceptual idle timer whose potential side effect is, once at least
   * the specified time has passed since the last received low-level traffic (or this call, whichever most
   * recently occurred), to emit the pipe-hosing error error::Code::S_RECEIVER_IDLE_TIMEOUT.
   *
   * All notes from Native_handle_receiver::idle_timer_run() doc header apply.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout);
}; // class Blob_receiver

} // namespace ipc::transport
