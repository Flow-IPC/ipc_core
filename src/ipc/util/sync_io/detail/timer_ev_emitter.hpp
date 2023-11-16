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

#include "ipc/util/util_fwd.hpp"
#include <flow/async/single_thread_task_loop.hpp>

namespace ipc::util::sync_io
{

/**
 * An object of this type, used internally to implement `sync_io`-pattern objects that require timer events,
 * starts a thread dedicated exclusively to running timer waits on the `sync_io` object's behalf, so that when
 * such a timer fires, it emits a pipe-readable event to be detected by the user's event loop, which it then reports to
 * `sync_io` object.
 *
 * @see ipc::util::sync_io for discussion of the `sync_io` pattern.  This is necessary background for the present
 *      class which is used in implementing objects within that pattern.
 *
 * ### Rationale ###
 * In the `sync_io` pattern, suppose there is ipc::transport or ipc::session object X (e.g.,
 * ipc::transport::sync_io::Native_socket_stream::Impl).  By definiton of the `sync_io` pattern, X itself
 * endeavours to not async-wait on various events itself, in its own threads it might start, but rather
 * instructs the user to wait on Native_handle being readable or writable and to inform X of such an event
 * (see sync_io::Event_wait_func doc header).  For example `Native_socket_stream::Impl`, when awaiting in-traffic,
 * uses #Event_wait_func to inform itself when the Unix-domain socket has become readable -- the user uses
 * their own `[e]poll*()` or boost.asio loop or ... to detect the readability and then informs X when this happens.
 *
 * Some objects, in particular at least various `Blob_receiver`s and `Blob_sender`s, also at times wait on
 * timer events; in implementing transport::Blob_receiver::idle_timer_run() and transport::Blob_sender::auto_ping()
 * respectively.  For example if the idle timer fires, then `Blob_receiver` X needs to know; and if there is
 * an outstanding `Blob_receiver::async_receive_blob()`, it should not continue being outstanding but rather:
 * (1) The user's event loop -- which might otherwise be in the middle of a blocking `epoll_wait()` -- must
 * exit any such wait; and (2) X should be informed, in this case so the outstanding receive-op would complete
 * with the idle-timeout error, detectable by the user's event loop before the next `epoll_wait()`.
 *
 * But how to even accompish (1)?  ((2) is relatively simple in comparison.)  Possibilities:
 *   - The user can be told of the next-soonest timer's expiration time, so that the `epoll_wait()` (or `poll()` or
 *     ...) can be set to time out at that time, latest.
 *     - This is viable but presents a tricky API design problem for the `sync_io` pattern.  The next-soonest
 *       timer expiration time will change over time, as new timers pop or existing timers are rescheduled.
 *       #Event_wait_func semantics have to be expanded beyond us specifying a mere (FD, read-vs-write flag)
 *       pair; it would now specify a timer, and somehow the next-soonest timer across *all* `sync_io` objects
 *       in use by the user (not just X alone) would need to be computable.  Doable but much more complex
 *       than a set of decoupled FD+flag pairs, one per #Event_wait_func invocation.
 *   - The OS can be somehow told to tickle a dedicated-to-this-purpose FD (per timer) at a particular time,
 *     while #Event_wait_func is used (same as it is for Unix domain sockets, etc.) to wait for readability of
 *     that FD.  `epoll_wait()` (or ...) would wake up like on any other event and inform X, and X would even
 *     know which timer it is, by being informed via `Event_wait_func`'s' `on_active_ev_func` arg.
 *
 * The latter option is much more attractive in terms of API simplicity from the user's point of view (at least).
 * So we go with that.  Having make this decision, how to actually make it work?  Possibilities:
 *   - The OS has these mechanisms.  Linux, at least, has `timerfd_create()` and buddies, specifically for this.
 *     One can schedule a timer, so the kernel keeps track of all that and does it in the background; when
 *     ready the FD becomes readable.  Done.
 *     - This is a very reasonable approach.  However it is not very portable, and the API is hairy enough to
 *       require a bunch of (internal to us) wrapping code: specifying the time along the proper clock,
 *       rescheduling, canceling....
 *   - boost.asio, of course, has `flow::util::Timer` (a/k/a `waitable_timer`).  `Timer::async_wait(F)` starts
 *     a wait, invoking `F` in whatever thread(s) are running the `Timer`'s attached `Task_engine`
 *     (a/k/a `io_context`); `Timer::expires_after()` (re)schedules the timer; etc.
 *     - This is a much nicer API, and it is portable and oft-used in the Flow world.  The only problem is it
 *       is not attached -- in a public way -- to any particular FD.  Although boost.asio source shows
 *       internally `timerfd_create()` is used (at least when certain compile-time flags are set),
 *       there's no `Timer::native_handle()` that exposes the timer FD.  That said `F()` can do whatever we
 *       want... including pinging some simple (internally-setup) IPC mechanism, like an anonymous pipe,
 *       whose read-end is accessed via an FD.
 *
 * Bottom line, we went with the latter approach.  It is more elegant.  It does, however, require two things
 * which the kernel-timer approach does not:
 *   - The IPC mechanism.  We use a pipe (per timer).  boost.asio even provides portable support for this.
 *   - A background thread in which `F()` would write a byte to this pipe, when the timer fires.
 *     - Threads are easy to set up for us.  Isn't this a performance problem though?  Answer: No; at least not
 *       in our use case.  The overhead is in 1, the context switch *at the time a timer actually fires* and
 *       2, the pipe write and resulting read.  These theoretically add latency but *only* when a timer
 *       fires.  If we were using timers for something frequent -- like, I don't know, packet pacing -- this
 *       would be an issue.  We do not: idle timers and auto-ping timers fire at worst seconds apart.
 *       There is no real latency impact there over time.
 *
 * So there you have it.  We need:
 *   - A thread for 1+ timers.  E.g., typically, one object X would require one thread for all its timers.
 *   - An unnamed pipe per timer.  The thread would write to the write end, while object X would read from
 *     the read end and give the read-end FD to the user to wait-on via #Event_wait_func.
 *   - Some simple logic such that when a given timer fires, the handler (in the aforementioned thread)
 *     merely writes a byte to the associated pipe.
 *
 * Timer_event_emitter supplies those things.
 *
 * ### How to use ###
 * Construct Timer_event_emitter.  This will start an idle thread (and it will remain totally idle with the
 * sole exception of a pipe-write executing when a timer actually fires).
 *
 * Call create_timer().  This just returns a totally normal `flow::util::Timer` to be saved in the
 * `sync_io`-pattern-implementing object.  create_timer() merely associates the `*this` thread with that
 * timer, so that when `Timer::async_wait(F)` eventually causes `F()` to execute, it will execute `F()`
 * in that thread.
 *
 * Call create_timer_signal_pipe() (for each timer one plans to use).  This creates a pipe; saves both ends
 * inside `*this`; and returns a pointer to the read-end.  The `sync_io`-pattern object saves this,
 * including so that it can pass the associated Native_handle (FD) to #Event_wait_func, whenever it
 * begins a timer-wait.
 *
 * Use the `Timer` completely as normal, including `Timer::expires_after()` to schedule and reschedule
 * the firing time... with one exception:
 *
 * Instead of the usual `Timer::async_wait()`, call instead timer_async_wait(), which takes:
 *   - Pointer to the `Timer`.  `*this` will `->async_wait(F)` on that `Timer`.
 *   - Pointer to the associated pipe read-end.  `*this` will use it as a key to determine the associated
 *     write-end.  `F()` will simply perform the 1-byte-write to that write-end.
 *
 * (timer_async_wait() is, basically, a convenience.  We could instead have had create_timer_signal_pipe()
 * return both the read-end and write-end; and had the `*this` user memorize both and itself do
 * the `Timer::async_wait()`, with the handler writing to the write-end.  It is nicer to keep that stuff
 * to `*this`.)
 *
 * Lastly, when #Event_wait_func `on_active_ev_func()` informs the `sync_op`-pattern-implementing object
 * of a timer-associated FD being readable, it must read the byte off the pipe by calling
 * consume_timer_firing_signal().  (Again, it's just a convenience.  We could have had the user object
 * read the byte themselves.  It is nicer to keep the protocol inside `*this`.)
 *
 * ### Thread safety note ###
 * A given `*this` itself has boring thread-safety properties: you may not call a non-`const` method
 * while calling another method.  That, however, is unlikely to be interesting.
 *
 * It is not safe to concurrently call `timer_async_wait(&T)`, where `T` is a `Timer`, with any other
 * method of `T`.  You can think, in this context, of `timer_async_wait(&T)` as being equivalent to
 * `T->async_wait()`.
 */
class Timer_event_emitter :
  public flow::log::Log_context,
  private boost::noncopyable // And non-movable.
{
public:
  // Types.

  /**
   * Object representing the read end of IPC mechanism, where readable status indicates the associated
   * timer_async_wait() call has resulted in the timer firing.  Formally the `*this` user shall perform only
   * the following operations on such an object R, after obtaining it from create_timer_signal_pipe():
   *   - Load your Asio_waitable_native_handle `E` via `E.assign(Native_handle(R.native_handle())`
   *     or equivalent.
   *   - Call `Event_wait_func`, passing in `&E`, just ahead of Timer_event_emitter::timer_async_wait().
   *   - Call `consume_timer_firing_signal(&R)` in the handler for the so-registered-via-`Event_wait_func`
   *     event (meaning, if the timer indeed fired).
   */
  using Timer_fired_read_end = boost::asio::readable_pipe;

  // Constructors/destructor.

  /**
   * Constructs emitter, creating idle thread managing no timers.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname_str
   *        Human-readable nickname of the new object, as of this writing for use in `operator<<(ostream)` and
   *        logging only.
   */
  explicit Timer_event_emitter(flow::log::Logger* logger_ptr, String_view nickname_str);

  // Methods.

  /**
   * Creates idle timer for use with timer_async_wait() subsequently.  It is associated with the thread
   * started in the ctor, meaning completion handlers shall execute in that thread.
   *
   * Formally, behavior is undefined if T is the returned timer or one moved-from it, and one invokes
   * `T.async_wait()` on it; you must use `timer_async_wait(&T, ...)` instead.  You may call other `T`
   * methods; in particular the `expires_*()` mutators will be useful.  You may in particular call
   * `T.cancel()` and destroy `T`.  However see timer_async_wait() regarding the effects of these various
   * methods/calls.
   *
   * @return See above.
   */
  flow::util::Timer create_timer();

  /**
   * Creates, and internally stores, an IPC mechanism instance intended for use with a given create_timer()-returned
   * `Timer`; returns pointer to the read-end of this mechanism.
   *
   * The returned object is valid through `*this` lifetime.
   *
   * @see #Timer_fired_read_end doc header for instructions on the acceptable/expected uses of the returned object.
   *
   * @return See above.
   */
  Timer_fired_read_end* create_timer_signal_pipe();

  /**
   * To be used on a timer `T` returned by create_timer(), this is the replacement for the usual
   * `T.async_wait(F)` call that loads a completion handler to execute at the expiration time.  Instead of
   * supplying the completion handler `F()`, one supplies a read-end returned by create_timer_signal_pipe();
   * when the timer fires a payload shall be written to the associated write-end.  You must call
   * your #Event_wait_func, passing it an Asio_waitable_native_handle, wrapping the handle in the same `*read_end`,
   * ahead of this timer_async_wait() call.
   *
   * You may not call timer_async_wait() on the same `*read_end` until one of the following things occurs:
   *   - The timer fires; and the user event loop's async-wait indeed wakes up and passes that to your object;
   *     and your object therefore invokes `consume_timer_firing_signal(read_end)`.
   *   - The async-wait is *successfully* canceled: `T.cancel()` or `T.expires_*()` returns non-zero (in fact, 1).
   *
   * @param timer
   *        The timer to arm.
   * @param read_end
   *        A value returned by create_timer_signal_pipe().
   */
  void timer_async_wait(flow::util::Timer* timer, Timer_fired_read_end* read_end);

  /**
   * Must be called after a timer_async_wait()-triggered timer fired, before invoking that method for the same
   * `Timer` again.  It reads the small payload that was written at the time of the timer's firing (in
   * the internal-IPC mechanism used for this purpose).
   *
   * @param read_end
   *        See timer_async_wait() and create_timer_signal_pipe().
   */
  void consume_timer_firing_signal(Timer_fired_read_end* read_end);

  /**
   * Returns nickname as passed to ctor.
   * @return See above.
   */
  const std::string& nickname() const;

  // Data.

  /// Nickname as passed to ctor.
  const std::string m_nickname;

private:
  // Data.

  /// The thread where (only) timer-firing events (from create_timer()-created `Timer`s) execute.
  flow::async::Single_thread_task_loop m_worker;

  /**
   * The readers (never null) returned by create_timer_signal_pipe().  The order is immaterial, as
   * this is just a place #m_signal_pipe_writers map keys can point into.  Wrapped into `unique_ptr`, so that
   * the address of a #Timer_fired_read_end (as returned by create_timer_signal_pipe()) remains valid until `*this`
   * dies.
   */
  std::vector<boost::movelib::unique_ptr<Timer_fired_read_end>> m_signal_pipe_readers;

  /**
   * Stores the write-ends of the pipes created in create_timer_signal_pipe(), indexed by
   * pointer to their respective read-ends.  timer_async_wait() can therefore look-up a write-end
   * based on the read-end pointer it gave to the user, which the user must pass-to timer_async_wait().
   */
  boost::unordered_map<Timer_fired_read_end*,
                       boost::movelib::unique_ptr<boost::asio::writable_pipe>> m_signal_pipe_writers;
}; // class Timer_event_emitter

// Free functions: in *_fwd.hpp.

} // namespace ipc::util::sync_io
