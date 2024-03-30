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
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/array.hpp>

namespace ipc::util
{

// Initializers.

// Read the doc headers to understand why the numeric values below were chosen.
const boost::array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>
  SHARED_RESOURCE_PERMISSIONS_LVL_MAP
    = {
        Permissions(0), // <= NO_ACCESS
        Permissions(0b110000000), // <= USER_ACCESS.  Value a/k/a 0600.
        Permissions(0b110110000), // <= GROUP_ACCESS.  Value a/k/a 660.
        Permissions(0b110110110) // <= UNRESTRICTED.  Value a/k/a 666.
      };
const boost::array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>
  PRODUCER_CONSUMER_RESOURCE_PERMISSIONS_LVL_MAP
    = {
        Permissions(0), // <= NO_ACCESS
        /* Note: creator must be able to write, so no choice but to allow any app running-as that user to write.
         * Typically some trust is required that the consumer apps won't use this ability to, in fact, write. */
        Permissions(0b110000000), // <= USER_ACCESS.  Value a/k/a 0600.
        // Other users can read.
        Permissions(0b110100000), // <= GROUP_ACCESS.  Value a/k/a 0640.
        Permissions(0b110100100) // <= UNRESTRICTED.  Value a/k/a 0644.
      };

/**
 * Maps general Permissions_level specifier to low-level #Permissions value, when the underlying resource
 * is in the file-system (e.g., a file) and is *produced* (and therefore always writable) by the owning (creating,
 * updating) user; but *consumed* (and therefore at most readable, never writable) by potentially other processes,
 * and therefore possibly other users (e.g., group access or unrestricted access -- but only for reading either way).
 * For example a PID file is typically only writable by the daemon's user but may be readable or inaccessible by
 * other users.
 */
extern const boost::array<Permissions, size_t(Permissions_level::S_END_SENTINEL)>
  PRODUCER_CONSUMER_RESOURCE_PERMISSIONS_LVL_MAP;

const fs::path IPC_KERNEL_PERSISTENT_RUN_DIR = "/var/run";

// Implementations.

void pipe_produce(flow::log::Logger* logger_ptr, Pipe_writer* pipe)
{
  using util::Blob_const;

  /* As promised, we will block if the pipe is full.
   *
   * @todo It would still be nicer if we made pipe non-blocking and detected would-block here and at least
   * assert()ed in that case.  However their boost::asio::*able_pipe doesn't expose such a method
   * (in contrast to various sockets)... which is a bit odd/annoying actually.  We could use ::fcntl() but...
   * anyway, that's the to-do.  Actually we could perhaps use async_write_some() with, like, a timeout...
   * or something.... */

  const uint8_t PAYLOAD = '\0';

  Error_code sys_err_code;
  const bool ok = pipe->write_some(Blob_const(&PAYLOAD, 1), sys_err_code) == 1;

  if (sys_err_code || (!ok))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

    FLOW_LOG_FATAL("Pipe sync-write failed; this should never happen.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_FATAL();
    assert(false && "Pipe-write bug?"); std::abort();
    return;
  }
  // else
} // pipe_produce()

void pipe_consume(flow::log::Logger* logger_ptr, Pipe_reader* pipe)
{
  using util::Blob_mutable;

  // @todo See similar thing in pipe_produce().

  uint8_t payload;

  Error_code sys_err_code;
  const bool ok = pipe->read_some(Blob_mutable(&payload, 1), sys_err_code) == 1;

  if (sys_err_code || (!ok))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

    FLOW_LOG_FATAL("Pipe sync-read failed; this should never happen.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_FATAL();
    assert(false && "Pipe-read bug?"); std::abort();
    return;
  }
  // else
} // pipe_consume()

void remove_persistent_shm_pool(flow::log::Logger* logger_ptr, const Shared_name& pool_name, Error_code* err_code)
{
  using bipc::shared_memory_object;
  using boost::system::system_category;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { remove_persistent_shm_pool(logger_ptr, pool_name, actual_err_code); },
         err_code, "remove_persistent_shm_pool()"))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_UTIL);

  FLOW_LOG_INFO("SHM pool [" << pool_name << "]: Removing persistent SHM pool if possible.");
  const bool ok = shared_memory_object::remove(pool_name.native_str()); // Does not throw.

  if (ok)
  {
    err_code->clear();
    return;
  }
  /* smo::remove() is strangely gimped -- though I believe so are the other kernel-persistent remove()s
   * throughout bipc -- it does not throw and merely returns true or false and no code.  Odd, since there can
   * be at least a couple of reasons one would fail to delete....  However, in POSIX, the Boost 1.78 source code
   * shows that smo::remove() calls shared_memory_object::remove() which calls some internal
   * ipcdetail::delete_file() which calls... drumroll... freakin' ::unlink(const char*).  Hence we haxor: */
#ifndef FLOW_OS_LINUX // @todo Should maybe check Boost version or something too?
  static_assert(false, "Code in remove_persistent_shm_pool() relies on Boost invoking Linux unlink() with errno.");
#endif
  const auto& sys_err_code = *err_code = Error_code(errno, system_category());
  FLOW_ERROR_SYS_ERROR_LOG_WARNING();
} // remove_persistent_shm_pool()

} // namespace ipc::util
