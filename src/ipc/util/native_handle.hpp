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

#include <ostream>
#include <flow/common.hpp>

namespace ipc::util
{

#ifndef FLOW_OS_LINUX
static_assert(false, "Flow-IPC supports transmitting native handles in Linux only; a core feature.  "
                       "Build in Linux only.");
#endif
// From this point on (in #including .cpp files as well) POSIX is assumed; and in a few spots specifically Linux.

// Types.

/* Note: We lack a native_handle_fwd.hpp, as it's such a thin wrapper around an FD (int) that there's no point
 * forward-declaring it, as it's ~always referred to by value. */

/**
 * A monolayer-thin wrapper around a native handle, a/k/a descriptor a/k/a FD.
 * It would have been acceptable to simply use an alias
 * to the native handle type (in POSIX, `int`), but this way is better stylistically with exactly zero performance
 * overhead.  Initialize it, e.g., `Native_handle hndl(some_fd);`, where
 * `some_fd` might be some POSIX-y FD (network socket, Unix domain socket, file descriptor, etc.).
 * Or initialize via no-arg construction which results in an `null() == true` object.  Copy construction, assignment,
 * equality, total ordering, and hashing all work as one would expect and essentially the same as on the underlying
 * native handles.
 *
 * It is either null() or stores a native handle.  In the former case, null() being `true` means explicitly
 * that `m_native_handle == Native_handle::S_NULL_HANDLE`.
 *
 * ### Rationale ###
 * Originally we tried to make it aggregate-initializable, like `Native_handle hndl = {some_fd}`; for example see
 * standard `array<>`.  This is reassuring perf-wise; and it involves less (i.e., no) code to boot.
 * However, it really is just one light-weight scalar, and there's 0% chance explicit construction is any slower.
 * So, perf isn't an issue.  Still, why not make it aggregate-initializable though?  I (ygoldfel) gave up, eventually,
 * because I wanted *some* non-default-behavior constructors to be available, namely no-args (`Native_handle()` that
 * makes an `null() == true` one) and move (`Native_handle(Native_handle&&)` that nullifies the source object).
 * This isn't allowed in aggregate-initializable classes; so then I made some `static` "constructors" instead; but
 * those aren't sufficient for containers... and on it went.  This way, though verbose, just works out better for the
 * API.
 */
struct Native_handle
{
  // Types.

  /// The native handle type.  Much logic relies on this type being light-weight (fast to copy).
  using handle_t = int;

  // Constants.

  /**
   * The value for #m_native_handle such that `null() == true`; else it is `false`.
   * No valid handle ever equals this.
   */
  static const handle_t S_NULL_HANDLE;

  // Data.

  /**
   * The native handle (possibly equal to #S_NULL_HANDLE), the exact payload of this Native_handle.
   * It can, of course, be assigned and accessed explicitly.  The best way to initialize a
   * new Native_handle is, therefore: `Native_handle{some_native_handle}`.  You may also use the
   * null() "constructor" to create a null() handle.
   */
  handle_t m_native_handle;

  // Constructors/destructor.

  /**
   * Constructs with given payload; also subsumes no-args construction to mean constructing an object with
   * `null() == true`.
   *
   * @param native_handle
   *        Payload.
   */
  Native_handle(handle_t native_handle = S_NULL_HANDLE);

  /**
   * Constructs object equal to `src`, while making `src.null() == true`.
   *
   * ### Rationale ###
   * This is move construction, but it's not about performance at all (as this is all quite cheap anyway);
   * more to allow for the pattern of making an object from another object without propagating more copies of the
   * underlying handle.  Wouldn't the auto-generated move ctor take care of it?  No, because it'd just copy the handle
   * and not nullify the source.
   *
   * @param src
   *        Source object which will be made `null() == true`.
   */
  Native_handle(Native_handle&& src);

  /**
   * Copy constructor.
   * @param src
   *        Source object.
   */
  Native_handle(const Native_handle& src);

  // Methods.

  /**
   * Move assignment; acts similarly to move ctor; but no-op if `this == &src`.
   * @param src
   *        Source object which will be made `.null() == true`, unless `this == &src`.
   * @return `*this`.
   */
  Native_handle& operator=(Native_handle&& src);

  /**
   * Copy assignment; acts similarly to copy ctor.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Native_handle& operator=(const Native_handle& src);

  /**
   * Returns `true` if and only if #m_native_handle equals #S_NULL_HANDLE.
   * @return See above.
   */
  bool null() const;

  /**
   * Little utility that returns #m_native_handle to the OS.
   * This is helpful to close, without invoking a native API (`close()` really), a value returned by
   * `transport::asio_local_stream_socket::Peer_socket::release()` or, perhaps, received over a
   * `transport::Native_socket_stream`.
   *
   * `*this` is nullified (null() shall return `true`).  No-op if already so at entry.
   *
   * Nothing is logged; no errors are emitted.  This is intended for no-questions-asked cleanup.
   *
   * @note The Native_handle destructor, such as it is, absolutely does not release() or anything similar.
   *       Similarly move ctor/assignment does not either.  In fact nothing else in Native_handle does.
   *       If you want to do it, you must call release() yourself.  It is a utility; and Native_handle as a whole
   *       intentionaly features minimal intelligence; it is merely an object wrapper around a raw handle.
   *       Movt ctor/assignment nullifying the source object is as "intelligent" as we get.
   */
  void release();
}; // struct Native_handle

// Free functions.

/**
 * Returns `true` if and only if the two Native_handle objects are the same underlying handle.
 *
 * @relatesalso Native_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 * @return See above.
 */
bool operator==(Native_handle val1, Native_handle val2);

/**
 * Negation of similar `==`.
 *
 * @relatesalso Native_handle
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 * @return See above.
 */
bool operator!=(Native_handle val1, Native_handle val2);

/**
 * Returns a less-than comparison of two Native_handle objects, with the usual total ordering guarantees.
 *
 * @relatesalso Native_handle
 *
 * @param val1
 *        Left-hand side object.
 * @param val2
 *        Right-hand side object.
 * @return Whether left side is considered strictly less-than right side.
 */
bool operator<(Native_handle val1, Native_handle val2);

/**
 * Hasher of Native_handle for boost.unordered et al.
 *
 * @relatesalso Native_handle
 *
 * @param val
 *        Object to hash.
 * @return See above.
 */
size_t hash_value(Native_handle val);

/**
 * Standard swap.  (Proper pattern is `using std::swap;` and then `swap()` in any using function.)
 *
 * ### Rationale for existence ###
 * `std::swap()` would perform 3 move-assigns; Native_handle move-assignment/construction involves nullifying
 * the source Native_handle::m_native_handle; that's not necessary in a swap and wastes cycles.
 *
 * @param val1
 *        Object to swap.
 * @param val2
 *        Object to swap.
 */
void swap(Native_handle& val1, Native_handle& val2);

/**
 * Prints string representation of the given Native_handle to the given `ostream`.
 *
 * @relatesalso Native_handle
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Native_handle& val);

} // namespace ipc::util
