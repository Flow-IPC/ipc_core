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

#include "ipc/util/shared_name.hpp"
#include "ipc/common.hpp"
#include <boost/unordered_set.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ipc::util::test
{

namespace
{

using std::string;

constexpr char SEP = Shared_name::S_SEPARATOR;
constexpr size_t MAX_LEN = Shared_name::S_MAX_LENGTH;

/* Reference model of sanitize(), written straight from its contract (not from the impl): '/' -> separator; then
 * collapse each run of 2+ separators into 1; result must then be sanitized() (legal chars, no double-separators --
 * true by construction --, length <= max); if not, nothing changes.  Returns (expected ret, expected str()). */
std::pair<bool, string> sanitize_model(const string& src)
{
  string out;
  for (char ch : src)
  {
    if (ch == '/')
    {
      ch = SEP;
    }
    if ((ch == SEP) && (!out.empty()) && (out.back() == SEP))
    {
      continue;
    }
    out += ch;
  }

  const bool legal_chars
    = out.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" + string(1, SEP))
        == string::npos;
  return ((out.size() <= MAX_LEN) && legal_chars) ? std::make_pair(true, out) : std::make_pair(false, src);
}

// Checks sanitize() on `src` against sanitize_model(); plus contract properties (ret == sanitized(), idempotence).
void check_sanitize(const string& src)
{
  const auto [exp_ok, exp_str] = sanitize_model(src);

  auto name = Shared_name::ct(src);
  const bool was_sanitized = name.sanitized();

  EXPECT_EQ(name.sanitize(), exp_ok) << "src = [" << src << "].";
  EXPECT_EQ(name.str(), exp_str) << "src = [" << src << "].";
  EXPECT_EQ(name.sanitized(), exp_ok) << "Return value must equal final sanitized(); src = [" << src << "].";
  if (was_sanitized)
  {
    EXPECT_EQ(name.str(), src) << "Already-sanitized() name must be left alone; src = [" << src << "].";
  }

  // Idempotence: a 2nd sanitize() changes nothing and returns the same.
  const auto after_1st = name.str();
  EXPECT_EQ(name.sanitize(), exp_ok) << "src = [" << src << "].";
  EXPECT_EQ(name.str(), after_1st) << "src = [" << src << "].";
}

template<typename Int>
void check_ct_from_int_type()
{
  using lim = std::numeric_limits<Int>;

  const auto check = [](Int val)
  {
    const auto name = Shared_name::ct_from_int(val);
    // `+val` promotes [un]signed char to int, so to_string() prints a number, not a character.
    EXPECT_EQ(name.str(), std::to_string(+val)) << "sizeof = [" << sizeof(Int) << "], signed = [" << lim::is_signed
                                                << "].";
    if constexpr(lim::is_signed)
    {
      EXPECT_EQ(name.sanitized(), val >= 0) << "Only a minus sign should make it non-sanitized().";
    }
    else
    {
      EXPECT_TRUE(name.sanitized());
    }
  };

  check(0);
  check(lim::max());
  check(Int(lim::max() - 1));
  check(lim::min()); // For signed types: the longest string (e.g., -128 or -9223372036854775808).
  check(Int(lim::min() + 1));

  // Each power of 10 that fits, plus neighbors: the digit-count boundaries.
  for (Int pwr = 1; ; pwr = Int(pwr * 10))
  {
    check(pwr);
    check(Int(pwr - 1));
    if constexpr(lim::is_signed)
    {
      check(Int(-pwr));
      check(Int(1 - pwr));
    }
    if (pwr > (lim::max() / 10))
    {
      break;
    }
  }
} // check_ct_from_int_type()

} // namespace (anon)

TEST(Shared_name_test, construction_and_assignment)
{
  using std::vector;

  const Shared_name dflt;
  EXPECT_TRUE(dflt.empty());
  EXPECT_TRUE(Shared_name::S_EMPTY.empty());

  EXPECT_EQ(Shared_name::ct("abc").str(), "abc");
  EXPECT_EQ(Shared_name::ct("").str(), "");

  const string src_str = "fromString";
  EXPECT_EQ(Shared_name::ct(src_str).str(), src_str);
  EXPECT_EQ(src_str, "fromString") << "Copying ct() must not touch the source.";

  const String_view src_view{"xxSubViewxx"};
  EXPECT_EQ(Shared_name::ct(src_view.substr(2, 7)).str(), "SubView") << "Non-NUL-terminated view must work.";

  string movable = "moveMe_please_make_this_long_enough_to_not_be_SSO";
  const auto moved_to = Shared_name::ct(std::move(movable));
  EXPECT_EQ(moved_to.str(), "moveMe_please_make_this_long_enough_to_not_be_SSO");
  EXPECT_TRUE(movable.empty()) << "ct(string&&) promises to empty the source.";

  const char raw[] = "rangeCtor";
  EXPECT_EQ(Shared_name(raw, raw + 5).str(), "range");
  EXPECT_TRUE(Shared_name(raw, raw).empty());
  const vector<char> vec{'v', 'e', 'c'};
  EXPECT_EQ(Shared_name(vec.begin(), vec.end()).str(), "vec");

  // Copy/move ctor and assignment.
  auto orig = Shared_name::ct("original");
  Shared_name copy_cted{orig};
  EXPECT_EQ(copy_cted, orig);
  Shared_name move_cted{std::move(copy_cted)};
  EXPECT_EQ(move_cted, orig);
  EXPECT_TRUE(copy_cted.empty()) << "Move-ctor promises to make the source empty().";

  Shared_name assigned;
  assigned = orig;
  EXPECT_EQ(assigned, orig);
  Shared_name move_assigned;
  move_assigned = std::move(assigned);
  EXPECT_EQ(move_assigned, orig);
  move_assigned = Shared_name::ct("replaced"); // The advertised way to assign from a string.
  EXPECT_EQ(move_assigned.str(), "replaced");
} // TEST(Shared_name_test, construction_and_assignment)

TEST(Shared_name_test, accessors)
{
  auto name = Shared_name::ct("abc");
  EXPECT_EQ(name.size(), 3u);
  EXPECT_FALSE(name.empty());
  EXPECT_EQ(std::strlen(name.native_str()), name.size()) << "native_str() must be NUL-terminated.";
  EXPECT_EQ(string(name.native_str()), name.str());

  name.clear();
  EXPECT_TRUE(name.empty());
  EXPECT_EQ(name.size(), 0u);
  EXPECT_STREQ(name.native_str(), "");

  // absolute() <=> first char is separator.  has_trailing_separator() <=> non-empty and last char is separator.
  EXPECT_FALSE(Shared_name().absolute());
  EXPECT_FALSE(Shared_name().has_trailing_separator());
  EXPECT_TRUE(Shared_name::ct(string(1, SEP)).absolute());
  EXPECT_TRUE(Shared_name::ct(string(1, SEP)).has_trailing_separator());
  EXPECT_TRUE(Shared_name::ct(SEP + string("a")).absolute());
  EXPECT_FALSE(Shared_name::ct(SEP + string("a")).has_trailing_separator());
  EXPECT_FALSE((Shared_name::ct("a") + SEP).absolute());
  EXPECT_TRUE((Shared_name::ct("a") + SEP).has_trailing_separator());
  EXPECT_FALSE(Shared_name::ct("/a").absolute()) << "Only the separator counts; '/' is just a character.";
} // TEST(Shared_name_test, accessors)

TEST(Shared_name_test, constants)
{
  EXPECT_EQ(SEP, '_');
  EXPECT_EQ(Shared_name::S_SENTINEL, "0");
  EXPECT_EQ(Shared_name::S_1ST_OR_ONLY, "1");

  for (const auto& fragment : { Shared_name::S_ROOT_MAGIC, Shared_name::S_SENTINEL, Shared_name::S_1ST_OR_ONLY,
                                Shared_name::S_RESOURCE_TYPE_ID_SHM, Shared_name::S_RESOURCE_TYPE_ID_MUTEX })
  {
    EXPECT_FALSE(fragment.empty());
    EXPECT_EQ(fragment.str().find(SEP), string::npos) << "Fragment [" << fragment.str() << "] must lack separators.";
    EXPECT_TRUE(fragment.sanitized()) << "Fragment [" << fragment.str() << "].";
  }
  EXPECT_NE(Shared_name::S_RESOURCE_TYPE_ID_SHM, Shared_name::S_RESOURCE_TYPE_ID_MUTEX);
} // TEST(Shared_name_test, constants)

TEST(Shared_name_test, append_ops)
{
  const auto a = Shared_name::ct("a");
  const auto b = Shared_name::ct("b");
  const string b_str = "b";
  const String_view b_view{"b"};
  const string a_sep_b = "a" + string(1, SEP) + "b";

  // += in all forms: plain concatenation.
  for (int form = 0; form != 5; ++form)
  {
    auto x = a;
    auto& ret = (form == 0) ? (x += b) : (form == 1) ? (x += b_str) : (form == 2) ? (x += b_view)
                  : (form == 3) ? (x += "b") : (x += 'b');
    EXPECT_EQ(&ret, &x) << "+= must return *this.";
    EXPECT_EQ(x.str(), "ab") << "form = [" << form << "].";
  }

  // /= in all forms: separator then concatenation.
  for (int form = 0; form != 5; ++form)
  {
    auto x = a;
    auto& ret = (form == 0) ? (x /= b) : (form == 1) ? (x /= b_str) : (form == 2) ? (x /= b_view)
                  : (form == 3) ? (x /= "b") : (x /= 'b');
    EXPECT_EQ(&ret, &x) << "/= must return *this.";
    EXPECT_EQ(x.str(), a_sep_b) << "form = [" << form << "].";
  }

  // Binary + and / in all forms, both orders; operands untouched.
  EXPECT_EQ((a + b).str(), "ab");
  EXPECT_EQ((a + b_str).str(), "ab");
  EXPECT_EQ((a + b_view).str(), "ab");
  EXPECT_EQ((a + "b").str(), "ab");
  EXPECT_EQ(("b" + a).str(), "ba");
  EXPECT_EQ((b_str + a).str(), "ba");
  EXPECT_EQ((b_view + a).str(), "ba");
  EXPECT_EQ((a / b).str(), a_sep_b);
  EXPECT_EQ((a / b_str).str(), a_sep_b);
  EXPECT_EQ((a / b_view).str(), a_sep_b);
  EXPECT_EQ((a / "b").str(), a_sep_b);
  EXPECT_EQ(("b" / a).str(), "b" + string(1, SEP) + "a");
  EXPECT_EQ((b_str / a).str(), "b" + string(1, SEP) + "a");
  EXPECT_EQ((b_view / a).str(), "b" + string(1, SEP) + "a");
  EXPECT_EQ(a.str(), "a");
  EXPECT_EQ(b.str(), "b");

  // Deliberately dumb: no double-separator avoidance; empty operands still get a separator.
  EXPECT_EQ(((a + SEP) / b).str(), "a" + string(2, SEP) + "b");
  EXPECT_EQ((Shared_name() / Shared_name()).str(), string(1, SEP));
  EXPECT_EQ((Shared_name() / a).str(), string(1, SEP) + "a");
  EXPECT_EQ((a / Shared_name()).str(), "a" + string(1, SEP));

  // Chaining.
  EXPECT_EQ((Shared_name() / a / b / "c").str(), string(1, SEP) + a_sep_b + SEP + "c");
} // TEST(Shared_name_test, append_ops)

TEST(Shared_name_test, self_append)
{
  /* Appending a name to itself.  Make the source string full to capacity (and heap-allocated), so that the first
   * character appended forces a reallocation; any internal view into the old buffer would then dangle (ASan:
   * heap-use-after-free; otherwise likely garbage or, by luck, the right answer). */
  const string base(40, 'x');

  const auto tight_name = [&]()
  {
    string str = base;
    str.shrink_to_fit();
    return Shared_name::ct(std::move(str));
  };

  {
    auto name = tight_name();
    name += name;
    EXPECT_EQ(name.str(), base + base);
  }
  {
    auto name = tight_name();
    const auto& same_name = name; // Alias, so clang's -Wself-assign-overloaded (x /= x) does not trigger.
    name /= same_name;
    EXPECT_EQ(name.str(), base + SEP + base);
  }
  {
    auto name = tight_name();
    name /= name.str();
    EXPECT_EQ(name.str(), base + SEP + base);
  }
  {
    auto name = tight_name();
    name /= name.native_str();
    EXPECT_EQ(name.str(), base + SEP + base);
  }
  {
    // A view of *part* of *this.
    auto name = Shared_name::ct(string(20, 'x') + string(20, 'y'));
    string str = name.str();
    str.shrink_to_fit();
    name = Shared_name::ct(std::move(str));
    const String_view part = String_view{name.str()}.substr(15, 10);
    name /= part;
    EXPECT_EQ(name.str(), string(20, 'x') + string(20, 'y') + SEP + string(5, 'x') + string(5, 'y'));
  }
  {
    // Degenerate: empty.
    Shared_name name;
    const auto& same_name = name; // See above.
    name /= same_name;
    EXPECT_EQ(name.str(), string(1, SEP));
    Shared_name name2;
    name2 /= name2.str();
    EXPECT_EQ(name2.str(), string(1, SEP));
  }
} // TEST(Shared_name_test, self_append)

TEST(Shared_name_test, comparison_hash_swap)
{
  using std::map;
  using boost::unordered_set;

  const auto a = Shared_name::ct("a");
  const auto a2 = Shared_name::ct("a");
  const auto b = Shared_name::ct("b");

  EXPECT_TRUE(a == a2);
  EXPECT_FALSE(a != a2);
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a != b);

  // Against raw strings, both sides.
  EXPECT_TRUE(a == "a");
  EXPECT_TRUE("a" == a);
  EXPECT_TRUE(a == string("a"));
  EXPECT_TRUE(string("a") == a);
  EXPECT_TRUE(a == String_view("a"));
  EXPECT_TRUE(String_view("a") == a);
  EXPECT_TRUE(a != "b");
  EXPECT_TRUE("b" != a);
  EXPECT_TRUE(a != String_view("ab"));
  EXPECT_TRUE(a != "");

  // No normalization in comparisons.
  EXPECT_NE(Shared_name::ct("a" + string(2, SEP) + "b"), Shared_name::ct("a" + string(1, SEP) + "b"));
  EXPECT_NE(Shared_name::ct("a/b"), Shared_name::ct("a" + string(1, SEP) + "b"));

  // < <=> str() <; usable as std::map key.
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_FALSE(a < a2);
  EXPECT_TRUE(Shared_name() < a);
  EXPECT_TRUE(a < Shared_name::ct("aa"));
  const map<Shared_name, int> sorted{ { b, 2 }, { a, 1 }, { Shared_name::ct("aa"), 3 } };
  ASSERT_EQ(sorted.size(), 3u);
  auto it = sorted.begin();
  EXPECT_EQ((it++)->first, "a");
  EXPECT_EQ((it++)->first, "aa");
  EXPECT_EQ(it->first, "b");

  // hash_value(): equal names => equal hashes; usable in boost.unordered.
  EXPECT_EQ(hash_value(a), hash_value(a2));
  unordered_set<Shared_name> set{ a, b };
  EXPECT_EQ(set.size(), 2u);
  set.insert(a2);
  EXPECT_EQ(set.size(), 2u);
  EXPECT_EQ(set.count(Shared_name::ct("a")), 1u);
  EXPECT_EQ(set.count(Shared_name::ct("c")), 0u);

  // ADL swap.
  auto x = Shared_name::ct("x");
  auto y = Shared_name::ct("yy");
  {
    using std::swap;
    swap(x, y);
  }
  EXPECT_EQ(x, "yy");
  EXPECT_EQ(y, "x");
} // TEST(Shared_name_test, comparison_hash_swap)

TEST(Shared_name_test, stream_io)
{
  using std::ostringstream;
  using std::istringstream;

  // <<: not equal to str(); includes str() and its length (decimal).
  const auto name = Shared_name::ct("someName");
  ostringstream os;
  os << name;
  const auto out = os.str();
  EXPECT_NE(out, name.str());
  EXPECT_NE(out.find(name.str()), string::npos) << "out = [" << out << "].";
  EXPECT_NE(out.find(std::to_string(name.size())), string::npos) << "out = [" << out << "].";

  ostringstream os_empty;
  os_empty << Shared_name();
  EXPECT_EQ(os_empty.str(), "null");

  // >>: same as `is >> string` (whitespace-delimited).
  istringstream is{"first  second"};
  Shared_name in1;
  Shared_name in2;
  is >> in1 >> in2;
  EXPECT_EQ(in1, "first");
  EXPECT_EQ(in2, "second");
} // TEST(Shared_name_test, stream_io)

TEST(Shared_name_test, ct_from_int)
{
  check_ct_from_int_type<char>();
  check_ct_from_int_type<signed char>();
  check_ct_from_int_type<unsigned char>();
  check_ct_from_int_type<short>();
  check_ct_from_int_type<unsigned short>();
  check_ct_from_int_type<int>();
  check_ct_from_int_type<unsigned int>();
  check_ct_from_int_type<long>();
  check_ct_from_int_type<unsigned long>();
  check_ct_from_int_type<long long>();
  check_ct_from_int_type<unsigned long long>();
  check_ct_from_int_type<int8_t>();
  check_ct_from_int_type<uint8_t>();
  check_ct_from_int_type<int16_t>();
  check_ct_from_int_type<uint16_t>();
  check_ct_from_int_type<int32_t>();
  check_ct_from_int_type<uint32_t>();
  check_ct_from_int_type<int64_t>();
  check_ct_from_int_type<uint64_t>();

  // Spot-check the longest possible outputs exactly (not just via to_string()).
  EXPECT_EQ(Shared_name::ct_from_int(std::numeric_limits<int64_t>::min()).str(), "-9223372036854775808");
  EXPECT_EQ(Shared_name::ct_from_int(std::numeric_limits<uint64_t>::max()).str(), "18446744073709551615");
  EXPECT_EQ(Shared_name::ct_from_int(int8_t(-128)).str(), "-128");
  EXPECT_EQ(Shared_name::ct_from_int(uint8_t(255)).str(), "255");

  // Usable as a path component, as S_1ST_OR_ONLY's doc suggests.
  EXPECT_EQ(Shared_name::ct_from_int(1), Shared_name::S_1ST_OR_ONLY);
  EXPECT_EQ(Shared_name::ct_from_int(0u), Shared_name::S_SENTINEL);
} // TEST(Shared_name_test, ct_from_int)

TEST(Shared_name_test, sanitized)
{
  const string sep(1, SEP);
  const auto ok = [](const string& str) { return Shared_name::ct(str).sanitized(); };

  EXPECT_TRUE(ok(""));
  EXPECT_TRUE(ok("abcXYZ019"));
  EXPECT_TRUE(ok(sep));
  EXPECT_TRUE(ok(sep + "a" + sep + "b" + sep)) << "Leading/trailing single separators are fine.";
  EXPECT_FALSE(ok(sep + sep));
  EXPECT_FALSE(ok("a" + sep + sep + "b"));
  EXPECT_FALSE(ok("a/b")) << "'/' is not a legal character; only sanitize() converts it.";
  EXPECT_FALSE(ok("a-b"));
  EXPECT_FALSE(ok("a b"));
  EXPECT_FALSE(ok("a.b"));
  EXPECT_FALSE(ok(string("a\0b", 3)));
  EXPECT_FALSE(ok("caf\xC3\xA9")) << "Non-ASCII bytes are not alphanumeric.";
  EXPECT_FALSE(ok("\xFF"));

  // Length limit: inclusive.
  EXPECT_TRUE(ok(string(MAX_LEN, 'a')));
  EXPECT_FALSE(ok(string(MAX_LEN + 1, 'a')));
  EXPECT_TRUE(ok(string(MAX_LEN - 1, 'a') + sep));
  EXPECT_FALSE(ok(string(MAX_LEN, 'a') + sep));
} // TEST(Shared_name_test, sanitized)

TEST(Shared_name_test, sanitize_corner_cases)
{
  const string sep(1, SEP);

  // Hand-picked cases; expected values come from sanitize_model(), but the comments say what each one should be.
  check_sanitize(""); // true, "".
  check_sanitize("abc"); // true, unchanged.
  check_sanitize(sep); // true, unchanged.
  check_sanitize("/"); // true, sep.
  check_sanitize("a/b"); // true, "a_b".
  check_sanitize("a//b"); // true, "a_b".
  check_sanitize("a" + sep + "/b"); // Mixed separators collapse: "a_b".
  check_sanitize("/" + sep + "/"); // sep.
  check_sanitize("//a//"); // "_a_".
  check_sanitize("a/"); // Trailing separator stays (single): "a_".
  check_sanitize("a" + sep + sep + sep); // "a_".
  check_sanitize(string(500, '/')); // Long input, collapses to 1 char: true.
  check_sanitize("a-b"); // false, unchanged.
  check_sanitize("a//-"); // false, unchanged: the '/' replacement must be undone, not left halfway.
  check_sanitize("-//a"); // false, unchanged (illegal char first).

  // Length: limit applies to the *output*, so collapsing can rescue an over-long input; and vice versa.
  check_sanitize(string(MAX_LEN, 'a')); // true.
  check_sanitize(string(MAX_LEN + 1, 'a')); // false, unchanged.
  check_sanitize(string(MAX_LEN - 1, 'a') + "///"); // true: MAX_LEN - 1 + 1.
  check_sanitize(string(MAX_LEN, 'a') + "/"); // false: MAX_LEN + 1.
  check_sanitize(string(MAX_LEN - 1, 'a') + "//b"); // false: MAX_LEN + 1.
  check_sanitize(string(40, 'a') + string(100, '/') + string(MAX_LEN - 41, 'b')); // true: exactly MAX_LEN.
  check_sanitize(string(40, 'a') + string(100, '/') + string(MAX_LEN - 40, 'b')); // false: MAX_LEN + 1.
  check_sanitize(string(MAX_LEN + 10, 'a') + "-"); // false (both reasons).
} // TEST(Shared_name_test, sanitize_corner_cases)

TEST(Shared_name_test, sanitize_exhaustive_small)
{
  /* Every string of length 0..6 over an alphabet covering each character class sanitize() treats differently:
   * legal non-separator, separator, alternate separator '/', illegal.  Compare against the contract-based model. */
  const string alphabet = string("a") + SEP + "/-";
  const size_t n_chars = alphabet.size();

  for (size_t len = 0; len <= 6; ++len)
  {
    size_t n_combos = 1;
    for (size_t i = 0; i != len; ++i)
    {
      n_combos *= n_chars;
    }

    for (size_t combo = 0; combo != n_combos; ++combo)
    {
      string src;
      for (size_t i = 0, rest = combo; i != len; ++i, rest /= n_chars)
      {
        src += alphabet[rest % n_chars];
      }
      check_sanitize(src);
      if (::testing::Test::HasFailure())
      {
        return; // One reported counterexample is plenty; avoid a flood.
      }
    }
  }
} // TEST(Shared_name_test, sanitize_exhaustive_small)

} // namespace ipc::util::test
