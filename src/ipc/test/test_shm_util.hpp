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

#pragma once

#include "ipc/util/shared_name.hpp"
#include <cstddef>

/* Test helpers for measuring how much RAM a SHM-pool actually takes ("commitment"), as opposed to its (possibly far
 * larger) size.  For example: verifying that a pool is sparse (pages committed only when first written), or that an
 * explicit commit-everything op did commit everything.
 *
 * How: In Linux a POSIX SHM-pool is a file in the tmpfs mounted at /dev/shm; and tmpfs charges a file exactly for
 * its allocated pages, which is what stat()'s `st_blocks` (in 512-byte units) reports.  ftruncate() charges
 * nothing; first-writing a page charges that page; posix_fallocate() charges the whole range.  (Note: mincore() is
 * not suitable for this: it reports pages as resident only once first touched in a certain way, which is
 * related but not the same.) */

namespace ipc::test
{

// Size in bytes of the system memory page; RAM is committed to a SHM-pool in units of this.
size_t page_sz();

/* Bytes of RAM currently committed to (charged to) the named SHM-pool, which must exist.
 * On failure (e.g., no such pool): reports a gtest failure and returns `size_t(-1)`. */
size_t shm_pool_committed_sz(const util::Shared_name& pool_name);

} // namespace ipc::test
