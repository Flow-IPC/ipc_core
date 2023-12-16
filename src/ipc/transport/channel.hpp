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

#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/transport/native_socket_stream.hpp"
#include "ipc/transport/sync_io/blob_stream_mq_snd.hpp"
#include "ipc/transport/blob_stream_mq_snd.hpp"
#include "ipc/transport/sync_io/blob_stream_mq_rcv.hpp"
#include "ipc/transport/blob_stream_mq_rcv.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/util/process_credentials.hpp"

namespace ipc::transport
{

// Types.

/**
 * Peer to a bundle of 1-2 full-duplex pipe(s), one for transmitting unstructured binary blobs; the other for
 * transmitting native handle+blob combos; hence a Blob_sender + Blob_receiver, a Native_handle_sender +
 * Native_handle_receiver, or both.  Generally a session::Session shall be in charge of opening
 * such `Channel`s between processes, while the user will likely wrap each Channel in a struc::Channel in order
 * to exchange structured (schema-based) messages, at times along with native sockets, through those `Channel`s.
 * (However this is by no means required.  One can use the `Channel` -- whether bundling async-I/O peer objects or
 * their `sync_io::` counterparts -- directly (in unstructured form) as well.)
 *
 * ### Main use: bundler of peer sender/receiver objects ###
 *
 * This bundling of the local peer objects of 1-2 pipes is the Channel template's core functionality;
 * it is therefore (data-wise -- but code-wise as well) an *extremely* thin wrapper around the stored 2-4 peer
 * objects.  At its core it provides:
 *   - construction in ??? state;
 *   - accessors for the peer objects (`Blob_sender`/`Blob_receiver`, which may be the same object or not; or
 *     `Native_handle_sender`/`Native_handle_receiver`, ditto; or both);
 *   - simple API for loading up the objects that shall be returned by those accessors.
 *
 * @note A key point is that, since we're really just storing 2-4 pointers (+ a nickname and a `Logger`),
 *       there is important freedom as to the pointees' *types*.  The `Blob_sender` can be either Null_peer
 *       (i.e., unused), a transport::Blob_sender (async-I/O pattern), *or* a transport::sync_io::Blob_sender
 *       (`sync_io` pattern; see util::sync_io doc header for background).  (And analogously for
 *       each of `Blob_receiver`, `Native_handle_sender`, `Native_handle_receiver`.)  So in other words
 *       a `Channel` can bundle async-I/O peer objects *or* `sync_io` peer objects.  You may use
 *       Channel::S_IS_SYNC_IO_OBJ or Channel::S_IS_ASYNC_IO_OBJ to determine which is the case at compile-time
 *       for a given Channel type.
 *
 * Accordingly it provides a very useful method: async_io_obj().  If you have a `sync_io`-pattern-peer-storing
 * Channel `x` (i.e., `"decltype(x)::S_IS_SYNC_IO_OBJ == true"`), simply call `x.async_io_obj()` to create/return
 * an async-I/O version of it.  For example, is `x` was
 * `Channel<transport::sync_io::Native_socket_stream, Null_peer, Null_peer, Null_peer>` containing
 * a single sync_io::Native_socket_stream core (acting as a blob-sender and -receiver), then
 * `x.async_io_obj()` shall return a new channel object with the same structure but with
 * a new *async*-I/O-pattern (i.e., auto-parallelizing, async-acting) transport::Native_socket_stream
 * which was constructed using the `sync_io`-core-adopting ctor form (per transport::Blob_sender and
 * transport::Blob_receiver concept).
 *
 * Generally speaking, APIs such as ipc::session (e.g., session::Session_server::async_accept())
 * and ipc::transport (e.g., transport::Native_socket_stream::async_accept()) will create
 * and subsume objects in their `sync_io`-pattern form, sometimes called *cores*.  These are lighter-weight
 * (compared to async-I/O ones) and don't auto-start background threads for example.  Yet whenever you want
 * the async-I/O goodies (and you *do* want them) simply `.async_channel()` to get yourself that guy from
 * the `sync_io` *core*.
 *
 * @note Notably struc::Channel (and struc::sync_io::Channel, should you wish to use one directly)
 *       subsumes a `Channel::S_IS_SYNC_IO_OBJ == true` unstructured Channel in every constructor form offered.
 *
 * ### Secondary use: itself a peer object: forwarding to stored peers ###
 * As a nicety, a Channel itself implements the concepts implemented by each of its stored objects, forwarding API
 * calls to it.  For example, if it bundles a Blob_sender and a Native_handle_sender, then it will itself have
 * send_blob() and send_native_handle(), forwarding to the stored Blob_sender and Native_handle_sender respectively.
 *   - For the main transmission methods, which have almost identical signatures in the `transport::` and
 *     `transport::sync_io::` sender/receiver concepts, each method simply forwards to the appropriate stored
 *     peer object.  Therefore they can be seen as simple syntactic sugar -- it is really just forwarding.
 *     - Blob_sender: send_blob(), send_blob_max_size().
 *     - Native_handle_sender: send_native_handle(), send_meta_blob_max_size().
 *     - Blob_receiver: async_receive_blob(), receive_blob_max_size().
 *     - Native_handle_receiver: async_receive_native_handle(), receive_meta_blob_max_size().
 *   - For the secondary transmission methods, which *also* coincide among certain concepts within
 *     each of those 2 (sync-versus-async) groups, the methods have a combined action for your convenience.
 *     Therefore they actually do something you might find algorithmically helpful rather than mere forwarding.
 *     Here I would single out async_end_sending() specifically.
 *     - Blob_sender, Native_handle_sender:
 *       - auto_ping(): Enables auto-pinging with the same frequency on all (1-2) stored out-pipes.
 *       - end_sending(), async_end_sending(): Performs that operation on all (1-2) stored out-pipes.
 *         `async_end_sending(F)` invokes completion handler `F()` only once *all* (1-2) out-pipes have finished
 *         (this may save you code having to worry about it).
 *         - There is an overload for each of `S_IS_SYNC_IO_OBJ` and `S_IS_ASYNC_IO_OBJ` `Channel` forms
 *     - Blob_receiver, Native_handle_receiver:
 *       - idle_timer_run(): Enables idle-timer with the same idle-timeout on all (1-2) stored in-pipes.
 *
 * In addition the following `transport::sync_io::` methods are available as syntactic sugar if and only if
 * `S_IS_SYNC_IO_OBJ`.
 *   - These method names do not coincide among concepts and therefore are simple forwards:
 *     - Blob_sender:
 *       start_send_blob_ops().
 *     - Native_handle_sender:
 *       start_send_native_handle_ops().
 *     - Blob_receiver:
 *       start_receive_blob_ops().
 *     - Native_handle_receiver:
 *       start_receive_native_handle_ops().
 *   - This method coincides among all 4 potential concepts:
 *     replace_event_wait_handles().
 *     - It invokes the stored peer objects' method of the same name.
 *
 * ### How to use: type, initialization ###
 * A Channel stores a `Blob_sender` and `Blob_receiver`; or a `Native_handle_sender` and `Native_handle_receiver`;
 * or both.
 *
 * The latter is termed the *handles pipe*; the former the *blobs pipe*.  A Channel can be 1 of 2 Channel peers
 * to a pipe bundle containing either the handles pipe, the blobs pipe, or both.  This setting is specified
 * during *initialization* and cannot be changed after that for a given `*this`.  Initialization consists of
 * the following simple steps:
 *   - Select the concrete types for `Blob_*er` and `Native_handle_*er` at construction time when specifying
 *     the concrete Channel type.
 *     - Determine whether the types shall be async-I/O-pattern-implementing (from ipc::transport directly) or
 *       sync-I/O-pattern implementing (from transport::sync_io).  In the latter case we assume you will mentally add
 *       the `sync_io::` qualifier below where applicable.
 *       - This shall determine whether Channel::S_IS_ASYNC_IO_OBJ or Channel::S_IS_SYNC_IO_OBJ.
 *     - For each pipe you *do not* want to use, specify Null_peer as the type.
 *     - For each pipe you *do* want to use, specify a class implementing that concept.
 *       The other (opposing) peer must specify a matching Channel type.
 *       - As of this writing: Blob_sender and Blob_receiver can be `Native_socket_stream`;
 *         Blob_sender can also be `Blob_stream_mq_sender<Mq>` (or the specific aliases
 *         `Posix_mq_sender`, `Bipc_mq_sender`); `Blob_receiver` can be `Blob_stream_mq_receiver<Mq>`
 *         (aliases `Posix_mq_receiver`, `Bipc_mq_receiver`).
 *       - As of this writing: Native_handle_sender and Native_handle_receiver can be Native_socket_stream.
 *     - Example: `Channel<Posix_mq_sender, Posix_mq_receiver, Null_peer, Null_peer>`:
 *                Blob pipe via 2 unidirectional POSIX MQs; handles pipe disabled.
 *     - Example: `Channel<Null_peer, Null_peer, Native_socket_stream, Native_socket_stream>`:
 *                Blob pipe disabled; handles pipe over a Unix domain stream socket connection.
 *     - Example: `Channel<Null_peer, Null_peer, sync_io::Native_socket_stream, sync_io::Native_socket_stream>`:
 *                Same but bundling `sync_io` cores instead of async-I/O counterparts.
 *   - After construction, before any transmission, load the actual concrete peer objects (via move semantics)
 *     by calling:
 *     - Exactly one init_blob_pipe() overload, if the blob pipe is enabled.  Exactly 0 such calls if disabled.
 *       - The 1-arg overload shall be used when the Blob_sender and Blob_receiver are one object.
 *         For example Native_socket_stream is such a class/object, because internally the full-duplex pipe
 *         involves a single low-level transport.
 *       - The 2-arg overload shall be used when they are separate objects.
 *         For example #Bipc_mq_sender and #Bipc_mq_receiver are separate, because internally the full-duplex pipe
 *         is composed of 2 opposite-facing unidirectional MQ-based pipes.
 *     - Exactly one init_native_handle_pipe() overload, if the handles pipe is enabled.  Exactly 0 such calls if
 *       disabled.  (Analogous.)
 *   - (Optional but recommended) Check initialized() and ensure it returns `true`.  Otherwise you've failed to
 *     follow the above instructions.  This may be reason enough to `assert(false)` or abort, but it's up to you.
 *     In any case it is unwise, at best, to proceed with anything below.
 *
 * After this, initialization is finished.  The Channel is in PEER state.  One can use these approaches:
 *   - Access the peer objects via blob_snd(), blob_rcv(), hndl_snd(), hndl_rcv() (of which only the ones
 *     enabled at compile-time -- not Null_peer in that template-parm slot -- shall compile).  Use their APIs as
 *     desired.
 *   - Use the concept-implementing forwarding API ("Secondary use" above) on `*this` itself.
 *
 * Informally: one approach to retain sanity might be -- for a given `*this` -- to use one or the other approach
 * (for a given object), not both.
 *
 * ### Aliases to Channel ###
 * To make it much easier to intantiate and initialize a concrete Channel, there are several aliases and glorified
 * aliases (data-free sub-classes).  See transport_fwd.hpp for the aliases and the present channel.hpp for the
 * glorified aliases.  For example:
 *   - #Posix_mqs_socket_stream_channel (or sync_io::Posix_mqs_socket_stream_channel) is a "full bundle":
 *     a POSIX MQ-based full-duplex blobs pipe; a Unix domain socket-based full-duplex handles pipe.
 *   - Socket_stream_channel is a "half-bundle": a Unix domain socket-based handles pipe only
 *     (template param `SIO` determines whether Native_socket_stream or sync_io::Native_socket_stream).
 *   - #Bipc_mqs_channel_of_blobs (or sync_io::Bipc_mqs_channel_of_blobs) is a "half-bundle":
 *     a bipc MQ-based full-duplex blobs pipe only.
 *
 * Generally such convenience types do not require any init_blob_pipe() or init_native_handle_pipe() calls;
 * one supplies what's necessary at construction time, and the ctor makes those needed calls for the user.
 *
 * This really applies only when creating `Channel`s *not* via ipc::session -- in its case the exact `Channel`
 * type is determined internally to ipc::session: you can not worry about typing or construction and just use
 * the guy.  However someone somewhere does need to actually type and create a `Channel`.
 *
 * Please realize that it is safe and appropriate to `static_cast<>` between pointers/references of any Channel type
 * to any other Channel type: all that has to be true between a given pair of types `C1` and `C2` is:
 * `is_same_v<C1::Blob_sender_obj, C2::Blob_sender_obj> == true`, repeated for `Blob_receiver_obj`,
 * `Native_handle_sender_obj`, `Native_handle_receiver_obj`.
 *
 * ### ??? versus PEER states; pipe interaction ###
 * A Channel contains minimal logic.  It's a bundling of pipe peers; and secondarily of concept implementations.
 *
 * In that secondary role, befitting a Blob_sender, Blob_receiver, and/or Native_handle_sender, Native_handle_receiver:
 * A Channel `*this` is in one of 2 states:
 *   - During initialization: ??? state.
 *   - After initialization: PEER state.  The peer objects have been moved-into `*this`: it can now transmit.
 *
 * As per those concepts: The only way to exit PEER state is to move-from `*this` which makes it ??? (as-if
 * default-cted) again.
 *
 * As of this writing, in ??? state methods other than `init*()` and the basic accessors shall have undefined
 * behavior.  In PEER state however they shall all strive to do work (per concepts).  There's technically also
 * the no-man's-land wherein one has called some but not all intended `init_*_pipe()` calls.  Behavior is similarly
 * undefined in that no-man's-land.
 *
 * To restate, indeed a Channel contains minimal logic and mainly bundles 1-2 pipes, supplying at least
 * 1 bidirectional pipe and optionally a way to transmit native handles along one if desired.  Mostly its API
 * keeps the pipes (if there are indeed 2) separate: send_blob() affects one, send_native_socket() affects the
 * other; the calls do not interact; similarly for receiving.  What minimal interaction does occur does so
 * in those APIs that have identical signatures in each concept pair Native_handle_sender/Blob_sender,
 * Native_handle_receiveir/Blob_receiver.  These interactions are documented in the respective methods' doc
 * headers.  For example `async_end_sending(F)` shall invoke `F()`, once each pipe's individual `*end_sending()`
 * has completed; and errors (if any) will be reported in a particular way.  Additionally note the following:
 *
 * ### Note on error handling/pipe hosing ###
 * Again `*this` is just bundling some senders/receivers.  To begin with, a sender and receiver for one
 * of the pipes (if there are two; or simply *the* pipe otherwise) might be the same object, such as
 * with Native_socket_stream; or separate ones such as Blob_stream_mq_sender and Blob_stream_mq_receiver.
 * Furthermore there might be 2 pipes, one for blobs, the other for sockets.  Now: Consider "pipe hosing."
 * A pipe is hosed (permanently) when an error is emitted to the async transmission methods
 * async_receive_blob(), async_receive_native_handle(), or `*end_sending()`; or (in most cases) the synchronous
 * ones send_blob(), send_native_handle().  The question is... *which* pipe is hosed?  The answer: it depends
 * on what is being bundled here and the type of error.  If Native_socket_stream::send_blob() fails
 * (because #Blob_sender_obj is Native_socket_stream), with system error connection-reset, then both the
 * in-pipe (within the blobs pipe) and out-pipe (ditto) are hosed, because when a Unix domain socket goes down
 * with connection-reset, that means both directions are hosed.  If async_receive_blob() fails with graceful-close
 * error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE, then only the in-pipe is hosed, while the out-pipe is fine.
 * On the other hand with Blob_stream_mq_sender::send_blob() (because #Blob_sender_obj is Blob_stream_mq_sender),
 * no matter how it fails, it does not affect the out-pipe which is presumably run through an entirely
 * separate Blob_stream_mq_receiver, operating necessarily over a different underlying MQ.  And in either case,
 * if there is also a 2nd bidirectional pipe also bundled, then errors over the first pipe mean nothing to
 * the second.
 *
 * So, it depends, and in non-trivial ways very much dependent on the concrete types given as template params.
 *
 * Therefore we make an informal recommendation to the user of any Channel but especially so
 * if programming generically (meaning without awareness of what type that particular Channel is):
 *
 * If a pipe-hosing error is detected through *any* Channel API, prefer to subsequently treat the other pipes
 * as hosed (even if it/they may still be capable of operating).  Stop using the Channel; consider
 * invoking `async_end_sending(F)` (or `sync_io`-pattern form `async_end_sending(E, F)`); and once `F()` is
 * executed (or `sync_io` equivalent; or `*end_sending()` returns `false`) then perhaps
 * destroy `*this`.  Do not try to keep using any remaining 1- or 2-directional pipe(s).
 *
 * Your code will be simpler -- probably without losing much functionality.
 *
 * A potential exception to this rule of thumb *could* be error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE, as it
 * indicates the graceful closing of an in-pipe, while the out-pipe could be fine... but not really;
 * because what about the other bidirectional pipe (if applicable)?  That one is fine all-around, both directions;
 * so now what?
 *
 * Therefore in our opinion it is really best, typically, to look for any 1-direction-pipe-hosing error, and once
 * detected:
 *   -# End regular use of `*this` Channel.
 *   -# (Optional depending on sensitivity of your task/data) Invoke `async_end_sending()`.  If it returns `false`,
 *      or once the completion handler executes (or the `sync_io` form synchronously succeeds):
 *   -# Destroy `*this` Channel (to avoid unnecessary resource use including background threads in the
 *      async-I/O-pattern case).
 *
 * ### Thread safety ###
 * This flows out of understanding the above explanation wherein it is shown that: Strictly after
 * initialization and excluding the acting-on-the-bundle methods end_sending(), async_end_sending(), auto_ping(),
 * idle_timer_run(), the rest of the methods -- most notably `async_receive_*()` and
 * `send_*()` -- operate on each pipe (if there are 2, not 1) independently.  Therefore async_receive_blob()
 * work occurs on entirely separate data from async_receive_native_handle() work; and similarly
 * send_blob() versus send_native_handle().  Therefore running such operations concurrently is safe.
 *
 * Certainly, post-initialization, accessors `{blob|hndl}_{snd|rcv}()` are safe to invoke concurrently.
 *
 * @tparam Blob_sender
 *         Implements that concept in `transport::` or `transport::sync_io::` if blobs pipe enabled; else Null_peer.
 *         If not Null_peer, and another 1 of remaining 3 parameters is not Null_peer, then they must both be
 *         in `transport::` or both in `transport::sync_io::`.
 * @tparam Blob_receiver
 *         Implements that concept in `transport::` or `transport::sync_io::` if blobs pipe enabled; else Null_peer.
 *         If not Null_peer, and another 1 of remaining 3 parameters is not Null_peer, then they must both be
 *         in `transport::` or both in `transport::sync_io::`.
 * @tparam Native_handle_sender
 *         Implements that concept in `transport::` or `transport::sync_io::` if handles pipe enabled; else Null_peer.
 *         If not Null_peer, and another 1 of remaining 3 parameters is not Null_peer, then they must both be
 *         in `transport::` or both in `transport::sync_io::`.
 * @tparam Native_handle_receiver
 *         Implements that concept in `transport::` or `transport::sync_io::` if handles pipe enabled; else Null_peer.
 *         If not Null_peer, and another 1 of remaining 3 parameters is not Null_peer, then they must both be
 *         in `transport::` or both in `transport::sync_io::`.
 *
 * @see Blob_sender: possible implemented concept.
 * @see sync_io::Blob_sender: alternative possible implemented concept (not both).
 * @see Blob_receiver: possible implemented concept.
 * @see sync_io::Blob_receiver: alternative possible implemented concept (not both).
 * @see Native_handle_sender: possible implemented concept.
 * @see sync_io::Native_handle_sender: alternative possible implemented concept (not both).
 * @see Native_handle_receiver: possible implemented concept.
 * @see sync_io::Native_handle_receiver: alternative possible implemented concept (not both).
 */
template<typename Blob_sender, typename Blob_receiver,
         typename Native_handle_sender, typename Native_handle_receiver>
class Channel :
  public flow::log::Log_context
{
public:
  // Types.

  /// Alias for `Blob_sender` template parameter.
  using Blob_sender_obj = Blob_sender;
  /// Alias for `Blob_receiver` template parameter.
  using Blob_receiver_obj = Blob_receiver;
  /// Alias for `Native_handle_sender` template parameter.
  using Native_handle_sender_obj = Native_handle_sender;
  /// Alias for `Native_handle_receiver` template parameter.
  using Native_handle_receiver_obj = Native_handle_receiver;

  static_assert((std::is_same_v<Blob_sender_obj, Null_peer> && std::is_same_v<Blob_sender_obj, Blob_receiver_obj>)
                ||
                ((!(std::is_same_v<Blob_sender_obj, Null_peer>)) && (!(std::is_same_v<Blob_receiver_obj, Null_peer>))),
                "Either both Blob_{send|receiv}er must be Null_peer (no blobs pipe); "
                  "or neither must be Null_peer (blob pipe enabled).");
  static_assert((std::is_same_v<Native_handle_sender_obj, Null_peer>
                   && std::is_same_v<Native_handle_sender_obj, Native_handle_receiver_obj>)
                ||
                ((!(std::is_same_v<Native_handle_sender_obj, Null_peer>))
                   &&
                     (!(std::is_same_v<Native_handle_receiver_obj, Null_peer>))),
                "Either both Native_handle_{send|receiv}er must be Null_peer (no handles pipe); "
                  "or neither must be Null_peer (handles pipe enabled).");
  static_assert((!(std::is_same_v<Blob_sender_obj, Null_peer>))
                  || (!(std::is_same_v<Native_handle_sender_obj, Null_peer>)),
                "Blob_{send|receiv}er and Native_handle_{send|receiv}er cannot both be Null_peer x 2; "
                  "at least one pipe must be enabled.");

  /**
   * Assuming #S_IS_SYNC_IO_OBJ yields async-I/O counterpart; else yields
   * `Channel<Null_peer, Null_peer, Null_peer, Null_peer>`.
   */
  using Async_io_obj = Channel<typename Blob_sender_obj::Async_io_obj,
                               typename Blob_receiver_obj::Async_io_obj,
                               typename Native_handle_sender_obj::Async_io_obj,
                               typename Native_handle_receiver_obj::Async_io_obj>;

  /**
   * Assuming #S_IS_ASYNC_IO_OBJ yields `sync_io` counterpart; else yields
   * `Channel<Null_peer, Null_peer, Null_peer, Null_peer>`.
   */
  using Sync_io_obj = Channel<typename Blob_sender_obj::Sync_io_obj,
                              typename Blob_receiver_obj::Sync_io_obj,
                              typename Native_handle_sender_obj::Sync_io_obj,
                              typename Native_handle_receiver_obj::Sync_io_obj>;

  // Constants.

  /// Useful for generic programming: `true` if and only if types imply both blobs and handles pipes are enabled.
  static constexpr bool S_HAS_2_PIPES = (!(std::is_same_v<Blob_sender_obj, Null_peer>))
                                        &&
                                        (!(std::is_same_v<Native_handle_sender_obj, Null_peer>));
  /// Useful for generic programming: `true` if and only if types imply only the blobs pipe is enabled.
  static constexpr bool S_HAS_BLOB_PIPE_ONLY = (!S_HAS_2_PIPES)
                                               &&
                                               (!(std::is_same_v<Blob_sender_obj, Null_peer>));
  /// Useful for generic programming: `true` if and only if types imply only the handles pipe is enabled.
  static constexpr bool S_HAS_NATIVE_HANDLE_PIPE_ONLY = (!S_HAS_2_PIPES) && (!S_HAS_BLOB_PIPE_ONLY);
  /// Useful for generic programming: `true` if and only if types imply *at least* the blobs pipe is enabled.
  static constexpr bool S_HAS_BLOB_PIPE = S_HAS_2_PIPES || S_HAS_BLOB_PIPE_ONLY;
  /// Useful for generic programming: `true` if and only if types imply *at least* the handles pipe is enabled.
  static constexpr bool S_HAS_NATIVE_HANDLE_PIPE = S_HAS_2_PIPES || S_HAS_NATIVE_HANDLE_PIPE_ONLY;

  /**
   * Useful for generic programming: `true` <=> each non-`Null_peer` peer type
   * (#Blob_sender_obj, #Blob_receiver_obj, #Native_handle_sender_obj, #Native_handle_receiver_obj)
   * implements the `sync_io` pattern (by convention living in namespace ipc::transport::sync_io).
   */
  static constexpr bool S_IS_SYNC_IO_OBJ
    = (S_HAS_BLOB_PIPE && std::is_same_v<typename Blob_sender_obj::Sync_io_obj, Null_peer>)
      ||
      (S_HAS_NATIVE_HANDLE_PIPE && std::is_same_v<typename Native_handle_sender_obj::Sync_io_obj, Null_peer>);
  /**
   * It equals the reverse of #S_IS_SYNC_IO_OBJ.   That is all peer types are of the async-I/O pattern variety
   * (by convention living directly in namespace ipc::transport). */
  static constexpr bool S_IS_ASYNC_IO_OBJ
    = !S_IS_SYNC_IO_OBJ;

  // Constructors/destructor.

  /**
   * Default ctor (Channel is in ??? state; intended to be move-assigned).
   *
   * This ctor is informally intended for the following uses:
   *   - A moved-from Channel (i.e., the `src` arg move-ctor and move-assignment operator)
   *     becomes as-if defaulted-constructed.
   *   - A target Channel for an API that generates PEER-state Channel objects shall typically be
   *     default-cted by user before being passed to that API.
   *
   * Therefore it would be unusual (though allowed) to make direct calls such as init_blob_pipe()
   * on a default-cted Channel without first moving a non-default-cted object into it.
   */
  Channel();

  /**
   * Constructs Channel in ??? state with the intention to continue initialization via init_blob_pipe() and/or
   * init_native_handle_pipe() call(s).
   *
   * This ctor is informally intended for the following use:
   *   - You create a Channel that is logger-appointed and nicely-nicknamed; then you call
   *     init_blob_pipe() and/or init_native_handle_pipe() to move actual transmitting peer objects into `*this`,
   *     moving `*this` into PEER state.
   *     It will retain the logger and nickname throughout.
   *
   * It may be more convenient to use an alias or data-less sub-class which typically comes with specialized
   * ctor forms that remove or reduce the need for laborious `init_*_pipe()` calls.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname_str
   *        Human-readable nickname of the new object, as of this writing for use in `operator<<(ostream)` and
   *        logging only.
   */
  Channel(flow::log::Logger* logger_ptr, util::String_view nickname_str);

  /**
   * Move-constructs from `src`; `src` becomes as-if default-cted (therefore in ??? state).
   *
   * @param src
   *        See above.
   */
  Channel(Channel&& src);

  /// Copy construction is disallowed.
  Channel(const Channel&) = delete;

  /**
   * Destructor: synchronously invokes the destructors for each peer object moved-into `*this` via
   * init_blob_pipe() and/or init_native_handle_pipe().
   *
   * All the notes for the N concepts' destructors apply.  In PEER state, for async-I/O peer objects,
   * any pending one-off completion handlers will be invoked with
   * error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER -- and so on.
   */
  ~Channel();

  // Methods.

  /**
   * Returns `true` if and only if the required init_blob_pipe() and init_native_handle_pipe() calls have been
   * made, loading exactly the expected peer objects as the template params #Blob_sender_obj, #Blob_receiver_obj,
   * #Native_handle_sender_obj, #Native_handle_receiver_obj specify.  This is useful particularly when meta-programming
   * on top of Channel: one can call initialized() before any transmission methods to ensure the proper
   * `init_*()` calls were made.  After that it is safe to meta-program in terms of just the compile-time values
   * #S_HAS_2_PIPES, #S_HAS_BLOB_PIPE, #S_HAS_NATIVE_HANDLE_PIPE, #S_HAS_BLOB_PIPE_ONLY, #S_HAS_NATIVE_HANDLE_PIPE_ONLY,
   * #S_IS_SYNC_IO_OBJ, #S_IS_ASYNC_IO_OBJ.
   *
   * Namely it will check that:
   *   - init_blob_pipe() was called if and only if #Blob_sender_obj is not Null_peer.
   *     - If an overload was indeed called, and its 1-arg was the one, then #Blob_sender_obj and #Blob_receiver_obj
   *       were the same type.
   *   - init_native_handle_pipe() was called if and only if #Native_handle_sender_obj is not Null_peer.
   *     - If an overload was indeed called, and its 1-arg was the one, then #Native_handle_sender_obj and
   *       #Native_handle_receiver_obj were the same type.
   *
   * If this returns `false`, formally, behavior is undefined, if one attempts transmission.
   * Informally, likely some intended-for-use transmission methods will always return `false` and no-op;
   * but the best recommendation is: If this returns `false`, do not use `*this` for transmission.
   *
   * @param suppress_log
   *        If and only if `true`, returning `false` shall not be explained via logging.
   *        This may be useful when one merely wants to check whether `*this` is in default-cted state
   *        without implying that's a terrible thing.
   * @return See above.
   */
  bool initialized(bool suppress_log = false) const;

  /**
   * Move-assigns from `src`; `*this` acts as if destructed; `src` becomes as-if default-cted (therefore in ??? state).
   * No-op if `&src == this`.
   *
   * @see ~Channel().
   *
   * @param src
   *        See above.
   * @return `*this`.
   */
  Channel& operator=(Channel&& src);

  /// Copy assignment is disallowed.
  Channel& operator=(const Channel&) = delete;

  // Methods.

  /**
   * Converts a `sync_io`-peer-bearing `*this` to a returned async-I/O-peer-bearing new Channel, while
   * `*this` becomes as-if default-cted.
   *
   * Compilable only if #S_IS_SYNC_IO_OBJ, and usable only after initialized(),
   * returns `Channel` with #S_IS_ASYNC_IO_OBJ, each peer object of which is created via
   * its respective `sync_io`-core-adopting ctors.  (For example:
   * `transport::Blob_sender(transport::sync_io::Blob_sender&&)`.)
   *
   * If not initialized() behavior is undefined (assertion may trip).
   *
   * Useful, in particular, to make an async-I/O Channel from a Channel obtained from a session::Session.
   *
   * @return See above.
   */
  Async_io_obj async_io_obj();

  /**
   * Pointer to the immutable owned #Blob_sender_obj; null if yet initialized.
   *
   * Compilable only if #Blob_sender_obj is not Null_peer (#S_HAS_BLOB_PIPE).
   *
   * @return See above.
   */
  const Blob_sender_obj* blob_snd() const;

  /**
   * Pointer to the immutable owned #Blob_receiver_obj; null if not yet initialized.
   *
   * Compilable only if #Blob_receiver_obj is not Null_peer (#S_HAS_BLOB_PIPE).
   *
   * @return See above.
   */
  const Blob_receiver_obj* blob_rcv() const;

  /**
   * Pointer to the immutable owned #Native_handle_sender_obj; null if not yet initialized.
   *
   * Compilable only if #Native_handle_sender_obj is not Null_peer (#S_HAS_NATIVE_HANDLE_PIPE).
   *
   * @return See above.
   */
  const Native_handle_sender_obj* hndl_snd() const;

  /**
   * Pointer to the immutable owned #Native_handle_receiver_obj; null if not yet initialized.
   *
   * Compilable only if #Native_handle_receiver_obj is not Null_peer (#S_HAS_NATIVE_HANDLE_PIPE).
   *
   * @return See above.
   */
  const Native_handle_receiver_obj* hndl_rcv() const;

  /**
   * Pointer to the mutable owned #Blob_sender_obj; null if yet initialized.
   *
   * Compilable only if #Blob_sender_obj is not Null_peer (#S_HAS_BLOB_PIPE).
   *
   * @return See above.
   */
  Blob_sender_obj* blob_snd();

  /**
   * Pointer to the mutable owned #Blob_receiver_obj; null if not yet initialized.
   *
   * Compilable only if #Blob_receiver_obj is not Null_peer (#S_HAS_BLOB_PIPE).
   *
   * @return See above.
   */
  Blob_receiver_obj* blob_rcv();

  /**
   * Pointer to the mutable owned #Native_handle_sender_obj; null if not yet initialized.
   *
   * Compilable only if #Native_handle_sender_obj is not Null_peer (#S_HAS_NATIVE_HANDLE_PIPE).
   *
   * @return See above.
   */
  Native_handle_sender_obj* hndl_snd();

  /**
   * Pointer to the mutable owned #Native_handle_receiver_obj; null if not yet initialized.
   *
   * Compilable only if #Native_handle_receiver_obj is not Null_peer (#S_HAS_NATIVE_HANDLE_PIPE).
   *
   * @return See above.
   */
  Native_handle_receiver_obj* hndl_rcv();

  /**
   * Completes initialization of the *blobs pipe* by taking ownership (via move semantics) of an object that
   * is simultaneously the #Blob_sender_obj and #Blob_receiver_obj for our end of the blobs pipe.  Call this 0 times
   * (successfully) if blobs pipe disabled (in which case #Blob_sender_obj and #Blob_receiver_obj should both be
   * Null_peer).  Call either this or the 2-arg overload exactly 1 time (successfully) otherwise.  If you call this,
   # #Blob_sender_obj and #Blob_receiver_obj must be the same type.
   *
   * Certain mistaken uses are caught in this method; it no-ops and returns `false` (failure):
   *   - You called this, but #Blob_sender_obj is Null_peer.
   *   - You called this, but #Blob_sender_obj and #Blob_receiver_obj are not the same type.
   *   - You called this after already calling an init_blob_pipe() overload successfully before.
   *
   * The remaining mistakes are caught by initialized(), if you choose to call it before any transmission (and you
   * should):
   *   - You called this or the overload 0 times (successfully), but #Blob_sender_obj is not Null_peer.
   *
   * Pre-condition: `snd_and_rcv` is in PEER state.  Behavior is undefined otherwise.
   * Post-condition (on success): The `*this` blobs pipe is in PEER state (transmission can begin).
   *
   * Informal recommendation: Complete the *handles pipe* initialization, if enabled, before transmitting anything
   * over the blobs pipe, even though technically it is possible to do so immediately.
   * Use initialized() to double-check.
   *
   * @warning It is an error (as noted above) to call this, unless #Blob_sender_obj and #Blob_receiver_obj are the
   *          same type.  This will no-op and return `false`.  The only reason it is not undefined behavior
   *          (assertion trip) is so that this mistake can be caught without an `assert()`, if you choose
   *          to call initialized() after the init phase yourself.
   *
   * @note As a user, it is likely you can/should use an alias type that will take care of calling this for you.
   *       See Channel doc header.
   *
   * @param snd_and_rcv
   *        A #Blob_sender_obj *and* #Blob_receiver_obj in PEER state.
   * @return `true` on success; `false` on no-op due to a mistaken use listed above.
   */
  bool init_blob_pipe(Blob_sender_obj&& snd_and_rcv);

  /**
   * Completes initialization of the *blobs pipe* by taking ownership (via move semantics) of separate
   * #Blob_sender_obj and #Blob_receiver_obj objects for our end of the blobs pipe.  Call this 0 times if
   * blobs pipe disabled (in which case #Blob_sender_obj and #Blob_receiver_obj should both be Null_peer).
   * Call either this or the 1-arg overload exactly 1 time otherwise.
   *
   * Certain mistaken uses are caught in this method; it no-ops and returns `false` (failure):
   *   - You called this, but #Blob_sender_obj is Null_peer and/or #Blob_receiver_obj is Null_peer.
   *   - You called this after already calling an init_blob_pipe() overload successfully before.
   *
   * The remaining mistakes are caught by initialized(), if you choose to call it before any transmission (and you
   * should):
   *   - You called this or the overload 0 times (successfully), but #Blob_sender_obj is not Null_peer.
   *
   * Pre-condition: `snd` and `rcv` are in PEER state.  Behavior is undefined otherwise.
   * Post-condition: The `*this` blobs pipe is in PEER state (transmission can begin).
   *
   * Informal recommendation: Complete the *handles pipe* initialization, if enabled, before transmitting anything
   * over the blobs pipe, even though technically it is possible to do so immediately.
   * Use initialized() to double-check.
   *
   * @note As a user, it is likely you can/should use an alias type that will take care of calling this for you.
   *       See Channel doc header.
   *
   * @param snd
   *        A #Blob_sender_obj in PEER state.
   * @param rcv
   *        A #Blob_receiver_obj in PEER state.
   * @return `true` on success; `false` on no-op due to a mistaken use listed above.
   */
  bool init_blob_pipe(Blob_sender_obj&& snd, Blob_receiver_obj&& rcv);

  /**
   * Analogous to 1-arg init_blob_pipe() but as applied to the *handles pipe*.  All its notes apply by analogy.
   *
   * @param snd_and_rcv
   *        A #Native_handle_sender_obj *and* #Native_handle_receiver_obj in PEER state.
   * @return See init_blob_pipe().
   */
  bool init_native_handle_pipe(Native_handle_sender_obj&& snd_and_rcv);

  /**
   * Analogous to 2-arg init_blob_pipe() but as applied to the *handles pipe*.  All its notes apply by analogy.
   *
   * @param snd
   *        A #Native_handle_sender_obj in PEER state.
   * @param rcv
   *        A #Native_handle_receiver_obj in PEER state.
   * @return See init_blob_pipe().
   */
  bool init_native_handle_pipe(Native_handle_sender_obj&& snd, Native_handle_receiver_obj&& rcv);

  /**
   * Yields `blob_snd()->` same method.
   *
   * @return See concept API.
   */
  size_t send_blob_max_size() const;

  /**
   * Yields `hndl_snd()->` same method.
   *
   * @return See concept API.
   */
  size_t send_meta_blob_max_size() const;

  /**
   * Yields `blob_rcv()->` same method.
   *
   * @return See concept API.
   */
  size_t receive_blob_max_size() const;

  /**
   * Yields `hndl_rcv()->` same method.
   *
   * @return See concept API.
   */
  size_t receive_meta_blob_max_size() const;

  /**
   * Yields `blob_snd()->` same method.
   *
   * @param blob
   *        See concept API.
   * @param err_code
   *        See concept API.
   * @return See concept API.
   */
  bool send_blob(const util::Blob_const& blob, Error_code* err_code);

  /**
   * Yields `hndl_snd()->` same method.
   *
   * @param hndl_or_null
   *        See concept API.
   * @param meta_blob
   *        See concept API.
   * @param err_code
   *        See concept API.
   * @return See concept API.
   */
  bool send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob, Error_code* err_code);

  /**
   * Performs `hndl_snd()->` and/or `blob_snd()->` same method, synthesizing completion handlers into one
   * if applicable.  Invoke only after initialized() is true; else behavior is undefined.  That aside:
   *   - If not #S_HAS_2_PIPES, then simply forwards to the appropriate one method of the enabled pipe.
   *   - If #S_HAS_2_PIPES:
   *     - `on_done_func()` is invoked (exactly once) if and only if both forwarded methods have completed.
   *       - If 1-2 completions do not occur, it is not invoked.
   *     - If that occurs, it is invoked in-place of the 2nd completion handler (chronologically in order of
   *       completion).
   *     - The `Error_code` passed to `on_done_func()` is:
   *       - falsy (success) if and only both completions were successful;
   *       - if one failed but not the other, the truthy `Error_code` from the failed completion;
   *       - if both failed, the truthy `Error_code` from the first failed completion.
   *
   * Behavior is undefined (assertion may trip) if there are 2 pipes enabled; and one's `.async_end_sending()`
   * returns `false` (dupe call), while the other `true`.  This will not occur, if you cleanly use
   * the Channel API directly on `*this` only, as opposed to shenanigans via non-`const` blob_snd(), blob_rcv(), etc.
   *
   * Compilable only if #S_IS_ASYNC_IO_OBJ.
   *
   * @tparam Task_err
   *         See concept API.
   * @param on_done_func
   *        See above.
   * @return If one forwarded invocation: returns what it returned.
   *         If two forwarded invocations: returns `true` if both returned `true`; `false` if both returned `false`;
   *         behavior undefined if they returned conflicting values.
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * Performs `hndl_snd()->` and/or `blob_snd()->` same method, synthesizing completions into one
   * if applicable.  Invoke only after initialized() is true; else behavior is undefined.  That aside:
   *   - If not #S_HAS_2_PIPES, then simply forwards to the appropriate one method of the enabled pipe.
   *   - If #S_HAS_2_PIPES:
   *     - `on_done_func()` is invoked (exactly once) if and only if both forwarded methods have completed.
   *       - If 1-2 completions do not occur, it is not invoked.
   *     - If that occurs, it is invoked in-place of the 2nd completion handler (chronologically in order of
   *       completion).
   *     - The `Error_code` passed to `on_done_func()` is:
   *       - falsy (success) if and only both completions were successful;
   *       - if one failed but not the other, the truthy `Error_code` from the failed completion;
   *       - if both failed, the truthy `Error_code` from the first failed completion.
   *
   * "Completion" in this case means the first (and only) one to occur for each pipe:
   *   - Its individual `.async_end_sending()` yielded synchronous (immediate) completion as opposed
   *     to error::Code::S_SYNC_IO_WOULD_BLOCK.  Or:
   *   - It returned WOULD_BLOCK, but later (via `sync_io`-pattern machinery) its completion handler executed.
   *
   * Behavior is undefined (assertion may trip) if there are 2 pipes enabled; and one's `.async_end_sending()`
   * returns `false` (dupe call), while the other `true`.  This will not occur, if you cleanly use
   * the Channel API directly on `*this` only, as opposed to shenanigans via non-`const` blob_snd(), blob_rcv(), etc.
   *
   * Compilable only if #S_IS_SYNC_IO_OBJ.
   *
   * @tparam Task_err
   *         See concept API.
   * @param sync_err_code
   *        See concept API.
   *        Do realize error::Code::S_SYNC_IO_WOULD_BLOCK *is* still an error, so if this pointer is null, then
   *        would-block *will* make this throw.
   * @param on_done_func
   *        See above.
   * @return If one forwarded invocation: returns what it returned.
   *         If two forwarded invocations: returns `true` if both returned `true`; `false` if both returned `false`;
   *         behavior undefined if they returned conflicting values.
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

  /**
   * Performs `hndl_snd()->` and/or `blob_snd()->` same method, returning `true` if
   * all (1-2) invoked methods returned `true`; `false` conversely.  As with async_end_sending() (either overload)
   * behavior is undefined (assertion may trip), if one returned `true` and the other `false`.
   *
   * @return See above.
   */
  bool end_sending();

  /**
   * Performs `hndl_snd()->` and/or `blob_snd()->` same method, returning `true` if
   * all (1-2) invoked methods returned `true`; `false` conversely.  As with async_end_sending() (either overload)
   * behavior is undefined (assertion may trip), if one returned `true` and the other `false`.
   *
   * @param period
   *        See concept API.
   * @return See above.
   */
  bool auto_ping(util::Fine_duration period = boost::chrono::seconds(2));

  /**
   * Yields `blob_rcv()->` same method.  Blob_receiver versus sync_io::Blob_receiver signatures differ slightly;
   * therefore this uses param-pack perfect forwarding.
   *
   * @tparam Args
   *         See above.
   * @param args
   *        See concept API.
   * @return See concept API.
   */
  template<typename... Args>
  bool async_receive_blob(Args&&... args);

  /**
   * Yields `hndl_rcv()->` same method.  Native_handle_receiver versus sync_io::Native_handle_receiver signatures
   * differ slightly; therefore this uses param-pack perfect forwarding.
   *
   * @tparam Args
   *         See above.
   * @param args
   *        See concept API.
   * @return See concept API.
   */
  template<typename... Args>
  bool async_receive_native_handle(Args&&... args);

  /**
   * Performs `hndl_rcv()->` and/or `blob_rcv()->` same method, returning `true` if
   * all (1-2) invoked methods returned `true`; `false` conversely.  As with async_end_sending() (either overload)
   * behavior is undefined (assertion may trip), if one returned `true` and the other `false`.
   *
   * @param timeout
   *        See above.
   * @return See above.
   */
  bool idle_timer_run(util::Fine_duration timeout = boost::chrono::seconds(5));

  /**
   * Executes same method on all unique stored peer objects; returns `true` if and only if they all did.
   * (initialized() must be `true`; else behavior undefined.)
   *
   * Compilable only if #S_IS_SYNC_IO_OBJ.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See concept API.
   * @param create_ev_wait_hndl_func
   *        See concept API.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Yields `blob_snd()->` same method.
   *
   * @tparam Event_wait_func_t
   *         See concept API.
   * @param ev_wait_func
   *        See concept API.
   * @return See concept API.
   */
  template<typename Event_wait_func_t>
  bool start_send_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Yields `hndl_snd()->` same method.
   *
   * @tparam Event_wait_func_t
   *         See concept API.
   * @param ev_wait_func
   *        See concept API.
   * @return See concept API.
   */
  template<typename Event_wait_func_t>
  bool start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Yields `blob_rcv()->` same method.
   *
   * @tparam Event_wait_func_t
   *         See concept API.
   * @param ev_wait_func
   *        See concept API.
   * @return See concept API.
   */
  template<typename Event_wait_func_t>
  bool start_receive_blob_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Yields `hndl_rcv()->` same method.
   *
   * @tparam Event_wait_func_t
   *         See concept API.
   * @param ev_wait_func
   *        See concept API.
   * @return See concept API.
   */
  template<typename Event_wait_func_t>
  bool start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Returns nickname, a brief string suitable for logging.  This is included in the output by the `ostream<<`
   * operator as well.  This method is thread-safe in that it always returns the same value.
   *
   * If this object is default-cted (or moved-from), this will return a value equal to "".
   *
   * @return See above.
   */
  const std::string& nickname() const;

private:
  // Types.

  /// Short-hand alias for stored object, lifetime [init_blob_pipe(), move-assignment or dtor].
  using Blob_sender_ptr = std::optional<Blob_sender_obj>;

  /// Short-hand alias for stored object, lifetime [init_blob_pipe(), move-assignment or dtor].
  using Blob_receiver_ptr = std::optional<Blob_receiver_obj>;

  /// Short-hand alias for stored object, lifetime [init_native_handle_pipe(), move-assignment or dtor].
  using Native_handle_sender_ptr = std::optional<Native_handle_sender_obj>;

  /// Short-hand alias for stored object, lifetime [init_native_handle_pipe(), move-assignment or dtor].
  using Native_handle_receiver_ptr = std::optional<Native_handle_receiver_obj>;

  // Friends.

  /// Friend of Channel.
  template<typename Blob_sender2, typename Blob_receiver2,
           typename Native_handle_sender2, typename Native_handle_receiver2>
  friend std::ostream& operator<<(std::ostream& os,
                                  const Channel<Blob_sender2, Blob_receiver2,
                                                Native_handle_sender2, Native_handle_receiver2>& val);

  // Data.

  /// See nickname().
  std::string m_nickname;

  /**
   * The #Blob_sender_obj (sender along the blobs pipe).  Lifetime is between init_blob_pipe() and move/dtor.
   * Null if blobs pipe is disabled or not yet initialized.
   *
   * In addition, if 1-arg init_blob_pipe() was called, this is *also* the #Blob_receiver_obj; blob_rcv()
   * `reinterpret_cast`s `*m_blob_snd` as `Blob_receiver&`.  The type safety is guaranteed by how #m_blob_rcv
   * is initialized in init_blob_pipe().
   */
  Blob_sender_ptr m_blob_snd;

  /**
   * If 2-arg init_blob_pipe() was used, this is the #Blob_sender_obj equivalent of #m_blob_snd; else null.
   * Null if blobs pipe is disabled or not yet initialized.
   *
   * @see #m_blob_snd doc header.
   */
  Blob_receiver_ptr m_blob_rcv;

  /**
   * The #Native_handle_sender_obj (sender along the handles pipe).  Lifetime is between init_native_handle_pipe()
   * and move/dtor.  Null if handles pipe is disabled or not yet initialized.
   *
   * In addition, if 1-arg init_native_handle_pipe() was called, this is *also* the #Native_handle_receiver_obj;
   * hndl_rcv() `reinterpret_cast`s `*m_hndl_snd` as `Native_handle_receiver_obj&`.  The type safety is guaranteed
   * by how #m_hndl_rcv is initialized in init_native_handle_pipe().
   */
  Native_handle_sender_ptr m_hndl_snd;

  /**
   * If 2-arg init_native_handle_pipe() was used, this is the Native_handle_receiver_obj equivalent of #m_hndl_snd;
   * else null.  Null if handles pipe is disabled or not yet initialized.
   *
   * @see #m_hndl_snd doc header.
   */
  Native_handle_receiver_ptr m_hndl_rcv;
}; // class Channel

/**
 * Dummy type for use as a template param to Channel when either the blobs pipe or handles pipe is disabled;
 * as well as to mark a given peer object as not having a counterpart form: a `sync_io` object shall have
 * its `using Sync_io_obj = Null_peer` and coversely for async-I/O guys and their `Async_io_obj`s.
 *
 * No object of this type is ever touched, at least if Channel is properly used.
 */
class Null_peer
{
public:
  /// You may disregard.
  using Sync_io_obj = Null_peer;
  /// You may disregard.
  using Async_io_obj = Null_peer;
};

/**
 * A Channel with a *handles pipe* only (no *blobs pipe*) that uses a Unix domain socket connection as the
 * underlying transport for that pipe.
 *
 * Note this is a glorified alias; it stores no additional data on top of the super-class.  It is easier to use
 * than the super-class, as the ctor takes care of the necessary init_native_handle_pipe() call to complete
 * initialization directly during construction.  You may freely `static_cast` pointers/references between
 * the 2 types.
 *
 *   - send_native_handle(), async_receive_native_handle() will work (over Unix domain socket stream transport).
 *   - send_blob(), async_receive_blob() will not compile.
 *
 * @tparam SIO
 *         Selects between `transport::sync_io::` and `transport::` version of held type(s); hence
 *         between Channel::S_IS_SYNC_IO_OBJ and Channel::S_IS_ASYNC_IO_OBJ.
 */
template<bool SIO>
class Socket_stream_channel :
  public Channel<Null_peer, Null_peer,
                 std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                 std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>>
{
public:
  // Types.

  /// Short-hand for our base class.
  using Base = Channel<Null_peer, Null_peer,
                       std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                       std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>>;

  // Constructors/destructor.

  /**
   * Constructs the Channel in PEER state.  `sock_stm` must be in PEER state; else behavior is undefined.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname_str
   *        See nickname().
   * @param sock_stm
   *        The peer object that becomes owned by `*this` via move.  `sock_stm` becomes as-if default-cted
   *        (NULL state).
   */
  explicit Socket_stream_channel(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                 typename Base::Native_handle_sender_obj&& sock_stm);

  /// Identical to Channel default ctor.
  Socket_stream_channel();

  // Methods.

  /**
   * Identical to Native_socket_stream::remote_peer_process_credentials(): OS-reported process credential
   * (PID, etc.) info about the *other* connected peer's process.  See its doc header.
   *
   * @param err_code
   *        See above.
   * @return See above.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code = 0) const;
}; // class Socket_stream_channel

/**
 * A Channel with a *blobs pipe* only (no *handles pipe*) that uses a Unix domain socket connection as the
 * underlying transport for that pipe.
 *   - send_blob(), async_receive_blob() will work (over Unix domain socket stream transport).
 *   - send_native_handle(), async_receive_native_handle() will not compile.
 *
 * One might use this instead of #Posix_mqs_socket_stream_channel
 * or #Bipc_mqs_channel_of_blobs or Mqs_channel if:
 *   - one feels it is faster or not-slower than those MQs; and/or
 *   - the connection setup/teardown concerns are cleaner or easier than for MQs.
 *     In particular, MQs have kernel persistence, and if a crash prevents a destructor from executing then
 *     additional cleanup is required after restart in order to free those RAM resources.
 *
 * Note this a glorified alias; it stores no additional data on top of the super-class.  It is easier to use
 * than the super-class, as the ctor takes care of the necessary init_blob_pipe() call to complete initialization
 * directly during construction.  You may freely `static_cast` pointers/references between
 * the 2 types.
 *
 * @tparam SIO
 *         Selects between `transport::sync_io::` and `transport::` version of held type(s); hence
 *         between Channel::S_IS_SYNC_IO_OBJ and Channel::S_IS_ASYNC_IO_OBJ.
 */
template<bool SIO>
class Socket_stream_channel_of_blobs :
  public Channel<std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                 std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                 Null_peer, Null_peer>
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Channel<std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                       std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                       Null_peer, Null_peer>;

  // Constructors/destructor.

  /**
   * Constructs the Channel in PEER state.  `sock_stm` must be in PEER state; else behavior is undefined.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname_str
   *        See nickname().
   * @param sock_stm
   *        The peer object that becomes owned by `*this` via move.  `sock_stm` becomes as-if default-cted
   *        (NULL state).
   */
  explicit Socket_stream_channel_of_blobs(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                          typename Base::Blob_sender_obj&& sock_stm);

  /// Identical to Channel default ctor.
  Socket_stream_channel_of_blobs();

  // Methods.

  /**
   * Identical to Native_socket_stream::remote_peer_process_credentials(): OS-reported process credential
   * (PID, etc.) info about the *other* connected peer's process.  See its doc header.
   *
   * @param err_code
   *        See above.
   * @return See above.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code = 0) const;
}; // class Socket_stream_channel_of_blobs

/**
 * A Channel with *at least* a *blobs pipe* consisting of two MQs of type Persistent_mq_handle (template arg 1);
 * and *possibly* a *handles pipe* as governed by template args 2 and 3 which default to Null_peer (in which
 * case the handles pipe is disabled).  To use:
 *   - Construct, passing in the two MQ peer objects for `*this` to own.
 *   - If and only if you require a handles pipe as well, call init_native_handle_pipe() to complete initialization.
 *
 * As a result:
 *   - send_blob(), async_receive_blob() will work (over MQs).
 *   - If one does not call init_native_handle_pipe():
 *     - send_native_handle(), async_receive_native_handle() will not compile.
 *
 * However it is likely more typical to use Mqs_socket_stream_channel (which extends Mqs_channel) or even more
 * likely its two concrete-type aliases #Posix_mqs_socket_stream_channel, #Bipc_mqs_socket_stream_channel
 * (and/or sync_io::Posix_mqs_socket_stream_channel, sync_io::Bipc_mqs_socket_stream_channel).
 * Either way init_native_handle_pipe() is then done for the user.  If no handles pipe is required, then it is
 * likely one would use #Posix_mqs_channel_of_blobs or #Bipc_mqs_channel_of_blobs (aliases to Mqs_channel).
 *
 * @tparam Persistent_mq_handle
 *         Implements that concept.
 * @tparam Native_handle_sender
 *         Implements that concept (in `transport::` or `transport::sync_io::`)
 *         if handles pipe enabled; otherwise Null_peer.
 * @tparam Native_handle_receiver
 *         Analogously.
 *
 * @tparam SIO
 *         Selects between `transport::sync_io::` and `transport::` version of held type(s); hence
 *         between Channel::S_IS_SYNC_IO_OBJ and Channel::S_IS_ASYNC_IO_OBJ.
 */
template<bool SIO, typename Persistent_mq_handle,
#ifdef IPC_DOXYGEN_ONLY // Mirror the transport_fwd.hpp fwd-declaration in the generated docs.
         typename Native_handle_sender = Null_peer, typename Native_handle_receiver = Null_peer>
#else
         typename Native_handle_sender, typename Native_handle_receiver>
#endif
class Mqs_channel :
  public Channel<std::conditional_t<SIO, sync_io::Blob_stream_mq_sender<Persistent_mq_handle>,
                                         Blob_stream_mq_sender<Persistent_mq_handle>>,
                 std::conditional_t<SIO, sync_io::Blob_stream_mq_receiver<Persistent_mq_handle>,
                                         Blob_stream_mq_receiver<Persistent_mq_handle>>,
                 Native_handle_sender, Native_handle_receiver>
{
public:
  // Types.

  /// Short-hand for the Persistent_mq_handle concrete type used for blobs pipe.
  using Mq = Persistent_mq_handle;

  /// Short-hand for base class.
  using Base = Channel<std::conditional_t<SIO, sync_io::Blob_stream_mq_sender<Persistent_mq_handle>,
                                               Blob_stream_mq_sender<Persistent_mq_handle>>,
                       std::conditional_t<SIO, sync_io::Blob_stream_mq_receiver<Persistent_mq_handle>,
                                               Blob_stream_mq_receiver<Persistent_mq_handle>>,
                       Native_handle_sender, Native_handle_receiver>;

  // Constructors/destructor.

  /**
   * Constructs the Channel in PEER state.
   * `mq_out` and `mq_in` must be suitable for Persistent_mq_handle PEER-state ctor (see it doc header); else
   * behavior is undefined.
   *
   * If and only if you intend to equip `*this` with a *handles pipe* as well, you must call init_native_handle_pipe()
   * to complete initialization.
   *
   * ### Error semantics ###
   * While the below formally outlines how it all works, informally using this is pretty simple:
   *   - If you choose the throwing form (null `err_code`),
   *     then by the nature of C++ `*this` cannot exist <=> this throws.
   *   - If you choose the non-throwing form (non-null `err_code`),
   *     then a truthy `*err_code` <=> `*this` is unusable (except to move-to).
   *   - Any error is logged.
   *
   * Formally though:
   *
   * Firstly: See `flow::Error_code` docs for error reporting semantics (including the exception/code dichotomy).
   * The bottom line is: If `err_code` is null, then success <=> this ctor does not throw.
   * If `err_code` is non-null, then success <=> `*err_code` is falsy after return.  If it is truthy,
   * the `*this` shall not be used except to invoke destructor or to be moved-to.  Otherwise behavior is undefined.
   *
   * Secondly: The source of error conditions is explicitly the following:
   *   - `Blob_stream_mq_sender<Mq>(logger_ptr, nickname_str, std::move(mq_out), err_code)` may emit an error.
   *   - `Blob_stream_mq_receiver<Mq>(logger_ptr, nickname_str, std::move(mq_in), err_code)` may emit an error.
   *   - These are invoked in that order; and if the first fails then the second is not invoked.
   *
   * Hence the eventualities are as follows:
   *   - If neither fails: This ctor will neither throw nor emit a truthy `*err_code`.  Yay!
   *   - If one fails: This ctor shall either throw or emit the truthy `*err_code` associated with the one that
   *     did fail.  (You cannot know which.  Sorry.)  Just give up on `*this` (though technically it can
   *     still be moved-to).
   *   - Both cannot fail due to the algorithm outlined above.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname_str
   *        See nickname().
   * @param mq_out
   *        Open handle to the underlying MQ for traffic *from* `*this` *to* opposing peer.
   *        The peer object becomes owned by `*this` via move.  `mq_out` becomes unusable for transmission.
   * @param mq_in
   *        Open handle to the underlying MQ for traffic *to* `*this` *from* opposing peer.
   *        The peer object becomes owned by `*this` via move.  `mq_in` becomes unusable for transmission.
   * @param err_code
   *        See above.
   */
  explicit Mqs_channel(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                       Mq&& mq_out, Mq&& mq_in, Error_code* err_code = 0);

  /// Identical to Channel default ctor.
  Mqs_channel();
}; // class Mqs_channel

/**
 * A Channel with a *blobs pipe* consisting of 2 MQs of type Persistent_mq_handle (template arg); and
 * a *handles pipe* over a Unix domain socket connection.  See also the 2 aliases
 * #Posix_mqs_socket_stream_channel and #Bipc_mqs_socket_stream_channel (also in transport::sync_io).
 *   - send_blob(), async_receive_blob() will work (over MQs).
 *   - send_native_handle(), async_receive_native_handle() will work (over Unix domain socket stream transport).
 *
 * In practice this is the most full-featured Channel available as of this writing.  The competition would be
 * Socket_stream_channel which uses a single Unix domain socket connection: It features only one full-duplex pipe,
 * but since a Native_handle_sender::send_native_handle() (and conversely
 * Native_handle_receiver::async_receive_native_handle()) supports sending a blob without any accompanying
 * #Native_handle, it should not be necessary to bundle 2 underlying full-duplex pipes; one suffices and uses fewer
 * resources and features less complexity.
 *
 * @tparam Persistent_mq_handle
 *         Implements that concept.
 *
 * @tparam SIO
 *         Selects between `transport::sync_io::` and `transport::` version of held type(s); hence
 *         between Channel::S_IS_SYNC_IO_OBJ and Channel::S_IS_ASYNC_IO_OBJ.
 */
template<bool SIO, typename Persistent_mq_handle>
class Mqs_socket_stream_channel :
  public Mqs_channel<SIO,
                     Persistent_mq_handle,
                     std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                     std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>>
{
public:
  // Types.

  /// Short-hand for the Persistent_mq_handle concrete type used for blobs pipe.
  using Mq = Persistent_mq_handle;

  /// Short-hand for base class.
  using Base = Mqs_channel<SIO,
                           Persistent_mq_handle,
                           std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>,
                           std::conditional_t<SIO, sync_io::Native_socket_stream, Native_socket_stream>>;

  // Constructors/destructor.

  /**
   * Constructs the Channel in PEER state.  `sock_stm` must be in PEER state; else behavior is undefined.
   * `mq_out` and `mq_in` must be suitable for Persistent_mq_handle PEER-state ctor (see it doc header); else
   * behavior is undefined.
   *
   * ### Error semantics ###
   * See Mqs_channel() ctor doc header.  The only source of error conditions is still MQs;
   * `sock_stm` is simply moved-from as-is.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname_str
   *        See nickname().
   * @param mq_out
   *        Open handle to the underlying MQ for traffic *from* `*this` *to* opposing peer.
   *        The peer object becomes owned by `*this` via move.  `mq_out` becomes unusable for transmission.
   * @param mq_in
   *        Open handle to the underlying MQ for traffic *to* `*this` *from* opposing peer.
   *        The peer object becomes owned by `*this` via move.  `mq_in` becomes unusable for transmission.
   * @param sock_stm
   *        The peer object becomes owned by `*this` via move.  `sock_stm` becomes as-if default-cted
   *        (NULL state).
   * @param err_code
   *        See above.
   */
  explicit Mqs_socket_stream_channel(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                     Mq&& mq_out, Mq&& mq_in,
                                     typename Base::Base::Native_handle_sender_obj&& sock_stm,
                                     Error_code* err_code = 0);

  /// Identical to Channel default ctor.
  Mqs_socket_stream_channel();

  // Methods.

  /**
   * Identical to Native_socket_stream::remote_peer_process_credentials(): OS-reported process credential
   * (PID, etc.) info about the *other* connected peer's process.  See its doc header.
   *
   * @param err_code
   *        See above.
   * @return See above.  If `*this` is as-if-default-cted then returns default-cted value.
   */
  util::Process_credentials remote_peer_process_credentials(Error_code* err_code = 0) const;
}; // class Mqs_socket_stream_channel

// Free functions: in *_fwd.hpp.

// Channel template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CHANNEL \
  template<typename Blob_sender, typename Blob_receiver, typename Native_handle_sender, typename Native_handle_receiver>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CHANNEL \
  Channel<Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver>

TEMPLATE_CHANNEL
CLASS_CHANNEL::Channel() :
  Channel(nullptr, "")
{
  // Cool.
}

TEMPLATE_CHANNEL
CLASS_CHANNEL::Channel(flow::log::Logger* logger_ptr, util::String_view nickname_str) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_nickname(nickname_str)
{
  FLOW_LOG_INFO("Channel [" << *this << "]: Created (no pipes bundled yet).");

  // Now they must call init_*_pipe() for any desired pipes they want to bundle in *this Channel.
}

TEMPLATE_CHANNEL
typename CLASS_CHANNEL::Async_io_obj CLASS_CHANNEL::async_io_obj()
{
  static_assert(S_IS_SYNC_IO_OBJ, "Cannot make an async-I/O object from an async-I/O object... just use move ctor.");

  assert(initialized()
         && "The adopted core *this must be initialized already.  It would actually be possible to be half-initialized "
            "and initialize the remaining pipe(s) via init_*(), but for the time being we are outlawing it "
            "out of an abundance of feelings of anti-entropy.");

  Async_io_obj target(get_logger(), nickname());
  if constexpr(S_HAS_BLOB_PIPE)
  {
    if constexpr(std::is_same_v<Blob_sender_obj, Blob_receiver_obj>)
    {
      target.init_blob_pipe(typename Async_io_obj::Blob_sender_obj(std::move(*(blob_snd()))));
    }
    else
    {
      target.init_blob_pipe(typename Async_io_obj::Blob_sender_obj(std::move(*(blob_snd()))),
                            typename Async_io_obj::Blob_receiver_obj(std::move(*(blob_rcv()))));
    }
  }
  if constexpr(S_HAS_NATIVE_HANDLE_PIPE)
  {
    if constexpr(std::is_same_v<Native_handle_sender_obj, Native_handle_receiver_obj>)
    {
      target.init_native_handle_pipe(typename Async_io_obj::Native_handle_sender_obj(std::move(*(hndl_snd()))));
    }
    else
    {
      target.init_native_handle_pipe(typename Async_io_obj::Native_handle_sender_obj(std::move(*(hndl_snd()))),
                                     typename Async_io_obj::Native_handle_receiver_obj(std::move(*(hndl_rcv()))));
    }
  }

  FLOW_LOG_INFO("Channel [" << target << "]: Created from sync_io::Channel core.");

  // As promised: *this becomes as-if default-cted.  So this will reset Logger and nickname too.
  *this = Channel();

  assert(target.initialized());

  return target;
} // Channel::async_io_obj()

TEMPLATE_CHANNEL
CLASS_CHANNEL::Channel(Channel&&) = default;

TEMPLATE_CHANNEL
CLASS_CHANNEL& CLASS_CHANNEL::operator=(Channel&&) = default;

TEMPLATE_CHANNEL
CLASS_CHANNEL::~Channel()
{
  FLOW_LOG_INFO("Channel [" << *this << "]: Deleting.  Component pipe peers (if any) shall now shut down.");
}

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::init_blob_pipe(Blob_sender_obj&& snd_and_rcv)
{
  static_assert(S_HAS_BLOB_PIPE,
                "Compilable only given the compile-time presence of that pipe.");
  static_assert(std::is_same_v<Blob_sender_obj, Blob_receiver_obj>,
                "Compilable only if 1 object can do both directions.");

  if (m_blob_snd || m_blob_rcv)
  {
    FLOW_LOG_WARNING("Channel [" << *this << "]: init_blob_pipe() succeeded before yet was called again.  Ignoring.");
    return false;
  }
  // else

  m_blob_snd.emplace(std::move(snd_and_rcv));
  // m_blob_rcv remains null.  Hence blob_rcv() knows to reinterpret_cast<Blob_receiver_obj&>(*m_blob_snd).

  FLOW_LOG_INFO("Channel [" << *this << "]: Added blob pipe with common sender/receiver (details to the left here).");

  return true;
} // Channel::init_blob_pipe()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::init_blob_pipe(Blob_sender_obj&& snd, Blob_receiver_obj&& rcv)
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");
  static_assert(!std::is_same_v<Blob_sender_obj, Blob_receiver_obj>,
                "Compilable only if each object does a direction.");

  m_blob_snd.emplace(std::move(snd));
  m_blob_rcv.emplace(std::move(rcv));

  FLOW_LOG_INFO("Channel [" << *this << "]: Added blob pipe with separate sender/receiver (details to the left here).");
  return true;
} // Channel::init_blob_pipe()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::init_native_handle_pipe(Native_handle_sender_obj&& snd_and_rcv)
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");
  static_assert(std::is_same_v<Native_handle_sender_obj, Native_handle_receiver_obj>,
                "Compilable only if 1 object can do both directions.");

  if (m_hndl_snd || m_hndl_rcv)
  {
    FLOW_LOG_WARNING("Channel [" << *this << "]: "
                     "init_native_handle_pipe() succeeded before yet was called again.  Ignoring.");
    return false;
  }
  // else

  m_hndl_snd.emplace(std::move(snd_and_rcv));
  // m_hndl_rcv remains null.  Hence hndl_rcv() knows to reinterpret_cast<Native_handle_receiver_obj&>(*m_hndl_snd).

  FLOW_LOG_INFO("Channel [" << *this << "]: Added handle pipe with common sender/receiver (details to the left here).");

  return true;
} // Channel::init_native_handle_pipe()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::init_native_handle_pipe(Native_handle_sender_obj&& snd, Native_handle_receiver_obj&& rcv)
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");
  static_assert(!std::is_same_v<Native_handle_sender_obj, Native_handle_receiver_obj>,
                "Compilable only if each object does a direction.");

  m_hndl_snd.emplace(std::move(snd));
  m_hndl_rcv.emplace(std::move(rcv));

  FLOW_LOG_INFO("Channel [" << *this << "]: "
                "Added handle pipe with separate sender/receiver (details to the left here).");
  return true;
} // Channel::init_native_handle_pipe()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::initialized(bool suppress_log) const
{
  /* These checks are the bottom line -- static_assert()s and `if constexpr()`s checked everything else already.
   * (In particular: static_assert() already assured at least one pipe is enabled type-wise.  So just check each
   * pipe separately.)  Even these checks could be more economical (but wordier); as for instance it would not
   * have been possible for S_HAS_BLOB_PIPE=false but m_blob_snd=true; but let's just keep it simple. */

  if (S_HAS_BLOB_PIPE != bool(m_blob_snd))
  {
    if (!suppress_log)
    {
      FLOW_LOG_WARNING("Channel [" << *this << "]: "
                       "Blob_sender=/=Null_peer? = [" << S_HAS_BLOB_PIPE << "]; yet "
                       "the Blob_sender has been initialized? = [" << bool(m_blob_snd) << "].  Misconfigured?  Bug?");
    }
    return false;
  }
  // else

  if (S_HAS_NATIVE_HANDLE_PIPE != bool(m_hndl_snd))
  {
    if (!suppress_log)
    {
      FLOW_LOG_WARNING("Channel [" << *this << "]: "
                       "Native_handle_sender=/=Null_peer? = [" << S_HAS_NATIVE_HANDLE_PIPE << "]; yet "
                       "the Native_handle_sender has been initialized? = [" << bool(m_hndl_snd) << "].  "
                       "Misconfigured?  Bug?");
    }
    return false;
  }
  // else

  return true;
} // Channel::initialized()

TEMPLATE_CHANNEL
typename CLASS_CHANNEL::Blob_sender_obj* CLASS_CHANNEL::blob_snd()
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");
  return &(*m_blob_snd);
}

TEMPLATE_CHANNEL
typename CLASS_CHANNEL::Blob_receiver_obj* CLASS_CHANNEL::blob_rcv()
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  // Type-safety ensured in init_blob_pipe().
  return m_blob_rcv
           ? &(*m_blob_rcv)
           : (m_blob_snd ? &(reinterpret_cast<Blob_receiver_obj&>(*m_blob_snd))
                         : nullptr);
}

TEMPLATE_CHANNEL
typename CLASS_CHANNEL::Native_handle_sender_obj* CLASS_CHANNEL::hndl_snd()
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");
  return &(*m_hndl_snd);
}

TEMPLATE_CHANNEL
typename CLASS_CHANNEL::Native_handle_receiver_obj* CLASS_CHANNEL::hndl_rcv()
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  // Type-safety ensured in init_native_handle_pipe().
  return m_hndl_rcv
           ? &(*m_hndl_rcv)
           : (m_hndl_snd ? &(reinterpret_cast<Native_handle_receiver_obj&>(*m_hndl_snd))
                         : nullptr);
}

TEMPLATE_CHANNEL
const typename CLASS_CHANNEL::Blob_sender_obj* CLASS_CHANNEL::blob_snd() const
{
  return const_cast<Channel*>(this)->blob_snd();
}

TEMPLATE_CHANNEL
const typename CLASS_CHANNEL::Blob_receiver_obj* CLASS_CHANNEL::blob_rcv() const
{
  return const_cast<Channel*>(this)->blob_rcv();
}

TEMPLATE_CHANNEL
const typename CLASS_CHANNEL::Native_handle_sender_obj* CLASS_CHANNEL::hndl_snd() const
{
  return const_cast<Channel*>(this)->hndl_snd();
}

TEMPLATE_CHANNEL
const typename CLASS_CHANNEL::Native_handle_receiver_obj* CLASS_CHANNEL::hndl_rcv() const
{
  return const_cast<Channel*>(this)->hndl_rcv();
}

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::send_blob(const util::Blob_const& blob, Error_code* err_code)
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->send_blob(blob, err_code);
} // Channel::send_blob()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::send_native_handle(Native_handle hndl_or_null, const util::Blob_const& meta_blob,
                                       Error_code* err_code)
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->send_native_handle(hndl_or_null, meta_blob, err_code);
} // Channel::send_native_handle()

TEMPLATE_CHANNEL
template<typename Task_err>
bool CLASS_CHANNEL::async_end_sending(Task_err&& on_done_func)
{
  static_assert(S_IS_ASYNC_IO_OBJ, "This overload usable only with async-I/O-pattern object.");

  using flow::async::Task_asio_err;
  using flow::util::Mutex_non_recursive;
  using flow::util::Lock_guard;
  using boost::make_shared;

  if constexpr(S_HAS_BLOB_PIPE_ONLY)
  {
    const auto peer = blob_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->async_end_sending(std::move(on_done_func));
  }
  else if constexpr(S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    const auto peer = hndl_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->async_end_sending(std::move(on_done_func));
  }
  else
  {
    static_assert(S_HAS_2_PIPES, "Wat!");

    FLOW_LOG_INFO("Channel [" << *this << "]: Request to send/queue graceful-close; shall proceed along "
                  "the 1-2 pipes listed earlier in this message; if not dupe-call; and "
                  "both pipes available then the on-done callback shall be invoked after the later of 2 individual "
                  "on-done callbacks triggers.");

    const auto peer1 = blob_snd();
    const auto peer2 = hndl_snd();
    assert(peer1 && peer2 && "Ensure initialized() first.");

    /* This is the only real logic we have to worry about in Channel concept-implementing API, because
     * async_end_sending() doc header promises to apply to both pipes, and indeed both pipes are enabled.
     * We promised that on_done_func() shall be invoked after each pipe's completion handler has finished.
     * So we just wrap on_done_func() with a handler that tracks whether it's the 1st or 2nd handler;
     * and in the latter case invokes the real on_done_func().
     *
     * The other difficulty is to track the error result.  We promised the following semantics:
     *   - If no error occurs report success (duh).
     *   - If one error occurs, while the other one succeeds, report the one error (duh).
     *   - If both fail report the first one to happen chronologically.
     *
     * So we track all that in a simple State struct which exists in the universe via capture by shared_ptr
     * until the 2nd completion handler runs.
     *
     * Thready safety: More details below but just a reminder: F in X.async_end_sending(F) is invoked at some point
     * no matter what (at the latest, when X is destroyed); and it is invoked from some unspecified thread that
     * is not the current thread (in which we are now executing) -- for async-I/O pattern -- or else at a time
     * of the user's choosing synchronously (sync_io pattern).  Since we do
     * X1.async_end_sending(F1) + X2.async_end_sending(F2), F1() and F2() very well might execute concurrently.
     * So we *do* need to protect against concurrent access if any. */

    struct State
    {
      Mutex_non_recursive m_mutex; // Protects m_n_done and m_err_code1.
      unsigned int m_n_done; // Init: 0.
      Error_code m_err_code1; // Init: Success.
      Task_asio_err m_on_done_func; // Set just below.  (Thread safety: It is only read once hence no sync required.)
    };
    const auto state = make_shared<State>();
    state->m_on_done_func = std::move(on_done_func); // @todo Make m_on_done_func const; use {} init semantics.

    auto combined_on_done = [state](const Error_code& async_err_code) mutable
    {
      Error_code err_code = async_err_code; // We may overwrite it.
      {
        Lock_guard<Mutex_non_recursive> lock(state->m_mutex);
        const auto post_n_done = ++state->m_n_done;
        const bool all_done = post_n_done == 2;
        assert((post_n_done == 1) || all_done);

        if (!all_done)
        {
          // Wwe are the 1st handler to fire>: Mark down the result for the (all_done) clause below.
          state->m_err_code1 = err_code;
          return;
        }
        // else if (<we are the 2nd handler to fire>):

        // Report the 1st error to occur; or success if none occurred.
        if (state->m_err_code1)
        {
          err_code = state->m_err_code1;
        }
        // else { err_code remains == async_err_code. }
      } // Lock_guard<Mutex_non_recursive> lock(state->m_mutex);

      state->m_on_done_func(err_code);
    }; // auto combined_on_done =

    if (!peer1->async_end_sending(Task_asio_err(combined_on_done))) // Copy combined_on_done for the call below.
    {
      return false;
    }
    // else see our contract.

#ifndef NDEBUG
    const bool ok =
#endif
    peer2->async_end_sending(std::move(combined_on_done));
    assert(ok && "Either both should be dupe-calls or neither; do not use them individually only to then use this.");
    return true;
  } // else if constexpr(S_HAS_2_PIPES)
} // Channel::async_end_sending()

TEMPLATE_CHANNEL
template<typename Task_err>
bool CLASS_CHANNEL::async_end_sending(Error_code* sync_err_code_ptr, Task_err&& on_done_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");

  using flow::async::Task_asio_err;
  using boost::make_shared;
  using std::atomic;

  if constexpr(S_HAS_BLOB_PIPE_ONLY)
  {
    const auto peer = blob_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->async_end_sending(sync_err_code_ptr, std::move(on_done_func));
  }
  else if constexpr(S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    const auto peer = hndl_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->async_end_sending(sync_err_code_ptr, std::move(on_done_func));
  }
  else
  {
    static_assert(S_HAS_2_PIPES, "Wat!");

    FLOW_LOG_INFO("Channel [" << *this << "]: (SIO) Request to send/queue graceful-close; shall proceed along "
                  "the 2 pipes listed earlier in this message; if not dupe-call; and "
                  "both pipes available then the on-done callback shall be invoked after the later of 2 individual "
                  "on-done callbacks triggers.  If one completes synchronously then only one callback to await.");

    const auto peer1 = blob_snd();
    const auto peer2 = hndl_snd();
    assert(peer1 && peer2 && "Ensure initialized() first.");

    /* See the async-I/O overload first.  Then come back here.  This one has to deal with that stuff plus
     * some more: that each .async_end_sending()
     *   - might complete synchronously instead of memorizing handler to invoke later;
     *   - either way has Flow error conventions: if passed null sync_err_code_ptr it will throw on error.
     *
     * That said the below should be reasonable self-explanatory. */

    /* Again: m_n_done reaching 2 is essentially a barrier for invoking m_on_done_func().  So it is the only datum
     * protected against concurrent access.  Since it's just an int, we use atomic<> for synchronization. */
    struct State
    {
      atomic<unsigned int> m_n_done; // Init: 0.  Thread safety: See preceding comment.
      Error_code m_err_code1; // Init: Success.  Thread safety: It is only touched once hence no sync required.
      Task_asio_err m_on_done_func; // Assigned just below.  Thread safety: It is only read once hence no sync required.
    };
    const auto state = make_shared<State>();
    state->m_on_done_func = std::move(on_done_func); // @todo Make m_on_done_func const; use {} init semantics.

    auto combined_on_done = [state](const Error_code& async_err_code) mutable
    {
      const auto post_n_done = ++state->m_n_done;
      const bool all_done = post_n_done == 2;
      assert((post_n_done == 1) || all_done);

      if (all_done) // I.e., if (<we are the 2nd handler to fire>)
      {
        // Report the 1st error to occur; or success if none occurred.
        state->m_on_done_func(state->m_err_code1 ? state->m_err_code1 : async_err_code);
        return;
      }
      // else if (<we are the 1st handler to fire>): Mark down the result for the (all_done) clause above.
      state->m_err_code1 = async_err_code;
    };

    Error_code sync_err_code1;
    if (!peer1->async_end_sending(&sync_err_code1,
                                  Task_asio_err(combined_on_done))) // Copy combined_on_done for the call below.
    {
      return false;
    }
    // else see our contract.

    Error_code sync_err_code2;
#ifndef NDEBUG
    const bool ok =
#endif
    peer2->async_end_sending(&sync_err_code2, std::move(combined_on_done));
    assert(ok && "Either both should be dupe-calls or neither; do not use them individually only to then use this.");

    // Now the extra stuff about completing synchronously versus asynchronously.

    if ((sync_err_code1 != error::Code::S_SYNC_IO_WOULD_BLOCK) &&
        (sync_err_code2 != error::Code::S_SYNC_IO_WOULD_BLOCK))
    {
      // Both synchronously finished.
      const Error_code sync_err_code = sync_err_code1 ? sync_err_code1 : sync_err_code2;
      // Standard error-reporting semantics.
      if ((!sync_err_code_ptr) && sync_err_code)
      {
        throw flow::error::Runtime_error(sync_err_code, "Channel::async_end_sending(2)");
      }
      // else
      sync_err_code_ptr && (*sync_err_code_ptr = sync_err_code);
      // And if (!sync_err_code_ptr) + no error => no throw.
      return true;
    }
    // else: 1 or neither synchronously finished.  So we return would-block, while those 1-2 finish.
    if (sync_err_code1 != error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      ++state->m_n_done; // Simulate it being done already, so the other one (if even needed) will run user handler.
      state->m_err_code1 = sync_err_code1;
    }
    else if (sync_err_code2 != error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      ++state->m_n_done; // Ditto.
      state->m_err_code1 = sync_err_code2;
    }
    // else { Neither synchronously finished. }

    // Okay, godspeed to the 1-2 remaining async ones.
    return true;
  } // else if constexpr(HAS_2_PIPES)
} // Channel::async_end_sending(2)

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::end_sending()
{
  if constexpr(S_HAS_BLOB_PIPE_ONLY)
  {
    const auto peer = blob_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->end_sending();
  }
  else if constexpr(S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    const auto peer = hndl_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->end_sending();
  }
  else
  {
    static_assert(S_HAS_2_PIPES, "Wat!");

    const auto peer1 = blob_snd();
    const auto peer2 = hndl_snd();
    assert(peer1 && peer2 && "Ensure initialized() first.");

    if (!peer1->end_sending())
    {
      return false;
    }
    // else see our contract.

#ifndef NDEBUG
    const bool ok =
#endif
    peer2->end_sending();
    assert(ok && "Either both should be dupe-calls or neither; do not use them individually only to then use this.");
    return true;
  }
} // Channel::end_sending()

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::auto_ping(util::Fine_duration period)
{
  // @todo This is very similar to end_sending().  Add some code reuse.  This code is simple but not that short.

  if constexpr(S_HAS_BLOB_PIPE_ONLY)
  {
    const auto peer = blob_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->auto_ping(period);
  }
  else if constexpr(S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    const auto peer = hndl_snd();
    assert(peer && "Ensure initialized() first.");

    return peer->auto_ping(period);
  }
  else
  {
    static_assert(S_HAS_2_PIPES, "Wat!");

    const auto peer1 = blob_snd();
    const auto peer2 = hndl_snd();
    assert(peer1 && peer2 && "Ensure initialized() first.");

    if (!peer1->auto_ping(period))
    {
      return false;
    }
    // else see our contract.

#ifndef NDEBUG
    const bool ok =
#endif
    peer2->auto_ping(period);
    assert(ok && "Either both should be dupe-calls or neither; do not use them individually only to then use this.");
    return true;
  }
} // Channel::auto_ping()

TEMPLATE_CHANNEL
template<typename... Args>
bool CLASS_CHANNEL::async_receive_blob(Args&&... args)
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->async_receive_blob(std::forward<Args>(args)...);
}

TEMPLATE_CHANNEL
template<typename... Args>
bool CLASS_CHANNEL::async_receive_native_handle(Args&&... args)
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->async_receive_native_handle(std::forward<Args>(args)...);
}

TEMPLATE_CHANNEL
bool CLASS_CHANNEL::idle_timer_run(util::Fine_duration timeout)
{
  // @todo This is very similar to auto_ping().  Add some code reuse.  This code is simple but not that short.

  if constexpr(S_HAS_BLOB_PIPE_ONLY)
  {
    const auto peer = blob_rcv();
    assert(peer && "Ensure initialized() first.");

    return peer->idle_timer_run(timeout);
  }
  else if constexpr(S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    const auto peer = hndl_rcv();
    assert(peer && "Ensure initialized() first.");

    return peer->idle_timer_run(timeout);
  }
  else
  {
    static_assert(S_HAS_2_PIPES, "Wat!");

    const auto peer1 = blob_rcv();
    const auto peer2 = hndl_rcv();
    assert(peer1 && peer2 && "Ensure initialized() first.");

    if (!peer1->idle_timer_run(timeout))
    {
      return false;
    }
    // else see our contract.

#ifndef NDEBUG
    const bool ok =
#endif
    peer2->idle_timer_run(timeout);
    assert(ok && "Either both should be dupe-calls or neither; do not use them individually only to then use this.");
    return true;
  }
} // Channel::idle_timer_run()

TEMPLATE_CHANNEL
size_t CLASS_CHANNEL::send_blob_max_size() const
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->send_blob_max_size();
}

TEMPLATE_CHANNEL
size_t CLASS_CHANNEL::send_meta_blob_max_size() const
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->send_meta_blob_max_size();
}

TEMPLATE_CHANNEL
size_t CLASS_CHANNEL::receive_blob_max_size() const
{
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->receive_blob_max_size();
}

TEMPLATE_CHANNEL
size_t
  CLASS_CHANNEL::receive_meta_blob_max_size() const
{
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->receive_meta_blob_max_size();
}

TEMPLATE_CHANNEL
template<typename Create_ev_wait_hndl_func>
bool CLASS_CHANNEL::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");

  /* There will be a compile error below if we're not a sync_io:: type dude.  @todo static_assert() somehow for
   * a nicer compile error. */

  assert(initialized() && "It is in the contract, man.");

  // We could have fancier compile-time checks but doesn't seem worth it perf-wise.

  // Ensure all the applicable ones run.
  bool ok1 = true;
  bool ok2 = true;
  bool ok3 = true;
  bool ok4 = true;

  /* Forced to make a copy of the function, possibly a few times, but hopefully it's not perf-critical.
   * We could do it fewer times depending on the scenario, but again don't bother. */
  if constexpr(S_HAS_BLOB_PIPE)
  {
    if (m_blob_snd)
    {
      ok1 = m_blob_snd->replace_event_wait_handles(create_ev_wait_hndl_func);
    }
    if (m_blob_rcv)
    {
      ok2 = m_blob_rcv->replace_event_wait_handles(create_ev_wait_hndl_func);
    }
  }
  if constexpr(S_HAS_NATIVE_HANDLE_PIPE)
  {
    if (m_hndl_snd)
    {
      ok3 = m_hndl_snd->replace_event_wait_handles(create_ev_wait_hndl_func);
    }
    if (m_hndl_rcv)
    {
      ok4 = m_hndl_rcv->replace_event_wait_handles(create_ev_wait_hndl_func);
    }
  }
  return ok1 && ok2 && ok3 && ok4;
} // Channel::replace_event_wait_handles()

TEMPLATE_CHANNEL
template<typename Event_wait_func_t>
bool CLASS_CHANNEL::start_send_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->start_send_blob_ops(std::move(ev_wait_func));
}

TEMPLATE_CHANNEL
template<typename Event_wait_func_t>
bool CLASS_CHANNEL::start_send_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_snd();
  assert(peer && "Ensure initialized() first.");

  return peer->start_send_native_handle_ops(std::move(ev_wait_func));
}

TEMPLATE_CHANNEL
template<typename Event_wait_func_t>
bool CLASS_CHANNEL::start_receive_blob_ops(Event_wait_func_t&& ev_wait_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");
  static_assert(S_HAS_BLOB_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = blob_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->start_receive_blob_ops(std::move(ev_wait_func));
}

TEMPLATE_CHANNEL
template<typename Event_wait_func_t>
bool CLASS_CHANNEL::start_receive_native_handle_ops(Event_wait_func_t&& ev_wait_func)
{
  static_assert(S_IS_SYNC_IO_OBJ, "This overload usable only with sync_io-pattern object.");
  static_assert(S_HAS_NATIVE_HANDLE_PIPE, "Compilable only given the compile-time presence of that pipe.");

  const auto peer = hndl_rcv();
  assert(peer && "Ensure initialized() first.");

  return peer->start_receive_native_handle_ops(std::move(ev_wait_func));
}

TEMPLATE_CHANNEL
const std::string& CLASS_CHANNEL::nickname() const
{
  return m_nickname;
}

TEMPLATE_CHANNEL
std::ostream& operator<<(std::ostream& os, const CLASS_CHANNEL& val)
{
  using std::string;

  os << '[' << (val.nickname().empty() ? string("null") : val.nickname()) << "]@" << static_cast<const void*>(&val);

  /* The way this written -- skipping wordy (if technically faster) compile-time checking -- friendship is needed.
   * E.g., if Blob_sender=Null_peer, then m_blob_snd is compiled -- just always null -- but blob_snd() is not
   * compiled.  For conciseness we'd rather just check for null and be done with it, here.  (In other more
   * perf/mission--critical code we are much more stringent; like blob_snd() being un-callable by user in this
   * example.) */

  if (val.m_blob_snd)
  {
    os << " blob_pipes[";
    if (val.m_blob_rcv)
    {
      os << "snd_out[" << *val.m_blob_snd << "] rcv_in[" << *val.m_blob_rcv << ']';
    }
    else
    {
      os << "snd_rcv[" << *val.m_blob_snd << ']';
    }
  }

  if (val.m_hndl_snd)
  {
    os << " hndl_pipes[";
    if (val.m_hndl_rcv)
    {
      os << "snd_out[" << *val.m_hndl_snd << "] rcv_in[" << *val.m_hndl_rcv << ']';
    }
    else
    {
      os << "snd_rcv[" << *val.m_hndl_snd << ']';
    }
  }

  return os;
} // operator<<(ostream&, Channel)

#undef CLASS_CHANNEL
#undef TEMPLATE_CHANNEL

// Other template implementations.

template<bool SIO>
Socket_stream_channel<SIO>::Socket_stream_channel(flow::log::Logger* logger_ptr, util::String_view nickname_str,
                                                  typename Base::Native_handle_sender_obj&& sock_stm) :
  Base(logger_ptr, nickname_str)
{
  Base::init_native_handle_pipe(std::move(sock_stm));
  assert(Base::initialized() && "Check the logs above this; something is misconfigured, maybe at compile-time.");
}

template<bool SIO>
Socket_stream_channel<SIO>::Socket_stream_channel() = default;

template<bool SIO>
util::Process_credentials Socket_stream_channel<SIO>::remote_peer_process_credentials(Error_code* err_code) const
{
  const auto stream = Base::hndl_snd();
  return stream ? stream->remote_peer_process_credentials(err_code) : util::Process_credentials();
}

template<bool SIO>
Socket_stream_channel_of_blobs<SIO>::Socket_stream_channel_of_blobs(flow::log::Logger* logger_ptr,
                                                                    util::String_view nickname_str,
                                                                    typename Base::Blob_sender_obj&& sock_stm) :
  Base(logger_ptr, nickname_str)
{
  Base::init_blob_pipe(std::move(sock_stm));
  assert(Base::initialized() && "Check the logs above this; something is misconfigured, maybe at compile-time.");
}

template<bool SIO>
Socket_stream_channel_of_blobs<SIO>::Socket_stream_channel_of_blobs() = default;

template<bool SIO>
util::Process_credentials
  Socket_stream_channel_of_blobs<SIO>::remote_peer_process_credentials(Error_code* err_code) const
{
  const auto stream = Base::blob_snd();
  return stream ? stream->remote_peer_process_credentials(err_code) : util::Process_credentials();
}

template<bool SIO, typename Persistent_mq_handle, typename Native_handle_sender, typename Native_handle_receiver>
Mqs_channel<SIO, Persistent_mq_handle, Native_handle_sender, Native_handle_receiver>::Mqs_channel
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq_out, Mq&& mq_in, Error_code* err_code) :

  Base(logger_ptr, nickname_str)
{
  using flow::error::Runtime_error;
  using flow::util::ostream_op_string;
  using util::String_view;

  /* We must create a Blob_stream_mq_sender(mq_out) and Blob_stream_mq_receiver(mq_in), in that order;
   * and do the latter only if the former doesn't fail; and report the 1st (and only) error to occur; or
   * report success if no problem occurs.  Oh and we must observe the usual error reporting semantics wherein
   * err_code==null => throw Runtime_error(<the error code>) on error/don't throw on success;
   * err_code!=null => set *err_code to truthy on error/falsy on success.
   *
   * So... that's what all that stuff below does.  Hopefully it's not too complicated to follow given the above. */

  // Regardless: The last op's result is stored here.
  Error_code our_err_code;

  // A little helper to invoke after each op: no-op if successful; else either throw or set *err_code on failure.
  const auto handle_result = [&](bool snd_else_rcv)
  {
    if (our_err_code)
    {
      if (err_code)
      {
        *err_code = our_err_code;
        return;
      }
      // else
      throw Runtime_error(our_err_code,
                          ostream_op_string("Mqs_channel::Mqs_channel():",
                                            snd_else_rcv ? "mq_snd_init" : "mq_rcv_init"));
    }
    // else our_err_code is still success.
  };

  typename Base::Blob_sender_obj snd_out(logger_ptr, nickname_str, std::move(mq_out), &our_err_code);
  handle_result(true); // May throw.
  typename Base::Blob_receiver_obj rcv_in;
  if (!our_err_code) // As promised: don't try to initialize the 2nd if the 1st failed anyway.
  {
    rcv_in = typename Base::Blob_receiver_obj(logger_ptr, nickname_str, std::move(mq_in), &our_err_code);
    handle_result(false); // May throw.
  }

  // If one threw, then we're not here (we've failed; our job is done).  Otherwise:

  if (our_err_code)
  {
    // There was an error.  We've failed; no point in init_blob_pipe().
    assert(err_code && "Bug in this ctor?  Problem initializing, but it didn't throw, yet err_code is null.");
    return;
  }
  // else if (!our_err_code): We've succeeded.

  if (err_code)
  {
    // Non-exception form was invoked, so mark that indeed we succeeded.
    err_code->clear();
  }
  // else { Exception form was invoked.  The fact we didn't throw = how we mark we've succeeded. }

  // We're good to go.  This can't fail (throw, or whatever):
  Base::init_blob_pipe(std::move(snd_out), std::move(rcv_in));

  if constexpr(std::is_same_v<typename Base::Native_handle_sender_obj, Null_peer>)
  {
    assert(Base::initialized() && "Check the logs above this; something is misconfigured, maybe at compile-time.");
  }
  // else { Probably some sub-ctor, or whatever, will do init_native_handle_pipe(). }
} // Mqs_channel::Mqs_channel()

template<bool SIO, typename Persistent_mq_handle, typename Native_handle_sender, typename Native_handle_receiver>
Mqs_channel<SIO, Persistent_mq_handle, Native_handle_sender, Native_handle_receiver>::Mqs_channel() = default;

template<bool SIO, typename Persistent_mq_handle>
Mqs_socket_stream_channel<SIO, Persistent_mq_handle>::Mqs_socket_stream_channel
  (flow::log::Logger* logger_ptr, util::String_view nickname_str, Mq&& mq_out, Mq&& mq_in,
   typename Base::Base::Native_handle_sender_obj&& sock_stm, Error_code* err_code) :

  Base(logger_ptr, nickname_str, std::move(mq_out), std::move(mq_in), err_code)
{
  // Just have the handles pipe to still init.  But first bail if the blob pipe (MQs) failed init w/o throwing.
  if (err_code && *err_code)
  {
    return;
  }
  // else

  Base::Base::init_native_handle_pipe(std::move(sock_stm));
  assert(Base::Base::initialized() && "Check the logs above this; something is misconfigured, maybe at compile-time.");
}

template<bool SIO, typename Persistent_mq_handle>
Mqs_socket_stream_channel<SIO, Persistent_mq_handle>::Mqs_socket_stream_channel() = default;

template<bool SIO, typename Persistent_mq_handle>
util::Process_credentials
  Mqs_socket_stream_channel<SIO, Persistent_mq_handle>::remote_peer_process_credentials(Error_code* err_code) const
{
  const auto stream = Base::Base::hndl_snd();
  return stream ? stream->remote_peer_process_credentials(err_code) : util::Process_credentials();
}

} // namespace ipc::transport
