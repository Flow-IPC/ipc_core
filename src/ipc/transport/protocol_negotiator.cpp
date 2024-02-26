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

#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>

namespace ipc::transport
{

Protocol_negotiator::Protocol_negotiator(flow::log::Logger* logger_ptr, util::String_view nickname,
                                         proto_ver_t local_max_proto_ver, proto_ver_t local_min_proto_ver) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_nickname(nickname),
  m_local_max_proto_ver(local_max_proto_ver),
  m_local_min_proto_ver(local_min_proto_ver),
  m_local_max_proto_ver_sent(false),
  m_negotiated_proto_ver(S_VER_UNKNOWN)
{
  assert((m_local_min_proto_ver > 0) && "Broke contract.");
  assert((m_local_max_proto_ver >= m_local_min_proto_ver) && "Broke contract.");

  FLOW_LOG_INFO("Protocol_negotiator [" << m_nickname << "]: We speak protocol version range "
                "[" << m_local_min_proto_ver << ", " << m_local_max_proto_ver << "].");
}

Protocol_negotiator::Protocol_negotiator(const Protocol_negotiator& src) = default;
Protocol_negotiator& Protocol_negotiator::operator=(const Protocol_negotiator& src) = default;

Protocol_negotiator::Protocol_negotiator(Protocol_negotiator&& src)
{
  operator=(std::move(src));
}

Protocol_negotiator& Protocol_negotiator::operator=(Protocol_negotiator&& src)
{
  if (this != &src)
  {
    operator=(static_cast<const Protocol_negotiator&>(src));
    // This (which satisfies our contract, to make src as-if-just-cted) is the only reason we're not `= default`:
    src.reset();
  }
  return *this;
}

void Protocol_negotiator::reset()
{
  m_local_max_proto_ver_sent = false;
  m_negotiated_proto_ver = S_VER_UNKNOWN;
}

Protocol_negotiator::proto_ver_t Protocol_negotiator::negotiated_proto_ver() const
{
  return m_negotiated_proto_ver;
}

bool Protocol_negotiator::compute_negotiated_proto_ver(proto_ver_t opposing_max_proto_ver, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Protocol_negotiator::compute_negotiated_proto_ver,
                                     opposing_max_proto_ver, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (negotiated_proto_ver() != S_VER_UNKNOWN)
  {
    FLOW_LOG_TRACE("Protocol_negotiator [" << m_nickname << "]: We were asked to compute negotiated protocol version "
                   "based on opposing max-version [" << opposing_max_proto_ver << "]; but we have already done so "
                   "in the past (result: [" << negotiated_proto_ver() << "]); no-op.  "
                   "Tip: Check for negotiated_proto_ver()=UNKNOWN before calling us.");
    return false;
  }
  // else: As advertised we validate opposing_max_proto_ver for the caller first.

  if (opposing_max_proto_ver <= 0)
  {
    FLOW_LOG_WARNING("Protocol_negotiator [" << m_nickname << "]: Negotiation computation: "
                     "We speak preferred (highest) version [" << m_local_max_proto_ver << "]; "
                     "they speak at most [" << opposing_max_proto_ver << "].  The latter value is in and of itself "
                     "invalid -- we cannot trust the other side at all; or there may be a bug in user code here.  "
                     "Presumably we will abruptly close this comm pathway shorly.");
    m_negotiated_proto_ver = S_VER_UNSUPPORTED;
    *err_code = error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID;
  }
  // Use the simple algorithm explained/justified in class doc header.

  else if (m_local_max_proto_ver <= opposing_max_proto_ver)
  {
    FLOW_LOG_INFO("Protocol_negotiator [" << m_nickname << "]: Negotiation computation: "
                  "We speak preferred (highest) version [" << m_local_max_proto_ver << "]; "
                  "they speak at most [" << opposing_max_proto_ver << "]; therefore we shall speak "
                  "[" << m_local_max_proto_ver << "], unless opposing side lacks backwards-compatibility with it, "
                  "in which case it will abruptly close this comm pathway shortly.");
    m_negotiated_proto_ver = m_local_max_proto_ver;
  }
  else if (opposing_max_proto_ver >= m_local_min_proto_ver)
  {
    FLOW_LOG_INFO("Protocol_negotiator [" << m_nickname << "]: Negotiation computation: "
                  "We speak preferred (highest) version [" << m_local_max_proto_ver << "]; "
                  "they speak at most [" << opposing_max_proto_ver << "]; therefore we shall speak "
                  "[" << opposing_max_proto_ver << "], as we are backwards-compatible with that version.");
    m_negotiated_proto_ver = m_local_max_proto_ver;
  }
  else
  {
    FLOW_LOG_WARNING("Protocol_negotiator [" << m_nickname << "]: Negotiation computation: "
                     "We speak preferred (highest) version [" << m_local_max_proto_ver << "]; "
                     "they speak at most [" << opposing_max_proto_ver << "]; therefore we lack backwards-compatibility "
                     "with that older version.  Presumably we will abruptly close this comm pathway shorly.");
    m_negotiated_proto_ver = S_VER_UNSUPPORTED;
    *err_code = error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD;
  }

  return true;
} // Protocol_negotiator::compute_negotiated_proto_ver()

Protocol_negotiator::proto_ver_t Protocol_negotiator::local_max_proto_ver_for_sending()
{
  if (m_local_max_proto_ver_sent)
  {
    return S_VER_UNKNOWN; // Don't spam logs; they might be calling this for every out-message.
  }
  // else
  FLOW_LOG_INFO("Protocol_negotiator [" << m_nickname << "]: About to send our preferred (highest) version "
                "[" << m_local_max_proto_ver << "] to opposing side for the first and only time.");
  m_local_max_proto_ver_sent = true;
  return m_local_max_proto_ver;
}

} // namespace ipc::transport
