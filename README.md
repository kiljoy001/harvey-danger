<img src="https://harvey-os.org/images/harvey-os-logo.svg" alt="Harvey" width="200px"/>

# Harvey: A distributed operating system

Welcome to Harvey, we are delighted that you are interested in the
project.

## What is harvey?

Harvey is a distributed operating system. It’s most directly descended from [Plan 9 from Bell Labs](https://en.wikipedia.org/wiki/Plan_9_from_Bell_Labs). This heritage spans from its distributed application architecture all the way down to much of its code. However, Harvey aims to be a more practical, general-purpose operating system, so it also uses ideas and code from other systems.

Starting July 2020, we will be replacing Harvey's user mode programs with programs written in Go. Many of these will come from [u-root](https://u-root.org).

## Prism Microkernel & Objective-9C Userland (Experimental)

The `micro-kvm` branch provides a stripped AMD64 microkernel running directly under a minimal KVM hypervisor (`util/9vm/9vm.c`) with an Objective-9C (`o9c`) actor supervisor as PID 1 (`boot/init.o9`).

### Architecture & Design
- **Host-Passthrough Namespace**: The diskless host-passthrough namespace mounts the host tree through `#~/host`, with VirtIO 9P (`#9/0`) as a fallback, and overlays synthetic `/dev`, `/proc`, `/srv`, and `/env` bindings. See [9vm host transports](util/9vm/README.md).
- **Concurrency Model**: Objective-9C actors use lightweight processes sharing memory via `rfork(RFMEM)` and CSP channels.
- **Toolchain**: Built with modern LLVM (`clang`, `ld.lld`, `llvm-ar`) without legacy Plan 9 compilers.
- **External Verification**: Automated test runner lives in `util/9vm/test_boot.py`.

The [architecture map](util/9vm/ARCHITECTURE.md) shows the Linux microVM,
guest IPC, Objective-9C, Lola prototype, and bare-metal portability target.
For the interactive Objective-9C Lola VM preview and its current limits, see
the [display guide](util/9vm/DISPLAY.md).

### Getting started

Start by reading the [Wiki](https://github.com/Harvey-OS/harvey/wiki).
There's a [Getting Started](https://github.com/Harvey-OS/harvey/wiki/Getting-Started)
guide there to learn how to get Harvey up and running and many other things.

### Contributing

You enjoyed playing with Harvey and you want to help out? Great!

Take a look at our [Contribution Guide](https://github.com/Harvey-OS/harvey/blob/main/CONTRIBUTING.md) it covers a wide variety of topics including the [Harvey coding style](https://github.com/Harvey-OS/harvey/blob/main/CONTRIBUTING.md#coding-conventions), [Code of Conduct](https://github.com/Harvey-OS/harvey/wiki/Code-of-Conduct), where to get help and so on.

### Docs

* [Harvey's wiki](https://github.com/Harvey-OS/harvey/wiki)
* [Apex's wiki](https://github.com/Harvey-OS/apex/wiki).

### Status

* ![harvey](https://github.com/Harvey-OS/harvey/workflows/harvey/badge.svg)
* [![Coverity Badge](https://scan.coverity.com/projects/5328/badge.svg)](https://scan.coverity.com/projects/5328)

### Elsewhere

* [Mailing list](https://groups.google.com/forum/#!forum/harvey)
* [Slack](https://harvey-slack.herokuapp.com/)
* [Harvey's Blog](https://harvey-os.org/#news)
* [Twitter](https://twitter.com/harvey_os)

## Donate to Harvey!

Harvey is a proud member of [SFC](https://sfconservancy.org/projects/current/) and gladly accepts donations.

[![Donate](https://img.shields.io/badge/Donate-PayPal-brightgreen.svg)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=7Q45ZGJBQZZVN)

## Licenses

* [![License GPL](https://img.shields.io/badge/license-GPL-blue.svg)](https://github.com/Harvey-OS/harvey/blob/main/LICENSE)
