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

#include "ipc/transport/detail/blob_stream_mq_snd_impl.hpp"
#include <experimental/propagate_const>

namespace ipc::transport
{

// Types.

/**
 * Implements Blob_sender concept by using an adopted Persistent_mq_handle MQ handle to an MQ (message queue)
 * of that type, such as a POSIX or bipc MQ.  This allows for high-performance, potentially zero-copy (except
 * for copying into the transport MQ) of discrete messages, each containing a binary blob.
 * This is a low-level (core) transport mechanism; higher-level (structured)
 * transport mechanisms may use Blob_stream_mq_sender (and Blob_stream_mq_receiver) to enable their work.
 *
 * ### Informal comparison to other core transport mechanisms ###
 * It is intended for transmission of relatively short messages -- rough guidance
 * being for max length being in the 10s-of-KiB range.  With a modern Linux kernel on server hardware from about 2014
 * to 2020, our performance tests show that its raw speed for messages of aforementioned size is comparable to
 * non-zero-copy mechanism based on Unix domain sockets.  Technically we found `Blob_stream_mq_*<Posix_mq_handle>`
 * to be somewhat faster; and `Blob_stream_*<Bipc_mq_handle>` (internally, SHM-based) somewhat slower.
 * However, for best perf, it is recommended to send handles to SHM areas containing arbitrarily long structured
 * messages (such as ones [de]serialized using zero-copy builders: capnp and the like).
 * This further reduces the importance of relative perf compared to
 * other low-level transports (which, as noted, is pretty close regardless -- though this is bound to stop being true
 * for much longer messages, if the send-SHM-handles technique were to *not* be used).
 *
 * @see Native_socket_stream which also implements Blob_sender (and Blob_receiver) in its degraded mode (where
 *      one does not transmit native handles, only blobs).
 *
 * ### Relationship between `Blob_stream_mq_*`, the underlying MQ resource, and its Shared_name ###
 * If one is familiar with MQs (doesn't matter whether POSIX or bipc -- their persistence semantics are identical),
 * it is easy to become confused as to what `Blob_stream_mq_*` is modeling.  To explain it then:
 *
 * - Think of the MQ itself as a file -- albeit in RAM -- a bulky thing that can store a certain number of bytes.
 * - Think of its Shared_name as a directory entry, essentially a file name in a certain directory in the file system.
 * - Think of a Persistent_mq_handle (the actual type being a template param; namely as of this writing
 *   Posix_mq_handle or Bipc_mq_handle) as a file handle.  They can be opened for reading, writing, or both.
 *   More than 1 can be opened simultaneously.
 *   - Writing through this handle *pushes* a message but really writes those bytes somewhere in the "file" (MQ)
 *     in RAM.  Any handle can be used for this; one can write through handle X and then right after that read through
 *     same handle X if desired.
 *   - Reading, similarly, *pops* a message by reading bytes from the "file."
 *   - One can open handle X, close handle X, wait 5 minutes, and from even another process open handle Y
 *     to the same name: the "stuff" (un-popped messages) will still be there.
 *
 * So that's MQs.  However, `Blob_stream_mq_*` -- implementing Blob_sender and Blob_receiver as they do -- emphatically
 * does *not* model that kind of free-for-all access (read/write, through 3+ handles if desired).  Instead it models
 * a *one-direction pipe*.  So: there is up to 1 (exactly 1, if you want it to be useful) Blob_stream_mq_sender
 * per Shared_name.  It *only* writes (pushes).  There is up to 1 (exactly 1, if...) Blob_stream_mq_receiver
 * per Shared_name.  It *only* reads (pops).  Therefore trying to create a 2nd `_sender`, when 1 already exists --
 * in *any* process -- is an error and does not work (an error is emitted from ctor; see its doc header).
 * Same thing for `_receiver`.
 *
 * Moreover it is not a persistent pipe, either, even though the underlying MQ is quite capable of persistence
 * (until reboot at any rate).  Per Blob_sender/Blob_receiver concept, when a destructor for *either*
 * the 1 Blob_stream_mq_sender *or* the 1 Blob_stream_mq_receiver is called (whichever happens first):
 *   - The Shared_name "directory entry" is deleted.  The MQ becomes anonymous: creating an MQ with the same name
 *     will begin working -- and will create a totally new MQ.
 *     - (Caveat/corner case: In practice this is only useful after *both* destructors have run.
 *       Indeed MQ creation is possible after 1, but not the other, has executed; but then that "the other" will
 *       sadly delete the new, same-named MQ.  So don't.)
 *   - The MQ handle is closed.
 *
 * Then once the other peer object (whether receiver or sender) dtor also executes:
 *   - (There is no "directory entry" to delete any longer.)
 *   - The MQ handle is closed.
 *
 * That last step brings the ref-count of things referring to the underlying MQ "file" (in RAM) to 0.  Hence
 * the MQ "file" (in RAM) is deleted (RAM is freed, becoming usable by other stuff, like maybe the general heap).
 *
 * That is how the Blob_sender/Blob_receiver one-direction pipe concept is satisfied, even though a general MQ
 * has greater capabilities.  That's not to say one should not use those; just not what this class is for;
 * but one can use Posix_mq_handle/Bipc_mq_handle directly if necessary.
 *
 * ### Cleanup ###
 * As just described, the two `Blob_stream_mq_*` classes are fairly aggressive about deleting the underlying MQ
 * RAM resource.  It is, generally, important not to leave those lying around post-use, as they can take non-trivial
 * RAM.
 *
 * However, MQs (being kernel-persistent at least) require an even greater amount of care in cleanup than that.
 * Assuming buglessness of the classes themselves and a graceful shutdown of the program -- including on clean
 * exception handling, wherein the stack fully unwinds, and the destructors all run -- there is no problem.
 *
 * However, if something (a hard crash perhaps) prevents a Blob_stream_mq_sender or Blob_stream_mq_receiver dtor
 * from running, the MQ "file" (in RAM) shall leak.  It is your responsibility to implement a contingency for such
 * leaks.  (Note that, beyond RAM, there are limits on the number of MQs -- and certain other internally used
 * kernel-persistent objects -- that exist system-wide.  If reached -- which is not hard -- then things will
 * screech to a halt, until something cleans up the leaks.)  We provide 2 techniques to aid this.
 *
 *   - You can maintain a *persistent* registry of created MQ streams.  In that case you'll have a list of
 *     `mq.absolute_name()`s (where `mq` is passed to our and the receiver peer's ctors).  After any potential
 *     abort, in another instance (process) of the application, invoke Blob_stream_mq_base::remove_persistent()
 *     on each such name.
 *   - To avoid the persistent registry, a much easier, though less surgical/definitive, technique is to maintain
 *     a prefix-based naming convention for `mq.absolute_name()`.  Then: After any potential abort, in another
 *     instance (process) of the application, invoke remove_each_persistent_with_name_prefix<Blob_stream_mq_base>(),
 *     passing in the applicable Shared_name prefix.  Having done so, the leaks (if any) should be cleaned.
 *
 * ipc::session uses the 2nd technique.
 *
 * ### Reusing the same absolute_name() for a new MQ and one-direction pipe ###
 * Formally, as pointed out in various docs, this is possible no later than the return of the later of the
 * Blob_stream_mq_sender and Blob_stream_mq_receiver dtors.  Informally, we recommend against this if it can be avoided.
 * Sure, it's fine if you can guarantee no process -- even *another* process -- would try to do so until
 * in fact both of those dtors has run.  This may not be trivial; it might even require more IPC to coordinate!
 * It is better to segregate these things via unique names, using a namespace from something like a PID which
 * will not, within reason, repeat before reboot.  ipc::session uses this technique for various kernel-persistent
 * resources including (not limited to) these MQs.
 *
 * ### Thread safety ###
 * We add no more thread safety guarantees than those mandated by Blob_sender concept.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Internally this class template uses uses the pImpl idiom (see https://en.cppreference.com/w/cpp/language/pimpl
 * for an excellent overview), except it is what I (ygoldfel) term "pImpl-lite".  That is: it is pImpl that achieves
 * performant and easily-coded move-semantics -- in the face of fairly complex async impl details --
 * but does *not* achieve a stable ABI (the thing where one can change impl method bodies without recompiling
 * the code/changing the binary signature of the class).  Long story short:
 *   - See Native_socket_stream's "Implementation design/rationale" for why they chose pImpl.  The same applies here.
 *   - However, we are a template, and this template-ness is not reasonably possible to elide (via type erasure or
 *     something) into a non-template impl class.  Therefore, simply, there is the non-movable
 *     Blob_stream_mq_sender_impl class *template* which is in detail/ and not to be `#include`d by the user;
 *     but *we* simply `#include` it above this doc header; and then write Blob_stream_mq_sender in terms of it.
 *   - So we get the quick/easy move-semantics that pImpl gives; but we don't get the binary separation between
 *     interface and implementation.
 *     - Also, stylistically, I (ygoldfel) did not bother to make Blob_stream_mq_sender_impl an inner class.
 *       It's in detail/ which means user must not instantiate it; this is a common pattern.
 *       I did not bother vaguely because it's not full pImpl anyway, and the circular reference nonsense would
 *       be annoying.
 *
 * @see Blob_stream_mq_sender_impl doc header.
 *
 * @endinternal
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 *
 * @see Blob_sender: implemented concept.
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

  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Blob_stream_mq_sender<Mq>;
  /// You may disregard.
  using Async_io_obj = Null_peer;

  // Constants.

  /// Implements concept API.  Equals `Mq::S_RESOURCE_TYPE_ID`.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Constructs the sender by taking over an already-opened MQ handle.
   * Note that this op does not implement any concept; Blob_sender concept does not define how a Blob_sender
   * is created in this explicit fashion.
   *
   * No traffic must have occurred on `mq_moved` up to this call.  Otherwise behavior is undefined.
   *
   * If this fails (sets `*err_code` to truthy if not null; throws if null), all transmission calls on `*this`
   * will fail with the post-value in `*err_code` emitted.  In particular, error::Code::S_BLOB_STREAM_MQ_SENDER_EXISTS
   * is that code, if the reason for failure was that another Blob_stream_mq_sender to
   * `mq_moved.absolute_name()` has already been created in this or other process.  See class doc header for
   * discussion of the relationship between Blob_stream_mq_sender, Blob_stream_mq_receiver, the underlying MQ
   * at a given Shared_name, and that Shared_name as registered in the OS.
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
   * If something prevents the destructor from running -- a hard crash, say -- the underlying MQ and/or the name
   * may be leaked.  External measures taken by ipc::session machinery are likely necessary to subsequently clean up
   * the resource which, depending on the parameters passed to the #Mq ctor when originally creating the MQ,
   * may use non-trivial RAM.
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
   *        error::Code::S_BLOB_STREAM_MQ_SENDER_EXISTS (another Blob_stream_mq_sender exists),
   *        system codes (other errors, all to do with the creation of a separate internally used tiny SHM pool
   *        used to prevent duplicate Blob_stream_mq_sender in the system).
   */
  explicit Blob_stream_mq_sender(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 Mq&& mq_moved, Error_code* err_code = 0);

  /**
   * Implements Blob_sender API, per its concept contract.
   * All the notes for that concept's core-adopting ctor apply.
   *
   * @param sync_io_core_in_peer_state_moved
   *        See above.
   *
   * @see Blob_sender::Blob_sender(): implemented concept.
   */
  explicit Blob_stream_mq_sender(Sync_io_obj&& sync_io_core_in_peer_state_moved);

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
   * Implements Blob_sender API.  All the notes for the concept's destructor apply but as a reminder:
   *
   * Destroys this peer endpoint which will end the one-direction pipe and cancel any pending
   * completion handlers by invoking it ASAP with error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   * As of this writing these are the completion handlers that would therefore be called:
   *   - The handler passed to async_end_sending() if not yet invoked.
   *     Since it is not valid to call async_end_sending() more than once, there is at most 1 of these.
   *
   * ### Fate of underlying MQ and its `.absolute_name()` ###
   * No later than the return of this destructor:
   *   - The MQ name, namely `mq_moved.absolute_name()` from ctor, shall have been deleted.
   *     Therefore one will be able to util::Create a new #Mq; whereas before the dtor is called and returns
   *     it may (most likely will) not be possible to do so.  This shall decrease the ref-count for the underlying
   *     MQ resource to at most 2.  (Its ref-count is ever at most 3: 1 from the OS-registered Shared_name in the
   *     file system; 1 from `*this` Blob_stream_mq_sender; and 1 from the peer Blob_stream_mq_receiver).
   *   - The ref-count for the underlying MQ resource shall further decrease from 2 to 1 (if the counterpart
   *     Blob_stream_mq_sender still lives) or from 1 to 0 (if not).  In the latter case the MQ itself shall be
   *     deleted, its bulk resources (in RAM) freed.
   *
   * While indeed util::Create of a new #Mq is possible after this dtor returns:
   *   - In practice it should only be done after *both* (sender and receiver) dtors have returned.
   *     Otherwise the later of the 2 will delete the new underlying MQ too: probably not what you want.
   *   - Even having accomplished that, it is still best not to reuse names if possible, at least not anytime soon.
   *     See class doc header for brief discussion.
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
   * @return See above.
   *
   * @see Blob_sender move assignment: implemented concept.
   */
  Blob_stream_mq_sender& operator=(Blob_stream_mq_sender&& src);

  /// Copy assignment is disallowed.
  Blob_stream_mq_sender& operator=(const Blob_stream_mq_sender&) = delete;

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
   *        #Error_code generated:
   *        error::Code::S_INVALID_ARGUMENT (`blob.size()` exceeds max_msg_size()),
   *        error::Code::S_SENDS_FINISHED_CANNOT_SEND (`*end_sending()` was called earlier),
   *        system codes (but never would-block), indicating the underlying transport is hosed for that
   *        specific reason, as detected during outgoing-direction processing.
   * @return See above.
   *
   * @see Native_handle_sender::send_native_handle(): implemented concept.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Implements Blob_sender API per contract.
   * Reminder: It's not thread-safe to call this concurrently with other transmission methods or destructor on
   * the same `*this`.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * system codes (but never would-block) (same as for send_blob()),
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   *
   * Reminder: In rare circumstances, an error emitted there may represent something
   * detected during handling of a preceding send_blob() call but after it returned.
   *
   * @tparam Task_err
   *         See above.
   * @param on_done_func
   *        See above.  Reminder: any moved/copied version of this callback's associated captured state will
   *        be freed soon after it returns.
   * @return See above.  Reminder: If and only if it returns `false`, we're in NULL state, or `*end_sending()` has
   *         already been called; and `on_done_func()` will never be called.
   *
   * @see Blob_sender::async_end_sending(): implemented concept.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

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

template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::Blob_stream_mq_sender
  (Sync_io_obj&& sync_io_core_in_peer_state_moved) :

  m_impl(boost::movelib::make_unique<Blob_stream_mq_sender_impl<Mq>>
           (std::move(sync_io_core_in_peer_state_moved)))
{
  // Yay.
}

// It's only explicitly defined to formally document it.
template<typename Persistent_mq_handle>
Blob_stream_mq_sender<Persistent_mq_handle>::~Blob_stream_mq_sender() = default;

template<typename Persistent_mq_handle>
size_t Blob_stream_mq_sender<Persistent_mq_handle>::send_blob_max_size() const
{
  return m_impl ? m_impl->send_blob_max_size() : 0;
}

template<typename Persistent_mq_handle>
bool Blob_stream_mq_sender<Persistent_mq_handle>::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  return m_impl ? (m_impl->send_blob(blob, err_code), true)
                : false;
}

template<typename Persistent_mq_handle>
template<typename Task_err>
bool Blob_stream_mq_sender<Persistent_mq_handle>::async_end_sending(Task_err&& on_done_func)
{
  return m_impl ? m_impl->async_end_sending(std::move(on_done_func)) : false;
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

} // namespace ipc::transport
