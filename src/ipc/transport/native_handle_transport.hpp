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

namespace ipc::transport
{

// Types.

/**
 * A documentation-only *concept* defining the behavior of an object capable of reliably/in-order *sending* of
 * discrete messages, each containing a native handle, a binary blob, or both.  This is paired with
 * the Native_handle_receiver concept which defines reception of such messages.
 *
 * ### Concept contents ###
 * The concept defines the following behaviors/requirements.
 *   - The object has at least 2 states:
 *     - NULL: Object is not a peer: is not connected/capable of transmission.  Essentially it is not a useful
 *       state.  A default-cted object is in NULL state; and a moved-from object is as-if default-cted, therefore
 *       also in a NULL state.  In this state all the transmission methods return `false` and no-op.
 *     - PEER: Object is or has been a peer (connected/capable of transmission).  It is not possible to exit PEER
 *       state, except via being moved-from.  A pipe-hosing error retains PEER state for instance.
 *     - Other states are allowed but outside the scope of the concept.  For example a CONNECTING state may or may
 *       not be relevant (but again not relevant to the concept).
 *   - The (outgoing) transmission-of-messages methods, including transmission of graceful-close message.
 *     See their doc headers.
 *   - Behavior when the destructor is invoked.  See ~Native_handle_sender()
 *     doc header.
 *   - Default ctor (which creates a NULL-state object that can be moved-to and to which "as-if"
 *     state any moved-from object is changed).
 *   - `sync_io`-core adopting ctor (which creates PEER-state object from an idle, as-if-just cted PEER-state
 *     sync_io::Native_handle_sender).
 *   - Move ctor, move assigment operator; these do the reasonable thing including setting the moved-from object
 *     to NULL state.  Various satellite APIs (e.g., Native_socket_stream_acceptor) shall
 *     need these.  That is such APIs do not rely on the factory/shared-ownership pattern.
 *
 * The concept (intentionally) does *not* define the following behaviors:
 *   - How to create a Native_handle_sender, except the default, `sync_io`-core-adopting, and move ctors.
 *     That is it does not define how to create a *new* (not moved-from) and *functioning* (PEER state: transmitting,
 *     connected) peer object, except the latter from an existing `sync_io`-pattern core.  It only defines behavior
 *     once the user has access to a Native_handle_sender that is already in PEER state: connected to a opposing
 *     peer Native_handle_receiver.
 *
 * @see Native_socket_stream: as of this writing one key class that implements this concept (and also
 *      Native_handle_receiver) -- using the Unix domain socket transport.
 * @see Channel, a pipe-composing class template that potentially implements this concept.
 * @see Blob_sender: a degenerate version of the present concept: capable of transmitting only blobs, not
 *      `Native_handle`s.
 *
 * @todo In C++20, if/when we upgrade to that, Native_handle_sender (and other such doc-only classes) can become an
 * actual concept formally implemented by class(es) that, today, implement it via the "honor system."
 * Currently it is a class `#ifdef`-ed out from actual compilation but still participating in doc generation.
 * Note that Doxygen (current version as of this writing: 1.9.3) claims to support doc generation from formal
 * C++20 concepts.
 *
 * ### Rationale: Why is send_native_handle() not asynchronous? ###
 * send_native_handle(), as noted in its doc header (and similarly Blob_sender::send_blob()) has 2 key properties:
 *   - It will *never* return would-block despite *always* being non-blocking.
 *   - It takes no completion handler; and it returns pipe-hosing errors synchronously via an out-arg or
 *     exception.
 *     - async_end_sending() does take a completion handler.
 *
 * That is, it is "somehow" both non-blocking/synchronous -- yet requires no would-block contingency on the user's
 * part.  Magical!  How can this possibly work?  After all we know that all known low-level transports will yield
 * would-block eventually (as of this writing -- Unix domain sockets, POSIX MQs, bipc SHM-based MQs), if the
 * opposing side is not popping data off the pipe.  Yet, as we look at the opposing Native_handle_receiver concept
 * we see that it makes no requirement on its impls to pop everything off the low-level transport in-pipe as soon
 * as it arrives.  (In actual fact all existing impls in Flow-IPC refuse to do any such thing.)
 *
 * Naturally the only answer as to how it can work is: Any given Native_handle_sender (and native Blob_sender) impl
 * shall be ready to encounter would-block internally; if this occurs it will need to make a copy of the offending
 * message (the meta-blob part specifically included -- that's the part that involves significant copying),
 * enqueue it internally -- and keep enqueuing any further send_native_handle()d messages until the would-block
 * clears, and the queue is eventually emptied.
 *
 * Fair enough: But isn't this a performance hazard?  Answer: yes but only in a necessary way.  We reached this
 * decision after considering the entire system end-to-end:
 *   -# user prepares message blob B and tells `Native_handle_sender` S to send B;
 *   -# S tells low-level transport (possibly kernel) to accept B;
 *   -# `Native_handle_receiver` R pops B from low-level transport;
 *   -# user tells R it wanst to accept B, and R does give the latter to the user.
 *
 * It was a no-go to have Flow-IPC be in charge of allocating either-end buffer for B on the user's behalf:
 * the user may want to user their own allocator, or the stack, or ??? -- possibly with zero-copy and prefix/postfix
 * data nearby.  The user buffer has to be ready, and S and R need to work with the memory areas provided by the
 * user.
 *
 * Given all that, *one* or more of the following "people" has to be ready to keep B, or a copy of B, around, until
 * the whole thing (1-4) has gone through: the sending user (1 above), S (2 above), or R (3 above).  (If
 * the receiving user (4 above) is always receiving things ASAP, without that process being processor-pegged or
 * something, then it won't be a problem: no copying is necessary, except in step 2 when B is copied into
 * the low-level process.  Indeed that should be the case usually; we are discussing what to do *if* something goes
 * wrong, or the receivier application is mis-coded, or who knows -- the receiver just isn't quick enough.)
 *
 * The answer, as already noted: *One* or more of 1, 2, 3 has to make a copy of B long enough until 4 has received it.
 * At that point, in terms of overall performance, it *which* one of 1, 2, 3 it should be.  Upon deliberating
 * a few options occurred, but it seemed clear enough that 1 (original user) should not be the done, if it can be
 * helped.  IPC is not networking; as an end user I expect sending an out-message to work synchronously.  Making
 * me worry about a completion handler complicates the outgoing-direction API hugely -- and not in a way that
 * makes the incoming-direction API any simpler, since that one always has to be ready for would-block
 * (i.e., no in-messages immediately ready -- an async-receive API is required).
 *
 * So that left 2 or 3.  I (ygoldfel) simply made the decision that one has to be chosen, and of those 2 is earlier
 * and (internally for the concept implementer, also me at the time) more straightforward.  If 3 were always
 * reading items out of the low-level transport -- even when the user was not invoking `async_receive_*()` --
 * then it would have to at least sometimes make copies of incoming data before the user chose to pop them out.
 * By contrast, queueing it -- only on would-block -- in 2 would effectively make it a last-resort activity, as
 * it should be.
 *
 * To summarize: Perfect opposing-receiver behavior cannot be guaranteed, and the low-level transport will eventually
 * have a limit as to how much data it can buffer => in that case some entity must make a copy of the overflowing data
 * => we want to make life easier for end user, so that entity should not be them => it must be either
 * the `*_sender` or `*_receiver` => we chose `*_sender`, because it is safer and easier and faster and a last resort.
 *
 * One arguable drawback of this API design is that the Native_handle_sender (and Blob_sender) user won't be
 * informed of an out-pipe-hosing error quite as soon as that object itself would know of it -- in some (rare)
 * cases.  I.e., if some data are queued up during would-block, and the user has stopped doing `send_*()`
 * (as far as she is concerned, they're all working fine and have succeeded), and some timer later
 * `*this` determines (in the background now) that the transport is hosed, the user would only find out about it
 * upon the next `send_*()` call (it would *then* return the `Error_code` immediately), if any, or via
 * async_end_sending().  Is this really a problem?  Answer: Just... no.  Exercise in reasoning this out (granted
 * it is *somewhat* subjective) left to the reader.  (Note we *could* supply some kind of out-of-band
 * on-any-send-error API, and we considered this.  It just was not worth it: inelegant, annoying.  async_end_sending()
 * with its completion handler is well sufficient.)
 *
 * It should be noted that this decision to make send_native_handle() synchronous/non-blocking/never-would-blocking
 * has percolated through much of ipc::transport and even ipc::session: all our impls; Blob_sender and all its
 * impls; Channel; and quite significantly struc::Channel.  You'll note struc::Channel::send() is
 * also synchronous/non-blocking/never-would-blocking, and even its implementation at the stage of actually
 * sending binary data via Native_handle_sender/Blob_sender is quite straightforward.  In Flow-IPC a
 * `send*()` "just works."
 *
 * @todo Comparing Blob_sender::send_blob_max_size() (and similar checks in that family of concepts) to test
 * whether the object is in PEER state is easy enough, but perhaps we can have a utility that would more
 * expressively describe this check: `in_peer_state()` free function or something?  It can still use the same
 * technique internally.
 */
class Native_handle_sender
{
public:
  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.  Equals `_receiver`'s.
  static const Shared_name S_RESOURCE_TYPE_ID;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.  In this state,
   * all transmission-related methods (e.g., send_native_handle()) shall return `false` and otherwise no-op
   * (aside from possible logging).
   *
   * This ctor is informally intended for the following uses:
   *   - A moved-from Native_handle_sender (i.e., the `src` arg for move-ctor and move-assignment operator)
   *     becomes as-if defaulted-constructed.
   *   - A target Native_handle_sender for a factory-like method (such as Native_socket_stream_acceptor::async_accept())
   *     shall typically be default-cted by the callin guse.  (E.g.: Native_socket_stream_acceptor shall asynchronously
   *     move-assign a logger-apointed, nicely-nicknamed into that target `*this`, typically default-cted.)
   *
   * ### Informal corollary -- ctor that begins in PEER state ###
   * Any functioning Native_handle_sender shall need at least one ctor that starts `*this` directly in PEER state.
   * In that state the transmission-related methods (e.g., send_native_handle()) shall *not* return `false` and
   * will in fact attempt transmission.  However the form and function of such ctors is entirely dependent on
   * the nature of the low-level transmission medium being encapsulated and is hence *intentionally* not a part
   * of the concept.  That said there is the `sync_io`-core-adopting ctor as well which *is* part of the concept.
   *
   * ### Informal corollary -- NULL-state ctors with 1+ args ###
   * Other, 1+ arg, ctors that similarly create a NULL-state peer object are allowed/encouraged
   * where relevant.  In particular one taking a `Logger*` and a `nickname` string -- so as to subsequently enter a
   * connecting phase via an `*_connect()` method, on the way to PEER state -- is reasonable.
   * However this is not a formal part of this concept and is arguably not a general behavior.
   * Such a ctor is informally intended for the following use at least:
   *   - One creates a Native_handle_sender that is `Logger`-appointed and nicely-`nickname`d; then one calls
   *     `*_connect()` on it in order to move it to a connecting phase and, hopefully, PEER state in that order.
   *     It will retain the logger and nickname (or whatever) throughout.
   */
  Native_handle_sender();

  /**
   * `sync_io`-core-adopting ctor: Creates a peer object in PEER state by subsuming a `sync_io` core in that state.
   * That core must be as-if-just-cted (having performed no work and not been configured via `.start_*_ops()`).
   * That core object becomes as-if default-cted (therefore in NULL state).
   *
   * @param sync_io_core_in_peer_state_moved
   *        See above.
   */
  Native_handle_sender(sync_io::Native_handle_sender&& sync_io_core_in_peer_state_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   */
  Native_handle_sender(Native_handle_sender&& src);

  /// Disallow copying.
  Native_handle_sender(const Native_handle_sender&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual outgoing-direction pipe (in PEER state, and if it's
   * still active) and cancels any pending completion handlers by invoking them ASAP with
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   * As of this writing these are the completion handlers that would therefore be called:
   *   - The handler passed to async_end_sending(), if it was not `.empty()` and has not yet been invoked.
   *     Since it is not valid to call async_end_sending() more than once, there is at most 1 of these.
   *
   * The pending completion handler will be called from an unspecified thread that is not the calling thread.
   * Any associated captured state for that handler will be freed shortly after the handler returns.
   *
   * We informally but very strongly recommend that your completion handler immediately return if the `Error_code`
   * passed to it is `error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER`.  This is similar to what one should
   * do when using boost.asio and receiving the conceptually identical `operation_aborted` error code to an
   * `async_...()` completion handler.  In both cases, this condition means, "we have decided to shut this thing down,
   * so the completion handlers are simply being informed of this."
   *
   * In NULL state the dtor shall no-op.
   *
   * ### Thread safety ###
   * Same as for the transmission methods.  *Plus* it is not safe to call this from within a completion handler supplied
   * to one of those methods on the same `*this`.  An implication of the latter is as follows:
   *
   * Any user source code line that nullifies a `Ptr` (`shared_ptr`) handle to `*this` should be seen as potentially
   * *synchoronously* invoking this dtor.  (By nullification we mean `.reset()` and anything else that makes the
   * `Ptr` null.  For example destroying a `vector<Ptr>` that contains a `Ptr` pointing to `*this` = nullification.)
   * Therefore, if a nullification statement can possibly make the ref-count reach 0, then the user must zealously
   * protect that statement from running concurrently with another send/receive API call on `*this`.  That is to say,
   * informally: When ensuring thread safety with respect to concurrent access to a Native_handle_sender, don't forget
   * that the destructor also is a write-type of access, and that nullifying a `Ptr` handle to it can synchronously
   * invoke it.
   *
   * @see Native_handle_receiver::~Native_handle_receiver(): sister concept with essentially equal requirements.
   * @see Native_socket_stream::~Native_socket_stream(): implements concept (also implements just-mentioned sister
   *      concept).
   */
  ~Native_handle_sender();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * @see ~Native_handle_sender().
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   * @return `*this`.
   */
  Native_handle_sender& operator=(Native_handle_sender&& src);

  /// Disallow copying.
  Native_handle_sender& operator=(const Native_handle_sender&) = delete;

  /**
   * In PEER state: Returns max `meta_blob.size()` such that send_native_handle() shall not fail due to too-long
   * payload with error::Code::S_INVALID_ARGUMENT.  Always the same value once in PEER state.  The opposing
   * Blob_receiver::receive_meta_blob_max_size() shall return the same value (in the opposing object potentially
   * in a different process).
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns zero; else
   * a positive value.
   *
   * @return See above.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * In PEER state: Synchronously, non-blockingly sends one discrete message, reliably/in-order, to the opposing peer;
   * the message consists of the provided native handle (if supplied); or the provided binary blob (if supplied);
   * or both (if both supplied).  The opposing peer's paired Native_handle_receiver shall receive it reliably and
   * in-order via Native_handle_receiver::async_receive_native_handle().
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).
   *
   * Providing neither a handle nor a blob results in undefined behavior (assertion may trip).
   * To *not* supply a handle, provide object with `.null() == true`.  To *not* supply a blob, provide
   * a buffer with `.size() == 0`.
   *
   * ### Blob copying behavior; synchronicity/blockingness guarantees ###
   * If a blob is provided, it need only be valid until this method returns.  The implementation shall, informally,
   * strive to *not* save a copy of the blob (except into the low-level transport mechanism); but it is *allowed* to
   * save it if required, such as if the low-level transport mechanism encounteres a would-block condition.
   *
   * This means, to the user, that this method is *both* non-blocking *and* synchronous *and* it must not
   * refuse to send and return any conceptual would-block error (unlike, say, with a typical networked TCP socket API).
   * Informally, the implementation must deal with any (typically quite rare) internal low-level would-block condition
   * internally.
   *
   * However!  Suppose send_native_handle() returns without error.  This concept does *not* guarantee the message has
   * been passed to the low-level transport at *this* point in time (informally it shall strive to make that
   * happen in most cases however).  A low-level would-block condition may cause it to be deferred.  If one
   * invokes ~Native_handle_sender() (dtor), the message may never be sent.
   *
   * However (part II)!  It *is* guaranteed to have been passed to the transport once `*end_sending()`
   * has been invoked, *and* its `on_done_func()` (if any) callback has been called.  Therefore the expected use
   * pattern is as follows:
   *   - Obtain a connected `Native_handle_sender` via factory.
   *   - Invoke send_native_handle() synchronously, hopefully without error.
   *   - (Repeat as needed for more such messages.)
   *   - Invoke `async_end_sending(F)` or `end_sending()` (to send graceful-close).
   *   - In `F()` destroy the `Native_handle_sender` (its dtor is invoked).
   *     - `F()` is invoked only once all queued messages, and the graceful-close, have been sent.
   *
   * ### Error semantics ###
   * Firstly: See `flow::Error_code` docs for error reporting semantics (including the exception/code dichotomy).
   *
   * Secondly: If the generated #Error_code is error::Code::S_INVALID_ARGUMENT:
   *   - This refers to an invalid argument to *this* method invocation.  This does *not* hose the pipe:
   *     further sends, etc., may be attempted with reasonable hope they will succeed.
   *     - send_meta_blob_max_size() is the limit for `meta_blob.size()`.  Exceeding it leads to `S_INVALID_ARGUMENT`.
   *       - This interacts with the opposing Native_handle_receiver::async_receive_native_handle() in a subtle
   *         way.  See "Blob underflow semantics" in that class concept's doc header.
   *     - At the impl's discretion, other conditions *may* lead to `S_INVALID_ARGUMENT`.  If so they must
   *       be documented.
   *
   * Thirdly: All other non-success `Error_code`s generated should indicate the pipe is now indeed hosed; the user
   * can count on the pipe being unusable from that point on.  (It is recommended they cease to use the pipe
   * and invoke the destructor to free resources.)  If such a non-success code is emitted, the same one shall be emitted
   * by all further send_native_handle() and async_end_sending() on `*this`.
   *
   * In particular error::Code::S_SENDS_FINISHED_CANNOT_SEND shall indicate that *this* method invocation
   * occurred after invoking `*end_sending()` earlier.
   *
   * No would-block-like error condition shall be emitted.
   *
   * Lastly: The implementation is allowed to return a non-`INVALID_ARGUMENT` #Error_code in *this* invocation even
   * though the true cause is a *previous* invocation of send_native_handle().
   * Informally this may occur (probably in rare circumstances) if internally the handling of a previous
   * call had to be deferred due to a low-level would-block condition; in that case the resulting problem is to be
   * reported opportunistically with the next call.  This allows for the non-blocking/synchronous semantics without
   * the need to make send_native_handle() asynchronous.
   *
   * ### Thread safety ###
   * You may call this from any thread.  It is *not* required to be safe to call concurrently with
   * any Native_handle_sender API, including this one and the destructor, invoked on `*this`.
   *
   * @param hndl_or_null
   *        A native handle (a/k/a FD) to send; or none if `hndl_or_null.null() == true`.
   * @param meta_blob
   *        A binary blob to send; or none if `meta_blob.size() == 0`.
   * @param err_code
   *        See above.
   * @return `false` if `*this` is not in PEER (connected, transmitting) state;
   *         otherwise `true`.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob, Error_code* err_code = 0);

  /**
   * Equivalent to send_native_handle() but sends a graceful-close message instead of the usual payload; the opposing
   * peer's Native_handle_receiver shall receive it reliably and in-order via
   * Native_handle_receiver::async_receive_native_handle() in the form of
   * #Error_code = error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE.  If invoked after already invoking
   * `*end_sending()`, the method shall no-op and return `false` and neither an exception nor truthy
   * `*err_code`.  Otherwise it shall return `true` -- but potentially emit a truthy #Error_code (if an error is
   * detected).
   *
   * In addition: if `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns
   * `false` immediately instead and otherwise no-ops (logging aside).
   *
   * Informally one should think of async_end_sending() as just another message, which is queued after
   * preceding ones; but: it is the *last* message to be queued by definition.  It's like an EOF or a TCP-FIN.
   *
   * ### Synchronicity/blockingness guarantees ###
   * async_end_sending() is, unlike send_native_handle(), formally asynchronous.  The reason for this,
   * and the expected use pattern of async_end_sending() in the context of preceding send_native_handle()
   * calls, is found in the doc header for Native_handle_sender::send_native_handle().  We omit further discussion here.
   *
   * ### Error semantics ###
   * This section discusses the case where `true` is returned, meaning `*end_sending()` had not already
   * been called.
   *
   * An #Error_code is generated and passed as the sole arg to `on_done_func()`.
   * A falsy one indicates success.  A truthy one indicates failure; but in particular:
   *   - error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   *     spiritually identical to `boost::asio::error::operation_aborted`);
   *   - other arbitrary codes are possible, as with send_native_handle().
   *
   * Reminder: By the nature of `on_done_func()`, the pipe is finished regardless of whether it receives a
   * success or non-success #Error_code.
   *
   * ### Thread safety ###
   * The notes for send_native_handle() apply.
   *
   * @internal
   * The semantic re. calling `*end_sending()` after already having called it and having that exclusively
   * return `false` and do nothing was a judgment call.  As of this writing there's a long-ish comment at the top of
   * of `"sync_io::Native_socket_stream::Impl::*end_sending()"`" body discussing why I (ygoldfel) went that way.
   * @endinternal
   *
   * @tparam Task_err
   *         A functor type with signature identical to `flow::async::Task_asio_err`.
   * @param on_done_func
   *        See above.  This shall be invoked from an unspecified thread that is not the calling thread.
   * @return `false` if and only if *either* `*this` is not in PEER (connected, transmitting) state, *or*
   *         `*end_sending()` has already been called before (so this is a no-op, in both cases).
   *         Otherwise `true`.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * Equivalent to `async_end_sending(F)` wherein `F()` does nothing.
   *
   * Informally: If one uses this overload, it is impossible to guarantee everything queued has actually been sent,
   * as `on_done_func()` is the only mechanism for this.  Most likely this is only useful for test or proof-of-concept
   * code; production-level robustness typically requires one to ensure everything queued has been sent.  Alternatively
   * the user's application-level protocol may already guarantee the message exchange has completed (e.g., by receiving
   * an acknowledgment message of some sort) -- but in that case one can simply not call `*end_sending()`
   * at all.
   *
   * @return Same as in async_end_sending().
   */
  bool end_sending();

  /**
   * In PEER state: Irreversibly enables periodic auto-pinging of opposing receiver with low-level messages that
   * are ignored except that they reset any idle timer as enabled via Native_handle_receiver::idle_timer_run()
   * (or similar).  Auto-pings, at a minimum, shall be sent (subject to internal low-level transport would-block
   * conditions) at the following times:
   *   - as soon as possible after a successful auto_ping() call; and subsequently:
   *   - at such times as to ensure that the time between 2 adjacent sends of *any* message is no more than
   *     `period`, until `*end_sending()` (if any) or error (if any).
   * To clarify the 2nd point: If user messages are being sent already, an auto-ping may not be required or may be
   * delayed until the last-sent message plus `period`).  The implementation shall strive not to wastefully
   * add auto-ping traffic unnecessary to this goal but is not *required* to do so.  However, per the 1st point,
   * an auto-ping shall be sent near auto_ping() time to establish a baseline.
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).  If auto_ping() has already been called successfuly,
   * subsequently it will return `false` and no-op (logging aside).  If `*end_sending()` has been called succesfully,
   * auto_ping() will return `false` and no-op (logging side).
   *
   * ### Behavior past `*end_sending()` ###
   * As noted: auto_ping() returns `false` and no-ops if invoked after successful `*end_sending()`.
   * Rationale: If `*end_sending()` has been called, then the receiver shall (assuming no other error) receive
   * graceful-close (and hose the in-pipe as a result) as soon as possible, or it has already received it
   * (and hosed the pipe).  Therefore any potential "further" hosing of the pipe due to idle timer would be redundant
   * and ignored anyway given the nature of the Native_handle_receiver (and similar) API.
   *
   * ### Thread safety ###
   * The notes for send_native_handle() apply.
   *
   * @param period
   *        Pinging occurs so as to cause the opposing receiver to receive *a* message (whether auto-ping or user
   *        message) at least this frequently, subject to limitations of the low-level transport.
   *        The optional default is chosen by the impl to most reasonably work with the opposing `idle_timer_run()`
   *        to detect a zombified/overloaded peer.
   * @return `false` if `*this` is not in PEER (connected, transmitting) state, or if already called successfully
   *         in `PEER` state, or if `*end_sending()` was called successfully in `PEER` state; otherwise `true`.
   */
  bool auto_ping(util::Fine_duration period = default_value);
}; // class Native_handle_sender

/**
 * A documentation-only *concept* defining the behavior of an object capable of reliably/in-order *receiving* of
 * discrete messages, each containing a native handle, a binary blob, or both.  This is paired with
 * the Native_handle_sender concept which defines sending of such messages.
 *
 * Concept contents
 * ----------------
 * The concept defines the following behaviors/requirements.
 *   - The object has at least 2 states, NULL and PEER.  See notes in Native_handle_sender w/r/t this;
 *     they apply equally here.
 *   - The (incoming) transmission-of-messages methods, including reception of graceful-close message and
 *     cutting off any further receiving.  See their doc headers.
 *   - Behavior when the destructor is invoked.  See ~Native_handle_receiver() doc header.
 *   - Default ctor.  See notes in Native_handle_sender w/r/t this; they apply equally here.
 *   - `sync_io`-core adopting ctor.  Same deal.
 *   - Move ctor, move assigment operator.  Same deal.
 *
 * The concept (intentionally) does *not* define the following behaviors:
 *   - How to create a Native_handle_receiver, except the default, `sync_io`-core-adopting, and move ctors.
 *     Notes for Native_handle_sender apply equally here.
 *
 * @see Native_socket_stream: as of this writing one key class that implements this concept (and also
 *      Native_handle_sender) -- using the Unix domain socket transport.
 * @see Channel, a pipe-composing class template that potentially implements this concept.
 * @see Blob_receiver: a degenerate version of the present concept: capable of transmitting only blobs, not
 *      `Native_handle`s.
 *
 * Blob underflow semantics
 * ------------------------
 * This potentially important subtlety is regarding the interplay between
 * Native_handle_receiver::S_META_BLOB_UNDERFLOW_ALLOWED, async_receive_native_handle() `meta_blob.size()`,
 * and Native_handle_sender::send_native_handle() `meta_blob.size()`.
 *
 * Consider a `*this` named `R` and an opposing `*_sender` named `S`.  Note that `R.receive_meta_blob_max_size()
 * == S.send_meta_blob_max_size()`; call this limit `L`.  `L` shall always be in practice small enough to where
 * allocating a buffer of this size as the receive target is reasonable; hence kilobytes at most, not megabytes.
 * (In some cases, such as with `Blob_stream_mq_*`, it is configurable at runtime.  In others, as with
 * Native_socket_stream, it is a constant: sync_io::Native_socket_stream::S_MAX_META_BLOB_LENGTH.)
 *
 * As noted in that concept's docs, naturally, `S.send_native_handle()` shall yield immediate, non-pipe-hosing
 * error::Code::S_INVALID_ARGUMENT, if `meta_blob.size() > L`.  This is called overflow and is always enforced.
 *
 * Now consider `N = meta_blob.size()` for `R.async_receive_native_handle()`.  This is the size of the target
 * blob: how many bytes it *can* receive.  How `L` is enforced by `R` is not uniform among this concept's impls:
 * rather it depends on the compile-time constant Native_handle_receiver::S_META_BLOB_UNDERFLOW_ALLOWED.
 *
 * ### Semantics: `META_BLOB_UNDERFLOW_ALLOWED` is `false` ###
 * Suppose it is `false` (as is the case for Blob_stream_mq_receiver).  In that case `N < L` is *disallowed*:
 * `R.async_receive_native_handle()` *shall always* fail with non-pipe-hosing error::Code::S_INVALID_ARGUMENT
 * for that reason.  Certainly `N` cannot be zero -- not even if the incoming message is expected to
 * contain only a non-`.null() Native_handle`.
 *
 * @note Formally it's as written.  Informally the rationale for allowing this restriction is that some low-level
 *       transports have it; and working around it would probably affect performance and is unnatural.
 *       In particular Persistent_mq_handle::try_receive() (MQ receive) shall immediately fail if a target buffer
 *       is supplied smaller than Persistent_mq_handle::max_msg_size().  For both POSIX (`man mq_receive`) and
 *       bipc (`boost::interprocess::message_queue::try_receive()` docs and source code) it is disallowed at the
 *       low-level API level.
 *
 * Thus since `N >= L` is guaranteed before any actual receipt attempt is made, it is *impossible* for
 * a message to arrive such that its meta-blob's size would overflow the target blob (buffer) (of size `N`).
 * Therefore error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE shall not be emitted.
 *
 * ### Semantics: `META_BLOB_UNDERFLOW_ALLOWED` is `true` ###
 * Suppose it is `true` (as is the case for Native_socket_stream).  In that case `N < L` is allowed, in the sense
 * that `R.async_receive_native_handle()` *shall not* fail with non-pipe-hosing error::Code::S_INVALID_ARGUMENT
 * for that reason.  In particular, in that case, `N` can even be zero: this implies the incoming message *must*
 * contain a non-`.null() Native_handle` (it is not allowed to send a message with no content at all).
 * (In the case of degenerate concept Blob_sender, `N` must exceed zero, as it cannot transmit anything but
 * blobs.)
 *
 * However suppose a message has arrived, and its meta-blob's size -- which cannot exceed `L` -- is larger than
 * `N`.  I.e., it would *underflow* the user-supplied target buffer (blob).  In that case:
 *   - async_receive_native_handle() shall emit error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE.
 *   - This error shall be pipe-hosing (further receive attempts will yield the same error).
 *
 * Informal tips: Any blob-size error can be completely avoided by always supplying target blob with `N == L`.
 * (`N > L` will also work but may be considered wasteful, all else being equal.)  In addition this tactic will
 * ensure the code will generically work with a different Native_handle_receiver impl for which
 * `S_META_BLOB_UNDERFLOW_ALLOWED == false`.  Alternatively, the user protocol may be such that you simply know
 * that `S` will never send messages beyond some limit (smaller than `L`), perhaps for particular messages.
 * In that case `N` can be set to this smaller-than-`L` value.  However -- such code will fail
 * if the concept impl is later to switched to one with `S_META_BLOB_UNDERFLOW_ALLOWED == true`.  This may or
 * may not be an issue depending on your future dev plans.
 *
 * ### Summary ###
 * This dichotomy of possible semantics is a conscious choice.  Mode `false` is a necessity: some low-level transports
 * enforce it, period.  Mode `true` is to support a possible algorithmic desire to allocate smaller-than-`L`
 * target buffers.  The problem is some low-level transports allow this; but others don't.  We did not want to
 * impose one type's limitations on users of the other type.  Therefore, if your code is agnostic to the type
 * of transport, then code as-if `S_META_BLOB_UNDERFLOW_ALLOWED == false`.  Otherwise you may code otherwise.
 *
 * Rationale: Why no `end_receiving()`, given opposing concept has `*end_sending()`?
 * ---------------------------------------------------------------------------------
 * An early version of Native_handle_receiver and Blob_receiver did have an `end_receiving()`.
 * The idea was it would have 2 effects (plus 1 bonus):
 *   -# Any ongoing `async_receive_*()`s would be interrupted with in-pipe-hosing error
 *      error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE -- the same one as when receing a graceful-close
 *      from opposing `*end_sending()`.
 *   -# There would be no further reading from the low-level transport for any reason.
 *   -# There was also the *thought*, but not the action, that an impl could inform the *opposing sender* of the
 *      pipe being closed by the reader.  This would be conceptually similar to SIGPIPE in POSIX.  In practice, of
 *      the available low-level transports as of this writing, only Native_socket_stream could actually implement it,
 *      as the otherwise-full-duplex independent pipes do live in this same peer (Unix domain socket), so the out-pipe
 *      could be used to inform the other guy that *his* out-pipe (our in-pipe) is now pointless.  `Blob_stream_mq_*`
 *      are fully independent, operating on separate MQ kernel objects, so it was not realistically possible in
 *      that case.
 *
 * In order: (1) is fairly dubious: the local user should know when they've stopped receiving and don't need to
 * use this mechanism to inform themselves.  (3), as stated, sounded like a possible future improvement, but time
 * told us it was not going to work out: if only a 2-pipe transport can do it in any case, organizing a concept
 * around such a thing is just complicated.  If necessary the user can (and we think likely will) just arrange
 * their own protocol.  So that leaves (2).
 *
 * To begin with -- in reality user protocols tend to be designed to not need such measures.  If the reader is no
 * longer interested, writer will probably know that.  This is not networking after all: it is IPC.
 * That aside, though, it only makes sense in any case if an impl *chooses* to greedily cache all low-level
 * in-traffic in user RAM, even while no `async_receive_*()`s are pending to read it.  Early on we actually did do
 * that; but with the move to `sync_io`-pattern cores, and after contemplating
 * "Rationale: Why is send-native-handle not asynchronous?" (Native_handle_sender concept doc header),
 * we got rid of this waste of compute, letting unwanted in-traffic accumulate within a sender peer object
 * (Native_handle_sender, Blob_sender) only.  So in practice:
 *
 * If `end_receiving()` does not signal anything of value ((1) and (3)), and does not actually prevent any
 * low-level reading that would otherwise occur, then it is a pointless complication.  So that is why we got rid of it.
 */
class Native_handle_receiver
{
public:
  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.  Equals `_sender`'s.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /**
   * If `false` then `meta_blob.size() > receive_meta_blob_max_size()` in PEER-state async_receive_native_handle()
   * shall yield non-pipe-hosing error::Code::INVALID_ARGUMENT, and it shall never yield
   * pipe-hosing error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE; else the latter may occur, while the former
   * shall never occur for that reason.
   *
   * @see "Blob underflow semantics" in class concept doc header.
   */
  static constexpr bool S_META_BLOB_UNDERFLOW_ALLOWED = value;

  // Constructors/destructor.

  /**
   * Default ctor: Creates a peer object in NULL (neither connected nor connecting) state.  In this state,
   * all transmission-related methods (e.g., async_receive_native_handle()) shall return `false` and otherwise no-op
   * (aside from possible logging).
   *
   * All notes from Native_handle_sender() default ctor doc header apply here analogously.  They are helpful so
   * please read Native_handle_sender() doc header.
   */
  Native_handle_receiver();

  /**
   * `sync_io`-core-adopting ctor: Creates a peer object in PEER state by subsuming a `sync_io` core in that state.
   * That core must be as-if-just-cted (having performed no work and not been configured via `.start_*_ops()`).
   * That core object becomes as-if default-cted (therefore in NULL state).
   *
   * @param sync_io_core_in_peer_state_moved
   *        See above.
   */
  Native_handle_receiver(sync_io::Native_handle_receiver&& sync_io_core_in_peer_state_moved);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in NULL state).
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   */
  Native_handle_receiver(Native_handle_receiver&& src);

  /// Disallow copying.
  Native_handle_receiver(const Native_handle_receiver&) = delete;

  /**
   * Destroys this peer endpoint which will end the conceptual incoming-direction pipe (in PEER state, and if it's
   * still active) and cancels any pending completion handlers by invoking them ASAP with
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   * As of this writing these are the completion handlers that would therefore be called:
   *   - Any handler passed to async_receive_native_handle() that has not yet been invoked.
   *     There can be 0 or more of these.
   *
   * The pending completion handler(s) (if any) will be called from an unspecified thread that is not the calling
   * thread.  Any associated captured state for that handler will be freed shortly after the handler returns.
   *
   * The rest of the notes in ~Native_handle_sender() apply equally here.
   *
   * @see Native_handle_sender::~Native_handle_sender(): sister concept with essentially equal requirements.
   * @see Native_socket_stream::~Native_socket_stream(): implements concept (also implements just-mentioned sister
   *      concept).
   */
  ~Native_handle_receiver();

  // Methods.

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in NULL state).
   * No-op if `&src == this`.
   *
   * @see ~Native_handle_receiver().
   *
   * @param src
   *        Source object.  For reasonable uses of `src` after this ctor returns: see default ctor doc header.
   * @return `*this`.
   */
  Native_handle_receiver& operator=(Native_handle_receiver&& src);

  /// Disallow copying.
  Native_handle_receiver& operator=(const Native_handle_receiver&) = delete;

  /**
   * In PEER state: Returns min `target_meta_blob.size()` such that (1) async_receive_native_handle() shall not fail
   * with error::Code::S_INVALID_ARGUMENT (only if #S_META_BLOB_UNDERFLOW_ALLOWED is `false`; otherwise not relevant),
   * and (2) it shall *never* fail with error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE.  Please see
   * "Blob underflow semantics" for explanation of these semantics.
   *
   * Always the same value once in PEER state.  The opposing
   * Blob_sender::send_meta_blob_max_size() shall return the same value (in the opposing object potentially
   * in a different process).
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns zero; else
   * a positive value.
   *
   * @return See above.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * In PEER state: Asynchronously awaits one discrete message -- as sent by the opposing peer via
   * Native_handle_sender::send_native_handle() or `"Native_handle_sender::*end_sending()"` -- and
   * receives it into the given target locations, reliably and in-order.  The message is, therefore, one of the
   * following:
   *   - A binary blob; a native handle; or both.  This is indicated by `on_done_func(Error_code(), N)`.
   *     The falsy code indicates success; `N <= target_meta_blob.size()` indicates the number of bytes received into
   *     `target_meta_blob.data()` (zero means no blob was sent in the message).  `*target_hndl` is set
   *     (`target_hndl->null() == true` means no handle was sent in the message).
   *   - Graceful-close.  This is indicated by `on_done_func(error::code::S_RECEIVES_FINISHED_CANNOT_RECEIVE, 0)`;
   *     neither the target blob nor target native handle are touched.
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).
   *
   * ### Blob copying behavior; synchronicity/blockingness guarantees ###
   * `*target_hndl` and the area described by `target_meta_blob` must both remain valid until `on_done_func()`
   * executes.  The method itself shall be non-blocking.
   *
   * The implementation shall, informally, strive to *not* copy the received blob (if any) into
   * `target_meta_blob.data()...` except from the low-level transport mechanism.  That is: it shall strive to not
   * store the blob data in some internal buffer before ending up in `target_meta_blob.data()...`.
   *
   * ### Error semantics ###
   * An #Error_code is generated and passed as the 1st arg to `on_done_func()`.
   * A falsy one indicates success.  A truthy one indicates failure; but in particular:
   *   - error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   *     spiritually identical to `boost::asio::error::operation_aborted`);
   *   - only if #S_META_BLOB_UNDERFLOW_ALLOWED is `true`:
   *     - error::Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE (opposing peer has sent a message with a meta-blob exceeding
   *       `target_meta_blob.size()` in length; in particular one can give an empty buffer if no meta-blob expected);
   *   - error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE (peer gracefully closed pipe via `*end_sending()`);
   *   - error::Code::S_RECEIVER_IDLE_TIMEOUT (idle timeout: see idle_timer_run());
   *   - other arbitrary codes are possible.
   *   - No would-block-like error condition shall be emitted.
   *
   * A non-success on-done invocation means the incoming pipe is hosed.  (It is recommended they cease to use the pipe
   * and invoke the destructor to free resources.)  If such a non-success code is emitted, the same one shall be emitted
   * by all further async_receive_native_handle() calls on `*this`.  Key exception:
   *   - error::Code::S_INVALID_ARGUMENT:
   *     - This refers to an invalid argument to *this* method invocation.
   *       - If and only if #S_META_BLOB_UNDERFLOW_ALLOWED is `false`: A length floor on the target blob is imposed.
   *         Hence if `target_meta_blob.size() < receive_meta_blob_max_size()` then `S_INVALID_ARGUMENT` is emitted.
   *     - This shall *not* hose the pipe.  Subsequent async-receives may be attempted with reasonable hope of
   *       success.
   *     - At the impl's discretion, other conditions *may* lead to `S_INVALID_ARGUMENT`.  If so they must
   *       be documented.
   *
   * ### Thread safety ###
   * You may call this either from any thread including the unspecified thread running within another call's
   * `on_done_func()`.  It is *not* required to be safe to call concurrently with any Native_handle_receiver API,
   * including this one and the destructor, invoked on `*this`.
   *
   * @tparam Task_err_sz
   *         A functor type with signature identical to `flow::async::Task_asio_err_sz`.
   * @param target_hndl
   *        `*target_hndl` shall be set by the time `on_done_func()` is executed with a falsy code.  See above.
   * @param target_meta_blob
   *        `target_meta_blob.data()...` shall be written to by the time `on_done_func()` is executed with a falsy
   *        code, bytes numbering `N`, where `N` is passed to that callback.  `N` shall not exceed
   *        `target_meta_blob.size()`.  See above.
   * @param on_done_func
   *        See above.  This shall be invoked from an unspecified thread that is not the calling thread.
   * @return `false` if `*this` is not in PEER (connected, transmitting) state;
   *         otherwise `true`.
   */
  template<typename Task_err_sz>
  bool async_receive_native_handle(Native_handle* target_hndl,
                                   const util::Blob_mutable& target_meta_blob,
                                   Task_err_sz&& on_done_func);

  /**
   * In PEER state: Irreversibly enables a conceptual idle timer whose potential side effect is, once at least
   * the specified time has passed since the last received low-level traffic (or this call, whichever most
   * recently occurred), to emit the pipe-hosing error error::Code::S_RECEIVER_IDLE_TIMEOUT.  The implementation
   * shall guarantee the observer idle timeout is at least the provided value but may exceed this value for
   * internal reasons, as long as it's by a less than human-perceptible period (roughly speaking -- milliseconds,
   * not seconds).
   *
   * If `*this` is not in PEER state (in particular if it is default-cted or moved-from), returns `false` immediately
   * instead and otherwise no-ops (logging aside).  If idle_timer_run() has already been called successfuly,
   * subsequently it will return `false` and no-op (logging aside).
   *
   * ### Important: Relationship between idle_timer_run() and async_receive_native_handle() ###
   * idle_timer_run() is optional: if you have not called it, then the following does not apply.  If you *have* called
   * it:
   *
   * It will work usefully if and only if subsequently an async_receive_native_handle() is oustanding at least
   * once each `timeout`.  Informally this means it's best to immediately issue async_receive_native_handle() --
   * unless one is already oustanding -- and then as soon as that completes (sans error) issue at least one more;
   * and so on.  Why?  What are we talking about?  Simple: async_receive_native_handle() impl is *not* required
   * to read any more data from the low-level transport than is sufficient to satisfy the current
   * async_receive_native_handle() deficit.  (If it were required to do so, it would've been required to store copies
   * of incoming meta-blobs when there's no user-request deficit; we've consciously avoided unnecessary copying.)
   * So while there's no async_receive_native_handle() outstanding, `*this` will be considered idle; and once
   * that continues long enough for `timeout` to be exceeded, the idle timer will hose the in-pipe.
   *
   * So: If you plan to use idle_timer_run(), then you need to be ~always async-receiving.  Otherwise you'll risk
   * hitting idle-timeout, even as the other side diligently sends stuff (`auto_ping()` or otherwise).
   *
   * ### Error semantics ###
   * If and only if the timeout does occur down the line, the aforementioned error will be emitted via
   * async_receive_native_handle() (or similar) handler.  It shall be treated as the reason to hose the pipe
   * (assuming it was not hosed by something else earlier).
   *
   * ### Thread safety ###
   * You may call this either from any thread including the unspecified thread running within another call's
   * `on_done_func()`.  It is *not* required to be safe to call concurrently with any API, including this one
   * and the destructor, invoked on `*this`.
   *
   * ### Suggested use ###
   * Informally: There are roughly two approaches to using idle_timer_run().
   *
   * Firstly it can be used to gauge the state of the opposing process; if no auto-pings are arriving regularly,
   * then the opposing Native_handle_sender (or similar) must be zombified or overloaded.  To use it in this
   * capacity, typically one must use Native_handle_sender::auto_ping() (or similar) on the opposing side.
   * Typically one would then leave `timeout` at its suggested default value.
   *
   * Secondly it can be used to more generally ensure some application algorithm on the opposing side (presumably
   * cooperating with the algorithm on the local side) is sending messages with expected frequency.
   * That is, one would not use `auto_ping()` but rather send their own messages in a way that makes sense for
   * the applications' algorithm.
   *
   * @param timeout
   *        The idle timeout to observe.
   *        The optional default is chosen by the impl to most reasonably work with the opposing `auto_ping()`
   *        to detect a zombified/overloaded peer.
   * @return `false` if `*this` is not in PEER (connected, transmitting) state, or if already called successfully
   *         in `PEER` state; otherwise `true`.
   */
  bool idle_timer_run(util::Fine_duration timeout = default_value);
}; // class Native_handle_receiver

} // namespace ipc::transport
