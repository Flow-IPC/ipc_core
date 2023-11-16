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

#include "ipc/util/util_fwd.hpp"

namespace ipc::util
{

// Types.

// Find doc headers near the bodies of these compound types.

class Shared_name;

// Free functions.

/**
 * Returns new object equal to `Shared_name(src1) /= src2`.
 *
 * @relatesalso Shared_name
 *
 * @param src1
 *        Object to precede the appended separator and `src2`.
 * @param src2
 *        Object to append after separator.
 * @return See above.
 */
Shared_name operator/(const Shared_name& src1, const Shared_name& src2);

/**
 * Returns new object equal to `Shared_name(src1) /= raw_src2`.
 *
 * @relatesalso Shared_name
 *
 * @tparam Source
 *         See Shared_name::operator/=() similar overload.
 * @param src1
 *        Object to precede the appended separator and `raw_src2`.
 * @param raw_src2
 *        String to append after separator.
 * @return See above.
 */
template<typename Source>
Shared_name operator/(const Shared_name& src1, const Source& raw_src2);

/**
 * Returns new object equal to `Shared_name(src1) /= raw_src2`.
 *
 * @relatesalso Shared_name
 *
 * @param src1
 *        Object to precede the appended separator and `raw_src2`.
 * @param raw_src2
 *        String to append after separator.
 * @return See above.
 */
Shared_name operator/(const Shared_name& src1, const char* raw_src2);

/**
 * Returns new object equal to `Shared_name(raw_src1) /= src2`.
 *
 * @relatesalso Shared_name
 *
 * @tparam Source
 *         See Shared_name::operator/=() similar overload.
 * @param raw_src1
 *        String to precede the appended separator and `src2`.
 * @param src2
 *        Object to append after separator.
 * @return See above.
 */
template<typename Source>
Shared_name operator/(const Source& raw_src1, const Shared_name& src2);

/**
 * Returns new object equal to `Shared_name(raw_src1) /= src2`.
 *
 * @relatesalso Shared_name
 *
 * @param raw_src1
 *        String to precede the appended separator and `src2`.
 * @param src2
 *        Object to append after separator.
 * @return See above.
 */
Shared_name operator/(const char* raw_src1, const Shared_name& src2);

/**
 * Returns new object equal to `Shared_name(src1) += src2`.
 *
 * @relatesalso Shared_name
 *
 * @param src1
 *        Object to precede the appended `src2`.
 * @param src2
 *        Object to append.
 * @return See above.
 */
Shared_name operator+(const Shared_name& src1, const Shared_name& src2);

/**
 * Returns new object equal to `Shared_name(src1) += raw_src2`.
 *
 * @relatesalso Shared_name
 *
 * @tparam Source
 *         See Shared_name::operator+=() similar overload.
 * @param src1
 *        Object to precede the appended `raw_src2`.
 * @param raw_src2
 *        String to append.
 * @return See above.
 */
template<typename Source>
Shared_name operator+(const Shared_name& src1, const Source& raw_src2);

/**
 * Returns new object equal to `Shared_name(src1) += raw_src2`.
 *
 * @relatesalso Shared_name
 *
 * @param src1
 *        Object to precede the appended `raw_src2`.
 * @param raw_src2
 *        String to append.
 * @return See above.
 */
Shared_name operator+(const Shared_name& src1, const char* raw_src2);

/**
 * Returns new object equal to `Shared_name(src2)` with `raw_src1` pre-pended to it.
 *
 * @relatesalso Shared_name
 *
 * @tparam Source
 *         See Shared_name::operator+=() similar overload.
 * @param raw_src1
 *        String to precede the appended `src2`.
 * @param src2
 *        Object to append.
 * @return See above.
 */
template<typename Source>
Shared_name operator+(const Source& raw_src1, const Shared_name& src2);

/**
 * Returns new object equal to `Shared_name(src2)` with `raw_src1` pre-pended to it.
 *
 * @relatesalso Shared_name
 *
 * @param raw_src1
 *        String to precede the appended `src2`.
 * @param src2
 *        Object to append.
 * @return See above.
 */
Shared_name operator+(const char* raw_src1, const Shared_name& src2);

/**
 * Prints embellished string representation of the given Shared_name to the given `ostream`.
 * @warning This is *not* equivalent to writing Shared_name::str(); as of this writing it includes not just
 *          `str()` but also the number of characters in it as a decimal and a separator, for convenience in
 *          test/debug, to visually detect names approaching certain length limits.  If you wish
 *          to output `val.str()`, then output... well... `val.str()`.
 *
 * @todo Does Shared_name `operator>>` and `operator<<` being asymmetrical get one into trouble when using
 * Shared_name with boost.program_options (or `flow::cfg` which is built on top of it)?  Look into it.  It may be
 * necessary to make `operator<<` equal to that of `ostream << string` after all; though the added niceties of the
 * current `<<` semantics may still at least be available via some explicit accessor.
 *
 * @relatesalso Shared_name
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Shared_name& val);

/**
 * Reads Shared_name from the given `istream`; equivalent to reading `string` into Shared_name::str().
 *
 * @relatesalso Shared_name
 *
 * @param is
 *        Stream to read.
 * @param val
 *        Object to which to deserialize.
 * @return `is`.
 */
std::istream& operator>>(std::istream& is, Shared_name& val);

/**
 * Returns `true` if and only if `val1.str() == val2.str()`.  Caution: this does not execute normalize() or anything
 * like that.
 *
 * @param val1
 *        Object to compare.
 * @param val2
 *        Object to compare.
 * @return See above.
 */
bool operator==(const Shared_name& val1, const Shared_name& val2);

/**
 * Negation of similar `==`.
 *
 * @param val1
 *        See `==`.
 * @param val2
 *        See `==`.
 * @return See above.
 */
bool operator!=(const Shared_name& val1, const Shared_name& val2);

/**
 * Returns `true` if and only if `val1.str() == string(val2)`.  Caution: this does not execute normalize() or anything
 * like that.
 *
 * @param val1
 *        Object to compare.
 * @param val2
 *        String to compare.
 * @return See above.
 */
bool operator==(const Shared_name& val1, util::String_view val2);

/**
 * Negation of similar `==`.
 *
 * @param val1
 *        See `==`.
 * @param val2
 *        See `==`.
 * @return See above.
 */
bool operator!=(const Shared_name& val1, util::String_view val2);

/**
 * Returns `true` if and only if `string(val1) == val2.str()`.  Caution: this does not execute normalize() or anything
 * like that.
 *
 * @param val1
 *        String to compare.
 * @param val2
 *        Object to compare.
 * @return See above.
 */
bool operator==(util::String_view val1, const Shared_name& val2);

/**
 * Negation of similar `==`.
 *
 * @param val1
 *        See `==`.
 * @param val2
 *        See `==`.
 * @return See above.
 */
bool operator!=(util::String_view val1, const Shared_name& val2);

/**
 * Returns `true` if and only if `val1.str() < val2.str()`.  Enables use of Shared_name in an `std::map` without
 * jumping through any special hoops.
 *
 * @param val1
 *        Object to compare.
 * @param val2
 *        Object to compare.
 * @return See above.
 */
bool operator<(const Shared_name& val1, const Shared_name& val2);

/**
 * Hasher of Shared_name for boost.unordered et al.
 *
 * @relatesalso Shared_name
 *
 * @param val
 *        Object to hash.
 * @return See above.
 */
size_t hash_value(const Shared_name& val);

/**
 * Swaps two objects.  Constant-time.  Suitable for standard ADL-swap pattern `using std::swap; swap(val1, val2);`.
 *
 * @relatesalso Shared_name
 *
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Shared_name& val1, Shared_name& val2);

/**
 * Utility that invokes `Persistent_object::for_each_persistent(name_prefix_or_empty)` and synchronously invokes
 * `Persistent_object::remove_persistent()` on each resulting item that passes the given filter,
 * where `Persistent_object` is a type that handles objects -- such as SHM pools or POSIX MQs -- with kernel-persistent
 * semantics.
 *
 * The number of items removed (without any error) is returned.
 * The nature of any error(s) encountered by individual `remove_persistent()` calls is ignored (not returned
 * in any way) except for logging.
 *
 * This "forgiving" error emission behavior is sufficient in many cases.  If you require finer control over
 * this please use `Persistent_object::for_each_persistent()` and `Persistent_object::remove_persistent()`
 * plus your own handling of the failure thereof in your custom `handle_name_func()`.
 *
 * @tparam Persistent_object
 *         See above.  It must have `static` methods `for_each_persistent()` and `remove_persistent()`
 *         with semantics identical to, e.g., shm::classic::Pool_arena versions of these methods.
 *         As of this writing this includes: shm::classic::Pool_arena, transport::Persistent_mq_handle (concept),
 *         transport::Posix_mq_handle, transport::Bipc_mq_handle (its impls).
 * @tparam Filter_func
 *         Function object with signature `bool F(const Shared_name&)`, which should return `true` to delete,
 *         `false` to skip.  For example it might simply check that `name` starts with a certain prefix
 *         (remove_each_persistent_with_name_prefix() uses this), or it might check whether the creating process --
 *         whose PID might be encoded in `name` by some convention (e.g., see ipc::session) -- is alive
 *         (util::process_running()).
 * @param logger_ptr
 *        Logger to use for subsequently logging.
 * @param filter_func
 *        See `Filter_func` above.
 * @return The number of successful `Persistent_object::remove_persistent()` calls invoked (might be zero).
 */
template<typename Persistent_object, typename Filter_func>
unsigned int remove_each_persistent_if(flow::log::Logger* logger_ptr, const Filter_func& filter_func);

/**
 * Utility that invokes remove_each_persistent_if() with the filter that returns `true` (yes, remove)
 * if and only if the item's name optionally matches a given Shared_name prefix.  (Optional in that
 * supplying an `.empty()` prefix deletes all items.)
 *
 * @tparam Persistent_object
 *         See remove_each_persistent_if().
 * @param logger_ptr
 *        See remove_each_persistent_if().
 * @param name_prefix_or_empty
 *        An object is removed only if its name starts with this value.  This filter is skipped if
 *        the value is `.empty()`.
 * @return See remove_each_persistent_if().
 */
template<typename Persistent_object>
unsigned int remove_each_persistent_with_name_prefix(flow::log::Logger* logger_ptr,
                                                     const Shared_name& name_prefix_or_empty);

} // namespace ipc::util
