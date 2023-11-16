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
#include <cctype>

namespace ipc::util
{

// Initializers.

/* Underscores are allowed for all applicable shared resource types.  Ideally we'd use them to separate words, but 2
 * factors are responsible for making _ the folder separator and using camelCase to separate words between pairs of
 * nearby underscores: 1, characters are at a premium against S_MAX_LENGTH, so we shouldn't waste them on cosmetic
 * concerns if possible; and 2, it's unclear what other special characters (not an alphanumeric) would be suitable. */
const char Shared_name::S_SEPARATOR = '_';

// See our doc header for discussion of chosen value.
const size_t Shared_name::S_MAX_LENGTH = 75;

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

Shared_name& Shared_name::operator+=(const char* raw_name_to_append)
{
  // Existence/impl rationale: This is faster than if they had to: `+= Shared_name::ct(raw_name_to_append)`.

  m_raw_name += raw_name_to_append;
  return *this;
}

Shared_name& Shared_name::operator+=(const Shared_name& to_append)
{
  using util::String_view;
  return operator+=(String_view(to_append.str()));
}

Shared_name& Shared_name::operator/=(const char* raw_name_to_append)
{
  // Existence/impl rationale: This is faster than if they had to: `/= Shared_name::ct(raw_name_to_append)`.

  using util::String_view;
  return operator/=(String_view(raw_name_to_append)); // Requires a strlen() internally but gets .reserve() in return.
}

Shared_name& Shared_name::operator/=(const Shared_name& to_append)
{
  using util::String_view;
  return operator/=(String_view(to_append.str()));
}

Shared_name operator+(const Shared_name& src1, const char* raw_src2)
{
  // Existence/impl rationale: This is faster than if they had to: `src1 + Shared_name::ct(raw_src2)`.

  using util::String_view;
  return operator+(src1, String_view(raw_src2)); // Requires a strlen() internally but gets .reserve() in return.
}

Shared_name operator+(const char* raw_src1, const Shared_name& src2)
{
  // Existence rationale: For symmetry with overload: (src1, raw_src2).

  using util::String_view;
  return operator+(String_view(raw_src1), src2);
}

Shared_name operator+(const Shared_name& src1, const Shared_name& src2)
{
  using util::String_view;
  return src1 + String_view(src2.str());
}

Shared_name operator/(const Shared_name& src1, const char* raw_src2)
{
  // Existence/impl rationale: This is faster than if they had to: `src1 / Shared_name::ct(raw_src2)`.

  using util::String_view;
  return operator/(src1, String_view(raw_src2));
}

Shared_name operator/(const char* raw_src1, const Shared_name& src2)
{
  // Existence rationale: For symmetry with overload: (src1, raw_src2).

  using util::String_view;
  return operator/(String_view(raw_src1), src2);
}

Shared_name operator/(const Shared_name& src1, const Shared_name& src2)
{
  return Shared_name(src1) /= src2;
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
  using std::isalnum;

  // Keep in sync with sanitize()!

  if (size() > S_MAX_LENGTH)
  {
    return false;
  }
  // else

  /* Following is self-explanatory given the contract.  It could be made much briefer, probably, by using built-in
   * string functions and/or string_algo.  However, for max perf, we'd rather do only one linear-time scan.
   * Finally, could other characters be potentially allowed by the various sys calls?  Maybe, but we'd rather be safe
   * and sorry in terms of portability, and also these chars should be aesthetically good as a convention. */

  bool prev_is_sep = false;
  for (const auto ch : m_raw_name)
  {
    bool is_sep = ch == S_SEPARATOR;
    if ((!is_sep) && (!isalnum(ch))) // Note: isalnum() explicitly checks for [A-Za-z0-9]; no internationalized stuff.
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

  // SEPS are the 2 allowed input separator characters.  They must be different.  Try to keep this compile-time.
  constexpr char SEPARATOR_ALT = '/';
  static_assert(S_SEPARATOR != SEPARATOR_ALT,
                "The real separator and the alt must be different characters.");
  constexpr const char SEPS[] = { SEPARATOR_ALT, S_SEPARATOR, '\0' };

  /* The following implementation, while self-explanatory, foregoes briefer approaches in favor of ~max perf.
   * This includes (but isn't limited to) re-implementing sanitized() functionality instead of just returning
   * actual sanitized() at the end, all so we can limit to just one linear scan of m_raw_name. */

  // Keep in sync with sanitized()!

  if (empty()) // Get rid of degenerate case for simplicity of below.
  {
    return true;
  }
  // else at least one character to read and (unless it's illegal) write.

  string orig_backup_or_empty_if_unchanged; // Unused until !empty().

  // Convenience function (should get inlined).  At worst, it's constant time + a dealloc of m_raw_name.
  const auto undo_changes_pre_return_false_func = [&]()
  {
    if (!orig_backup_or_empty_if_unchanged.empty())
    {
      // Sanitizations had been done to m_raw_name.  So undo all of it (constant-time move; dealloc).
      m_raw_name = std::move(orig_backup_or_empty_if_unchanged);
    }
    // else { No sanitizations had been done so far to m_raw_name, so nothing to undo. }
  };

  const size_t n = size();
  size_t out_pos = 0;
  for (size_t in_pos = 0; in_pos != n;
       ++in_pos, ++out_pos)
  {
    /* We cannot predict, at the start, whether we'll ultimately result in a too-long string, because repeating
     * separators can actually lower the length.  However, if we get to this iteration, then we *will* be writing a
     * character (unless ch is illegal, but then we'd return false anyway); hence we can detect ASAP whether the
     * output is too long for sure. */
    if (out_pos >= S_MAX_LENGTH)
    {
      undo_changes_pre_return_false_func();
      return false;
    }
    // else not too long yet.

    auto ch = m_raw_name[in_pos];
    bool is_last = in_pos == n - 1; // Attn: intentionally not `const` (in_pos may change below).
    const bool is_sep = (ch == S_SEPARATOR) || (ch == SEPARATOR_ALT);

    // Perform nearly identical check to sanitized() (reminder: the 2 functions must remain in sync).
    if ((!is_sep) && (!isalnum(ch))) // Note: isalnum() explicitly checks for [A-Za-z0-9]; no i18n stuff going on.
    {
      undo_changes_pre_return_false_func();
      return false;
    }
    // else { Legal character ch, though it may be sanitized by replacing with a different value. }

    if (is_sep)
    {
      ch = S_SEPARATOR; // Namely, all separator possibilities are normalized to this one.

      /* Since we found a separator, immediately skip past any more adjacent separators by moving in_pos
       * inside this { body }.  Set it to the position just before the first non-separator.  That ranges between
       * not touching in_pos at all (i.e., only one separator after all); and setting it to (n - 1) (meaning either
       * we're at end of input string, or the rest of it is all separators).  Note for() will ++in_pos right after this.
       *
       * out_pos doesn't change; we are scanning string and writing it out at the same time. */
      if (!is_last)
      {
        in_pos = m_raw_name.find_first_not_of(SEPS, in_pos + 1);
        if (in_pos == string::npos)
        {
          in_pos = n - 1;
          is_last = true; // We updated in_pos; so update this accordingly.
        }
        else
        {
          --in_pos;
          assert(in_pos != (n - 1));
          assert(!is_last);
        }
      }
    } // if (is_sep)
    // in_pos and is_last are finalized.

    // Actually write out the character (though no need if it would result in no change).
    if (m_raw_name[out_pos] != ch)
    {
      /* Make backup if this is the very first modification we're going to make.
       * As a little optimization, skip that, if there are no characters left to scan (and thus write); as then
       * there is no chance left of `return false`, hence no undo would occur. */
      if (orig_backup_or_empty_if_unchanged.empty() && (!is_last))
      {
        // Alloc + linear-time copy; as advertised in contract this is a possible perf cost.
        orig_backup_or_empty_if_unchanged = m_raw_name;
      }
      m_raw_name[out_pos] = ch;
    }
  } // for ({in|out}_pos in [0, size())) - But note in_pos can skip more, inside { body }.

  // Passed gauntlet in terms of whether we can `return true` (we can and must).  Finish up.
  m_raw_name.erase(out_pos); // Constant-time.  Possible no-op.
  return true;
  /* If !orig_backup_or_empty_if_unchanged.empty(), it is now deallocated.
   * If we'd returned false with !orig_backup_or_empty_if_unchanged.empty(), then orig_backup_or_empty_if_unchanged
   * would've been swapped with m_raw_name, and the latter's buffer would be deallocated instead.  Either way,
   * as we'd promised in contract, overall it's 1 alloc, 1 copy of m_raw_name, 1 dealloc, and some constant-time
   * stuff.  (And if orig_backup_or_empty_if_unchanged.empty(), then even those are skipped.) */
} // Shared_name::sanitize()

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
} // Shared_name::build_conventional_non_session_based_shared_name()

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
  val = Shared_name::ct(std::move(str));
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
  return std::operator==(String_view(val1.str()), val2);
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
