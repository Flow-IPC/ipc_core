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
#include "ipc/util/util_fwd.hpp"
#include <flow/log/log.hpp>

namespace ipc::transport
{

/**
 * A simple state machine that, assuming the opposide side of a comm pathway uses an equivalent state machine,
 * helps negotiate the protocol version to speak over that pathway, given each side being capable of speaking
 * a range of protocol versions and reporting the highest such version to the other side.  By *comm pathway*
 * we mean a bidirectional communication channel of some sort with two mutually-opposing endpoints (it need
 * not be full-duplex).
 *
 * It is trivially copyable and movable, so that containing object can be copyable and movable too.
 *
 * The algorithm followed is quite straightforward and is exposed completely in the contract of this simple API:
 * The impetus behind this class is not for it to be able to perform some complex `private` operations but rather
 * to provide a reusable component that both internal and (optionally) user code can reliably count on to apply
 * the same consistent rules.  So while it does some minimal work, its main value is in presenting a common
 * algorithm that (1) achieves protocol negotiation and (2) doesn't shoot ourselves in the foot in the face
 * of future protocol changes.
 *
 * ### The algorithm ###
 * A Protocol_negotiator `*this` assumes it is used by a single open comm pathway's local endpoint, and that a
 * logically-equivalent Protocol_negotiator (or equivalent sofwatre) is used in symmetrical fashion by the opposide
 * side's endpoint.  (For example, a Native_socket_stream in PEER state uses a Protocol_negotiator internally,
 * and it therefore assumes the opposing Native_socket_stream does the same.)
 *
 * Further we assume that there is a range of protocols for the comm pathway, such that at least version 1
 * (the initial version) exists, and versions 2, 3, ... may be developed (or have already been developed).
 * There is understood to be no ambiguity as to what a version X of the protocol means: so, if certain software
 * knows about version X at all, then what it understands protocol version X to be is exactly equal to what any
 * other software (that also knows of version X) understands about protocol version X.
 *   - We (the local endpoint) can speak a range of versions of the protocol: [L, H] with H >= L >= 1.
 *     Obviously we know L and H: i.e., the code instantiating `*this` knows L and H (in fact it gives them
 *     to our ctor).
 *     - Note: Informally, one can think of H being the *preferred* version for us: We want to speak it if possible.
 *       However, if necessary, we can invoke alternative code paths to speak a lower version for compatibility.
 *   - They (the opposing endpoint) can similarly speak a range [Lp, Hp], with Hp > Lp >= 1.
 *     However we do not, at first at least, know Lp nor Hp.
 *   - Each side shall speak the *highest* possible version of the protocol such that:
 *     - It is in range [L, H].
 *     - It is in range [Lp, Hp].
 *   - Therefore there are two possibilities:
 *     - If there is *no* such version, then the comm pathway cannot proceed: they have no protocol version in common
 *       they can both speak.  The pathway should close ASAP upon either side realizing this.
 *     - If there is such a version, then the comm pathway can proceed.  All we need is for *each side* to determine
 *       what that version V is.  Naturally each side must come to the same answer.
 *
 * While there are various ways to achieve this, including a back-and-forth negotiation, we opt for something quite
 * simple and symmetrical.  (Recall that the opposide side is assumed to have a Protocol_negotiator (equivalent)
 * following the same logic, and neither side shall be chosen (in our context) to be different from the other.
 * E.g., there's no client and server dichotomy -- even if in the subsequently negotiated protocol there is; that's
 * none of our business.)  The procedure:
 *   - We send H to them, ASAP.
 *     - Similarly they send Hp to us.
 *   - Having received their Hp, we choose V = min(H, Hp).
 *     - Similarly having received our H, they choose V = min(H, Hp).  Important: This value V *is* the same on each
 *       side.  However the next computation will potentially differ between the 2 sides.
 *   - On our side: If V < L (which is possible only if V = Hp, meaning Hp > H, meaning we are more advanced than
 *     they are), we are insufficiently backwards-compatible.  Therefore V = UNSUPPORTED.  We should
 *     close the comm pathway ASAP.
 *     - Similarly, on their side, if V < Lp -- they are more advanced than we are, and they don't speak enough
 *       older versions to accomodate us -- then they will detect that V = UNSUPPORTED and should close the comm
 *       pathway ASAP.
 *   - On our side: Otherwise (V >= L), speak V from this point on.
 *     - Similarly, on ther side, if V >= Lp, they shall speak V from this point on.
 *
 * The role of Protocol_negotiator is simple:
 *   - It memorizes the local L and H as passed to its ctor, and it starts with V = UNKNOWN.  negotiated_proto_ver()
 *     always simply returns V which is the chief output of a `*this`.
 *   - Upon receiving Hp, user gives it to compute_negotiated_proto_ver(); this computes V based on the above
 *     algorithm; namely: `(H <= Hp) ? H : ((Hp >= L) ? Hp : UNSUPPORTED)`.  So negotiated_proto_ver() shall return
 *     that value (one of H, Hp, #S_VER_UNSUPPORTED) from then on.
 *     - compute_negotiated_proto_ver() shall not be called again.  One can check
 *     `negotiated_proto_ver() == S_VER_UNKNOWN`, if one would rather not independently keep track of whether
 *     compute_negotiated_proto_ver() has been called yet or not.
 *   - local_max_proto_ver_for_sending() shall return H once; after that UNSUPPORTED.  This is to encourage/help
 *     the sending-out of our H exactly once, no more.
 *
 * Couple notes:
 *   - If H = Hp, then everyone will agree on everything and just speak H.  This is perhaps most typical.
 *     Usually then L = Lp also, but it's conceivable they're not equal (e.g. one side has a backwards-compatibility
 *     patch but not the other).  It doesn't matter really.
 *   - Otherwise, though, there are 2 possibilities:
 *     - Both sides agree on the same V, with no V = UNSUPPORTED.  This occurs, if in fact there is overlap between
 *       [L, H] and [Lp, Hp], just Hp > H or vice versa: One gets to speak its "preferred" protocol, while the other
 *       has to speak an earlier version.
 *     - One side detects V = UNSUPPORTED (the other does not).  This occurs, if one side is so much newer than the
 *       other, it doesn't even have backwards-compatibility support for the other side.  (And/or perhaps the
 *       protocol does not attempt to ever be backwards-compatible; so H = L and Hp = Lp always.)
 *       - In this case one side will try to proceed; but it won't get far, as the other (more advanced) side
 *         will close the comm pathway instead of either sending or receiving anything beyond its H or Hp.
 *
 * We could have avoided the latter asymmetric situation by sending over both L and H (and they both Lp and
 * Hp); then both sides would do exactly the same computation.  However it seems an unnecessary complication
 * and payload.
 *
 * ### Key tip: Coding for version-1 versus one version versus multiple versions ###
 * Using a `*this` is in and ofi itself extremely simple; just look at the API and/or read the above.  What is somewhat
 * more subtle is how to organize your comm pathway's behavior around the start, when the negotiation occurs.
 * That part is also straightforward for the most part:
 *   - Before you send out your first stuff, or possibly together with it, send an encoding of
 *     local_max_proto_ver_for_sending().
 *   - When reading your first stuff, read the similar encoding from the opposing side.
 *     Now compute_negotiated_proto_ver() will determine negotiated_proto_ver(); call it V.  From this point on:
 *     - Anything you receive should be interpreted per protocol version V.
 *       - This is straightforward: since the stuff that makes it possible to determine V comes first, anytime
 *         you need to know how to interpret anything else, you'll know V already.
 *     - Anything you send should be according to protocol version V.
 *
 * That last part is the only subtle part.  The potential challenge is that if you want to send something, you may
 * not have gotten the protocol-negotiation in-message yet; in which case you do *not* know V.  If it is necessary
 * to know it at that point, then there's no choice but to enter a would-block state of sorts, during which time
 * any out-messages would need to be internally queued or otherwise deferred, until you do know V (have received
 * the first in-message and called compute_negotiated_proto_ver()).  Protocol_negotiator offers no help as such.
 *
 * However there are several common situations where this concern is entirely mitigated away.  If it applies to you,
 * then you will have no problem.
 *   - If `local_min_proto_ver == local_max_proto_ver` (to ctor, a/k/a L = H in the above algorithm sketch),
 *     meaning locally you support only one protocol version, then there is no ambiguity in any case.
 *     You know which protocol you'll speak.  The only ways there'd be a problem are: (1) compute_negotiated_proto_ver()
 *     determines incompatibility -- by which point the supposed problem is moot; and (2) the other side
 *     does the same -- in which case it will close the comm pathway (not our problem, by design).
 *     - In particular, if L = H = 1 -- in the first release of the protocol -- this is of course the case.
 *     - If you do not maintain backwards compatibility (e.g., L = H = 2, then next time L = H = 3, then... etc.),
 *       then this is also the case.
 *   - If L does not equal H, in many (most?) cases the actual negotiated version V does not affect many (most)
 *     messages sent.  E.g., if you have a "ping" message, perhaps it will never change in any version of the
 *     protocol.
 *     - So the problem only occurs when there's actual ambiguity about what to send.  Your code may still need to be
 *       somewhat more complex than the L=H case, but perf/responsiveness at least is less likely to be affected.
 *   - If your protocol has a built-in handshake/log-in/etc. phase (where one set side is expected to send
 *     a SYN-like thing, and the other side is supposed to reply with an ACK of some kind in response), then you
 *     can specifically:
 *     - Not add any version-dependent payload to the SYN-like (opening) message; or at least defer any such
 *       version-dependent interpretation thereof, until V is known shortly.
 *     - But, do include the version in the SYN-like (opening) message and to its ACK-like response, specifically.
 *       By the time the hand-shake completes, both sides know V and can proceed.
 *
 * @note In general, for the case of initial-protocol-release -- version 1 -- the usefulness of Protocol_negotiator
 *       minimal, but it does exist.  It is minimal, because *assuming* both sides promise to follow this
 *       algorithm, then *all* the code actually *needs* to do is: (1) send `H = 1` in some fashion that will never
 *       change in the future; (2) receive Hp in some fashion that will never change in the future either; and
 *       (3) explode, if Hp is not present or is not 1.  This is very much doable without Protocol_negotiator.
 *       However it is almost equally easy with Protocol_negotiator, too; and it *is a good practice* to leverage it,
 *       so that if there is a version 2 later, the conventions in force will be unambiguous.  Only then it *might*
 *       be necessary to worry about the subtle situation above, where we want to send something out whose expression
 *       depends on V, but we haven't received the protocol-negotiating in-message yet and hence don't know V yet.
 *
 * ### Safety, trust assumption ###
 * We emphasize that this is not authentication.  There is the implicit trust that the opposing side will be using
 * a very specific *protocol for determining the rest of the protocol* and trust us to do the same.  Furthermore
 * there's implicit trust that the protocol versions being referred-to mean the same thing on both sides.
 *
 * To establish this trust is well beyond the mission of `*this` class; and explaining how to do so is well beyond
 * this doc header.
 *
 * ### Thread safety ###
 * For the same `*this`, the standard default assumptions apply (mutating access disallowed concurrently with
 * any other access) with the following potentially important exception: local_max_proto_ver_for_sending() can
 * be executed concurrently with any other API except itself.  In other words, the outgoing-direction
 * (local_max_proto_ver_for_sending()) and incoming-direction work
 * (compute_negotiated_proto_ver(), negotiated_proto_ver()) can be safely performed independently/concurrently
 * w/r/t each other.
 */
class Protocol_negotiator :
  public flow::log::Log_context
{
public:
  // Types.

  /**
   * Type sufficient to store a protocol version; positive values identify newer versions of a protocol;
   * while non-positive values S_VER_UNKNOWN and S_VER_UNSUPPORTED are special values.
   */
  using proto_ver_t = int16_t;

  // Constants.

  /**
   * A #proto_ver_t value, namely zero, which is a reserved value indicating "unsupported version"; it is not
   * a valid version number identifying a protocol that can actually be spoken by relevant software.  Its specific
   * meaning is identified specifically where it might be returned or taken by the API.
   */
  static constexpr proto_ver_t S_VER_UNSUPPORTED = 0;

  /**
   * A #proto_ver_t value, namely a negative one, which is a reserved value indicating "unknown version"; it is not
   * a valid version number identifying a protocol that can actually be spoken by relevant software.  Its specific
   * meaning is identified specifically where it might be returned or taken by the API.
   */
  static constexpr proto_ver_t S_VER_UNKNOWN = -1;

  // Constructors/destructor.

  /**
   * Constructs a comm pathway's negotiator object in initial state wherein: (1) negotiated_proto_ver()
   * returns #S_VER_UNKNOWN (not yet negotiated with opposing Protocol_negotiator); and (2) we've not yet
   * sent `local_max_proto_ver` to opposing side via first being queried using local_max_proto_ver_for_sending().
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname
   *        String to use subsequently in logging to identify `*this`.
   * @param local_max_proto_ver
   *        The highest version of the protocol the comm pathway can speak; a/k/a the preferred version.
   *        Positive or undefined behavior (assertion may trip).
   * @param local_min_proto_ver
   *        The lowest version of the protocol the comm pathway can speak; so either `local_max_proto_ver`
   *        (if our side has no backwards compatibility) or the oldest version with which we are backwards-compatible.
   *        Positive and at most `local_max_proto_ver` or undefined behavior (assertion may trip).
   */
  explicit Protocol_negotiator(flow::log::Logger* logger_ptr, util::String_view nickname,
                               proto_ver_t local_max_proto_ver, proto_ver_t local_min_proto_ver);

  // Methods.

  /**
   * Returns `S_VER_UNKNOWN` before compute_negotiated_proto_ver(); then either the positive version of the protocol
   * we shall speak subsequently, or `S_VER_UNSUPPORTED` if the two sides are incompatible.
   *
   * @return See above.
   */
  proto_ver_t negotiated_proto_ver() const;

  /**
   * Based on the presumably-just-received-from-opposing-side value of their `local_max_proto_ver`, passed-in as
   * the arg, calculates the protocol version we shall speak over the comm pathway, so it will be returned by
   * negotiated_proto_ver() subsequently.  Returns `true`; unless it has already been called -- in which case
   * it is a no-op (outside of logging); returns `false` (unless exception thrown; see next paragraph).
   *
   * If it is not a no-op, and the result is that the two sides lack a protocol version they can both speak,
   * a suggested truthy `Error_code` is emitted using standard Flow error-emission convention.
   *
   * Tip: It's a reasonable tactic to potentially call this for every in-message, if you only do so after ensuring
   * `negotiated_proto_ver() == S_VER_UNKNOWN`.  That will effectively ensure the negotiation occurs ASAP and at most
   * once.
   *
   * More formally:
   *   - If `negotiated_proto_ver() != S_VER_UNKNOWN`: does nothing except possibly logging; returns `false`.
   *     Informal tip: If you don't know whether that's the case, check for it via negotiated_proto_ver() and
   *     neither try to parse `opposing_max_proto_ver` from your in-message, nor call the present method.
   *   - If `negotiated_proto_ver() == S_VER_UNKNOWN` (the case right after construction): Upon return
   *     `negotiated_proto_ver() != S_VER_UNKNOWN` and:
   *     - If `err_code != nullptr`: `*error_code` is set to truthy suggested error to emit if applicable.
   *       Returns `true`.
   *     - If `err_code == nullptr`: a truthy suggested error is emitted via exception.
   *
   * @param opposing_max_proto_ver
   *        Value that the opposing Protocol_negotiator (or equivalent) sent to us over pathway:
   *        their `local_max_proto_ver`.  Any value is allowed (we will check that it's positive and
   *        report negotiation failure if not).  Informal advice: if you were unable to parse
   *        it from your in-message, you should pass S_VER_UNKNOWN.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_TOO_OLD (incompatible protocol version -- we're more
   *        advanced and lack backwards-compatibility for their preferred version),
   *        error::Code::S_PROTOCOL_NEGOTIATION_OPPOSING_VER_INVALID (`opposing_max_proto_ver` is invalid: not
   *        positive).
   * @return `false` if pre-condition was `negotiated_proto_ver() != S_VER_UNKNOWN`, so we no-oped;
   *         `true` otherwise (unless exception thrown, only if `err_code == nullptr`.
   */
  bool compute_negotiated_proto_ver(proto_ver_t opposing_max_proto_ver, Error_code* err_code = 0);

  /**
   * To be called at most once, this returns `local_max_proto_ver` from ctor the first time and
   * #S_VER_UNKNOWN subsequently.
   *
   * Tip: It's a reasonable tactic to call this when about to send any out-message; if it returns
   * #S_VER_UNKNOWN, then you've already sent it and don't need to do so now; otherwise encode this value
   * before/with the out-message and send it.
   *
   * @return See above.  Either a positive version number or #S_VER_UNKNOWN.
   */
  proto_ver_t local_max_proto_ver_for_sending();

  /**
   * Resets the negotiation state, meaning back to the state as-if just after ctor invoked.  Hence:
   * negotiated_proto_ver() yields #S_VER_UNKNOWN, while
   * local_max_proto_ver_for_sending() would yield not-`S_VER_UNKNOWN`.
   */
  void reset();

private:
  // Data.

  /// The `nickname` from ctor.  Not `const` so as to support copyability.
  std::string m_nickname;

  /// `local_max_proto_ver` from ctor.  Not `const` so as to support copyability.
  proto_ver_t m_local_max_proto_ver;

  /// `local_min_proto_ver` from ctor.  Not `const` so as to support copyability.
  proto_ver_t m_local_min_proto_ver;

  /// Init value `false` indicating has local_max_proto_ver_for_sending() has not been called; subsequently `true`.
  bool m_local_max_proto_ver_sent;

  /// See negotiated_proto_ver().
  proto_ver_t m_negotiated_proto_ver;
}; // class Protocol_negotiator

} // namespace ipc::transport
