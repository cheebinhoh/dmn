# Dmn Feature Development Plan

**Status:** Research-backed draft; proposed work, not an approved commitment.

## Purpose and constraints

This is a repository-wide product and architecture survey, not a line-by-line
code audit. It draws on the tracked source inventory, public headers, library
and application sources, build configuration, tests, README, and existing
specifications. The online survey below is a technical ecosystem survey, not
quantitative market research or proof of demand. Each opportunity should be
validated with prospective users before committing to a large implementation.

Ideas are selected for Linux/POSIX use, practical software-only development,
and a credible path for one senior engineer. No proposal requires custom
hardware; the Bluetooth ideas use standard consumer devices when available
and can be developed and tested with the existing demo backend and private-bus
fakes. Windows support is not a goal; keep platform-specific work optional and
Linux-scoped, and preserve the existing non-GUI core build.

The recommendations deliberately avoid treating unfinished distributed
coordination as safe, or adding features that conflict with an existing
specification without calling that out.

## Repository snapshot

Dmn already has a broad foundation:

- **Core C++23 library:** process/runtime helpers, async execution and
  scheduling, timers, queues, I/O pipes, UDP sockets, state machines,
  publish/subscribe, and an interval B-tree.
- **Messaging:** DMesg topic messaging and DMesgNet over injected I/O
  endpoints, plus optional Kafka and local D-Bus transports.
- **Linux D-Bus and Bluetooth:** a generic D-Bus client; BlueZ adapter/device
  inventory and discovery; a backend-neutral GATT API; BlueZ GATT; and ANCS.
- **Applications:** Kafka examples and a DMesg daemon stub; BlueZ discovery
  and ANCS CLIs; a shared Bluetooth engine; GTK and COSMIC Bluetooth apps.
  The Bluetooth apps support a hardware-free demo mode.
- **Reuse gap:** libraries and headers have CMake install rules, but the
  project does not install an exported CMake package that a downstream
  `find_package()` call can consume.
- **Development and validation:** extensive focused tests, private-bus BlueZ
  fakes, fault-injection tests, and optional feature build gates.

The current Bluetooth apps focus on live ANCS notifications. Their event feed
is bounded and process-local; the repository intentionally does not promise a
complete phone notification history. The DMesgNet specifications also
explicitly distinguish current best-effort behavior from reliable delivery or
consensus.

## Recommendation at a glance

Treat Dmn as a reusable C++ library with optional Linux applications, not one
ever-growing application:

1. **First improve reuse:** export and test the libraries as installable CMake
   targets. This is a directly evidenced integration gap, though external
   consumer demand still needs validation.
2. **For network users:** make DMesgNet failure behavior observable and
   deterministic before adding discovery or another transport. This improves
   the existing messaging story without claiming consensus or guaranteed
   delivery.
3. **For desktop users:** test whether there is demand beyond the existing
   Bluetooth/ANCS workflow. Device battery visibility is a small candidate;
   a background monitor service is a larger, already-designed option and
   should not be built speculatively.

MQTT is a follow-on integration, not the first project: validate that Dmn users
need a broker lighter than Kafka before adding a dependency and another
transport lifecycle.

## Online ecosystem survey

The sources below establish platform capabilities and integration boundaries.
They do **not** establish how many people experience each pain point.

| Area | Online finding | Product implication |
| --- | --- | --- |
| Bluetooth status | BlueZ's [`Battery1` API](https://bluez.readthedocs.io/en/latest/battery-api/) defines a read-only `Percentage` byte on a Battery object. | Battery visibility is a small integration candidate, not a universal device capability; Dmn must handle missing values and measure support on ordinary devices. |
| Desktop notifications | The [XDG Notification portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html) can add/update and withdraw app notifications; its API does not provide notification-center history. | Use desktop notification APIs for alerts. Do not market Dmn as a history reader or duplicate the desktop's notification store. |
| Service lifecycle | [systemd service units](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html) supervise processes, and [timer units](https://www.freedesktop.org/software/systemd/man/latest/systemd.timer.html) activate services. | A Dmn background service should integrate with the user service manager instead of becoming a general-purpose process supervisor or cron replacement. |
| Local discovery | [RFC 6763](https://www.rfc-editor.org/rfc/rfc6763) specifies DNS-SD browsing and resolution. [Avahi's API guidance](https://avahi.org/doxygen/html/) recommends `avahi-client` for C/C++ and discourages embedding another mDNS stack in ordinary desktop applications. | An optional Avahi wrapper is a natural fit, but discovery must remain separate from transport, authentication, and trust. |
| Broker transport | The [OASIS MQTT 5.0 specification](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html) defines publish/subscribe and three protocol delivery QoS levels. | MQTT may complement Kafka for some deployments, but MQTT QoS is not equivalent to Dmn-level state convergence or consensus. |
| Desktop process identity | GLib [`GApplication`](https://docs.gtk.org/gio/class.Application.html) supports session-scoped uniqueness and exported D-Bus actions on Linux. | Prefer established desktop/session APIs for single-instance activation; a separate service is warranted only for shared state or background ownership. |
| Existing Bluetooth UX | [GNOME help](https://help.gnome.org/gnome-help/bluetooth.html) documents common Bluetooth management. [KDE Connect](https://kdeconnect.kde.org/) advertises phone notifications, file/link sharing, remote controls, commands, and phone battery display. | Do not build another general Bluetooth settings panel or a broad KDE Connect clone. Dmn's distinct documented niche is its BlueZ-based iPhone ANCS path without a custom iPhone app; any expansion should address a validated gap. |

The value ranking below is an engineering hypothesis based on repository fit
and implementation leverage, not measured market demand. The sources establish
available interfaces and existing alternatives, not customer counts or
competitive superiority.

### Research interpretation and limits

- **Standards are evidence of an available integration surface, not user
  demand.** Before MQTT, service discovery, or a resident Bluetooth service,
  ask target users what they configure or run today and what fails for them.
- **Do not recreate existing platform infrastructure.** Dmn should supply a
  typed C++ integration boundary and useful domain behavior, while BlueZ,
  Avahi, systemd, D-Bus, and desktop notification services keep their existing
  responsibilities.
- **Do not compete on breadth without evidence.** GNOME already provides
  ordinary Bluetooth management, and KDE Connect already advertises a broad
  phone/PC integration workflow. Dmn should stay narrow: reusable Linux C++
  building blocks, robust LAN messaging, and its distinctive ANCS integration.
- **Bluetooth support remains conditional.** The BlueZ battery property is
  optional and device-reported. ANCS needs an ANCS-capable phone, but the
  repository's development and CI do not need a physical phone.
- **Do not add persistent ANCS payload storage by accident.** Current app
  specifications intentionally keep event content in bounded process memory.
  Any archive/export proposal needs explicit user demand, retention controls,
  and a privacy review; it is not part of this plan.

## Ranked opportunities (hypotheses)

| Priority | Opportunity | Value hypothesis | Effort / risk |
| --- | --- | --- | --- |
| 1 | Installable CMake package for library consumers | Removes source-tree/manual-linking friction for downstream C++ users | Small-to-medium; dependency and generated-header packaging needs care |
| 2 | Harden DMesgNet's current behavior and observability | Makes existing message flows easier to diagnose and failures more explicit | Medium; preserves only best-effort semantics |
| 3 | DNS-SD discovery library and endpoint configuration | Can reduce manual peer lookup; automatic DMesgNet integration needs transport work | Medium-to-large; discovery is not authentication |
| 4 | Bluetooth battery visibility | Adds optional device status to the existing browser | Small-to-medium; demand and device support are uncertain |
| 5 | Optional user-session Bluetooth monitor service | Lets explicit monitoring outlive the application process and supports multiple frontends | Large; a detailed proposal already exists |
| 6 | Optional MQTT transport for DMesgNet | Offers a lighter broker integration alongside Kafka | Medium; new dependency and transport semantics |

The installed-package work is the best first contribution if the target is
library reuse.
DMesgNet hardening is a prerequisite for additional network integrations, but
it does not make the protocol suitable for safety-critical work. DNS-SD and
Bluetooth status are independent product directions; select between them
based on user feedback rather than starting both in parallel.

### 1. Make existing libraries installable as a CMake package

**Problem addressed:** Dmn has reusable static libraries and public headers,
but the CMake export/install-export statements are commented out. Based on
the current CMake files, a downstream developer has no supported
`find_package(Dmn CONFIG)` interface that provides imported targets and their
transitive build requirements.

This is the most direct repository-reuse improvement: a developer should be
able to install Dmn, add one `find_package()` call, and link only the library
component they use. The detailed packaging sequence and acceptance checks are
in Phase 1 of the proposed roadmap below and
[`cmake-package-spec.md`](specs/cmake-package-spec.md).

**Why it fits:** it improves the value of every existing library without
adding runtime behavior or a new platform dependency. It also exposes build
and installation defects in public API headers, optional dependencies, and
generated protobuf headers before Dmn adds more consumers. The implementation
evidence is in [`src/CMakeLists.txt`](../src/CMakeLists.txt), where the target
install rules exist but the export section is commented out. The same file
does not install the build-tree `dmn-gatt` interface target; a usable package
must account for it if it remains a supported public target.

### 2. Harden DMesgNet before building stronger distributed features

**Problem addressed:** Dmn's messaging stack is useful for exchanging topic
state, but its own design review documents missing semantic validation,
unbounded pre-ready outbound buffering, stale membership, best-effort replay,
and timing-driven tests. Those gaps make operational failures difficult to
distinguish from successful synchronization.

**A sensible first release:**

- Validate message envelopes and required fields before mutating peer or topic
  state; report invalid and oversized messages as observable errors.
- Bound pre-ready queues, preserve FIFO by default, and define observable
  overflow behavior. Do not silently coalesce messages unless a topic is
  explicitly defined as replaceable state.
- Expose parse, queue, replay, output-failure, and shutdown status. Report only
  what the layer knows: a successful local `write()` is not proof of remote
  receipt or application.
- Replace sleep-dependent tests with deterministic clocks and controllable
  fake I/O; cover endpoint combinations and recovery behavior.
- In a later, separately scoped step, use monotonic elapsed time for peer
  liveness observations and add an incarnation identifier. Treat timeout as
  local suspicion only, not proof of failure or authority to revoke a peer.

Keep the contract explicitly **best effort** unless a separate protocol proves
stronger guarantees. Better replay, heartbeats, and deterministic elections
are not consensus. In particular, do not extend the partial `Dmn_DLock`
prototype to claim network-safe distributed locks or fencing.

**Why it fits:** it closes gaps in the existing DMesg/DMesgNet path and creates
a safer base for applications that need coordination. It requires no special
hardware or new external service. The repository already has a detailed
code-derived gap analysis and test evidence in
[`dmn-dmesgnet-spec.md`](specs/dmn-dmesgnet-spec.md).

### 3. Add optional DNS-SD peer discovery

**Problem addressed:** Dmn has a datagram/network messaging layer, but
discovering peers and configuring their addresses is a separate operational
step. A small optional discovery library could browse for and advertise
configured Dmn services on a local network.

**A bounded first slice:**

- Add a Linux-optional C++ wrapper around the system Avahi client API.
- Support browse, resolve, register, cancellation, and typed service-state
  errors; keep the API independent of DMesgNet.
- Provide a diagnostic CLI or endpoint-candidate API so users can browse and
  resolve peers without manually copying addresses.
- Test API behavior through an injected discovery interface; use a local
  Avahi daemon only for an opt-in integration test.

Discovery must not be presented as peer authentication, transport encryption,
reliable delivery, or cluster authority. Keep it behind an optional Linux
build target and make manual configuration remain available.

**Important integration constraint:** `Dmn_DMesgNet` currently accepts one
`Dmn_Io<std::string>` input and one output. `Dmn_Socket` is IPv4 UDP with a
fixed destination per instance; there is no peer registry or multi-peer
fan-out contract. DNS-SD can first provide discovery results and endpoint
candidates, but it cannot by itself connect DMesgNet to a set of peers.
Automatic integration requires a separate transport design covering
receive/bind behavior, fan-out, peer identity, and failure semantics. Keep
that work out of the first discovery slice.

**Why it fits:** adding a service-discovery wrapper is already listed in
[`TODOS`](../TODOS), while IETF DNS-SD defines a standard way to enumerate and
resolve named service instances. Avahi's own documentation recommends
`avahi-client` for C/C++ applications and advises desktop software not to
embed a second mDNS stack.

### 4. Add Bluetooth battery and device-health status

**Problem addressed:** the BlueZ inventory already tracks adapters, devices,
connection state, and live changes, but it does not expose battery percentage.
A battery view and optional low-battery alert would make the desktop app useful
for supported headphones, keyboards, mice, and other ordinary peripherals.

**A bounded MVP:**

- Decode BlueZ's optional `org.bluez.Battery1.Percentage` interface into an
  optional battery field on device inventory/events.
- Show battery status only when BlueZ supplies it; never infer or fabricate a
  value when the device does not report one.
- Add an opt-in threshold alert with rate limiting and per-device suppression.
- Extend the existing demo backend and private-bus BlueZ fake to cover present,
  changed, removed, malformed, and unsupported battery state.

Treat battery reporting as opportunistic: the BlueZ API defines a read-only
percentage property, but a device may not expose it. Do not make GATT polling
or a phone-only feature a prerequisite for the MVP.

**Why it fits:** this is a contained extension of the existing BlueZ device
model and both Bluetooth frontends can reuse the engine. BlueZ documents the
`Battery1` interface and its `Percentage` property in its
[Battery API](https://bluez.readthedocs.io/en/latest/battery-api/).
This is a repository-reuse opportunity first; user demand for another
peripheral battery display is unverified. KDE Connect's documented battery
feature refers to the phone's battery and is not evidence that BlueZ exposes
every peripheral's battery.

**Repository touchpoints:** `Dmn_BluezDevice` and the ObjectManager/property
decoders in `include/dmn-bluez-client.hpp` and `src/dmn-bluez-client.cpp`;
`Dmn_BackendInventory` and `Dmn_DeviceEntry` in the shared engine; demo data;
the GTK and COSMIC device rows; and the existing fake-BlueZ inventory tests.
The COSMIC C ABI/JSON path must either add the field compatibly or explicitly
version the protocol. Keep battery state outside ANCS types.

### 5. Implement the optional user-session Bluetooth service when needed

**Problem addressed:** currently the application process owns live monitor
sessions. A user-session service could own sessions on the session bus and let
a GTK app, COSMIC applet, or another frontend observe the same state.

This is **already designed**, not a new proposal: see
[`dmn-bluetooth-session-service-spec.md`](specs/dmn-bluetooth-session-service-spec.md).
Only implement it if users need monitoring to continue after the application
process exits or multiple independent frontends need a common live state.
Merely hiding a window may be handled by an in-process background mode and is
not, by itself, justification for a service. Preserve the spec's key
boundaries: explicit user start, no root/system-bus daemon, no automatic
pairing, bounded in-memory events, no persisted ANCS payloads, and an optional
build/package.

Development can remain hardware-free: begin with the specified private-bus
tests and fake monitor, then treat real-device validation as a separate
qualification step. The design is consistent with D-Bus session-bus
applications; GLib's [`GApplication`](https://docs.gtk.org/gio/class.Application.html)
also documents session-scoped uniqueness and exported actions.

### 6. Consider MQTT as an optional transport, not a reliability shortcut

**Problem addressed:** the current network integration includes Kafka, which
may be more infrastructure than a small local deployment wants. An optional
MQTT adapter could connect Dmn I/O or a carefully specified DMesgNet transport
to a locally managed broker.

**A bounded MVP:**

- First add a standalone `Dmn_Io<std::string>` adapter, gated behind its own
  CMake option; do not make MQTT a core-library dependency.
- Define reconnect, shutdown, message-size, topic naming, credentials, and
  backpressure behavior before DMesgNet integration.
- Do not hide MQTT's asynchronous QoS completion behind `Dmn_Io::write()`,
  whose current contract is `void`. If callers need broker acknowledgements,
  design a separate completion/result API; otherwise document that `write()`
  only submits work locally.
- Add local-broker integration tests that are opt-in, plus deterministic
  adapter tests with an injected client boundary.
- If used with DMesgNet, map broker delivery outcomes separately from DMesg
  topic-counter/conflict state. MQTT QoS is not proof of application-level
  state convergence or consensus.

**Why it fits:** Dmn already abstracts I/O and provides a Kafka integration.
The [OASIS MQTT 5.0 standard](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)
defines a lightweight publish/subscribe transport and three delivery QoS
levels, which could serve deployments preferring a small broker.

**Decision gate:** only proceed if interviews or issues show concrete target
deployments that already run an MQTT broker or reject Kafka's operational
cost. Otherwise, DNS-SD and DMesgNet correctness offer better value per unit
of maintenance.

## Proposed implementation roadmap

This sequence is intentionally incremental and can be paused at every gate.
The estimated size labels are relative: **S** is a focused library/UI change,
**M** crosses multiple modules or adds an optional integration, and **L**
introduces a new long-lived service or protocol.

### Phase 0 — Validate the audience and choose one path

Before feature code:

- Talk to Dmn users or likely adopters about their current workaround:
  manually configured LAN peers, Kafka deployment overhead, battery checking,
  or a Bluetooth monitor that stops with the application process.
- For the Bluetooth path, test representative consumer devices and BlueZ
  versions to learn which expose `Battery1`; the private-bus fake proves
  behavior but not device coverage.
- Record a short user problem statement and a measurable first-use outcome.
  If no user can identify a current workaround or a meaningful consequence,
  defer the feature.

**Exit:** pick one first audience and one opportunity. Do not start DNS-SD,
MQTT, Bluetooth battery work, and a resident service as a single initiative.

### Phase 1 — Make installed libraries easy to consume (S–M)

The repository installs static libraries and headers, but the current CMake
export/install-export calls are commented out. A downstream project therefore
does not get a supported `find_package(Dmn CONFIG)` interface with namespaced
targets and transitive dependencies.

Recommended implementation:

1. Export the targets that are actually built: `dmn`, `dmn-gatt`, and the
   conditional `dmn-ancs`, `dmn-dbus`, `dmn-bluez`, and
   `dmn-ancs-bluez-monitor`. Give them stable namespaced imported targets, but
   do not call `dmn` a minimal Core component unless its current
   feature-conditioned composition is split or clearly documented.
2. Define build-tree and install-tree include interfaces for each target.
   Avoid exporting absolute source/build paths. The current `dmn` target has
   absolute source and generated-build include paths; optional targets also
   need explicit install include interfaces.
3. Generate relocatable package config/version files with CMake's
   `CMakePackageConfigHelpers`; resolve each exported target's transitive
   dependencies only when its component is requested.
4. Account for the actual dependency graph: with
   `ENABLE_DMN_DMESGNET=ON`, `dmn` includes DMesgNet/Kafka and links Protobuf
   and librdkafka publicly; the core-only configuration disables that stack.
   D-Bus, BlueZ, and ANCS are separately gated. Do not leak FetchContent
   source/build paths into an installed config; define whether installable
   packages require system dependencies or another explicit strategy.
5. Install generated protobuf headers when the public DMesg headers require
   them, or deliberately narrow the installed public API so generated
   build-tree paths do not leak.
6. Add external-consumer smoke projects that install Dmn to a temporary
   prefix, configure a separate project using `find_package()`, and link the
   selected targets. Test at least core-only and one optional configuration,
   plus relocation by moving the install prefix.

**Acceptance:** a clean downstream project builds from the install prefix with
no source/build-tree include paths; optional targets are present only when
built/installed; dependencies and compile definitions propagate correctly;
the core-only configuration remains free of DBus, BlueZ, Kafka, GTK, and Rust
requirements. An `ENABLE_DMN_DMESGNET=ON` package correctly reports its
Protobuf/librdkafka requirements. Keep pkg-config files as a later
distro-integration choice, not a second package-discovery mechanism in the
first slice. CMake's [package guide](https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html)
and [`CMakePackageConfigHelpers`](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html)
document config-package and relocatable-config mechanisms.

### Phase 2 — DMesgNet reliability and truthful status (M)

Follow the existing gap analysis in
[`dmn-dmesgnet-spec.md`](specs/dmn-dmesgnet-spec.md), starting with input
validation, repair-path safety, bounded buffers, readiness behavior, and
deterministic tests. Keep each change small and document observable results.

Suggested implementation order:

1. Add explicit validation and errors for semantically invalid envelopes and
   oversized payloads. Keep malformed protobuf handling distinct from
   transport-level truncation/errors.
2. Remove unsafe assumptions in conflict repair when the cached value or
   output endpoint is absent.
3. Bound pre-ready outbound data, preserve FIFO unless a topic explicitly
   defines replacement semantics, and expose overflow outcomes.
4. Define readiness for all endpoint combinations and cover each in tests.
5. Replace sleep-based protocol assertions with controlled clocks and fake
   transports where the tested behavior allows it.

**Acceptance:** tests cover invalid input, no-cache/no-output repair, bounded
queue overflow, shutdown, and all endpoint combinations; status distinguishes
local acceptance/output failure from remote receipt (which cannot be claimed
without an acknowledgement protocol); docs continue to state that there is no
authentication, durable history, consensus, or exactly-once delivery. Do not
make network DLock the acceptance test for this phase.

### Phase 3 — LAN discovery as an optional library (M)

The proposed [discovery specification and test-driven implementation
plan](specs/dmn-mdns-discovery-spec.md) refines this opportunity. Its ordered
fake-first phases, Linux Avahi/macOS Bonjour backend gates, and explicit
cross-platform qualification take precedence over the illustrative slices
below. The specification remains unimplemented.

Add a small optional `dmn-discovery` target and a discovery interface,
initially independent of DMesgNet. The interface should represent browse,
resolve, add/remove, daemon-unavailable, cancellation, and shutdown outcomes
without exposing Avahi types to callers. Keep the existing manual endpoint
configuration.

Recommended slices:

1. Prototype browse/resolve against `avahi-client` and confirm its event-loop
   integration does not block Dmn's runtime or GUI.
2. Define stable service type, instance metadata, version compatibility, and
   finite TXT-record/address limits.
3. Add a fake discovery backend and deterministic lifecycle tests; add one
   opt-in Avahi-daemon integration test.
4. Add an explicit discovery option to a diagnostic CLI or configuration
   surface. Discovery should list/resolve candidates, not automatically trust
   them or silently change the configured cluster.
5. Only after a separate transport design, evaluate DMesgNet integration. The
   design must specify peer fan-out, bind/receive behavior, identity, and
   best-effort failure reporting; a DNS-SD record alone is not a transport.

**Acceptance:** builds without Avahi when the option is off; no core Dmn
dependency on Avahi; tests cover daemon loss, duplicate instances, malformed
records, cancellation, and peer disappearance; documentation clearly states
that discovery provides neither authentication nor transport security.
Acceptance for the discovery library does not claim automatic DMesgNet peer
connection; that requires its own transport/API milestone.

### Phase 4 — Bluetooth device health MVP (S)

Implement BlueZ battery reporting as an optional device value. Do not turn
this into a generic health framework until another concrete signal is
identified.

Recommended slices:

1. Decode and update `Battery1.Percentage` in inventory snapshots and
   `PropertiesChanged`/interface add/remove handling.
2. Carry the optional value through the shared engine and both demo/production
   backends.
3. Present “Battery: N%” or “Not reported” in GTK and COSMIC, with no automatic
   connection, pairing, scan, or monitoring behavior added.
4. If users want alerts, add an explicit threshold setting and deduplicate
   alerts across repeated property updates.

**Acceptance:** private-bus tests cover present, changed, removed, invalid, and
absent battery properties; app tests cover both available and unavailable
states; demo mode exercises both; a device without battery support remains
fully usable. Hardware qualification is optional and uses ordinary supported
peripherals.

### Phase 5 — User-session Bluetooth service (L, conditional)

Use the existing
[`dmn-bluetooth-session-service-spec.md`](specs/dmn-bluetooth-session-service-spec.md)
as the source of truth. This is not a prerequisite for the Phase 4 battery
MVP. Start only after users confirm the need for monitoring that survives the
application process or for multiple frontends sharing one live monitor.

Keep the implementation scoped to one user session, explicit start/stop,
bounded in-memory events, payload-free status for shell clients, private-bus
tests, and optional packaging. Do not add a system-wide/root service, automatic
login monitoring, or persistent notification content.

**Acceptance:** fake-monitor lifecycle tests pass without BlueZ; clients recover
from missed signals using sequence numbers/snapshots; no monitor starts on
service activation alone; service shutdown/logout clears event payloads;
packaging installs a user unit only when explicitly selected.

### Phase 6 — MQTT adapter (M, conditional)

Proceed only after a user-backed requirement is identified. Keep a standalone
MQTT `Dmn_Io<std::string>` adapter separate from `Dmn_DMesgNet` initially.
First verify ordering, reconnect, retained-message, duplicate, maximum-size,
and shutdown semantics against the broker client chosen. Then make a separate
decision about a DMesgNet adapter.

**Acceptance:** the optional target does not affect default builds; credential
configuration is external to source control; broker integration tests are
opt-in and local; API docs do not equate MQTT QoS with Dmn commit or conflict
resolution.

## Existing plans to keep distinct

These are already present in repository TODOs or specifications and should be
treated as planned work rather than rediscovered feature ideas:

- **Per-node D-Bus DMesg gateway:** the conflict observer exists, but the
  daemon/client gateway remains a proposal in
  [`dmesgnet-dbus-node-gateway-spec.md`](specs/dmesgnet-dbus-node-gateway-spec.md).
- **Distributed locking:** `Dmn_DLock` has a partial local prototype, while
  the publisher-serialized and consensus-backed designs remain incomplete;
  see [`dmn-distributed-lock-spec.md`](specs/dmn-distributed-lock-spec.md).
- **Future-based async API:** an incremental design already exists in
  [`future-based-async-api-spec.md`](specs/future-based-async-api-spec.md).
- **GNOME Shell extension and desktop release qualification:** both are
  documented separately; the extension is optional and GNOME-specific, while
  app qualification is not itself a new product feature.
- **Bluetooth notification archive/export:** not currently a plan. The
  existing GTK and COSMIC specs intentionally keep notification contents
  session-local; revisit only with explicit user demand and a separate privacy
  design.

## Research references

- [BlueZ Battery API](https://bluez.readthedocs.io/en/latest/battery-api/) —
  optional `org.bluez.Battery1` percentage property.
- [XDG Desktop Portal Notification API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html)
  — send/update/withdraw behavior and notification action model; not a
  notification-history API.
- [Avahi API documentation](https://avahi.org/doxygen/html/) — API selection
  guidance for C/C++ and event-loop integration.
- [RFC 6763: DNS-Based Service Discovery](https://www.rfc-editor.org/rfc/rfc6763)
  — standard service browsing and instance resolution.
- [OASIS MQTT Version 5.0](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)
  — publish/subscribe transport and delivery QoS.
- [systemd service units](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
  and [timer units](https://www.freedesktop.org/software/systemd/man/latest/systemd.timer.html)
  — process supervision and scheduled activation references if Linux user
  services are packaged.
- [Gio.Application](https://docs.gtk.org/gio/class.Application.html) — Linux
  session-scoped application uniqueness and D-Bus actions.
- [CMake package guide](https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html)
  and [`CMakePackageConfigHelpers`](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html)
  — config packages, exported targets, and relocatable package files.
- [GNOME Bluetooth help](https://help.gnome.org/gnome-help/bluetooth.html) —
  standard desktop Bluetooth management already covers pairing and
  connect/disconnect workflows.
- [KDE Connect](https://kdeconnect.kde.org/) — an established broader
  device-integration product, including notification synchronization and
  device battery display.

Online sources were consulted on 2026-11-20. Re-check API versions, packaging
guidance, and distribution support before implementation; standards and
project documentation can change independently of this plan.
