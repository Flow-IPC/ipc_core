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

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_core` lacks its own generated documentation.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.  We are included in a subdirectory off the Flow-IPC root.
- Via Git:
  - `git clone --recurse-submodules git@github.com:Flow-IPC/ipc.git`; or
  - `git clone git@github.com:Flow-IPC/ipc_core.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
