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
#include "ipc/transport/asio_local_stream_socket.hpp"
#include "ipc/transport/error.hpp"
#include <flow/common.hpp>
#include <boost/array.hpp>

#ifndef FLOW_OS_LINUX // Sanity-re-check.  We'll be sending sockets through sockets, etc., which requires Linux.
static_assert(false, "Should not have gotten to this line; should have required Linux; this .cpp file assumes it.  "
                       "Might work in other POSIX OS (e.g., macOS) but must be checked/tested.");
#endif
#include <sys/types.h>
#include <sys/socket.h>

namespace ipc::transport::asio_local_stream_socket
{

// Free function implementations.

size_t nb_write_some_with_native_handle(flow::log::Logger* logger_ptr,
                                        Peer_socket* peer_socket_ptr,
                                        Native_handle payload_hndl, const util::Blob_const& payload_blob,
                                        Error_code* err_code)
{
  using boost::system::system_category;
  using boost::array;
  namespace sys_err_codes = boost::system::errc;
  using ::sendmsg;
  using ::msghdr;
  using ::iovec;
  using ::cmsghdr;
  // using ::SOL_SOCKET; // It's a macro apparently.
  using ::SCM_RIGHTS;
  using ::MSG_DONTWAIT;
  using ::MSG_NOSIGNAL;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, nb_write_some_with_native_handle,
                                     logger_ptr, peer_socket_ptr, payload_hndl, payload_blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  /* To reader/maintainer: The below isn't that difficult, but if you need exact understanding and definitely if you
   * plan to make changes -- even just clarifications or changes in the doc header -- then read the entire doc header
   * of nb_read_some_with_native_handle() first. */

  assert(peer_socket_ptr);
  auto& peer_socket = *peer_socket_ptr;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Connected local peer socket wants to write from location @ [" << payload_blob.data() << "] "
                 "plus native handle [" << payload_hndl << "].  Will try to send.");

  /* Let us send as much as possible of payload_blob; and the native socket payload_hndl.
   * As explicitly documented in boost.asio docs: it does not provide an API for the latter (fairly hairy
   * and Linux-specific) feature, but it is doable by using sendmsg() natively via peer_socket.native_handle()
   * which gets the native handle (a/k/a FD).
   *
   * Side note: After I (ygoldfel) had written this and async_write_with_native_handle() (which calls us),
   * I found validation for the approach in boost.asio docs in their example for Peer_socket::native_non_blocking(),
   * which shows how to implement async_sendfile(), spiritually similar to our thing which is essentially
   * a case of an analogous hypothetical async_sendmsg().  Subtlety: we, too, could have used native_non_blocking(),
   * as their example does; and the only reason we did not is that `sendmsg()` happens to support a per-call flag
   * that forces a non-blocking transmission, so one needn't deal with socket state.  Subtlety: realize (as per
   * boost.asio docs) that Peer_socket::non_blocking() is orthogonal to this entirely; it is orthogonal to
   * native_non_blocking() and the per-call flag thing. */

  iovec native_buf1 = { const_cast<void*>(payload_blob.data()), payload_blob.size() };
  // (const_cast<> is OK, as sendmsg() is read-only. Probably the API is const-janky due to iovec's reuse in recvmsg().)

  msghdr sendmsg_hdr =
  {
     0, // msg_name - Address; not used; we are connected.
     0, // msg_namelen - Ditto.
     /* msg_iov - Scatter/gather array.  We just put the one blob here.  It's natural to instead put a
      * a buffer sequence, meaning N>=1 buffers, here.  There's a to-do elsewhere as of this writing to
      * make that API as well; as of now it's not needed.  Take a look inside boost.asio for how they convert
      * between the 2; it's not rocket science, but we can either reuse their code or learn from it;
      * for example there's the matter of MAX_IOV (max size of this array). */
     &native_buf1,
     1, // msg_iovlen - # elements in msg_iov.

     /* The other 3 fields can be uninitialized: msg_flags is unused; msg_control[len] are set below.
      * The language won't let me leave them as garbage though so: */
     0, 0, 0
  };

  /* What's left is to set up msg_control[len] which is how we send the native handle (payload_hndl).
   * This is based on example snippet taken from Linux `man cmsg`.  FWIW it's rather difficult to figure out
   * how to write it from the rest of the documentation, which is likely complete and correct but opaque; the
   * example is invaluable.  Note we are particularly using the SOL_SOCKET/SCM_RIGHTS technique. */

  constexpr size_t N_PAYLOAD_FDS = 1;
  union
  {
    /* (Comment taken verbatim from aforementioned `man`.)
     * Ancillary data buffer, wrapped in a union in order to ensure it is suitably aligned.
     *
     * ...If I (ygoldfel) understand correctly, and I think I do, the idea is that:
     *   - msg_control points to a contiguous data area (ancillary data); which is split up into variable-length
     *     sub-areas, each of which starts with a header, in the form of a `cmsghdr`, which contains a couple of
     *     enum-like ints, a cmsg_len specifying the length of the rest of that sub-area (within the sequence),
     *     and then that area which contains the stuff to be transmitted (whose exact size/meaning depends on the
     *     enums; in our case it'll be native handles).
     *   - A key caveat is that in order to be properly interpreted, the header -- probably cast by the kernel/something
     *     to `cmsghdr` -- must begin on an aligned address.  So, in particular, if whatever comes before it
     *     (could be the preceding ancillary data sub-area, depending) ends just before an odd address, then
     *     the data sub-area must be a byte or a few to the right.
     *   - So this trick explicitly puts a cmsghdr there; so that its co-inhabitant m_buf (the actual area where we'll
     *     shove handles) will also begin on the same address/byte.
     *   - CMSG_ALIGN() is a Linux extension that might be usable to avoid having to use the trick, but seeing as
     *     how `man cmsg` in my Linux says it's not portable and itself foregoes it in favor of the trick in the
     *     example, I'll just also use the trick. */
    array<uint8_t, CMSG_SPACE(sizeof(Native_handle::handle_t) * N_PAYLOAD_FDS)> m_buf;
    cmsghdr m_align;
  } msg_control_as_union;

  sendmsg_hdr.msg_control = msg_control_as_union.m_buf.c_array();
  sendmsg_hdr.msg_controllen = sizeof(msg_control_as_union.m_buf);
  cmsghdr* const sendmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&sendmsg_hdr); // Or cast from &m_buf; or use &m_align....
  sendmsg_hdr_cmsg_ptr->cmsg_level = SOL_SOCKET;
  sendmsg_hdr_cmsg_ptr->cmsg_type = SCM_RIGHTS;
  sendmsg_hdr_cmsg_ptr->cmsg_len = CMSG_LEN(sizeof(Native_handle::handle_t) * N_PAYLOAD_FDS);

  // Copy the FDs.  We have just the one; simply assign (omit `memcpy`) but static_assert() to help future-proof.
  static_assert(N_PAYLOAD_FDS == 1, "Should be only passing one native handle into sendmsg() as of this writing.");
  *(reinterpret_cast<Native_handle::handle_t*>(CMSG_DATA(sendmsg_hdr_cmsg_ptr))) = payload_hndl.m_native_handle;

  const auto n_sent_or_error
    = sendmsg(peer_socket.native_handle(), &sendmsg_hdr,
              /* If socket is un-writable then don't block;
               * EAGAIN/EWOULDBLOCK instead.  Doing it this way is easier than messing with fcntl(), particularly
               * since we're working with a boost.asio-managed socket; that's actually fine -- there's a portable
               * API native_non_blocking() in boost.asio for setting this -- but feels like the less interaction between
               * portable boost.asio code we use and this native stuff, the better -- so just keep it local here. */
              MSG_DONTWAIT | MSG_NOSIGNAL);
              // ^-- Dealing with SIGPIPE is a pointless pain; if conn closed that way just give as an EPIPE error.

  if (n_sent_or_error == -1)
  {
    /* Not even 1 byte of the blob was sent; and hence nor was payload_hndl.  (Docs don't explicitly say that 2nd
     * part after the semicolon, but it's essentially impossible that it be otherwise, as then it'd be unknowable.
     * Update: Confirmed in kernel source and supported by this delightfully reassuring (in several ways) link:
     * [ https://gist.github.com/kentonv/bc7592af98c68ba2738f4436920868dc ] (Googled "SCM_RIGHTS gist").) */

    const Error_code sys_err_code(errno, system_category());
    if ((sys_err_code == sys_err_codes::operation_would_block) || // EWOULDBLOCK
        (sys_err_code == sys_err_codes::resource_unavailable_try_again)) // EAGAIN (same meaning)
    {
      FLOW_LOG_TRACE("Write attempt indicated would-block; not an error condition.  Nothing sent.");
      /* Subtlety: We could just set it to sys_err_code; but we specifically promised in contract we'd set it to the
       * boost::asio::error::would_block representation of would-block condition.  Why did we promise that?  2 reasons:
       * 1, that is what boost.asio's own spiritually similar Peer_socket::write_some() would do in non_blocking() mode.
       * 2, then we can promise a specific code instead of making them check for the above 2 errno values.
       *
       * Subtlety: net_flow::Peer_socket::sync_receive uses somewhat different semantics; it indicates would-block by
       * returning 0 *but* a falsy *err_code.  Why are we inconsistent with that?  Answer: Because net_flow is not
       * trying to be a boost.asio extension; we are.  In net_flow's context (as of this writing) no one is surprised
       * when semantics are somewhat different from boost.asio; but in our context they might be quite surprised
       * indeed. */
      *err_code = boost::asio::error::would_block;
      return 0;
    }
    // else

    assert(sys_err_code);

    // All other errors are fatal.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
    FLOW_LOG_WARNING("Connected local peer socket tried to write from "
                     "location @ [" << payload_blob.data() << "] plus native handle [" << payload_hndl << "]; "
                     "but an unrecoverable error occurred.  Nothing sent.");
    *err_code = sys_err_code;
    return 0;
  } // if (n_sent_or_error == -1)
  // else if (n_sent_or_error != -1)

  assert (n_sent_or_error > 0);

  FLOW_LOG_TRACE("sendmsg() reports the native handle [" << payload_hndl << "] was successfully sent; as "
                 "were [" << n_sent_or_error << "] of the blob's [" << payload_blob.size() << "] bytes.");
  err_code->clear();
  return n_sent_or_error;
} // nb_write_some_with_native_handle()

size_t nb_read_some_with_native_handle(flow::log::Logger* logger_ptr,
                                       Peer_socket* peer_socket_ptr,
                                       Native_handle* target_payload_hndl_ptr,
                                       const util::Blob_mutable& target_payload_blob,
                                       Error_code* err_code,
                                       int message_flags)
{
  using boost::system::system_category;
  using boost::array;
  namespace sys_err_codes = boost::system::errc;
  using ::sendmsg;
  using ::msghdr;
  using ::iovec;
  using ::cmsghdr;
  // using ::SOL_SOCKET; // It's a macro apparently.
  using ::SCM_RIGHTS;
  using ::MSG_DONTWAIT;
  using ::MSG_CTRUNC;
  // using ::errno; // It's a macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, nb_read_some_with_native_handle,
                                     logger_ptr, peer_socket_ptr, target_payload_hndl_ptr,
                                     target_payload_blob, _1, message_flags);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  /* To reader/maintainer: The below isn't that difficult, but if you need exact understanding and definitely if you
   * plan to make changes -- even just clarifications or changes in the doc header -- then read the entire doc header
   * of nb_read_some_with_native_handle() first. */

  assert(peer_socket_ptr);
  assert(target_payload_hndl_ptr);

  auto& peer_socket = *peer_socket_ptr;
  auto& target_payload_hndl = *target_payload_hndl_ptr;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Connected local peer socket wants to read up to [" << target_payload_blob.size() << "] bytes "
                 "to location @ [" << target_payload_blob.data() << "] "
                 "plus possibly a native handle.  Will try to receive.");
  target_payload_hndl = Native_handle();
  assert(target_payload_hndl.null()); // We promised to set to this if no socket received (including on error).

  /* Let us receive as much as possible into target_payload_blob up to its size; and a native socket (if any) into
   * target_payload_hndl (if none, then it'll remain null()).
   * As explicitly documented in boost.asio docs: it does not provide an API for the latter (fairly hairy
   * and Linux-specific) feature, but it is doable by using recvmsg() natively via peer_socket.native_handle()
   * which gets the native handle (a/k/a FD).
   *
   * Recommending first looking at nb_write_some_with_native_handle(), as we operate symmetrically/similarly. */

  iovec native_buf1 = { target_payload_blob.data(), target_payload_blob.size() };
  msghdr recvmsg_hdr =
  {
     0, // msg_name - Address; not used; we are connected.
     0, // msg_namelen - Ditto.
     /* msg_iov - Scatter/gather array.  We just put the one target blob here.  See similarly-themed notes at similar
      * spot in nb_write_some_with_native_handle(); they apply here too, more or less. */
     &native_buf1,
     1, // msg_iovlen - # elements in msg_iov.  This is an *input* arg only; output is total length read: the ret value.
     /* The following fields may or may need not be initialized; but let's discuss them generally:
      * msg_control, msg_controllen - These are out-args, indirectly set/reserved and interpreted with CMSG* below.
      * msg_flags - This is an out-arg containing special feature flags.  These may be checked below after call.
      * The language won't let me leave them as garbage though so: */
     0, 0, 0
  };
  // May not be needed, but at least some examples online do it; seems prudent and cheap.
  recvmsg_hdr.msg_flags = 0;

  /* Set up .msg_control*.  This, and then interpreting the output after the call, is based on the out-equivalent in
   * nb_write_some_with_native_handle() as well as cross-referencing with some Internet sources.
   *
   * Before the call, we must reserve space for the ancillary out-data, if any; set msg_control to point to that;
   * and msg_controllen to the size of that thing. */

  constexpr size_t N_PAYLOAD_FDS = 1;
  union
  {
    // See nb_write_some_with_native_handle().  It explains the m_align thing.
    array<uint8_t, CMSG_SPACE(sizeof(Native_handle::handle_t) * N_PAYLOAD_FDS)> m_buf;
    cmsghdr m_align;
  } msg_control_as_union;
  // Probably paranoia.  Just in case pre-fill it with zeroes (0x00... is not a valid FD) for sanity check later.
  msg_control_as_union.m_buf.fill(0);

  recvmsg_hdr.msg_control = msg_control_as_union.m_buf.c_array();
  recvmsg_hdr.msg_controllen = sizeof(msg_control_as_union.m_buf);

  const auto n_rcvd_or_error
    = recvmsg(peer_socket.native_handle(), &recvmsg_hdr,
              /* If socket is un-writable then don't block; EAGAIN/EWOULDBLOCK instead. ...Further comment omitted;
               * see sendmsg() elsewhere in this .cpp.  Same thing here. */
              MSG_DONTWAIT | message_flags); // <-- Re. message_flags please read notes in our doc header.

  // Carefully check all the outputs.  Return value first.

  if (n_rcvd_or_error == -1)
  {
    /* Not even 1 byte of a blob was read; and hence nor was any target_payload_hndl.  (See comment in similar
     * sport in nb_write_some_with_native_handle(); applies here equally.) */

    const Error_code sys_err_code(errno, system_category());
    if ((sys_err_code == sys_err_codes::operation_would_block) || // EWOULDBLOCK
        (sys_err_code == sys_err_codes::resource_unavailable_try_again)) // EAGAIN (same meaning)
    /* Reader: "But, ygoldfel, why didn't you just check errno itself against those 2 actual Evalues?"
     * ygoldfel: "Because it makes me feel better about portable-ish style.  Shut up, that's why.  Get off my lawn." */
    {
      FLOW_LOG_TRACE("Read attempt indicated would-block; not an error condition.  Nothing received.");
      // Subtlety x 2: ...omitted.  See similar spot in nb_write_some_with_native_handle().  Same here.
      *err_code = boost::asio::error::would_block;
      return 0; // target_payload_hndl already set.
    }
    // else

    assert(sys_err_code);

    // All other errors are fatal.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
    FLOW_LOG_WARNING("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes at "
                     "location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                     "but an unrecoverable error occurred.  Nothing received.");
    *err_code = sys_err_code;
    return 0; // target_payload_hndl already set.
  } // if (n_rcvd_or_error == -1)
  // else if (n_rcvd_or_error != -1)

  if (n_rcvd_or_error == 0)
  {
    /* WARNING doesn't feel right: it's a graceful connection end.
     * INFO could be good, but it might be too verbose depending on the application.
     * Use TRACE to be safe; caller can always log differently if desired. */
    FLOW_LOG_TRACE("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes at "
                   "location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                   "but it returned EOF meaning orderly connection shutdown by peer.  Nothing received.");
    *err_code = boost::asio::error::eof;
    return 0; // target_payload_hndl already set.
  }
  // else
  assert(n_rcvd_or_error > 0);

  /* Next, target_payload_blob... which is already written to (its first n_rcvd_or_error bytes).
   *
   * Next, recvmsg_hdr.msg_flags.  Basically only the following is relevant: */
  if (recvmsg_hdr.msg_flags != 0)
  {
    FLOW_LOG_INFO("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes at "
                  "location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                  "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also returned raw "
                  "out-flags value [0x" << std::hex << recvmsg_hdr.msg_flags << std::dec << "].  "
                  "Will check for relevant flags but otherwise "
                  "ignoring if nothing bad.  Logging at elevated level because it's interesting; please investigate.");

    if ((recvmsg_hdr.msg_flags & MSG_CTRUNC) != 0)
    {
      FLOW_LOG_WARNING("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes "
                       "at location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                       "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also returned raw "
                       "out-flags value [0x" << recvmsg_hdr.msg_flags << "] which includes MSG_CTRUNC.  "
                       "That flag indicates more stuff was sent as ancillary data; but we expect at most 1 native "
                       "handle.  Other side sent something strange.  Acting as if nothing received + error.");
      *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
      return 0; // target_payload_hndl already set.
    }
    // else
  }

  /* Lastly examine ancillary data (and note MSG_CTRUNC already eliminated above).
   * Use, basically, the method from `man cmsg` in Linux. */
  cmsghdr* const recvmsg_hdr_cmsg_ptr = CMSG_FIRSTHDR(&recvmsg_hdr);
  if (recvmsg_hdr_cmsg_ptr)
  {
    // There is some ancillary data.  It can only (validly according to our expected protocol) be one thing.
    if ((recvmsg_hdr_cmsg_ptr->cmsg_level == SOL_SOCKET) &&
        (recvmsg_hdr_cmsg_ptr->cmsg_type == SCM_RIGHTS))
    {
      static_assert(N_PAYLOAD_FDS == 1, "Should be only dealing with one native handle with recvmsg() "
                                        "as of this writing.");
      target_payload_hndl.m_native_handle
        = *(reinterpret_cast<const Native_handle::handle_t*>(CMSG_DATA(recvmsg_hdr_cmsg_ptr)));
    }
    else
    {
      FLOW_LOG_WARNING("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes "
                       "at location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                       "and it returned it read [" << n_rcvd_or_error << "] bytes successfully but also "
                       "unexpected ancillary data of csmg_level|cmsg_type "
                       "[" << recvmsg_hdr_cmsg_ptr->cmsg_level << '|' << recvmsg_hdr_cmsg_ptr->cmsg_type << "].  "
                       "Acting as if nothing received + error.");
      *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
      return 0; // target_payload_hndl already set.
    }
    // else
    if (CMSG_NXTHDR(&recvmsg_hdr, recvmsg_hdr_cmsg_ptr))
    {
      /* This is rather strange: we didn't provide enough space for more ancillary data; yet there is somehow more,
       * even though we didn't detect MSG_CTRUNC earlier.  Well, whatever.  It's bad just like MSG_CTRUNC. */
      FLOW_LOG_WARNING("Connected local peer socket tried to read up to [" << target_payload_blob.size() << "] bytes "
                       "at location @ [" << target_payload_blob.data() << "] plus possibly a native handle; "
                       "and it returned it read [" << n_rcvd_or_error << "] bytes and native handle "
                       "[" << target_payload_hndl << "] but also more ancillary data; but we expect at most 1 native "
                       "handle.  Other side sent something strange.  Acting as if nothing received + error.");
      target_payload_hndl = Native_handle(); // Undo the above (to match promised semantics on error).
      *err_code = error::Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL;
      return 0;
    }
    // else { No more ancillary data, as expected. }
  }
  // else { No ancillary data, meaning no native handle; that's quite normal. }

  FLOW_LOG_TRACE("recvmsg() reports receipt of possible native handle [" << target_payload_hndl << "]; as well as "
                 "[" << n_rcvd_or_error << "] of the blob's [" << target_payload_blob.size() << "]-byte capacity.");
  err_code->clear();
  return n_rcvd_or_error;
} // nb_read_some_with_native_handle()

void release_native_peer_socket(Native_handle&& peer_socket_native_or_null)
{
  using flow::util::Task_engine;

  // As promised:
  if (peer_socket_native_or_null.null())
  {
    return;
  }
  // else

  /* Purely for style reasons let's wrap it in a boost.asio `socket` and let its dtor
   * take care of it.  ::close() would have worked too, but I suppose this is less "native" and more
   * consistent. */
  Task_engine task_engine;
  [[maybe_unused]] Peer_socket sock(task_engine, Protocol(), peer_socket_native_or_null.m_native_handle);

  peer_socket_native_or_null = Native_handle(); // As promised nullify it.

  // Now destroy `sock`, then destroy task_engine.
} // release_native_peer_socket()

// Opt_peer_process_credentials implementations.

Opt_peer_process_credentials::Opt_peer_process_credentials() = default;
Opt_peer_process_credentials::Opt_peer_process_credentials(const Opt_peer_process_credentials&) = default;
Opt_peer_process_credentials& Opt_peer_process_credentials::operator=(const Opt_peer_process_credentials&) = default;

} // namespace ipc::transport::asio_local_stream_socket
