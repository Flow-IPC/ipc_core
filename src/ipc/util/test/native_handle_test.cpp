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
#include "ipc/transport/error.hpp"
#include "ipc/common.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/resource.h>
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

// Return whether `hndl` (which must be open) has close-on-exec set.
bool fd_is_cloexec(Native_handle hndl)
{
  const int flags = ::fcntl(hndl.m_native_handle, F_GETFD);
  EXPECT_NE(flags, -1);
  return (flags & FD_CLOEXEC) != 0;
}

/* RAII: while alive, RLIMIT_NOFILE soft limit = `limit` (descriptor numbers must stay below it).
 * Note: a limit of 0 is useless for forcing EMFILE: F_DUPFD's minimum-number arg (0) would then be at/above the
 * limit, which is EINVAL by definition; EMFILE requires a positive limit with every number below it in use. */
struct Fd_limit_scope
{
  rlimit m_saved;

  explicit Fd_limit_scope(rlim_t limit)
  {
#ifndef FLOW_OS_LINUX
static_assert(false,
              "Not tested in non-LINUX; revisit when porting this to other OS.");
#endif
    EXPECT_EQ(::getrlimit(RLIMIT_NOFILE, &m_saved), 0);
    rlimit new_limit = m_saved;
    new_limit.rlim_cur = limit;
    EXPECT_EQ(::setrlimit(RLIMIT_NOFILE, &new_limit), 0);
  }

  ~Fd_limit_scope()
  {
    EXPECT_EQ(::setrlimit(RLIMIT_NOFILE, &m_saved), 0);
  }
};

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

TEST(Native_handle_test, is_open)
{
  EXPECT_FALSE(Native_handle{}.is_open()); // Docced: null => false.

  const auto hndl = make_fd();
  EXPECT_TRUE(hndl.is_open());
  Native_handle{hndl}.close();
  EXPECT_FALSE(hndl.is_open()); // Stale value (same number, now closed) => false.
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
  EXPECT_TRUE(hndl.is_open());

  // Only explicit close() closes; and it nullifies.
  auto doomed = hndl; // (Keep hndl itself as the record of the raw FD value.)
  doomed.close();
  EXPECT_TRUE(doomed.null());
  EXPECT_FALSE(hndl.is_open());

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
    EXPECT_TRUE(hndl.is_open());
  }
  EXPECT_FALSE(hndl.is_open());

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
    EXPECT_TRUE(hndl.is_open());
  }
  EXPECT_FALSE(hndl.is_open());

  // Move-assignment-onto must close the target's previously-owned resource.
  hndl = make_fd();
  const auto hndl2 = make_fd();
  {
    Own_native_handle own_dst{hndl};
    Own_native_handle own_src{hndl2};
    own_dst = std::move(own_src);
    EXPECT_FALSE(hndl.is_open()); // Old resource closed by the assignment.
    EXPECT_TRUE(hndl2.is_open()); // New resource alive and well...
    EXPECT_EQ(own_dst.get(), hndl2); // ...owned here...
    EXPECT_TRUE(own_src.get().null()); // ...and only here.
  }
  EXPECT_FALSE(hndl2.is_open());

  // reset(<new resource>) must close the old and adopt the new.
  hndl = make_fd();
  const auto hndl3 = make_fd();
  {
    Own_native_handle own{hndl};
    own.reset(Native_handle{hndl3});
    EXPECT_FALSE(hndl.is_open());
    EXPECT_TRUE(hndl3.is_open());
  }
  EXPECT_FALSE(hndl3.is_open());

  // release() must disable the auto-close and (per resource-traits) leave a null stored value behind.
  hndl = make_fd();
  {
    Own_native_handle own{hndl};
    own.release();
    EXPECT_TRUE(own.get().null());
  }
  EXPECT_TRUE(hndl.is_open());
  Native_handle{hndl}.close(); // Clean up after ourselves.
}

TEST(Native_handle_test, disowned_native_handle)
{
  const auto hndl = make_fd();
  Own_native_handle own{hndl};

  const auto stolen = disowned_native_handle(std::move(own));
  EXPECT_EQ(stolen, hndl);
  EXPECT_TRUE(own.get().null()); // Docced post-condition.
  EXPECT_TRUE(hndl.is_open()); // Auto-close disarmed: still open...

  own.reset(); // (Explicitly, for clarity; dtor would do the same, namely nothing.)
  EXPECT_TRUE(hndl.is_open()); // ...still open...

  Native_handle{stolen}.close();
  EXPECT_FALSE(hndl.is_open()); // ...until closed by (as of the disowning) its sole owner: the user.  That's us.
}

TEST(Native_handle_test, dup)
{
  const auto hndl = make_fd();

  Error_code err_code;
  const auto dupe = hndl.dup(&err_code);
  ASSERT_FALSE(err_code) << err_code.message();
  ASSERT_FALSE(dupe.null());
  EXPECT_NE(dupe, hndl);
  EXPECT_TRUE(dupe.is_open());
  EXPECT_TRUE(hndl.is_open()); // Original unaffected...
  EXPECT_TRUE(fd_is_cloexec(dupe)); // Docced.

  Native_handle{hndl}.close(); // ...and independent: closing the original...
  EXPECT_TRUE(dupe.is_open()); // ...leaves the duplicate open.

  Native_handle{dupe}.close();
  EXPECT_FALSE(dupe.is_open());
}

TEST(Native_handle_test, duped_native_handle)
{
  const auto hndl = make_fd();

  Native_handle dupe;
  {
    Error_code err_code;
    const auto own = duped_native_handle(hndl, &err_code);
    ASSERT_FALSE(err_code) << err_code.message();
    dupe = own.get();
    ASSERT_FALSE(dupe.null());
    EXPECT_TRUE(dupe.is_open());
  } // Auto-close of the duplicate...
  EXPECT_FALSE(dupe.is_open());
  EXPECT_TRUE(hndl.is_open()); // ...not of the original.

  Native_handle{hndl}.close();
}

TEST(Native_handle_test, dup_errors)
{
  using flow::error::Runtime_error;
  using boost::system::errc::too_many_files_open;

  // Null input: invalid-argument, in both error-reporting styles.
  {
    Error_code err_code;
    EXPECT_TRUE(Native_handle{}.dup(&err_code).null());
    EXPECT_EQ(err_code, transport::error::Code::S_INVALID_ARGUMENT);

    EXPECT_THROW(Native_handle{}.dup(), Runtime_error);
    EXPECT_THROW(duped_native_handle(Native_handle{}), Runtime_error);
  }

  /* System error: EMFILE.  `hndl` was just allocated lowest-free, so every descriptor number below it is in use;
   * hence a limit of `hndl + 1` leaves no free number for the duplicate. */
  {
    const auto hndl = make_fd();
    Error_code err_code;
    {
      const Fd_limit_scope limit_scope{rlim_t(hndl.m_native_handle) + 1};
      EXPECT_TRUE(hndl.dup(&err_code).null());
    }
    EXPECT_EQ(err_code, too_many_files_open);
    EXPECT_TRUE(hndl.is_open()); // Original unaffected by the failure.

    Native_handle{hndl}.close();
  }
}

} // namespace ipc::util::test
