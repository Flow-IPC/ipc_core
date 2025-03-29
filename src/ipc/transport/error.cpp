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
#include "ipc/transport/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::transport module.  Think of it as the polymorphic
 * counterpart of error::Code, and it kicks in when, for `Error_code ec`, something
 * like `ec.message()` is invoked.
 *
 * Note that this class's declaration is not available outside this translation unit (.cpp
 * file), and its logic is accessed indirectly through standard boost.system machinery
 * (`Error_code::name()` and `Error_code::message()`).  This class enables those basic
 * features (e.g., `err_code.message()` returns the string description of the error `err_code`) to
 * work.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Implements super-class API: returns a `static` string representing this `error_category` (which,
   * for example, shows up in the `ostream` representation of any Category-belonging
   * #Error_code).
   *
   * @return A `static` string that's a brief description of this error category.
   */
  const char* name() const noexcept override;

  /**
   * Implements super-class API: given the integer error code of an error in this category, returns a description of
   * that error (similarly in spirit to `std::strerror()`).  This is the "engine" that allows `ec.message()`
   * to work, where `ec` is an #Error_code with an error::Code value.
   *
   * @param val
   *        Error code of an Category error (realistically, a #Code `enum` value cast to
   *        `int`).
   * @return String describing the error.
   */
  std::string message(int val) const override;

  /**
   * The guts of the `ostream << Code` operation: outputs, e.g., Code::S_INVALID_ARGUMENT => `"INVALID_ARGUMENT"`.
   * @param code
   *        A Code.
   * @return What would/should be printed to an `ostream` given `code`.
   */
  static util::String_view code_symbol(Code code);

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code(static_cast<int>(err_code), Category::S_CATEGORY);
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/transport";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  /* Just create a string (unless compiler is smart enough to do this only once and reuse later)
   * based on a static char* which is rapidly indexed from val by the switch() statement.
   * Since the error_category interface requires that message() return a string by value, this
   * is the best we can do speed-wise... but it should be fine.
   *
   * Some error messages can be fancier by specifying outside values (e.g., see
   * net_flow's S_INVALID_SERVICE_PORT_NUMBER). */
  switch (static_cast<Code>(val))
  {
  case Code::S_SENDS_FINISHED_CANNOT_SEND:
    return "Will not send message: local user already ended sending via API marking this.";
  case Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE:
    return "Will not receive message: opposing user sent graceful-close via API.";
  case Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND:
    return "Unable to send outgoing traffic: an earlier-reported, or at least logged, system error had hosed the "
           "underlying transport mechanism; or user had ejected/closed the underlying transport handle.";
  case Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE:
    return "Unable to receive incoming traffic: an earlier-reported, or at least logged, system error had hosed the "
           "underlying transport mechanism; or user had ejected/closed the underlying transport handle.";
  case Code::S_LOW_LVL_TRANSPORT_HOSED:
    return "Unable to access low-level details: an earlier-reported system error had hosed the underlying "
           "transport mechanism; or user had ejected/closed the underlying transport handle.";
  case Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL:
    return "Unable to receive incoming traffic: message contains more than: 1 blob plus 0-1 native handles.";
  case Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE:
    return "User protocol-code mismatch: local user-provided storage cannot fit entire message received from "
           "opposing peer.";
  case Code::S_BLOB_RECEIVER_GOT_NON_BLOB:
    return "User protocol-code mismatch: local user expected blob only and no native handle; received at "
           "least the latter.";
  case Code::S_RECEIVER_IDLE_TIMEOUT:
    return "No messages (optional auto-pings or otherwise) have been received; optionally configured timeout "
           "exceeded.  If a session::Session emitted this error, the session is now hosed due to master channel "
           "idle timeout indicating poor health of the opposing side or the system.";
  case Code::S_INVALID_ARGUMENT:
    return "User called an API with 1 or more arguments against the API spec.";
  case Code::S_TIMEOUT:
    return "A (usually user-specified) timeout period has elapsed before a blocking operation completed.";
  case Code::S_INTERRUPTED:
    return "A blocking operation was intentionally interrupted or preemptively canceled.";
  case Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER:
    return "Async completion handler is being called prematurely, because underlying object is shutting down, "
           "as user desires.";
  case Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW:
    return "Low-level message queue send-op buffer overflow (> max size) or receive-op buffer underflow (< max size).";
  case Code::S_MQ_BIPC_MISC_LIBRARY_ERROR:
    return "Low-level message queue: boost.interprocess emitted miscellaneous library exception sans a system code; "
           "a WARNING message at throw-time should contain all possible details.";
  case Code::S_BLOB_STREAM_MQ_SENDER_EXISTS:
    return "Message-queue blob stream outgoing-direction peer could not be created, because one already "
           "exists at that name.";
  case Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS:
    return "Message-queue blob stream incoming-direction peer could not be created, because one already "
           "exists at that name.";
  case Code::S_BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD:
    return "Message-queue blob stream peer received a malformed internal-use message from opposing side.";
  case Code::S_SYNC_IO_WOULD_BLOCK:
    return
      "A sync_io operation could not immediately complete; it will complete contingent on active async-wait event(s).";
  case Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID:
    return "In protocol negotiation, opposing side sent invalid version value (not positive, not a number, etc.); "
           "the comm pathway must close.";
  case Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD:
    return "In protocol negotiation, opposing side reported its newest protocol version is even older than the most "
           "backwards-compatible (oldest, smallest) version we speak; the comm pathway must close.";

  case Code::S_END_SENTINEL:
    assert(false && "SENTINEL: Not an error.  "
                    "This Code must never be issued by an error/success-emitting API; I/O use only.");
  }
  assert(false);
  return "";
} // Category::message()

util::String_view Category::code_symbol(Code code) // Static.
{
  // Note: Must satisfy istream_to_enum() requirements.

  switch (code)
  {
  case Code::S_SENDS_FINISHED_CANNOT_SEND:
    return "SENDS_FINISHED_CANNOT_SEND";
  case Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE:
    return "RECEIVES_FINISHED_CANNOT_RECEIVE";
  case Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND:
    return "LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND";
  case Code::S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE:
    return "LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE";
  case Code::S_LOW_LVL_TRANSPORT_HOSED:
    return "LOW_LVL_TRANSPORT_HOSED";
  case Code::S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL:
    return "LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL";
  case Code::S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE:
    return "MESSAGE_SIZE_EXCEEDS_USER_STORAGE";
  case Code::S_BLOB_RECEIVER_GOT_NON_BLOB:
    return "BLOB_RECEIVER_GOT_NON_BLOB";
  case Code::S_RECEIVER_IDLE_TIMEOUT:
    return "RECEIVER_IDLE_TIMEOUT";
  case Code::S_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case Code::S_TIMEOUT:
    return "TIMEOUT";
  case Code::S_INTERRUPTED:
    return "INTERRUPTED";
  case Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER:
    return "OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER";
  case Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW:
    return "MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW";
  case Code::S_MQ_BIPC_MISC_LIBRARY_ERROR:
    return "MQ_BIPC_MISC_LIBRARY_ERROR";
  case Code::S_BLOB_STREAM_MQ_SENDER_EXISTS:
    return "BLOB_STREAM_MQ_SENDER_EXISTS";
  case Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS:
    return "BLOB_STREAM_MQ_RECEIVER_EXISTS";
  case Code::S_BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD:
    return "BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD";
  case Code::S_SYNC_IO_WOULD_BLOCK:
    return "SYNC_IO_WOULD_BLOCK";
  case Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID:
    return "PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID";
  case Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD:
    return "PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD";

  case Code::S_END_SENTINEL:
    return "END_SENTINEL";
  }
  assert(false);
  return "";
}

std::ostream& operator<<(std::ostream& os, Code val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  return os << Category::code_symbol(val);
}

std::istream& operator>>(std::istream& is, Code& val)
{
  /* Range [<1st Code>, END_SENTINEL); no match => END_SENTINEL;
   * allow for number instead of ostream<< string; case-insensitive. */
  val = flow::util::istream_to_enum(&is, Code::S_END_SENTINEL, Code::S_END_SENTINEL, true, false,
                                    Code(S_CODE_LOWEST_INT_VALUE));
  return is;
}

} // namespace ipc::transport::error
