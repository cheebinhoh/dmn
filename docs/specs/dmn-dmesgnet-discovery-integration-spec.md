<!-- Copyright © 2026 Chee Bin HOH. All rights reserved. -->

# Explicit mDNS-to-DMesgNet Integration

**Status: design and TDD planning; no automatic peer connection is
implemented.**

This specification defines the separate Phase 9 feature that may connect
`Dmn_DiscoveryClient` observations to `Dmn_DMesgNet`. Discovery remains a
local, unauthenticated observation. A discovered hostname, port or TXT record
is never sufficient by itself to create a DMesgNet connection.

## 1. Scope and non-goals

The feature provides an explicit opt-in coordinator that:

- filters discovery observations by a caller-selected service type, domain and
  interface;
- validates an application-owned service identity, protocol version and
  transport metadata from untrusted TXT data;
- creates and retires transport sessions through an injected factory;
- feeds serialized DMesgNet frames through the existing `Dmn_Io<std::string>`
  boundary;
- reports peer admission, readiness, loss, retry, authentication failure and
  shutdown as owned status values;
- preserves bounded queues, cooperative shutdown and the existing
  `Dmn_DMesgNet` election behavior.

The first implementation does not add implicit peer discovery to
`Dmn_DMesgNet`, change its wire format, or make mDNS an authentication
mechanism. It does not define application-level authorization, consensus,
leader election changes, or reliable delivery beyond the existing DMesgNet
contract.

## 2. Proposed boundary

The coordinator is a separate public type, tentatively named
`Dmn_DMesgNetDiscovery`, rather than a constructor mode on
`Dmn_DMesgNet`. It owns:

1. one explicit `Dmn_DiscoveryClient` browser;
2. one bounded peer registry keyed by the discovery service key;
3. one injected `Dmn_DMesgNet` transport-session factory;
4. one serialized control path for discovery events, session state and retry;
5. one owned status snapshot and ordered status callback.

The final name and exact value types are fixed only after the fake-transport
tests below pass. No implementation may expose Avahi, Bonjour, socket or
TLS-specific types in the public header.

Each accepted service must provide:

- a unique instance identity;
- an exact supported protocol version;
- an explicit transport scheme and endpoint;
- a stable node identity;
- an authentication credential or verifier input that is not treated as
  trustworthy merely because it came from TXT.

The coordinator must reject missing, duplicate, malformed, oversized or
unsupported metadata before creating a transport session.

## 3. TDD test matrix

Tests use a controllable discovery fake, fake clock/backoff, fake transport
factory and fake `Dmn_Io<std::string>`. No test depends on multicast, Avahi,
Bonjour, DNS, a broker or wall-clock sleeps.

### 3.1 Admission and identity

- discovery is ignored until the coordinator is explicitly started;
- service type, domain and interface filters are enforced;
- a candidate with incomplete SRV/TXT/address state is not admitted;
- valid metadata admits exactly one peer;
- duplicate events and equivalent transport observations do not create a
  second session;
- conflicting node identity, protocol version, endpoint or credentials are
  rejected and surfaced;
- TXT size, entry-count and field-length limits are enforced;
- removal before session creation leaves no registry entry or retry task.

### 3.2 Transport and DMesgNet lifecycle

- the factory receives only validated, owned endpoint data;
- transport construction failure reports an admission failure without a
  leaked session;
- authenticated handshake failure retires the peer and records the reason;
- successful session construction wires both I/O directions to
  `Dmn_DMesgNet`;
- inbound and outbound frames remain bounded by the configured limits;
- a peer is not marked ready until the transport and DMesgNet readiness
  contract is satisfied;
- discovery removal closes the session exactly once;
- a stale session cannot close a newer session for the same key.

### 3.3 Loss, retry and reordering

- transient transport loss schedules bounded retry with fake time;
- permanent authentication or metadata errors do not retry indefinitely;
- a newer candidate supersedes an older endpoint generation;
- late discovery, transport and retry callbacks are ignored by generation;
- duplicate retry requests coalesce;
- retry admission failure is observable and does not starve shutdown;
- peer loss does not fabricate a discovery removal event.

### 3.4 Capacity, backpressure and shutdown

- peer, session, command, status and frame limits are independently bounded;
- admission is rejected with a resource-limit status at capacity;
- one peer's blocked callback or full queue cannot stop another peer;
- shutdown rejects new admission, cancels retries, closes sessions and drains
  accepted callbacks before returning;
- concurrent clear, removal, loss, retry and shutdown have deterministic
  ownership and ordering;
- a callback cannot re-enter a barrier operation;
- shutdown remains safe when a discovery handle outlives the coordinator.

### 3.5 Security and protocol handling

- mDNS values are parsed as untrusted input;
- no connection is attempted without an explicit caller policy;
- unsupported schemes, versions, identities and authentication modes fail
  closed;
- malformed frames, oversized frames and invalid peer identities are rejected;
- credentials are not logged or copied into diagnostic status;
- successful transport connection does not imply application authorization.

## 4. Integration gates

1. **Deterministic gate:** all tests in section 3 pass on Linux and macOS
   workflows with no native mDNS daemon requirement.
2. **Linux process gate:** two independent processes communicate through a
   loopback transport with an injected discovery result and verify message
   exchange, loss, retry and shutdown.
3. **Linux real-network gate:** two hosts advertise and consume the selected
   mDNS service, then establish authenticated DMesgNet communication. Record
   the transport, interface, authentication setup and packet path.
4. **Linux-to-macOS gate:** repeat the real-network test with one host on each
   platform. A discovery-only `READY` result is not sufficient.
5. **macOS-to-macOS gate:** repeat with two macOS hosts when available.

GitHub-hosted CI may run the deterministic suites and single-host process
tests. It must not claim any real-network gate unless the job explicitly has
two reachable hosts and reports the required environment.

## 5. Implementation order

1. Add fake discovery, fake transport and fake clock fixtures.
2. Add the admission, identity and lifecycle tests as the first failing
   contract tests.
3. Implement the coordinator with bounded control and peer state.
4. Add retry, backpressure, authentication and shutdown tests.
5. Add the Linux loopback process test.
6. Add opt-in real-network applications only after the deterministic and
   process gates pass.
7. Keep Phase 8 mDNS qualification scripts independent; they validate
   discovery and withdrawal, not this feature's transport communication.

