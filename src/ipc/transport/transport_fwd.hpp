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

#include "ipc/util/shared_name_fwd.hpp"
#include "ipc/util/native_handle.hpp"

/**
 * Flow-IPC module providing transmission of structured messages and/or low-level blobs (and more)
 * between pairs of processes.  See namespace ::ipc doc header for an overview of Flow-IPC modules including
 * how ipc::transport relates to the others.  Then return here.  A synopsis follows:
 *
 * The main transport features of ipc::transport are: class template struc::Channel (for structured message and
 * native handle transport) and various lower-level utilities (Channel; blob/handle streams; message queue
 * (MQ) streams).  Structured transmission facilities (struc::Channel being the main guy) are segregated in
 * sub-namespace transport::struc.
 *
 * Generally speaking, to communicate (via struc::Channel and others), the two processes A and B that intend to
 * talk must have established a broad conversation called a *session* within which all communication occurs.
 * In fact, a transport::struc::Channel wraps a transport::Channel, and the latter can be established, in factory-ish
 * fashion and otherwise, from an ipc::session::Session.  Hence, see namespace ipc::session doc header to learn about
 * establishing/terminating sessions.  Once you have a session::Session, you can actually use main ipc::transport
 * facilities.
 *
 * That said: ipc::transport does *not* require ipc::session to be used: One can instantiate all the various IPC
 * mechanisms therein directly.  ipc::session provides the lifecycle and organization to make this as simple as
 * possible (but no simpler).  In that sense ipc::transport has the essential building blocks; ipc::session provides
 * access to those building blocks in one possible fashion -- for example by establishing a naming convention
 * for the various required `Shared_name`s taken by the various ipc::transport constructors.  Formally speaking
 * there can certainly be other fashions of organizing ipc::transport resources.  Therefore the API design of
 * ipc::transport is not rigid.
 *
 * @internal
 * @todo An ipc::transport internal protocol versioning system is likely necessary at some point.  This can *probably*
 * be put off until after the first production-used version of Flow-IPC is shipped.  It's probably best to at least
 * make a plan (and verify that it can indeed be put off in a forward-compatible way) before then however.
 * One should contemplate any protocol that might change which includes the low-level (core-layer-internal)
 * protocols used by each of ipc::transport::Blob_stream_mq_sender + ipc::transport::Blob_stream_mq_receiver,
 * ipc::transport::Native_socket_stream; and struc::Channel.  On a related note one should contemplate the
 * versioning and/or forward-compatibility of the session master ipc::transport::struc::Channel protocol
 * used internally by ipc::session.
 */
namespace ipc::transport
{

// Types.

// Find doc headers near the bodies of these compound types.

class Native_socket_stream;
class Native_socket_stream_acceptor;
class Posix_mq_handle;
class Bipc_mq_handle;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver;
class Null_peer;
template<typename Blob_sender, typename Blob_receiver, typename Native_handle_sender, typename Native_handle_receiver>
class Channel;
template<bool SIO>
class Socket_stream_channel;
template<bool SIO>
class Socket_stream_channel_of_blobs;
template<bool SIO,
         typename Persistent_mq_handle,
         typename Native_handle_sender = Null_peer, typename Native_handle_receiver = Null_peer>
class Mqs_channel;
template<bool SIO,
         typename Persistent_mq_handle>
class Mqs_socket_stream_channel;
class Protocol_negotiator;

/// Convenience alias for the commonly used type util::Native_handle.
using Native_handle = util::Native_handle;

/// Convenience alias for the commonly used type util::Shared_name.
using Shared_name = util::Shared_name;

/**
 * Convenience alias: Blob_sender via unidirectional POSIX MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mq_sender = Blob_stream_mq_sender<Posix_mq_handle>;

/**
 * Convenience alias: Blob_receiver via unidirectional POSIX MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mq_receiver = Blob_stream_mq_receiver<Posix_mq_handle>;

/**
 * Convenience alias: Blob_sender via unidirectional bipc MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mq_sender = Blob_stream_mq_sender<Bipc_mq_handle>;

/**
 * Convenience alias: Blob_receiver via unidirectional bipc MQ (message queue).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mq_receiver = Blob_stream_mq_receiver<Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver) at one end of full-duplex (bidirectional) pipe
 * composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mqs_channel_of_blobs = Mqs_channel<false, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver) at one end of full-duplex (bidirectional) pipe
 * composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mqs_channel_of_blobs = Mqs_channel<false, Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver)
 * at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs
 * (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain stream connection,
 * transmitting native-handle-and/or-meta-blob messages.
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Posix_mqs_socket_stream_channel = Mqs_socket_stream_channel<false, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver)
 * at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing unidirectional bipc MQs
 * (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain stream connection,
 * transmitting native-handle-and/or-meta-blob messages.
 *
 * Tip: In `sync_io` sub-namespace there is the `sync_io`-pattern counterpart.
 */
using Bipc_mqs_socket_stream_channel = Mqs_socket_stream_channel<false, Bipc_mq_handle>;

// Free functions.

/**
 * Prints string representation of the given `Native_socket_stream` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);

/**
 * Prints string representation of the given `Native_socket_stream_acceptor` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_acceptor
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_receiver
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_sender
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given Bipc_mq_handle to the given `ostream`.
 *
 * @relatesalso Bipc_mq_handle
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Bipc_mq_handle& val);

/**
 * Prints string representation of the given Posix_mq_handle to the given `ostream`.
 *
 * @relatesalso Posix_mq_handle
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Posix_mq_handle& val);

/**
 * Prints string representation of the given `Channel` to the given `ostream`.
 *
 * @relatesalso Channel
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Blob_sender, typename Blob_receiver, typename Native_handle_sender, typename Native_handle_receiver>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver>& val);

/**
 * Dummy that is never invoked.  It must still exist in order for Channel to build successfully with at least 1
 * Null_peer template arg.
 *
 * Assertion may trip if this is invoked.  Formally behavior is undefined.
 *
 * @relatesalso Null_peer
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Null_peer& val);

/**
 * Implements Persistent_mq_handle related concept: Swaps two objects.
 *
 * @relatesalso Bipc_mq_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Bipc_mq_handle& val1, Bipc_mq_handle& val2);

/**
 * Implements Persistent_mq_handle related concept: Swaps two objects.
 *
 * @relatesalso Posix_mq_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Posix_mq_handle& val1, Posix_mq_handle& val2);

} // namespace ipc::transport

/**
 * `sync_io`-pattern counterparts to async-I/O-pattern object types in parent namespace ipc::transport.
 * For example transport::sync_io::Native_socket_stream <=> transport::Native_socket_stream.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 */
namespace ipc::transport::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.
class Native_socket_stream;
class Native_socket_stream_acceptor;
template<typename Persistent_mq_handle>
class Blob_stream_mq_sender;
template<typename Persistent_mq_handle>
class Blob_stream_mq_receiver;

/// Convenience alias: sync_io::Blob_sender via unidirectional POSIX MQ (message queue).
using Posix_mq_sender = Blob_stream_mq_sender<Posix_mq_handle>;
/// Convenience alias: sync_io::Blob_receiver via unidirectional POSIX MQ (message queue).
using Posix_mq_receiver = Blob_stream_mq_receiver<Posix_mq_handle>;
/// Convenience alias: sync_io::Blob_sender via unidirectional bipc MQ (message queue).
using Bipc_mq_sender = Blob_stream_mq_sender<Bipc_mq_handle>;
/// Convenience alias: sync_io::Blob_receiver via unidirectional bipc MQ (message queue).
using Bipc_mq_receiver = Blob_stream_mq_receiver<Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver) at one end of full-duplex
 * (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 */
using Posix_mqs_channel_of_blobs = transport::Mqs_channel<true, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver) at one end of full-duplex
 * (bidirectional) pipe composed of 2 opposite-facing unidirectional POSIX MQs (message queues).
 */
using Bipc_mqs_channel_of_blobs = transport::Mqs_channel<true, Bipc_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver, sync_io::Native_handle_sender,
 * sync_io::Native_handle_receiver) at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing
 * unidirectional POSIX MQs (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain
 * stream connection, transmitting native-handle-and/or-meta-blob messages.
 */
using Posix_mqs_socket_stream_channel = Mqs_socket_stream_channel<true, Posix_mq_handle>;

/**
 * Convenience alias: Channel peer (sync_io::Blob_sender, sync_io::Blob_receiver, sync_io::Native_handle_sender,
 * sync_io::Native_handle_receiver) at one end of a full-duplex (bidirectional) pipe composed of 2 opposite-facing
 * unidirectional bipc MQs (message queues), transmitting blobs only; and a full-duplex pipe over a Unix domain
 * stream connection, transmitting native-handle-and/or-meta-blob messages.
 */
using Bipc_mqs_socket_stream_channel = Mqs_socket_stream_channel<true, Bipc_mq_handle>;

// Free functions.

/**
 * Prints string representation of the given `Native_socket_stream` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream& val);

/**
 * Prints string representation of the given `Native_socket_stream_acceptor` to the given `ostream`.
 *
 * @relatesalso Native_socket_stream_acceptor
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_socket_stream_acceptor& val);

/**
 * Prints string representation of the given `Blob_stream_mq_sender` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_sender
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_sender<Persistent_mq_handle>& val);

/**
 * Prints string representation of the given `Blob_stream_mq_receiver` to the given `ostream`.
 *
 * If object is default-cted (or moved-from), this will output something graceful indicating this.
 *
 * @relatesalso Blob_stream_mq_receiver
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Persistent_mq_handle>
std::ostream& operator<<(std::ostream& os, const Blob_stream_mq_receiver<Persistent_mq_handle>& val);

} // namespace ipc::transport::sync_io
