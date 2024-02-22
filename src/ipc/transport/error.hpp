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
#pragma once

#include "ipc/common.hpp"

/**
 * Namespace containing the ipc::transport module's extension of boost.system error conventions, so that that API
 * can return codes/messages from within its own new set of error codes/messages.  Note that many errors
 * ipc::transport might report are system errors and would not draw from this set of codes/messages but rather
 * from `boost::asio::error` or `boost::system::errc` (possibly others).  (If you're familiar with the boost.system
 * framework, you'll know such mixing is normal -- in fact arguably one of its strengths.)
 *
 * @see ipc::transport::struc::error which covers errors from the *structured* layer (ipc::transport::struc).
 *      The present namespace applies to unstructured/lower-level traffic or to both that and structured/higher-level.
 *
 * See flow's `flow::net_flow::error` doc header which was used as the model for this and similar.
 * As of this writing there is discussion there useful for someone new to boost.system error reporting.
 *
 * @internal
 *
 * @todo This file and error.cpp are perfectly reasonable and worth-it and standard boiler-plate; and
 * it's not that lengthy really; but it'd be nice to stop having to copy/paste all the stuff outside of just the
 * error codes and messages.  Should add a feature to Flow that'll reduce the boiler-plate to just that; maybe
 * some kind of base class or something.  boost.system already makes it pretty easy, but even that can probably be
 * "factored out" into Flow.  Update: I (ygoldfel) took a look at it for another project, and so far no obvious
 * de-boiler-plate ideas come to mind.  Ideally the inputs are: (1) an `enum` with the codes, like error::Code;
 * (2) an `int`-to-`string` message table function, for log messages; and (3) a brief `const char*` identifying the
 * code set, for log messages.  The rest is the boiler-plate, but all of it seems to either already be accepably
 * brief, and where something isn't quite so, I can't think of any obvious way to factor it out.
 * Of course a macro-based "meta-language" is always a possibility, as we did in `flow::log`, but in this case it
 * doesn't seem worth it at all.
 */
namespace ipc::transport::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/**
 * All possible errors returned (via `Error_code` arguments) by ipc::transport functions/methods *outside of*
 * system-triggered errors such as `boost::asio::error::connection_reset`.
 * These values are convertible to #Error_code (a/k/a `boost::system::error_code`) and thus
 * extend the set of errors that #Error_code can represent.
 *
 * @internal
 *
 * When you add a value to this `enum`, also add its description to
 * error.cpp’s Category::message().  This description must be identical to the
 * description in the /// comment below, or at least as close as possible.  This mirrors Flow's convention.
 *
 * When you add a value to this `enum`, also add its symbolic representation to error.cpp's
 * error.cpp's Category::code_symbol().  This string must be identical to the symbol, minus the `S_`;
 * e.g., Code::S_INVALID_ARGUMENT => `"INVALID_ARGUMENT".  This enables the consistent and human-friendly
 * serialization `<<` and deserialization `>>` of a Code w/r/t standard streams.
 *
 * If, when adding a new revision of the code, you add a value to this `enum`, add it to the end, but ahead of
 * Code::S_END_SENTINEL.
 * If, when adding a new revision of the code, you deprecate a value in this `enum`, do not delete
 * it from this `enum`.  Instead mark it as deprecated here and then remove it from
 * Ipc_transport_category::message().
 *
 * Errors that indicate apparent logic bugs (in other words, assertions that we were too afraid
 * to write as actual `assert()`s) should be prefixed with `S_INTERNAL_ERROR_`, and their messages
 * should also indicate "Internal error: ".  This mirrors Flow's convention.
 */
enum class Code
{
  /// Will not send message: local user already ended sending via API marking this.
  S_SENDS_FINISHED_CANNOT_SEND = S_CODE_LOWEST_INT_VALUE,

  /// Will not receive message: either opposing user sent graceful-close via API.
  S_RECEIVES_FINISHED_CANNOT_RECEIVE,

  /**
   * Unable to send outgoing traffic: an earlier-reported, or at least logged, system error had hosed the
   * underlying transport mechanism.
   */
  S_LOW_LVL_TRANSPORT_HOSED_CANNOT_SEND,

  /**
   * Unable to receive incoming traffic: an earlier-reported, or at least logged, system error had hosed the
   * underlying transport mechanism.
   */
  S_LOW_LVL_TRANSPORT_HOSED_CANNOT_RECEIVE,

  /// Unable to access low-level details: an earlier-reported system error had hosed the underlying transport mechanism.
  S_LOW_LVL_TRANSPORT_HOSED,

  /// Unable to receive incoming traffic: message contains more than: 1 blob plus 0-1 native handles.
  S_LOW_LVL_UNEXPECTED_STREAM_PAYLOAD_BEYOND_HNDL,

  /// User protocol-code mismatch: local user-provided storage cannot fit entire message received from opposing peer.
  S_MESSAGE_SIZE_EXCEEDS_USER_STORAGE,

  /// User protocol-code mismatch: local user expected blob only and no native handle; received at least the latter.
  S_BLOB_RECEIVER_GOT_NON_BLOB,

  /**
   * No messages (optional auto-pings or otherwise) have been received; optionally configured timeout exceeded.
   * If a session::Session emitted this error, the session is now hosed due to master channel
   * idle timeout indicating poor health of the opposing side or the system.
   */
  S_RECEIVER_IDLE_TIMEOUT,

  /**
   * In protocol negotiation, opposing side sent invalid version value (not positive, not a number, etc.);
   * the comm pathway must close.
   */
  S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID,

  /**
   * In protocol negotiation, opposing side reported its newest protocol version is even older than the most
   * backwards-compatible (oldest, smallest) version we speak; the comm pathway must close.
   */
  S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD,

  /// User called an API with 1 or more arguments against the API spec.
  S_INVALID_ARGUMENT,

  /// A (usually user-specified) timeout period has elapsed before a blocking operation completed.
  S_TIMEOUT,

  /// A blocking operation was intentionally interrupted or preemptively canceled.
  S_INTERRUPTED,

  /// Async completion handler is being called prematurely, because underlying object is shutting down, as user desires.
  S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER,

  /// Low-level message queue send-op buffer overflow (> max size) or receive-op buffer underflow (< max size).
  S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW,

  /**
   * Low-level message queue: boost.interprocess emitted miscellaneous library exception sans a system code;
   * a WARNING message at throw-time should contain all possible details.
   */
  S_MQ_BIPC_MISC_LIBRARY_ERROR,

  /// Message-queue blob stream outgoing-direction peer could not be created, because one already exists at that name.
  S_BLOB_STREAM_MQ_SENDER_EXISTS,

  /// Message-queue blob stream incoming-direction peer could not be created, because one already exists at that name.
  S_BLOB_STREAM_MQ_RECEIVER_EXISTS,

  /// Message-queue blob stream peer received a malformed internal-use message from opposing side.
  S_BLOB_STREAM_MQ_RECEIVER_BAD_CTL_CMD,

  /// A sync_io operation could not immediately complete; it will complete contingent on active async-wait event(s).
  S_SYNC_IO_WOULD_BLOCK,

  /// SENTINEL: Not an error.  This Code must never be issued by an error/success-emitting API; I/O use only.
  S_END_SENTINEL
}; // enum class Code

// Free functions.

/**
 * Given a `Code` `enum` value, creates a lightweight #Error_code (a/k/a boost.system `error_code`)
 * representing that error.  This is needed to make the
 * `boost::system::error_code::error_code<Code>()` template implementation work.  Or, slightly more in English,
 * it glues the (completely general) #Error_code to the (`ipc::transport`-specific) error code set
 * ipc::transport::error::Code, so that one can implicitly covert from the latter to the former.
 *
 * @param err_code
 *        The `enum` value.
 * @return A corresponding #Error_code.
 */
Error_code make_error_code(Code err_code);

/**
 * Deserializes a transport::error::Code from a standard input stream.  Reads up to but not including the next
 * non-alphanumeric-or-underscore character; the resulting string is then mapped to a log::Sev.  If none is
 * recognized, Code::S_END_SENTINEL is the result.  The recognized values are:
 *   - "1", "2", ...: Corresponds to the `int` conversion of that Code.
 *   - Case-insensitive encoding of the non-S_-prefix part of the actual Code member; e.g.,
 *     "INVALID_ARGUMENT" (or "invalid_argument" or "Invalid_argument" or...) for Code::S_INVALID_ARGUMENT.
 * This enables a few I/O things to work, including parsing from config file/command line via and conversion from
 * `string` via `boost::lexical_cast`.
 *
 * @param is
 *        Stream from which to deserialize.
 * @param val
 *        Value to set.
 * @return `is`.
 */
std::istream& operator>>(std::istream& is, Code& val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

/**
 * Serializes a transport::error::Code to a standard output stream.  The output string is compatible with the reverse
 * `istream>>` operator.
 *
 * ### Rationale ###
 * This shall print a representation that looks like the identifier in C++ code; e.g.,
 * Code::S_INVALID_ARGUMENT => `"INVALID_ARGUMENT"`.  Nevertheless, when printing an #Error_code storing
 * a Code, continue to do the standard thing: output the #Error_code (which will print the category name, in our
 * case, something like `"ipc/transport"`; and the numeric `enum` value) itself plus its `.message()`
 * (which will print a natural-language message).  Naturally that technique will work with any #Error_code, including
 * but *not* limited to one storing a Code.
 *
 * So why does the present `operator<<` even exist?  Answer: Primarily it's in order to enable a consistent, yet
 * human-friendly, machinery of serializing/deserializing `Code`s.  No such machinery exists for general #Error_code,
 * and specifying an #Error_code in an input stream would probably invovle (at least) providing a numeric value; but
 * we can still provide a symbolic [de]serialization for specifically transport::error::Code.  Why did we want that?
 * Answer: The original use case was in testing scenarios, to easily specify an expected outcome of an ipc::transport
 * API call symbolically instead of resorting to numeric `enum` values (it's nicer to say "expect INVALID_ARGUMENT"
 * rather than "expect 13" -- though incidentally the latter also works).
 *
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Code val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::transport::error

/**
 * Small group of miscellaneous utilities to ease work with boost.system, joining its `boost::system` namespace.
 * As of this writing circumstances have to very particular and rare indeed for something to actually be added
 * by us to this namespace.  As I write this, it contains only `is_error_code_enum<>` specializations to properly
 * extend the `boost::system` error-code system.
 */
namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::transport::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  /* ^-- Could instead derive publicly from true_type, but then we'd have to find a true_type
   * in some header we could use. At any rate this works just the same, and boost.asio does it this way, so if
   * it's good enough for them (the author of boost.asio = the author of boost.system [error_code, etc.]),
   * I say it's good enough for us. */
};

} // namespace boost::system
