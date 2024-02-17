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

#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <flow/log/config.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/move/make_unique.hpp>

namespace ipc::transport
{
// Types.

/**
 * Internal implementation of Blob_stream_mq_base class template; and common utilities used by
 * Blob_stream_mq_sender_impl and Blob_stream_mq_receiver_impl (`static` items only as of this writing).
 *
 * @tparam Persistent_mq_handle
 *         See Persistent_mq_handle concept doc header.
 */
template<typename Persistent_mq_handle>
class Blob_stream_mq_base_impl
{
public:
  // Types.

  /// Short-hand for template arg for underlying MQ handle type.
  using Mq = Persistent_mq_handle;

  /**
   * Persistent_mq_handle holder that takes a deleter lambda on construction, intended here to perform additional deinit
   * steps in addition to closing the #Mq by deleting it.  Used by ensure_unique_peer() machinery.
   */
  using Auto_closing_mq = boost::movelib::unique_ptr<Mq, Function<void (Mq*)>>;

  // Methods.

  /**
   * See Blob_stream_mq_base counterpart.
   *
   * @param logger_ptr
   *        See Blob_stream_mq_base counterpart.
   * @param name
   *        See Blob_stream_mq_base counterpart.
   * @param err_code
   *        See Blob_stream_mq_base counterpart.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& name, Error_code* err_code);

  /**
   * See Blob_stream_mq_base counterpart.
   *
   * @tparam Handle_name_func
   *         See Blob_stream_mq_base counterpart.
   * @param handle_name_func
   *        See Blob_stream_mq_base counterpart.
   */
  template<typename Handle_name_func>
  static void for_each_persistent(const Handle_name_func& handle_name_func);

  /**
   * Internal helper for Blob_stream_mq_sender and Blob_stream_mq_receiver that operates both the start and end
   * of the anti-dupe-endpoint machinery used by those 2 classes to prevent more than 1 `_sender` and more than 1
   * `_receiver` for a given underlying MQ.
   *
   * The input is a handle to the MQ, as created by user as of this writing, either on the sender or receiver
   * end (`snd_else_rcv`).  The output is one of:
   *   - a null return, indicating that either there's already a sender (or receiver) already registered (typically),
   *     or some other (unlikely) system error.  `*err_code` will indicate what happened; or
   *   - a non-null returned `unique_ptr` that has moved `mq` into itself/taken over ownership of it, with certain
   *     machinery such that when that `unique_ptr` (or moved version thereof) is `.reset()` or destroyed,
   *     the proper deinit will run, making it possible to create another MQ at the same Shared_name.
   *
   * `mq` is untouched if null is returned.
   *
   * @param logger_ptr
   *        Logger to use subsequently. Errors are logged as WARNING; otherwise nothing of INFO or higher verbosity
   *        is logged.
   * @param mq
   *        See above.
   * @param snd_else_rcv
   *        `true` if this is the sender side; else the receiver side.
   * @param err_code
   *        Must not be null, or behavior undefined; will be set to success if non-null returned or reason for failure
   *        otherwise.
   * @return null or pointer to new Persistent_mq_handle moved-from `mq`, and with deinit deleter.
   */
  static Auto_closing_mq ensure_unique_peer(flow::log::Logger* logger_ptr, Mq&& mq, bool snd_else_rcv,
                                            Error_code* err_code);

protected:
  // Types.

  /**
   * If Blob_stream_mq_sender_impl sends an empty message, in NORMAL state Blob_stream_mq_receiver enters CONTROL
   * state and expects one of these values in the next message, to react as documented per `enum` value
   * (upon re-entering NORMAL state).
   *
   * ### Rationale: Why is the underlying type signed? ###
   * There is a special type of CONTROL command used only at the beginning of the conversation: protocol negotiation;
   * each side sends to the other the highest protocol version it can speak.  In that case instead of using one of
   * the `enum` encodings it sends the encoding of the inverse of the version number which is always positive
   * (so -1 means version 1, -2 means 2, etc.).  Using a signed type makes working with that eventuality a little
   * easier and more expressive.  (No, we don't really expect the `enum` variants to anywhere close to all these
   * bits anyway.)
   *
   * ### Rationale: Why 64 bits? ###
   * No huge reason.  It felt prudent to leave a bit of reserved space, maybe for forward compatibility.
   * CONTROL messages are rare, so it should not affect perf.
   */
  enum class Control_cmd : int64_t
  {
    /// Indicates sender user invoked Blob_sender::end_sending() (graceful close).  Emit/queue graceful-close to user.
    S_END_SENDING,

    /**
     * Indicates sender user invoked auto_ping() earlier, and this is an incoming periodic ping.  If
     * receiver user invoked idle_timer_run() earlier, reset countdown to death due to lacking in-pings.
     */
    S_PING,

    /// Sentinel: not a valid value.  May be used to, e.g., ensure validity of incoming value of allegedly this type.
    S_END_SENTINEL
  }; // enum class Control_cmd

private:
  // Methods.

  /**
   * Name of sentinel SHM pool created by ensure_unique_peer() and potentially cleaned up by remove_persistent().
   *
   * @param mq_name
   *        `mq.absolute_name()`.  See ensure_unique_peer().
   * @param snd_else_rcv
   *        See ensure_unique_peer().
   * @return Absolute name.
   */
  static Shared_name mq_sentinel_name(const Shared_name& mq_name, bool snd_else_rcv);
}; // class Blob_stream_mq_base_impl

// Template implementations.

template<typename Persistent_mq_handle>
void Blob_stream_mq_base_impl<Persistent_mq_handle>::remove_persistent(flow::log::Logger* logger_ptr, // Static.
                                                                       const Shared_name& name, Error_code* err_code)
{
  using util::remove_persistent_shm_pool;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { remove_persistent(logger_ptr, name, actual_err_code); },
         err_code, "Blob_stream_mq_base_impl::remove_persistent()"))
  {
    return;
  }
  // else
  assert(err_code);

  // This part is the no-brainer:
  Error_code mq_err_code;
  Mq::remove_persistent(logger_ptr, name, &mq_err_code);

  /* This part is what is needed on top of the above, because Blob_stream_mq_*er also creates these little pools
   * to keep track of uniqueness (that there's only one sender and receiver per MQ). */
  Error_code sentinel_err_code1;
  remove_persistent_shm_pool(logger_ptr, mq_sentinel_name(name, true), &sentinel_err_code1);
  Error_code sentinel_err_code2;
  remove_persistent_shm_pool(logger_ptr, mq_sentinel_name(name, false), &sentinel_err_code2);

  // Report an error <=> any one of three failed; but the key MQ's error (if it occurred) "wins."
  *err_code = mq_err_code ? mq_err_code
                          : (sentinel_err_code1 ? sentinel_err_code1 : sentinel_err_code2);
} // Blob_stream_mq_base_impl::remove_persistent()

template<typename Persistent_mq_handle>
template<typename Handle_name_func>
void // Static.
  Blob_stream_mq_base_impl<Persistent_mq_handle>::for_each_persistent(const Handle_name_func& handle_name_func)
{
  // Do exactly what we promised in contract.
  Mq::for_each_persistent(handle_name_func);

  /* Discussion: Is what we promised actually the right thing to promise though?  Answer: Yes and no.  Or:
   * Yes, within reason.  ensure_unique_peer(), in the deleter it generated/returns in the Auto_closing_mq,
   * deletes the MQ and both sentinels.  (The other-direction ensure_unique_peer() then loses the race and has
   * nothing to delete in its Auto_closing_mq; and that's fine.)  The deleter first removes the sentinels, then
   * the MQ; so even if the abort occurred *during* the deleter, listing the still-existing MQs now should
   * yield all MQs for which there are still resources to delete (at least the MQ entry; and possibly 0, 1, or 2
   * sentinels, depending on when the abort occurred (probably 2, in most cases, unless unlucky).
   *
   * We could also list all SHM pools matching the naming convention we use in mq_sentinel_name(), deducing
   * the MQ name from any sentinel name found and adding that to the list we emit.  Frankly I (ygoldfel) simply
   * don't want to.  This is a best-effort cleanup for abort situation only; and we do remove the MQ last
   * in the deleter, so everything should work out fine this way, and the code is simple. */
} // Blob_stream_mq_base_impl::for_each_persistent()

template<typename Persistent_mq_handle>
typename Blob_stream_mq_base_impl<Persistent_mq_handle>::Auto_closing_mq
  Blob_stream_mq_base_impl<Persistent_mq_handle>::ensure_unique_peer(flow::log::Logger* logger_ptr, // Static.
                                                                     Mq&& mq, bool snd_else_rcv,
                                                                     Error_code* err_code)
{
  using util::remove_persistent_shm_pool;
  using flow::error::Runtime_error;
  using flow::log::Sev;
  using boost::system::system_category;
  using boost::movelib::make_unique;
  using bipc::shared_memory_object;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT); // @todo Maybe just make this method non-static.
  assert(err_code);

  /* Read Blob_stream_mq_sender class doc header (particularly regarding lifetimes of things); then return here.
   * Summarizing:
   *
   * We've promised to enforce certain semantics, namely that a given MQ can be accessed, ever, by
   * at most 1 Blob_stream_mq_sender (and 1 _receiver) -- across all processes, not just this one.
   * So we do that here by maintaining a separate SHM-stored record indicating a _sender (or _receiver) exists already.
   *
   * Also we have to make it so that if _sender dtor (or _receiver dtor,
   * whichever happens first) runs, within a short amount of time after that, they can again open a *new*
   * channel at the same Shared_name.  (In a sense we're trying to make our _sender/_receiver pair similar to
   * a peer-pair Unix domain socket connection: once you close it: only two sides to it (though ours is 1-directional);
   * and destroying either side hoses the connection.)  To do that, we put a custom deleter on the MQ unique_ptr:
   * once it goes away (whether in dtor, or earlier due to fatal error when sending/whatever), we have to "undo"
   * whatever mechanism is guarding against multiple `_sender`s (or `_receiver`s) at that Shared_name. */

  /* First atomically attempt to create a tiny dummy SHM pool whose existence indicates a Blob_stream_mq_....er
   * to that absolute_name() exists.  Its name: */

  const auto sentinel_name = mq_sentinel_name(mq.absolute_name(), snd_else_rcv);

  FLOW_LOG_TRACE("MQ handle [" << mq << "]: Atomic-creating sentinel SHM-pool [" << sentinel_name << "] "
                 "to ensure no more than 1 [" << (snd_else_rcv ? "snd" : "rcv") << "] side of pipe exists.");
  try
  {
    /* Permissions note: leave at default (*nix: 644, meaning read/write by this user, read only for everyone else).
     * However this controls writing to the pool itself -- but no one will be writing to the pool; it is only a
     * sentinel by its existence.  Hence maybe we could restrict it even more, but probably it is not important.
     * @todo For good measure look into restricting it more; maybe even 000. */
    [[maybe_unused]] auto shm_pool_sentinel
      = make_unique<shared_memory_object>(util::CREATE_ONLY, sentinel_name.native_str(), bipc::read_only);
    // On success it is immediately destroyed... but the name, and tiny SHM pool, lives on until deleter below execs.
  }
  catch (const bipc::interprocess_exception& exc)
  {
    const auto native_code_raw = exc.get_native_error();
    const auto bipc_err_code_enum = exc.get_error_code();
    const bool is_dupe_error = bipc_err_code_enum == bipc::already_exists_error;
    FLOW_LOG_WARNING("MQ handle [" << mq << "]: While creating [" << (snd_else_rcv ? "send" : "receiv") << "er] "
                     "was checking none already exists in any process, as this is a one-directional pipe; "
                     "tried to create SHM sentinel pool [" << sentinel_name << "]; "
                     "bipc threw interprocess_exception; will emit some hopefully suitable Flow-IPC Error_code; "
                     "probably it's already-exists error meaning dupe-sender; "
                     "but here are all the details of the original exception: native code int "
                     "[" << native_code_raw << "]; bipc error_code_t enum->int "
                     "[" << int(bipc_err_code_enum) << "]; latter==already-exists = [" << is_dupe_error << "]; "
                     "message = [" << exc.what() << "].");
    *err_code = is_dupe_error ? Error_code(snd_else_rcv
                                             ? error::Code::S_BLOB_STREAM_MQ_SENDER_EXISTS
                                             : error::Code::S_BLOB_STREAM_MQ_RECEIVER_EXISTS)
                              : Error_code(errno, system_category());
    return Auto_closing_mq();
  } // catch (bipc::interprocess_exception)
  // Got here: OK, no dupe, no other problem.
  err_code->clear();

  /* Now take over their MQ handle.  As noted above set up the auto-closing extra behavior to keep above scheme working,
   * if they want to reuse the name to make another MQ later (although informally we don't recommend it -- much
   * like, conceptually, binding a TCP socket to the same port soon after closing another can bring problems). */
  return Auto_closing_mq(new Mq(std::move(mq)), // Now we own the MQ handle.
                         [get_logger, get_log_component, snd_else_rcv, sentinel_name](Mq* mq_ptr)
  {
    const auto other_sentinel_name = mq_sentinel_name(mq_ptr->absolute_name(), !snd_else_rcv);

    FLOW_LOG_INFO("MQ handle [" << *mq_ptr << "]: MQ handle ([" << (snd_else_rcv ? "send" : "receiv") << "er] "
                  "side of pipe) about to close.  Hence by contract closing pipe itself too.  "
                  "To prevent any attempt to reattach to underlying MQ "
                  "we make MQ anonymous by removing persistent-MQ-name [" << mq_ptr->absolute_name() << "].  "
                  "Then removing sentinel SHM-pool [" << sentinel_name << "] to enable reuse of name for new MQ; "
                  "as well as the other-direction sentinel SHM-pool [" << other_sentinel_name << "].");

    /* (If sender does it first, receiver will fail here; and vice versa.  Just eat all logging (null logger);
     * and ignore any errors below. Nothing we can do anyway.) */
    Error_code sink;

    // Blob_stream_mq_*er<Mq>(Mq(mq.absolute_name())) would fail until we do:

    remove_persistent_shm_pool(nullptr, sentinel_name, &sink);
    remove_persistent_shm_pool(nullptr, other_sentinel_name, &sink);

    // Mq(Open_only, mq.absolute_name()) would now succeed.  Mq(Create_only, mq.absolute_name()) would now fail.
    Mq::remove_persistent(nullptr, mq_ptr->absolute_name(), &sink);
    // Mq(Open_only, mq.absolute_name()) would now fail.  Mq(Create_only, mq.absolute_name()) would now succeed.

    /* Discussion:
     *   - The order (sentinels versus MQ itself) is ~reversed compared to the creation, where MQ is made first.
     *     In general that's a good idea (all else being equal); but specifically it could help avoid a corner
     *     case involving post-abort cleanup using for_each_persistent() and remove_persistent(): If an abort
     *     occurs during the code you're reading now, the thing on which for_each_persistent() is keyed
     *     (listing of the MQs) is the last thing removed.  Otherwise the sentinel(s) could get leaked, if abort
     *     occurs after MQ removed but before the sentinel(s) are; for_each_persistent() won't detect it/them.
     *
     * - Is it weird we *created* sentinel_name pool but are deleting both it and the one that would've been
     *   created by the other-direction peer's invocation of the present function?  It does seem weird, but it's
     *   actually good and proactive; and safe.
     *   - Is it safe?  Answer: If the other sentinel exists still, then the other-direction peer dtor has not yet run.
     *     The sentinel is only useful if they try to create (open-only) another other-direction peer for the same name.
     *     That will fail in any case though: we just removed the MQ!  If they try to create such a peer in
     *     create mode, then they are breaking the contract wherein they must not reuse a name until both dtors
     *     have executed (and even then, it is *recommended* a name is never reused, or not anytime soon at least).
     *     So yes, it is safe, in that the only reason the other-direction sentinel would even be checked does not
     *     validly exist at this stage.
     *   - Is it beneficial?  Answer: Yes, because it means in practice all 3 persistent items will be deleted at
     *     essentially the same time.  This helps avoid an on-abort leak mentioned in for_each_persistent(). */

    // Lastly... don't forget:

    delete mq_ptr;
  }); // return make_unique<Mq>()
} // Blob_stream_mq_base_impl::ensure_unique_peer()

template<typename Persistent_mq_handle>
Shared_name Blob_stream_mq_base_impl<Persistent_mq_handle>::mq_sentinel_name(const Shared_name& mq_name,
                                                                             bool snd_else_rcv)
{
  using util::String_view;

  auto sentinel_name
    = build_conventional_non_session_based_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM); // It's a (silly) SHM pool.
  // That was the standard stuff.  Now the us-specific stuff:
  sentinel_name /= Mq::S_RESOURCE_TYPE_ID; // Differentiate between different MQ types.
  sentinel_name /= "sentinel";
  sentinel_name += String_view(snd_else_rcv ? "Snd" : "Rcv");
  assert(mq_name.absolute());
  sentinel_name += mq_name; // The MQ name -- ALL of it -- is completely in here (starting with separator).

  return sentinel_name;
}

} // namespace ipc::transport
