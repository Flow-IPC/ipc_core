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
#include <boost/shared_ptr.hpp>

/**
 * Contains common code, as well as important explanatory documentation in the following text, for the `sync_io`
 * pattern used in ipc::transport and ipc::session to provide fine-tuned control over integrating
 * asynchronous Flow-IPC activities into the user's event loop.
 *
 * ### What's all this, then? ###
 * You may notice that for ~every class/class template `X` in this library that provides 1+ `X::async_*(..., F)`
 * method(s), where `F()` is a completion handler for an async operation, there exists also -- in a sub-namespace
 * named `sync_io` -- a similar-looking type `sync_io::X`.
 *
 * @note At times, generally in ipc::session and class template ipc::transport::struc::Channel specifically, async
 *       ops don't necessarily use `async_*` naming, but the point remains that: an operation occurs in some
 *       sense in the background, and then a *handler* function, given as an arg, is called when the operation is
 *       completed.  Neverthelss for this discussion we will often take the case of, indeed, an operation
 *       named `X::async_*(..., F)`, with `F()` a one-off completion handler.  It's the basic, most mainstream pattern.
 *
 * Long story short, all else being equal, we would recommend the use of `X`
 * over `sync_io::X`: it is easier (in most cases), less error-prone, and delivers
 * similar performance -- quite possibly even better (due to automatic parallelization albeit at the cost
 * of mandatory context-switching).  However `sync_io::X` does exist for a good reason, at least for some important
 * `X`es, and in an advanced, highly peformance-sensitive application it may be worth considering switching
 * to the *direct* use of `sync_io::X`.  For what it is worth, internally `X` is usually written
 * in terms of `sync_io::X`; the latter is essentially the core logic, while the former provides
 * auto-parallelization and a simpler interface.
 *
 * @note For this reason comments sometimes refer to a `sync_io::X` *core*: where the basic `X`-ish capabalities and
 *       data live.  The *async-I/O* `X` is then often built around a `sync_io::X` core.  Because of this it is
 *       usually easy, and fast, to convert a `sync_io::X` into an `X` -- via a move-like ctor called
 *       `sync_io`-core *adopting ctor*.  Additionally, ipc::transport::Channel template -- which bundles local peer
 *       objects of 1-2 IPC pipes -- can bundle *either* async-I/O peer objects *or* `sync_io` peer objects, and
 *       one can always convert the latter to the former by calling `x.async_io_obj()`.
 *
 * @note In cases where performance is not a real concern, such as for the assumed-rare
 *       ipc::session::Session_server::async_accept() operations, internally `sync_io::X` may actually be written
 *       in terms of `X` instead... but we digress.  Either way it is a black box.
 *
 * Some examples of `X`es that have `sync_io::X` counterparts:
 *   - core-layer transport::Native_socket_stream
 *     (including its `async_receive_*()`, and the handler-less -- but nevertheless potentially
 *     asynchronous -- `send_*()`) and all other `Blob_sender`, `Blob_receiver`, `Native_handle_sender`,
 *     `Native_handle_receiver` concept impls including the bundling transport::Channel;
 *   - structured-layer ipc::transport::struc::Channel;
 *   - session::Client_session (with its channel-accept and error handlers) and session::Session_server (ditto, plus
 *     with its `async_accept()`);
 *     - all their session::shm counterparts.
 *
 * ### The async-I/O (default) pattern ###
 * Consider `X` -- take for example transport::Native_socket_stream -- and a particular async operation -- take,
 * e.g., transport::Native_socket_stream::async_receive_blob().
 *
 * When `x.async_receive_blob(..., F)` is invoked, `F()` is the user-specified completion handler, while ...
 * specifies details about the operation, in this case the target buffer where to write data.  It works as follows:
 * `x` attempts to perform the operation (in this case receive a single in-message as soon as it becomes available
 * which may or may not be instant); and once it has suceeded, it invokes `F(...)` (where ... indicates
 * results, usually at least an `Error_code`) from *an unspecified thread* that is not the user's calling thread
 * (call it thread U, where `x.async_*()` was called).  Even if the op completes immediately, `x.async_*()` will
 * never invoke `F()` synchronously; always from the *unspecified thread*.
 *
 * That's great, but what does really happen?  Answer: `x`, usually at construction, invisibly, starts a separate
 * thread (technically it could be co-using a thread with other objects; but in reality as of this writing each
 * object really starts a thread).  An async operation might complete synchronously (perhaps a message is available
 * in a kernel receive buffer and is therefore immediately, internally, received inside `x.async_receive_blob()`
 * body); or it might occur in the background and involve (internally) async waiting of native-handle readability --
 * possibly even more threads might start (internally) to get things to work.  *Either* way, there is that thread --
 * call it thread W -- where *at least* the completion handler `F()` will be called.
 *
 * (If `x` is destroyed before this has a chance to happen, the `x`
 * destructor shall -- last-thing -- invoke `F()`, passing it the special
 * operation-aborted `Error_code`.  That is the case for one-off async-ops like that one.  There are also variations
 * such as the completion handlers of transport::struc::Channel, but the key point -- that work happens in
 * the background, in the object-created own thread W, and user-supplied handlers are run from thread W -- remains
 * the same.  Another variation is async-ops that don't require a completion handler; for example
 * transport::Native_socket_stream::send_blob() may perform work in the background upon encountering would-block
 * conditions internally -- and this again occurs in thread W -- but there is no completion handler to invoke.)
 *
 * What is the user supposed to do with an async-op like this?  In practice we tend to think of this in terms
 * of 2 basic possiblities for how the user's own event loop might be organized.
 *   - Proactor pattern: This is what boost.asio uses, what flow.async facilitates further, and what we generally
 *     recommend all else being equal.  We won't describe it here in detail, but integrating such a loop with
 *     this async-I/O pattern in Flow-IPC is quite simple:
 *     -# Call `x.async_*(..., F)` as explained above.
 *     -# As `F()` supply a short wrapper that will place the true handling of the event onto the same event loop --
 *        thread U (though multiple such threads might be in use alternatively) -- where you invoked `x.async_*(F)`.
 *        For example:
 *
 *   ~~~
 *     ...
 *     m_my_asio_loop.post([t]() { start_async_op(); }
 *     ...
 *   void start_async_op()
 *   {
 *     // We are in thread U.
 *     x.async_receive_blob(..., [this](const Error_code& err_code, size_t sz)
 *     {
 *       // We are in "unspecified thread" W.
 *       m_my_asio_loop.post([this, err_code, sz]() { on_async_op_done(err_code, sz); }
 *     }
 *   }
 *   void on_async_op_done(const Error_code& err_code, size_t sz)
 *   {
 *     // We are in thread U.
 *     // ...handle it....
 *   }
 *   ~~~
 *
 * Alternatively:
 *   - Reactor pattern: Usually built on top of an OS-supplied blocking polling method of some kind -- in POSIX
 *     nowadays usually at least `poll()`, in Linux possibly using the more advanced `epoll_*()` -- which centers
 *     on an "FD-set" (native handle set), where one describes `Native_handle`s (FDs) and the events one awaits
 *     (readable, writable) for each; and in each event loop iteration one runs a poll-wait operation
 *     (like `poll()` or `epoll_wait()`).  This blocks until 1 or more events-of-interest are active; then wakes up
 *     and reports which ones they were.  User code then synchronously invokes handling for each event of interest;
 *     such handling might modify the events-of-interest set, etc., until all such work is done, and the
 *     next poll-wait op executes.  Thus in the reactor pattern methods like `on_async_op_done()` are invoked
 *     in a flow-control setup inverted versus the proactor pattern; but ultimately they're both doing the same thing.
 *     To integrate such a loop with this async-I/O pattern in Flow-IPC, a little extra work is required:
 *     - Sometimes event-loop libraries provide this as a built-in feature: a *task queue*.  So a facility similar to
 *       `post()` above is supplied.  Hence the code above would be adapted to a reactor-pattern loop and end up
 *       fairly similar: In `F()` do some equivalent to the `post()` in the snippet above.  Internally it'll set up
 *       some "interrupter" handle in the central `[e]poll*()` handle-set and cause -- from thread W -- for thread U's
 *       poll-wait to wake up.  Otherwise:
 *     - This task-queue facility can be written with relatively little difficulty.  Essentially it involves
 *       a simple IPC mechanism, perhaps an anonymous pipe, through which thread-W-invoked `F()`s can
 *       inform the thread-U poll-wait that it must wake up and handle events, among any others that the poll-wait
 *       covers.
 *
 * So that's the async-I/O (default) pattern in Flow-IPC.  Generally it is easy to work with -- especially
 * in a proactor-pattern event loop, but otherwise also not hard.  It cleanly separates Flow-IPC's internal needs
 * from the rest of the application's: Flow-IPC needs to do background work?  It takes care of its own needs:
 * it starts and ends threads without your participation.  Moreover this may
 * well help performance of the user's own event loop: Flow-IPC's
 * cycles are mostly spent in separate threads, reducing the length of your loop's single iteration and thus
 * helping reduce your latency.  The processors' context-switching is automatic and usually efficient; and it
 * automatically makes use of multiple hardware cores.
 *
 * ### Rationale for the `sync_io` pattern ###
 * So why might the default pattern described above be insufficient?  A detailed study of this is outside our scope
 * here; but basically it is a matter of control.  The way it starts threads, in a way that cannot be specified
 * by you (the user), and switches between them may be helpful in 95% of cases; but some applications want complete
 * control of any such thing.  For instance suppose I'd like to start with doing all that background work
 * of `Native_socket_stream::async_receive_blob()` directly in thread U.  It should be possible, right?  Whatever
 * events it waits on -- in reality, internally, principally it waits for readability of a Unix domain socket --
 * I could just wait-on in my own thread-U `epoll_wait()`.  When an active event is detected, I could do the
 * resulting non-blocking-reads -- that normally would be done in the background thread W -- directly after
 * the poll-wait.
 *
 * Maybe that would be good, reducing context-switching overhead.  Or maybe it wouldn't be good, as a big fat
 * loop iteration could cause latency in serving the next batch of work.  If so, and I *did* want
 * to do some of the work in some other thread for parallelization, maybe I want to share that other thread with
 * some other processing.  Or... or....  Point is: perhaps I want to explicitly structure what threads do what, whether
 * or not I want multi-threaded processing.
 *
 * If that is the case, then the `sync_io` pattern will serve that need.  In this pattern, for example
 * in transport::sync_io::Native_socket_stream, you'll notice completion handlers are still used as part of the API.
 * However, they are *never* invoked in the background: *you* call into a `sync_io::X` API, and it might
 * *synchronously only* and at very specific points invoke a completion handler `F()` that you supplied it earlier.
 *
 * We'll get into details below, but to summarize how this is integrated with the 2 above-covered user event loop
 * patterns:
 *   - Proactor pattern: The `sync_io` pattern in Flow-IPC has first-class support for a boost.asio event loop
 *     on the user's part.  So -- basically -- if you've got a `boost::asio::io_context E` (`flow::util::Task_engine E`)
 *     `run()`ning over 1+ threads U, then `sync_io::X` shall give you boost.asio `descriptor` objects associated
 *     with `E` and ask *you* -- yourself, albeit on its behalf -- to perform `.async_wait(write, G)` or
 *     `.async_wait(read, G)` on those `descriptor`s; and inform the `sync_io::X` when they've completed, inside `G()`.
 *     As a result, `sync_io::X` internals might inform you of the completion of an `async_...()` op earlier requested
 *     by you.
 *   - Reactor pattern: If your loop isn't boost.asio-based, then `sync_io::X` will similarly ask you to perform
 *     async-waits on this or that handle for read or write or both, just over a slightly different API -- one
 *     conducive to `poll()`, `epoll_ctl()`/`epoll_wait()`, and the like.  In short it'll tell you what FD and what
 *     event it wants you to wait-on (and again give you function `G()` to call when it is indeed active).
 *
 * Thus, *you* control what happens in what thread -- and everything can happen in *your* single thread, if you
 * so desire.  *You* can create the other threads, arrange necessary synchronization -- including of access to
 * the `sync_io::X` in question -- as opposed to rely on whatever we've internally designed inside non-`sync_io` `X`.
 * *You* control when a `sync_io::X` does something.  In particular, it is only in a couple of
 * specific `sync_io::X::*` APIs that a completion handler you gave it can actually be called.  *If* it is called,
 * it is always called synchronously right then and there, not from some unknown background thread.
 *
 * ### So `sync_io`-pattern-implementing APIs will never start threads? ###
 * Well, they might.  In some cases it's an internal need that cannot be avoided.  However, when *both* (1) it can be
 * avoided, *and* (2) performance could possibly be affected, then correct: Flow-IPC will avoid starting a thread and
 * performing context-switching.  If it's immaterial for performance in practice, then it absolutely reserves the
 * right to make background threads, whether for ease of internal implementation or some other reason.  And,
 * of course, if there's some blocking API that must be used internally -- and there is simply no choice but to
 * use that API -- then a thread will need to be started behind the scenes.  We can't very well block your thread U, so
 * at that point we do what we must.
 *
 * However, even in *that* case, a `sync_io` *API* is still supplied.  This may be helpful to more easily integrate
 * with your reactor-pattern event loop.  (However, if you have a proactor like boost.asio as your event loop, then
 * in our view it is unlikely to be helpful in that sense.  At that point you might as well use the async-I/O
 * alternative API -- unless, again, there is some performance benefit to maintaining greater control of what
 * part of Flow-IPC executes when from what thread.)
 *
 * ### Using the `sync_io` pattern: design rationale ###
 * Though it will look different and perhaps complex, it is actually at its core similar to other third-party
 * APIs that require the user to perform async-waits on their behalf.  The most well known example of such an API
 * is perhaps OpenSSL.  Take `SSL_read()` -- quite similar in spirit to `sync_io`-pattern `x.async_read_blob()`.
 * When invoked, barring connection-hosing errors, one of 2 things will happen:
 *   - It will succeed and read 1 or more bytes and return this fact ("no error, I have read N bytes").
 *   - It will fail due to would-block, because it either needs the underlying stream socket to be
 *     readable, or to be writable.  (The latter -- needing writability -- may seem strange for `SSL_read()`,
 *     but it's normal: perhaps the connection is in the middle of a cert negotiation, and at this stage needs
 *     to write something *out* first; and the connection happens to be in such a state as to not be able to write
 *     bytes to the kernel send buffer at that moment.)
 *     So it will return either:
 *     - `SSL_ERROR_WANT_READ` (meaning, "the underlying stream-socket handle (FD) needs to be readable -- call me
 *       again after you've *async-waited* successfully on this *event-of-interest*, and I will try again"); or
 *     - `SSL_ERROR_WANT_WRITE` (same but needs writability isntead).
 *
 * Your application would then internally register interest in FD so-and-so to be readable or writable.  Perhaps
 * some `SSL_write()` would be interested in another such event simultaneously too.  So then the next time the
 * event loop came up to the next `poll()` or `epoll_wait()`, you'd indeed wait on these registered events.
 * If the `SSL_read()`-related event-of-interest was indeed returned as active, your program would know that
 * fact, based on its own data structures, and know to try `SSL_read()` again.  That time `SSL_read()` might
 * succeed; or it might require writability again, or readability this time, and would return
 * `SSL_ERROR_WANT_*` again.  Eventually it'd get what it needs and return success.
 *
 * `sync_io` pattern in Flow-IPC is not much different.  It is arguably more complex to use, but there are good
 * reasons for it.  Namely there are some differences as to our requirements compared to OpenSSL's.  To wit:
 *   - For a given operation -- whether it's `Native_socket_stream::async_receive_blob()` or the even more
 *     internally complex transport::struc::Channel::expect_msg() -- there will usually be more than 1
 *     event of interest a time, in fact spread out over more than 1 handle (FD) at a time at that.  Namely
 *     in addition to readability/writability of the underlying low-level trqnsport, there are timer(s)
 *     (such as the idle timer -- `idle_timer_run()`); and `struc::Channel` can be configured to have
 *     2 in-pipes and thus will be async-waiting on 2 FDs' read events.
 *     - Hence we need to be able to express the desired async-waits to the user in a more flexible way than
 *       a simple "can't do it, tell me when my socket is Xable and call me again."  We have to communicate
 *       an entire set-of-events-of-interest as well as changes in it over time.
 *   - OpenSSL is targeted at old-school event loops -- ~all written in the reactor-pattern style.
 *     Flow-IPC is meant to be usable by modern proactor-pattern applications *and* old-school reactor-pattern
 *     ones as well.  It would be unfortunate, in particular, if a given `X` is written in the modern handler-based way,
 *     while `X::sync_io` counterpart is just entirely different.  In fact, suppose `sync_io::X` is available;
 *     *our* `X` internally "wants" to be written around a boost.asio loop *and* reuse `sync_io::X` internally
 *     as well.  So in other words, both aesthetically and practically, an OpenSSL-style old-school API would be
 *     a pain to use in a boost.asio-based application (our own async-I/O-pattern classes being a good test case).
 *
 * Therefore the `sync_io` pattern is strongly inspired by the boost.asio proactor pattern API.  As a corollary it
 * provides special support for the case when the user's event loop *is* boost.asio-based.  If your event loop
 * is old-school, you will lose nothing however -- just don't use that feature.
 *
 * ### Using the `sync_io` pattern: how-to ###
 * Take `x.async_receive_blob(..., F)`.  If `x` is an `X`, we've already described it.  Now let `x` be a `sync_io::X`.
 * The good news is initiating the same operation uses *almost* the exact same signature.  It takes the same arguments
 * including a completion handler `F()` with the exact same signature itself.  There is however one difference:
 * if `F()` takes 1+ args (1st one usually `const Error_code& err_code`; followed at times by something like
 * `size_t sz` indicating how many bytes were transferred) -- then the `sync_io` form of the `async_...()` method
 * shall take `sync_`-prefixed *out-arg* counterparts to those args directly as well.  Thus specifically:
 *   - If async-I/O sig is `x.async_receive_blob(..., F)`,
 *     where `F()` is of form `void (const Error_code& err_code, size_t sz)`, then:
 *   - `sync_io` sig is: `x.async_receive_blob(..., Error_code* sync_err_code, size_t* sync_sz, F)`,
 *     where `F()` is of the same form as before.  You shall provide `&sync_err_code, &sync_sz`, and your local
 *     variables shall be set to certain values upon return.
 *     - You may also leave `sync_err_code` (if applicable) null.  In that case standard Flow error-reporting
 *       semantics are active: if (and only if) `*sync_err_code` *would* be set to truthy (non-success) value,
 *       but `sync_err_code` is a null pointer, then a `flow::error::Runtime_error` is thrown with
 *       the would be `*sync_err_code` stored inside the exception object (and a message in its `.what()`).
 *
 * So let's say you call `sync_io` `x.async_receive_blob()`, providing it `&sync_err_code, &sz` args, otherwise
 * using the same arg values as with async-I/O.  (For simplicity of discussion let's assume you did not pass
 * null pointer for the sync-err-code arg.)  Then: `F()` will no longer
 * execute from some unspecified thread at some unknown future time.  Instead there are 2 possibilities.
 *   - If `x.async_receive_blob()` was able to receive a message (more generally -- complete the operation)
 *     synchronously (immediately), then:
 *     - The method *itself* shall emit (via `sync_err_code` out-arg) either *no* error (falsy value)
 *       or an error (truthy value) that is specifically *not* ipc::transport::error::Code::S_SYNC_IO_WOULD_BLOCK.
 *       - This is quite analogous to `SSL_read()` succeeding immediately, without `SSL_ERROR_WANT_*`.
 *     - The method *itself* shall synchronously emit other `sync_` out-arg values indicating the result
 *       of the operation.  In this case that is `sync_sz == 0` on error, or `sync_sz > 0` indicating how many
 *       bytes long the successfully, *synchronously* received blob is.
 *       - Again: This is quite analogous to `SSL_read()` succeeding immediately, without `SSL_ERROR_WANT_*`.
 *     - The async completion handler, `F`, shall be utterly and completely ignored.  It will *not* be saved.
 *       It will *not* be called.  The operation already finished: you get to deal with it right then and there
 *       like a normal synchronous human being.
 *   - Conversely, if `x.async_receive_blob()` can only complete the op after some events-of-interest (to `x`)
 *     become active (we'll discuss how this works in a moment) -- then:
 *     - The method *itself* shall emit (via `sync_err_code` out-arg) the specific code
 *       ipc::transport::error::Code::S_SYNC_IO_WOULD_BLOCK.
 *       - This is analogous to `SSL_read()` returning `SSL_ERROR_WANT_*`.
 *     - It shall save `F` internally inside `x`.
 *     - Later, when your loop *does* detect an event-of-interest (to `x`) is active, it shall explicitly
 *       *call into another `x` API method* -- we call it `(*on_active_ev_func)()` -- and *that* method may
 *       *synchronously* invoke the memorized completion handler `F()`.  In the case of `x.async_receive_blob()`
 *       this `F()` call will be just as you're used-to with async-I/O pattern:
 *       `err_code` truthy on error, `sz == 0`; or `err_code` falsy on success, `sz > 0` indicating size of
 *       received blob.
 *       - Since `x.async_receive_blob()` has one-off completion handler semantics, at this point it'll forget `F`.
 *       - However, with `sync_io` pattern, `x` shall *never* issue an operation-aborted call to `F()` or
 *         `x` destruction or anything like that.  That's an async-I/O thing.
 *
 * The "later, when your loop" step is analogous to: your loop awaiting an event as asked by the would-block
 * `SSL_read()`, then once active calling `SSL_read()` again; and the latter, this time, returning success.
 * Note, however, that you do *not* re-call `x.async_receive_blob()` on the desired active event.  Conceptually
 * you're doing the same thing -- you're saying, "the event you wanted is ready; if you can get what I wanted
 * then do it now" -- but you're doing it by using a separate API.
 *
 * That leaves the other other major piece of the API: how to in fact be informed of a desired event-of-interest
 * and subsequently indicate that event-of-interest is indeed active.  In terms of the API, this procedure is
 * decoupled from the actual `x.async_receive_blob()` API.  Moreover it is not expressed as some kind of big
 * set-of-handles-and-events-in-which-`x`-has-interest-at-a-given-time either.  Instead, conceptually, it is
 * expressed similarly to boost.asio: `x` says to itself: I want to do
 * `handle_so_and_so.async_wait(readable, F)`; or: I want to do
 * `handle_so_and_so.async_wait(writable, F)`.  But since `handle_so_and_so` is not a boost.asio I/O object, it has
 * to express the same thing to the user of `x`.  How?  Answer: sync_io::Event_wait_func.  Its doc header explains
 * the details.  We summarize for our chosen example of `x.async_receive_blob()`:
 *   - Just after constructing the `x`, set up the async-wait function: `x.start_receive_blob_ops(E)`,
 *     where `E` is a function with signature a-la `Event_wait_func`.
 *     - (Note that `receive_blob` fragment of the method name.  This is replaced with other fragments for other
 *       operations.  In some cases an object -- ipc::transport::Native_socket_stream being a prime example -- supports
 *       2+ sets of operations.  For example `Native_socket_stream::start_send_blob_ops()` is for the *independent*
 *       outgoing-direction operations that can occur *concurrently to* incoming-direction `start_receive_blob_ops()`
 *       ops.)
 *       No need to worry about that for now though; we continue with our `receive_blob` example use case.
 *   - *If* (and only if!) your event loop is boost.asio-based, then precede this call with one that supplies
 *     the boost.asio `Task_engine` (or strand or...) where you do your async work: `.replace_event_wait_handles()`.
 *
 *   ~~~
 *   using ipc::util::sync_io::Task_ptr;
 *   using ipc::util::sync_io::Asio_waitable_native_handle;
 *
 *   x.replace_event_wait_handles
 *     ([this]() -> auto { return Asio_waitable_native_handle(m_my_task_engine); });
 *   x.start_receive_blob_ops([this](Asio_waitable_native_handle* hndl_of_interest,
 *                                   bool ev_of_interest_snd_else_rcv,
 *                                   Task_ptr&& on_active_ev_func)
 *   {
 *     my_rcv_blob_sync_io_ev_wait(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
 *   });
 *   ~~~
 *
 * We are now ready for the last sub-piece: named `my_rcv_blob_sync_io_ev_wait()` in that snippet.  This key piece
 * responds to the instruction: async-wait for the specified event for me please, and let me know when that event
 * is active by calling `(*on_active_ev_func)()` at that time.  The details of how exactly to do this can be
 * found in the sync_io::Event_wait_func doc header -- including tips on integrating with `poll()` and `epoll_*()`.
 * Here we will assume you have a boost.asio loop, continuing from the above snippet.  The simplest possible
 * continuation:
 *
 *   ~~~
 *   void my_rcv_blob_sync_io_ev_wait(Asio_waitable_native_handle* hndl_of_interest,
 *                                    bool ev_of_interest_snd_else_rcv,
 *                                    Task_ptr&& on_active_ev_func)
 *   {
 *     // sync_io wants us to async-wait.  Oblige.
 *     hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv
 *                                    ? Asio_waitable_native_handle::Base::wait_write
 *                                    : Asio_waitable_native_handle::Base::wait_read,
 *                                  [this, on_active_ev_func = std::move(on_active_ev_func)]
 *                                    (const Error_code& err_code)
 *     {
 *       if (err_code == boost::asio::error::operation_aborted) { return; } // Shutting down?  Get out.
 *
 *       // Event is active.  sync_io wants us to inform it of this.  Oblige.
 *       (*on_active_ev_func)();
 *       // ^-- THAT POTENTIALLY INVOKED COMPLETION HANDLER F() WE PASS IN TO x.async_receive_blob(F)!!!
 *     });
 *   }
 *   ~~~
 *
 * That's it.  Note well: `(*on_active_ev_func)()` is (a) informing `sync_io`-pattern-implementing `x` of the
 * event it (`x`) wanted; and (b) possibly getting the result *you* wanted in the original `async_receive_*()` call.
 * Thus it is the equivalent of the 2nd `SSL_read()` call, after satisying the `SSL_ERROR_WANT_READ` or
 * `SSL_ERROR_WANT_WRITE` result.  The only difference, really, is the mechanic of getting the result is through
 * a *synchronous* call to your completion handler.
 *
 * There is, however, an extremely important subtlety to consider additionally.  The key point:
 *   - `(*on_active_ev_func)()` should be thought of as an API -- a method! -- of `x`, the
 *     `sync_io`-pattern-implementing Flow-IPC object...
 *   - ...right alongside (in our use case here) `.async_receive_blob()`.
 *
 * That means it is *not* thread-safe to call `(*on_active_ev_func)()` concurrently with
 * `x.async_receive_blob()`.  What does this mean in practice?  Answer:
 *   - If you execute all these things in a single-threaded event loop -- nothing.  You need not worry about
 *     synchronization.
 *   - If you perform some of your work on `x` (perhaps the `.async_*()` calls) in one thread but
 *     the async-wait handling -- and the potentially resulting *synchronous* invocation of your own completion
 *     handlers -- in another...
 *     - ...then you, at a minimum, need a mutex-lock (or use of a strand, or ...) around the call sites into
 *       `x.method(...)`, for all related `method`s of `x`...
 *     - ...*including* the call to `(*on_active_ev_func)()`.
 *
 * Chances are, though, that if you're operating in multiple threads, then anyway you'll need to protect *your own*
 * data structures against concurrent writing.  Remember: `(*on_active_ev_func)()` may well invoke
 * *your own completion handler* from the original `x.async_receive_blob(..., F)` call.  That code (*your* code)
 * is more than likely to react to the received payload in some way, and that might be touching data structures
 * accessed from 2+ threads.  It would therefore make sense that the relevant mutex be already locked.  In this
 * example we presuppose your application might invoke async-receives from a thread U while placing handlers
 * onto a thread W:
 *
 *   ~~~
 *   using flow::util::Lock_guard;
 *
 *   ...
 *     // In your thread U:
 *
 *     Lock_guard<decltype(m_my_rcv_mutex)> lock(m_my_rcv_mutex); // <-- ATTN!  Protects x receive ops at least.
 *     ...
 *     ipc::Error_code sync_err_code;
 *     size_t sync_sz;
 *     x.async_receive_blob(m_target_msg,
 *                          &sync_err_code, &sync_sz,
 *                          [this](const ipc::Error_code& err_code, size_t sz) { on_msg_in(err_code, sz); });
 *     if (sync_err_code != ipc::transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
 *     {
 *       // ...Handle contents of m_target_msg, or sync_err_code indicating error ion -- perhaps ~like on_msg_in()!
 *       // Note m_my_rcv_mutex is locked... as it would be in on_msg_in() (which won't run in this case)!
 *     }
 *     // else { on_msg_in() will run later.  x.receive_...() probably invoked my_rcv_blob_sync_io_ev_wait(). }
 *   ...
 *
 *   void my_rcv_blob_sync_io_ev_wait(Asio_waitable_native_handle* hndl_of_interest,
 *                                    bool ev_of_interest_snd_else_rcv,
 *                                    Task_ptr&& on_active_ev_func)
 *   {
 *     // In your thread U:
 *
 *     // sync_io wants us to async-wait.  Oblige.  (Do not lock m_my_rcv_mutex!)
 *     hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv
 *                                    ? Asio_waitable_native_handle::Base::wait_write
 *                                    : Asio_waitable_native_handle::Base::wait_read,
 *                                  [this, on_active_ev_func = std::move(on_active_ev_func)]
 *                                    (const Error_code& err_code)
 *     {
 *       if (err_code == boost::asio::error::operation_aborted) { return; } // Shutting down?  Get out.
 *
 *       // In your thread W:
 *
 *       // Event is active.  sync_io wants to inform it of this.  Oblige.
 *
 *       flow::util::Lock_guard<decltype(m_my_rcv_mutex)> lock(m_my_rcv_mutex); // <-- ATTN!
 *
 *       (*on_active_ev_func)();
 *       // ^-- THAT POTENTIALLY INVOKED on_msg_in()!!!
 *     });
 *   }
 *
 *   void on_msg_in(const Error_code& err_code, size_t sz)
 *   {
 *     // In your thread W:
 *     // m_my_rcv_mutex is locked!
 *
 *     ... // Handle contents of m_target_msg, or err_code indicating error.
 *   }
 *   ~~~
 *
 * Just remember: *You* choose when `x` does anything that might touch your data, or itself.  This happens
 * in exactly three possible places and always synchronously:
 *   - The `x.async_receive_blob()` initiating call itself (and ones like it, all having async-I/O almost-identical-sig
 *     counterparts in `X::` for any given `sync_io::X::`).
 *     - OpenSSL analogy: initial `SSL_read()`.
 *   - Your code reacting to non-would-block return from `x.async_receive_blob()`, right after it (and ones like it).
 *     - OpenSSL analogy: reacting to initial `SSL_read()` succeeding right away.
 *   - `(*on_active_ev_func)()`.
 *     - OpenSSL analogy: subsequent `SSL_read()`, once you've detected its `SSL_ERROR_WANT_*` has been satisfied.
 *
 * ### Is there always a completion handler? ###
 * Answer: No.  An async op might not have a completion handler, but it is still an async op and may need to
 * ask you to async-wait for some handle to be readable and/or writable to work properly.  The most prominent
 * case of this is sending items as exemplified by ipc::transport::sync_io::Native_socket_stream::send_blob().
 * How to handle it?  Well, it's the same as any `async_*()` op (which shall always take a completion handler) --
 * but simpler.  Simply put: Such an op shall be initiated by a method that takes no completion handler arg, so
 * you simply don't provide one.  (By convention it will also lack an `async_` prefix in the method name.)
 * Beyond that, everything is the same:
 *   - It may (or may not) initiate an async-wait by calling the `Event_wait_func` you supplied via
 *     `start_*ops()`.
 *   - Your `Event_wait_func` should indeed -- on successful async-wait -- invoke `(*on_active_ev_func)()`.
 *     Internally that may continue, or complete, whatever async processing `x` needs to do.
 *     - The difference: it won't invoke some completion handler of yours... as you didn't (and couldn't) provide one.
 *
 * ### Multiple op-types in a given `sync_io`-pattern-implementing object ###
 * Consider, first, ipc::session::sync_io::Session_server_adapter.  It has only one operation, really:
 * session::sync_io::Session_server_adapter::async_accept().  To set it up, you'll do (as explained above by another
 * example use case): `x.start_ops()`, possibly preceded by `x.replace_event_wait_handles()` (but that guy is
 * beside the point).
 *
 * Easy enough.  The `x` can't do any other "type of thing."  Same is true of, say,
 * ipc::transport::sync_io::Blob_stream_mq_sender: it can only `x.send_blob()` (and `x.auto_ping()`,
 * `x.end_sending()`, `x.async_end_sending()` -- all of which deal with sending messages *out*); and needs only
 * `x.start_send_blob_ops()` for setup.
 *
 * What if an object can do multiple things though?  `Native_socket_stream` (operating as a `Blob_sender` and
 * `Blob_receiver`) can do at least 2: it can
 *   - send (`.send_blob()`, `.end_sending()`, `.async_end_sending()`, `.auto_ping()`),
 *   - and receive (`.async_receive_blob()`, `.idle_timer_run()`).
 *
 * These are 2 distinct *op-types*, and each one has its own independent API started via
 * `.start_send_blob_ops()` and `.start_receive_blob_ops()` respectively.  (If it were networked -- this might
 * be in the works -- it would probably gain a 3rd op-type via `.start_connect_ops()`.)
 *
 * Not that interesting in and of itself; but what about concurrency?  Answer:
 *
 * Things only get interesting once 2+ op-types (in practice, as of this writing, it's
 * 2 at most in all known use cases) can occur concurrently.  Namely that's
 * sending (started via `.start_send_blob_ops()` in this case) and receiving (`.start_receive_blob_ops()`).
 * How does it work?  Answer: Formally speaking, it's described in the appropriate class's doc header; in this case
 * ipc::transport::sync_io::Native_socket_stream.  Informally the intuition behind it is as follows:
 *   - In a full-duplex object (like this one), you can indeed send and receive at the same time.
 *     That is, e.g., you can interleave an async-receive-blob op and its eventual completion with
 *     invoking something send-related and its eventual completion (if applicable).
 *   - Moreover you can use the API of op-type "receive" *actually concurrently* with the API of op-type "send."
 *     It *is* thread-safe, even though you're ostensily mutating the same object `x`.
 *     This is analogous to being able to concurrently `read(fd, ...)` and `write(fd, ...)` on the same OS handle
 *     `fd` for many socket types including TCP and Unix-domain-socket.  This may, all in all, lead to significant
 *     perf savings in your application, if you have multiple threads available to handle sending and receiving.
 *     If you *do* need a mutex, you can have 1 mutex for send ops and 1 mutex for receive ops (assuming of course
 *     your algorithm doesn't involve the 2 directions' algorithms concurrently mutating the same outside-`x` data).
 *
 * Do keep in mind: This applies to `sync_io::X` only -- not its async-I/O counterpart `X`.  E.g., you cannot do
 * `x.async_receive_blob()` and `x.send_blob()` concurrently, in the latter case, but you can too do so in the former. 
 * With plain `X` you'd need to mutex-protect (or use strands or ...) `x`.  However you can of course still have 2+
 * async operations outstanding simultaneously (e.g., initiating a send while an `.async_receive_*(..., F)` has not yet
 * finished as signified by `F()`): you just cannot literally call mutating parts of `x` API concurrently.
 *
 * @see ipc::util::sync_io::Event_wait_func -- details about hooking up your event loop to a
 *      `sync_io`-pattern-implementing Flow-IPC object.
 * @see ipc::util::sync_io::Asio_waitable_native_handle -- take a look particularly if your event loop is built on
 *      boost.asio.
 * @see ipc::transport::Native_socket_stream internal source code: This exemplifies a fairly advanced
 *      "eat-our-own-dog-food" usage of a `sync_io`-pattern-implementing API
 *      (ipc::transport::sync_io::Native_socket_stream in this case) in a multi-threaded setting
 *      (user's thread U and internal worker thread W in this case).  In this case the event loop is a boost.asio
 *      one.
 *
 * @todo Write an example of `sync_io`-pattern use with an old-school reactor-pattern event loop, using
 *       `poll()` and/or `epoll_*()`.
 *
 * @internal
 *
 * @see ipc::util::sync_io::Timer_event_emitter -- how to hook up a boost.asio `Timer` inside
 *      a `sync_io`-pattern-implementing Flow-IPC object.
 */
namespace ipc::util::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.

class Asio_waitable_native_handle;

/**
 * Short-hand for ref-counted pointer to a `Function<>` that takes no arguments and returns nothing;
 * in particular used for `on_active_ev_func` arg of sync_io::Event_wait_func.
 *
 * ### Rationale ###
 * This is defined here because of its central role in sync_io::Event_wait_func (see that doc header).
 *
 * Why wrap it in a smart pointer at all as opposed to passing around `Function<>`s as objects (particularly
 * as arg to #Event_wait_func)?  Answer: Performance.  Our `sync_io` pattern is intended for the highly
 * perf-conscious user, to the extent they'd forego the significantly easier to use async-I/O pattern
 * just because that would involve involuntary (from the user's point of view) thread creation and context
 * switching; copying or moving polymorphic functors, including all their captures, is an unnecessary expense.
 *
 * In that case why `shared_ptr`, not `unique_ptr`, given that it adds potential ref-counting behind the scenes?
 * Answer: `unique_ptr` would have been nice; however it is likely the user (and/or internal Flow-IPC code)
 * will want to lambda-capture the wrapped `Task`, and capturing movable-but-not-copyable types like `unique_ptr`
 * does not compile (as of C++17), even if one never copies the capturing lambda.  One would need to upgrade
 * to `shared_ptr` to capture, and that is annoying.
 *
 * @note That said it is recommended that one `std::move()` any #Task_ptr whenever possible, such as when
 *       capturing it in a lambda (e.g., `[task_ptr = std::move(task_ptr)`).  This advice applies generally
 *       to all `shared_ptr` captures (*"whenever possible"* being important), but this is just a reminder.
 */
using Task_ptr = boost::shared_ptr<Task>;

/**
 * In `sync_io` pattern, concrete type storing user-supplied function invoked by pattern-implementing
 * ipc::transport and ipc::session object to indicate interest in an I/O status event (writable, readable)
 * for a particular Native_handle.
 *
 * @see ipc::util::sync_io doc header first.  It explains the pattern in detail including example code for
 *      setting up an `Event_wait_func`.
 *
 * Use in `sync_io` pattern
 * ------------------------
 * Suppose `T` is an ipc::transport or ipc::session object type, always in a `"sync_io"` sub-namespace, that
 * operates according to the `sync_io` pattern.  (For example, `T` might be transport::sync_io::Native_socket_stream.)
 * Then `T::start_*ops(Event_wait_func&&)`, and/or a compatible template, must be invoked to begin unidirectional work
 * of type X (e.g., `start_*ops()` might be `start_send_blob_ops()` or
 * `start_receive_native_handle_ops()`) on a given `T`; the `T` memorizes the function until its destruction.
 *
 * From that point on, the `T` might at times be unable to complete an operation (for example
 * transport::sync_io::Native_socket_stream::send_blob()) synchronously due to a would-block condition on some
 * internal Native_handle (for example, internally, the Unix domain stream socket in this case encountering
 * would-block on a `"::writemsg()"` attempt).  It shall then -- synchronously, inside
 * `Native_socket_stream::send_blob()` -- invoke the saved #Event_wait_func and pass to it the following arguments:
 *   - `hndl_of_interest`: The native handle on which the user's own event loop must wait for an I/O event.
 *      In-depth tips on how to do so are below; but for now:
 *      - If your event loop is built on boost.asio, you may use `hndl_of_interest->async_wait()` directly.
 *      - Otherwise (if you're using `[e]poll*()` perhaps), obtain the raw handle as follows:
 *        `hndl_of_interest->native_handle().m_native_handle`.  It can be input to `poll()`, `epoll_ctl()`, etc.
 *   - `ev_of_interest_snd_else_rcv`: Which event it must await: `true` means "writable"; `false` means "readable."
 *     - What if the user's wait (such as `epoll_wait()`) encounters an error-occurred event instead
 *       (`EPOLLERR`)?
 *       Answer: They must in fact report this as-if the requested event (whether writable or readable) is active.
 *   - `on_active_ev_func`: If (and, to avoid pointless perf loss, only if) the above-specified event is active,
 *     the user must invoke `(*on_active_ev_func)()` (without args and expecting no return value).
 *     - In terms of thread safety, and generally, one should consider this function a non-`const` member of `T`'s
 *       sub-API.  (The sub-API in question is the set of methods that correspond to unidirectional-operation
 *       of type X, where `T::start_*ops()` was invoked to kick things off.  For example, in `Native_socket_stream`
 *       as used as a Blob_sender, that's its `send_blob()` and `*end_sending()` methods.)
 *       That is, `(*on_active_ev_func)()` may not be called concurrently to any `T` sub-API method
 *       (`Native_socket_stream::send_blob()`, `*end_sending()` in the recent example) or other
 *       `(*on_active_ev_func)()`.  `(*on_active_ev_func)()` may well, itself, synchronously invoke
 *       `Event_wait_func` to indicate interest in a new event.
 *
 * Naturally the key question arises: what, specifically, should a particular #Event_wait_func (as passed-into
 * a `T::start_*ops()`) *do*?  Indeed, this requires great care on the `sync_io` pattern user's part.
 *
 * Formally speaking the contract is as follows.  Let `F` be the particular `Event_wait_func`.
 *   -# Upon `F()` being called, it shall register -- through a technique of the user's choice (a couple are
 *      explained below for your convenience) -- the specified event as one of interest.  The sooner this registration
 *      occurs, the more responsively `T` will behave.
 *   -# It shall arrange, via that same technique, to do the following upon detecting the
 *      event (or `hndl_of_interest` becoming hosed, i.e., the error event).  The sooner it does so, the more
 *      responsively `T` will behave.
 *      -# *Deregister* the specified event.  Each `Event_wait_func` invocation indicates a **one-off** wait.
 *      -# Call `(*on_active_ev_func)()`.  (It is best, for a little perf bump, for the user
 *         to save `std::move(on_active_ev_func))` as opposed to a smart-pointer copy.)
 *
 * @warning Do not forget to deregister the event before `(*on_active_ev_func)()`.  Failure to do so can easily
 *          result in processor pegging at best; or undefined behavior/assertion tripping.
 *          Worse still, if it's mere processor pegging, you might not notice it happening.
 *          E.g., if `Native_socket_stream::send*()` encounters would-block internally,
 *          it will register interest in writability of an internal handle; suppose when you report writability
 *          it is able to push-through any queued internal payload.  Now it no longer needs writability; if
 *          informed of writability anyway, it will at best do nothing -- leading to an infinite loop
 *          of user reporting writability and `Native_socket_stream` ignoring it.  Or, if that is how
 *          `Native_socket_stream` is written, it will detect that a write event is being reported despite it
 *          not asking for this and log WARNING or even FATAL/abort program.  With `epoll_*()`, `EPOLLONESHOT` and/or
 *          `EPOLLET` may be of aid, but be very careful.
 *
 * ### Integrating with reactor-pattern `poll()` and similar ###
 * Suppose your application is using POSIX `poll()`.  Typically a data structure will be maintained mirroring
 * the `fds[].events` (events of interest) sub-argument to `poll()`; for example an `unordered_map<>` from FD
 * (Native_handle::m_native_handle) to an `enum { S_NONE, S_RD, S_WR, S_RD_WR }`; or simply the `fds[]` array itself.
 * (In the latter case lookup by FD may not be constant-time.  However `fds[].events` does not need to be built
 * from a mirroring structure ahead of each `poll()`.)
 *
 * When `Event_wait_func F(hndl, snd_else_rcv, on_ev_func)` is invoked, you will register the event:
 *   -# If necessary, insert `raw_hndl = hndl->native_handle().m_native_handle` into the data structure,
 *      with no events of interest (which will change in the next step).
 *      If not necessary (already inserted, hence with a (different) event of interest already), continue to next step.
 *   -# Depending on `snd_else_rcv`, change the events-of-interest `enum` NONE->RD, NONE->WR, WR->RD_WR, or
 *      RD->RD_WR.  (If operating on an `fd[].events` directly, that's: `fd.events = fd.events | POLLIN` or
 *      `fd.events = fd.events | POLLOUT`.)
 *   -# In some data structure keyed on, conceptually, the pair `(raw_hndl, bool snd_else_rcv)`,
 *      record `std::move(on_ev_func)`.  Call it, say, `ipc_events_map`.
 *
 * At some point in your reactor-pattern event loop, you are ready to call `poll()`.  Construct `fds[].events` array
 * if needed, or just use the long-term-maintained one, depending on how you've set this up.  Call `poll()`.
 * For each *individual* active event in an `fds[].revents`:
 *   -# Construct the pair `(raw_hndl, snd_else_rcv)`.
 *   -# If it's not in `ipc_events_map`, no IPC-relevant event fired; it must be something relevant to other
 *      parts of your application (such as network traffic).  Exit algorithm for now (until next `poll()`);
 *      the IPC-registered event is still being waited-on.
 *   -# If it *is* in `ipc_events_map`:
 *      -# Remove it from there.
 *      -# Deregister the event w/r/t next `poll()`:
 *         -# Depending on `snd_else_rcv`: Change the events-of-interest `enum` RD->NONE, WR->NONE, RD_WR->WR, or
 *            RD_WR->RD.  (If operating on an `fd[].events` directly, that's `fd.events = fd.events & ~POLLIN)` or
 *            `fd.events = fd.events & ~POLLOUT`.)
 *         -# If the events-of-interest for the FD have become NONE (or `fd.events == 0` if tracking it directly),
 *            delete the handle's entry from the `events`-mirroring structure (or `events` itself if tracking it
 *            directly).
 *      -# Invoke the saved `(*on_ev_func)()`.
 *
 * @note While each `F()` invocation indicates one-off event interest, the handler `(*on_ev_func)()` will sometimes,
 *       or possibly frequently, re-indicate interest in the same event within its own handler.  If done very
 *       carefully, it *might* be possible to detect this situation by deferring the editing of `events` or
 *       the mirroring structure until `(*on_ev_func)()` finishes; possibly this would net to making no change to
 *       the events-of-interest structure.  This could save some processor cycles.  I (ygoldfel) would recommend
 *       only getting into that if compelling perf savings evidence appears.
 *
 * @note You might notice `hndl` is a pointer to Asio_waitable_native_handle, but you simply get `raw_hndl`
 *       out of it and forget about the rest of `*hndl`.  Why not just provide `raw_hndl` to you?  Answer:
 *       It is useful when *not* integrating with a reactor-pattern event loop a-la `poll()` or similar but
 *       rather when integrating with a boost.asio event loop.  See "Integrating with boost.asio" below.
 *
 * ### What about `epoll_*()`? ###
 * In Linux `epoll_*()` is considered superior (its `man` page at least says that, when used in non-`EPOLLET`
 * mode, it is a "faster" `poll()`).  We leave the exercise of how to apply the above suggestions (for `poll()`)
 * to `epoll_*()` to the reader.  Briefly: Essentially `epoll_ctl()` lets the kernel track a long-running `fds[].events`
 * sub-array, with add/remove/modify operations specified by the user as syscalls.  However, it's not possible to simply
 * say "I am interested in FD X, event writable"; the events-of-interest per FD are still specified as
 * an ORing of `EPOLLIN` and/or `EPOLLOUT` in one per-FD entry, whereas `Event_wait_func` is finer-grained than that.
 * It is not possible to iterate through the kernel-stored events-of-interest set or obtain the existing
 * `events` bit-mask so as to then `|=` or `&=` it.  Therefore a mirroring data structure (such as the
 * aforementioned `unordered_map<>` from FD to rd/wr/rd-wr) may be necessary in practice.  In my (ygoldfel)
 * experience using such a mirroring thing is typical in any case.
 *
 * One tip: `EPOLLONESHOT` may be quite useful: It means you can limit your code to just doing
 * `epoll_ctl(...ADD)` without requiring the counterpart `epoll_ctl(...DEL)` once the event does fire.
 * (`EPOLLET` may be usable to decrease the number of epoll-ctl calls needed further, but as of this writing
 * we haven't investigated this sufficiently to make a statement.)
 *
 * However `EPOLLONESHOT` pertains to the entire descriptor, not one specific waited-on event (read or write).
 * Therefore this optimization is helpful only if `.events` is being set to `EPOLLIN` or `EPOLLOUT` -- not
 * `EPOLLIN | EPOLLOUT`.  Be careful.  That said, it should be possible to use `dup()` to clone the descriptor
 * in question; then use the original for (e.g.) `EPOLLIN` exclusively and the clone for `EPOLLOUT` exclusively
 * resulting in a clean solution.
 *
 * ### Integrating with boost.asio ###
 * Suppose your application is using boost.asio (possibly with flow.async to manage threads),
 * with a `flow::util::Task_engine::run()` comprising the event loop, in proactor pattern fashion.
 * (We would recommend just that, preferring it to an old-school reactor-pattern via `[e]poll*()` directly.)
 * *Your* loop is asynchronously expressed, but you can still use the `sync_io` pattern to graft
 * Flow-IPC operations into it, so that they are invoked synchronously, when you want, in the exact thread you
 * want.  In fact, doing so is *significantly* simpler than integrating with a reactor-style `[e]poll*()`.
 * That is because, conceptually, each `Event_wait_func` invocation is essentially expressing the following
 * boost.asio operation:
 *
 *   ~~~
 *   some_low_level_transport.async_wait(wait_write, // (or `wait_read`.)
 *                                       [...](const Error_code& err_code)
 *   {
 *     // Can do I/O, or error?  Do I/O, or handle error.
 *     // ...If no error then possibly even at some point continue the async-op chain...
 *     same_or_other_low_level_transport.async_wait(wait_write, // (or `wait_read`.)
 *                                                  ...);
 *   }
 *   ~~~
 *
 * Because of that you have to code essentially none of the stuff above (pertaining to a reactor-style `[e]poll*()`
 * loop): no manual registering/deregistering, wondering how to handle error-event, etc. etc.
 *
 * So what to do, specifically?  It is quite straightforward.  Suppose you're got `Task_engine E` doing
 * `E.run()` as your event loop.  (Any `flow::async::*_loop` does that too, just internally once you do
 * `loop.start()`.)  E.g., if you've got TCP sockets attached to `E`, you might be doing
 * `m_tcp_sock.async_read_some(..., [](const Error_code& err_code) { ... };` and so on.
 *
 * When `Event_wait_func F(hndl, snd_else_rcv, on_ev_func)` is invoked, you will simply do:
 *
 *   ~~~
 *   hndl->async_wait(snd_else_rcv ? Asio_waitable_native_handle::Base::wait_write
 *                                 : Asio_waitable_native_handle::Base::wait_read,
 *                    [on_ev_func = std::move(on_ev_func)](const Error_code& err_code)
 *   {
 *     if (err_code != boost::asio::error::operation_aborted)
 *     {
 *       (*on_ev_func)(); // Note: err_code is disregarded.  Whether it's readable/writable or hosed, must invoke.
 *     }
 *     // else { Stuff is shutting down; do absolutely nothing!  SOP on operation_aborted generally. }
 *   }
 *   ~~~
 *
 * Basically, Flow-IPC wants to do an `async_wait()`... so you do it for Flow-IPC, being the master of your
 * (boost.asio) domain (so to speak).
 *
 * `hndl` is an `Asio_waitable_native_handle*`.  Asio_waitable_native_handle is a razor-thin wrapper around
 * a boost.asio `posix::descriptor` which itself is a thin wrapper around a native handle (FD).  It has
 * boost.asio-supplied `.async_wait()`.  However, and this is a key point:
 *
 * To make it work, before invoking `T::start_*ops()`, you must supply your execution context/executor -- usually
 * a boost.asio `Task_engine` (a/k/a `boost::asio::io_contexst`) or strand (`boost::asio::io_context::strand`) --
 * w/r/t which you plan to `.async_wait()` down the line.  This is done via `T::replace_event_wait_handles()`,
 * an otherwise optional call.  If using flow.async, this might be (e.g.):
 *
 *   ~~~
 *   flow::async::Single_thread_task_loop m_your_single_threaded_event_loop;
 *   ipc::transport::Native_socket_stream m_sock_stream;
 *   // ...
 *   m_sock_stream.replace_event_wait_handles([this]() -> auto
 *   {
 *     return ipc::util::async_io::Asio_waitable_native_handle
 *              (*(m_your_single_threaded_event_loop.task_engine()));
 *   });
 *   m_sock_stream.start_send_blob_ops(F); // F is your Event_wait_func.
 *   ~~~
 *
 * @note As explained in "Integrating with reactor-pattern..." above, `hndl` is
 *       a boost.asio I/O object, as opposed to just a Native_handle or even Native_handle::handle_t,
 *       specifically for the boost.asio integration use case.  If *not* integrating with boost.asio,
 *       `start_*ops()` is to be used without preceding it by `replace_event_wait_handles()`,
 *       and `hndl->native_handle()` is the only meaningful part of `*hndl`, with `.async_wait()` being meaningless
 *       and unused.  Conversely, if integrating with boost.asio, `hndl->native_handle()` itself should not be
 *       required in your code, while `.async_wait()` is the only meaningful aspect of `*hndl`.
 */
using Event_wait_func = Function<void (Asio_waitable_native_handle* hndl_of_interest,
                                       bool ev_of_interest_snd_else_rcv,
                                       Task_ptr&& on_active_ev_func)>;

} // namespace ipc::util::sync_io
