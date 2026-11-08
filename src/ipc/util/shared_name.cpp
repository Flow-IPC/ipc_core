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
#include "ipc/util/shared_name.hpp"
#include <boost/functional/hash/hash.hpp>
#include <cassert>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <algorithm>

namespace ipc::util
{

// Initializers.


const Shared_name Shared_name::S_EMPTY;

const Shared_name Shared_name::S_ROOT_MAGIC = Shared_name::ct("libipc");
const Shared_name Shared_name::S_SENTINEL = Shared_name::ct("0");
const Shared_name Shared_name::S_1ST_OR_ONLY = Shared_name::ct("1");

const Shared_name Shared_name::S_RESOURCE_TYPE_ID_SHM = Shared_name::ct("shm");
const Shared_name Shared_name::S_RESOURCE_TYPE_ID_MUTEX = Shared_name::ct("mtx");

// Implementations.

Shared_name::Shared_name() = default;
Shared_name::Shared_name(const Shared_name&) = default;
Shared_name::Shared_name(Shared_name&&) = default;

Shared_name Shared_name::ct(const char* src) // Static.
{
  Shared_name result;
  result.m_raw_name.assign(src); // Copy it.
  return result;
}

Shared_name Shared_name::ct(std::string&& src_moved) // Static.
{
  Shared_name result;
  result.m_raw_name.assign(std::move(src_moved)); // Move it.
  return result;
}

Shared_name& Shared_name::operator=(const Shared_name&) = default;
Shared_name& Shared_name::operator=(Shared_name&&) = default;

Shared_name& Shared_name::operator+=(const Shared_name& to_append)
{
  using util::String_view;
  return operator+=(String_view{to_append.str()});
}

Shared_name& Shared_name::operator/=(const Shared_name& to_append)
{
  using util::String_view;
  return operator/=(String_view{to_append.str()});
}

Shared_name operator+(const Shared_name& src1, const Shared_name& src2)
{
  using util::String_view;
  return src1 + String_view{src2.str()};
}

Shared_name operator/(const Shared_name& src1, const Shared_name& src2)
{
  return Shared_name{src1} /= src2;
}

const std::string& Shared_name::str() const
{
  return m_raw_name;
}

const char* Shared_name::native_str() const
{
  return m_raw_name.c_str();
}

size_t Shared_name::size() const
{
  return m_raw_name.size();
}

bool Shared_name::empty() const
{
  return m_raw_name.empty();
}

bool Shared_name::has_trailing_separator() const
{
  return (!empty()) && (m_raw_name.back() == S_SEPARATOR);
}

bool Shared_name::absolute() const
{
  return (!m_raw_name.empty()) && (m_raw_name.front() == S_SEPARATOR);
}

void Shared_name::clear()
{
  m_raw_name.clear();
}

bool Shared_name::sanitized() const
{
  using flow::util::in_closed_range;

  // Keep in sync with sanitize()!

  if (size() > S_MAX_LENGTH)
  {
    return false;
  }
  // else

  /* Following is self-explanatory given the contract.  It could be made much briefer, probably, by using built-in
   * string functions and/or string_algo/regex/....  However, for nicer perf, we'd rather do only one linear-time scan.
   * Finally, could other characters be potentially allowed by the various sys calls?  Maybe, but we'd rather be safe
   * than sorry in terms of portability, and also these chars should be aesthetically good as a convention. */

  bool prev_is_sep = false;
  for (const auto ch : m_raw_name)
  {
    bool is_sep = ch == S_SEPARATOR;
    if ((!is_sep) && (!sanitized_non_sep(ch)))
    {
      return false;
    }
    // else

    if (is_sep)
    {
      if (prev_is_sep)
      {
        return false;
      }
      // else
      prev_is_sep = true;
    }
    else
    {
      prev_is_sep = false; // Might be no-op.
    }
  } // for (ch : m_raw_name)

  return true;
} // Shared_name::sanitized()

bool Shared_name::sanitize()
{
  using std::string;
  using std::swap;

  // SEPS are the 2 allowed input separator characters.  They must be different.  Try to keep this compile-time.
  constexpr char SEPARATOR_ALT = '/';
  static_assert(S_SEPARATOR != SEPARATOR_ALT,
                "The real separator and the alt must be different characters.");
  constexpr const char SEPS[] = { SEPARATOR_ALT, S_SEPARATOR, '\0' };

  /* The following implementation, while self-explanatory, foregoes briefer approaches in favor of better perf.
   * Keep in sync with sanitized()! */

  if (empty()) // Get rid of degenerate case for simplicity of below.
  {
    return true;
  }
  // else at least one character to read and (unless it's illegal) write.

  const auto n = size();
  const auto start_ptr = m_raw_name.cbegin();
  const auto end_ptr = m_raw_name.cend();
  string sanitized_str;
  sanitized_str.reserve(size());

  for (auto ptr = start_ptr; ptr != end_ptr; ++ptr)
  {
    /* We cannot predict, at the start, whether we'll ultimately result in a too-long string, because repeating
     * separators can actually lower the length.  However, if we get to this iteration, then we *will* be writing a
     * character (unless ch is illegal, but then we'd return false anyway); hence we can detect ASAP whether the
     * output is too long for sure. */
    if (sanitized_str.size() >= S_MAX_LENGTH)
    {
      return false;
    }
    // else not too long yet.

    auto ch = *ptr;
    const bool is_sep = (ch == S_SEPARATOR) || (ch == SEPARATOR_ALT);

    // Perform nearly identical check to sanitized() (reminder: the 2 functions must remain in sync).
    if ((!is_sep) && (!sanitized_non_sep(ch)))
    {
      return false;
    }
    // else: Legal character ch; though it may be sanitized by replacing with a different value.

    if (is_sep)
    {
      ch = S_SEPARATOR; // Namely, all separator possibilities are normalized to this one.

      /* Since we found a separator, immediately skip past any more adjacent separators by moving ptr
       * inside this { body }.  Set it to the position just before the first non-separator.  That ranges between:
       * not touching ptr at all (i.e., only one separator after all); and setting it to &*(n - 1) (meaning either
       * we're at end of m_raw_name, or the rest of it is all separators).  Note for() will ++ptr right after this. */
      auto pos = size_t(ptr - start_ptr);
      if (pos != (n - 1))
      {
        pos = m_raw_name.find_first_not_of(SEPS, pos + 1);
        if (pos == string::npos)
        {
          pos = n;
        }
        --pos;

        ptr = start_ptr + pos;
      }
    } // if (is_sep)

    sanitized_str += ch;
  } // for (ptr in [start_ptr, end_ptr))

  // Passed gauntlet in terms of whether we can `return true` (we can and must).  Finish up.
  swap(m_raw_name, sanitized_str); // Constant-time.  Then sanitized_str is deallocated.
  return true;
} // Shared_name::sanitize()

bool Shared_name::sanitized_non_sep(char ch) // Static.
{
  /* std::isalpha/isalnum(s_c<uchar>(ch)) is tempting, but it's affected by the locale; we want none of that.
   * std::isdigit(s_c<uchar>(ch)) should be fine... but just screw it, man. */
  return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z'))
         || ((ch >= '0') && (ch <= '9'));
}

Shared_name build_conventional_non_session_based_shared_name(const Shared_name& resource_type)
{
  assert(!resource_type.empty());

  const auto& SENTINEL = Shared_name::S_SENTINEL;
  const auto& ROOT_MAGIC = Shared_name::S_ROOT_MAGIC;

  Shared_name name;
  name /= ROOT_MAGIC;
  name /= resource_type;
  name /= SENTINEL;
  name /= SENTINEL;
  name /= SENTINEL;
  return name;
} // build_conventional_non_session_based_shared_name()

std::ostream& operator<<(std::ostream& os, const Shared_name& val)
{
  // Output char count for convenience: these can be at a premium.
  if (val.str().empty())
  {
    return os << "null";
  }
  // else
  return os << val.str().size() << '|' << val.str();
}

std::istream& operator>>(std::istream& is, Shared_name& val)
{
  // @todo Can be made a tiny bit faster by writing directly into val.m_raw_name.  Requires `friend` or something.
  std::string str;
  is >> str;
  if (is)
  {
    val = Shared_name::ct(std::move(str));
  }
  // else { We'd write an empty string; technically that is against our contract. }
  return is;
}

bool operator==(const Shared_name& val1, const Shared_name& val2)
{
  return val1.str() == val2.str();
}

bool operator!=(const Shared_name& val1, const Shared_name& val2)
{
  return !(operator==(val1, val2));
}

bool operator==(const Shared_name& val1, util::String_view val2)
{
  /* Existence/impl rationale: This is faster than if they had to: `val1 == Shared_name::ct(val2)`.
   * Existence rationale: It's also nice to be able to write: `val1 == "something"` (String_view implicitly cted). */

  using util::String_view;
  return std::operator==(String_view{val1.str()}, val2);
}

bool operator!=(const Shared_name& val1, util::String_view val2)
{
  return !(operator==(val1, val2));
}

bool operator==(util::String_view val1, const Shared_name& val2)
{
  return operator==(val2, val1);
}

bool operator!=(util::String_view val1, const Shared_name& val2)
{
  return !(operator==(val1, val2));
}

bool operator<(const Shared_name& val1, const Shared_name& val2)
{
  return val1.str() < val2.str();
}

size_t hash_value(const Shared_name& val)
{
  using boost::hash;
  using std::string;

  return hash<string>()(val.str());
}

void swap(Shared_name& val1, Shared_name& val2)
{
  using std::swap;
  swap(val1.m_raw_name, val2.m_raw_name);
}

} // namespace ipc::util
