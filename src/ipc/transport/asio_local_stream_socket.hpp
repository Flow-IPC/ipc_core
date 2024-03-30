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

#include "ipc/transport/detail/asio_local_stream_socket.hpp"
#include "ipc/util/process_credentials.hpp"
#include <flow/common.hpp>
#include <stdexcept>

// See asio_local_stream_socket_fwd.hpp for doc header (intro) to this namespace.
namespace ipc::transport::asio_local_stream_socket
{

// Types.

#ifndef FLOW_OS_LINUX
static_assert(false, "Flow-IPC must define Opt_peer_process_credentials w/ Linux SO_PEERCRED semantics.  "
                       "Build in Linux only.");
#endif

/**
 * Gettable (read-only) socket option for use with asio_local_stream_socket::Peer_socket `.get_option()` in order to
 * get the connected opposing peer process's credentials (PID/UID/GID/etc.).  Note accessors and data are
 * in non-polymorphic base util::Process_credentials.
 *
 * If one calls `X.get_option(Opt_peer_process_credentials& o)` on #Peer_socket `X`, and `X` is connected to opposing
 * peer socket in process P, then `o.*()` credential accessors (such as `o.process_id()`) shall return values that were
 * accurate about process P, at the time P executed `Peer_socket::connect()` or `local_ns::connect_pair()` yielding
 * the connection to "local" peer `X`.
 *
 * @see boost.asio docs: `GettableSocketOption` in boost.asio docs: implemented concept.
 *
 * @internal
 * This is the Linux `getsockopt()` option with level `AF_LOCAL` (a/k/a `AF_UNIX`), name `SO_PEERCRED`.
 */
class Opt_peer_process_credentials :
  public util::Process_credentials
{
public:
  // Constructors/destructor.

  /// Default ctor: each value is initialized to zero or equivalent.
  Opt_peer_process_credentials();

  /**
   * Boring copy ctor.
   * @param src
   *        Source object.
   */
  Opt_peer_process_credentials(const Opt_peer_process_credentials& src);

  // Methods.

  /**
   * Boring copy assignment.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Opt_peer_process_credentials& operator=(const Opt_peer_process_credentials& src);

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::level()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `AF_LOCAL` a/k/a `AF_UNIX`.
   */
  template<typename Protocol>
  int level(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::name()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `SO_PEERCRED`.
   */
  template<typename Protocol>
  int name(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::data()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `SO_PEERCRED`.
   */
  template<typename Protocol>
  void* data(const Protocol& proto);

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::size()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @return See concept API.
   *
   * @internal
   * It's `sizeof(ucred)`.
   */
  template<typename Protocol>
  size_t size(const Protocol& proto) const;

  /**
   * For internal boost.asio use, to enable `Peer_socket::get_option(Opt_peer_process_credentials&)` to work.
   *
   * @see boost.asio docs: `GettableSocketOption::resize()` in boost.asio docs: implemented concept.
   *
   * @tparam Protocol
   *         See concept API.
   * @param proto
   *        See concept API.
   * @param new_size_but_really_must_equal_current
   *        See concept API.
   *
   * @internal
   * Resizing is not allowed for this option, so really it's either a no-op, or -- if
   * `new_size_but_really_must_equal_current != size()` -- throws exception.
   */
  template<typename Protocol>
  void resize(const Protocol& proto, size_t new_size_but_really_must_equal_current) const;
}; // class Opt_peer_process_credentials

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Task_err>
void async_write_with_native_handle(flow::log::Logger* logger_ptr,
                                    Peer_socket* peer_socket_ptr,
                                    Native_handle payload_hndl,
                                    const util::Blob_const& payload_blob,
                                    Task_err&& on_sent_or_error)
{
  using util::blob_data;
  using flow::util::buffers_dump_string;

  assert(peer_socket_ptr);
  assert((!payload_hndl.null()) && (payload_blob.size() != 0));

  auto& peer_socket = *peer_socket_ptr;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  FLOW_LOG_TRACE("Starting: Via connected local peer socket, will send handle [" << payload_hndl << "] with "
                 "blob of size [" << payload_blob.size() << "] "
                 "located @ [" << payload_blob.data() << "].");

  // Verbose and slow (100% skipped unless log filter passes).
  FLOW_LOG_DATA("Starting: Blob contents are "
                "[" << buffers_dump_string(payload_blob, "  ") << "].");

  /* OK, now our task is to asynchronously send (1) payload_hndl (native handle) and (2) payload_blob (a buffer)
   * over peer_socket (a connected local socket).  As explicitly documented in boost.asio docs: it does not provide an
   * API for the former (fairly hairy and Linux-specific) handle-transmitting feature, but it is doable by using
   * sendmsg() natively via peer_socket.native_handle() which gets the native handle (a/k/a FD).  That can only
   * be called when the socket is actually writable; meaning sendmsg() will return 1+ bytes sent; and if not writable
   * then it'd return EWOULDBLOCK/EAGAIN.  Hence we must in this case use reactor-style pattern which is a fancy way
   * way of saying we must split the typical proactor-style wait-and-write-when-writable operation into 2:
   * first wait; then once ready try to write.  boost.asio will nicely do the first part for us; but once it claims
   * peer_socket is in fact writable, then we step in with the aforementioned native sendmsg() stuff.  So do the
   * wait now (peer_socket.async_wait()).
   *
   * Subtlety: In most cases we'd try to put the body of the handler fully right in here; stylistically that's more
   * compact and arguably readable.  Instead we delegate 99.9% of the code to a helper executed from inside this
   * closure that'd normally have all the code.  Why?  Answer: The handler may need to retry this same wait; so it
   * must specify itself as a handler again.  That wouldn't work, even if syntactically achievable by saving
   * the handler itself via [capture], because async_wait() immediately returns, hence the present function does
   * too, hence anything on the stack disappears, hence whatever was [captured] would point to nothing.  If one tries
   * to do it, there's probably purely syntactically going to be a chicken-egg problem.  This could be worked
   * around via the oft-used shared_ptr<> technique... but why go crazy?  It is more straightforward to
   * have a permanent actual (helper) function and just have it refer to itself when it must.  Splitting this
   * function body into the above and a helper is perfectly fine anyway, so let's not get too clever.
   *
   * Subtlety: I have confirmed that the proper wait type is _write.  _error may sound like it detects error conditions
   * on the socket; but actually that refers to (no thanks to boost.asio docs BTW) obscure
   * stuff like MSG_OOB... nothing relevant.  An actual error like ECONNRESET would stop the _write wait and pass
   * the triggering error code (as desired). */

  peer_socket.async_wait
    (Peer_socket::wait_write,
     [logger_ptr, payload_hndl, payload_blob, peer_socket_ptr,
      on_sent_or_error = std::move(on_sent_or_error)]
       (const Error_code& sys_err_code) mutable
  {
    on_wait_writable_or_error(logger_ptr, sys_err_code, payload_hndl, payload_blob, peer_socket_ptr,
                              std::move(on_sent_or_error));
  });
} // async_write_with_native_handle()

template<typename Task_err_blob, typename Target_payload_blob_func, typename Should_interrupt_func>
void async_read_with_target_func
       (flow::log::Logger* logger_ptr, Peer_socket* peer_socket,
        Target_payload_blob_func&& target_payload_blob_func, Should_interrupt_func&& should_interrupt_func,
        Task_err_blob&& on_rcvd_or_error)
{
  using util::Blob_mutable;

  assert(peer_socket);

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Starting: Connected local peer socket wants to read 1+ bytes to some currently-unknown location "
                 "TBD when at least some data are available to read.  Will try to receive.");

  /* Without having to support should_interrupt_func feature, we could have done async_wait()->determine target
   * buffer->async_read().  We must support it, so we have so split the latter into repeated
   * async_wait()->read_some(), so we can check should_interrupt_func() ahead of each read_some().
   * Kick it off here. */

  peer_socket->async_wait
    (Peer_socket::wait_read, [logger_ptr, peer_socket,
                              target_payload_blob_func = std::move(target_payload_blob_func),
                              on_rcvd_or_error = std::move(on_rcvd_or_error),
                              should_interrupt_func = std::move(should_interrupt_func)]
                                (const Error_code& async_err_code) mutable
  {
    on_wait_readable_or_error<true> // true => See just below for meaning.
      (logger_ptr, async_err_code, peer_socket, std::move(should_interrupt_func), std::move(on_rcvd_or_error),
       std::move(target_payload_blob_func), // true => Use this to determine target blob.  false would mean ignore it.
       Blob_mutable(), // true => Ignore this.  But it will async-invoke itself with <false> next time.
       0); // Ditto.
  });
} // async_read_with_target_func()

template<typename Task_err, typename Should_interrupt_func>
void async_read_interruptible
       (flow::log::Logger* logger_ptr, Peer_socket* peer_socket, util::Blob_mutable target_payload_blob,
        Should_interrupt_func&& should_interrupt_func, Task_err&& on_rcvd_or_error)
{
  assert(peer_socket);

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Starting: Connected local peer socket wants to read 1+ bytes to already-known location "
                 "TBD when at least some data are available to read.  Will try to receive.");

  auto on_rcvd_or_error_expected_form_func
    = [on_rcvd_or_error = std::move(on_rcvd_or_error)]
        (const Error_code& err_code, auto)
  {
    // Just ignore the blob location (2nd arg).  It's known from the start anyway.
    on_rcvd_or_error(err_code);
  };

  peer_socket->async_wait
    (Peer_socket::wait_read, [logger_ptr, peer_socket,
                              target_payload_blob,
                              on_rcvd_or_error_expected_form_func = std::move(on_rcvd_or_error_expected_form_func),
                              should_interrupt_func = std::move(should_interrupt_func)]
                                (const Error_code& async_err_code) mutable
  {
    on_wait_readable_or_error<false> // false => See just below for meaning.
      (logger_ptr, async_err_code, peer_socket, std::move(should_interrupt_func),
       std::move(on_rcvd_or_error_expected_form_func),
       0, // false => Ignore this.
       target_payload_blob, // false => Just read into this buffer.
       0); // No bytes read yet.
  });
} // async_read_interruptible()

// Opt_peer_process_credentials template implementations.

template<typename Protocol>
int Opt_peer_process_credentials::level(const Protocol&) const
{
  return AF_LOCAL;
}

template<typename Protocol>
int Opt_peer_process_credentials::name(const Protocol&) const
{
  return SO_PEERCRED;
}

template<typename Protocol>
void* Opt_peer_process_credentials::data(const Protocol&)
{
  return static_cast<void*>(static_cast<util::Process_credentials*>(this));
}

template<typename Protocol>
size_t Opt_peer_process_credentials::size(const Protocol&) const
{
  return sizeof(util::Process_credentials);
}

template<typename Protocol>
void Opt_peer_process_credentials::resize(const Protocol& proto, size_t new_size_but_really_must_equal_current) const
{
  using flow::util::ostream_op_string;
  using std::length_error;

  if (new_size_but_really_must_equal_current != size(proto))
  {
    throw length_error(ostream_op_string
                         ("Opt_peer_process_credentials does not actually support resizing; requested size [",
                          new_size_but_really_must_equal_current, "] differs from forever-size [",
                          size(proto), "].  boost.asio internal bug or misuse?"));
  }
}

} // namespace ipc::transport::asio_local_stream_socket
