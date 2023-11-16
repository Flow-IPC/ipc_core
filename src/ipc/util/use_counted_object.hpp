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

#include <cassert>

namespace ipc::util
{

/// Simple counter that manually tracks utilization. It is not thread-safe.
class Use_counted_object
{
public:

  // Constructors/destructor.

  /// Constructor.
  Use_counted_object();

  // Methods.

  /**
   * Returns the current usage.
   * @return See above.
   */
  unsigned int get_use_count() const;

  /// Increments the usage.
  void increment_use();

  /// Decrements the usage.  The current count must be greater than one.
  void decrement_use();

private:
  // Data.

  /// The current usage count.
  unsigned int m_use_count;
}; // class Use_counted_object

} // namespace ipc::util
