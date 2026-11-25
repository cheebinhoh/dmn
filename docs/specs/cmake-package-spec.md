<!-- Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved. -->

# Installable CMake Package Specification

Status: implementation specification for work item 1 in
[`feature-opportunities.md`](../feature-opportunities.md). This document
defines the package contract and a test-first implementation/validation
sequence. It describes intended behavior, not behavior already present.

## 1. Objective

Install Dmn's reusable C++ libraries as a relocatable CMake config package so
an independent downstream project can use:

```cmake
find_package(Dmn CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Dmn::dmn)
```

The package must accurately represent the targets built by a given Dmn
configuration, propagate their required include paths, compile definitions,
and link dependencies, and avoid references to the original source or build
tree.

The first release exports the existing target composition. It does not split
the current `dmn` target into new libraries. In particular,
`ENABLE_DMN_DMESGNET=ON` means `dmn` includes the DMesg/DMesgNet and Kafka
implementation and has Protobuf and librdkafka dependencies. It must not be
presented as a dependency-free or DMesgNet-free “Core” target.

## 2. Scope and non-goals

### In scope

- Install a `DmnConfig.cmake`, version file, and namespaced imported targets.
- Export only targets available in the configured build.
- Install public headers, including generated protobuf headers required by
  installed DMesg headers.
- Provide relocatable build/install include interfaces.
- Resolve external dependencies needed by exported targets.
- Add an independent consumer smoke test that configures, compiles, and links
  against the installed package.
- Document configuration-dependent targets and downstream usage.

### Out of scope

- Splitting `dmn` into core, DMesgNet, and Kafka libraries.
- Exporting applications, test targets, or internal implementation headers.
- Installing third-party dependencies or bundling/fetching dependencies from
  the consumer's package configuration.
- Adding a pkg-config package in this change.
- A component-selection API such as `find_package(Dmn COMPONENTS BlueZ)`.
- Changing public header names or introducing a new header layout.
- Windows support. Linux-specific optional libraries remain optional; the
  ordinary package remains for the project's supported POSIX platforms.

## 3. Package and target contract

The package name and config files are `Dmn` / `DmnConfig.cmake`. Exported
targets use the `Dmn::` namespace and retain their existing logical names:

| Imported target | Availability | Required exported dependencies |
| --- | --- | --- |
| `Dmn::dmn` | Always | `ICU::uc`; also `protobuf::libprotobuf` and `PkgConfig::RDKAFKA` when `ENABLE_DMN_DMESGNET=ON`; `PkgConfig::LIBFIU` when `ENABLE_FAULT_INJECTION=ON`; `atomic` where the existing target links it |
| `Dmn::dmn-gatt` | Always | None |
| `Dmn::dmn-ancs` | `ENABLE_ANCS=ON` | `Dmn::dmn`, `Dmn::dmn-gatt` |
| `Dmn::dmn-dbus` | `ENABLE_DBUS=ON` | `Dmn::dmn`; D-Bus pkg-config dependency for the enabled API and static link interface |
| `Dmn::dmn-bluez` | `ENABLE_BLUEZ=ON` | `Dmn::dmn-dbus`, `Dmn::dmn-gatt` |
| `Dmn::dmn-ancs-bluez-monitor` | `ENABLE_ANCS=ON` and `ENABLE_BLUEZ=ON` | `Dmn::dmn-ancs`, `Dmn::dmn-bluez` |

Target presence, not a component argument, tells consumers which optional
libraries were installed. A consumer may test, for example,
`if(TARGET Dmn::dmn-bluez)`.

The install layout follows GNUInstallDirs:

- Public headers: `${CMAKE_INSTALL_INCLUDEDIR}/dmn`
- Libraries: `${CMAKE_INSTALL_LIBDIR}/dmn`
- Package config and exported targets:
  `${CMAKE_INSTALL_LIBDIR}/cmake/Dmn`

The installed include directory is the include root, preserving current source
usage such as `#include <dmn-proc.hpp>`. Generated protobuf headers are
installed under `${CMAKE_INSTALL_INCLUDEDIR}/dmn/proto` so public includes
such as `#include "proto/dmn-dmesg.pb.h"` remain valid.

## 4. Dependency and relocatability rules

1. The installed config uses `CMakeFindDependencyMacro` and
   `find_dependency()` for required CMake packages.
2. The package always resolves ICU's `uc` component.
3. If DMesgNet is included, it resolves Protobuf and the librdkafka pkg-config
   imported target. If D-Bus is included, it resolves the D-Bus pkg-config
   imported target needed for static-link consumers. If fault injection is
   included, it resolves the libfiu pkg-config imported target.
4. An installed package does not call `FetchContent`, use `_deps` paths, or
   expect source/build directories from the Dmn build to remain available.
   Consumers must install the required development dependencies themselves.
5. Exported target properties use `$<BUILD_INTERFACE:...>` for source/build
   paths and `$<INSTALL_INTERFACE:...>` for installed headers. No absolute
   Dmn source/build path may appear in installed target files.
6. Targets propagate the C++23 compile feature required by the public headers;
   consumers must not need to copy Dmn's global CMake language-standard
   settings.
7. Generated protobuf headers are installed only when DMesgNet is enabled.
   Internal D-Bus and discovery implementation headers and feature-disabled
   public headers remain excluded.
8. Static libraries and public headers are installed as Development
   components; runtime components retain their existing install behavior.

## 5. Public target composition to preserve

The top-level options control the exported set:

- `dmn` always exists. `ENABLE_DMN_DMESGNET` defaults ON and adds DMesg,
  DMesgNet, Kafka, generated protobuf interfaces, and their public
  dependencies. `ENABLE_FAULT_INJECTION` adds its public libfiu dependency.
  With the corresponding option OFF, those sources, headers, and dependencies
  are absent.
- `dmn-gatt` is an interface target. It is exported and its public contract
  header is installed so it can be linked independently or as a dependency of
  ANCS/BlueZ targets.
- `dmn-ancs` is conditional on `ENABLE_ANCS`.
- `dmn-dbus` is conditional on `ENABLE_DBUS`; BlueZ enables D-Bus.
- `dmn-bluez` is conditional on `ENABLE_BLUEZ`.
- `dmn-ancs-bluez-monitor` requires both ANCS and BlueZ.

The package must not add app dependencies such as GTK, libadwaita, GLib UI
bindings, or Rust. `dmn`'s current public link dependencies remain public;
dependency visibility is not weakened merely to simplify export.

## 6. Test-driven implementation sequence

Each implementation step starts with a test or a failing downstream probe,
then makes the smallest change that satisfies it. Keep the spec synchronized
when validation exposes a necessary contract correction.

### Step 1 — Prove current package consumption fails

Create a minimal independent consumer fixture with `find_package(Dmn CONFIG
REQUIRED)`, link `Dmn::dmn`, include a public Dmn header, and build an
executable. First configure it with `Dmn_DIR` pointed at an empty/nonexistent
package location and record the expected failure: no `DmnConfig.cmake` /
imported target is available.

The fixture must live outside the Dmn source tree so accidental source-tree
include directories cannot make it pass.

### Step 2 — Add namespaced target export tests

Add a CMake install-and-consume test that stages the current build into a
temporary prefix, configures the fixture against only that prefix, and builds
it. Initially this test should fail because installed targets are not exported.

Export `dmn` and `dmn-gatt`, then each enabled optional library, through one
`DmnTargets` export set. Add build-tree aliases with the same `Dmn::` names
where useful, and install the export under `lib/cmake/Dmn`.

### Step 3 — Make target include interfaces relocatable

Extend the consumer fixture to include the public core and GATT headers.
Change target include directories to build/install generator expressions and
install `dmn-gatt` with its interface include path. Remove public source and
build paths from installed target interfaces. Keep private implementation
include paths private.

The installed consumer must build without using source/build paths. Existing
in-tree project targets continue to use their build-interface include paths.

### Step 4 — Generate package config and resolve dependencies

Add `DmnConfig.cmake.in` and use `CMakePackageConfigHelpers` to generate the
config and version files. Encode the enabled feature/dependency set when
configuring the package. Add downstream checks that link enabled targets and
observe required definitions/includes.

The config must call `find_dependency()` only for dependencies required by
the exported targets. A test with a deliberately missing required dependency
must fail at configure time with a clear dependency error rather than fail
later at link time.

### Step 5 — Install generated and feature-conditional headers

With DMesgNet enabled, extend the consumer to include a public DMesg header
that includes a generated protobuf header. Confirm the staged prefix contains
the corresponding `proto/*.pb.h` files and the consumer compiles without any
build-tree include path.

Repeat with DMesgNet disabled. The config must not require Protobuf or
librdkafka, generated headers must not be installed, and DMesgNet-only
headers/targets must not be exposed.

### Step 6 — Verify optional library dependencies and target presence

Configure an optional build with D-Bus, BlueZ, and ANCS enabled where the
platform dependencies are available. The external consumer must link each
installed optional target and verify its transitive Dmn target dependencies.
In a default/minimal build, those imported targets must be absent and their
dependencies must not be required by `find_package(Dmn)`.

### Step 7 — Prove installation relocation

After installing, move the staged prefix to a different path and configure
and build the consumer using only the moved prefix. Inspect installed
`DmnTargets*.cmake` and `DmnConfig.cmake` for the original source and build
paths. Relocation succeeds only if the consumer still configures and builds
without either original path.

## 7. Automated validation design

The package smoke test is a CTest test when `DMN_BUILD_TESTS=ON`, or a
standalone documented validation command if keeping package checks out of the
normal unit-test suite is necessary. It must:

1. Install the already-built project into a unique temporary staging prefix.
2. Configure and build an independent consumer fixture using that prefix.
3. Link a symbol from the library, not merely configure an empty target.
4. Use only installed headers and imported package targets.
5. Verify target presence matches the build options.
6. Verify disabled features do not impose their dependencies.
7. Relocate the staged prefix and repeat downstream configure/build.
8. Remove only its own uniquely named temporary build and staging directories.

The test must be safe under parallel CTest runs and must not overwrite a
developer-selected install prefix. The test should skip only a configuration
that cannot be exercised on the current platform; dependency absence for an
enabled feature is a configuration error, not a passing skip.

## 8. Acceptance criteria

- A clean external CMake project can use `find_package(Dmn CONFIG REQUIRED)`
  after installation and link `Dmn::dmn`.
- Imported target names and presence match the table in Section 3.
- The default DMesgNet-enabled package correctly propagates Protobuf and
  librdkafka requirements; the DMesgNet-disabled package does not require
  either.
- Public core, GATT, optional, and generated protobuf headers are available
  only under the documented feature conditions.
- Static-link transitive dependencies and public compile definitions propagate
  correctly, and a consumer inherits the required C++23 language feature.
- The installed package contains no absolute Dmn source/build paths and
  remains usable after prefix relocation.
- Core-only downstream consumption does not require D-Bus, BlueZ, Kafka,
  GTK, libadwaita, or Rust.
- Existing in-tree builds/tests and install component behavior remain intact.

## 9. Required validation matrix

Run the smallest useful set after each change, then complete this matrix before
considering the package contract done:

| Configuration | Required result |
| --- | --- |
| `ENABLE_DMN_DMESGNET=OFF`, optional libraries OFF | Install and consume `Dmn::dmn` and `Dmn::dmn-gatt`; no protobuf/librdkafka/D-Bus/BlueZ/GTK/Rust requirement |
| Default `ENABLE_DMN_DMESGNET=ON` | Install and consume core plus DMesg/Kafka headers; generated protobuf headers and Protobuf/librdkafka transitive links work |
| D-Bus + BlueZ + ANCS ON (Linux with dependencies) | All enabled library targets export and link; static D-Bus dependency propagates |
| Relocated install prefix | Consumer configure and build succeed with no source/build paths |
| Existing unit tests | No regression in configured Dmn tests |

At every stage, run CMake configure/build for the touched configuration,
CTest's package consumer test where available, and `git diff --check`.
Documentation changes need no separate formatter unless the repository adds a
Markdown lint requirement.

## 10. Risks and explicit decisions

- **Fetched build dependencies:** a Dmn build may fetch Protobuf or
  librdkafka. Installed consumers still need compatible system development
  packages; the package does not install or vendor these dependencies.
- **Pkg-config target reconstruction:** the consumer config must recreate
  `PkgConfig::RDKAFKA` and the D-Bus imported target only when exported targets
  need them.
- **Public protobuf headers:** installed DMesg headers directly include
  generated headers, so installing only the library binary is insufficient.
- **GATT target contract:** `dmn-gatt` exists in every build but its header is
  currently excluded from header installation unless ANCS or BlueZ is
  enabled. This spec makes it an always-exported target with an installed
  interface header, matching its unconditional target creation.
- **No artificial core target:** target splitting may be valuable later, but
  this package change exports the actual `dmn` composition rather than
  silently changing which code or dependencies it represents.
