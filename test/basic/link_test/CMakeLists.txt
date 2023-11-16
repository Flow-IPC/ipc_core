# Flow-IPC: Core
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

set(NAME ipc_core_link_test.exec)
add_executable(${NAME} main.cpp)

# Do stuff we've resolved to do on all our targets.
common_set_target_properties(${NAME})

target_compile_options(${NAME} PRIVATE ${WARN_FLAGS})

# Link good ol' libipc_core.
target_link_libraries(${NAME} PRIVATE ${PROJ})

# Might as well export if they `make install` or equivalent.
install(TARGETS ${NAME}
        RUNTIME DESTINATION bin)
