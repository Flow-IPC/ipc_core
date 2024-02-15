# Flow-IPC Sub-project -- Core -- Basics, unstructured transport

This project is a sub-project of the larger Flow-IPC meta-project.  Please see
a similar `README.md` for Flow-IPC, first.  You can most likely find it either in the parent
directory to this one; or else in a sibling GitHub repository named `ipc.git`.

A more grounded description of the various sub-projects of Flow-IPC, including this one, can be found
in `./src/ipc/common.hpp` off the directory containing the present README.  Look for
`Distributed sub-components (libraries)` in a large C++ comment.

Took a look at those?  Still interested in `ipc_core` as an independent entity?  Then read on.  Before you
do though: it is, typically, both easier and more functional to simply treat Flow-IPC as a whole.
To do so it is sufficient to never have to delve into topics discussed in this README.  In particular
the Flow-IPC generated documentation guided Manual + Reference are monolithic and cover all the
sub-projects together, including this one.

Still interested?  Then read on.

`ipc_core` depends on `flow` and no other `ipc_*` components.  It provides
`ipc::util` and `ipc::transport` (excluding `ipc::transport::struc`).  Therefore, it's got the basics
(such as `util::Native_handle` and `util::Shared_name`) and unstructured data transport: Unix domain sockets,
POSIX MQs, and a few other items of this nature.  It may well be useful to many application in its own
right: certainly decent native and Boost-y APIs exist for access to these low-level resources, but generally
not in a particularly convenient or consistent way; for example message boundaries are not consistently
implemented, and names/addresses are a mess.  `ipc_core` is nicer.

Arguably more importantly, however, `ipc_core` is the dependency of all other `ipc_*` components.  In
particular structured-data transport features of `ipc::transport::struc` are built on a foundation of
of `ipc_core`-contained low-level unstructured (blob and native-handle) transport features.

## Installation

An exported `ipc_core` consists of C++ header files installed under "ipc/..." in the
include-root; and a library such as `libipc_core.a`.
Certain items are also exported for people who use CMake to build their own
projects; we make it particularly easy to use `ipc_core` proper in that case
(`find_package(IpcCore)`).  However documentation is generated monolithically for all of Flow-IPC;
not for `ipc_core` separately.

The basic prerequisites for *building* the above:

  - Linux;
  - a C++ compiler with C++ 17 support;
  - Boost headers (plus certain libraries) install;
  - {fmt} install;
  - dependency headers and library (from within this overall project) install(s); in this case those of:
    `flow`;
  - CMake.

The basic prerequisites for *using* the above:

  - Linux, C++ compiler, Boost, above-listed dependency lib(s) (but CMake is not required); plus:
  - your source code `#include`ing any exported `ipc/` headers must be itself built in C++ 17 mode;
  - any executable using the `ipc_*` libraries must be linked with certain Boost and ubiquitous
    system libraries.

We intentionally omit version numbers and even specific compiler types in the above description; the CMake run
should help you with that.

To build `ipc_core`:

  1. Ensure a Boost install is available.  If you don't have one, please install the latest version at
     [boost.org](https://boost.org).  If you do have one, try using that one (our build will complain if insufficient).
     (From this point on, that's the recommended tactic to use when deciding on the version number for any given
     prerequisite.  E.g., same deal with CMake in step 2.)
  2. Ensure a {fmt} install is available (available at [{fmt} web site](https://fmt.dev/) if needed).
  3. Ensure a CMake install is available (available at [CMake web site](https://cmake.org/download/) if needed).
  4. Use CMake `cmake` (command-line tool) or `ccmake` (interactive text-UI tool) to configure and generate
     a build system (namely a GNU-make `Makefile` and friends).  Details on using CMake are outside our scope here;
     but the basics are as follows.  CMake is very flexible and powerful; we've tried not to mess with that principle
     in our build script(s).
     1. Choose a tool.  `ccmake` will allow you to interactively configure aspects of the build system, including
        showing docs for various knobs our CMakeLists.txt (and friends) have made availale.  `cmake` will do so without
        asking questions; you'll need to provide all required inputs on the command line.  Let's assume `cmake` below,
        but you can use whichever makes sense for you.
     2. Choose a working *build directory*, somewhere outside the present `ipc` distribution.  Let's call this
        `$BUILD`: please `mkdir -p $BUILD`.  Below we'll refer to the directory containing the present `README.md` file
        as `$SRC`.
     3. Configure/generate the build system.  The basic command line:
        `cd $BUILD && cmake -DCMAKE_INSTALL_PREFIX=... -DCMAKE_BUILD_TYPE=... $SRC`,
        where `$CMAKE_INSTALL_PREFIX/{include|lib|...}` will be the export location for headers/library/goodies;
        `CMAKE_BUILD_TYPE={Release|RelWithDebInfo|RelMinSize|Debug|}` specifies build config.
        More options are available -- `CMAKE_*` for CMake ones; `CFG_*` for Flow-IPC ones -- and can be
        viewed with `ccmake` or by glancing at `$BUILD/CMakeCache.txt` after running `cmake` or `ccmake`.
        - Regarding `CMAKE_BUILD_TYPE`, you can use the empty "" type to supply
          your own compile/link flags, such as if your organization or product has a standard set suitable for your
          situation.  With the non-blank types we'll take CMake's sensible defaults -- which you can override
          as well.  (See CMake docs; and/or a shortcut is checking out `$BUILD/CMakeCache.txt`.)
        - This is the likeliest stage at which CMake would detect lacking dependencies.  See CMake docs for
          how to tweak its robust dependency-searching behavior; but generally if it's not in a global system
          location, or not in the `CMAKE_INSTALL_PREFIX` (export) location itself, then you can provide more
          search locations by adding a semicolon-separated list thereof via `-DCMAKE_PREFIX_PATH=...`.
        - Alternatively most things' locations can be individually specified via `..._DIR` settings.
     4. Build using the build system generated in the preceding step:  In `$BUILD` run `make`.  
     5. Install (export):  In `$BUILD` run `make install`.  

To use `ipc_core`:

  - `#include` the relevant exported header(s).
  - Link the exported library (such as `libipc_core.a`) and the required other libraries to
    your executable(s).
    - If using CMake to build such executable(s):
      1. Simply use `find_package(IpcCore)` to find it.
      2. Then use `target_link_libraries(... IpcCore::ipc_core)` on your target to ensure all necessary libraries are
         linked.
         (This will include the libraries themselves and the dependency libraries it needs to avoid undefined-reference
         errors when linking.  Details on such things can be found in CMake documentation; and/or you may use
         our CMake script(s) for inspiration; after all we do build all the libraries and a `*_link_test.exec`
         executable.)
    - Otherwise specify it manually based on your build system of choice (if any).  To wit, in order:
      - Link against `libipc_core.a` and `libflow.a`.
      - Link against Boost libraries mentioned in a `flow/.../CMakeLists.txt` line (search `flow` dependency for it):
        `set(BOOST_LIBS ...)`.
      - Link against the {fmt} library, `libfmt`.
      - Link against the system pthreads library and `librt`.
  - Read the documentation to learn how to use Flow-IPC's (and/or Flow's) various features.
    (See Documentation below.)

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_core` lacks its own generated documentation.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Contributing

See Flow-IPC meta-project's `README.md` Contributing section.
