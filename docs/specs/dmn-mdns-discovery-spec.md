<!-- Copyright © 2026 Chee Bin HOH. All rights reserved. -->

# Reusable DNS-SD/mDNS Discovery

**Status: implementation in progress; not yet platform-qualified.** This
document defines a test-driven Phase 3 design. Contract sketches are not
evidence of platform qualification. Phase 2's `Dmn_DMesgNet` behavior is
unchanged.

## 1. Decision and scope

Provide an optional, reusable Dmn-owned DNS Service Discovery (DNS-SD) API,
independent of Dmn's DMesgNet, D-Bus and BlueZ features, protobuf, Kafka and
any GUI. The Linux backend itself uses the system D-Bus through Avahi. Its first
backend uses the system **Avahi client library and daemon on Linux**. A native
**Bonjour/mDNSResponder backend on macOS** implements the same portable
contract; it is a separate milestone and is required before claiming Phase 3
is cross-platform complete. Do not silently substitute one backend for the
other. Linux-first releases must explicitly report the missing macOS backend.

The interface supports explicit service browsing, resolution, live changes,
service registration, local snapshots and clearing a Dmn-owned view. It is
deliberately not a one-for-one wrapper for every Avahi or Bonjour API. Extend
the public domain model only when a concrete use case and tests require it.

```text
Applications / future DMesgNet integration (separate decision)
             |
    Dmn discovery API: owned values, browse, advertise, snapshot
             |
    private backend interface and deterministic fake
          /                       \
Linux: avahi-client            macOS: DNSService* API
          |                       |
system D-Bus + avahi-daemon    mDNSResponder
```

Like `Dmn_DbusClient` and `Dmn_BluezClient`, this boundary owns its data,
exposes typed failures, has explicit lifetimes, and does not expose backend
handles. Unlike `dmn-bluez`, the Avahi implementation uses the upstream
`avahi-client` C API directly: Avahi already wraps its D-Bus API. It does not
reimplement Avahi over `Dmn_DbusClient`, embed `avahi-core`, or require
`ENABLE_DBUS`/`dmn-dbus`.

### Goals

- Discover service instances by type and domain on selected interfaces and
  obtain host, port, address candidates and opaque TXT data.
- Observe additions, changes, removals and backend failures, including daemon
  loss; provide a bounded, owned snapshot for each browse session.
- Offer explicit registration and withdrawal of a caller-supplied service.
- Preserve interface identity, IPv4/IPv6 information, error causes and data
  ownership across platform backends.
- Compile and test the core abstraction with deterministic fakes, without a
  daemon or physical network; qualify both native backends separately.

### Non-goals

- Automatic DMesgNet transport configuration, peer fan-out, network sockets,
  reconnecting application protocols, cluster membership or distributed locks.
- A general DNS resolver, an mDNS protocol implementation, arbitrary DNS
  record browsing, domain enumeration, service subtypes, multiple SRV/TXT
  records per instance, a graphical browser, or a full Avahi/Bonjour binding.
- Global daemon-cache deletion, persistent discovery history, TTL inference,
  peer health checks, authentication, authorization or encryption.
- Guaranteeing every service, every address, a fresh network query, a bounded
  discovery time, cross-subnet discovery or successful remote communication.

### Product deliverable and acceptance boundary

The deliverable is an installable C++ library with which an independent
application can explicitly advertise a service on one host and browse that
service type on another, observe its interface-scoped instance, hostname,
port, available address candidates and TXT, track changes/removal, refresh
and clear its own view, and stop cleanly. The library must also work in
applications unrelated to DMesgNet. The release acceptance demonstration and
its environmental prerequisites are defined in section 11.

This deliverable **does not automatically discover or configure DMesgNet
peers**. A future complete DMesgNet discovery product requires a separately
specified integration and transport milestone, including peer identity,
binding, fan-out and trust. Passing this discovery-library specification
cannot be presented as evidence that DMesgNet nodes connect automatically.

## 2. Research and platform boundaries

DNS-SD uses PTR records to enumerate named instances, SRV records for a target
host and port, and TXT records for metadata; it can work over mDNS or unicast
DNS ([RFC 6763, sections 1, 4–6](https://www.rfc-editor.org/rfc/rfc6763)).
mDNS is link-scoped multicast DNS with protocol-defined cache, TTL, goodbye
and cache-flush behavior ([RFC 6762](https://www.rfc-editor.org/rfc/rfc6762)).
DNS-SD discovery and mDNS are related but not synonymous: a system's default
browse domain can include configured non-multicast DNS-SD. This design defaults
to the explicit `local.` domain; another domain is an explicit caller choice.
The backend must not label a result multicast merely because it was browsed.
Initial release qualification covers `local.` browsing and registration.
Non-local browsing is best-effort only when the configured native backend
supports it; subscription or policy failures must be reported, not silently
retried as `local.`. Non-local registration is outside initial acceptance and
must return an explicit unsupported error rather than promise dynamic DNS
updates. Long-lived unicast updates depend on the DNS server and backend.

Avahi recommends `avahi-client` for C/C++ applications, and warns against
embedding another mDNS stack for ordinary desktop applications. Its service
browser produces instance events with interface and protocol; a service
resolver supplies host, port, address and TXT. A live resolver can report
changes; browser events can be collapsed. Its `CACHE_EXHAUSTED` and
`ALL_FOR_NOW` events are not proof that the network is complete or that all
instances resolved ([Avahi API guidance](https://avahi.org/doxygen/html/),
[lookup API](https://avahi.org/doxygen/html/lookup_8h.html),
[events and flags](https://avahi.org/doxygen/html/defs_8h.html)).

macOS provides Bonjour through system `DNSService*` APIs and mDNSResponder,
not the Linux system Avahi daemon as a standard assumption. Browse reports an
instance and interface; resolve reports a host, network-byte-order port and
TXT, while address lookup is separate. Apple's documentation recommends
`DNSServiceQueryRecord` to monitor TXT rather than assuming
`DNSServiceResolve` supplies an ongoing TXT subscription
([browse](https://developer.apple.com/library/archive/documentation/Networking/Conceptual/dns_discovery_api/Articles/browse.html),
[resolve](https://developer.apple.com/library/archive/documentation/Networking/Conceptual/dns_discovery_api/Articles/resolving.html),
[DNS-SD C API](https://developer.apple.com/documentation/dnssd/dns-service-discovery-c)).
An application bundle may need local-network privacy declarations and user
permission; consult Apple's
[local-network privacy guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy).
The library cannot grant permission or supply a host application's Info.plist.

| Environment | Proposed backend | Requirements and qualification |
| --- | --- | --- |
| Linux | `avahi-client`/`avahi-common` | Development libraries at build time; a reachable system bus and Avahi daemon at runtime. Offline and restart behavior are explicit. |
| macOS | System DNS-SD/Bonjour | Native `dns_sd.h` and mDNSResponder; no Avahi or Linux D-Bus dependency. Host-app privacy policy applies. |
| Other platforms | None initially | Building core without discovery remains supported; explicitly enabling an unsupported backend fails configuration. |

Both platforms require suitable network interfaces and policy. Multicast may
be filtered or isolated; a result is neither proof of reachability nor a
trusted identity. The backend must preserve provenance when known without
inventing it when the native API does not expose it.

## 3. Targets and API boundary

The public header is `include/dmn-discovery.hpp`. Internal discovery headers
also live in `include/`, are private implementation/test seams, and are not
installed or part of the supported API. Build-tree consumers can technically
include them because the source include directory is public; they must not
depend on them. Discovery is an optional
feature of `dmn`, exported as `Dmn::dmn`, with a build option
`ENABLE_DMN_DISCOVERY` defaulting to `OFF`. The Dmn-owned types are
platform-neutral. No Avahi, libdbus, DNSService, protobuf or GUI types appear
in public headers. Backend objects and fake injection are private
implementation/test seams, not a public test-only constructor.

The following is a **contract sketch**, not a final compilable header.
Names may be refined before implementation while preserving the semantics:

```cpp
using Dmn_DiscoveryResult = std::expected<void, Dmn_DiscoveryError>;
template <typename T>
using Dmn_DiscoveryValueResult = std::expected<T, Dmn_DiscoveryError>;

class Dmn_DiscoveryBrowser {
public:
  Dmn_DiscoveryValueResult<Dmn_DiscoverySnapshot> snapshot() const;
  Dmn_DiscoveryValueResult<Dmn_DiscoveryRefreshId>
  refresh(const Dmn_DiscoveryServiceKey& key);
  Dmn_DiscoveryResult clearCache(); // this browser's Dmn-owned view only
  Dmn_DiscoveryResult stop();       // idempotent drain barrier
  ~Dmn_DiscoveryBrowser();
};

class Dmn_DiscoveryRegistration {
public:
  Dmn_DiscoveryValueResult<Dmn_DiscoveryRegistrationStatus> status() const;
  Dmn_DiscoveryResult stop();       // withdraw this registration
  ~Dmn_DiscoveryRegistration();
};

class Dmn_DiscoveryClient {
public:
  static Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryClient>>
  create(Dmn_DiscoveryOptions options);
  Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryBrowser>>
  browse(Dmn_DiscoveryQuery query, Dmn_DiscoveryEventHandler handler);
  Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryRegistration>>
  advertise(Dmn_DiscoveryAdvertisement advertisement,
            Dmn_DiscoveryRegistrationHandler handler);
  Dmn_DiscoveryResult shutdown();
  ~Dmn_DiscoveryClient();
};
```

Construction/factory errors are explicit, not null success values. A client
can create independent browsers and registrations subject to configured
limits. `create()` validates options and establishes owned local worker
resources; daemon absence alone is not a construction failure. It does not
browse or advertise until requested. Backend initialization and daemon
availability are reported asynchronously; invalid configuration, allocation
or worker-creation failure return an error and leave no worker or native
resource running. Owned handles are non-copyable. Client shutdown stops all handles;
handles that outlive the client remain safely stopped through shared internal
state and do not dereference the destroyed client. Operations after
stop/shutdown return a typed stopped error except idempotent
`stop()`/`shutdown()`; browser `snapshot()` and registration `status()` remain
available while their handles are alive to inspect terminal status, without
posting to a worker that has already exited.
`browse()`/`advertise()` mean **accepted locally**, not that discovery or
registration has completed. Backend-ready/failed events report the eventual
outcome. A callback may run before the creating call returns, so a handler
must not assume its handle has already been published to the caller. Do not
offer synchronous waiting from the callback worker.

### 3.1 Query and owned discovery values

- A query specifies one DNS-SD service type (`_name._tcp` or `_name._udp`),
  an explicit domain defaulting to `local.`, an optional interface index, and
  requested address families. Do not implicitly browse all service types or
  start browsing before `browse()`. Advertising is an independent explicit
  operation and does not require a prior browse.
- A portable service key is the tuple `(instance label, type, domain,
  interface index)`. Identical names on different interfaces are separate.
  mDNS query transport and resolved address family are **not** key fields:
  Bonjour's browse callback does not supply Avahi's query-transport protocol.
  Preserve original display strings; use DNS-aware ASCII case-insensitive
  comparison and correct escaping internally. Never split the instance at a
  literal dot or equate Unicode-normalized forms without a protocol basis.
  A backend result with no meaningful interface index must be marked
  interface-unknown; do not merge it with a known interface or invent scope.
- A candidate owns the key, resolution state, hostname, host-order port,
  zero or more typed IP addresses and TXT entries. Addresses retain their
  family and interface/scope needed for IPv6 link-local use. An unresolved
  instance is not an endpoint; no address is fabricated. A port of zero or
  unusable address is not presented as a usable endpoint. In particular, an
  IPv6 link-local address without a known interface/scope is not usable.
- Preserve every backend-reported TXT entry as length-delimited bytes,
  including any reported empty entries, duplicate keys, binary values,
  backend-provided ordering and unknown keys.
  Distinguish an absent TXT record from an empty record when the backend can
  report it; otherwise represent that distinction as unknown. Do not
  treat TXT as a NUL-terminated string, credentials, or trusted configuration.
  A future convenience parser must not replace the raw representation.
  This applies to the data delivered by the backend, not necessarily the
  original network packet. In particular, Avahi 0.8 discards empty strings
  when parsing and serializing TXT RDATA and reserializes the daemon's record
  for a record-browser callback. An Avahi consumer cannot recover those
  discarded entries or claim byte-for-byte network TXT preservation. To avoid
  silently changing an advertisement, the Avahi backend rejects TXT with more
  than one entry if any entry is empty (for example `["version=1", ""]` or
  `["", ""]`). An empty list or a single empty entry remains supported as an
  empty TXT record. Other daemon versions may behave differently, but the
  conservative rejection applies to the Avahi backend.
  ([Avahi TXT parser](https://github.com/avahi/avahi/blob/v0.8/avahi-common/strlst.c),
  [DNS parser](https://github.com/avahi/avahi/blob/v0.8/avahi-core/dns.c),
  [record browser](https://github.com/avahi/avahi/blob/v0.8/avahi-daemon/dbus-record-browser.c)).
- A snapshot is an owned, point-in-time **local observation**, with browser
  state, epoch and sequence. It is not a network-wide inventory. Include
  unresolved/error state rather than a success-shaped empty endpoint.
- `Dmn_DiscoveryError` has a stable Dmn category and human-readable detail,
  plus optional backend domain/code. Categories distinguish invalid input,
  unsupported platform/capability, unavailable/disconnected backend,
  permission/policy, resource limit/overflow, resolution failure, collision,
  cancellation/stopped and internal failure. Never require callers to parse
  error strings or expose borrowed backend buffers. An allocation failure may
  have empty detail so reporting it does not require another allocation.

The public options must expose the limits and attempt deadline in sections 3.3
and 7; queries and advertisements own their strings and byte sequences.
Browser events own kind, epoch, sequence and the applicable service
key/value or error. Browser snapshots own browser state, epoch, sequence and
candidate values. Registration status owns its state and last error. A key
can be copied from an event or snapshot and supplied to `refresh()`. Public
enums must represent all states named by this specification, including
monitoring-incomplete, record-conflict, callback-failure and stopped.

DNS-SD instance names are not globally unique peer IDs. Treat values as
untrusted network input: validate protocol syntax and UTF-8 where the DNS-SD
instance-name contract requires it, enforce finite byte/count limits before
copying, and escape control characters in any diagnostic output. Preserve a
valid instance's punctuation and spelling. Use the platform/protocol name
validators where applicable, but test equivalent portable behavior.

### 3.2 Event and snapshot semantics

Each browser has one monotonically increasing epoch and a per-browser
monotonic sequence assigned on the serialized state worker. Events carry
owned values and include `Added`, `Changed`, `Removed`, `Reset`,
`BackendAvailable`, `BackendUnavailable`, and `Error`; an addition can be
unresolved and resolution later produces `Changed`. The public contract does
not require backend-specific cache-exhausted or all-for-now events.
The initial snapshot has sequence zero. Assign the next sequence exactly once
per observable event, including control events; a snapshot reports the last
assigned sequence and does not increment it. Merge the separate bounded data
and control queues in sequence order. Reset, stop and overflow may discard
old deltas as explicitly specified; reset/overflow tells consumers to resync.
Do not silently assign sequence numbers to invisible internal changes.

- Deduplicate equivalent observations per key. Internally retain a set of
  active browse contributions per key: Avahi distinguishes the interface and
  IPv4/IPv6 *query transport*, while Bonjour supplies an interface-scoped
  observation without a transport. Duplicate `NEW` for an existing
  contribution is idempotent, not a reference-count increment. Emit `Added`
  on the first contribution and `Removed` only on removal of the last one.
  Losing one of several contributions removes only its derived data and
  produces `Changed` if the visible candidate changes. Different interface
  indices remain separate. Last removal cancels the key's work; a late
  resolver callback cannot recreate it.
- Snapshots and events are ordered by epoch/sequence. A snapshot can include
  changes whose callbacks are still queued; sequence numbers let consumers
  discard events at or below the snapshot sequence. Events are **deltas**:
  clients detecting a gap or `Reset` must fetch a new snapshot.
- A backend may expose only some addresses; the contract does not promise a
  complete A/AAAA set. Preserve distinct currently observed addresses and
  IPv6 scope. Address family and query transport are distinct; Avahi
  `UNSPEC` alone does not guarantee both address families. Per-source
  replacement, failures and retry are specified in section 3.3.
- Daemon loss invalidates the local view, cancels associated resolutions and
  increments the epoch. Emit `BackendUnavailable` and `Reset`; do not
  manufacture remote per-service goodbyes. On reconnection, recreate
  backend objects and rebuild from fresh backend events with a new epoch.
  Initial absence, loss, repeated loss, and permanent shutdown are distinct.
- Network silence, a stale address or an absent entry does not prove a
  machine is down. A caller can request `refresh(key)` before a connection;
  this does not guarantee a network query or reachability.

### 3.3 Resolution, refresh and record conflicts

Each active key has a monotonically increasing resolution generation.
Discovery starts a bounded live resolution for each active backend
contribution and requested address family. Each native resolver/lookup is a
separate source. On an Avahi `FOUND`, **replace** that source's prior address
with the returned address; never append successive answers from the same
resolver. On a Bonjour address add/remove, update that lookup's address set.
The exposed set is the deduplicated union of current, valid sources. On
source failure, contribution removal or cancellation, remove that source's
addresses immediately, retaining those independently supported by another
source. Never retain an old address merely because a replacement failed.

SRV or TXT records are watched independently as described in section 8.
Use bounded sets of distinct currently observed records, deduplicated across
contributions with per-contribution presence. Compare SRV priority, weight,
port and DNS-normalized target; compare TXT as exact backend-reported wire
bytes except for the RFC 6763 empty-TXT equivalence below. Avahi's prior
normalization can make distinct network records indistinguishable here. A record
remove affects only its contribution; delete it from the union when the last
contribution disappears. The following applies to records *observed* by these
subscriptions, not to an unknowable complete network-wide record set. A
singleton record may be used before another conflicting record is observed;
the later conflict immediately invalidates the endpoint. Neither subscription
startup nor an initial progress event proves that no conflict exists.

- Zero SRV records means unresolved. As required by
  [RFC 6763 section 6.1](https://www.rfc-editor.org/rfc/rfc6763.html#section-6.1),
  no TXT record, zero-length RDATA and a single empty string are equivalent
  as application data. A service can therefore resolve without an observed
  TXT record. Preserve whether a record was observed as metadata, without
  claiming an absent record when the backend merely has not reported it.
  Treat zero-length RDATA and the single empty string as equivalent for
  conflict detection; do not emit zero-length TXT RDATA in advertisements.
- More than one distinct observed SRV or TXT record means
  `MultipleRecordsUnsupported`. Clear usable addresses and emit `Changed`
  with that per-key error; do not silently select one record. When the observed
  set returns to one SRV and at most one TXT, clear the error and restart
  resolution.
- A changed or removed SRV, conflict, or reset invalidates all address results
  from the old resolution generation before starting new lookups. A TXT change
  updates metadata without retaining stale TXT. Ignore late results from old
  generations. Transient overlap during record replacement can temporarily
  produce the conflict state; no arbitrary quiet-period timer implies that
  all answers have arrived.
- An SRV target of `.` or port zero denotes no usable endpoint; do not query
  or advertise an endpoint for it. Keep the instance and expose its
  unresolved/unavailable state.
- Publish a resolver's address only when its resolved host and port match the
  currently observed singleton SRV; otherwise wait/retry and report partial
  resolution. TXT comes from the independent live record subscription, not a
  possibly older resolver callback. This prevents combining addresses with
  an incompatible service target.

On a recoverable resolver or record-subscription failure, remove that source's
usable data, expose the per-key/family error, release failed native handles,
and retry while its browse contribution is present. Use exponential backoff
starting at 1 second, capped at 30 seconds; retries continue until success,
removal, reset or stop. Only one retry timer per failed source is permitted;
retrying must respect resource limits and must not require another `NEW`.
Permanent/unsupported and policy errors remain visible without automatic
retry. A recovered source clears its own error and emits a change.
While any active contribution lacks a functioning SRV or TXT subscription,
mark that key's record monitoring incomplete and do not expose a usable
endpoint; resume only once subscriptions recover. Malformed or oversized
record data likewise cannot silently pass as a complete monitored view.
Record subscriptions use their own lifetime tokens: refreshing address
resolution does not invalidate still-live record watches, while reset,
contribution removal and replacement do.

`refresh(key)` validates that the key is currently present, allocates a
browser-local monotonically increasing refresh ID and starts a new resolution
generation on the state worker. It cancels old lookups, clears old addresses,
and schedules the same monitored resolution path; it does not restart the
entire browser or flush the daemon cache. The call returns the ID on accepted
submission, not an endpoint. The candidate snapshot records the latest ID
and `Pending`, `Succeeded` or `Failed`; emit a `Changed` event on completion
even if values match the previous attempt. Success means one usable address
was observed under the matching singleton SRV and without a record conflict;
failed family attempts are visible separately, and retries may later recover
the candidate. A later
refresh supersedes the earlier one; its ID is no longer pending. Removal,
reset, loss or stop cancels in-flight refresh work; the corresponding
`Removed`, `Reset`, unavailable or terminal state signals that cancellation
instead of promising a completion callback after a stop barrier. An unknown
key, stopped browser or exhausted command queue returns a typed error.
Refreshing from an event callback may wait for state-worker submission and
validation, but must never wait for the callback worker or for a DNS result.
Each explicit attempt has a configurable 5-second default deadline; when no
source succeeds by then, mark that refresh failed without stopping background
retries. The deadline is a Dmn operation timeout, not a DNS TTL, a proof of
absence or a promise to bypass the daemon cache. A timed-out refresh remains
`Failed`; a later background recovery changes candidate state but does not
retroactively change that completed attempt to `Succeeded`.

## 4. Cache and `clearCache()` contract

There are **two separate caches**: the system daemon's shared DNS/mDNS cache,
and Dmn's per-browser bounded view. Dmn's `clearCache()` affects only the
calling browser's view and outstanding browse/resolution work. It does not
flush Avahi, mDNSResponder, the operating-system DNS cache, other browsers or
other processes, and does not send goodbye packets or withdraw registrations.
Do not expose a privileged daemon-wide flush or promise bypassing its cache.

On a live browser, `clearCache()` is a serialized local reset:

1. Advance the epoch, stop the old browser and its resolvers, discard its
   local candidates and invalidate their callbacks.
2. Suppress queued but not yet started old-epoch data callbacks; allow any
   already-running callback to finish before the reset barrier returns.
3. Enqueue one `Reset` for the new epoch, then recreate the browse using the
   same query if the backend is available. If unavailable, remain visibly
   unavailable and rebuild when reconnected. If the bounded control queue
   cannot accept `Reset`, transition to overflow and return its error rather
   than reporting a successful reset.
4. Return after local invalidation and restart **submission**, not after all
   network responses or event delivery. A newly browsed service can immediately
   reappear from the daemon cache. If restarting fails, return/report the
   typed error and leave the browser in a visible failed state.

No old-epoch data callback may begin after `clearCache()` returns; `Reset`
precedes all new-epoch data events. `snapshot()` after return reflects the
new epoch and may already contain newly rediscovered entries. Calling
`clearCache()` from the browser's event callback is prohibited because it
waits for the callback drain. `stop()` and client shutdown follow the same
barrier principle. There is no Dmn timer-based TTL: backend removal/expiry
events govern the view, and daemon loss invalidates it. A snapshot is not a
promise that an entry remains reachable for any duration.

## 5. Registration

An advertisement explicitly specifies instance label, service type, domain,
port, optional interface selection and bounded raw TXT entries. Validate before
backend calls; reject port zero, invalid names, malformed TXT framing and
resource limits. No default DMesgNet service type, reserved port or protocol
version is invented here. The native backend advertises the host machine's
daemon-selected target hostname; this version does not publish an arbitrary
remote host or open a listening socket for the caller.

For Avahi, reject a multi-entry TXT value containing any empty entry with an
explicit unsupported-capability error before starting native registration.
Do not silently remove entries. Both an empty list and a single empty entry
are accepted as an empty TXT record, using the RFC 6763 empty-TXT equivalence.
This restriction does not prohibit binary or duplicate non-empty entries.

`advertise()` returns a registration handle on local acceptance. Its
serialized events distinguish registering, established, collision,
unavailable and failure. A collision is reported; do not silently rename the
service. `status()` returns an owned state and last error, including callback
failure or terminal stopped state, and remains callable after client shutdown.
`stop()` is idempotent, withdraws only this handle's registration, suppresses
queued events, drains any callback already started, records stopped status
and does not stop another process's services. It does **not** emit a stopped
callback. No callback starts after `stop()` returns. Client shutdown uses the
same rule. Withdrawal releases the local native registration; it does not
guarantee when remote caches expire or that a goodbye packet was delivered.
An established event means the backend reported registration, not that any
peer discovered it.

On Linux, use an Avahi entry group, commit only when the daemon permits it,
and handle registering/established/collision/failure state transitions. On
macOS, retain and deallocate the owned DNSService registration reference.
Pass `kDNSServiceFlagsNoAutoRename` so a name conflict is reported instead of
allowing Bonjour's default automatic rename; convert the requested host-order
port to network byte order for registration.
On daemon loss, registrations become unavailable; after reconnect, re-register
only still-owned, explicitly requested registrations and report the new
result. No automatic retry may change the instance name. Updates to a live
advertisement are deferred to a separate API/design; stop and create a new
registration when needed.

## 6. Threading, ownership and lifecycle

- One owned state worker handles backend polling, native callbacks and
  client/browser state. Copy borrowed native values before returning from
  the callback; apply or enqueue the resulting state change on that worker.
  Deliver application callbacks on a separate serialized callback worker.
  Never call application code or wait for application work in a native
  callback. Do not use Dmn's process-wide runtime thread for blocking backend
  work. Document actual worker ownership in public Doxygen.
- API calls are thread-safe while the client and handles are alive.
  Callers must externally coordinate destruction with all method calls.
  Callbacks must not throw, block indefinitely, destroy a handle/client, or
  call barrier operations (`clearCache`, `stop`, `shutdown`). An exception
  escaping a callback is caught, recorded and transitions the affected
  session to a visible callback-failure state. Stop invoking that session's
  handler after it throws, invalidate its outstanding native work, and retain
  the error for `snapshot()` or registration `status()`. Other sessions remain
  usable, subject to their own limits.
- Detect barrier calls from the callback context and return a typed reentrant
  call error without beginning a barrier. `snapshot()` is permitted from a
  callback; barrier coordination must not block the state worker while it
  waits for that callback, or a concurrent `snapshot()` could deadlock.
- `stop()` prevents new backend work, invalidates the generation, destroys
  browser/resolver or registration objects on their owning context, drains
  any started user callback, and guarantees no further callbacks for that
  handle after return. Shutdown stops all handles, tears down backend objects,
  stops/joins workers, and only then releases their state. Idempotent repeated
  calls do not issue duplicate native cleanup. Because callbacks and native
  shutdown may block, this is a drain guarantee, not a fixed-time guarantee.
- Each async callback carries a session/epoch token. Late callbacks after
  removal, reset, daemon restart or stop are ignored without touching freed
  objects. No callback captures a raw object pointer beyond its proven
  lifetime. Concurrent stop/reset/shutdown operations are serialized.

Use `AvahiSimplePoll` on the state worker and marshal external commands to
that worker with an interruptible wakeup. Avahi constructors can invoke a
native callback immediately; initialize callback state first and test this
reentrancy. On macOS, integrate DNSService sockets and the same command
wakeup in an owned poll loop on the state worker. Process and deallocate each
reference there; no concurrent `DNSServiceProcessResult` and deallocation of
the same reference. This choice eliminates a separate unbounded native-to-state
handoff and avoids calling user code with an Avahi threaded-poll lock held.
The implementation must verify that the poll/wakeup integration supports
prompt shutdown on both systems before claiming platform acceptance.

### 6.1 Reuse of existing Dmn execution primitives

The private fake uses a `DiscoveryCommandWorker` and a `DiscoveryDispatcher`.
Both own a `Dmn_Proc`, as do the D-Bus input/output workers. Use composition
and keep discovery's separate state
and callback workers. `Dmn_Proc` is not a process-wide runtime and does not
itself provide a command queue, a poll wakeup or a callback-drain barrier.

The worker must stop cooperatively: close admission, wake its wait or native
poll, finish or explicitly cancel admitted work as required, then call
`Dmn_Proc::wait()` under serialized lifecycle ownership. Check `exec()` failure
and report it through discovery's typed error channel; roll back a partially
started client. Do not rely on the destructor's pthread cancellation as the
normal shutdown path. Select and handle the worker exception policy explicitly;
never swallow a pthread forced-unwind exception as an ordinary callback or
command failure. Discovery workers disable pthread cancellation while running:
native C callbacks cannot safely propagate cancellation, and shutdown uses
the explicit wake and cooperative join path. A worker cannot join itself.
Keep callback-side snapshot and refresh usable while an external barrier drains
a callback. A fatal state-worker failure completes queued waiters with an
error and tears down its poller; a fatal callback-worker failure stops
delivery, records the error and requests native cleanup on the state worker.
If cleanup submission cannot allocate, retain a resource-limit error, mark
the affected session stopped and wake the native owner to finish cleanup.
If shutdown exits before completing because of an allocation failure, later
or concurrent shutdown calls must be able to retry instead of waiting forever.
Reset and stop leaders prepare their failure diagnostics before admission and
publish results without allocating for an error copy. Allocation failures
release joined callers with a resource-limit error and leave the barrier
available for a later retry.
On Linux with libstdc++, cancellation of a barrier leader propagates pthread
forced unwind, releases joined callers with a cancellation error, and permits
a subsequent barrier call to finish cleanup.

`Dmn_Pipe` is an in-memory typed FIFO with an optional worker, not an OS pipe
or an automatically pollable file descriptor. Its default queue is unbounded,
and its current API does not provide discovery's per-session control slots,
priority/coalescing, overflow latch, cancellation of queued resets, event
epochs or callback barriers. Scheduled mode drains accepted deadlines during
shutdown. Consequently, replacing either discovery queue with an unmodified
pipe or `Dmn_Async` would not satisfy sections 6 and 7. A native poll loop
still needs a separately designed interruptible wakeup, with commands owned
and bounded on the state worker.

If a reusable `Dmn_Pipe` or queue extension is pursued, first prove that it
atomically bounds admission, preserves reserved controls and cancellation,
and provides the required wakeup, priority, error and drain semantics. Do not
hide an unbounded pipe behind a bounded counter or use one pipe per session
if that changes the single serialized callback-worker contract. Keep existing
unbounded/default behavior and public API compatibility for existing users.
The discovery-specific ordered queues and barriers may remain layered above a
reusable transport; do not remove them until equivalent behavior is tested.

`Dmn_Timer` and scheduled `Dmn_Async` use scheduled pipes whose pending
deadlines may delay destruction. They are not direct substitutes for bounded,
cancellable per-source retries and the injectable fake clock. Retain the
current retry model unless an adaptation proves those properties. Likewise,
`Dmn_Inflight_Guard` tracks admitted API calls, not callback epochs, native
reference quotas or per-session drain barriers. `stringCompare` performs
Unicode folding and normalization, unlike the DNS ASCII case-folding rules;
do not substitute it for discovery identity comparison.

## 7. Limits, overflow and failure recovery

All limits are nonzero, configurable at client creation and validated before
work starts. Initial defaults are: 16 active browsers, 16 registrations,
64 instances per browser, 640 live native operation slots per browser,
32 addresses per instance, 64 TXT entries and 4096 aggregate TXT bytes per
instance, and 1024 queued data events per browser. Bound distinct SRV and TXT
records to 8 per kind per instance and bound each record before copying.
Instance labels are at most 63 encoded bytes;
DNS name and TXT entry limits also obey their respective protocol constraints.
An implementation may choose lower platform-specific limits only if it returns
an explicit error. These are Dmn-owned limits, not limits on a daemon's cache
or allocations inside native libraries. No credentials or TXT payloads are
logged by default.

One native operation slot counts one live browser, resolver, SRV/TXT record
subscription, address lookup or equivalent native reference, including a
temporarily cancelling reference until it is freed. On Linux, two browse
transports and two requested address families can require up to four live
resolvers plus four SRV/TXT subscriptions for a key; 64 such keys plus the
browser require at most 513 slots before temporary transitions. Reuse/free
old handles before replacement and reserve slots before constructing new ones.
The 640 default allows local transition headroom; it does not reserve daemon
resources or guarantee that system object/file-descriptor limits permit that
many operations. Handle native quota failures as explicit errors.

Bound all Dmn-owned handoffs:

- The state worker processes native callbacks directly; it does not use a
  second, unbounded native-event inbox. Limits and validation apply before
  copying native payloads. Allocations inside the daemon/native library and
  OS socket buffers are outside this bound.
- Use a bounded client command queue of 256 ordinary submissions. A full
  queue rejects new ordinary calls with `ResourceLimit`; `snapshot()`,
  registration `status()` and stop/reset/shutdown control paths have reserved
  capacity and cannot be starved by ordinary submissions. Control requests
  coalesce per handle, and concurrent callers wait on the same bounded
  operation rather than allocating unbounded internal waiter lists.
- Each browser has its bounded 1024-event data queue; each registration has
  a bounded 32-event queue. Each session also has an eight-event control queue,
  a separate emergency overflow-notification slot and an authoritative
  status/error latch. Daemon loss has room for both `BackendUnavailable` and
  `Reset` in order. Only one callback per client executes at a time; a slow
  callback can delay other sessions, but cannot expand their queues
  indefinitely.
- If an observation, native slot, record, address or event exceeds a browser
  limit, atomically mark it `ResourceLimit`/`Overflow`, invalidate its epoch
  and view, stop its native work, discard queued events and use its emergency
  slot for an overflow notification. A callback already running may finish
  first. The terminal status remains observable through `snapshot()` even if
  the callback fails. `clearCache()` explicitly attempts a restart; there is
  no automatic overflow retry.
- An allocation failure while mutating the browser model or preparing its
  event invalidates the entire view in the same way, rather than retaining
  a partially updated model or an unannounced delta. Allocation failure while
  applying reset or backend transitions likewise latches overflow; a failed
  reset still drains callbacks from older epochs. The emergency transition
  itself does not allocate; native objects are retired after callback stacks
  unwind. Native callback allocation failures retain `ResourceLimit` even if
  their failure notification cannot run; diagnostic text may be empty when
  memory is exhausted. Public event and registration-status conversion
  failures use the same allocation-free error detail and move the error into
  session status before callback cleanup.
- If a registration queue or slot overflows, withdraw that registration,
  discard its queued events, set a persistent overflow status and use its
  emergency overflow slot. `status()` reports it even if no callback can run.
  The overflow error and emergency transition are prepared without allocating
  during failure handling. A new registration is required to retry.
- Multiple pending control transitions must not silently overwrite an
  undelivered required transition. If they cannot fit, invalidate the
  affected session, discard its queued deltas and replace them with one
  overflow control notification plus terminal status. Recovery requires an
  explicit reset/new registration. Shutdown/stop bypass the queue, suppress
  pending notifications and preserve their drain guarantees.

Caller-owned snapshots and arbitrary concurrently blocked caller threads are
not library-owned queue memory. Backoff timers are bounded by the number of
admitted sources, not an unbounded retry task queue. Every queue, terminal
latch and reserved path requires a deterministic saturation test.

Avahi's `AVAHI_CLIENT_NO_FAIL` supports waiting for an absent Avahi daemon;
on documented disconnection, free associated objects and the client before
recreating it. Its documentation does not promise transparent recovery from
all D-Bus daemon restarts. The backend must report errors and use bounded
backoff for supported retries; retry waits must be interruptible by shutdown.
For `AVAHI_ERR_DISCONNECTED` and `AVAHI_ERR_NO_DAEMON`, notify the owner,
release the failed client on the state worker after its callback returns, and
recreate it using one pending timer with delays doubling from one second to a
maximum of thirty seconds. Reset the delay when the client reaches RUNNING.
Other client errors remain visible and are not assumed recoverable. Session
objects must be invalidated before the failed client is released.
The macOS backend must likewise report broken references and either rebuild
with a new epoch or remain visibly unavailable. No cross-platform recovery
guarantee is accepted until the relevant platform tests demonstrate it.

## 8. Backend mapping and portability

### Linux/Avahi

1. Discover `avahi-client` with pkg-config only when the option is enabled;
   its link metadata supplies `avahi-common` on distributions such as Ubuntu
   that do not ship a separate `avahi-common.pc`. Verify the resulting headers
   and libraries are usable. Construct the client with an owned poll context;
   handle immediate constructor callbacks and failure without exposing partial
   state.
2. Use service browse with explicit type, domain and interface and both
   supported query transports. Map `NEW` and `REMOVE` to per-transport
   contributions to the portable key. Reject results outside the requested
   type, domain or explicit interface. Cache exhaustion/all-for-now are backend
   progress hints, not global completion.
3. For each contribution, watch SRV and TXT using bounded
   `AvahiRecordBrowser` objects. For each requested address family, use a
   bounded `AvahiServiceResolver`, passing the contribution's interface and
   query protocol separately from the requested address family. Use
   `AVAHI_LOOKUP_NO_TXT` for these resolvers so an absent TXT record cannot
   prevent address resolution; the separate TXT browser supplies metadata.
   Copy and validate RDATA, host, port and address before native callback
   return. Decode record data correctly, including DNS name representation,
   rather than comparing raw pointer/string values. Resolver
   updates replace that source's previous address. Free failed operations and
   retry as in section 3.3; free all on removal, reset and shutdown. Filter
   late results by session, key and the relevant resolution or record-watch
   generation.
4. Model callback flags as optional provenance; cached/local/wide-area/
   multicast is observation metadata, not authentication.
5. Free browsers, resolvers and groups before freeing the disconnected client;
   stop/join the poll context before releasing callback-owned state. Define a
   consistent lock/worker order and test construction and teardown failures.

### macOS/Bonjour

1. Browse using `DNSServiceBrowse`, retaining interface index and add/remove
   events. Do not equate `MoreComing` with a permanent completion event.
2. Monitor SRV and TXT records with owned, interface-scoped
   `DNSServiceQueryRecord` references. Track add/remove and apply the bounded
   conflict and generation rules in section 3.3. Parse SRV RDATA safely and
   preserve TXT wire bytes. A batch or absence of `MoreComing` is not proof
   that all possible records have been seen.
3. Use the monitored singleton SRV's decoded target and network-to-host
   converted port directly for interface-scoped `DNSServiceGetAddrInfo`.
   This path does not wait for `DNSServiceResolve` and therefore does not
   depend on a TXT record arriving first. Manage address additions/removals;
   do not rely on an unscoped blocking `getaddrinfo()` for `.local` identity.
   Recreate the address lookup when the SRV target changes; callbacks from
   older generations cannot contribute addresses. Immediate/asynchronous
   failures use the same retry/cancellation rules as Linux.
4. Process references using the owned socket event loop and
   `DNSServiceProcessResult`; cancel/deallocate every browser, resolver,
   query, address lookup and registration exactly once.
   Validate local-network privacy behavior in a signed host application when
   applicable; CI success is not proof of permission in every distribution.

## 9. Build, packaging and documentation

- `ENABLE_DMN_DISCOVERY=OFF` introduces no Avahi, Bonjour, D-Bus, BlueZ,
  protobuf or Kafka discovery dependency. On Linux, `DMN_AVAHI_PROVIDER=AUTO`
  uses system Avahi development files when available, otherwise builds the
  pinned upstream client/common libraries if `DMN_ALLOW_DEPENDENCY_FETCH=ON`.
  `SYSTEM` requires system development files; `SOURCE` forces the source
  build. The source build requires D-Bus development files and make, and does
  not install or start an Avahi daemon. Missing prerequisites and disabled
  fetching produce actionable errors. The opt-in
  `DMN_BUILD_AVAHI_ADAPTER_TESTS=ON` can prepare this dependency for private
  adapter tests before the discovery target is available. On macOS, enabling
  discovery selects the native system API. Unsupported enabled configurations
  fail explicitly, rather than building a success-shaped no-op.
- Install the supported public header(s) and export discovery through `dmn`; propagate
  static-link dependencies correctly through `DmnConfig.cmake` only when
  needed. Do not export build-machine paths or leak native types into the API.
  Add feature information to the umbrella/header and standalone-header checks
  according to existing package conventions, and test a relocated downstream
  `find_package(Dmn CONFIG)` consumer with the option both on and off.
- **Packaging decision:** preserve the existing single `Dmn` package and its
  eager dependency discovery; do not introduce a component-aware export or a
  second package in this feature. Discovery is part of `dmn`, not a separate
  library. For a discovery consumer that must not require
  protobuf/Kafka, install Dmn with `ENABLE_DMN_DISCOVERY=ON` and
  `ENABLE_DMN_DMESGNET=OFF` (and other unrelated options off). The current
  `DmnConfig.cmake` still requires ICU and any dependencies enabled in that
  installed build. In particular,
  a default DMesgNet-enabled package requires protobuf and librdkafka at
  `find_package` time. Component-aware packaging is future work and
  would require a separate package-spec change.
- Keep `Dmn_Proc` in `dmn` and use it for the discovery workers. Preserve
  existing symbols and lifecycle behavior; do not introduce a second copy or
  a separate support library. Test installed, relocated consumers linking
  `Dmn::dmn` with DMesgNet off and protobuf and Kafka unavailable.
- Public classes, types, lifetime, thread-safety, errors, byte ownership and
  examples require Doxygen. Update the package specification, feature
  documentation and installed-header expectations as implementation lands.
  Provide a small diagnostic example or CLI only after the library works; it
  must not automatically connect to discovered services.

## 10. Test-driven development and acceptance matrix

Before each implementation slice, add focused failing tests for its observable
contract. Use a private controllable backend fake, fake clock/backoff and
explicit synchronization; never require sleeps or a host daemon for routine
unit tests. Tests assert externally visible events, snapshots, errors and
lifecycle outcomes, not backend implementation details.

| Area | Required deterministic tests |
| --- | --- |
| Configuration and input | Option off, unsupported platform, invalid type/domain/interface/port, malformed or oversized TXT, zero/overflow limits, missing dependency and no implicit daemon/network activity. |
| Browse and resolve | Initial empty snapshot; per-source address replacement and independent source retention; SRV target/port change invalidates old address generation, TXT-only change preserves addresses; root target and port zero are unusable; address removal; transient timeout then retry without another browse `NEW`; permanent error; refresh success, failure, supersession, absent key and cancellation; late callback and independent handles. |
| Identity and values | Same key via two Avahi transports produces one `Added`; removing one transport retains the key; last removal emits one `Removed`; Bonjour equivalent and distinct interfaces; unknown interface; literal dots and UTF-8; DNS case equivalence; IPv6 link-local scope; binary/empty/duplicate backend-reported TXT; Avahi rejects multi-entry advertisements containing empty entries but accepts an empty TXT record; network-to-host port conversion. |
| Record conflicts | Duplicate identical SRV/TXT across transports; absent TXT, zero-length TXT and single empty string; two distinct observed records, transient overlapping update, conflict clears after removal, old endpoint invalidation, singleton changes, malformed RDATA, per-record limits and late old-generation callbacks on both adapters. No test asserts complete network-wide enumeration. |
| Cache/reset | Clear populated/empty view, one reset before new-epoch events, immediate reappearance from fake daemon cache, no daemon-global clear, callback in flight, queued old callback suppression, stale resolution after clear, restart failure and unavailable clear. |
| Loss/recovery | Daemon absent at startup, available later, loss while resolving, invalidation without false removal, repeated loss, bounded/interruptible backoff, reappearance and fresh epoch; bus/daemon failure that cannot recover is explicitly surfaced. |
| Advertisement | Locally accepted vs established, collision without renaming, malformed advertisement, status and callback failure, withdraw/idempotent stop with no stopped callback, client shutdown, no impact on another handle, daemon loss/re-registration, late callback after withdrawal. |
| Concurrency and bounds | Callback before handle publication; snapshot/event sequencing; snapshot and refresh inside a callback racing a barrier; reentrant barrier rejection; simultaneous clear/stop/shutdown; handles outliving client; callback exception and blocked callback; 64 keys with two transports and two address families, slot admission/release, native quota failure, saturation of commands, data/registration/control queues, reserved stop path and status; no event after stop; no use-after-free or deadlock. |
| Packaging | Feature on/off, standalone header, relocated consumer with DMesgNet off and protobuf/Kafka unavailable; ordinary DMesgNet-enabled package still requires its dependencies; platform selection and actionable missing Avahi dependency failure. |

Backend contract tests run against each fake adapter. Add Linux Avahi-adapter
tests with controlled native shims or a private bus/daemon where practical,
including immediate callbacks and daemon restart. Add macOS DNSService-adapter
tests with controlled native shims, query/update/address lifecycle and port
byte order. Test each failure/cancellation cleanup path under Valgrind on
Linux where supported and sanitizers where available.

With `DMN_BUILD_AVAHI_ADAPTER_TESTS=ON`, the opt-in
`DMN_AVAHI_REAL_DAEMON_TEST=ON` adapter fixture uses separate user, mount, PID
and network namespaces, a private D-Bus session, a test-only Avahi daemon and
a dummy multicast interface. It requires unprivileged user namespaces, mount,
iproute2, D-Bus and Avahi daemon support. It does not modify the host daemon or
use the host network. Its native-seam checks are not a substitute for an
installed public-client test or cross-host qualification.

Real-network tests are **opt-in**, isolated and time-bounded: Linux
Avahi-to-Avahi, macOS Bonjour-to-Bonjour, and Linux-to-macOS on the same
multicast-capable link. Advertise a uniquely named test service, browse,
resolve, verify the expected port/TXT/address family, withdraw and observe
removal where the environment permits. Clean up only the test-owned service.
Report missing daemon, multicast, permissions or host privacy approval as a
skipped prerequisite, not a passing interoperability result. Routine CI
requires no installed/running Avahi daemon or Mac peer.

## 11. Implementation slices and release gates

This section is the execution plan for a coding agent. Phases 0–8 are ordered
prerequisites, not independent feature-completion claims. Each phase starts
by adding or extending focused tests that fail for the missing behavior,
implements only that behavior, then builds and runs its tests and relevant
earlier tests. Record exact commands, results and any environmental skips in
the implementation handoff. Do not commit failing tests, no-op backend stubs,
or a public target that pretends a partially implemented backend is complete.
Do not treat an unrun integration test as a pass.

### Common test/build rules

- Name the principal deterministic suite `dmn-test-discovery-core` and native
  adapter suites `dmn-test-discovery-avahi` and
  `dmn-test-discovery-bonjour`; register available tests with CTest labels
  `dmn` and `dmn-discovery`. Test names below describe behavior, not mandatory
  GoogleTest spelling. Use the existing `ADD_TEST_EXECUTABLE` and
  `ADD_TEST_VALGRIND` patterns where appropriate, and link each test to the
  actual target(s) it exercises. Test the installed `Dmn::dmn` target
  separately from the in-tree test macro.
- Until a complete native backend is ready, build the portable core and fake
  as private test-only code when `DMN_BUILD_TESTS=ON`. Their fake injection
  seam is private and is never installed or selected by the production
  factory. The public `ENABLE_DMN_DISCOVERY` option remains off by default;
  enabling an unfinished platform must fail configuration or be unavailable,
  not produce a success-shaped placeholder. Only export/install the target
  for platforms whose required implementation is actually present.
- For a Linux fake/core build, after the test target exists, an example
  targeted loop is:

  ```sh
  cmake -S . -B build-discovery -DCMAKE_BUILD_TYPE=Debug \
    -DDMN_BUILD_TESTS=ON -DENABLE_DMN_DMESGNET=OFF \
    -DENABLE_DMN_DISCOVERY=OFF
  cmake --build build-discovery --target dmn-test-discovery-core
  ctest --test-dir build-discovery -R '^dmn-test-discovery-core$' --output-on-failure
  ```

  The ordinary project still requires its configured dependencies, including
  ICU and GoogleTest; disabling discovery does not remove those requirements.
  Add platform options only after those options and targets exist. Do not
  infer that the fake suite needs a running daemon.
- Run the repository's targeted formatter/checks on changed code and
  `git diff --check`. The existing formatting script rewrites many source
  files; avoid unrelated formatting changes. Add the deterministic discovery
  suites to `ADD_TEST_VALGRIND` under `ENABLE_VALGRIND`, and run the relevant
  `valgrind-dmn-test-discovery-*` CTest entries on Linux when available.
- A fake or shim validates state-machine and adapter behavior, not real
  native API/network behavior. Test the latter separately in phases 6–8.

### Phase 0 — Baseline and contract seam

**Tests first:** verify feature-off configuration has no Avahi requirement;
add tests for valid/invalid options and names, empty/binary TXT, local
factory failure cleanup and construction without implicit browse or
advertisement. Add a private controllable backend fake and fake clock.

**Implement:** portable value types, validation and the private backend
boundary sufficient to run those tests; create a test-only core target.
Factory/API names in section 3 are sketches, but the stated ownership and
error behavior are requirements.

**Gate:** targeted tests and the feature-off build pass without a daemon.
No native support or installed library is claimed.

### Phase 1 — Browse identity and owned snapshots

**Tests first:** initial empty state; add/change/remove; duplicate contribution
idempotency; IPv4/IPv6 transport merging and last-contribution removal;
distinct interfaces; unknown scope; retained owned values; sequence/snapshot
ordering and a late callback after removal.

**Implement:** portable browser state, key comparison, contribution tracking,
event dispatch and owned snapshots against the fake. No native network work.

**Gate:** new and phase 0 tests pass, including a consumer that copies a key
from an event and uses it after the callback returns.

### Phase 2 — Record and address resolution

**Tests first:** singleton SRV and TXT; zero/empty/absent TXT equivalence;
binary TXT; root target and port zero; duplicate versus conflicting records;
conflict recovery; per-source address replacement and removal; IPv6 scope;
target change and stale-generation rejection; incomplete record monitoring.

**Implement:** bounded record sets, conflict state and per-source resolution
model. Check raw record bounds and DNS wire parsing in focused tests; do not
assume a fake's convenient representation matches native wire data.

**Gate:** phases 0–2 pass with fake-controlled ordering, including a service
whose TXT never appears. Do not claim a complete network inventory.

### Phase 3 — Retry and explicit refresh

**Tests first:** timeout then later success without a second browse `NEW`;
1-second/30-second bounded backoff with fake time; permanent error; refresh
success, deadline, supersession and cancellation; stale results after refresh;
no cache-flush or forced-network-query claim.

**Implement:** retry timers, refresh IDs, status transitions and cancellation.
Timers and retry work must be bounded by admitted sources.

**Gate:** phases 0–3 pass without real sleeps or a daemon; a timed-out refresh
stays failed when later background recovery updates the candidate.

### Phase 4 — Reset, failure and lifecycle

**Tests first:** reset barrier and event order with a callback in flight;
daemon loss/recovery, no fabricated remote removal; stop and shutdown during
work; post-shutdown snapshot; callback exception; snapshot/refresh from a
callback; rejected reentrant barriers; handles outliving their client.
Independently saturate command, browser data and control queues and native
slots; verify observable overflow and the reserved stop path. Registration
queues are tested in Phase 5, when registration sessions exist.

**Implement:** worker/callback ownership, generation checks, bounded queues,
error latches and drain barriers. Use synchronization gates in tests rather
than timing assumptions.

**Gate:** phases 0–4 pass; add core suite to Valgrind and run it on supported
Linux. No work or callback survives its documented stop boundary.

### Phase 4a — Dmn worker reuse and compatibility

This is a follow-up to completed Phase 4, before registration work. It does
not retroactively claim that the current private fake uses Dmn primitives.

**Tests first:** retain the existing Phase 4 lifecycle and saturation tests;
add deterministic worker-start failure and partial-start rollback, concurrent
shutdown, callback reentrancy, worker exception reporting and prompt
cooperative wake/join tests. Verify no queued waiter is stranded, no stale
callback begins after a barrier, and one session's callback failure does not
stop another. Test discovery and existing `Dmn_Proc` consumers through
`dmn`. Exercise both supported library linkage modes where
available.

**Implement:** adapt the two discovery workers to use the existing
`Dmn_Proc` in `dmn`. Keep bounded discovery queues and the separate callback
worker unless a tested reusable extension provides equivalent semantics. Any
proposed change to `Dmn_Proc`, `Dmn_Pipe`, or queue defaults must preserve
existing callers; add focused compatibility and failure-injection tests for
the shared primitive itself before adopting it. A more capable pipe is
optional and must not delay or weaken the stop/control paths.
`Dmn_Proc` relies on the POSIX initial cancellation state and type (enabled
and deferred) instead of repeating their setup before the task. This removes
the pre-task failure path without changing its object layout. Discovery's own
worker wrappers disable cancellation before admitting work. Worker construction
waits for that setup and reports a failure through the typed admission path,
rolling back any worker already started; failures after startup use the
serialized worker-failure paths.

**Gate:** existing proc, pipe, async, timer, queue and discovery tests pass,
including relevant fault-injection tests where configured and discovery
Valgrind tests. Test the existing `dmn` linkage and relocated discovery
consumer in static and shared builds. Native poll integration and macOS
execution remain phase 6/7 gates. Worker reuse is not complete until its
failure and lifecycle tests pass.

### Phase 5 — Registration contract

**Tests first:** valid and invalid advertisements; acceptance versus
establishment; conflict without rename; binary TXT; failure and callback
exception status; withdrawal, idempotent stop and post-shutdown status;
re-registration after daemon recovery; no stopped callback. Saturate the
registration event and control queues independently and verify the persistent
overflow status, withdrawal and reserved stop path.

**Implement:** registration state machine, fake backend operations and
bounded failure paths, independent of browsing and DMesgNet.

**Gate:** all fake/core tests pass. No native advertisement is claimed yet.

### Phase 6 — Linux Avahi adapter and packaging

**6a — Poll and lifetime.** First test immediate constructor callbacks,
command wakeup, absent daemon, native errors and prompt stop with controlled
native shims. Implement one owned `AvahiSimplePoll` worker and safe teardown.
Gate on the adapter suite plus phases 0–5.
The native poll limits each blocking wait to one second so that a failed
command-wake pipe write is observed even if it cannot wake an already
blocked poll; ordinary successful writes still wake it promptly.

**6b — Browse and resolve.** First test native `NEW`/`REMOVE` mapping,
transport-versus-address-family arguments, SRV/TXT wire parsing,
`AVAHI_LOOKUP_NO_TXT`, replacement, retry and stale callbacks. Implement
native browse, bounded record watches and resolvers. Gate on adapter and core
tests; use a private bus/daemon when available to check real initialization,
resource limits and daemon restart.
Avahi's set browse-result flags provide positive provenance; unset flags are
unknown. For merged live observations, each provenance field is true if any
source reports true, false only if every source reports false, and unknown
otherwise. Changes to the merged value produce a changed event.
Browser model preparation occurs before dispatcher admission. If the initial
unavailable event cannot be queued, the unpublished session is discarded and
its capacity released, including when admission is attempted from a callback.
After local admission, Avahi startup allocation failures retain the accepted
handle and latch resource overflow; startup exceptions are reported through
the handle's backend status. This also applies to nested admission from a
callback. If a disconnect occurs during startup, release all native objects
before the client can be freed and finish the pending loss transition even
when the newly admitted browser has overflowed.
Contribution teardown releases all native watches and resolvers before copying
session state or updating the model. An allocation failure in that remaining
cleanup is reported as a resource-limit error so the owner can invalidate the
session and release its operation leases. Repeated contribution cleanup
retains that allocation error rather than reporting success. On worker
teardown, backend failure or daemon loss, all browsers' native objects are
released before potentially allocating contribution cleanup and before the
Avahi client is freed.
If allocation fails during global backend-failure reporting, every live browser
and registration latches resource overflow, including sessions already visited
by the partial report. Command execution reports allocation failure without
allocating diagnostic text, so that failure alone does not kill the worker.

**6c — Advertisement and install.** First test entry-group creation,
immediate callbacks, binary TXT, collision, withdrawal and recovery, then
implement them. Add the real optional target, export and package tests. Run
the relocated downstream consumer with discovery on, DMesgNet off and
protobuf/Kafka unavailable; independently verify the feature-off build. The
consumer must link `Dmn::dmn` directly rather than rely on the in-tree test
macro. Exercise both system Avahi and forced source providers, including
their relocated package dependencies and applicable license notices. Register
and run the Avahi adapter's Valgrind test.

**Linux gate:** all previous deterministic tests and Linux adapter/package
tests pass. Run the opt-in real Avahi browse/advertise/withdraw test where its
prerequisites are available; record a skip otherwise. Without that real
test, report the Linux implementation as unqualified, not network-verified.

**Concurrency investigation, not a qualification:** on one Linux environment,
ThreadSanitizer passed ten deterministic discovery and ten controlled Avahi
test executables when ASLR was disabled for the test processes. Real-daemon
tests reported a D-Bus lock-order inversion during Avahi client creation and
destruction. The same warning reproduced with an isolated, single-threaded
Avahi-only client using system Avahi 0.8 and libdbus 1.14.10, without Dmn
code. This establishes an upstream-library reproduction, not that concurrent
clients are free of deadlock. With lock-order detection disabled solely to
check for data races, the real public and recovery tests also passed
ThreadSanitizer. An isolated real-daemon test also exercised four concurrent
clients, each creating and stopping three browser/client lifecycles; it passed
normally and under ThreadSanitizer with lock-order detection disabled. This
bounded test does not resolve the lock-order warning. In one Helgrind poll
test run, all 57 reported contexts involved command-future storage or test
data read after the corresponding command completed. A separate synchronized
future reproducer shows that Helgrind can miss that happens-before
relationship. This triage is limited to that run and does not establish race
freedom or complete Phase 4a.

### Phase 7 — macOS Bonjour adapter and packaging

**7a — Poll and lifetime.** First test readable/error socket handling,
reference ownership, asynchronous errors, wakeup, cancellation and no
double-deallocation. Implement DNSService socket polling on the state worker.

Current work includes a DNSService reference poller, browse, record-watch,
address-lookup and registration adapters, shared session integration, public
backend selection and a controlled native shim. Linux shim tests and a
simulated platform/package build pass, but these do not constitute a macOS
SDK build or macOS qualification. Registration failure-path and native
lifecycle qualification remain open.

**7b — Browse and resolve.** First test browse interface mapping; SRV/TXT
add/remove and byte-order parsing; TXT-less resolution from SRV;
`DNSServiceGetAddrInfo` add/remove; target changes, retry and stale
callbacks. Implement the native mapping and run the common fake suite plus
Bonjour adapter suite.

**7c — Advertisement and install.** First test `NoAutoRename`, network-byte
order registration port, collision, withdrawal and error recovery. Implement
native registration and package discovery; run the relocated Dmn consumer
with discovery enabled and DMesgNet disabled, plus feature-off tests.

**macOS CI integration.** Update
`.github/workflows/cmake-macos-platform.yml` after the Linux implementation:

1. Retain the existing macOS feature-off build and its `dmn` test suite, with
   the Homebrew ICU prefix and existing Linux-only features disabled. Use a
   separate build directory for discovery-on testing so its CMake cache cannot
   mask feature-off regressions.
2. Configure and build discovery on with `ENABLE_DMN_DISCOVERY=ON`, selecting
   the system Bonjour/DNSService API, not Avahi or Linux D-Bus. Run the common
   deterministic suite and the Bonjour shim/adapter suite on `macos-latest`;
   register their CTest labels so the workflow actually executes them.
3. In a separate discovery-only configuration, disable DMesgNet and unrelated
   optional integrations, install and relocate the package, and build and run
   a consumer linking `Dmn::dmn`. Verify configuration and
   consumption do not require protobuf or Kafka. Keep the existing ordinary
   package smoke test for the DMesgNet-enabled configuration.
4. Make Bonjour adapter tests portable to a Linux controlled-native shim and
   register/run them under the repository's Valgrind suite there; also run the
   tests against the macOS adapter build. If a test cannot be run under
   Valgrind, report the exact limitation rather than claiming that every new
   test passed memory checking.
5. Do not treat GitHub-hosted CI or shim-only results as proof of multicast,
   daemon availability or local-network privacy permission. Real-network tests
   remain opt-in with explicit prerequisites and results as in Phase 8.

**macOS gate:** all prior portable and Bonjour deterministic/package tests
pass on a macOS runner. Run the opt-in Bonjour test on an approved local
network; document OS version and applicable privacy setup. A shim-only or
skipped real-daemon test is not network qualification.

### Phase 8 — Product and interoperability qualification

Use the installed library from independent test applications on two distinct
hosts sharing a multicast-capable link. Run Linux→Linux, macOS→macOS and
Linux↔macOS, with a unique, test-owned `local.` service name. On each pair:

1. Explicitly advertise a known instance, nonzero port and binary TXT on one
   host; confirm the local registration reached established state.
2. Browse from the other host and assert the name/type/domain, interface,
   expected hostname/port/TXT and at least one usable address. Exercise
   refresh and local cache reset without claiming a forced network query.
3. Stop the test-owned registration and observe removal within the test's
   bounded timeout; clean up only test-owned objects. Where supported, run
   controlled daemon loss and recovery and verify the stated lifecycle.
4. Repeat with the roles reversed for cross-platform testing. Report
   environmental preconditions and observed results, not only CTest status.

The test demonstrates discovery and withdrawal, **not** application
connection, authenticated identity or guaranteed delivery. Publish a
cross-platform-complete claim only when phases 0–7 and all three real-network
pairings pass; otherwise identify precisely which gate was skipped or failed.
Keep manual DMesgNet configuration unchanged.

The opt-in `dmn-discovery-qualification` consumer in `test/cmake-package/`
can be built against an installed Dmn package on each host:

```sh
cmake -S test/cmake-package -B build-qualification \
  -DDmn_DIR=/path/to/installed/lib/cmake/Dmn \
  -DDMN_EXPECT_DISCOVERY=ON -DDMN_EXPECT_DMESGNET=OFF
cmake --build build-qualification --target dmn-discovery-qualification
```

Use the discovery-enabled, DMesgNet-disabled package and its normal platform
dependencies. Choose a unique instance name owned by the test and determine
each host's interface index and the advertiser's expected local hostname.
Start `dmn-discovery-qualification advertise INSTANCE INTERFACE` on the first
host; after it prints `ESTABLISHED`, start
`dmn-discovery-qualification browse INSTANCE HOSTNAME INTERFACE` on the
second. After the browser reports `READY`, press Enter on the advertiser;
the browser must report `REMOVED`. All waits have deadlines. The client
verifies port 12345, binary TXT, hostname, interface and a usable address,
requests refresh, and resets its own view. It never starts a service
implicitly. Both programs return failure for an unmet check or timeout.
Record both OS versions, interface and privacy configuration, outcomes and
any missing prerequisites. A single-host isolated-daemon test checks the
client itself but is not cross-host or macOS qualification.

### Phase 9 — Separate future DMesgNet integration

Before any automatic peer configuration, write and review a separate
specification and TDD plan for service identity/version, transport selection,
interface scope, bind/receive, peer registry and fan-out, authentication,
loss/reconnect and shutdown. Its acceptance must test actual DMesgNet
communication between nodes, not merely discovery of their advertised port.
It is not a prerequisite for implementing the reusable discovery library,
and its behavior is not warranted by phases 0–8.

If qualification exposes an unavailable semantic, do not fabricate success
or silently weaken the common contract: revise this specification and its
tests explicitly before implementation continues. Discovery does not provide
consensus or reliable delivery.

### Feasibility assessment and implementation risks

The design is implementable in principle using the cited native APIs and
Dmn's optional target/export and GoogleTest patterns, but this is a
**documentation review, not a working prototype or platform qualification**.
The following are explicit gates rather than assumed facts:

| Risk | Evidence and implementation gate |
| --- | --- |
| Poll integration and teardown | Avahi documents `AvahiSimplePoll` iteration and threaded wakeup; Bonjour exposes a daemon socket. Phases 6a/7a must demonstrate command wakeup, constructor/callback reentrancy, prompt cancellation and exactly-once cleanup with their actual API adapters. |
| Record data and live updates | Avahi exposes record-browser RDATA and Bonjour exposes `DNSServiceQueryRecord`; correct RDATA parsing, update/removal behavior and TXT-less resolution need native adapter tests and real-daemon tests. Never infer a complete record set from initial callbacks. |
| Capacity and daemon policy | The native operation count is locally bounded but daemon quotas and file descriptors can be lower; phase 6 must surface quota errors and phase 8 must record the tested settings. Defaults are not evidence that a deployment supports 64 simultaneously resolved instances. |
| Recovery and platform policy | Avahi's own documentation does not guarantee transparent D-Bus restart recovery; macOS privacy and daemon behavior vary. Tests and diagnostics must distinguish unavailable, retrying and permanent failure, and document the supported recovery actually observed. |
| Installation and independence | The package eagerly discovers dependencies of all enabled features. Phase 6c/7c must compile and link an installed, relocated Dmn consumer with DMesgNet off; fake in-tree tests are not sufficient. |
| Dmn primitive reuse | `Dmn_Proc` lives in `dmn`, while `Dmn_Pipe` uses unbounded queues by default. Phase 4a must preserve bounds, cooperative shutdown and existing users when changing the worker implementation. |
| Discovery versus DMesgNet | Phase 8 proves only discovery and withdrawal. Automatic DMesgNet peer connectivity is expressly deferred to phase 9 and cannot be inferred from a discovered hostname and port. |

## 12. Design decisions and tradeoffs

- **Portable identity over transport-specific identity.** Merge IPv4 and IPv6
  query-transport observations for the same interface-scoped service. Keeping
  separate transport keys would expose distinctions Bonjour does not report;
  merging requires per-contribution accounting, but avoids fabricated values
  and duplicate portable services.
- **Detect observed conflicts rather than pick one record.** Watch SRV and TXT
  on both backends and withhold endpoints while distinct records overlap.
  Selecting the first record would use fewer native objects and be simpler,
  but could quietly expose an arbitrary endpoint. The conservative choice
  costs additional daemon objects and can briefly mark a valid service
  unavailable during a normal update. It detects only records actually
  observed, not every record on the network; full multiple-record selection
  and load balancing remain out of scope.
- **Bounded failure rather than silent truncation.** Stopping a session on
  local overflow makes loss visible and keeps memory bounded. An alternative
  is an incomplete rolling cache, but it needs an explicit partial-inventory
  contract and eviction policy that this API does not provide. Limits are
  configurable, and a native daemon can enforce lower limits.
- **Explicit refresh rather than implied freshness.** A per-key refresh avoids
  clearing all browser state and reports an attempt ID, but the system cache
  may still answer it. A guarantee of an on-wire query or real-time
  reachability is unavailable through these portable APIs.
- **Preserve the current package export.** Building with DMesgNet off provides
  discovery without protobuf/Kafka, but the Dmn package still requires ICU and
  other enabled features' dependencies. Splitting exports or introducing
  CMake components would let a consumer of a multi-feature installation avoid
  unrelated dependencies, at the cost of a broader package redesign and
  compatibility work; it is deliberately not part of this phase.
- **Terminal status instead of a stopped callback.** A queryable stopped
  status and a callback-drain barrier are unambiguous even if a callback
  throws or a handler is blocked. A final stopped callback would make `stop()`
  wait for delivery and complicate callback reentrancy; no such event is
  promised.
