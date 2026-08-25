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

#include <flow/common.hpp>
#include <boost/scope/unique_resource.hpp>
#include <boost/core/functor.hpp>
#include <ostream>

namespace ipc::util
{

#ifndef FLOW_OS_LINUX
static_assert(false, "Flow-IPC supports transmitting native handles in Linux only; a core feature.  "
                       "Build in Linux only.");
#endif
// From this point on (in #including .cpp files as well) POSIX is assumed; and in a few spots specifically Linux.

/* Note: We lack a native_handle_fwd.hpp, as it's such a thin wrapper around an FD (int) that there's no point
 * forward-declaring it, as it's ~always referred to by value, and almost everything is `constexpr`. */

// Types.

/**
 * A monolayer-thin wrapper around a native handle, a/k/a descriptor a/k/a FD.
 * It would have been acceptable to simply use an alias to the native handle type (in POSIX, `int`),
 * but this way is better stylistically with exactly zero performance overhead.  Initialize it, e.g.,
 * `Native_handle hndl{some_fd};`, where `some_fd` might be some POSIX-y FD (network socket, Unix domain socket,
 * file descriptor, etc.).  Or initialize via no-arg construction which results in an `null() == true` object.
 * Copy construction, assignment, equality, total ordering, and hashing all work as one would expect and
 * essentially the same as on the underlying native handles.
 *
 * It is either null() or stores a native handle.  In the former case, null() being `true` means explicitly
 * that `m_native_handle == Native_handle::S_NULL_HANDLE`.
 *
 * ### Ownership ###
 * Native_handle is extremely simple.  Other than providing move semantics, where the moved-from guy becomes `.null()`,
 * and an always-manual close() API, it does not add ownership semantics.  You're free to, e.g., assign a non-null
 * socket value, then make a copy, then (e.g.) `.close()` them both -- the second one being an erroneous
 * double-close.  However, if you need added cleverness to avoid such things:
 *
 * @see Helper type #Own_native_handle: It adds auto-`close()` behavior, on destruction and being moved-to.
 *      (To get at the Native_handle itself, given an `Own_native_handle x`: Call `x.get()`.  The full API
 *      is similar to `unique_ptr`'s basic API: `.reset()`, `.release()`, and so on.)
 *      This can be quite useful when one is juggling `Native_handle`s in some operation and, e.g., does not
 *      want to leak the OS-handles (descriptors) on early `return` due to error conditions.  If ultimately
 *      a raw `Native_handle` is required, one can always get it out via `.release()`.
 */
struct Native_handle
{
  // A method (odd placement b/c needed for Closer below).

  /**
   * Simply calls `hndl.close()`; helpful for things like `boost::core::functor<static_close>`.
   *
   * @param hndl
   *        See above.  It can be `.null()`.
   */
  static void static_close(Native_handle& hndl) noexcept;

  // Types.

  /// The native handle type.  Much logic relies on this type being light-weight (fast to copy).
  using handle_t = int;

  /**
   * Function object type (storing no data), suitable for `unique_resource`, whose `(Native_handle& hndl)` operator
   * invokes Native_handle::close() on `hndl`.
   */
  using Closer = boost::core::functor<static_close>;

  // Constants.

  /**
   * The value for #m_native_handle such that `null() == true`; else it is `false`.
   * No valid handle ever equals this.
   */
  static constexpr handle_t S_NULL_HANDLE = -1;

  // Data.

  /**
   * The native handle (possibly equal to #S_NULL_HANDLE), the exact payload of this Native_handle.
   * It can, of course, be assigned and accessed explicitly.
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
  constexpr Native_handle(handle_t native_handle = S_NULL_HANDLE) noexcept;

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
  constexpr Native_handle(Native_handle&& src) noexcept;

  /**
   * Copy constructor.
   * @param src
   *        Source object.
   */
  constexpr Native_handle(const Native_handle& src) noexcept;

  // Methods.

  /**
   * Move assignment; acts similarly to move ctor; but no-op if `this == &src`.
   * @param src
   *        Source object which will be made `.null() == true`, unless `this == &src`.
   * @return `*this`.
   */
  constexpr Native_handle& operator=(Native_handle&& src) noexcept;

  /**
   * Copy assignment; acts similarly to copy ctor.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  constexpr Native_handle& operator=(const Native_handle& src) noexcept;

  /**
   * Returns `true` if and only if #m_native_handle equals #S_NULL_HANDLE.
   * @return See above.
   */
  constexpr bool null() const noexcept;

  /**
   * Little utility that returns #m_native_handle to the OS.
   *
   * This is helpful to close, without invoking a native API (`"::close()"` really), a value returned by
   * `transport::asio_local_stream_socket::Peer_socket::release()` or, perhaps, received over a
   * `transport::Native_socket_stream`.
   *
   * `*this` is nullified (null() shall return `true`).  No-op if already so at entry.
   *
   * Nothing is logged; no errors are emitted.  This is intended for no-questions-asked cleanup.
   *
   * @note The Native_handle destructor, such as it is, absolutely does not close() or anything similar.
   *       Similarly move ctor/assignment does not either.  In fact nothing else in Native_handle does.
   *       If you want to do it, you must call close() yourself.  It is a utility; and Native_handle as a whole
   *       intentionally features minimal intelligence; it is merely an object wrapper around a raw handle.
   *       Move ctor/assignment nullifying the source object is as "intelligent" as we get.
   *
   * @see However: #Own_native_handle adds auto-`close()` behavior, on destruction and being moved-to.
   *      This can be quite useful when one is juggling `Native_handle`s and, e.g., does not want to leak
   *      the OS-handle (descriptor) on early `return` due to error conditions.
   */
  void close() noexcept;

  /**
   * Simply returns `Native_handle{}`, a null handle.
   *
   * ### Rationale for existence ###
   * It is so that Native_handle can be used as its own traits class in `unique_resource<Native_handle>`.
   * You probably need not worry about it: just use #Own_native_handle, when you need those capabilities.
   *
   * @return See above.
   */
  static constexpr Native_handle make_default() noexcept;

  /**
   * Simply returns `!hndl.null()`.  Rationale for existence: same as make_default().
   * @param hndl
   *        See above.
   * @return See above.
   */
  static constexpr bool is_allocated(Native_handle hndl) noexcept;
}; // struct Native_handle

/**
 * A `unique_resource` wrapper for Native_handle useful to avoid descriptor leaks and double-closing; can be thought
 * of as a `unique_ptr<Native_handle>` except involves no heap allocation, and nullness is determined by
 * Native_handle::null().
 *
 * For a bit more background on rationale and usage: see Native_handle doc header Ownership section.
 *
 * Reminder: if/when ready to get a raw, non-auto-closing Native_handle back: use `.release()`.
 *
 * @internal
 *
 * ### Impl notes ###
 * `unique_resource` is almost perfect for what we want here.  Just a couple subtleties:
 *   - `boost.scope` itself threw in a `unique_fd` which, in POSIX-land at least, is very similar or identical
 *     to `Own_native_handle`; it uses `unique_resource` too, naturally, but around `int` a/k/a Native_handle::handle_t
 *     as opposed to some wrapper like our `Native_handle`.  We contemplated reusing it anyway, for that
 *     particular purpose, but decided that the Native_handle abstraction works for us, so we'd rather re-wrap
 *     it ourselves.  As you can see it is not hard: one line here plus the traits functions
 *     Native_handle::make_default() and Native_handle::is_allocated().
 *   - The resulting thing, due to our use of traits, is quite tight perf-wise: Its size is that of `handle_t` --
 *     an `int` in POSIX at least -- and, combining `constexpr`ness and `unique_resource`'s design, all the
 *     basic operations (assign, compare, construct...) should be trivially fast.
 *     - There is one *little* asterisk on that front: Given a *non-null object*: the close operation (through
 *       `.reset()`, dtor, being assigned-onto or -from) will check for `m_native_handle == S_NULL_HANDLE`
 *       twice: 1, `unique_resource` through Native_handle::is_allocated() (traits); 2,
 *       in Native_handle::close() again.  It'll be `false` both times, so then `"::close(m_native_handle)"`
 *       will occur.  The 2nd equality check is redundant.  (If it's a null object, then the initial check
 *       returns `true` => GTFO; no problem there.)
 *       - The reason: `.close()` is defensive, as it should be; and Native_handle::Closer inherits that -- why
 *         shouldn't it, in and of itself? -- and then it is convenient to use `Closer` for
 *         our `unique_resource` setup.  Could we "defeat" it by using a, like, `Closer_unchecked` as the deleter?
 *         Yes; but pragmatically speaking that's over-engineering.  A redundant `int` compare -- probably optimized
 *         away at some point anyway -- only when the close OS-call will be invoked?  Not a real problem.
 */
using Own_native_handle = boost::scope::unique_resource<Native_handle, Native_handle::Closer, Native_handle>;

// Free functions.

/**
 * Returns Native_handle constructed by relieving an existing auto-closing #Own_native_handle of its ownership of a
 * particular raw handle and storing it, sans auto-closing protection, in that Native_handle.  That is it is equal to:
 *
 *   ~~~
 *   // src is Own_native_handle.
 *   Native_handle ret{src.get()};
 *   src.release();
 *   return ret;
 *   ~~~
 *
 * @relatesalso Native_handle
 *
 * @param src
 *        Source object; post-condition is `src.get().null() == true`.
 * @return See above.
 */
Native_handle disowned_native_handle(Own_native_handle&& src) noexcept;

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
constexpr bool operator==(Native_handle val1, Native_handle val2) noexcept;

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
constexpr bool operator!=(Native_handle val1, Native_handle val2) noexcept;

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
constexpr bool operator<(Native_handle val1, Native_handle val2) noexcept;

/**
 * Hasher of Native_handle for boost.unordered et al.
 *
 * @relatesalso Native_handle
 *
 * @param val
 *        Object to hash.
 * @return See above.
 */
size_t hash_value(Native_handle val) noexcept;

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
constexpr void swap(Native_handle& val1, Native_handle& val2) noexcept;

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

// constexpr implementations.

constexpr Native_handle::Native_handle(handle_t native_handle) noexcept :
  m_native_handle(native_handle)
{
  // Nope.
}

constexpr Native_handle::Native_handle(Native_handle&& src) noexcept :
  Native_handle(src)
{
  // It's why we exist: prevent duplication of src.m_native_handle.
  src.m_native_handle = S_NULL_HANDLE;
}

constexpr Native_handle::Native_handle(const Native_handle&) noexcept = default;

constexpr Native_handle& Native_handle::operator=(Native_handle&& src) noexcept
{
  if (this != &src)
  {
    m_native_handle = src.m_native_handle;
    src.m_native_handle = S_NULL_HANDLE;
  }
  return *this;
}

constexpr Native_handle& Native_handle::operator=(const Native_handle&) noexcept = default;

constexpr bool Native_handle::null() const noexcept
{
  return m_native_handle == S_NULL_HANDLE;
}

constexpr Native_handle Native_handle::make_default() noexcept // Static.
{
  return {};
}

constexpr bool Native_handle::is_allocated(Native_handle hndl) noexcept // Static.
{
  return !hndl.null();
}

constexpr bool operator==(Native_handle val1, Native_handle val2) noexcept
{
  return val1.m_native_handle == val2.m_native_handle;
}

constexpr bool operator!=(Native_handle val1, Native_handle val2) noexcept
{
  return !operator==(val1, val2);
}

constexpr void swap(Native_handle& val1, Native_handle& val2) noexcept
{
  // (See `Rationale for existence` in our doc header.)

  // Looks like this std::swap() is not constexpr (C++17), so let's just:
  const auto tmp = val1.m_native_handle;
  val1.m_native_handle = val2.m_native_handle;
  val2.m_native_handle = tmp;
}

constexpr bool operator<(Native_handle val1, Native_handle val2) noexcept
{
  /* In POSIX and Windows this is valid.  So don't worry about #ifdef'ing for OS, static_assert(false), etc.
   * I did check that even in Windows they are formally integers still.  Update: Actually... not sure... depends
   * on what's a Native_handle; if it's SOCKET then sure, but otherwise -- who knows?  There's probably a ticket
   * about that whole topic. */
  return val1.m_native_handle < val2.m_native_handle;
}

} // namespace ipc::util
