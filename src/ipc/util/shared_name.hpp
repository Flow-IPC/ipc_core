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

#include "ipc/util/shared_name_fwd.hpp"
#include "ipc/common.hpp"

namespace ipc::util
{

/**
 * String-wrapping abstraction representing a name uniquely distinguishing a kernel-persistent
 * entity from all others in the system, or a fragment of such a name.  Conceptually it relates to `std::string`
 * similarly to how `filesystem::path` does.
 *
 * This is a very simple class in terms of what logic it actually adds: it encapsulates an `std::string` and allows for,
 * basically, string operations like concatenation, with some syntactic sugar added for a simple *folder* convention.
 * However, the design *context* (namely, how shared resources are named and why) is less trivial, and this doc header
 * is a good place to get into those topics as well.  Hence, we first cover practical aspects, with the architectural
 * context referenced only as needed.  Then, below that, there's an architecture discussion about naming in general.
 *
 * ### Construction/assignment from and conversion to strings/similar ###
 * Interanally it stores an `std::string`.
 *
 * Conversion: That `string` is accessible by `const&` via str() and similarly the NUL-terminated native_str().
 * Also there is an `ostream<<` printer; do note it does not simply print str() but rather a beautified version.
 * (`ostream>>` input is also provided but is identical to `ostream >> string`.)
 *
 * Construction/assignment: It comes with standard default/copy/move ctors and assignment.  There is also a
 * constructor that takes two iterators-to-`char` (including 2 `const char*`).  However, construction
 * from `string` (including destuctive move-construction), util::String_view, NUL-terminated `const char*`,
 * `vector<char>` (etc.) is available exclusively in the form of `static` quasi-ctors ct().  Lastly: To assign please
 * use move ctor: `existing_sh_name = Shared_name::ct(src)`.
 *
 * Rationale: C++ ADL semantics cause a ton of super-annoying problems -- especially inside ipc::transport
 * namespace itself -- where compilers will auto-convert, e.g., a `string` to `Shared_name` without being asked
 * to do so at all.  For example, one sees `operator << string` output the `string` in "beautified" form, because
 * a `Shared_name` is constructed implicitly and then printed via its `<<`.  I (ygoldfel), then, took a cue from
 * boost.asio's IP address classes which use `static` quasi-ctors to avoid any such issues.  (The alternative
 * was to fine-tune the ctor set a-la `filesystem::path`, with conversion traits and all kinds of craziness.)
 *
 * ### Practical Shared_name matters ###
 * A *shared resource* in this context is some entity (such as a SHM-mapped addressable area; or a POSIX message queue)
 * that can be *opened* for further access by potentially 2+ processes simultaneously.  When opening (at least),
 * the shared resource is referred to -- in the associated opening sys call or similar -- by a string name, and the name
 * works equally from those 2+ processes.  Shared_name stores such a string.  It is also suitable for *fragments* of
 * these names, including prefixes, suffixes, or middle parts.  Therefore -- much like `boost::filesystem::path` is
 * basically a string wrapper -- Shared_name is just a string wrapper.  In fact the `std::string` it stores is
 * accessible through str() by reference.
 *
 * All standard string-like operations (generally, the `+=`, `/=`, `+`, and `/` operations) are equally performant and
 * not *any* smarter than the corresponding string concatenation ops (the `/` variants just add a separator character).
 * The same holds for all operations, except see the next section.  In particular, all characters are allowed in any
 * position, and there is no max length enforced.  Hence, the user may assume max possible performance and zero
 * restrictions, with the exception of:
 *
 * ### Conventions understood/enforced by Shared_name ###
 * As noted, generally the class allows everything and does nothing smart that `std::string` wouldn't do.  This is for
 * flexibility and performance and is inspired by `boost::filesystem::path`.  However, there is *optional* support
 * for some simple conventions.  First let's discuss those conventions:
 *
 * In Flow-IPC, Shared_name is to be used for all shared resource *types* needed.  (These are not enumerated here.)
 * Different resource types might have different rules for (1) characters allowed; and (2) max length allowed; so that
 * if this is violated, a creation/opening sys call (or similar) will fail with an error.  Therefore, the following
 * *conventions* are understood and represent the *union* of known system restrictions, so that sanitized()
 * names will work for *all* known resource types.  Also, some conventions are for human usability.
 * In total, these conventions are understood:
 *   - Only certain characters are allowed.  Namely, only alphanumerics [A-Za-z0-9] are allowed as of this writing.
 *   - There is a *folder* convention: We anticipate a file tree-like organization of various items; hence a
 *     folder separator character, #S_SEPARATOR, is allowed also.
 *     - A complete, usable (in a sys call or similar) name is to start with one leading #S_SEPARATOR.
 *     - We anticipate no empty "folder" names; and therefore no sequences of 2+ adjacent #S_SEPARATOR chars.
 *   - Names shall not exceed a certain max length.  #S_MAX_LENGTH is chosen to be low enough to work for all supported
 *     resource types.
 *
 * How does Shared_name actually *use* the knowledge of these conventions?  To reiterate: normally, it does not care.
 * The only parts of its API that *do* care are as follows:
 *   - absolute() will return `true` if and only if the first char is #S_SEPARATOR.  By convention, you should not
 *     pass str() to a sys call/similar, unless `absolute() == true`.
 *   - sanitized() will return `true` if and only if str() is a valid name or name fragment (no illegal characters;
 *     does not exceed #S_MAX_LENGTH; no multi-separator sequences).
 *   - sanitize() will attempt to non-destructively modify (if needed) name in such a way as to make sanitized() return
 *     `true`.
 * You should call them only when you specifically need some conventions checked or enforced.  Otherwise Shared_name
 * ops will be as dumb/fast as possible.
 *
 * @note The Flow-IPC library *user* is unlikely to directly pass a Shared_name into a sys call or similar.  Probably
 *       Flow-IPC internals will do it for the user.  Therefore, the likeliest pattern for the user to encounter in
 *       public Flow-IPC APIs is: When initially naming some `transport` object, a constructor or factory will take
 *       a *relative* (`absolute() == false`) Shared_name prepared by the user.  Within that relative fragment,
 *       the user is to use the folder conventions above (via `/=` or `/` operators perhaps) if needed.  Inside
 *       Flow-IPC, the impl can then construct an absolute name by internally pre-pending stuff to the
 *       user-constructed fragment. If you have ensured the name is sanitized(), then it will not fail on account of a
 *       bad name.  If you have *not*, then it may still not fail: sanitized() is a conservative criterion and may be
 *       too stringent for some resource types and OS calls.  It is up to *you* to either ensure sanitized() or
 *       otherwise worry about the possibility of a bad name (illegal characters, excessive length).
 *
 * ### Thread safety ###
 * Shared_name has the same safety under concurrency as `std::string` (i.e., if you intend on writing while
 * reading/writing same `*this` concurrently, synchronized access is necessary to avoid corruption and other standard
 * thread safety violations).
 *
 * @internal
 * ### Architecture discussion / shared name convention synopsis ###
 * As promised, we've covered the practical aspects of Shared_name first.  Now let's discuss the larger design.
 * Note that while this does inform some of Shared_name API (which is quite simple), really the discussion has wider
 * implications, and this doc header merely seems like a good place for it.  It would've been okay to split off
 * much of the below info into ipc::session somewhere, but... maybe that's better... but the human in me (ygoldfel)
 * feels you'll want to know this now, here, even though for formal appropriateness maybe it should be moved.
 * In any case here it is:
 *
 * As noted before, there are N shared resource types, and Shared_name is to be used for all of them.  At minimum,
 * then:
 *   - 2 names are to be equal if and only if they refer to the same object of the same type, or they're 2 different
 *     objects but of *different* types.
 *   - This must be the case at each given point in time.
 *
 * When designing Flow-IPC, we decided that if would be helpful for humans if we immediately made this more stringent
 * by enforcing the following stronger requirement as well:
 *   - 2 names are to be equal if and only if they are referring to objects of the same (resource) type.  So then,
 *     e.g., if a SHM-area is named "X" then a POSIX queue cannot also be named "X" at the same point in time.
 *
 * Further, some things are made easier if the distinctness restriction (2 distinct names <=> 2 distinct objects
 * at each given point in time) is made more stringent by extending it to *all time*; resulting in:
 *   - 2 names are to be equal if and only if they refer to the same object of the same type.
 *   - This must be the case over all time (not merely a each *given* point in time).
 *     - In other words, if a name "X" is used for some object of some type, then it's only *ever* used for that
 *       object and never another (until reboot).
 *
 * @note Shared resources are assumed to be *kernel-persistent*, which means the slate is wiped with each system boot.
 *       Hence "all time" really means "within the relevant uptime."
 *
 * Practically speaking, then, how do we enable these distinctness guarantees, in absolute names (i.e., names to be
 * passed, usually internally by Flow-IPC, into sys calls/similar)?  Firstly, the resource type for the object
 * referred to by each absolute name should be encoded in a standard prefix inside the name; a simple distinct constant
 * for each resource type works.  There should be a handful at most of these possible values.  This will guarantee
 * non-clashing between different types of resource.
 *
 * That only leaves the distinct-across-all-time requirement.  However, it's outside the scope of this header to
 * explain how that's achieved, as it involves ipc::session and other concepts and is just a large topic.
 *
 * For convenience, we now summarize the resulting absolute shared name convention, as of this writing, without a full
 * rationale and with references to other doc headers if appropriate.  In order, the characters of a shared name
 * in the Flow-IPC convention are as follows.  Use session::build_conventional_shared_name() to construct a
 * path with these semantics.
 *
 *   -# Leading #S_SEPARATOR.  Indicates a full (absolute) name or prefix thereof.
 *   -# A certain constant magic string, #S_ROOT_MAGIC, indicating ::ipc is operating.
 *      (E.g., if... uh... emacs likes to put stuff in, say, SHM, at a certain SHM path, hopefully it won't clash
 *      with us accidentally.)
 *   -# A #S_SEPARATOR.
 *   -# Hard-coded resource type constant.  Indicates resource type.  E.g., something like "posixq" might mean a
 *      POSIX message queue.  The (quite few) possible values for this are stored internally in Flow-IPC.
 *   -# A #S_SEPARATOR.
 *   -# Session-server application name.  This brief name is 1-1 with all distinct executables supported by Flow-IPC.
 *      See ipc::session for details.
 *   -# A #S_SEPARATOR.
 *   -# Server-namespace.  This uniquely identifies the server application *instance* across all time, for a given
 *      session-server application name.  See ipc::session for how this value is computed.
 *      In the rare case where the resource applies to *all* server-namespaces, use special value #S_SENTINEL.
 *   -# A #S_SEPARATOR.
 *   -# What comes next depends on the *scope* of this particular resource.
 *      - Relevant to a given session A-B (where A and B are processes that can converse with each other),
 *        a/k/a *per-session scope*:
 *        -# Session-client application name.  Analogous to session-server application name but for the supported
 *           clients.  See ipc::session for details.
 *        -# A #S_SEPARATOR.
 *        -# Client-namespace.  Given everything up to this point, uniquely identifies client application *instance*
 *           across all time (like server-namespace above, basically).  See ipc::session for how it's computed.
 *      - Relevant to no particular process pair A-B, but does apply only to a given session-client application,
 *        a/k/a *per-client-app scope*:
 *        -# Session-client application name, as just above.
 *        -# A #S_SEPARATOR.
 *        -# The reserved client-namespace, #S_SENTINEL, indicating no particular session-client.
 *      - Global (not limited to any smaller scope above), a/k/a *global scope*.  This is intended largely for
 *        internal Flow-IPC needs, as of this writing.
 *        -# The reserved session-client application name, #S_SENTINEL, indicating no particular client application.
 *   -# A #S_SEPARATOR.
 *   -# The rest of the name depends on the convention decided for that particular resource type and hence outside
 *      our scope here; see doc header for the relevant class corresponding to a shared resource type of interest.
 *      - For each object, determine the *scope* (the 3 possibilities and resulting naming conventions are just above).
 *      - Keep using the basic folder convention, with non-empty folder names and #S_SEPARATOR as separator.
 *      - When uniqueness is desired, the `1`->`2`->`3`... technique is typically used (always remembering that
 *        such an auto-ID must be safely generated across all participating processes in system).
 *
 * In rare cases a Shared_name will be used outside the ipc::session paradigm, such as for a lower-level purpose
 * that nevertheless requires a shared-resource name (e.g., for a lower-level-use SHM pool).  In that case
 * the following shall be equal to #S_SENTINEL: session-server application name; server-namespace.  The components
 * subsequent to that shall be as-if it's a global-scope resource; hence: #S_SENTINEL.
 * build_conventional_non_session_based_share_name() shall construct such a name.
 *
 * @note *camelCase*: As of this writing, #S_SEPARATOR is `_` (because `/` is not available for native reasons).
 *       So, when it comes up, the convention is to use camelCase for tokens between nearby separators.  Briefly:
 *       start with lower case; do not word-smoosh; each new word after word 1 is to begin with exactly 1 capital or
 *       number; an acronym is to be treated as a single word.  Examples of compliant tokens:
 *       `httpUrlConnectionV44Beta`, `tableIdent`.  These are bad: `httpURLConnection` (wrong acronym convention),
 *       `HTTPURLConnection` (ditto), `tableident` (word-smooshing).
 *
 * ### Rationale for max length handling ###
 * As noted in the public section of this doc header: #S_MAX_LENGTH is a part of sanitized() criteria; it is not
 * enforced by Shared_name itself outside of sanitized() and sanitize().  It is up to the Shared_name user (which
 * may, and usually is, internal Flow-IPC code) to actually ensure sanitized() is `true` -- or otherwise avoid system
 * API errors on account of bad names.  All *we* choose to do, regarding the length aspect, is choose one conservatively
 * low so that:
 *   - At least it is no greater than the *lowest* inherent limit out of all the known resource types to which
 *     Shared_name apply.
 *   - Additionally, because in many cases a given `public` API takes a relative Shared_name and pre-pends an
 *     absolute "folder" path of known length, further leave slack to make it so that that prefix can still comfortably
 *     fit without hitting the limit from the previous bullet point.
 *
 * To wit, then, current tally of relevant resource types:
 *     - POSIX message queue (`man mq_overview`): Linux: An internal forward-slash, included in `NAME_MAX = 255`
 *       characters including NUL; so apparently: 253.  (The `man` text could be interpreted as meaning 253, 254, 255;
 *       it is ambiguous.  Experiments show it's 253.)
 *     - SHM pool (ultimately: `man shm_open`): Linux: Same as preceding: 253.
 *     - bipc message queue (search: boost.interprocess `message_queue`): It just opens a SHM pool per MQ; so
 *       same as preceding: 253.
 *     - Unix domain sockets (ultimately: `man unix`): Linux, abstract namespace: It's a hairy topic.  `sun_path[108]`
 *       is the case as of this writing; minus 1 leading and trailing NUL; and there's a note some (non-Linux?) Unix
 *       domain socket implementations go as low as `sun_path[92]`.  But in Linux: 106.
 *
 * So that puts us at 107 without the "prefix slack."  Applying a prefix slack of 32 characters gives us: 107 - 32 =
 * 75.
 */
class Shared_name
{
public:
  // Constants.

  /// A (default-cted) Shared_name.  May be useful for functions returning `const Shared_name&`.
  static const Shared_name S_EMPTY;

  /**
   * Max value of size() such that, if str() used to name a supported shared resource, sys call safely won't barf.
   *
   * @todo Shared_name::S_MAX_LENGTH currently applies to all shared resource types, but it'd be a useful feature to
   * have different limits depending on OS/whatever limitations for particular resources types such as SHM
   * object names versus queue names versus whatever.
   *
   * @internal
   *
   * @todo Research real limits on Shared_name::S_MAX_LENGTH for different real resource types; choose something for
   * MAX_LENGTH that leaves enough slack to avoid running into trouble when making actual sys calls; as discussed in
   * the at-internal section of Shared_name doc header about this topic.  Explain here how we get to the
   * limit ultimately chosen.  The limit as of this writing is 64, but real research is needed.
   */
  static const size_t S_MAX_LENGTH;

  /// Character we use, by convention, to separate conceptual folders within str().
  static const char S_SEPARATOR;

  /**
   * A Shared_name fragment, with no #S_SEPARATOR characters inside, to be used in any Shared_name maintained by
   * ::ipc itself as the leading path component.
   */
  static const Shared_name S_ROOT_MAGIC;

  /**
   * A Shared_name fragment, with no #S_SEPARATOR characters inside, that represents a path component that shall
   * be different from any other generated string at the same path depth in the same context; and represents
   * a sentinel.  In actual fact it equals `0`; and typically (though not necessarily -- other conventions may exist)
   * generated strings shall be `1`, `2`, ..., of which the former is #S_1ST_OR_ONLY.
   */
  static const Shared_name S_SENTINEL;

  /**
   * A Shared_name fragment, with no #S_SEPARATOR characters inside, that represents a path component that
   * (1) is not #S_SENTINEL and (2) is suggested as the *first* or *only* unique ID of
   * items at the same depth in the same context.  In actual fact it equals `1`; and typically (though not
   * necessarily -- other conventions may exist) generated strings shall be `1`, `2`, ..., of which
   * the former is #S_1ST_OR_ONLY.
   *
   * So if #S_SENTINEL means "not a thing" or "special thing to be treated not like a normal thing",
   * then #S_1ST_OR_ONLY means "a normal thing, of which there may be only one, and this is the first one
   * so created."
   *
   * ### Suggested use pattern ###
   * This is most useful when, at this path level, you currently only have reason to have one object.
   * Then you can name it #S_1ST_OR_ONLY without relying on a magic string `1`.  However, if in the future
   * there is reason to add more objects at the same level, then remove the use of `S_1ST_OR_ONLY` and instead
   * use a `uint` or `atomic<uint>` data member that starts at simply the numeric `1`, `++`ing it each time a
   * new object is added (and converting it via `Shared_name::ct(std::to_string(x))` to string).
   * The first object will have the same name as before, compatibly, while subsequent ones will be
   * efficiently named uniquely.
   *
   * Is this cheesy and reliant on the fact that this constant is, in fact, a string conversion of the number one?
   * Yes, but the above pattern works and is reasonably efficient.  I (ygoldfel) considered making this
   * a super-general API that keeps generating unique IDs, but it just seemed like overkill given the simplicity
   * of the task.  Converting back from Shared_name, generating a new `++`ed one, then converting back -- while
   * providing a simple-enough API to ensure atomicity -- seemed inferior to just letting people
   * maintain a compatible `atomic<uint>`, when and if desired, and using #S_1ST_OR_ONLY until then.
   */
  static const Shared_name S_1ST_OR_ONLY;

  /// Relative-folder fragment (no separators) identifying the resource type for: SHM pools.
  static const Shared_name S_RESOURCE_TYPE_ID_SHM;

  /// Relative-folder fragment (no separators) identifying the resource type for: boost.interprocess named mutex.
  static const Shared_name S_RESOURCE_TYPE_ID_MUTEX;

  // Constructors/destructor.

  /// Constructs empty() name.
  Shared_name();

  /**
   * Copy-constructs from an existing Shared_name.
   *
   * @param src
   *        Source object.
   */
  Shared_name(const Shared_name& src);

  /**
   * Move-constructs from an existing Shared_name, which is made empty() if not already so.
   *
   * @param src_moved
   *        Source object, which is potentially modified.
   */
  Shared_name(Shared_name&& src_moved);

  /**
   * Copy-constructs from a `char` range given as a pair of random-iterators; in particular `const char*`s work.
   *
   * @tparam Input_it
   *         An STL-compliant random iterator type.  In particular `const char*` works.
   * @param begin
   *        Start of range to copy.
   * @param end
   *        One past last element in range to copy (`begin` to copy nothing).
   */
  template<typename Input_it>
  explicit Shared_name(Input_it begin, Input_it end);

  // `static` ctors.

  /**
   * Copy-constructs from a `char`-sequence container (including `string`, util::String_view, `vector<char>`).
   * This overload shall be used on a non-`&&` arg if `ct(const char*)` does not apply.
   *
   * Specifically the returned object's internal `std::string` is constructed as: `std::string s(raw_name)`.
   * As a result, whatever most-performant available single-arg ctor `basic_string` makes available is forwarded-to.
   * (E.g., C++17 has a `String_view_like` ctor which is overload-resolved-to only when it most makes sense.)
   *
   * ### Rationale ###
   * See class doc header for rationale as-to why this is a `static` ctor as opposed to a regular ctor.
   *
   * @tparam Source
   *         See above.
   * @param src
   *        String to copy.
   * @return The new object.
   */
  template<typename Source>
  static Shared_name ct(const Source& src);

  /**
   * Copy-constructs from a NUL-terminated `const char*` string.
   *
   * ### Rationale ###
   * See class doc header for rationale as-to why this is a `static` ctor as opposed to a regular ctor.
   *
   * @tparam Source
   *         See above.
   * @param src
   *        String to copy.
   * @return The new object.
   */
  static Shared_name ct(const char* src);

  /**
   * Destructively move-constructs from an `std::string`, emptying that source object.
   *
   * ### Rationale ###
   * See class doc header for rationale as-to why this is a `static` ctor as opposed to a regular ctor.
   *
   * @param src_moved
   *        String to move (make-`.empty()`).
   * @return The new object.
   */
  static Shared_name ct(std::string&& src_moved);

  // Methods.

  /**
   * Copy-assigns from an existing Shared_name.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Shared_name& operator=(const Shared_name& src);

  /**
   * Move-assigns from an existing Shared_name.
   *
   * @param src_moved
   *        Source object, which is potentially modified.
   * @return `*this`.
   */
  Shared_name& operator=(Shared_name&& src_moved);

  /**
   * Returns (sans copying) ref to immutable entire wrapped name string, suitable to pass into sys calls when naming
   * supported shared resources assuming `absolute() == true`.  If you require a NUL-terminated string (such as for
   * a native call), use native_str().
   *
   * @note Returning a util::String_view seemed pointless, as one can always be constructed easily by the caller, and
   *       in any case -- for the time being at least -- some/many APIs that take non-C-strings will take `std::string`
   *       as opposed to a util::String_view.
   *
   * @return See above.
   */
  const std::string& str() const;

  /**
   * Returns (sans copying) pointer to NUL-terminated wrapped name string, suitable to pass into sys calls when naming
   * supported shared resources assuming `absolute() == true`.  If you require an `std::string` (such as for some fancy
   * boost.interprocess call) See also str().
   *
   * ### Why isn't it named `c_str()`? ###
   * It was, but `capnp::StringPtr` (and similar/derived classes such as `capnp::Text::Reader`) has a "clever"
   * `operator T()` conversion operator that is enabled for all `T` that have `.c_str()`; and its implementation
   * relies on being able to symmetrically construct `T(const char*)` -- like `std::string`.  We intentionally
   * lack that.  Hence renamed this away from `.c_str()`.  This may appear like kow-towing to capnp's quirks,
   * but actually conceivably that's a pattern people use, so let's not break it.
   *
   * @return See above.
   */
  const char* native_str() const;

  /**
   * Returns `str().size()`.
   * @return See above.
   */
  size_t size() const;

  /**
   * Returns `true` if and only if `str().empty() == true`.
   * @return See above.
   */
  bool empty() const;

  /**
   * Returns `true` if and only if `!this->empty()`, and str() ends with the #S_SEPARATOR character.
   * @return See above.
   */
  bool has_trailing_separator() const;

  /**
   * Returns `true` if and only if the first character is #S_SEPARATOR.  By Flow-IPC convention, any name actually
   * passed to a sys call in order to name a shared resource has `absolute() == true`.  Note, however, that absolute()
   * being `true` does not necessarily mean str() is the full name of a resource: it may well still be a fragment
   * (a prefix) of some eventual name passed to a sys call.  To obtain the full name one would append more stuff.
   *
   * @return See above.
   */
  bool absolute() const;

  /// Makes it so `empty() == true`.
  void clear();

  /**
   * Returns `true` if and only if the contained name/fragment is *sanitized* according to length, legal characters,
   * and similar.  More precisely, a sanitized `*this` satisfies all of the following:
   *   - There are no illegal characters.  (E.g., in particular, some characters would not be accepted when naming
   *     a SHM object in Linux.)
   *     - In particular, only #S_SEPARATOR is used as a folder separator.
   *   - There is no more than 1 instance of #S_SEPARATOR character in a row.
   *   - size() does not exceed #S_MAX_LENGTH.
   *
   * ### Performance ###
   * It is linear-time, with at most one scan through str().
   *
   * @return See above.
   */
  bool sanitized() const;

  /**
   * Best-effort attempt to turn sanitized() from `false` to `true`, unless it is already `true`; returns the final
   * value of sanitized() indicating whether it was successful.  If `false` returned, then the final value of str()
   * will equal the initial value.
   *
   * This utility should be used very judiciously and with full knowledge of what it actually does.  It should *not* be
   * used "just in case" or "prophylactically" but only with full knowledge of where the current value of str() might
   * originate.  For example, it might come from some user input.  Another example involves, perhaps, concatenating
   * two path fragments in such a way as to potentially yield a double-#S_SEPARATOR situation: sanitize() after this
   * would get collapse the separators into just 1 separator.  Yet another example is simply that if one combines
   * two paths which don't exceed #S_MAX_LENGTH, but the result might exceed it: running sanitize() and ensuring it
   * returns `true` guaranteed one didn't accidentally exceed #S_MAX_LENGTH.
   *
   * In other words, use it when you want it to do what it, in fact, does.  And that is:
   *   - Any '/' (forward-slash) character, as a special case, is transformed into #S_SEPARATOR.
   *   - After any character replacements above: Any sequence of 2 or more #S_SEPARATOR characters is collapsed into
   *     one #S_SEPARATOR.
   *
   * Things it does *not* do, except the above:
   *   - No legal or illegal character is changed (except '/' and separator collapsing).
   *   - It doesn't truncate to try to bring length to #S_MAX_LENGTH or less.
   *
   * However, the method's return value is still significant, in that if it is "still" `false`, then you know you have
   * a real problem -- yet if it's `true`, then it didn't do anything destructive to make it so.
   *
   * Note, again, that the above replacements are "undone" if `false` is returned.  In other words, the function
   * won't sanitize "halfway."
   *
   * ### Performance ###
   * There is at most one linear scan through str().  In addition, though only if actual sanitizing (by changing
   * str()) might be necessary: an allocation, copy of str(), and deallocation may be performed.
   * Overall, it is linear-time regardless, plus those potential alloc/dealloc ops.
   *
   * @return What sanitized() would return just before returning from the present function.
   */
  bool sanitize();

  /**
   * Appends a folder separator followed by the given other Shared_name.
   * Functionally equivalent to `return *this += (string(1, S_SEPARATOR) + src_to_append.str());`.
   * If there's any reason to suspect `this->str()` already ends in #S_SEPARATOR, and/or that the resulting name
   * might be too long, execute sanitize() afterwards and ensure that returns `true`.
   *
   * It is stylistically (and possibly for performance) better to use this rather than manually appending
   * a separator and then `src_to_append` (with `+=`).
   *
   * ### Rationale for not doing something smarter like avoiding a double-separator due to concatenation ###
   * Basically we want things to be as fast as possible by default; and that is to make it as close to trivial
   * string concatenation as possible.  If more paranoia is required, we want the user to intentionally use
   * sanitize() or sanitized().
   *
   * @param src_to_append
   *        Thing to append after appending separator.
   * @return `*this`.
   *
   * @internal
   * ### Rationale for having this `/=` as opposed to only the version taking `Source`, or vice versa ###
   * If we had only the one taking Shared_name, then tried to use `+= X`, where `X` is `std::string` or `const char*`
   * or util::String_view, it'd have to (implicitly) construct a Shared_name first, and that is a bit slower.
   *
   * If we had only the one taking `Source` (e.g., `String_view`), and user wanted to append a Shared_name, they'd
   * have to use `X.str()` explicitly, losing a bit of syntactic sugar.
   *
   * Same deal with `+=` and the binary (`+`, `/`) derivations of these.
   */
  Shared_name& operator/=(const Shared_name& src_to_append);

  /**
   * Simply appends a folder separator followed by `raw_name_to_append` to the current value of str().
   * Specifically the internal `std::string` is modified as: `s += S_SEPARATOR; s += raw_name_to_append`.
   * As a result, whatever most-performant available (single-arg by definition) `operator+=` that `basic_string`
   * makes available is forwarded-to for the 2nd `+=`.  (E.g., C++17 has a `String_view_like` appender which is
   * overload-resolved-to only when it most makes sense.)
   *
   * If there's any reason to suspect `this->str()` already ends in #S_SEPARATOR, and/or that the resulting name
   * might be too long, execute sanitize() afterwards and ensure that returns `true`.
   *
   * It is stylistically (and possibly for performance) better to use this rather than manually appending
   * a separator and then `src_to_append` (with `+=`).
   *
   * See Rationale(s) for the other operator/=(), as they apply here.
   *
   * @tparam Source
   *         See above.
   * @param raw_name_to_append
   *        Thing to append after appending separator.
   * @return `*this`.
   */
  template<typename Source>
  Shared_name& operator/=(const Source& raw_name_to_append);

  /**
   * Similar to the overload that takes `const Source&`, but takes NUL-terminated string instead.  See that doc header.
   *
   * @param raw_name_to_append
   *        Thing to append after appending separator.
   * @return `*this`.
   */
  Shared_name& operator/=(const char* raw_name_to_append);

  /**
   * Appends the given other Shared_name.
   * Functionally equivalent to `return *this += src_to_append.str());`.
   * If there's any reason to suspect the resulting name might be too long, execute sanitize() afterwards and ensure
   * that returns `true`.
   *
   * It is stylistically (and possibly for performance) better to use `/=` rather than manually appending
   * a separator and then `src_to_append` (with this `+=`).
   *
   * See Rationale(s) for operator/=(), as they apply here.
   *
   * @param src_to_append
   *        Thing to append.
   * @return `*this`.
   */
  Shared_name& operator+=(const Shared_name& src_to_append);

  /**
   * Simply appends `raw_name_to_append` to the current value of str().  Specifically the internal
   * `std::string` is modified as: `s += raw_name_to_append`.  As a result, whatever most-performant available
   * (single-arg by definition) `operator+=` that `basic_string` makes available is forwarded-to.
   * (E.g., C++17 has a `String_view_like` appender which is overload-resolved-to only when it most makes sense.)
   *
   * If there's any reason to suspect the resulting name might be too long, execute sanitize() afterwards and ensure
   * that returns `true`.
   *
   * It is stylistically (and possibly for performance) better to use `/=` rather than manually appending
   * a separator and then `src_to_append` (with this `+=`).
   *
   * See Rationale(s) for operator/=(), as they apply here.
   *
   * @tparam Source
   *         See above.
   * @param raw_name_to_append
   *        Thing to append.
   * @return `*this`.
   */
  template<typename Source>
  Shared_name& operator+=(const Source& raw_name_to_append);

  /**
   * Similar to the overload that takes `const Source&`, but takes NUL-terminated string instead.  See that doc header.
   *
   * @param raw_name_to_append
   *        Thing to append.
   * @return `*this`.
   */
  Shared_name& operator+=(const char* raw_name_to_append);

private:
  // Friends.

  // Friend for access to Shared_name.
  friend void swap(Shared_name& val1, Shared_name& val2);

  // Data.

  /**
   * The name or name fragment; see str().
   * @todo Consider providing a ref-to-mutable accessor to Shared_name::m_raw_name (or just make `public`).
   * There are pros and cons; the basic pro being that Shared_name is meant to be a very thin wrapper around
   * `std::string`, so it might make sense to allow for in-place modification without supplying some kind of reduced
   * subset of `string` API.  Suggest doing this to-do if a practical desire comes about.
   */
  std::string m_raw_name;
}; // class Shared_name

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Input_it>
Shared_name::Shared_name(Input_it begin, Input_it end) :
  m_raw_name(begin, end) // Copy it.
{
  // Nothing.
}

template<typename Source>
Shared_name Shared_name::ct(const Source& src) // Static.
{
  Shared_name result;

  /* As advertised: invoke whatever `string` ctor best applies.  Well, actually, to avoid needing to move-assign
   * (which involves a few scalar copies), use .assign() instead; but its complement of overloads equals that
   * of the relevant complement of ctor overloads. */
  result.m_raw_name.assign(src);

  return result;
}

template<typename Source>
Shared_name& Shared_name::operator+=(const Source& raw_name_to_append)
{
  // Existence/impl rationale: This is faster than if they had to: `+= Shared_name::ct(raw_name_to_append)`.

  m_raw_name += raw_name_to_append;
  return *this;
}

template<typename Source>
Shared_name& Shared_name::operator/=(const Source& raw_name_to_append)
{
  // Existence/impl rationale: This is faster than if they had to: `/= Shared_name::ct(raw_name_to_append)`.

  /* m_raw_name.reserve(m_raw_name.size() + 1 + raw_name_to_append.size()); // Tiny optimization, as 2 appends follow.
   * Oops; can't do that; `Source` could be, e.g., `char`.  @todo Reconsider. */

  m_raw_name += S_SEPARATOR;
  return operator+=(raw_name_to_append);
}

template<typename Source>
Shared_name operator+(const Shared_name& src1, const Source& raw_src2)
{
  // Existence/impl rationale: This is faster than if they had to: `src1 + Shared_name::ct(raw_src2)`.

  return Shared_name(src1) += raw_src2;
}

template<typename Source>
Shared_name operator+(const Source& raw_src1, const Shared_name& src2)
{
  // Existence rationale: For symmetry with overload: (src1, raw_src2).

  return Shared_name::ct(raw_src1) += src2;
}

template<typename Source>
Shared_name operator/(const Shared_name& src1, const Source& raw_src2)
{
  // Existence/impl rationale: This is faster than if they had to: `src1 / Shared_name::ct(raw_src2)`.

  return Shared_name(src1) /= raw_src2;
}

template<typename Source>
Shared_name operator/(const Source& raw_src1, const Shared_name& src2)
{
  // Existence rationale: For symmetry with overload: (src1, raw_src2).

  return Shared_name::ct(raw_src1) /= src2;
}

template<typename Persistent_object, typename Filter_func>
unsigned int remove_each_persistent_if(flow::log::Logger* logger_ptr, const Filter_func& filter_func)
{
  // This is pretty much completely described by the doc header; so keeping comments light.

  Error_code err_code;
  unsigned int count = 0;

  Persistent_object::for_each_persistent([&](const Shared_name& name)
  {
    if (filter_func(name))
    {
      Persistent_object::remove_persistent(logger_ptr, name, &err_code);
      if (!err_code)
      {
        ++count;
      }
      // else { Probably permission error; but regardless just move past it as advertised. }
    }
  });

  return count;
} // remove_each_persistent_if()

template<typename Persistent_object>
unsigned int remove_each_persistent_with_name_prefix(flow::log::Logger* logger_ptr,
                                                     const Shared_name& name_prefix_or_empty)
{
  using util::String_view;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT);
  FLOW_LOG_TRACE("Will attempt to remove-persistent objects (type [" << typeid(Persistent_object).name() << "]) with "
                 "prefix [" << name_prefix_or_empty.str() << "] (<-- may be blank; then all are removed).");

  const String_view name_prefix_view(name_prefix_or_empty.str());
  const auto count = remove_each_persistent_if<Persistent_object>(get_logger(), [&](const Shared_name& name)
  {
    return name_prefix_view.empty() || String_view(name.str()).starts_with(name_prefix_view);
  });

  if (count != 0)
  {
    FLOW_LOG_INFO("Removed-persistent successfully [" << count << "] objects "
                  "(type [" << typeid(Persistent_object).name() << "]) with "
                  "prefix [" << name_prefix_or_empty << "] (<-- may be blank; then all were potentially removed).  "
                  "Note this counts only successful removals (not ones that failed due to permissions, say).  "
                  "Details potentially logged above (including any errors which are otherwise ignored).");
  }

  return count;
} // remove_each_persistent_with_name_prefix()

} // namespace ipc::util
