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

#include <memory>

namespace ipc::test
{

/**
 * Shared pointer deleter that does nothing (e.g., doesn't delete the underlying pointer) as there is
 * no storage allocated for the pointer.
 */
template <class T>
struct Fake_object_deleter
{
  /**
   * The delete operator, which does nothing.
   *
   * @param obj The pointer to delete.
   */
  void operator()([[maybe_unused]] T* obj) const
  {
    // Intentionally do nothing as there is no memory allocated
  }
}; // struct Fake_object_deleter

template <class T>
std::shared_ptr<T> create_fake_shared_object(T* address)
{
  return std::shared_ptr<T>(static_cast<T*>(address), Fake_object_deleter<T>());
}

} // namespace ipc::test
