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
 * A documentation-only *concept* defining the behavior of an object representing a light-weight handle
 * to a message queue, capable of receiving/sending discrete messages in non-blocking/blocking/timed-blocking
 * fashion, as well as some support for polling/waiting and interruptions thereof.  The message queue (MQ)
 * is assumed to be of at least kernel persistence, meaning a handle to an MQ
 * closing does not delete the MQ, and another handle may reattach to it; in order to delete the underlying MQ
 * a separate remove op is needed (and is provided as well as a `static` method).
 *
 * ### How to use ###
 * The following are relevant operations to do with MQs.  (Read Rationale for background.)
 *   - Create (named) MQ: Persistent_mq_handle() ctor, with either util::Create_only or util::Open_or_create tag arg.
 *     A Shared_name specifies the name.
 *     If one does not need a handle simply destroy the resulting object: the underlying created MQ continues existing.
 *   - Create handle to the (named) MQ: Persistent_mq_handle() ctor, with either util::Open_only or
 *     util::Open_or_create tag arg.
 *     A Shared_name specifies the name.
 *   - Send message into the MQ, via handle: try_send() (non-blocking), send() (blocking), timed_send() (blocking with
 *     timeout).
 *     - Await writability without writing: is_sendable() (instant poll), wait_sendable() (blocking),
 *       timed_wait_sendable() (blocking with timeout).
 *     - Interrupt and/or preempt all above blocking operations including concurrently/preempt is_sendable():
 *       interrupt_sends().  Undo: allow_sends().
 *   - Receive message out of the MQ, via handle: try_receive() (non-blocking), receive() (blocking),
 *     timed_receive() (blocking with timeout).
 *     - Await readability without reading: is_receivable() (instant poll), wait_receivable() (blocking),
 *       timed_wait_receivable() (blocking with timeout).
 *     - Interrupt and/or preempt all above blocking operations including concurrently/preempt is_receivable():
 *       interrupt_receives().  Undo: allow_receives().
 *   - Destroy handle to the MQ: ~Persistent_mq_handle() destructor.  The underlying MQ continues existing.
 *   - Destroy (named) MQ: remove_persistent().  Underlying MQ name disappears, but MQ continues existing until
 *     all handles machine-wide are closed.  A Shared_name specifies the name.
 *
 * ### Thread safety ###
 * Concurrent ops safe on different objects (including when the same-named queue is being accessed).
 * Concurrent non-`const` ops (where at least 1 is non-`const`) not safe on the same `*this`.
 *
 * The notable exception to the latter: each of `interrupt_*()` and `allow_*()` is safe to call on the same
 * `*this` concurrently to any method except `interrupt_*()` and `allow_*()` themselves.  In fact
 * `interrupt_*()` ability to interrupt a concurrent timed-waiting of infinitely-waiting read or write is
 * most of those methods' utility.
 *
 * ### Rationale ###
 * Here's why this concept exists and how it was designed to be the way it is.  It's actually quite simple;
 * the only question is why it is this way and not some other way.  Update: It's somewhat less simple now but not bad.
 *
 * ipc::transport features the Blob_sender and Blob_receiver concepts, each of which is a peer endpoint of a
 * connected one-way pipe capable of transmitting discrete messages, each consisting of a binary blob.
 * They are first-class citizens of ipc::transport, or at least of its core-layer portion, meaning they're likely
 * to be used by the user directly or at least in highly visible fashion by another aspect of ipc::transport impl.
 * They have a vaguely socket-like API, with an asynchronous receive method and a convenient non-blocking/synchronous
 * sender method (that cannot yield would-block).  Lastly the destructor of either ensures the underlying transport
 * cannot be used again by somehow opening another handle to the same thing.
 *
 * One implementation of Blob_sender and Blob_receiver simultaneously might be Native_socket_stream which
 * uses Unix domain stream connection as underlying transport.  That one is a natural fit for the concept,
 * including the fact that closing a Unix domain socket hoses the connection entirely (one cannot somehow "reattach"
 * to it).  It's good.  However, benchmark tests (details omitted) show that, when need not also send native handles
 * over such a connection, *message queue* low-level transports might be more performant or at least competitive.
 *
 * In our ecosystem, two message queue (MQ) transports are candidates: the POSIX message queue (provided by Linux)
 * (see `man mq_overview`) and the boost.interprocess (bipc) message queue (web-search for
 * `boost::interprocess::message_queue`).  We wanted to support both and then provide the choice of which one to use.
 * So the goal is to write Blob_sender and Blob_receiver concept impls for each type of MQ (and possibly others
 * over time).  So how to do that nicely?
 *
 * The 2 low-level MQ APIs (POSIX MQ and bipc MQ) are extremely different, conceptually and in practice, from
 * stream socket APIs of any kind.  For one thing, the MQs have kernel persistence -- they're like files in memory that
 * disappear at reboot but otherwise stay around, so one can reattach a handle to one even after closing another one;
 * and deleting it is a separate operation not involving any handle.  For another, they don't use socket-descriptors
 * and hence do not participate in `epoll/poll/select()` mechanisms and feature only blocking, non-blocking, and
 * blocking-with-timeout send and receive APIs.  (POSIX MQ does have `mq_notify()`, but this async-notify mechanism
 * is quite limited, either creating a new thread for each notification, or using a signal.  POSIX MQ in Linux
 * *does* happen to have a non-portable property wherein the descriptor is really an FD, so it *can* participate
 * in `epoll/poll/select()` mechanisms; and boost.asio *does* have a `posix::descriptor` that can wrap such a
 * native descriptor.  However: bipc MQ lacks anything like this at all.)
 *
 * The 2 APIs are, however, extremely similar to each other.  In fact, one gets the impression that the authors
 * of bipc MQ considered wrapping POSIX MQs but reconsidered for whatever reason (maybe the obscure limit semantics,
 * lack of availability in all Linux versions and other Unixes and Windows) -- but fairly closely mirrored the API
 * therein.
 *
 * Knowing this, it looked pretty natural to write some Blob_sender/Blob_receiver impls holding
 * some `bipc::message_queue` handles internally.  However, while conceptually similar, the POSIX MQ API is a
 * C API, not a Boost-style C++ thing.  It would be entirely possible to still write Blob_sender/Blob_receiver impls
 * around this API, but since conceptually it's so similar to bipc MQ, why waste the effort?  Code reuse for the win.
 *
 * That brings us to this concept.  What it is is: A thing I (ygoldfel) came up with by simply taking
 * `bipc::message_queue` and working backwards, making a concept for an MQ handle, Persistent_mq_handle, to which
 * `bipc::message_queue` directly conforms already in most key ways (to wit: the send/receive methods).
 * Then one could write a very thin non-polymorphic HAS-A wrapper class (with no added data stored) and a couple other
 * utility functions with very thin adaptations to match ipc::transport style (like using Shared_name instead of
 * strings when creating or removing kernel-persistent queue instances).  This class is Bipc_mq_handle; it
 * implements this Persistent_mq_handle.
 *
 * Next, a class can be written to mirror this API but for POSIX MQs.  This is a little harder, since it can't just
 * expose some existing super-class's send/receive functions, but it would just wrap the Linux C API and store
 * an MQ handle; done.  This class is Posix_mq_handle; it also implements Persistent_mq_handle.
 *
 * Now that Persistent_mq_handle provides a uniform API, a Blob_sender/Blob_receiver can be written around a
 * `typename Persistent_mq_handle` which can be either Bipc_mq_handle or Posix_mq_handle (or a future wrapper around
 * something else).
 *
 * ### Poll/wait and interrupt facilities ###
 * Somewhat later we added a major feature to the concept (and both known impls): For each type of transmission
 * operation in a given direction (instant poll, blocking, blocking with timeout), there is a poll/wait counterpart
 * which is almost the same except it does not actually transmit but merely returns the fact transmission
 * is possible.  E.g., try_receive() <=> is_receivable(), receive() <=> wait_receivable(),
 * timed_receive() <=> timed_wait_receivable().  In fact -- in the absence of a competing `*this` --
 * receive() = wait_receivable() + try_receive() (succeeds), timed_receive() = timed_wait_receivable() + try_receive()
 * (succeeds).
 *
 * This is useful at least in that it allows one to defer deciding on a target buffer for receiving, until
 * receiving would (probably) actually work.  In the other direction it can still provide useful flexibility in
 * a multi-threaded setup (one thread blockingly-waits, the other is signaled about readiness and tries to
 * transmit but without blocking -- useful in a thread U/thread W setup).
 *
 * Lastly, any blocking (with or without timeout) can be interrupted via the `interrupt_*()` methods.
 * For example a receive() or wait_receivable() may be ongoing in thread W, and thread U can interrupt_receives()
 * to immediately make the blocking op exit with error::Code::S_INTERRUPTED.
 *
 * ### Discussion: The road to boost.asio-like async I/O ###
 * What would be cool, which isn't here, is if this concept described a boost.asio I/O object, capable of plugging
 * into a boost.asio loop; or ~equivalently something a-la Blob_sender and/or Blob_receiver.  However that was not
 * our aim -- we only want a thin wrapper as noted in "Rationale" above -- and Blob_stream_mq_receiver and
 * Blob_stream_mq_sender achieve it (and arguably more -- albeit with the limit of a single writer and single reader).
 * That said: it may be useful to contemplate what parts the concept does have that are conducive to that type of work.
 *
 * Without the poll/wait facilities combined with interrupt facilities, it was possible but somewhat clunky,
 * having to at least start a background thread in which to perform blocking transmit calls; they'd have to
 * be broken up into subject-to-timeout shorter calls, so that the thread could be stopped and joined during
 * deinit.  Even then full deinit could only occur with a potential pause until the current short call in the
 * background thread W could return.  By having thread W do (indefinite) waits only, and allowing thread U to
 * do non-blocking transmits only *and* `interrupt_*()`, we achieve pretty good flexibility and responsiveness.
 * That is what Blob_stream_mq_sender and Blob_stream_mq_receiver can do given the present concept.
 *
 * However -- it *can* be more advanced still.  Consider the specific impl Posix_mq_handle, which has the
 * additional-to-concept (edit: see below update) method Posix_mq_handle::native_handle(); in Linux a #Native_handle.
 * This can be waited-on, natively with `[e]poll*()`; with boost.asio via util::sync_io::Asio_waitable_native_handle
 * (more or less a `boost::asio::posix::descriptor`).  *Now* no background thread W is necessary: thread U
 * can ask the *kernel* to report readability/writability -- when active it can do non-blocking stuff.
 *
 * The wrinkle: Posix_mq_handle has it; but Bipc_mq_handle does not (and there is no FD inside its impl either;
 * it is fully SHM-based internally).  Can it be generalized nevertheless?  Yes and no.  Yes: in that it can be
 * simulated by "secretly" having a thread W and having it use a pipe (or something) to translate readable/writable
 * events into a live FD that could be detected via `[e]poll*()` or boost.asio.  No: in that it turns a
 * hypothetical Persistent_mq_handle impl, namely Bipc_mq_handle, into something complex as opposed to any kind
 * of thin wrapper around an MQ API.  Therefore we did not do it.
 *
 * However -- we did ~such a thing with sync_io::Blob_stream_mq_sender and sync_io::Blob_stream_mq_receiver which
 * are templated on Persistent_mq_handle as a template-parameter; and, being
 * `sync_io`-pattern-impls (see util::sync_io), they each expose a waitable-on #Native_handle.
 * Indeed as of this writing each of these templates keeps a "secret" thread W that performs
 * blocking waits, while the user-accessible API makes it look like a nice, kernel-reported-via-FDs
 * reactor/proactor-supporting I/O object.  By operating on what is directly available via the present concept,
 * this setup of Blob_stream_mq_sender and Blob_stream_mq_receiver internals is agnostic to the type of MQ.
 *
 * However, if one wanted to take advantage of the non-concept (edit: see below update) ability to be watched
 * (via FD) with the kernel's help and without an added thread, they could specialize `Blob_stream_mq_*er`
 * for Posix_mq_handle which does offer a kernel-FD accessor `.native_handle()`.  Update: This is now done
 * (in fact it is not specialized as of this writing but rather uses a few simple `if constexpr
 * ()`s).  Accordingly the concept now allows for `native_handle()` optionally: see
 * Persistent_mq_handle::S_HAS_NATIVE_HANDLE.
 */
class Persistent_mq_handle // Note: movable but not copyable.
{
public:
  // Constants.

  /// Shared_name relative-folder fragment (no separators) identifying this resource type.
  static const Shared_name S_RESOURCE_TYPE_ID;

  /// `true` if and only if native_handle() method exists, and the returned value may be waited-on by `poll()`/etc.
  static constexpr bool S_HAS_NATIVE_HANDLE = unspecified;

  // Constructors/destructor.

  /**
   * Construct null handle, suitable only for being subsequently moved-to or destroyed.
   * If you do anything on `*this`, other than invoking dtor or move-assignment, behavior is undefined.
   */
  Persistent_mq_handle();

  /**
   * Construct handle to non-existing named MQ, creating it first.  If it already exists, it is an error.
   * If an error is emitted via `*err_code`, and you do anything on `*this` other than invoking dtor or
   * move-assignment, behavior is undefined.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param absolute_name
   *        Absolute name at which the persistent MQ lives.
   * @param mode_tag
   *        API-choosing tag util::CREATE_ONLY.
   * @param perms
   *        Permissions to use for creation.  Suggest the use of util::shared_resource_permissions() to translate
   *        from one of a small handful of levels of access; these apply almost always in practice.
   *        The applied permissions shall *ignore* the process umask and shall thus exactly match `perms`,
   *        unless an error occurs.
   * @param max_n_msg
   *        Max # of unpopped messages in created queue.
   * @param max_msg_sz
   *        Max # of bytes in any one message in created queue.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   */
  explicit Persistent_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                util::Create_only mode_tag, size_t max_n_msg, size_t max_msg_sz,
                                const util::Permissions& perms = util::Permissions(),
                                Error_code* err_code = 0);

  /**
   * Construct handle to existing named MQ, or else if it does not exist creates it first and opens it (atomically).
   * If an error is emitted via `*err_code`, and you do anything on `*this` other than invoking dtor or
   * move-assignment, behavior is undefined.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param absolute_name
   *        Absolute name at which the persistent MQ lives.
   * @param mode_tag
   *        API-choosing tag util::OPEN_OR_CREATE.
   * @param perms_on_create
   *        Permissions to use if creation is required.  Suggest the use of util::shared_resource_permissions() to
   *        translate from one of a small handful of levels of access; these apply almost always in practice.
   *        The applied permissions shall *ignore* the process umask and shall thus exactly match `perms_on_create`,
   *        unless an error occurs.
   * @param max_n_msg_on_create
   *        Max # of unpopped messages in created queue if creation is required.
   * @param max_msg_sz_on_create
   *        Max # of bytes in any one message in created queue if creation is required.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions.
   */
  explicit Persistent_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                util::Open_or_create mode_tag, size_t max_n_msg_on_create, size_t max_msg_sz_on_create,
                                const util::Permissions& perms_on_create = util::Permissions(),
                                Error_code* err_code = 0);

  /**
   * Construct handle to existing named MQ.  If it does not exist, it is an error.
   * If an error is emitted via `*err_code`, and you do anything on `*this` other than invoking dtor or
   * move-assignment, behavior is undefined.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param absolute_name
   *        Absolute name at which the persistent MQ lives.
   * @param mode_tag
   *        API-choosing tag util::OPEN_ONLY.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely it already existed.
   */
  explicit Persistent_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name,
                                util::Open_only mode_tag, Error_code* err_code = 0);

  /**
   * Constructs handle from the source handle while making the latter invalid.
   * If you do anything on `src` after this, other than invoking dtor or move-assignment, behavior is undefined.
   *
   * Informally: this is a light-weight op.
   *
   * @param src
   *        Source object which is nullified.
   */
  Persistent_mq_handle(Persistent_mq_handle&& src);

  /// Copying of handles is prohibited.
  Persistent_mq_handle(const Persistent_mq_handle&) = delete;

  /**
   * Destroys this handle (or no-op if no handle was successfully constructed, or if it's a moved-from or default-cted
   * handle).  The underlying MQ (if any) is *not* destroyed and can be attached-to by another handle.
   */
  ~Persistent_mq_handle();

  // Methods.

  /**
   * Replaces handle with the source handle while making the latter invalid.
   * If you do anything on `src` after this, other than invoking dtor or move-assignment, behavior is undefined.
   *
   * Informally: this is a light-weight op.
   *
   * @param src
   *        Source object which is nullified.
   * @return `*this`.
   */
  Persistent_mq_handle& operator=(Persistent_mq_handle&& src);

  /// Copying of handles is prohibited.
  Persistent_mq_handle& operator=(const Persistent_mq_handle&) = delete;

  /**
   * Removes the named persistent MQ.  The name `name` is removed from the system immediately; and
   * the function is non-blocking.  However the underlying MQ if any continues to exist until all handles to it are
   * closed; their presence in this or other process is *not* an error.
   *
   * @see `util::remove_each_persistent_*`() for a convenient way to remove more than one item.  E.g.,
   *      `util::remove_each_persistent_with_name_prefix<Pool_arena>()` combines remove_persistent() and
   *      for_each_persistent() in a common-sense way to remove only those `name`s starting with a given prefix;
   *      or simply all of them.
   *
   * Trying to remove a non-existent name *is* an error.
   *
   * Logs INFO message.
   *
   * @warning The impl should be carefully checked to conform to this.  As of this writing the 2 relevant
   *          low-level MQ APIs (bipc and POSIX) do, but there could be more.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param name
   *        Absolute name at which the persistent MQ lives.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& name,
                                Error_code* err_code = 0);

  /**
   * Lists all named persistent MQs currently persisting, invoking the given handler synchronously on each one.
   *
   * Note that, in a sanely set-up OS install, all existing pools will be listed by this function;
   * but permissions/ownership may forbid certain operations the user may typically want to invoke on
   * a given listed name -- for example remove_persistent().  This function does *not* filter-out any
   * potentially inaccessible items.
   *
   * @tparam Handle_name_func
   *         Function object matching signature `void F(const Shared_name&)`.
   * @param handle_name_func
   *        `handle_name_func()` shall be invoked for each (matching, if applicable) item.  See `Handle_name_func`.
   */
  template<typename Handle_name_func>
  static void for_each_persistent(const Handle_name_func& handle_name_func);

  /**
   * Non-blocking send: pushes copy of message to queue and returns `true`; if queue is full then no-op and returns
   * `false`.  A null blob (`blob.size() == 0`) is allowed.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob.size()` exceeds message size limit (if any), a particular error, which shall be
   * documented below, is emitted; this is not fatal to `*this`.
   *
   * @param blob
   *        Buffer to copy into MQ; if empty then an empty message is pushed.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (excessive-size error: buffer exceeds
   *        max_msg_size()).
   * @return `true` on success; `false` on failure; in the latter case `*err_code` distinguishes
   *         between would-block and fatal error.
   */
  bool try_send(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Blocking send: pushes copy of message to queue; if queue is full blocks until it is not.
   * A null blob (`blob.size() == 0`) is allowed.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob.size()` exceeds message size limit (if any), a particular error, which shall be
   * documented below, is emitted; this is not fatal to `*this`.  Exception to this: interrupt_sends()
   * leads to the emission of a particular error which shall be documented below; this is not fatal to `*this.
   *
   * @param blob
   *        See try_send().
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (excessive-size error: buffer exceeds
   *        max_msg_size()).
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_sends()).
   */
  void send(const util::Blob_const& blob, Error_code* err_code = 0);

  /**
   * Blocking timed send: pushes copy of message to queue; if queue is full blocks until it is not, or the
   * specified time passes, whichever happens first.  Returns `true` on success; `false` on timeout or error.
   * A null blob (`blob.size() == 0`) is allowed.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob.size()` exceeds message size limit (if any), a particular error, which shall be
   * documented below, is emitted; this is not fatal to `*this`.  Exception to this: interrupt_sends()
   * leads to the emission of a particular error which shall be documented below; this is not fatal to `*this.
   *
   * @warning The user must not count on precision/stability -- unlike with, say, boost.asio timers -- here.
   *          If timing precision is required, the user will have to add an async layer with more precise timing
   *          and not rely on this.  For example, informally, suggest: Use timed_send() with 250msec increments
   *          to model an interruptible indefinitely-blocking send().  In a parallel thread, if it's time to
   *          interrupt the modeled endless send(), set some flag that'll cause the 250msec-long attempts to cease.
   *          The last one (that straddles the interruption point) can just be ignored.
   *
   * @param blob
   *        See try_send().
   * @param timeout_from_now
   *        Now + (this long) = the timeout time point.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.  Timeout shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (excessive-size error: buffer exceeds
   *        max_msg_size()).
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_sends()).
   * @return `true` on success; `false` on failure; in the latter case `*err_code` distinguishes
   *         between timeout and fatal error.
   */
  bool timed_send(const util::Blob_const& blob, util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Equivalent to try_send() except stops short of writing anything, with `true` result indicating that
   * try_send() *would* work at that moment.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_sends() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted by interrupt_sends()).
   * @return `true` if transmissible; `false` if not, or on error.
   */
  bool is_sendable(Error_code* err_code = 0);

  /**
   * Equivalent to send() except stops short of writing anything, with non-error return indicating that
   * try_send() *would* work at that moment.  It shall block indefinitely until fatal error, or
   * error::Code::S_INTERRUPTED, or transmissibility.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_sends() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_sends()).
   */
  void wait_sendable(Error_code* err_code = 0);

  /**
   * Equivalent to timed_send() except stops short of writing anything, with `true` result indicating that
   * try_send() *would* work at that moment.  It shall block until fatal error, or
   * error::Code::S_INTERRUPTED, or timeout, or transmissibility.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_sends() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param timeout_from_now
   *        See timed_send().
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_sends()).
   * @return `true` if transmissible; `false` if not, or on error or timeout; in the latter case `*err_code`
   *         distinguishes between timeout and fatal error.
   */
  bool timed_wait_sendable(util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Non-blocking receive: pops copy of message from queue into buffer and returns `true`; if queue is empty then no-op
   * and returns `false`.  On `true` `blob->size()` is modified to store the # of bytes received.
   * A null blob (`blob->size() == 0` post-condition) is possible.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob->size()` is less than the required message size (or upper limit on this, if any),
   * a particular error, which shall be documented below, is emitted; this is not fatal to `*this`.
   *
   * @param blob
   *        Buffer into which to copy into MQ and whose `->size()` to update to the # of bytes received;
   *        if message empty it is set to zero.  Original `->size()` value indicates capacity of buffer;
   *        if this is insufficient based on either the popped message size or an upper limit (if any), it is an error.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (size-underflow error: buffer is smaller than
   *        max_msg_size()).
   * @return `true` on success; `false` on failure; in the latter case `*err_code` distinguishes
   *         between would-block and fatal error.
   */
  bool try_receive(util::Blob_mutable* blob, Error_code* err_code = 0);

  /**
   * Blocking receive: pops copy of message from queue into buffer; if queue is empty blocks until it is not.
   * On `true` `blob->size()` is modified to store the # of bytes received.
   * A null blob (`blob->size() == 0` post-condition) is possible.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob->size()` is less than the required message size (or upper limit on this, if any),
   * a particular error, which shall be documented below, is emitted; this is not fatal to `*this`.
   * Exception to this: interrupt_receives()
   * leads to the emission of a particular error which shall be documented below; this is not fatal to `*this.
   *
   * @param blob
   *        See try_receive().
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (size-underflow error: buffer is smaller than
   *        max_msg_size()).
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_receives()).
   */
  void receive(util::Blob_mutable* blob, Error_code* err_code = 0);

  /**
   * Blocking timed receive: pops copy of message from queue into buffer; if queue is empty blocks until it is not, or
   * the specified time passes, whichever happens first.  Returns `true` on success; `false` on timeout or error.
   * On `true` `blob->size()` is modified to store the # of bytes received.
   * A null blob (`blob->size() == 0` post-condition) is possible.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: If `blob->size()` is less than the required message size (or upper limit on this, if any),
   * a particular error, which shall be documented below, is emitted; this is not fatal to `*this`.
   *
   * @warning Read the warning on timed_send() about the accuracy of the timeout.  Same applies here.
   *
   * @param blob
   *        See try_receive().
   * @param timeout_from_now
   *        Now + (this long) = the timeout time point.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.  Timeout shall not be emitted.
   *        error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW (size-underflow error: buffer is smaller than
   *        max_msg_size()).
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_receives()).
   * @return `true` on success; `false` on failure; in the latter case `*err_code` distinguishes
   *         between timeout and fatal error.
   */
  bool timed_receive(util::Blob_mutable* blob, util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Equivalent to try_receive() except stops short of reading anything, with `true` result indicating that
   * try_receive() *would* work at that moment.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_receives() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted by interrupt_receives()).
   * @return `true` if transmissible; `false` if not, or on error.
   */
  bool is_receivable(Error_code* err_code = 0);

  /**
   * Equivalent to receive() except stops short of reading anything, with non-error return indicating that
   * try_receive() *would* work at that moment.  It shall block indefinitely until fatal error, or
   * error::Code::S_INTERRUPTED, or transmissibility.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_receives() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_receives()).
   */
  void wait_receivable(Error_code* err_code = 0);

  /**
   * Equivalent to timed_receive() except stops short of reading anything, with `true` result indicating that
   * try_receive() *would* work at that moment.  It shall block until fatal error, or
   * error::Code::S_INTERRUPTED, or timeout, or transmissibility.
   *
   * If error is emitted, `*this` shall be considered hosed: Behavior is undefined except dtor or move-assignment.
   * Exception to this: interrupt_receives() leads to the emission of a particular error which shall be documented
   * below; this is not fatal to `*this.
   *
   * @param timeout_from_now
   *        See timed_receive().
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Would-block shall not be emitted.
   *        error::Code::S_INTERRUPTED (preempted or interrupted by interrupt_receives()).
   * @return `true` if transmissible; `false` if not, or on error or timeout; in the latter case `*err_code`
   *         distinguishes between timeout and fatal error.
   */
  bool timed_wait_receivable(util::Fine_duration timeout_from_now, Error_code* err_code = 0);

  /**
   * Enables sends-interrupted mode: is_sendable() (future calls), send() (future or concurrent calls),
   * timed_send() (ditto), wait_sendable() (ditto), timed_wait_sendable() (ditto) shall emit
   * error::Code::S_INTERRUPTED as soon as possible.
   *
   * @return `true` on success; `false` is already enabled.
   */
  bool interrupt_sends();

  /**
   * Disables mode enabled by interrupt_sends().
   *
   * @return `true` on success; `false` if already disabled.
   */
  bool allow_sends();

  /**
   * Enables receives-interrupted mode: is_receivable() (future calls), receive() (future or concurrent calls),
   * timed_receive() (ditto), wait_receivable() (ditto), timed_wait_receivable() (ditto) shall emit
   * error::Code::S_INTERRUPTED as soon as possible.
   *
   * @return `true` on success; `false` is already enabled.
   */
  bool interrupt_receives();

  /**
   * Disables mode enabled by interrupt_receives().
   *
   * @return `true` on success; `false` if already disabled.
   */
  bool allow_receives();

  /**
   * Returns name equal to `absolute_name` passed to ctor.
   * @return See above.
   */
  const Shared_name& absolute_name() const;

  /**
   * Returns the max message size of the underlying queue.  This should at least approximately equal
   * what was passed to ctor when creating the MQ; however the underlying transport may tweak it, such as rounding
   * to page size.
   *
   * @return See above.
   */
  size_t max_msg_size() const;

  /**
   * Returns the max message count of the underlying queue.  This should at least approximately equal
   * what was passed to ctor when creating the MQ; however the underlying transport may tweak it if it desires.
   *
   * @return See above.
   */
  size_t max_n_msgs() const;

  /**
   * Available if and only if #S_HAS_NATIVE_HANDLE is `true`: Returns the stored native MQ handle; null means a
   * constructor failed, or `*this` has been moved-from, hence the handle is to not-a-queue.
   * See class doc header for important background discussion w/r/t the rationale behind this method.
   *
   * Medium-length story short: this is (in Linux impl of POSIX MQ) really an FD and as such can
   * be wrapped via `boost::asio::posix::descriptor` and hence participate
   * in a `Task_engine` event loop which internally will use `epoll()` together with all the other waited-on
   * resources; or simply be `poll()`ed (etc.).  E.g., boost.asio `async_wait()` for readability;
   * then invoke `this->try_receive()` once it is readable.
   *
   * Behavior is undefined if you operate on this value, such as sending or receiving, concurrently to any
   * transmission APIs acting on `*this`.
   *
   * @note It is possible to get away with not using this by using the `*_sendable()` and `*_receivable()`
   *       methods.  However the blocking and timed variations cannot be straightforwardly interrupted.
   *
   * @return See above.
   */
  Native_handle native_handle() const;
}; // class Persistent_mq_handle

// Free functions.

/**
 * Swaps two objects.  Constant-time.  Suitable for standard ADL-swap pattern `using std::swap; swap(val1, val2);`.
 *
 * @relatesalso Persistent_mq_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Persistent_mq_handle& val1, Persistent_mq_handle& val2);

} // namespace ipc::transport
