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

/* @todo More consistent to move this below `#include "ipc/..."`; but flow/common.hpp needs to #undef a couple things
 * before `#define`ing them (FLOW_LOG_CFG_COMPONENT_ENUM_*) for that to work.  It really should anyway. */
#include <flow/util/util.hpp>

#include "ipc/detail/common.hpp"
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/filesystem.hpp>

/* We build in C++17 mode ourselves (as of this writing), but linking user shouldn't care about that so much.
 * The APIs and header-inlined stuff (templates, constexprs, possibly explicitly-inlined functions [though we avoid
 * those]), however, also requires C++17 or newer; and that applies to the linking user's `#include`ing .cpp file(s)!
 * Therefore enforce it by failing compile unless compiler's C++17 or newer mode is in use.
 *
 * Note this isn't academic; as of this writing there's at least a C++14-requiring constexpr feature in use in one
 * of the headers.  So at least C++14 has been required for ages in actual practice.  Later, notched it up to C++17
 * by similar logic. */
#if (!defined(__cplusplus)) || (__cplusplus < 201703L)
#  error "To compile a translation unit that `#include`s any ipc/ API headers, use C++17 compile mode or later."
#endif

/**
 * Catch-all namespace for the Flow-IPC project: A library/API in modern C++17 providing high-performance
 * communication between processes.  It includes schema-based structured message definition and zero-copy transport
 * between processes.  It also includes a SHared Memory (SHM) module for direct allocation and related needs; and
 * particular support for passing references to such *bulk* objects through the aforementioned messaging transport
 * system.
 *
 * @note Nomenclature: The project is called Flow-IPC.
 *
 * From the user's perspective, one should view this namespace as the "root," meaning it consists of two parts:
 *   - Symbols directly in Flow-IPC: The absolute most basic, commonly used symbols (such as the alias
 *     ipc::Error_code).  There should be only a handful of these, and they are likely to be small.
 *     - In particular this includes `enum class` ipc::Log_component which defines the set of possible
 *       `flow::log::Component` values logged from within all modules of Flow-IPC.  See end of common.hpp.
 *   - Sub-namespaces (like ipc::transport, ipc::util), each of which represents an Flow-IPC *module* providing
 *     certain grouped functionality.  The modules are discussed just below.
 *
 * Flow-IPC modules overview
 * ---------------------------
 * Unlike with, say, Boost or Flow, the user of Flow-IPC should be familiar with the totality of its modules.  They're
 * interrelated and to be used together in a typical application.  Contrast with how one might use
 * `flow::log` but not `flow::async`, or boost.asio but not boost.random.  Summary of modules follows:
 *
 *   - *ipc::transport*:
 *     It allows for low-level (unstructured) and structured message passing between each given pair of
 *     processes A and B.  This is really *the point* of Flow-IPC, meaning wanting to use ipc::transport is the reason
 *     one links/includes Flow-IPC in their application in the first place.
 *     - *ipc::transport::struc* contains the structured-message facilities.  To define a structured message,
 *       the user is expected to write a *schema* for it.  As of this writing schemas
 *       are to be written in the Cap'n Proto (capnp) domain-specific language (DSL).  Hence ipc::transport::struc has
 *       capnp as a dependency.
 *       - *ipc::transport::struc::shm* contains some important items, albeit not ones *typically* directly mentioned
 *         by user code, that power the ability to place structured messages directly in SHM, thus enabling end-to-end
 *         *zero-copy* of all messages passed through transport::struc::Channel.
 *   - *ipc::session*:
 *     Before a given process pair A and B talks via ipc::transport, they (typically) will want to establish a
 *     broad conceptual connection (called a *session*) within the context of which all
 *     the talking occurs.  ipc::session is concerned with establishing
 *     such sessions, so that most user code can then mostly forget about that and use ipc::transport to communicate.
 *     It includes safety measures like authentication/tokens, but these should be mostly invisible in day-to-day coding
 *     of IPC logic via ipc::transport.  It *is* possible to establish ipc::transport pipes without sessions, and
 *     for smaller and/or legacy applications and/or prototyping that may be a reasonable approach.
 *     - *ipc::session::shm* conventionally contains SHM-enabled session functionality.  The location of a
 *       given set of classes will mirror the ipc::shm facilities to which those SHM-enabled sessions provides access.
 *       For example ipc::session::shm::classic contains SHM-classic sessions which provide access to arenas
 *       supplied by ipc::shm::classic.  Similarly ipc::session::shm::arena_lend::jemalloc contains SHM-jemalloc sessions
 *       <=> core arena facilities in ipc::shm::arena_lend::jemalloc.  Note the 1-1 naming of namespaces in both cases.
 *   - *ipc::shm*:
 *     It provides explicit shared memory (SHM) functionality, including allocating in SHM -- vaguely
 *     analogously to using the regular heap.  ipc::shm and ipc::transport are co-designed to support transmitting
 *     references to SHM-stored objects.  ipc::session treats setup of SHM arenas for (direct or background) use
 *     in a given session as an important, albeit optional, capability.  Unless the user wants to explicitly
 *     place data structures (such as `struct`s and STL containers) into SHM -- which is an advanced but sometimes
 *     desirable capability -- direct use of ipc::shm is not necessary.
 *   - *ipc::util*: Miscellaneous items.  That said some of these are quite important and oft-used throughout other
 *     modules.  Here we single out, in particular, ipc::util::Shared_name and ipc::util::Native_handle.
 *
 * @note Nomenclature: We refer to Cap'n Proto as capnp, lower-case, no backticks.  Keep to this consistent convention
 *       in comments.
 *
 * The above text views Flow-IPC somewhat as a monolithic whole.  Indeed the present documentation generally treats
 * the entirety of Flow-IPC as available and usable, even though the are various sub-namespaces as shown that break
 * the monolith into cooperating modules.  When it comes to practical needs, this view is sufficient.  Really, most
 * users will (1) start a session (using ipc::session::shm for max performance), (2) use the session to create
 * 1+ ipc::transport::Channel, (3) typically upgrade each to ipc::transport::struc::Channel immediately (a one-liner),
 * (3) use the `struc::Channel` API to send/receive messages with automatic end-to-end zero-copy performance.
 * (4) Optionally one can also access a SHM-arena for direct C++ object placement and access in SHM; the SHM arenas
 * are available from the session object.
 *
 * That said, read on if you want to maintain or otherwise deeper understand Flow-IPC.  There's a deeper organization
 * of this monolith, in which one builds up the whole out of smaller parts, where we generally avoid circular
 * dependencies (A needs B, and B needs A).  Let's briefly go through the process of naming the most basic parts and
 * then showing what depends on them, and so on, until everything is listed in bottom-up order.  To wit:
 *
 *   -# ipc::util: This contains basic, simple building blocks.  ipc::util::Shared_name is used to name various
 *      shared resource throughout Flow-IPC.  ipc::util::Native_handle is a trivial wrapper around a native handle
 *      (FD in POSIX/Linux/Unix parlance).  There are various other items which you'll note when they're mentioned.
 *      - Dependents: Essentially all other code routinely depends on ipc::util.
 *   -# ipc::transport, *excluding* ipc::transport::struc: This is the transport *core layer*.  Think of
 *      this as wrappers around legacy IPC transport APIs with which you may already be familiar: e.g., Unix domain
 *      sockets.  There are key concepts, including ipc::transport::Blob_sender and `Blob_receiver`
 *      (+ `Native_handle_{send|receiv}er`); and their implementations over the aforementioned specific low-level
 *      transports (Unix domain sockets, MQs as of this writing).
 *      - Dependents:
 *        - ipc::transport::struc.  The key point is `transport::struc::Channel`,
 *          the absolute most important object (possibly in all of Flow-IPC), adapts `transport::Channel`, including
 *          for example leveraging that an unstructured `Channel` might contain a blobs pipe *and* a native handles
 *          pipe.
 *        - ipc::session depends on ipc::transport, in that the most important function of an ipc::session::Session
 *          is to *painlessly create* (open) ipc::transport::Channel objects.  You start a session to the opposing
 *          process; and you use that session to create 1+ channels; then you speak over these channels as needed.
 *          (*If* you want to speak using structured messages, you upgrade a `transport::Channel` to a
 *          `transport::struc::Channel`: a structured channel.)
 *   -# ipc::transport::struc: The transport *structured layer* builds on top of (1) the core layer and (2) capnp.
 *      `struc::Channel` adapts an unstructured `Channel`, allowing efficient transmission of structured messages
 *      filled-out according to user-provided capnp-generated schema(s).  At its simplest, wihout the (spoiler alert)
 *      `shm` sub-namespace, an out-message is backed by the regular heap (`new`, etc.); and a received in-message is
 *      a copy also backed by the regular heap of the recipient process.
 *      - Dependents: ipc::session and ipc::session::shm::arena_lend, for orthogonal reasons, use
 *        ipc::transport::struc::Channel for their internal purposes.  It's a useful guy!
 *   -# ipc::session, *excluding* ipc::session::shm: This is the core support for sessions, which are how one painlessly
 *      begins a conversation between your process and the opposing process.  Without it you'll need to worry about
 *      low-level resource naming and cleanup; with it, it's taken care-of -- just open channels and use them.
 *      Spoiler alert: the sub-namespace `shm` (see below) will add SHM capabilities.
 *      - Dependents: ipc::session::shm builds on top of this and hence depends on it.
 *   -# ipc::shm::stl: A couple of key facilities here enable storage of STL-compliant C++ data structures directly
 *      in SHM; e.g., a map from strings to vectors of strings and `struct`s and... etc.  You, the user, will *only*
 *      directly use this, if you need such functionality.  If you only need to send structured messages with max
 *      perf (which internally is achieved using SHM), then you need not directly mention this.
 *      - Dependents:
 *        - ipc::transport::struc::shm "eats our own dog food" by internally representing certain data
 *          structures using STL-compliant APIs, including `list<>` and `flow::util::Blob`.
 *   -# ipc::transport::struc::shm: This essentially just adds `shm::Builder` and `shm::Reader` which are impls
 *      of ipc::transport::struc::Struct_builder and `Struct_reader` concepts that enable end-to-end zero-copy
 *      transmission of any capnp-schema-based message -- as long as one has certain key SHM-enabling objects, most
 *      notably a `Shm_arena`.
 *      - Dependents:
 *        - ipc::session::shm mentions `shm::Builder` and `shm::Reader` in a key convenience alias.
 *        - The bottom line is, if you use SHM-enabled sessions -- which is at least easily the most convenient way
 *          to obtain end-to-end zero-copy perf when transmitting structured messages along channels -- then
 *          ipc::transport::struc::shm shall be used, most likely without your needing to mention it or worry about it.
 *   -# ipc::shm::classic: This is a *SHM-provider* (of SHM-arenas); namely the *SHM-classic* provider.  The core item
 *      is ipc::shm::classic::Pool_arena, a "classic" single-segment (pool) SHM arena with a simple arena-allocation
 *      algorithm.
 *      - Dependents: ipc::session::shm::classic provides SHM-enabled sessions with this SHM-provider as the
 *        required SHM engine.  Naturally in so doing it depends on ipc::shm::classic, especially `classic::Pool_arena`.
 *   -# ipc::shm::arena_lend (more specifically ipc::shm::arena_lend::jemalloc): This is the other *SHM-provider*
 *      (of SHM-arenas); namely the *SHM-jemalloc* provider.  The core item is `jemalloc::Ipc_arena`.
 *      is `Pool_arena`, a "classic" single-segment (pool) SHM arena with a simple arena-allocation algorithm.
 *      - Dependents: ipc::session::shm::arena_lend::jemalloc provides SHM-enabled sessions with this SHM-provider as
 *        the required SHM engine.  Naturally in so doing it depends on ipc::shm::arena_lend::jemalloc,
 *        especially `jemalloc::Ipc_arena`.
 *   -# ipc::session::shm: This namespace adds SHM-enabled sessions.  Namely that adds two capabilities; one, to
 *      easily get end-to-end zero-copy performance along `struc::Channel` objects opened via these sessions.  And,
 *      optionally, two: To have direct access to SHM-arena(s) in which to place and access C++ objects.
 *      - More specifically: ipc::session::shm::classic = SHM-classic-provider-enabled sessions;
 *        ipc::session::shm::arena_lend::jemalloc = SHM-jemalloc-provider-enabled sessions.
 *        - Dependents: none (inside Flow-IPC).
 *
 * Again -- there's no need to understand all this, if you're just using Flow-IPC in the expected mainstream ways.
 * Nevertheless it could be a useful, if wordy, map to the building blocks of Flow-IPC and how they interact.
 *
 * Distributed sub-components (libraries)
 * --------------------------------------
 * The above describes Flow-IPC as a whole.  Generally we recommend a distribution of Flow-IPC which includes all
 * the pieces, to be used at will.  That said, for reasons outside our scope here, this project is actually distributed
 * in a few parts, each of which is a library with a set of header files.  (The generated documentation is created
 * from all of them together, and therefore various comments aren't particularly shy about referring to items across
 * the boundaries between those parts.)  These parts (libraries) are:
 *
 *   - `ipc_core`: Contains: ipc::util, ipc::transport (excluding ipc::transport::struc).
 *   - `ipc_transport_structured`: Contains: ipc::transport::struc (excluding ipc::transport::struc::shm).
 *   - `ipc_session`: Contains: ipc::session (excluding ipc::session::shm).
 *   - `ipc_shm`: Contains: ipc::shm::stl, ipc::transport::struc::shm
 *                          (including ipc::transport::struc::shm::classic but excluding all other
 *                          such sub-namespaces),
 *                          ipc::shm::classic + ipc::session::shm::classic.
 *   - `ipc_shm_arena_lend`: Contains: ipc::transport::struc::shm::arena_lend,
 *                                     ipc::shm::arena_lend + ipc::session::shm::arena_lend.
 *
 * The dependencies between these are as follows:
 *   - `ipc_core` <- each of the others;
 *   - `ipc_transport_structured` <- `ipc_session` <- `ipc_shm` <- `ipc_shm_arena_lend`.
 *     - (There are are, e.g., direct dependencies redundant to the above, such as how `ipc_shm_arena_lend`
 *       depends on `ipc_transport_structured` in certain key internal impl details.  We are not pointing out every
 *       direct dependency here, leaving it out as long as it's implied by another indirect dependency such
 *       as `ipc_shm_arena_lend` indirectly depending on `ipc_transport_structured` via several others. )
 *
 * Each one, in the source code, is in a separate top-level directory; and generates a separate static library.
 * However, their directory structures -- and accordingly the namespace trees -- overlap in naming, which
 * manifests itself when 2 or more of the sub-components are installed together.  For example `ipc_session`
 * places `ipc/session` into the `#include` tree; and `ipc_shm` places `ipc/session/shm/classic` within that.
 *
 * Relationship with Flow and Boost
 * --------------------------------
 * Flow-IPC requires Flow and Boost, not only for internal implementation purposes but also in some of its APIs.
 * For example, `flow::log` is the assumed logging system, and `flow::Error_code` and related conventions are used
 * for error reporting; and `boost::interprocess` and `boost::thread` APIs may be exposed at times.
 *
 * Moreover, Flow-IPC shares Flow "DNA" in terms of coding style, error, logging, documentation, etc., conventions.
 * Flow-IPC and Flow itself are also more loosely inspired by Boost "DNA."  (For example: `snake_case` for identifier
 * naming is inherited from Flow, which inherits it more loosely from Boost; the error reporting API convention is taken
 * from Flow which uses a modified version of the boost.asio convention.)
 *
 * Documentation / Doxygen
 * -----------------------
 * All code in the project proper follows a high standard of documentation, almost solely via comments therein
 * (plus a guided Manual in manual/....dox.txt files, also as Doxygen-read comments).
 * The standards and mechanics w/r/t documentation are entirely inherited from Flow.  Therefore, see the
 * `namespace flow` doc header's "Documentation / Doxygen" section.  It applies
 * verbatim here (within reason).  (Spoiler alert: Doc header comments on most entities (classes, functions, ...) are
 * friendly to doc web page generation by Doxygen.  Doxygen is a tool similar to Javadoc.)
 *
 * The only exception to this is the addition of the aforementioned guided Manual as well which Flow lacks as of
 * this writing (for the time being).
 *
 * Using Flow-IPC modules
 * ------------------------
 * This section discusses usability topics that apply to all Flow-IPC modules including hopefully any future ones but
 * definitely all existing ones as of this writing.
 *
 * ### Error reporting ###
 * The standards and mechanics w/r/t error reporting are entirely
 * inherited from Flow.  Therefore, see the `namespace flow` doc header's "Error reporting" section.  It applies
 * verbatim (within reason) here.
 *
 * ### Logging ###
 * We use the Flow log module, in `flow::log` namespace, for logging.  We are just a consumer, but this does mean
 * the Flow-IPC user must supply a `flow::log::Logger` into various APIs in order to enable logging.  (Worst-case,
 * passing `Logger == null` will make it log nowhere.)  See `flow::log` docs.  Spoiler alert: You can hook it up to
 * whatever logging output/other logging API you desire, or it can log for you in certain common ways including console
 * and rotated files.
 *
 * @internal
 *
 * Implementation notes
 * --------------------
 * There is a high standard of consistency and style, as well as documentation, in Flow-IPC.  The standards and
 * mechanics for all such aspects (including source tree org, code style, doc style) are entirely
 * inherited from Flow.  Therefore, see the `namespace flow` doc header: its analogous sections
 * apply verbatim here (within reason).
 *
 * To-dos and future features
 * --------------------------
 * Firstly see `namespace flow` doc header's similar section.  There's a good chance each of those to-dos applies
 * to Flow-IPC as well.  Further general to-dos should be added right here as needed over time.
 */
namespace ipc
{

// Types.  They're outside of `namespace ::ipc::util` for brevity due to their frequent use.

/* (The @namespace and @brief thingies shouldn't be needed, but some Doxygen bug necessitated them.
 * See flow::util::bind_ns for explanation... same thing here.) */

/**
 * @namespace ipc::bipc
 * @brief Short-hand for boost.interprocess namespace.
 */
namespace bipc = boost::interprocess;

/**
 * @namespace ipc::fs
 * @brief Short-hand for `filesystem` namespace.
 *
 * ### Rationale for aliasing to `boost::filesystem` instead of `std::filesystem` ###
 * `boost::filesystem` is rock-solid and the model/original impl; which is not to say that always is enough
 * to take it over the `std::` counterpart.  However, some experiences with gcc-7's `std::filesystem` were
 * negative; it did not exist, and `std::experimental::filesystem` lacked basic chunks from the standard.
 * This left a bad taste in the mouth; whereas in the author's (ygoldfel) experience Boost's has been great.
 * It is very mature.
 */
namespace fs = boost::filesystem;

/// Short-hand for `flow::Error_code` which is very common.
using Error_code = flow::Error_code;

/// Short-hand for polymorphic functor holder which is very common.  This is essentially `std::function`.
template<typename Signature>
using Function = flow::Function<Signature>;

#ifdef IPC_DOXYGEN_ONLY // Actual compilation will ignore the below; but Doxygen will scan it and generate docs.

/**
 * The `flow::log::Component` payload enumeration containing various log components used by Flow-IPC internal logging.
 * Internal Flow-IPC code specifies members thereof when indicating the log component for each particular piece of
 * logging code.  Flow-IPC user specifies it, albeit very rarely, when configuring their program's logging
 * such as via `flow::log::Config::init_component_to_union_idx_mapping()` and
 * `flow::log::Config::init_component_names()`.
 *
 * If you are reading this in Doxygen-generated output (likely a web page), be aware that the individual
 * `enum` values are not documented right here, because `flow::log` auto-generates those via certain macro
 * magic, and Doxygen cannot understand what is happening.  However, you will find the same information
 * directly in the source file `log_component_enum_declare.macros.hpp` (if the latter is clickable, click to see
 * the source).
 *
 * ### Details regarding overall log system init in user program ###
 * See comment in similar place in `flow/common.hpp`.
 */
enum class Log_component
{
  /**
   * CAUTION -- see ipc::Log_component doc header for directions to find actual members of this
   * `enum class`.  This entry is a placeholder for Doxygen purposes only, because of the macro magic involved
   * in generating the actual `enum class`.
   */
  S_END_SENTINEL
};

// Constants.

/**
 * The map generated by `flow::log` macro magic that maps each enumerated value in ipc::Log_component to its
 * string representation as used in log output and verbosity config.  Flow-IPC user specifies, albeit very rarely,
 * when configuring their program's logging via `flow::log::Config::init_component_names()`.
 *
 * As an Flow-IPC user, you can informally assume that if the component `enum` member is called `S_SOME_NAME`, then
 * its string counterpart in this map will be auto-computed to be `"SOME_NAME"` (optionally prepended with a
 * prefix as supplied to `flow::log::Config::init_component_names()`).  This is achieved via `flow::log` macro magic.
 *
 * @see ipc::Log_component first.
 */
extern const boost::unordered_multimap<Log_component, std::string> S_IPC_LOG_COMPONENT_NAME_MAP;

#endif // IPC_DOXYGEN_ONLY

} // namespace ipc
