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

#include <ostream>

namespace ipc::util
{

// Types.

// Find doc headers near the bodies of these compound types.

class Process_credentials;

// Free functions.

/**
 * Checks for by-value equality between two Process_credentials objects.
 * process_invoked_as() does not participate in this and is not invoked.
 *
 * @relatesalso Process_credentials
 * @param val1
 *        Value to compare.
 * @param val2
 *        Value to compare.
 * @return Whether the accessors all compare equal.
 */
bool operator==(const Process_credentials& val1, const Process_credentials& val2);

/**
 * Checks for by-value inequality between two Process_credentials objects.
 * process_invoked_as() does not participate in this and is not invoked.
 *
 * @relatesalso Process_credentials
 * @param val1
 *        Value to compare.
 * @param val2
 *        Value to compare.
 * @return Whether at least one accessor compares unequal.
 */
bool operator!=(const Process_credentials& val1, const Process_credentials& val2);

/**
 * Prints string representation of the given util::Process_credentials to the given `ostream`.
 *
 * process_invoked_as() does not participate in this and is not invoked; you may query that information
 * if desired manually; just remember `val.process_id()` must be live at the time for it to work.
 *
 * @relatesalso util::Process_credentials
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Process_credentials& val);

} // namespace ipc::util
