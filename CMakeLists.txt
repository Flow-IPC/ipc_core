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

cmake_minimum_required(VERSION 3.26.3) # See FlowLikeCodeGenerate.cmake for details.
# See that guy; it'll explain inside.  It mandates the following procedure and documents details.

set(PROJ "ipc_core")

message(CHECK_START "(Project [${PROJ}] root CMake script executing.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

set(PROJ_CAMEL "IpcCore")
set(PROJ_VERSION "1.0")
set(PROJ_HUMAN "Flow-IPC (Core)")

set(OS_SUPPORT_MSG "Only Linux is supported for now.  For ipc_core: macOS/BSD might require some coding; "
                      "Windows might require still more.")
# Subtext: Off the top of one's head: POSIX MQ support in Mac may need investigation; unavailable in Windows
# so needs to be not compiled.  Otherwise we might be all-right; but requires looking-into and testing.
# (That's after ensuring Flow is supported in the OS in question first.)

project(${PROJ_CAMEL} VERSION ${PROJ_VERSION} LANGUAGES CXX)

if(IPC_META_ROOT)
  message(VERBOSE
            "IPC_META_ROOT is set indicating we are a subdir of a meta-project.  Dependencies should be pre-loaded.")
  set(FLOW_LIKE_TOOLS "${IPC_META_ROOT_flow}/tools/cmake")
else()
  message(VERBOSE
            "IPC_META_ROOT not set; this indicates we are being distributed separately as opposed to "
              "in the meta-project.  We will load dependencies via find_package() from an external install-dir.")

  find_package(Flow 1.0 CONFIG REQUIRED)
  # That should have found (somewhere)/lib/cmake/Flow/FlowConfig.cmake based on the "Flow" token we gave it
  # and saved its location into ${Flow_DIR} (standard CMake technique); and we need
  # (somewhere)/share/flow/cmake/FlowLikeCodeGenerate.cmake; hence:
  set(FLOW_LIKE_TOOLS "${Flow_DIR}/../../../share/flow/cmake")
endif()

include("${FLOW_LIKE_TOOLS}/FlowLikeCodeGenerate.cmake")

# Tip: Most likely you are next interested in ./src/CMakeLists.txt and then perhaps ./test/{basic|suite}/CMakeLists.txt.

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done, success.)")
