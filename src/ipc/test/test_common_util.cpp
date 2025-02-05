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

#include "ipc/test/test_common_util.hpp"
#include <gtest/gtest.h>
#include <flow/util/string_ostream.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <regex>
#include <fstream>

using std::string;
using std::stringstream;
using std::ostream;
using std::vector;

namespace ipc::test
{

const ipc::util::Process_credentials& get_process_creds()
{
  static const auto S_PROCESS_CREDS = ipc::util::Process_credentials::own_process_credentials();
  return S_PROCESS_CREDS;
}

} // namespace ipc::test
