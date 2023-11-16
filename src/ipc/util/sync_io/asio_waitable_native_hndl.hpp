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

#include "ipc/util/native_handle.hpp"
#include <boost/asio.hpp>

namespace ipc::util::sync_io
{

// Types.

/**
 * Useful if using the `sync_io` pattern within a user event loop built on boost.asio (optionally with flow.async
 * help), an object of this class wraps a non-null Native_handle and allows one to use `.async_wait()` to perform
 * event waiting on behalf of any `sync_io`-implementing ipc::transport or ipc::session object.  If boost.asio
 * integration is not the goal, it can at least act as a mere container of a Native_handle.  `sync_io` pattern
 * uses Asio_waitable_native_handle in either capacity.  If you're familiar with boost.asio POSIX `descriptor`,
 * then you're familiar with this class.
 *
 * @see sync_io::Event_wait_func doc header; and specifically section "Integrating with boost.asio."
 *      It provides context and rationale for this guy when used for `.async_wait()`.
 *
 * It may also be useful in its own right independently.
 *
 * In any case use is quite straightforward:
 *   -# Construct from a non-null, opened w/r/t kernel Native_handle --
 *      and an executor/execution context (typically a `Task_engine` or possibly boost.asio `strand`).
 *      (If boost.asio integration is not the goal, then any blank/otherwise-unused `Task_engine` is fine.)
 *      -# (Optional) If the wrapped handle is unknown at construction time, use the 1-arg ctor form;
 *         then `.assign()` it when it is known.
 *   -# Invoke `.async_wait()` (from Asio_waitable_native_handle::Base a/k/a `boost::asio::posix::descriptor`) as
 *      many times as desired.
 *      - And/or invoke accessor `.native_handle()` to re-obtain the wrapped handle as desired.
 *   -# Destroy when no more `.async_wait()`ing or `.native_handle()` access is necessary.
 *      -# `.release()` may be used to essentially nullify a `*this`.  This may be desirable for cleanliness
 *         in some situations.
 *
 * You may not use `*this` object directly to perform actual I/O; it lacks such methods as `async_read_some()`.
 * Moreover it is likely against the (informal) aim of Asio_waitable_native_handle to perform actual I/O
 * by obtaining the handle via `.native_handle()` and then doing reads/writes on that handle.
 *
 * Construction is likely performed by internal Flow-IPC code only in practice, though you are free to use
 * it for your own purposes.
 *
 * @warning This object, like Asio_waitable_native_handle::Base, isn't *just* a Native_handle wrapper:
 *          It also has to be associated with an execution context, usually `Task_engine` (`boost::asio::io_context`)
 *          or boost.asio strand.  If you associate, via ctor or assign(), 2 `*this`es each wrapping
 *          the same-valued Native_handle (storing the same raw handle) with the same `Task_engine`,
 *          behavior is undefined formally; though internally it'll cause a duplicate-descriptor error/exception
 *          (in Linux from `errno == EEXIST` from an internal `epoll_ctl()`).  Even if you don't plan to
 *          `async_wait()`, this will unfortunately still happen; and for `async_wait()` purposes it will
 *          and *should* happen.  So just take care not to create duplicate-handle `*this`es associated with
 *          the same `Task_engine`.
 * @note Move assignment works which can be useful to associate a `*this` with a different execution context
 *       (event loop, `Task_engine`, etc.).
 *
 * ### Rationale for `protected` rather than `public` inheritance ###
 * `public` inheritance would have been fine, more or less, but it seemed prudent to close off access to mutators
 * from Asio_waitable_native_handle::Base like `close()` and `non_blocking()` which could sow sheer chaos;
 * the point here is to watch for events on this guy, not (e.g.) blow up some internal transport within
 * transport::Native_socket_stream et al.
 */
class Asio_waitable_native_handle : protected boost::asio::posix::descriptor
{
public:
  // Types.

  /// Short-hand for our base type; can be used for, e.g., `Base::wait_write` and `Base::wait_read`.
  using Base = boost::asio::posix::descriptor;

  // Constructors/destructor.

  /**
   * Construct boost.asio descriptor object such that one can `async_wait()` events on the given non-`.null()`
   * Native_handle.  The handle must be a valid (not closed) handle (descriptor, FD).
   *
   * Consider also the 1-arg ctor variant which stores no handle; followed by assign().
   *
   * @param ex
   *        See boost.asio docs for similar `posix::basic_descriptor` ctor.
   * @param hndl
   *        The handle that might be watched subsequently.  `.null()` leads to undefined behavior;
   *        assertion may trip.  If the associated `Task_engine` is already associated with another
   *        Asio_waitable_native_handle (or other I/O object) that stores the same Native_handle::m_native_handle,
   *        it is formally undefined behavior (in reality a boost.asio exception is thrown; but just don't do it).
   */
  explicit Asio_waitable_native_handle(const Base::executor_type& ex, Native_handle hndl);

  /**
   * Construct boost.asio descriptor object that stores no handle.  You may call assign() to change this.
   *
   * @param ex
   *        See boost.asio docs for similar `posix::basic_descriptor` ctor.
   */
  explicit Asio_waitable_native_handle(const Base::executor_type& ex);

  /**
   * Construct boost.asio descriptor object such that one can `async_wait()` events on the given non-`.null()`
   * Native_handle.  The handle must be a valid (not closed) handle (descriptor, FD).
   *
   * Consider also the 1-arg ctor variant which stores no handle; followed by assign().
   *
   * @tparam Execution_context
   *         See boost.asio docs for similar `posix::basic_descriptor` ctor.
   * @param context
   *        See boost.asio docs for similar `posix::basic_descriptor` ctor.
   * @param hndl
   *        The handle that might be watched subsequently.  `.null()` leads to undefined behavior;
   *        assertion may trip.  If the associated `Task_engine` is already associated with another
   *        Asio_waitable_native_handle that stores the same Native_handle::m_native_handle, it is formally
   *        undefined behavior (in reality a boost.asio exception is thrown; but just don't do it).
   * @param ignored
   *        Disregard.
   */
  template<typename Execution_context>
  explicit Asio_waitable_native_handle(Execution_context& context, Native_handle hndl,
                                        typename std::enable_if_t<std::is_convertible_v
                                                                    <Execution_context&,
                                                                     boost::asio::execution_context&>>*
                                          ignored = nullptr);

  /**
   * Construct boost.asio descriptor object that stores no handle.  You may call assign() to change this.
   *
   * @tparam Execution_context
   *         See boost.asio docs for similar `posix::basic_descriptor` ctor.
   * @param context
   *        See boost.asio docs for similar `posix::basic_descriptor` ctor.
   * @param ignored
   *        Disregard.
   */
  template<typename Execution_context>
  explicit Asio_waitable_native_handle(Execution_context& context,
                                       typename std::enable_if_t<std::is_convertible_v
                                                                   <Execution_context&,
                                                                    boost::asio::execution_context&>>*
                                          ignored = nullptr);

  /**
   * Move-construct.
   *
   * @param src
   *        See boost.asio docs for similar `posix::basic_descriptor` ctor.
   */
  Asio_waitable_native_handle(Asio_waitable_native_handle&& src);

  /**
   * Destructor that does *not* OS-close (`"::close()"`) the stored native handle (if any) but rather
   * performs `this->release()` (eating any potential error in doing so).
   */
  ~Asio_waitable_native_handle();

  // Methods.

  /**
   * Move-assign.
   *
   * @param src
   *        See boost.asio docs for similar `posix::basic_descriptor` method.
   * @return See boost.asio docs for similar `posix::basic_descriptor` method.
   */
  Asio_waitable_native_handle& operator=(Asio_waitable_native_handle&& src);

  /**
   * Returns the same Native_handle as passed to original handle-loading ctor or assign(), whichever happened last;
   * or `.null()` Native_handle, if neither has been invoked, or release() was invoked more recently than either.
   *
   * @return See above.
   */
  Native_handle native_handle();

  /**
   * Loads value to be returned by native_handle().  As with the 2+-arg ctor(s) the handle must be valid
   * (not closed).  See also `.release()` and dtor.
   *
   * If a handle is already loaded (`Base::is_open() == true`), this method automatically release()s it first.
   *
   * @param hndl
   *        The handle that might be watched subsequently.  `.null()` leads to undefined behavior;
   *        assertion may trip.  If the associated `Task_engine` is already associated with another
   *        Asio_waitable_native_handle that stores the same Native_handle::m_native_handle, it is formally
   *        undefined behavior (in reality a boost.asio exception is thrown; but just don't do it).
   */
  void assign(Native_handle hndl);

  /// Importantly, inherit and publicly expose `posix::basic_descriptor::async_wait()`.  See its boost.asio docs.
  using Base::async_wait;

  /// Inherit and publicly expose `posix::basic_descriptor::release()`.  See its boost.asio docs.
  using Base::release;

  /// Inherit and publicly expose `posix::basic_descriptor::is_open()`.  See its boost.asio docs.
  using Base::is_open;
}; // class Asio_waitable_native_handle

// Template implementations.

template<typename Execution_context>
Asio_waitable_native_handle::Asio_waitable_native_handle
  (Execution_context& context, Native_handle hndl,
   typename std::enable_if_t<std::is_convertible_v<Execution_context&,
                                                   boost::asio::execution_context&>>*) :
  Base(context, hndl.m_native_handle)
{
  // See comment in similar ctor in .cpp.
  assert(!hndl.null());
}

template<typename Execution_context>
Asio_waitable_native_handle::Asio_waitable_native_handle
  (Execution_context& context,
   typename std::enable_if_t<std::is_convertible_v<Execution_context&,
                                                   boost::asio::execution_context&>>*) :
  Base(context)
{
  // Yeah.
}

} // namespace ipc::util::sync_io
