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

#include "ipc/transport/test/persistent_mq_handle_test.hpp"
#include "ipc/transport/posix_mq_handle.hpp"

namespace ipc::transport::test
{

TEST(Posix_mq_handle_test, lifecycle)
{
  persistent_mq_handle_lifecycle_test<Posix_mq_handle>();
}

TEST(Posix_mq_handle_test, move_and_swap)
{
  persistent_mq_handle_move_and_swap_test<Posix_mq_handle>();
}

TEST(Posix_mq_handle_test, transmission_nb)
{
  persistent_mq_handle_transmission_nb_test<Posix_mq_handle>();
}

TEST(Posix_mq_handle_test, blocking)
{
  persistent_mq_handle_blocking_test<Posix_mq_handle>();
}

TEST(Posix_mq_handle_test, interruption)
{
  persistent_mq_handle_interruption_test<Posix_mq_handle>();
}

} // namespace ipc::transport::test
