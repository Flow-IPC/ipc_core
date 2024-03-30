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
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>
#include <flow/common.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/move/make_unique.hpp>

namespace ipc::transport
{

// Initializers.

const Shared_name Bipc_mq_handle::S_RESOURCE_TYPE_ID = Shared_name::ct("bipcQ");

// Implementations.

Bipc_mq_handle::Bipc_mq_handle() = default;

template<typename Mode_tag>
Bipc_mq_handle::Bipc_mq_handle(Mode_tag mode_tag, flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                               size_t max_n_msg, size_t max_msg_sz,
                               const util::Permissions& perms,
                               Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_absolute_name(absolute_name_arg),
  m_interrupting_snd(false),
  m_interrupting_rcv(false)
{
  using flow::log::Sev;
  using boost::io::ios_all_saver;
  using boost::movelib::make_unique;
  using bipc::message_queue;

  assert(max_n_msg >= 1);
  assert(max_msg_sz >= 1);

  static_assert(std::is_same_v<Mode_tag, util::Create_only> || std::is_same_v<Mode_tag, util::Open_or_create>,
                "Can only delegate to this ctor with Mode_tag = Create_only or Open_or_create.");
  constexpr char const * MODE_STR = std::is_same_v<Mode_tag, util::Create_only>
                                      ? "create-only" : "open-or-create";

  if (get_logger()->should_log(Sev::S_TRACE, get_log_component()))
  {
    ios_all_saver saver(*(get_logger()->this_thread_ostream())); // Revert std::oct/etc. soon.
    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("Bipc_mq_handle [" << *this << "]: Constructing MQ handle to MQ at name [" << absolute_name() << "] in "
       "[" << MODE_STR << "] mode; max msg size [" << max_msg_sz << "] x [" << max_n_msg << "] msgs; "
       "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].");
  }

  /* m_mq is null.  Try to create/create-or-open it; it may throw exception; this will do the right thing including
   * leaving m_mq at null, as promised, on any error.  Note we might throw exception because of this call. */
  op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle(): bipc::message_queue()",
                                     [&]()
  {
    m_mq = make_unique<message_queue>(mode_tag, absolute_name().native_str(), max_n_msg, max_msg_sz, perms);
    /* Bonus: All bipc CREATE/OPEN_OR_CREATE guys take care to ensure permissions are set regardless of umask,
     * so no need for us to set_resource_permissions() here. */
  });
} // Bipc_mq_handle::Bipc_mq_handle(Mode_tag)

Bipc_mq_handle::Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                               util::Create_only, size_t max_n_msg, size_t max_msg_sz,
                               const util::Permissions& perms, Error_code* err_code) :
  Bipc_mq_handle(util::CREATE_ONLY, logger_ptr, absolute_name_arg, max_n_msg, max_msg_sz, perms, err_code)
{
  // Cool.
}

Bipc_mq_handle::Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                               util::Open_or_create, size_t max_n_msg, size_t max_msg_sz,
                               const util::Permissions& perms, Error_code* err_code) :
  Bipc_mq_handle(util::OPEN_OR_CREATE, logger_ptr, absolute_name_arg, max_n_msg, max_msg_sz, perms, err_code)
{
  // Cool.
}

Bipc_mq_handle::Bipc_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                               util::Open_only, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_absolute_name(absolute_name_arg),
  m_interrupting_snd(false),
  m_interrupting_rcv(false)
{
  using boost::movelib::make_unique;
  using bipc::message_queue;

  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Constructing MQ handle to MQ at name "
                 "[" << absolute_name() << "] in open-only mode.");

  /* m_mq is null.  Try to create it; it may throw exception; this will do the right thing including
   * leaving m_mq at null, as promised, on any error.  Note we might throw exception because of this call. */
  op_with_possible_bipc_mq_exception(err_code,  "Bipc_mq_handle(): bipc::message_queue(OPEN_ONLY)",
                                     [&]()
  {
    m_mq = make_unique<message_queue>(util::OPEN_ONLY, absolute_name().native_str());
  });
} // Bipc_mq_handle::Bipc_mq_handle(Open_only)

// Just move-construct m_mq, m_absolute_name, and Log_holder.
Bipc_mq_handle::Bipc_mq_handle(Bipc_mq_handle&&) = default;

Bipc_mq_handle::~Bipc_mq_handle()
{
  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Closing MQ handle (already null? = [" << (!m_mq) << "]).");
}

// Just do m_mq = std::move(src.m_mq) and same with m_absolute_name and Log_holder.
Bipc_mq_handle& Bipc_mq_handle::operator=(Bipc_mq_handle&& src) = default;

void swap(Bipc_mq_handle& val1, Bipc_mq_handle& val2)
{
  using flow::log::Log_context;
  using std::swap;

  // This is a bit faster than un-specialized std::swap() which would require a move ction + 2 move assignments.

  swap(static_cast<Log_context&>(val1), static_cast<Log_context&>(val2));
  swap(val1.m_mq, val2.m_mq);
  swap(val1.m_absolute_name, val2.m_absolute_name);
}

size_t Bipc_mq_handle::max_msg_size() const
{
  assert(m_mq && "As advertised: max_msg_size() => undefined behavior if not successfully cted or was moved-from.");
  return m_mq->get_max_msg_size();
} // Bipc_mq_handle::max_msg_size()

size_t Bipc_mq_handle::max_n_msgs() const
{
  assert(m_mq && "As advertised: max_n_msgs() => undefined behavior if not successfully cted or was moved-from.");
  return m_mq->get_max_msg();
} // Bipc_mq_handle::max_msg_size()

bool Bipc_mq_handle::try_send(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Bipc_mq_handle::try_send, flow::util::bind_ns::cref(blob), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert(m_mq && "As advertised: try_send() => undefined behavior if not successfully cted or was moved-from.");

  bool not_blocked;
  op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle::try_send(): bipc::message_queue::try_send()",
                                     [&]()
  {
    auto blob_data = blob.data();
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                   "size [" << blob.size() << "].");
    if (blob.size() == 0)
    {
      /* bipc::message_queue::try_send() invokes memcpy(X, nullptr, N), even when N == 0;
       * which is (1) empirically speaking harmless but (2) technically in violation of arg 2's non-null decl
       * hence (3) causes a clang UBSAN sanitizer error.  So waste a couple cycles by feeding it this dummy
       * non-null value. */
      blob_data = static_cast<const void*>(&blob_data);
    }
    else // if (blob.size() != 0)
    {
      FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(blob, "  ") << "].");
    }

    not_blocked
      = m_mq->try_send(blob_data, blob.size(), 0); // Throws <=> error wrapper sets truthy *err_code.
    if (!not_blocked)
    {
      FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                     "size [" << blob.size() << "]: would-block.");
    }
  }); // op_with_possible_bipc_mq_exception()
  if (*err_code) // It logged if truthy.
  {
    return false; // not_blocked is garbage.
  }
  // else

  return not_blocked; // Logged about it already.
} // Bipc_mq_handle::try_send()

void Bipc_mq_handle::send(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { send(blob, actual_err_code); },
         err_code, "Bipc_mq_handle::send()"))
  {
    return;
  }
  // else
  err_code->clear();

  assert(m_mq && "As advertised: send() => undefined behavior if not successfully cted or was moved-from.");

  auto blob_data = blob.data();
  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-push of blob @[" << blob_data << "], "
                 "size [" << blob.size() << "].  Trying nb-push first; if it succeeds -- great.  "
                 "Else will wait/retry/wait/retry/....");
  if (blob.size() == 0)
  {
    // See similarly-placed comment in try_send() which explains this.
    blob_data = static_cast<const void*>(&blob_data);
  }
  else // if (blob.size() != 0)
  {
    FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(blob, "  ") << "].");
  }

  /* We could just invoke blocking m_mq->send(), but to get the promised logging we go a tiny bit fancier as follows.
   * Update: Now that we have to be interrupt_*()ible, also reuse wait_*() instead of using native
   * m_mq->*(). */

  bool ok;
  while (true)
  {
    op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle::send(): bipc::message_queue::try_send()",
                                       [&]()
    {
      ok = m_mq->try_send(blob_data, blob.size(), 0); // Throws <=> error wrapper sets truthy *err_code.
    });
    if (*err_code || ok)
    {
      // Threw => true error => *err_code set; get out.  Didn't throw and returned true => success; get out.
      return;
    }
    // else if (would-block): as promised, INFO logs.

    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                   "size [" << blob.size() << "]: would-block.  Executing blocking-wait.");

    wait_sendable(err_code);
    if (*err_code)
    {
      // Whether interrupted or true error, we're done.
      return;
    }
    // else
    FLOW_LOG_TRACE("Blocking-wait reported transmissibility.  Retrying.");
  } // while (true)
} // Bipc_mq_handle::send()

bool Bipc_mq_handle::timed_send(const util::Blob_const& blob, util::Fine_duration timeout_from_now,
                                Error_code* err_code)
{
  using flow::util::time_since_posix_epoch;
  using flow::util::buffers_dump_string;
  using flow::Fine_clock;
  using boost::chrono::round;
  using boost::chrono::microseconds;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Bipc_mq_handle::timed_send,
                                     flow::util::bind_ns::cref(blob), timeout_from_now, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // Similar to a combo of (blocking) send() and (non-blocking) try_send() -- keeping comments light where redundant.

  assert(m_mq && "As advertised: timed_send() => undefined behavior if not successfully cted or was moved-from.");

  auto blob_data = blob.data();
  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-timed-push of blob @[" << blob_data << "], "
                 "size [" << blob.size() << "]; timeout ~[" << round<microseconds>(timeout_from_now) << "].  "
                 "Trying nb-push first; if it succeeds -- great.  Else will wait/retry/wait/retry/....");
  if (blob.size() == 0)
  {
    // See similarly-placed comment in try_send() which explains this.
    blob_data = static_cast<const void*>(&blob_data);
  }
  else // if (blob.size() != 0)
  {
    FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(blob, "  ") << "].");
  }

  auto now = Fine_clock::now();
  auto after = now;
  bool ok;

  while (true)
  {
    op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle::timed_send(): bipc::message_queue::try_send()",
                                       [&]()
    {
      ok = m_mq->try_send(blob_data, blob.size(), 0); // Throws <=> error wrapper sets truthy *err_code.
    });
    if (*err_code)
    {
      // Threw => true error => *err_code set; get out.
      return false;
    }
    // else if (would-block or success):
    if (ok)
    {
      break; // Instant success.
    }
    // else if (would-block): as promised, INFO logs.

    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                   "size [" << blob.size() << "]: would-block.  Executing blocking-wait.");

    timeout_from_now -= (after - now); // No-op the first time; after that reduces time left.
    const bool ready = timed_wait_sendable(timeout_from_now, err_code);
    if (*err_code)
    {
      // Whether interrupted or true error, we're done.
      return false;
    }
    // else:

    if (!ready) // I.e., if (timed out).
    {
      FLOW_LOG_TRACE("Did not finish before timeout.");
      return false;
    }
    // else: successful wait for transmissibility.  Try nb-transmitting again.
    FLOW_LOG_TRACE("Blocking-wait reported transmissibility.  Retrying.");

    after = Fine_clock::now();
    assert((after >= now) && "Fine_clock is supposed to never go backwards.");
  } // while (true)

  return true;
} // Bipc_mq_handle::timed_send()

bool Bipc_mq_handle::try_receive(util::Blob_mutable* blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;
  using util::Blob_mutable;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Bipc_mq_handle::try_receive, blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert(m_mq && "As advertised: try_receive() => undefined behavior if not successfully cted or was moved-from.");

  bool not_blocked;
  op_with_possible_bipc_mq_exception(err_code,  "Bipc_mq_handle::try_receive(): bipc::message_queue::try_receive()",
                                     [&]()
  {
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                   "max-size [" << blob->size() << "].");

    size_t n_rcvd = {};
    /* (^-- Initializer not needed algorithmically, but gcc-13 gives maybe-uninitialized warning;
     * while at least various modern clangs and gcc-9 are fine.  It's OK; we can afford it.) */
    unsigned int pri_ignored;
    not_blocked // Throws <=> error wrapper sets truthy *err_code. --v
      = m_mq->try_receive(blob->data(), blob->size(), n_rcvd, pri_ignored);
    if (not_blocked)
    {
      *blob = Blob_mutable(blob->data(), n_rcvd);
      FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
      if (blob->size() != 0)
      {
        FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
      }
    }
    else // if (!not_blocked)
    {
      FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                    "max-size [" << blob->size() << "]: would-block.");
    }
  }); // op_with_possible_bipc_mq_exception()
  if (*err_code) // It logged if truthy.
  {
    return false; // not_blocked is garbage; *blob is untouched.
  }
  // else

  return not_blocked; // Logged about it already.
} // Bipc_mq_handle::try_receive()

void Bipc_mq_handle::receive(util::Blob_mutable* blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;
  using util::Blob_mutable;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { receive(blob, actual_err_code); },
         err_code, "Bipc_mq_handle::receive()"))
  {
    return;
  }
  // else
  err_code->clear();

  assert(m_mq && "As advertised: receive() => undefined behavior if not successfully cted or was moved-from.");

  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-pop to blob @[" << blob->data() << "], "
                 "max-size [" << blob->size() << "].  Trying nb-pop first; if it succeeds -- great.  "
                 "Else will wait/retry/wait/retry/....");

  /* We could just invoke blocking m_mq->receive(), but to get the promised logging we go a tiny bit fancier as follows.
   * Update: Now that we have to be interrupt_*()ible, also reuse wait_*() instead of using native
   * m_mq->*(). */

  size_t n_rcvd = {}; // (Why initialize?  See comment near first such initializer for explanation.)
  unsigned int pri_ignored;
  bool ok;

  while (true)
  {
    op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle::receive(): bipc::message_queue::try_receive()",
                                     [&]()
    {
      ok = m_mq->try_receive(blob->data(), blob->size(), // Throws <=> error wrapper sets truthy *err_code.
                             n_rcvd, pri_ignored);
    });
    if (*err_code)
    {
      // Threw => true error => *err_code set; get out.
      return;
    }
    // else if (would-block or success):
    if (ok)
    {
      *blob = Blob_mutable(blob->data(), n_rcvd);
      FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
      if (blob->size() != 0)
      {
        FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
      }
      return; // Instant success.
    }
    // else if (would-block): as promised, INFO logs.

    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                  "max-size [" << blob->size() << "]: would-block.  Executing blocking-pop.");

    wait_receivable(err_code);
    if (*err_code)
    {
      // Whether interrupted or true error, we're done.
      return;
    }
    // else
    FLOW_LOG_TRACE("Blocking-wait reported transmissibility.  Retrying.");
  } // while (true)
} // Bipc_mq_handle::receive()

bool Bipc_mq_handle::timed_receive(util::Blob_mutable* blob, util::Fine_duration timeout_from_now, Error_code* err_code)
{
  using util::Blob_mutable;
  using flow::util::time_since_posix_epoch;
  using flow::util::buffers_dump_string;
  using flow::Fine_clock;
  using boost::chrono::round;
  using boost::chrono::microseconds;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Bipc_mq_handle::timed_receive, blob, timeout_from_now, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // Similar to a combo of (blocking) receive() and (non-blocking) try_receive() -- keeping comments light.

  assert(m_mq && "As advertised: timed_receive() => undefined behavior if not successfully cted or was moved-from.");

  FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-timed-pop to blob @[" << blob->data() << "], "
                 "max-size [" << blob->size() << "]; timeout ~[" << round<microseconds>(timeout_from_now) << "].  "
                 "Trying nb-pop first; if it succeeds -- great.  Else will wait/retry/wait/retry/....");

  size_t n_rcvd = {}; // (Why initialize?  See comment near first such initializer for explanation.)
  unsigned int pri_ignored;

  auto now = Fine_clock::now();
  auto after = now;
  bool ok;

  while (true)
  {
    op_with_possible_bipc_mq_exception(err_code, "Bipc_mq_handle::timed_send(): bipc::message_queue::try_send()",
                                       [&]()
    {
      ok = m_mq->try_receive(blob->data(), blob->size(), // Throws <=> error wrapper sets truthy *err_code.
                             n_rcvd, pri_ignored);
    });
    if (*err_code)
    {
      // Threw => true error => *err_code set; get out.
      return false;
    }
    // else if (would-block or success):
    if (ok)
    {
      *blob = Blob_mutable(blob->data(), n_rcvd);
      FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
      if (blob->size() != 0)
      {
        FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
      }
      break; // Instant success.
    }
    // else if (would-block): as promised, INFO logs.

    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                  "max-size [" << blob->size() << "]: would-block.  Executing blocking-wait.");

    timeout_from_now -= (after - now); // No-op the first time; after that reduces time left.
    const bool ready = timed_wait_receivable(timeout_from_now, err_code);
    if (*err_code)
    {
      // Whether interrupted or true error, we're done.
      return false;
    }
    // else:

    if (!ready) // I.e., if (timed out).
    {
      FLOW_LOG_TRACE("Did not finish before timeout.");
      return false;
    }
    // else: successful wait for transmissibility.  Try nb-transmitting again.
    FLOW_LOG_TRACE("Blocking-wait reported transmissibility.  Retrying.");

    after = Fine_clock::now();
    assert((after >= now) && "Fine_clock is supposed to never go backwards.");
  } // while (true)

  return true;
} // Bipc_mq_handle::timed_receive()

template<bool SND_ELSE_RCV, bool ON_ELSE_OFF>
bool Bipc_mq_handle::interrupt_allow_impl()
{
  using Classic_shm_area = bipc::ipcdetail::managed_open_or_create_impl<bipc::shared_memory_object, 0, true, false>;
  using Bipc_mq = bipc::message_queue;
  using Bipc_mq_hdr = bipc::ipcdetail::mq_hdr_t<Bipc_mq::void_pointer>;
  using Bipc_mq_mtx = bipc::interprocess_mutex;
  using Bipc_mq_lock = bipc::scoped_lock<Bipc_mq_mtx>;

  assert(m_mq
         && "As advertised: interrupt_allow_impl() => undefined behavior if not successfully cted or was moved-from.");

  /* First see m_interrupting_snd doc header.  Then check out wait_impl() carefully.  Then the below will
   * probably make sense. */

  bool* interrupting_ptr;
  auto& shm_area = reinterpret_cast<Classic_shm_area&>(*m_mq);
  auto* const mq_hdr = static_cast<Bipc_mq_hdr*>(shm_area.get_user_address());
  decltype(mq_hdr->m_cond_recv)* cond_ptr;
  if constexpr(SND_ELSE_RCV)
  {
    interrupting_ptr = &m_interrupting_snd;
    cond_ptr = &mq_hdr->m_cond_send;
  }
  else
  {
    interrupting_ptr = &m_interrupting_rcv;
    cond_ptr = &mq_hdr->m_cond_recv;
  }
  auto& interrupting = *interrupting_ptr;
  auto& cond = *cond_ptr;

  {
    Bipc_mq_lock lock(mq_hdr->m_mutex);

    if (interrupting == ON_ELSE_OFF)
    {
      FLOW_LOG_WARNING("Bipc_mq_handle [" << *this << "]: Interrupt mode already set for "
                       "snd_else_rcv [" << SND_ELSE_RCV << "], on_else_off [" << ON_ELSE_OFF << "].  Ignoring.");
      return false;
    }
    // else

    interrupting = ON_ELSE_OFF;
    FLOW_LOG_INFO("Bipc_mq_handle [" << *this << "]: Interrupt mode set for "
                  "snd_else_rcv [" << SND_ELSE_RCV << "], on_else_off [" << ON_ELSE_OFF << "].  If on -- we "
                  "shall now ping the associated condition variable to wake up any ongoing waits.");

    if constexpr(ON_ELSE_OFF)
    {
      /* *All* ongoing waits shall be woken up.  Each one will auto-re-lock mq_hdr->m_mutex and query
       * their local m_interrupting_*.  (Note: If `*this` wakes up, it'll see it's `true`.  If another
       * one wakes up, it will probably see `false` and re-enter the wait.  If by some coincidence that non-`*this`
       * had just set *that* m_interrupting_*, then -- well, cool -- presumably we won a race against
       * their own interrupt_allow_impl() doing the same thing. */
      cond.notify_all(); // By the docs, and even by the source code as of Boost-1.81, this does not throw.

      /* (bipc::message_queue code does .notify_one() in send/receive impl, which makes sense since one
       * message/one empty space would be used by at most one blocked dude.  It also does it outside the mutex lock,
       * citing performance at the cost of occasional spurious wakeups.  Neither thing applies to us, so let's
       * .notify_all() and keep it simple inside the locked section. */
    } // if constexpr(ON_ELSE_OFF)
    /* else if constexpr(!ON_ELSE_OFF)
     * { We're good!  No need to wake anyone up to tell them... not to stop. } */
  } // Bipc_mq_lock lock(mq_hdr->m_mutex);

  return true;
} // Bipc_mq_handle::interrupt_allow_impl()

bool Bipc_mq_handle::interrupt_sends()
{
  return interrupt_allow_impl<true, true>();
}

bool Bipc_mq_handle::allow_sends()
{
  return interrupt_allow_impl<true, false>();
}

bool Bipc_mq_handle::interrupt_receives()
{
  return interrupt_allow_impl<false, true>();
}

bool Bipc_mq_handle::allow_receives()
{
  return interrupt_allow_impl<false, false>();
}

template<Bipc_mq_handle::Wait_type WAIT_TYPE, bool SND_ELSE_RCV>
bool Bipc_mq_handle::wait_impl([[maybe_unused]] util::Fine_duration timeout_from_now, Error_code* err_code)
{
  using util::Fine_time_pt;
  using flow::util::time_since_posix_epoch;
  using boost::chrono::round;
  using boost::chrono::microseconds;
  using Classic_shm_area = bipc::ipcdetail::managed_open_or_create_impl<bipc::shared_memory_object, 0, true, false>;
  using Bipc_mq = bipc::message_queue;
  using Bipc_mq_hdr = bipc::ipcdetail::mq_hdr_t<Bipc_mq::void_pointer>;
  using Bipc_mq_mtx = bipc::interprocess_mutex;
  using Bipc_mq_lock = bipc::scoped_lock<Bipc_mq_mtx>;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Bipc_mq_handle::timed_wait_receivable, timeout_from_now, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert(m_mq
         && "As advertised: wait_impl() => undefined behavior if not successfully cted or was moved-from.");

  [[maybe_unused]] Fine_time_pt timeout_since_epoch;
  if constexpr(WAIT_TYPE == Wait_type::S_TIMED_WAIT)
  {
    timeout_since_epoch = Fine_time_pt(time_since_posix_epoch() + timeout_from_now);
  }

  /* Most likely the below code will elicit a "WTF."  The background is that bipc::message_queue lacks anything like
   * wait_impl().  It seems to mimic the mq_*() API.  However mq_*() does have it, in Linux, because mq_t is an FD,
   * and it can be used with epoll_*(), so that is what Posix_mq_handle uses.  To get the same thing here I had
   * to hack it as seen below.  What it does is it mimics *send() and *receive() internal Boost source code, but
   * it stops short of actually performing the read or write once the queue becomes pushable/poppable.
   * To get it work I (ygoldfel) operate directly on their internal data structures, same as those methods do.
   * Normally this would be beyond the pale; however in this case a couple of points make it reasonable-enough.
   *
   * Firstly, we can even do it in the first place, because the data structures -- all of which are directly in
   * SHM -- `public`ly expose their data members (though, interestingly, not the methods; however the methods
   * are very basic wrappers around public things like condition variables and mutexes).  This fact is suggestive
   * of this being intentionally possible.
   *
   * Secondly, and this is the main reason I consider this reasonably maintainable, is the fact that the code
   * itself -- including a comment above the BOOST_INTERPROCESS_MSG_QUEUE_CIRCULAR_INDEX define -- says that
   * their goal was to make version A of bipc interoperable (this is IPC after all) if version B>A of bipc.
   * That means that if they *do* change this stuff, it will be protected by #define(s) like
   * BOOST_INTERPROCESS_MSG_QUEUE_CIRCULAR_INDEX, so as to make any change possible to roll-back via a compile
   * flag.  They *could* rename a data member without changing the physical structures, in which case this
   * would stop building, but that seems unlikely, as Boost updates are not made willy-nilly.
   *
   * Is there risk of this breaking with a newer Boost version?  Yes, but the above evidence shows the risk is
   * manageably low.  Note that this wait_impl() feature, particularly since I've made it interruptible, is quite
   * useful.  Without it one must used timed_*(), and even with that -- suppose we want to stop work on
   * a queue from another thread -- we have to put up a deinit time equal to the fine-grainedness of the timeout
   * one would have to use.  Plus even that aside, it is annoying to have to break up an operation into smaller
   * ones. */

  if constexpr(WAIT_TYPE == Wait_type::S_POLL)
  {
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Poll-unstarved for snd_else_rcv [" << SND_ELSE_RCV << "].");
  }
  else if constexpr(WAIT_TYPE == Wait_type::S_TIMED_WAIT)
  {
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-timed-await-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]; "
                   "timeout ~[" << round<microseconds>(timeout_from_now) << "].");
  }
  else
  {
    static_assert(WAIT_TYPE == Wait_type::S_WAIT, "What!");
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-await-unstarved for snd_else_rcv "
                   "[" << SND_ELSE_RCV << "].");
  }

  auto& shm_area = reinterpret_cast<Classic_shm_area&>(*m_mq);
  auto* const mq_hdr = static_cast<Bipc_mq_hdr*>(shm_area.get_user_address());
  size_t* blocked_dudes_ptr;
  bool* interrupting_ptr;
  decltype(mq_hdr->m_cond_recv)* cond_ptr;
  if constexpr(SND_ELSE_RCV)
  {
    blocked_dudes_ptr = &mq_hdr->m_blocked_senders;
    cond_ptr = &mq_hdr->m_cond_send;
    interrupting_ptr = &m_interrupting_snd;
  }
  else
  {
    blocked_dudes_ptr = &mq_hdr->m_blocked_receivers;
    cond_ptr = &mq_hdr->m_cond_recv;
    interrupting_ptr = &m_interrupting_rcv;
  }
  auto& blocked_dudes = *blocked_dudes_ptr;
  auto& cond = *cond_ptr;
  bool& interrupting = *interrupting_ptr;

  const auto is_starved_func = [&]() -> bool
  {
    if constexpr(SND_ELSE_RCV)
    {
      return mq_hdr->m_cur_num_msg == mq_hdr->m_max_num_msg;
    }
    else
    {
      return mq_hdr->m_cur_num_msg == 0;
    }
  }; // const auto is_starved_func =

  bool interrupted = false;
  bool not_starved;
  /* Technically [timed_]wait() can throw, I guess if bool(mutex) is false.  Their internal code has a try{}
   * so why not. */
  op_with_possible_bipc_mq_exception(err_code,
                                     "Bipc_mq_handle::wait_impl(): "
                                       "bipc::interprocess_condition::[timed_]wait()",
                                     [&]()
  {
#ifndef BOOST_INTERPROCESS_MSG_QUEUE_CIRCULAR_INDEX
    static_assert(false,
                  "bipc comments show this shall be true as of many Boost versions ago, unless unset which "
                    "would decrease performance; and we do not do that.  Our code for simplicity assumes "
                    "it and does not support the lower-perf bipc MQ algorithm.");
#endif

    {
      Bipc_mq_lock lock(mq_hdr->m_mutex);

      // See interrupt_allow_impl() to understand this check (also below on cond.*wait()).
      if (interrupting)
      {
        FLOW_LOG_TRACE("Interrupted before wait/poll started (preemptively).");
        interrupted = true;
        return;
      }
      // else

      if (is_starved_func())
      {
        // timed_receive() would INFO-log here, but let's not slow things down while the bipc MQ mutex is locked.

        if constexpr(WAIT_TYPE == Wait_type::S_POLL)
        {
          FLOW_LOG_TRACE("Not immediatelly unstarved.  Poll = done.");
          not_starved = false;
          return;
        }
        else // if constexpr(WAIT_TYPE == Wait_type::S_[TIMED_]WAIT)
        {
          FLOW_LOG_TRACE("Not immediatelly unstarved.  Awaiting unstarvedness or timeout.");

          ++blocked_dudes;
          try
          {
            do
            {
              if constexpr(WAIT_TYPE == Wait_type::S_WAIT)
              {
                cond.wait(lock);

                // See interrupt_allow_impl() to understand this check (also above).
                if (interrupting)
                {
                  FLOW_LOG_TRACE("Interruption detected upon waking up from wait (interrupted concurrently).");
                  --blocked_dudes;
                  interrupted = true;
                  return;
                }
                // else: Another loop iteration.
              }
              else // if (WAIT_TYPE==TIMED_WAIT)
              {
                static_assert(WAIT_TYPE == Wait_type::S_TIMED_WAIT, "The hell?");

                const bool wait_result = cond.timed_wait(lock, timeout_since_epoch); // Lock unlocked throughout wait.

                // See interrupt_allow_impl() to understand this check (also above).
                if (interrupting)
                {
                  FLOW_LOG_TRACE("Interruption detected upon waking up from wait (interrupted concurrently).");
                  --blocked_dudes;
                  interrupted = true;
                  return;
                }
                // else

                if (!wait_result)
                {
                  // Timeout reached.
                  if (is_starved_func())
                  {
                    // Timeout reached; still starved.  Done (exit algo).
                    --blocked_dudes;
                    not_starved = false;
                    return;
                  }
                  // else: Timeout reached; *is* unstarved.  Done (exit loop -- no need to recheck is_starved_func()).
                  break;
                } // if (!wait_result)
                // else: Timeout not reached; probably *is* unstarved; but check it as loop exit condition.
              } // else if (WAIT_TYPE==TIMED_WAIT)
            }
            while (is_starved_func());
          } // try
          catch (...)
          {
            --blocked_dudes;
            throw;
          }
          --blocked_dudes;
        } // else if constexpr(WAIT_TYPE == Wait_type::S_[TIMED_]WAIT)

        // Will INFO-log shortly (outside lock).
        not_starved = true;
      } // if (is_starved_func())
      else
      {
        FLOW_LOG_TRACE("Immediately unstarved.");
        not_starved = true;
      }
    } // Bipc_mq_lock lock(mq_hdr->m_mutex);
  }); // op_with_possible_bipc_mq_exception()

  if ((!*err_code) && interrupted)
  {
    FLOW_LOG_INFO("Bipc_mq_handle [" << *this << "]: Poll/wait/timed-wait-unstarved for "
                  "snd_else_rcv [" << SND_ELSE_RCV << "]: interrupted (TRACE message -- if visible -- "
                  "indicates whether preemptively or concurrently).");
    *err_code = error::Code::S_INTERRUPTED;
  }

  if (*err_code) // It logged if truthy.
  {
    return false; // not_starved is garbage.
  }
  // else: as promised, INFOx1 log.

  if constexpr(WAIT_TYPE == Wait_type::S_POLL)
  {
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Poll-unstarved for snd_else_rcv [" << SND_ELSE_RCV << "]: "
                   "succeeded? = [" << not_starved << "].");
  }
  else if constexpr(WAIT_TYPE == Wait_type::S_TIMED_WAIT)
  {
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-timed-await-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]; timeout ~[" << round<microseconds>(timeout_from_now) << "]: "
                   "succeeded? = [" << not_starved << "].  "
                   "Either was immediately unstarved, or was not but waited until success or timeout+failure.  "
                   "If success: TRACE message (if visible) above indicates which occurred.");
  }
  else
  {
    assert(not_starved);
    FLOW_LOG_TRACE("Bipc_mq_handle [" << *this << "]: Blocking-await-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]: succeeded eventually.  "
                   "Either was immediately unstarved, or was not but waited it out.  "
                   "TRACE message (if visible) above indicates which occurred.");
  }

  return not_starved; // Logged about it already.
} // Bipc_mq_handle::wait_impl()

bool Bipc_mq_handle::is_sendable(Error_code* err_code)
{
  return wait_impl<Wait_type::S_POLL, true>(util::Fine_duration(), err_code);
}

void Bipc_mq_handle::wait_sendable(Error_code* err_code)
{
  wait_impl<Wait_type::S_WAIT, true>(util::Fine_duration(), err_code);
}

bool Bipc_mq_handle::timed_wait_sendable(util::Fine_duration timeout_from_now, Error_code* err_code)
{
  return wait_impl<Wait_type::S_TIMED_WAIT, true>(timeout_from_now, err_code);
}

bool Bipc_mq_handle::is_receivable(Error_code* err_code)
{
  return wait_impl<Wait_type::S_POLL, false>(util::Fine_duration(), err_code);
}

void Bipc_mq_handle::wait_receivable(Error_code* err_code)
{
  wait_impl<Wait_type::S_WAIT, false>(util::Fine_duration(), err_code);
}

bool Bipc_mq_handle::timed_wait_receivable(util::Fine_duration timeout_from_now, Error_code* err_code)
{
  return wait_impl<Wait_type::S_TIMED_WAIT, false>(timeout_from_now, err_code);
}

void Bipc_mq_handle::remove_persistent(flow::log::Logger* logger_ptr, // Static.
                                       const Shared_name& absolute_name, Error_code* err_code)
{
  using bipc::message_queue;
  using boost::system::system_category;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { remove_persistent(logger_ptr, absolute_name, actual_err_code); },
         err_code, "Bipc_mq_handle::remove_persistent()"))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  FLOW_LOG_INFO("Bipc_mq @ Shared_name[" << absolute_name << "]: Removing persistent MQ if possible.");
  const bool ok = message_queue::remove(absolute_name.native_str()); // Does not throw.

  if (ok)
  {
    err_code->clear();
    return;
  }
  /* message_queue::remove() is strangely gimped -- though I believe so are the other kernel-persistent remove()s
   * throughout bipc -- it does not throw and merely returns true or false and no code.  Odd, since there can
   * be at least a couple of reasons one would fail to delete....  However, in POSIX, the Boost 1.78 source code
   * shows that mq::remove() calls shared_memory_object::remove() which calls some internal
   * ipcdetail::delete_file() which calls... drumroll... freakin' ::unlink(const char*).  Hence we haxor: */
#ifndef FLOW_OS_LINUX // @todo Should maybe check Boost version or something too?
  static_assert(false,
                "Code in Bipc_mq_handle::remove_persistent() relies on Boost invoking Linux unlink() with errno.");
#endif
  const auto& sys_err_code = *err_code = Error_code(errno, system_category());
  FLOW_ERROR_SYS_ERROR_LOG_WARNING();
} // Bipc_mq_handle::remove_persistent()

template<typename Func>
void Bipc_mq_handle::op_with_possible_bipc_mq_exception(Error_code* err_code, util::String_view context,
                                                        const Func& func)
{
  using flow::error::Runtime_error;
  using bipc::interprocess_exception;
  using boost::system::system_category;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { op_with_possible_bipc_mq_exception(actual_err_code, context, func); },
         err_code, context))
  {
    return;
  }
  // else

  try
  {
    func();
  }
  catch (const interprocess_exception& exc)
  {
    /* They appear to always throw this guy with some interesting semantics.  We want to yield our consistent
     * Flow-style semantics, namely produce a truthy *err_code (which, if actual original err_code was null will
     * be instead thrown after being wrapped in a flow::Runtime_error, which is really a boost::system::system_error
     * with a cleaner what() message).  More details inline below.
     *
     * So interprocess_exception is not a system_error even but rather an exception with a particular custom API.
     * To ensure the information is logged *somewhere* for sure and not lost do log all of this.
     * After that normalize to the *err_code semantics, even if some information is lost -- though we'll try
     * to lose nothing even so. */
    const auto native_code_raw = exc.get_native_error();
    const auto bipc_err_code_enum = exc.get_error_code();
    const bool is_size_error = bipc_err_code_enum == bipc::size_error;
    FLOW_LOG_WARNING("bipc threw interprocess_exception; will emit some hopefully suitable Flow-IPC Error_code; "
                     "but here are all the details of the original exception: native code int "
                     "[" << native_code_raw << "]; bipc error_code_t enum->int "
                     "[" << int(bipc_err_code_enum) << "]; latter==size_error? = [" << is_size_error << "]; "
                     "message = [" << exc.what() << "]; context = [" << context << "].");

    /* Special case: size_error is thrown by message_queue on a receive with underflow or send with overflow.
     * Our API contract (Persistent_mq_handle concept doc header) is to emit this particular code then. */
    if (is_size_error)
    {
      // Sufficient.  (Check of Boost 1.78 source code confirms there's no native code in this case anyway.)
      *err_code = error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW;
      return;
    }
    // else
    if (native_code_raw != 0)
    {
      /* At least in POSIX, interprocess_exception only does the following in this case:
       *   - strerror(native_code_raw) => the message.  But that's standard boost.system errno handling already;
       *     so if just emit a system_category() Error_code, all will be equally well message-wise in what().
       *   - They have a table that maps certain native_code_raw values to one of a few (not super-many but not
       *     a handful) bipc_err_code_enum enum values.  Technically the following will lose that information;
       *     but (1) it is logged above; and (2) so what? */
      const auto& sys_err_code = *err_code = Error_code(native_code_raw, system_category());
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      return;
    }
    // else

    /* All we have is bipc_err_code_enum.  We've already handled the known likely issue, size_error.  Beyond that
     * there does not seem to be much we can do; really it's a matter of either emitting one catch-all code or
     * having our own equivalents of their enum.  For now, at least, the former is OK.  @todo Revisit. */
    *err_code = error::Code::S_MQ_BIPC_MISC_LIBRARY_ERROR; // The earlier WARNING is good enough.
    return;
  } // catch ()
  // Got here: all good.
  err_code->clear();
} // Bipc_mq_handle::op_with_possible_bipc_mq_exception()

const Shared_name& Bipc_mq_handle::absolute_name() const
{
  return m_absolute_name;
}

std::ostream& operator<<(std::ostream& os, const Bipc_mq_handle& val)
{
  return os << '@' << &val << ": sh_name[" << val.absolute_name() << ']';
}

} // namespace ipc::transport
