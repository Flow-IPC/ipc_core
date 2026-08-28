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

#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>
#include <gtest/gtest.h>
#include <utility>

namespace ipc::transport::test
{

namespace
{

using Pn = Protocol_negotiator;

// Convenience maker.  Attention: the ctor takes (max, min) -- in that order.
Pn make_pn(Pn::proto_ver_t max_ver, Pn::proto_ver_t min_ver)
{
  return Pn{nullptr, "test", max_ver, min_ver};
}

} // namespace (anon)

// Special values; initial state; the send-tracking side (local_max_proto_ver_for_sending()); reset().
TEST(Protocol_negotiator_test, basics)
{
  // The wire encoding relies on these specific value ranges: versions positive, sentinels not.
  EXPECT_EQ(Pn::S_VER_UNSUPPORTED, 0);
  EXPECT_LT(Pn::S_VER_UNKNOWN, 0);
  static_assert(Pn::S_VER_ALREADY_SENT == Pn::S_VER_UNKNOWN, "ALREADY_SENT is (only) a clearer-named alias.");

  auto pn = make_pn(3, 1);
  EXPECT_EQ(pn.negotiated_proto_ver(), Pn::S_VER_UNKNOWN);

  // First call yields our max (preferred) version; subsequent ones yield ALREADY_SENT.
  EXPECT_EQ(pn.local_max_proto_ver_for_sending(), 3);
  EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);
  EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);

  // The two directions are independent: negotiating does not un-send or otherwise affect the send side.
  Error_code err_code;
  EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
  EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);

  // reset() returns to the just-cted state: nothing negotiated, nothing sent; config retained.
  pn.reset();
  EXPECT_EQ(pn.negotiated_proto_ver(), Pn::S_VER_UNKNOWN);
  EXPECT_EQ(pn.local_max_proto_ver_for_sending(), 3);
}

// The negotiation algorithm proper: V = (H <= Hp) ? H : ((Hp >= L) ? Hp : UNSUPPORTED); plus dupe-call no-op.
TEST(Protocol_negotiator_test, negotiation_outcomes)
{
  Error_code err_code;

  { // Both prefer the same version: speak it.
    auto pn = make_pn(3, 1);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
  }

  { // They are more advanced: speak our max (they deal with backwards compatibility, or hose the pathway).
    auto pn = make_pn(3, 1);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(5, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
  }

  { // We are more advanced but backwards-compatible with their max: speak *their* max.
    auto pn = make_pn(5, 2);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
  }

  { // Ditto at the edge: their max = our min.
    auto pn = make_pn(5, 2);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(2, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_EQ(pn.negotiated_proto_ver(), 2);
  }

  { // We are more advanced, and they are too old for our backwards compatibility: incompatible.
    auto pn = make_pn(5, 4);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
    EXPECT_EQ(err_code, error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD);
    EXPECT_EQ(pn.negotiated_proto_ver(), Pn::S_VER_UNSUPPORTED);
  }

  // Non-positive opposing version (e.g., if one failed to parse it and passed UNKNOWN as advised): invalid.
  for (const Pn::proto_ver_t bad_ver : { Pn::proto_ver_t(0), Pn::S_VER_UNKNOWN, Pn::proto_ver_t(-7) })
  {
    auto pn = make_pn(3, 1);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(bad_ver, &err_code));
    EXPECT_EQ(err_code, error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID);
    EXPECT_EQ(pn.negotiated_proto_ver(), Pn::S_VER_UNSUPPORTED);
  }

  { // Dupe call: no-op; returns false; result stands even given a different opposing version.
    auto pn = make_pn(3, 1);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
    EXPECT_FALSE(pn.compute_negotiated_proto_ver(2, &err_code));
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
  }
}

// The two error-reporting forms: error-code out-arg (including falsy-on-success hygiene) and exception.
TEST(Protocol_negotiator_test, error_reporting)
{
  { // Success must *clear* a previously-truthy out-arg (standard Flow convention).
    auto pn = make_pn(3, 1);
    Error_code err_code = error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD; // Simulated stale value.
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3, &err_code));
    EXPECT_FALSE(err_code);
  }

  { // Exception form, success: no throw.
    auto pn = make_pn(3, 1);
    EXPECT_TRUE(pn.compute_negotiated_proto_ver(3));
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);

    // Exception form, dupe call: still no throw; returns false.
    EXPECT_FALSE(pn.compute_negotiated_proto_ver(3));
  }

  { // Exception form, failure: throws Runtime_error carrying the suggested Error_code.
    auto pn = make_pn(5, 4);
    bool threw = false;
    try
    {
      pn.compute_negotiated_proto_ver(3);
    }
    catch (const flow::error::Runtime_error& exc)
    {
      threw = true;
      EXPECT_EQ(exc.code(), error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(pn.negotiated_proto_ver(), Pn::S_VER_UNSUPPORTED);
  }
}

// Copy/move ctors and assignments; moved-from state (as-if just-cted).
TEST(Protocol_negotiator_test, copy_move_assign)
{
  Error_code err_code;

  // Set up a source with both state bits flipped: negotiated; and max-version-for-sending consumed.
  auto pn_src = make_pn(5, 2);
  EXPECT_TRUE(pn_src.compute_negotiated_proto_ver(3, &err_code));
  EXPECT_EQ(pn_src.local_max_proto_ver_for_sending(), 5);

  { // Copy ctor: full state comes along (negotiated version; already-sent-ness).
    auto pn = pn_src;
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
    EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);
    // Copy did not disturb the source.
    EXPECT_EQ(pn_src.negotiated_proto_ver(), 3);
  }

  { // Copy assignment: ditto.
    auto pn = make_pn(9, 9);
    pn = pn_src;
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
    EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);
  }

  { // Move ctor: state moves out; the moved-from becomes as-if just-cted (config kept, dynamic state reset).
    auto pn_from = pn_src; // (Copy, so pn_src stays usable for the next scope.)
    auto pn = std::move(pn_from);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
    EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);
    EXPECT_EQ(pn_from.negotiated_proto_ver(), Pn::S_VER_UNKNOWN);
    EXPECT_EQ(pn_from.local_max_proto_ver_for_sending(), 5);
  }

  { // Move assignment: ditto.
    auto pn_from = pn_src;
    auto pn = make_pn(9, 9);
    pn = std::move(pn_from);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
    EXPECT_EQ(pn.local_max_proto_ver_for_sending(), Pn::S_VER_ALREADY_SENT);
    EXPECT_EQ(pn_from.negotiated_proto_ver(), Pn::S_VER_UNKNOWN);
    EXPECT_EQ(pn_from.local_max_proto_ver_for_sending(), 5);

    // Self-move-assignment: contractually a no-op.  (Via pointer, to avoid compiler self-move warnings.)
    auto* const pn_ptr = &pn;
    pn = std::move(*pn_ptr);
    EXPECT_EQ(pn.negotiated_proto_ver(), 3);
  }
}

} // namespace ipc::transport::test
