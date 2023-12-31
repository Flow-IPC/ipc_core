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

#include "ipc/test/test_config.hpp"
#include <boost/program_options.hpp>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

using std::string;
using std::vector;

namespace ipc::test
{

// Static constants
const string Test_config::S_HELP_PARAM("help");
const string Test_config::S_LOG_SEVERITY_PARAM("min-log-severity");

} // namespace ipc::test
