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

#include "ipc/util/native_handle.hpp"
#include <flow/common.hpp>
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace ipc::util::test
{

namespace
{

// Obtain a real, open FD (of /dev/null); we shall check whether the various APIs close it when they should.
Native_handle make_fd()
{
  const int fd = ::open("/dev/null", O_RDONLY);
  EXPECT_NE(fd, -1) << "Could not open /dev/null; nothing else can be sanely tested.";
  return Native_handle{fd};
}

// Return whether `hndl` stores an open descriptor (as opposed to a closed or never-opened value).
bool fd_is_open(Native_handle hndl)
{
#ifndef FLOW_OS_LINUX
static_assert(false,
              "Not tested in non-LINUX; revisit when porting this to other OS.");
#endif

  return ::fcntl(hndl.m_native_handle, F_GETFD) != -1;
}

} // namespace (anon)

// Basic observers; and the constexpr-ness of ~everything (checked at compile time: failures = does not compile).
TEST(Native_handle_test, basics)
{
  static_assert(Native_handle{}.null(), "Default-cted Native_handle must be null.");
  static_assert(Native_handle{}.m_native_handle == Native_handle::S_NULL_HANDLE, "Null <=> the sentinel value.");
  static_assert(!Native_handle{5}.null(), "Payload-cted Native_handle must not be null.");
  static_assert(Native_handle{5} == Native_handle{5}, "Equality must compare payloads.");
  static_assert(Native_handle{4} != Native_handle{5}, "Inequality must compare payloads.");
  static_assert(Native_handle{4} < Native_handle{5}, "Ordering must compare payloads.");
  constexpr bool move_semantics_and_swap_ok
    = []() constexpr
        {
          Native_handle a{5};
          Native_handle b{std::move(a)}; // Move ctor: b gets payload; a nullified.
          Native_handle c;
          c = std::move(b); // Move assignment: ditto.
          Native_handle d{7};
          swap(c, d); // c <-> d.
          return a.null() && b.null() && (c == Native_handle{7}) && (d == Native_handle{5});
        }();
  static_assert(move_semantics_and_swap_ok, "Move semantics and swap() must work in constexpr evaluation.");

  const Native_handle null_hndl;
  EXPECT_TRUE(null_hndl.null());
  EXPECT_EQ(null_hndl, Native_handle{});
  EXPECT_EQ(hash_value(Native_handle{5}), hash_value(Native_handle{5}));
}

TEST(Native_handle_test, move_and_copy_semantics)
{
  Native_handle src{5};

  // Copy (ctor and assignment) must *not* nullify the source.
  const Native_handle copy{src};
  EXPECT_EQ(copy, src);
  EXPECT_FALSE(src.null());

  // Move ctor must transfer payload and nullify the source.
  Native_handle dst{std::move(src)};
  EXPECT_TRUE(src.null());
  EXPECT_EQ(dst, Native_handle{5});

  // Move assignment: ditto.
  Native_handle dst2;
  dst2 = std::move(dst);
  EXPECT_TRUE(dst.null());
  EXPECT_EQ(dst2, Native_handle{5});

  // Self-move-assignment is a documented no-op.  (Launder through a reference to avoid compiler warnings.)
  auto& dst2_alias = dst2;
  dst2 = std::move(dst2_alias);
  EXPECT_EQ(dst2, Native_handle{5});
}

TEST(Native_handle_test, no_implicit_close)
{
  const auto hndl = make_fd();

  /* Native_handle is docced to be ownership-free: dtor, copies, moves -- none of them shall close.
   * Exercise all three on a real descriptor; then verify it is still open. */
  {
    auto copy = hndl;
    const Native_handle moved{std::move(copy)};
    EXPECT_TRUE(copy.null());
    EXPECT_EQ(moved, hndl);
    // moved dies; copy dies (as-if never opened at this point due to the move).
  }
  EXPECT_TRUE(fd_is_open(hndl));

  // Only explicit close() closes; and it nullifies.
  auto doomed = hndl; // (Keep hndl itself as the record of the raw FD value.)
  doomed.close();
  EXPECT_TRUE(doomed.null());
  EXPECT_FALSE(fd_is_open(hndl));

  // close() of null: promised no-op.
  doomed.close();
  EXPECT_TRUE(doomed.null());
}

TEST(Native_handle_test, own_native_handle)
{
  // Dtor must close.
  auto hndl = make_fd();
  {
    const Own_native_handle own{hndl};
    EXPECT_EQ(own.get(), hndl);
    EXPECT_TRUE(fd_is_open(hndl));
  }
  EXPECT_FALSE(fd_is_open(hndl));

  // Default-cted guy stores null and (implicitly: no crash) closes nothing.
  {
    const Own_native_handle own;
    EXPECT_TRUE(own.get().null());
  }

  // Move ctor must transfer ownership (in particular: no double-close; source left inactive/null).
  hndl = make_fd();
  {
    Own_native_handle own_src{hndl};
    const Own_native_handle own_dst{std::move(own_src)};
    EXPECT_TRUE(own_src.get().null());
    EXPECT_EQ(own_dst.get(), hndl);
    EXPECT_TRUE(fd_is_open(hndl));
  }
  EXPECT_FALSE(fd_is_open(hndl));

  // Move-assignment-onto must close the target's previously-owned resource.
  hndl = make_fd();
  const auto hndl2 = make_fd();
  {
    Own_native_handle own_dst{hndl};
    Own_native_handle own_src{hndl2};
    own_dst = std::move(own_src);
    EXPECT_FALSE(fd_is_open(hndl)); // Old resource closed by the assignment.
    EXPECT_TRUE(fd_is_open(hndl2)); // New resource alive and well...
    EXPECT_EQ(own_dst.get(), hndl2); // ...owned here...
    EXPECT_TRUE(own_src.get().null()); // ...and only here.
  }
  EXPECT_FALSE(fd_is_open(hndl2));

  // reset(<new resource>) must close the old and adopt the new.
  hndl = make_fd();
  const auto hndl3 = make_fd();
  {
    Own_native_handle own{hndl};
    own.reset(Native_handle{hndl3});
    EXPECT_FALSE(fd_is_open(hndl));
    EXPECT_TRUE(fd_is_open(hndl3));
  }
  EXPECT_FALSE(fd_is_open(hndl3));

  // release() must disable the auto-close and (per resource-traits) leave a null stored value behind.
  hndl = make_fd();
  {
    Own_native_handle own{hndl};
    own.release();
    EXPECT_TRUE(own.get().null());
  }
  EXPECT_TRUE(fd_is_open(hndl));
  Native_handle{hndl}.close(); // Clean up after ourselves.
}

TEST(Native_handle_test, disowned_native_handle)
{
  const auto hndl = make_fd();
  Own_native_handle own{hndl};

  const auto stolen = disowned_native_handle(std::move(own));
  EXPECT_EQ(stolen, hndl);
  EXPECT_TRUE(own.get().null()); // Docced post-condition.
  EXPECT_TRUE(fd_is_open(hndl)); // Auto-close disarmed: still open...

  own.reset(); // (Explicitly, for clarity; dtor would do the same, namely nothing.)
  EXPECT_TRUE(fd_is_open(hndl)); // ...still open...

  Native_handle{stolen}.close();
  EXPECT_FALSE(fd_is_open(hndl)); // ...until closed by (as of the disowning) its sole owner: the user.  That's us.
}

} // namespace ipc::util::test
