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

# See FlowLikeLib.cmake per below; it'll explain inside.
# It mandates the following procedure and documents details.

# We just need executables to link Flow (and whatever it says it must link); and system rt library in the case
# of Linux (for shm_open() and such, at least).
#
# Beyond that we have nothing to add (even though we do use some of Flow's dependencies directly too; but
# by convention we're told not to list those redundantly).

message(STATUS "Dependents shall need to import/link libs: flow, (Linux only) rt.")
if(TARGET flow)
  set(DEP_LIBS flow) # In meta-project mode it's loaded by this short name.
else()
  set(DEP_LIBS Flow::flow) # In separate-project mode it's been install(EXPORT)ed with the namespace prefix.
endif()

if(LINUX)
  list(APPEND DEP_LIBS rt)
endif()

# Mirror Flow's find_package() call from our root CMakeLists.txt.  As directed omit "REQUIRED."
set(DEP_LIBS_PKG_ARG_LISTS "Flow 1.0 CONFIG")

# (See comment in FlowLikeLib.cmake if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.
set(SRCS
    ipc/common.cpp
    ipc/transport/asio_local_stream_socket.cpp
    ipc/transport/bipc_mq_handle.cpp
    ipc/transport/channel.cpp
    ipc/transport/detail/asio_local_stream_socket.cpp
    ipc/transport/detail/native_socket_stream_impl.cpp
    ipc/transport/error.cpp
    ipc/transport/native_socket_stream.cpp
    ipc/transport/native_socket_stream_acceptor.cpp
    ipc/transport/posix_mq_handle.cpp
    ipc/transport/protocol_negotiator.cpp
    ipc/transport/sync_io/detail/native_socket_stream_impl.cpp
    ipc/transport/sync_io/detail/native_socket_stream_impl_rcv.cpp
    ipc/transport/sync_io/detail/native_socket_stream_impl_snd.cpp
    ipc/transport/sync_io/native_socket_stream.cpp
    ipc/transport/sync_io/native_socket_stream_acceptor.cpp
    ipc/util/detail/util.cpp
    ipc/util/native_handle.cpp
    ipc/util/process_credentials.cpp
    ipc/util/shared_name.cpp
    ipc/util/sync_io/asio_waitable_native_hndl.cpp
    ipc/util/sync_io/detail/timer_ev_emitter.cpp
    ipc/util/use_counted_object.cpp
    ipc/util/util.cpp)
set(HDRS
    ipc/common.hpp
    ipc/detail/common.hpp
    ipc/detail/macros/log_component_enum_declare.macros.hpp
    ipc/transport/asio_local_stream_socket.hpp
    ipc/transport/asio_local_stream_socket_fwd.hpp
    ipc/transport/bipc_mq_handle.hpp
    ipc/transport/blob_stream_mq.hpp
    ipc/transport/blob_stream_mq_rcv.hpp
    ipc/transport/blob_stream_mq_snd.hpp
    # ipc/transport/blob_transport.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/channel.hpp
    ipc/transport/detail/asio_local_stream_socket.hpp
    ipc/transport/detail/asio_local_stream_socket_fwd.hpp
    ipc/transport/detail/blob_stream_mq_impl.hpp
    ipc/transport/detail/blob_stream_mq_rcv_impl.hpp
    ipc/transport/detail/blob_stream_mq_snd_impl.hpp
    ipc/transport/detail/native_socket_stream_impl.hpp
    ipc/transport/detail/transport_fwd.hpp
    ipc/transport/error.hpp
    # ipc/transport/native_handle_transport.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/native_socket_stream.hpp
    ipc/transport/native_socket_stream_acceptor.hpp
    # ipc/transport/persistent_mq_handle.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/posix_mq_handle.hpp
    ipc/transport/protocol_negotiator.hpp
    ipc/transport/sync_io/blob_stream_mq_rcv.hpp
    ipc/transport/sync_io/blob_stream_mq_snd.hpp
    # ipc/transport/sync_io/blob_transport.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/sync_io/detail/async_adapter_rcv.hpp
    ipc/transport/sync_io/detail/async_adapter_snd.hpp
    ipc/transport/sync_io/detail/blob_stream_mq_rcv_impl.hpp
    ipc/transport/sync_io/detail/blob_stream_mq_snd_impl.hpp
    ipc/transport/sync_io/detail/native_socket_stream_impl.hpp
    # ipc/transport/sync_io/native_handle_transport.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/sync_io/native_socket_stream.hpp
    ipc/transport/sync_io/native_socket_stream_acceptor.hpp
    ipc/transport/transport_fwd.hpp
    ipc/util/default_init_allocator.hpp
    ipc/util/detail/util.hpp
    ipc/util/detail/util_fwd.hpp
    ipc/util/native_handle.hpp
    ipc/util/process_credentials.hpp
    ipc/util/process_credentials_fwd.hpp
    ipc/util/shared_name.hpp
    ipc/util/shared_name_fwd.hpp
    ipc/util/sync_io/asio_waitable_native_hndl.hpp
    ipc/util/sync_io/detail/timer_ev_emitter.hpp
    ipc/util/sync_io/sync_io_fwd.hpp
    ipc/util/use_counted_object.hpp
    ipc/util/util_fwd.hpp)

include("${FLOW_LIKE_TOOLS}/FlowLikeLib.cmake")
