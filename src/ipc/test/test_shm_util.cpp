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

#include "ipc/test/test_shm_util.hpp"
#include <flow/common.hpp>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

#ifndef FLOW_OS_LINUX
static_assert(false, "These helpers rely on Linux's /dev/shm tmpfs semantics; not tested elsewhere.  Look into it "
                       "when porting.");
#endif

namespace ipc::test
{

size_t page_sz()
{
  return size_t(::sysconf(_SC_PAGESIZE));
}

size_t shm_pool_committed_sz(const util::Shared_name& pool_name)
{
  using std::string;

  // The pool's file-name in /dev/shm is its name sans the leading slash, if any, used in the shm_open() call.
  string file_name = pool_name.native_str();
  if ((!file_name.empty()) && (file_name.front() == '/'))
  {
    file_name.erase(0, 1);
  }
  const string path = "/dev/shm/" + file_name;

  struct ::stat st;
  if (::stat(path.c_str(), &st) != 0)
  {
    ADD_FAILURE() << "stat() of SHM-pool file [" << path << "] failed: [" << std::strerror(errno) << "].";
    return size_t(-1);
  }
  // else
  return size_t(st.st_blocks) * 512;
}

} // namespace ipc::test
