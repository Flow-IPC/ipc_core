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
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/error.hpp"
#include <flow/error/error.hpp>
#include <flow/util/util.hpp>
#include <boost/chrono/floor.hpp>
#include <boost/array.hpp>

namespace ipc::transport
{

namespace
{

/**
 * File-local helper: takes Shared_name; returns name suitable for `mq_open()` and buddies.
 * @param name
 *        Shared_name.
 * @return See above.
 */
std::string shared_name_to_mq_name(const Shared_name& name);

} // namespace (anon)

// Initializers.

const Shared_name Posix_mq_handle::S_RESOURCE_TYPE_ID = Shared_name::ct("posixQ");

// Implementations.

Posix_mq_handle::Posix_mq_handle() :
  m_interrupting_snd(false), // (Not algorithmically needed as of this writing, but avoid some sanitizer complaints.)
  m_interrupting_rcv(false), // (Ditto.  E.g., swap() through clang's UBSAN => complains it's uninitialized.)
  m_interrupter_snd(m_nb_task_engine),
  m_interrupt_detector_snd(m_nb_task_engine),
  m_interrupter_rcv(m_nb_task_engine),
  m_interrupt_detector_rcv(m_nb_task_engine)
{
  // Done.
}

template<typename Mode_tag>
Posix_mq_handle::Posix_mq_handle(Mode_tag, flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                                 size_t max_n_msg, size_t max_msg_sz,
                                 const util::Permissions& perms, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_absolute_name(absolute_name_arg),
  m_interrupting_snd(false),
  m_interrupting_rcv(false),
  m_interrupter_snd(m_nb_task_engine),
  m_interrupt_detector_snd(m_nb_task_engine),
  m_interrupter_rcv(m_nb_task_engine),
  m_interrupt_detector_rcv(m_nb_task_engine)
{
  using util::set_resource_permissions;
  using flow::error::Runtime_error;
  using flow::log::Sev;
  using boost::io::ios_all_saver;
  using boost::system::system_category;
  using ::mq_open;
  using ::mq_close;
  using ::mq_unlink;
  using ::mq_attr;
  using ::close;
  // using ::O_RDWR; // A macro apparently.
  // using ::O_CREAT; // A macro apparently.
  // using ::O_EXCL; // A macro apparently.

  assert(max_n_msg >= 1);
  assert(max_msg_sz >= 1);

  static_assert(std::is_same_v<Mode_tag, util::Create_only> || std::is_same_v<Mode_tag, util::Open_or_create>,
                "Can only delegate to this ctor with Mode_tag = Create_only or Open_or_create.");
  constexpr bool CREATE_ONLY_ELSE_MAYBE = std::is_same_v<Mode_tag, util::Create_only>;
  constexpr char const * MODE_STR = CREATE_ONLY_ELSE_MAYBE ? "create-only" : "open-or-create";

  if (logger_ptr && logger_ptr->should_log(Sev::S_TRACE, get_log_component()))
  {
    ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.

    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("Posix_mq_handle [" << *this << "]: Constructing MQ handle to MQ at name [" << absolute_name() << "] in "
       "[" << MODE_STR << "] mode; max msg size [" << max_msg_sz << "] x [" << max_n_msg << "] msgs; "
       "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].");
  }

  const auto mq_name = shared_name_to_mq_name(absolute_name());
  bool created = false; // If we end up failing overall, and this is true, we shall unlink the MQ we created.
  /* Whether we end up failing overall = sys_err_code falsiness.  The fail-points are all determined
   * by sys_err_code, and some logging + possible throwing at the end will use its value too. */
  Error_code sys_err_code;

  ([&]() // Error?  Just return; we will cleanup before ctor exits.
  {
    /* Naively: Use mq_open() to both create-or-open/exclusively-create and to set mode `perms`.
     * In reality though there are two subtleties that make it tougher than that.  Fortunately this has been
     * adjudicated out there, and internal boost.interprocess code very nicely confirms that adjudication/sets
     * a sufficiently-authoritative precedent.
     *
     * Subtlety 1 is that mq_open(), like all POSIX/Linux ::open()-y calls, doesn't listen to `perms` verbatim
     * but rather makes it subject to the process umask.  The proper way (citation omitted) to deal with it is
     * to follow it up with ::[f]chmod() or equivalent, with the resource descriptor if available (or path if not).
     * (We do have the descriptor -- m_mq -- and we have a util::set_resource_permissions() overload for this.)
     *
     * Subtlety 2 is that we should only do that last thing if, indeed, we are *creating* the resource (MQ);
     * if CREATE_ONLY_ELSE_MAYBE==false, and it already exists, then we mustn't do any such thing; we've succeeded.
     * Sadly there's no (atomic/direct) way to find out whether mq_open() (in ==false case) did in fact create
     * the thing.  Therefore, we have to do things in a kludgy way -- that is, nevertheless, reasonable in practice
     * and indeed is validated by bipc's use of it internally (search headers for `case ipcdetail::DoOpenOrCreate:`
     * followed by shm_open(), fchmod(), etc.).  We explain it here for peace of mind.
     *
     * In the CREATE_ONLY_ELSE_MAYBE=true case, we can just do what we'd do anyway -- shm_open() in create-only
     * mode; then set_resource_permissions(); and that's that.  Otherwise:
     *
     * We can no longer use the built-in atomic create-or-open mode (O_CREAT sans O_EXCL).  Instead, we logically
     * split it into two parts: try to create-only (O_CREAT|O_EXCL); if it succeeds, done (can
     * set_resource_permissions()); if it fails with file-exists error, then mq_open() again, this time in open-only
     * mode (and no need for set_resource_permissions()).  Great!  The problem, of course, is that this no longer uses a
     * guaranteed-atomic open-or-create OS-call semantic.  Namely it means set_resource_permissions() could fail due to
     * file-not-found, because during the non-atomic gap the MQ got removed by someone.  Then we spin-lock (in a way)
     * through it: just try the whole procedure again.  Sooner or later -- assuming no other errors of course --
     * either the first mq_open() will succeed, or it'll "fail" with file-exists, yet the 2nd mq_open() will succeed.
     * Pegging processor is not a serious concern in this context. */

    // Get pipe stuff out of the way, as it does not need m_mq.
    if ((sys_err_code = pipe_setup()))
    {
      return;
    }
    // else

    // Setup m_mq.  (Open handle, which may or may not create name in file system; in latter case set perms as well.)

    const auto do_mq_open_func = [&](bool create_else_open)
    {
      /* Note: O_NONBLOCK is something we are forced to set/unset depending on the transmission API being called.
       * So just don't worry about it here. */
      ::mqd_t raw;
      if (create_else_open)
      {
        mq_attr attr;
        attr.mq_maxmsg = max_n_msg;
        attr.mq_msgsize = max_msg_sz;

        raw = mq_open(mq_name.c_str(),
                      O_RDWR | O_CREAT | O_EXCL, // Create-only always: per above.  Can't use create-or-open.
                      perms.get_permissions(),
                      &attr);
      }
      else
      {
        raw = mq_open(mq_name.c_str(), O_RDWR);
      }
      if (raw != -1)
      {
        sys_err_code.clear();
        m_mq = Native_handle{raw};
        create_else_open && (created = true);
      }
      else
      {
        sys_err_code = {errno, system_category()};
      }
    }; // do_mq_open_func =

    if (CREATE_ONLY_ELSE_MAYBE)
    {
      // Simple case.

      if ((do_mq_open_func(true), sys_err_code)
          || (set_resource_permissions(get_logger(), m_mq, perms, &sys_err_code),
              sys_err_code))
      {
        return;
      }
    }
    else // if (!CREATE_ONLY_ELSE_MAYBE)
    {
      // Complicated case.

      bool success = false; // While this remains false, sys_err_code indicates whether it's false b/c of fatal error.
      do
      {
        if (do_mq_open_func(true), !sys_err_code)
        {
          // Created!  Only situation where we must indeed set_resource_permissions().
          if (set_resource_permissions(get_logger(), m_mq, perms, &sys_err_code),
              sys_err_code)
          {
            return; // Real error.  GTFO.
          }
          // else

          success = true;
          continue; // `break`, really.
        }
        // else if (do_mq_open_func() failed):
        if (sys_err_code != boost::system::errc::file_exists)
        {
          return; // Real error.  GTFO.
        }
        // else if (file_exists): Create failed, because it already exists.  Open the existing guy!

        if (do_mq_open_func(false), !sys_err_code)
        {
          success = true; // Opened!
          continue; // `break`, really.
        }
        // else if (do_mq_open_func() failed):
        if (sys_err_code != boost::system::errc::no_such_file_or_directory)
        {
          return; // Real error.  GTFO.
        }
        // else if (no_such_file_or_directory): Open failed, because MQ *just* got unlinked.  Try again!

        // This is fun and rare enough to warrant an INFO message.
        if (logger_ptr && logger_ptr->should_log(Sev::S_INFO, get_log_component()))
        {
          ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.
          FLOW_LOG_INFO_WITHOUT_CHECKING
            ("Posix_mq_handle [" << *this << "]: Create-or-open algorithm encountered the rare concurrency: "
             "MQ at name [" << absolute_name() << "] existed during the create-only mq_open() but disappeared "
             "before we were able to complete open-only mq_open().  Retrying in spin-lock fashion.  "
             "Details: max msg size [" << max_msg_sz << "] x [" << max_n_msg << "] msgs; "
             "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].");
        }

        // success remains false, and that wasn't a fatal error; so we did not return, and loop continues.
      }
      while (!success);

      // Now fall through.
    } // else if (!CREATE_ONLY_ELSE_MAYBE)

    // We never use blocking transmission -- always using *wait_*able() so that interrupt_*() works.  Non-blocking 4eva.
    if (!set_non_blocking(true, &sys_err_code)) // (On error it'd print Error_code redundantly.  It's fine.)
    {
      assert(sys_err_code);
      return;
    }
    // else

    sys_err_code = epoll_setup(); // Lastly setup the watcher mechanism.
  })(); // The returning-on-error gauntlet.

  if (sys_err_code)
  {
    Error_code sink;

    m_interrupt_detector_rcv.close(sink);
    m_interrupter_rcv.close(sink);
    m_interrupt_detector_snd.close(sink);
    m_interrupter_snd.close(sink);
    /* (As of this writing could use m_epoll_hndl_snd.close() to replace prev 3 lines; also below and in several
     * places in this class, where ::close() is used.  Since the whole class is based on the epoll() API, though,
     * it seemed more honest and even, perversely, maintainable to use the proper native API for
     * epoll() FD-closing). */
    close(m_epoll_hndl_rcv.m_native_handle); m_epoll_hndl_rcv = {};
    close(m_epoll_hndl_snd.m_native_handle); m_epoll_hndl_snd = {};
    mq_close(m_mq.m_native_handle); m_mq = {};

    if (created)
    {
      /* Roll back the creation: else a healthy-looking -- but, e.g., possibly wrongly-permissioned -- MQ would
       * remain at `absolute_name` despite our reporting failure.  Best-effort: disregard any error. */
      mq_unlink(mq_name.c_str());
    }

    if (logger_ptr && logger_ptr->should_log(Sev::S_WARNING, get_log_component()))
    {
      ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.

      FLOW_LOG_WARNING_WITHOUT_CHECKING
        ("Posix_mq_handle [" << *this << "]: Setup error (additional details may be above) while "
         "constructing MQ handle to MQ at name [" << absolute_name() << "] in "
         "[" << MODE_STR << "] mode; max msg size [" << max_msg_sz << "] x [" << max_n_msg << "] msgs; "
         "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].");
    }
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    if (err_code)
    {
      *err_code = sys_err_code;
    }
    else
    {
      throw Runtime_error{sys_err_code, FLOW_UTIL_WHERE_AM_I_STR()};
    }
  } // if (sys_err_code)
  else if (err_code)
  {
    *err_code = sys_err_code; // Cool!
  }
  // else { Cool!  Just do not throw. }
} // Posix_mq_handle::Posix_mq_handle(Mode_tag)

Posix_mq_handle::Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                                 util::Create_only, size_t max_n_msg, size_t max_msg_sz,
                                 const util::Permissions& perms, Error_code* err_code) :
  Posix_mq_handle(util::CREATE_ONLY, logger_ptr, absolute_name_arg, max_n_msg, max_msg_sz, perms, err_code)
{
  // Cool.
}

Posix_mq_handle::Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                                 util::Open_or_create, size_t max_n_msg, size_t max_msg_sz,
                                 const util::Permissions& perms, Error_code* err_code) :
  Posix_mq_handle(util::OPEN_OR_CREATE, logger_ptr, absolute_name_arg, max_n_msg, max_msg_sz, perms, err_code)
{
  // Cool.
}

Posix_mq_handle::Posix_mq_handle(flow::log::Logger* logger_ptr, const Shared_name& absolute_name_arg,
                                 util::Open_only, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_absolute_name(absolute_name_arg),
  m_interrupting_snd(false),
  m_interrupting_rcv(false),
  m_interrupter_snd(m_nb_task_engine),
  m_interrupt_detector_snd(m_nb_task_engine),
  m_interrupter_rcv(m_nb_task_engine),
  m_interrupt_detector_rcv(m_nb_task_engine)
{
  using flow::log::Sev;
  using flow::error::Runtime_error;
  using boost::system::system_category;
  using ::mq_open;
  using ::mq_close;
  using ::close;
  // using ::O_RDWR; // A macro apparently.

  FLOW_LOG_TRACE
    ("Posix_mq_handle [" << *this << "]: Constructing MQ handle to MQ at name [" << absolute_name() << "] in "
     "open-only mode.");

  // Similar setup to the Mode_tag ctor; just somewhat simpler (no `created` possibility; less stuff to log).
  Error_code sys_err_code;

  ([&]()
  {
    // Get pipe stuff out of the way, as it does not need m_mq.
    if ((sys_err_code = pipe_setup()))
    {
      return;
    }
    // else

    /* Note: O_NONBLOCK is something we are forced to set/unset depending on the transmission API being called.
     * So just don't worry about it here. */
    const auto raw = mq_open(shared_name_to_mq_name(absolute_name()).c_str(), O_RDWR);
    if (raw == -1)
    {
      sys_err_code = {errno, system_category()};
      return;
    }
    // else

    m_mq = Native_handle{raw};

    // We never use blocking transmission -- always using *wait_*able() => interrupt_*() works.  Non-blocking 4eva.
    if (!set_non_blocking(true, &sys_err_code)) // (On error it'd print Error_code redundantly.  It's fine.)
    {
      assert(sys_err_code);
      return;
    }
    // else

    sys_err_code = epoll_setup(); // Lastly setup the watcher mechanism.
  })();

  if (sys_err_code)
  {
    Error_code sink;

    m_interrupt_detector_rcv.close(sink); // @todo Code reuse with other cleanup sequences.
    m_interrupter_rcv.close(sink);
    m_interrupt_detector_snd.close(sink);
    m_interrupter_snd.close(sink);
    close(m_epoll_hndl_rcv.m_native_handle); m_epoll_hndl_rcv = {};
    close(m_epoll_hndl_snd.m_native_handle); m_epoll_hndl_snd = {};
    mq_close(m_mq.m_native_handle); m_mq = {};

    FLOW_LOG_WARNING
      ("Posix_mq_handle [" << *this << "]: Setup error (additional details may be above) while "
       "constructing MQ handle to MQ at name [" << absolute_name() << "] in open-only mode.");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    if (err_code)
    {
      *err_code = sys_err_code;
    }
    else
    {
      throw Runtime_error{sys_err_code, FLOW_UTIL_WHERE_AM_I_STR()};
    }
  }
  else if (err_code)
  {
    *err_code = sys_err_code; // Cool!
  }
  // else { Cool!  Just do not throw. }
} // Posix_mq_handle::Posix_mq_handle(Open_only)

Error_code Posix_mq_handle::pipe_setup()
{
  using boost::asio::connect_pipe;

  // Reminder: Caller will cleanup on error, so we can just return.

  Error_code sys_err_code;
  connect_pipe(m_interrupt_detector_snd, m_interrupter_snd, sys_err_code);
  if (!sys_err_code)
  {
    connect_pipe(m_interrupt_detector_rcv, m_interrupter_rcv, sys_err_code);
  }

  if (sys_err_code)
  {
    FLOW_LOG_WARNING
      ("Posix_mq_handle [" << *this << "]: Constructing MQ handle to MQ at name [" << absolute_name() << "]: "
       "connect-pipe failed.  Details follow."); // Caller logs about sys_err_code.
  }

  return sys_err_code;
} // Posix_mq_handle::pipe_setup()

Error_code Posix_mq_handle::epoll_setup()
{
  using boost::system::system_category;
  using ::epoll_create1;
  using ::epoll_ctl;
  using Epoll_event = ::epoll_event;
  // using ::EPOLL_CTL_ADD; // A macro apparently.
  // using ::EPOLLIN; // A macro apparently.

  /* m_mq is good to go.
   *
   * Set up one epoll handle for checking for writability; another for readability.
   * Into each one add an interrupter FD (readability of read end of an anonymous pipe).
   *
   * Reminder: Caller will cleanup on error, so we can just return. */
  Error_code sys_err_code;

  const auto setup = [&](Native_handle* epoll_hndl_ptr, bool snd_else_rcv)
  {
    auto& epoll_hndl = *epoll_hndl_ptr;
    auto& interrupt_detector = snd_else_rcv ? m_interrupt_detector_snd : m_interrupt_detector_rcv;

    epoll_hndl = Native_handle{epoll_create1(0)};
    if (epoll_hndl.m_native_handle == -1)
    {
      epoll_hndl = {}; // No-op as of this writing, but just to keep it maintainable do it anyway.

      sys_err_code = {errno, system_category()};
      FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: Created MQ handle fine, but epoll_create1() failed; "
                       "details follow."); // Caller logs about sys_err_code.
      return;
    }
    // else if (epoll_hndl.m_native_handle != -1)

    // Each epoll_wait() will listen for transmissibility of the queue itself + readability of interrupt-pipe.
    Epoll_event event_of_interest1;
    event_of_interest1.events = snd_else_rcv ? EPOLLOUT : EPOLLIN;
    event_of_interest1.data.fd = m_mq.m_native_handle;
    Epoll_event event_of_interest2;
    event_of_interest2.events = EPOLLIN;
    event_of_interest2.data.fd = interrupt_detector.native_handle();
    if ((epoll_ctl(epoll_hndl.m_native_handle, EPOLL_CTL_ADD, event_of_interest1.data.fd, &event_of_interest1) == -1)
        ||
        (epoll_ctl(epoll_hndl.m_native_handle, EPOLL_CTL_ADD, event_of_interest2.data.fd, &event_of_interest2) == -1))
    {
      sys_err_code = {errno, system_category()};
      FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: Created MQ handle fine, but an epoll_ctl() failed; "
                       "snd_else_rcv = [" << snd_else_rcv << "]; details follow.");
      return;
    }
  }; // const auto setup =

  if (setup(&m_epoll_hndl_snd, true), !sys_err_code) // Do setup1.  If it succeeds...
  {
    setup(&m_epoll_hndl_rcv, false); // ...do setup2.
  }

  return sys_err_code;
} // Posix_mq_handle::epoll_setup()

Posix_mq_handle::Posix_mq_handle(Posix_mq_handle&& src) :
  Posix_mq_handle()
{
  operator=(std::move(src));
}

Posix_mq_handle::~Posix_mq_handle()
{
  using ::mq_close;
  using ::close;

  if (m_mq.null())
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: No MQ handle to close in destructor.");
    assert(m_epoll_hndl_snd.null());
    assert(m_epoll_hndl_rcv.null());
  }
  else
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Closing MQ handle (and epoll set).");
    // Disregard any error.  In Linux, by the way, only EBADF is possible apparently; we should be fine.
    mq_close(m_mq.m_native_handle);

    assert(!m_epoll_hndl_snd.null());
    assert(!m_epoll_hndl_rcv.null());
    close(m_epoll_hndl_snd.m_native_handle); // Ditto regarding errors.
    close(m_epoll_hndl_rcv.m_native_handle);
  }
} // Posix_mq_handle::~Posix_mq_handle()

Posix_mq_handle& Posix_mq_handle::operator=(Posix_mq_handle&& src)
{
  using std::swap;
  using ::mq_close;
  using ::close;

  if (&src != this)
  {
    if (!m_mq.null())
    {
      mq_close(m_mq.m_native_handle);
    }
    m_mq = {};

    m_absolute_name.clear();

    if (!m_epoll_hndl_snd.null())
    {
      close(m_epoll_hndl_snd.m_native_handle);
    }
    m_epoll_hndl_snd = {};
    if (!m_epoll_hndl_rcv.null())
    {
      close(m_epoll_hndl_rcv.m_native_handle);
    }
    m_epoll_hndl_rcv = {};

    m_interrupter_snd = Pipe_writer{m_nb_task_engine};
    m_interrupt_detector_snd = Pipe_reader{m_nb_task_engine};
    m_interrupter_rcv = Pipe_writer{m_nb_task_engine};
    m_interrupt_detector_rcv = Pipe_reader{m_nb_task_engine};

    swap(*this, src);
  }
  return *this;
}

void swap(Posix_mq_handle& val1, Posix_mq_handle& val2)
{
  using util::Pipe_writer;
  using util::Pipe_reader;
  using flow::log::Log_context;
  using std::swap;
  using boost::array;

  // This is a bit faster than un-specialized std::swap() which would require a move ction + 3 move assignments.

  swap(static_cast<Log_context&>(val1), static_cast<Log_context&>(val2));
  swap(val1.m_mq, val2.m_mq);
  swap(val1.m_absolute_name, val2.m_absolute_name);
  swap(val1.m_epoll_hndl_snd, val2.m_epoll_hndl_snd);
  swap(val1.m_epoll_hndl_rcv, val2.m_epoll_hndl_rcv);
  swap(val1.m_interrupting_snd, val2.m_interrupting_snd);
  swap(val1.m_interrupting_rcv, val2.m_interrupting_rcv);

  /* This is annoying!  Maybe we should just wrap all these in unique_ptr<>s to avoid this nonsense.
   * As it is, it might be unsafe to swap asio objects apart from their Task_engines, so doing this blech. */
  try
  {
    array<Native_handle, 4> fds1;
    array<Native_handle, 4> fds2;
    const auto unload = [&](Posix_mq_handle& val, auto& fds)
    {
      if (val.m_interrupter_snd.is_open())
      {
        fds[0].m_native_handle = val.m_interrupter_snd.release();
      }
      if (val.m_interrupter_rcv.is_open())
      {
        fds[1].m_native_handle = val.m_interrupter_rcv.release();
      }
      if (val.m_interrupt_detector_snd.is_open())
      {
        fds[2].m_native_handle = val.m_interrupt_detector_snd.release();
      }
      if (val.m_interrupt_detector_rcv.is_open())
      {
        fds[3].m_native_handle = val.m_interrupt_detector_rcv.release();
      }
    };
    unload(val1, fds1);
    unload(val2, fds2);

    const auto reload = [&](Posix_mq_handle& val, auto& fds)
    {
      // Leave their stupid task engines in-place.  Do need to reassociate them with the swapped FDs though.
      val.m_interrupter_snd
        = fds[0].null() ? Pipe_writer{val.m_nb_task_engine}
                        : Pipe_writer{val.m_nb_task_engine, fds[0].m_native_handle};
      val.m_interrupter_rcv
        = fds[1].null() ? Pipe_writer{val.m_nb_task_engine}
                        : Pipe_writer{val.m_nb_task_engine, fds[1].m_native_handle};
      val.m_interrupt_detector_snd
        = fds[2].null() ? Pipe_reader{val.m_nb_task_engine}
                        : Pipe_reader{val.m_nb_task_engine, fds[2].m_native_handle};
      val.m_interrupt_detector_rcv
        = fds[3].null() ? Pipe_reader{val.m_nb_task_engine}
                        : Pipe_reader{val.m_nb_task_engine, fds[3].m_native_handle};
    };
    reload(val1, fds2); // Swap 'em.
    reload(val2, fds1);
  }
  catch (...) // .release() and ctor can throw, and they really shouldn't at all.
  {
    assert(false && "Horrible exception."); std::abort();
  }
} // swap()

size_t Posix_mq_handle::max_msg_size() const
{
  using ::mq_attr;
  using ::mq_getattr;

  assert((!m_mq.null())
         && "As advertised: max_msg_size() => undefined behavior if not successfully cted or was moved-from.");

  mq_attr attr;
  if (mq_getattr(m_mq.m_native_handle, &attr) != 0)
  {
    /* We could handle error gracefully, as we do elsewhere, but EBADF is the only possibility, and there's zero
     * reason it should happen unless m_mq is null at this point.  It'd be fine to handle it, but then we'd have
     * to emit an Error_code, and the API would change, and the user would have more to worry about; I (ygoldfel)
     * decided it's overkill.  So if it does happen, log and assert(). @todo Maybe reconsider. */
    Error_code sink;
    handle_mq_api_result(-1, &sink, "Posix_mq_handle::max_msg_size(): mq_getattr()");
    assert(false && "mq_getattr() failed (details logged); this is too bizarre.");
    return 0;
  }
  // else

  return size_t(attr.mq_msgsize);
} // Posix_mq_handle::max_msg_size()

size_t Posix_mq_handle::max_n_msgs() const
{
  // Very similar to max_msg_size().  @todo Maybe code reuse.  Though line count might be even greater then so....

  using ::mq_attr;
  using ::mq_getattr;
  assert((!m_mq.null())
         && "As advertised: max_n_msgs() => undefined behavior if not successfully cted or was moved-from.");
  mq_attr attr;
  if (mq_getattr(m_mq.m_native_handle, &attr) != 0)
  {
    Error_code sink;
    handle_mq_api_result(-1, &sink, "Posix_mq_handle::max_n_msgs(): mq_getattr()");
    assert(false && "mq_getattr() failed (details logged); this is too bizarre.");
    return 0;
  }
  return size_t(attr.mq_maxmsg);
} // Posix_mq_handle::max_n_msgs()

bool Posix_mq_handle::set_non_blocking(bool nb, Error_code* err_code)
{
  using boost::system::system_category;
  using ::mq_attr;
  using ::mq_setattr;
  // using ::O_NONBLOCK; // A macro apparently.

  assert(err_code);

  /* By the way -- no, this cannot be done in mq_attr that goes into mq_open().  It is ignored there according to docs.
   * Would've been nice actually.... */

  mq_attr attr;
  attr.mq_flags = nb ? O_NONBLOCK : 0;
  return handle_mq_api_result(mq_setattr(m_mq.m_native_handle, &attr, nullptr),
                              err_code, "Posix_mq_handle::set_non_blocking(): mq_setattr()");
} // Posix_mq_handle::set_non_blocking()

bool Posix_mq_handle::try_send(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;
  using ::mq_send;
  // using ::EAGAIN; // A macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, try_send, blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert((!m_mq.null())
         && "As advertised: try_send() => undefined behavior if not successfully cted or was moved-from.");

  auto blob_data = blob.data();
  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                 "size [" << blob.size() << "].");
  if (blob.size() == 0)
  {
    /* ::mq_send(..., nullptr, N), even when N == 0, is (1) empirically speaking harmless but (2) technically
     * in violation of arg 2's non-null decl (even though `N == 0` is explicitly allowed per `man` page) hence
     * (3) causes a clang UBSAN sanitizer error.  So waste a couple cycles by feeding it this dummy
     * non-null value. */
    blob_data = static_cast<const void*>(&blob_data);
  }
  else // if (blob.size() != 0)
  {
    FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(blob, "  ") << "].");
  }

  if (mq_send(m_mq.m_native_handle,
              static_cast<const char*>(blob_data),
              blob.size(), 0) == 0)
  {
    err_code->clear();
    return true; // Instant success.
  }
  // else
  if (errno == EAGAIN)
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                  "size [" << blob.size() << "]: would-block.");
    err_code->clear();
    return false; // Queue full.
  }
  // else

  handle_mq_api_result(-1, err_code, "Posix_mq_handle::try_send(): mq_send(nb)");
  assert(*err_code);
  return false;
} // Posix_mq_handle::try_send()

void Posix_mq_handle::send(const util::Blob_const& blob, Error_code* err_code)
{
  using flow::util::buffers_dump_string;
  using ::mq_send;
  // using ::EAGAIN; // A macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { send(blob, actual_err_code); },
         err_code, "Posix_mq_handle::send()"))
  {
    return;
  }
  // else

  assert((!m_mq.null())
         && "As advertised: send() => undefined behavior if not successfully cted or was moved-from.");

  auto blob_data = blob.data();
  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-push of blob @[" << blob_data << "], "
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

  /* We could just invoke blocking mq_send(m_mq), but to get the promised logging we go a tiny bit fancier as follows.
   * Update: Now that we have to be interrupt_*()ible, also reuse wait_*() instead of using native
   * blocking mq_*(). */

  while (true)
  {
    if (mq_send(m_mq.m_native_handle,
                static_cast<const char*>(blob_data),
                blob.size(), 0) == 0)
    {
      err_code->clear();
      return; // Instant success.
    }
    // else
    if (errno != EAGAIN)
    {
      handle_mq_api_result(-1, err_code, "Posix_mq_handle::send(): mq_send(nb)");
      assert(*err_code);
      return;
    }
    // else if (would-block): TRACE-logs.

    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
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
} // Posix_mq_handle::send()

bool Posix_mq_handle::timed_send(const util::Blob_const& blob, util::Fine_duration timeout_from_now,
                                 Error_code* err_code)
{
  using flow::util::time_since_posix_epoch;
  using flow::util::buffers_dump_string;
  using flow::Fine_clock;
  using boost::chrono::floor;
  using boost::chrono::round;
  using boost::chrono::seconds;
  using boost::chrono::microseconds;
  using boost::chrono::nanoseconds;
  using ::mq_send;
  // using ::EAGAIN; // A macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, timed_send, blob, timeout_from_now, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // Similar to a combo of (blocking) send() and (non-blocking) try_send() -- keeping comments light where redundant.

  assert((!m_mq.null())
         && "As advertised: timed_send() => undefined behavior if not successfully cted or was moved-from.");

  auto blob_data = blob.data();
  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-timed-push of blob @[" << blob_data << "], "
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

  auto before = Fine_clock::now();

  while (true)
  {
    if (mq_send(m_mq.m_native_handle,
                static_cast<const char*>(blob_data),
                blob.size(), 0) == 0)
    {
      err_code->clear();
      break; // Instant success.
    }
    // else
    if (errno != EAGAIN)
    {
      handle_mq_api_result(-1, err_code, "Posix_mq_handle::timed_send(): mq_send(nb)");
      assert(*err_code);
      return false;
    }
    // else if (would-block): TRACE-logs.

    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-push of blob @[" << blob_data << "], "
                   "size [" << blob.size() << "]: would-block.  Executing blocking-wait.");

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

    const auto after = Fine_clock::now();
    timeout_from_now -= (after - before); // Reduce time left by the duration of the last attempt.
    before = after;
  } // while (true)

  return true;
} // Posix_mq_handle::timed_send()

bool Posix_mq_handle::try_receive(util::Blob_mutable* blob, Error_code* err_code)
{
  using util::Blob_mutable;
  using flow::util::buffers_dump_string;
  using ::mq_receive;
  // using ::EAGAIN; // A macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, try_receive, blob, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert((!m_mq.null())
         && "As advertised: try_receive() => undefined behavior if not successfully cted or was moved-from.");

  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                 "max-size [" << blob->size() << "].");

  ssize_t n_rcvd;
  unsigned int pri_ignored;
  if ((n_rcvd = mq_receive(m_mq.m_native_handle,
                           static_cast<char*>(blob->data()),
                           blob->size(), &pri_ignored)) >= 0)
  {
    *blob = Blob_mutable{blob->data(), size_t(n_rcvd)};
    FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
    if (blob->size() != 0)
    {
      FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
    }
    err_code->clear();
    return true; // Instant success.
  }
  // else
  if (errno == EAGAIN)
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                   "max-size [" << blob->size() << "]: would-block.");
    err_code->clear();
    return false; // Queue empty.
  }
  // else

  handle_mq_api_result(-1, err_code, "Posix_mq_handle::try_receive(): mq_receive(nb)");
  assert(*err_code);
  return false;
} // Posix_mq_handle::try_receive()

void Posix_mq_handle::receive(util::Blob_mutable* blob, Error_code* err_code)
{
  using util::Blob_mutable;
  using flow::util::buffers_dump_string;
  using ::mq_receive;
  // using ::EAGAIN; // A macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { receive(blob, actual_err_code); },
         err_code, "Posix_mq_handle::receive()"))
  {
    return;
  }
  // else

  assert((!m_mq.null())
         && "As advertised: receive() => undefined behavior if not successfully cted or was moved-from.");

  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-pop to blob @[" << blob->data() << "], "
                 "max-size [" << blob->size() << "].  Trying nb-pop first; if it succeeds -- great.  "
                 "Else will wait/retry/wait/retry/....");

  /* We could just invoke blocking mq_receive(m_mq), but to get the promised logging we go tiny bit fancier as follows.
   * Update: Now that we have to be interrupt_*()ible, also reuse wait_*() instead of using native
   * blocking mq_*(). */

  ssize_t n_rcvd;
  unsigned int pri_ignored;

  while (true)
  {
    if ((n_rcvd = mq_receive(m_mq.m_native_handle,
                             static_cast<char*>(blob->data()),
                             blob->size(), &pri_ignored)) >= 0)
    {
      *blob = Blob_mutable{blob->data(), size_t(n_rcvd)};
      FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
      if (blob->size() != 0)
      {
        FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
      }
      err_code->clear();
      return; // Instant success.
    }
    // else
    if (errno != EAGAIN)
    {
      handle_mq_api_result(-1, err_code, "Posix_mq_handle::receive(): mq_receive(nb)");
      assert(*err_code);
      return;
    }
    // else if (would-block): TRACE-logs.

    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                   "max-size [" << blob->size() << "]: would-block.  Executing blocking-wait.");

    wait_receivable(err_code);
    if (*err_code)
    {
      // Whether interrupted or true error, we're done.
      return;
    }
    // else

    FLOW_LOG_TRACE("Blocking-wait reported transmissibility.  Retrying.");
  } // while (true)
} // Posix_mq_handle::receive()

bool Posix_mq_handle::timed_receive(util::Blob_mutable* blob, util::Fine_duration timeout_from_now,
                                    Error_code* err_code)
{
  using util::Blob_mutable;
  using flow::Fine_clock;
  using flow::util::time_since_posix_epoch;
  using flow::util::buffers_dump_string;
  using boost::chrono::floor;
  using boost::chrono::round;
  using boost::chrono::seconds;
  using boost::chrono::microseconds;
  using boost::chrono::nanoseconds;
  using ::mq_receive;
  // using ::EAGAIN; // A macro apparently.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, timed_receive, blob, timeout_from_now, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // Similar to a combo of (blocking) receive() and (non-blocking) try_receive() -- keeping comments light if redundant.

  assert((!m_mq.null())
         && "As advertised: timed_receive() => undefined behavior if not successfully cted or was moved-from.");

  FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-timed-pop to blob @[" << blob->data() << "], "
                 "max-size [" << blob->size() << "]; timeout ~[" << round<microseconds>(timeout_from_now) << "].  "
                 "Trying nb-pop first; if it succeeds -- great.  Else will wait/retry/wait/retry/....");

  ssize_t n_rcvd;
  unsigned int pri_ignored;

  auto before = Fine_clock::now();

  while (true)
  {
    if ((n_rcvd = mq_receive(m_mq.m_native_handle,
                             static_cast<char*>(blob->data()),
                             blob->size(), &pri_ignored)) >= 0)
    {
      *blob = Blob_mutable{blob->data(), size_t(n_rcvd)};
      FLOW_LOG_TRACE("Received message sized [" << n_rcvd << "].");
      if (blob->size() != 0)
      {
        FLOW_LOG_DATA("Blob contents: [\n" << buffers_dump_string(*blob, "  ") << "].");
      }
      err_code->clear();
      break; // Instant success.
    }
    // else
    if (errno != EAGAIN)
    {
      handle_mq_api_result(-1, err_code, "Posix_mq_handle::timed_receive(): mq_receive(nb)");
      assert(*err_code);
      return false;
    }
    // else if (would-block): TRACE-logs.

    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Nb-pop to blob @[" << blob->data() << "], "
                   "max-size [" << blob->size() << "]: would-block.  Executing blocking-wait.");

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

    const auto after = Fine_clock::now();
    timeout_from_now -= (after - before); // Reduce time left by the duration of the last attempt.
    before = after;
  } // while (true)

  return true;
} // Posix_mq_handle::timed_receive()

template<bool SND_ELSE_RCV>
bool Posix_mq_handle::interrupt_impl()
{
  using util::Blob_const;

  assert((!m_mq.null())
         && "As advertised: interrupt_impl() => undefined behavior if not successfully cted or was moved-from.");

  Pipe_writer* interrupter_ptr;
  bool* interrupting_ptr;
  if constexpr(SND_ELSE_RCV)
  {
    interrupter_ptr = &m_interrupter_snd; interrupting_ptr = &m_interrupting_snd;
  }
  else
  {
    interrupter_ptr = &m_interrupter_rcv; interrupting_ptr = &m_interrupting_rcv;
  }
  auto& interrupter = *interrupter_ptr;
  auto& interrupting = *interrupting_ptr;

  if (interrupting)
  {
    FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: Interrupt mode already ON for "
                     "snd_else_rcv [" << SND_ELSE_RCV << "].  Ignoring.");
    return false;
  }
  // else

  interrupting = true; // Reminder: This member is for the above guard *only*.  wait_impl() relies on FD state.
  FLOW_LOG_INFO("Posix_mq_handle [" << *this << "]: Interrupt mode turning ON for "
                "snd_else_rcv [" << SND_ELSE_RCV << "].");

  util::pipe_produce(get_logger(), &interrupter);

  // Now any level-triggered poll-wait will detect that this mode is on.
  return true;
} // Posix_mq_handle::interrupt_impl()

template<bool SND_ELSE_RCV>
bool Posix_mq_handle::allow_impl()
{
  using util::Blob_mutable;

  assert((!m_mq.null())
         && "As advertised: allow_impl() => undefined behavior if not successfully cted or was moved-from.");

  // Inverse of interrupt_impl().  Keeping comments light.

  Pipe_reader* interrupt_detector_ptr;
  bool* interrupting_ptr;
  if constexpr(SND_ELSE_RCV)
  {
    interrupt_detector_ptr = &m_interrupt_detector_snd; interrupting_ptr = &m_interrupting_snd;
  }
  else
  {
    interrupt_detector_ptr = &m_interrupt_detector_rcv; interrupting_ptr = &m_interrupting_rcv;
  }
  auto& interrupt_detector = *interrupt_detector_ptr;
  auto& interrupting = *interrupting_ptr;

  if (!interrupting)
  {
    FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: Interrupt mode already OFF for "
                     "snd_else_rcv [" << SND_ELSE_RCV << "].  Ignoring.");
    return false;
  }
  // else

  interrupting = false;
  FLOW_LOG_INFO("Posix_mq_handle [" << *this << "]: Interrupt mode turning OFF for "
                "snd_else_rcv [" << SND_ELSE_RCV << "].");

  util::pipe_consume(get_logger(), &interrupt_detector);

  // Now any level-triggered poll-wait will detect that this mode is off.
  return true;
} // Posix_mq_handle::allow_impl()

bool Posix_mq_handle::interrupt_sends()
{
  return interrupt_impl<true>();
}

bool Posix_mq_handle::allow_sends()
{
  return allow_impl<true>();
}

bool Posix_mq_handle::interrupt_receives()
{
  return interrupt_impl<false>();
}

bool Posix_mq_handle::allow_receives()
{
  return allow_impl<false>();
}

template<bool SND_ELSE_RCV>
bool Posix_mq_handle::wait_impl(util::Fine_duration timeout_from_now_or_none, Error_code* err_code)
{
  using util::Fine_time_pt;
  using util::Fine_duration;
  using flow::util::time_since_posix_epoch;
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::system::system_category;
  using boost::array;
  using ::epoll_wait;
  using Epoll_event = ::epoll_event;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, wait_impl<SND_ELSE_RCV>, timeout_from_now_or_none, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  assert((!m_mq.null())
         && "As advertised: wait_impl() => undefined behavior if not successfully cted or was moved-from.");

  /* By the way -- epoll_wait() takes # of milliseconds, so round up to that and no need to do crazy stuff like
   * timed_send() et al; just convert to milliseconds. */
  int epoll_timeout_from_now_ms;
  milliseconds epoll_timeout_from_now;
  if (timeout_from_now_or_none == Fine_duration::max())
  {
    epoll_timeout_from_now_ms = -1;
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Infinite-await-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "].  Will perform an epoll_wait().");
  }
  else
  {
    epoll_timeout_from_now = round<milliseconds>(timeout_from_now_or_none);
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-await/poll-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]; timeout ~[" << epoll_timeout_from_now << "] -- "
                   "if 0 then poll.  Will perform an epoll_wait().");
    epoll_timeout_from_now_ms = int(epoll_timeout_from_now.count());
  }

  array<Epoll_event, 2> evs; // Only one possible event (we choose 1 of 2 event sets).
  Native_handle::handle_t epoll_hndl;
  if constexpr(SND_ELSE_RCV) { epoll_hndl = m_epoll_hndl_snd.m_native_handle; } else
                             { epoll_hndl = m_epoll_hndl_rcv.m_native_handle; }
  const auto epoll_result = epoll_wait(epoll_hndl, evs.begin(), sizeof(evs) / sizeof(Epoll_event),
                                       epoll_timeout_from_now_ms);
  if (epoll_result == -1)
  {
    const auto& sys_err_code = *err_code = {errno, system_category()};
    FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: epoll_wait() yielded error.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    return false;
  }
  // else

  assert(epoll_result <= 2);
  if ((epoll_result == 2) // Max of 2 events; if interrupted then as promised disregard it being also transmissible.
      ||
      ((epoll_result == 1) // 1 event: need to check whether it's the interruptor as opposed to the actual queue.
       && (evs[0].data.fd != m_mq.m_native_handle)))
  {
    if (timeout_from_now_or_none == Fine_duration::max())
    {
      FLOW_LOG_INFO("Posix_mq_handle [" << *this << "]: Infinite-await-unstarved for "
                    "snd_else_rcv [" << SND_ELSE_RCV << "]: interrupted.");
    }
    else
    {
      FLOW_LOG_INFO("Posix_mq_handle [" << *this << "]: Blocking-await/poll-unstarved for "
                    "snd_else_rcv [" << SND_ELSE_RCV << "]; timeout ~[" << epoll_timeout_from_now << "] -- "
                    "if 0 then poll: interrupted.");
    }
    *err_code = error::Code::S_INTERRUPTED;
    return false;
  }
  // else if (epoll_result <= 1): Not interrupted.

  const bool success = epoll_result == 1;
  if (timeout_from_now_or_none == Fine_duration::max())
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Infinite-await-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]: succeeded? = [" << success << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Posix_mq_handle [" << *this << "]: Blocking-await/poll-unstarved for "
                   "snd_else_rcv [" << SND_ELSE_RCV << "]; timeout ~[" << epoll_timeout_from_now << "] -- "
                   "if 0 then poll: succeeded? = [" << success << "].");
  }

  err_code->clear();
  return success;
} // Posix_mq_handle::wait_impl()

bool Posix_mq_handle::is_sendable(Error_code* err_code)
{
  return wait_impl<true>(util::Fine_duration::zero(), err_code);
}

void Posix_mq_handle::wait_sendable(Error_code* err_code)
{
  wait_impl<true>(util::Fine_duration::max(), err_code);
}

bool Posix_mq_handle::timed_wait_sendable(util::Fine_duration timeout_from_now, Error_code* err_code)
{
  return wait_impl<true>(timeout_from_now, err_code);
}

bool Posix_mq_handle::is_receivable(Error_code* err_code)
{
  return wait_impl<false>(util::Fine_duration::zero(), err_code);
}

void Posix_mq_handle::wait_receivable(Error_code* err_code)
{
  wait_impl<false>(util::Fine_duration::max(), err_code);
}

bool Posix_mq_handle::timed_wait_receivable(util::Fine_duration timeout_from_now, Error_code* err_code)
{
  return wait_impl<false>(timeout_from_now, err_code);
}

Native_handle Posix_mq_handle::native_handle() const
{
  return m_mq;
}

void Posix_mq_handle::remove_persistent(flow::log::Logger* logger_ptr, // Static.
                                        const Shared_name& absolute_name, Error_code* err_code)
{
  using boost::system::system_category;
  using ::mq_unlink;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { remove_persistent(logger_ptr, absolute_name, actual_err_code); },
         err_code, "Posix_mq_handle::remove_persistent()"))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);

  FLOW_LOG_INFO("Posix_mq @ Shared_name[" << absolute_name << "]: Removing persistent MQ if possible.");
  if (mq_unlink(shared_name_to_mq_name(absolute_name).c_str()) == 0)
  {
    err_code->clear();
    return;
  }
  // else

  const auto& sys_err_code = *err_code = {errno, system_category()};
  FLOW_LOG_WARNING("Posix_mq @ Shared_name[" << absolute_name << "]: While removing persistent MQ: "
                   "mq_unlink() yielded error.  Details follow.");
  FLOW_ERROR_SYS_ERROR_LOG_WARNING();
} // Posix_mq_handle::remove_persistent()

bool Posix_mq_handle::handle_mq_api_result(int result, Error_code* err_code, util::String_view context) const
{
  using boost::system::system_category;

  if (result == 0)
  {
    err_code->clear();
    return true;
  }
  // else

  const auto& sys_err_code = *err_code
    = (errno == EMSGSIZE)
        ? Error_code{error::Code::S_MQ_MESSAGE_SIZE_OVER_OR_UNDERFLOW} // By contract: this specific code for this.
        : Error_code{errno, system_category()}; // Otherwise whatever it was.
  FLOW_LOG_WARNING("Posix_mq_handle [" << *this << "]: mq_*() yielded error; context = [" << context << "].  "
                   "Details follow.");
  FLOW_ERROR_SYS_ERROR_LOG_WARNING();

  return false;
} // Posix_mq_handle::handle_mq_api_result()

const Shared_name& Posix_mq_handle::absolute_name() const
{
  return m_absolute_name;
}

std::ostream& operator<<(std::ostream& os, const Posix_mq_handle& val)
{
  os << '@' << &val << ": sh_name[" << val.absolute_name() << "] native_handle[";
  const auto native_handle = val.native_handle();
  if (native_handle.null())
  {
    return os << "null]";
  }
  // else
  return os << native_handle << ']';
}

namespace
{

std::string shared_name_to_mq_name(const Shared_name& name)
{
  // Pre-pend slash.  See `man mq_overview`.
  std::string mq_name("/");
  return mq_name += name.str();
}

} // namespace (anon)

} // namespace ipc::transport
