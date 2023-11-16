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

#include <functional>
#include <ostream>
#include <regex>
#include <type_traits>
#include <ipc/util/process_credentials.hpp>

namespace ipc::test
{

/**
 * Returns the test name.
 *
 * @return See above.
 */
std::string get_test_suite_name();

/**
 * Executes a function and examines stream output for a match.
 *
 * @param func The function to execute.
 * @param os The stream to check output on.
 * @param regex_match The regular expression match (gtest format) to check in the stream output.
 * @param output_buffer Whether to output the contents of the buffer to the intended destination.
 *
 * @return Whether the output matched the expected regular expression.
 */
bool check_output(const std::function<void()>& func,
                  std::ostream& os,
                  const std::string& regex_match,
                  bool output_buffer = true);
/**
 * Executes a function and examines stream output for a match.
 *
 * @param func The function to execute.
 * @param os The stream to check output on.
 * @param regex_matches The regular expression matches (gtest format) to check in the stream output.
 * @param output_buffer Whether to output the contents of the buffer to the intended destination.
 *
 * @return Whether the output matched the expected regular expression.
 */
bool check_output(const std::function<void()>& func,
                  std::ostream& os,
                  const std::vector<std::string>& regex_matches,
                  bool output_buffer = true);
/**
 * Collects output during the execution of a function.
 *
 * @param func The function to execute.
 * @param os The stream to check output on.
 * @param output_buffer Whether to output the contents of the buffer to the intended destination.
 *
 * @return The collected output.
 */
std::string collect_output(const std::function<void()>& func, std::ostream& os, bool output_buffer = true);
/**
 * Returns the RSS memory used by the process.
 *
 * @return See above.
 */
size_t get_rss();
/**
 * Returns the effective process credentials of the running process initialized at first execution and remaining
 * unchanged for the remainder of the application.
 *
 * @return See above.
 */
const ipc::util::Process_credentials& get_process_creds();

/**
 * Casts an enumeration to its primitive type.
 *
 * @tparam Enum The enumeration type.
 * @param e The enumeration value.
 *
 * @return The primitive type form of the enumeration value.
 */
template <class Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
{
  return static_cast<std::underlying_type_t<Enum>>(e);
}

} // namespace ipc::test
