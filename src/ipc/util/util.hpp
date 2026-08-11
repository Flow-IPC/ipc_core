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

// Template implementations.

template<typename T, typename... Ctor_args>
void construct_at(T* obj, Ctor_args&&... ctor_args)
{
  using Value = T;

  // Use placement-new expression used by C++20's construct_at() per cppreference.com.
  ::new (const_cast<void*>
           (static_cast<void const volatile*>
              (obj)))
    Value(std::forward<Ctor_args>(ctor_args)...);
}

} // namespace ipc::util

namespace ipc::util::stat
{

// Types.

/**
 * Knobs meant to control the cosmetics of printing (`ostream <<`) a bundling of stats/info for some particular
 * purpose -- whether a snapshot `struct` (e.g., ipc::shm::arena_lend::jemalloc::stat::Arena_info_dump) or a live
 * accessor bundle (e.g., ipc::session::Info_collector).
 *
 * ### Rationale ###
 * This is an optional convenience thing: We figured various info-dump ops like the above example will tend to
 * have at least some knobs in common; so standardizing them in a simple `struct` is nice for convenience/reduced
 * boiler-plate and nice for keeping patterns generic.
 */
struct Info_dump_format
{
  // Data.

  /**
   * If `true`: Print everything you've got; if `false`: refrain from producing multi-page output if possible.
   *
   * Default: `true`.
   */
  bool m_verbose = true;

  /**
   * If `true`: Use newlines where appropriate for readability (except at the very end of the output); if `false`:
   * avoid newlines entirely, if at all possible.
   *
   * Default: `true`.
   *
   * If #m_multiline comes in conflict with #m_verbose then `m_multiline` wins.  That is: Observing
   * `m_multiline == false` if first-priority, whereas observing `m_verbose == true` is secondary/best-effort.
   *
   * ### Example of conflict scenario ###
   * In the SHM-jemalloc module (ipc::shm::arena_lend::jemalloc), #m_verbose = `true` means including jemalloc
   * memory-manager's own print-stats-dump output, which is pages and pages of tables and readouts (lots of newlines
   * and other formatting included); `false` means omit it: print only other carefully curated stats but avoid the
   * jemalloc-dump.
   *
   * `m_verbose = true` and `m_multiline = true` is no problem.  `m_verbose = false` and `m_multiline = false`:
   * also no problem.  `m_verbose = false` and `m_multiline = true`: ditto.  However:
   *
   * `m_verbose = true` and `m_multiline = false` presents a conflict: The jemalloc-dump must include newlines;
   * erasing them would make the tables unreadable; could in some jemalloc-versions use JSON-output, but JSON
   * is not human-readable in this context; so the bottom line is can't be done.  So: `m_multiline = false` wins.
   * `m_verbose = true` is to be ignored; skip the jemalloc-dump.
   */
  bool m_multiline = true;
}; // struct Info_dump_format

} // namespace ipc::util::stat
