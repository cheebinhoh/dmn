<!-- Copyright © 2026 Chee Bin HOH. All rights reserved. -->

# Explicit mDNS-to-DMesgNet Integration

**Status: Phase 9.1 implementation complete; Phase 9.2 authentication and
cross-process qualification remain.**

This specification defines the separate Phase 9 feature that may connect
`Dmn_DiscoveryClient` observations to `Dmn_DMesgNet`. Discovery remains a
local, unauthenticated observation. A discovered hostname, port or TXT record
is never sufficient by itself to create a DMesgNet connection.

## 1. Scope and non-goals

The feature provides an explicit opt-in coordinator that:

- is shipped as a reusable Dmn library component for CLIs and other clients;
- accepts owned discovery events from a caller-owned
  `Dmn_DiscoveryClient` browser;
- filters observations by a caller-selected service type, domain and
  interface;
- validates an application-owned service identity, protocol version and
  transport metadata from untrusted TXT data;
- creates and retires transport sessions through an injected factory;
- provides an explicit environment-trusted TCP/framed adapter for deployments
  that already enforce network isolation;
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
contract. It also does not provide a CLI, `main()` function, argument parser,
configuration file format, logging policy or a cryptographically authenticated
transport.

### 1.1 Delivery profiles

Phase 9 is delivered in two deliberately small profiles:

1. **Environment-trusted profile (Phase 9.1).** The library connects to the
   validated mDNS address and port using a length-prefixed TCP stream, then
   constructs `Dmn_DMesgNet` over that stream. No cryptographic authentication
   is performed. The profile is explicit, reports `ConnectedUnverified`, and
   is suitable only when the deployment already enforces network isolation and
   node admission outside Dmn.
2. **Authenticated profile (Phase 9.2).** A future adapter reuses the same
   coordinator/session boundary and performs TLS/mTLS or another approved
   handshake before creating `Dmn_DMesgNet`. It reports authenticated
   readiness only after the handshake and authorization policy succeed.

The environment-trusted profile is operationally usable in a controlled
network, but it is not cryptographically secure. mDNS metadata is still
untrusted and spoofable.

## 2. Proposed boundary

The coordinator is the separate public library type
`Dmn_DMesgNetDiscovery`, rather than a constructor mode on
`Dmn_DMesgNet`. It owns:

1. one explicit event-consumption boundary for a caller-owned
   `Dmn_DiscoveryClient` browser;
2. one bounded peer registry keyed by the discovery service key;
3. one injected `Dmn_DMesgNet` transport-session factory;
4. one mutex-serialized caller control path for discovery events, session
   state and retry; the coordinator does not own an unbounded event queue;
5. one owned status snapshot and ordered status callback.

The public header exposes only Dmn value types, standard-library ownership and
the injected factory contract. No implementation may expose Avahi, Bonjour,
socket or TLS-specific types in the public header.

### 2.1 Library boundary

The installed library target is the product. A client integrates it by:

1. creating a `Dmn_DiscoveryClient` and browser;
2. constructing `Dmn_DMesgNetDiscovery` with a policy and authenticated
   session factory;
3. forwarding browser events to the coordinator;
4. consuming coordinator status and owning application behavior.

The client may be a CLI, daemon or GUI. It owns presentation and
configuration; the library owns admission, lifecycle and resource bounds.
The coordinator must not create a process-global singleton, read environment
variables, parse command-line arguments, write logs, or start a native mDNS
backend implicitly.

### 2.2 Session factory contract

The injected factory receives an immutable, validated endpoint containing
only copied values. It is responsible for:

- authenticating the remote peer using an application-selected mechanism;
- creating the concrete `Dmn_Io<std::string>` transport;
- constructing and returning a session that owns the associated
  `Dmn_DMesgNet`;
- reporting whether a failure is transient or permanent for retry policy.

The coordinator must never infer authentication from mDNS TXT data. The
factory must not retain references to the candidate or endpoint arguments.
Factory failure details are caller-supplied diagnostics and must be sanitized;
credentials or other secrets must never be placed in them.

`Dmn_DMesgNetDiscovery::EnvironmentTrusted::factory()` is the reference
Phase 9.1 factory. It owns a connected TCP socket, uses a four-byte
network-order frame length followed by the serialized DMesgNet payload, and
passes the resulting `Dmn_Io<std::string>` to `Dmn_DMesgNet`. It never reports
the session as authenticated.

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
- the library can be used without constructing a CLI or native mDNS backend;
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
- the factory is the only component allowed to choose a concrete transport;
- transport construction failure reports an admission failure without a
  leaked session;
- authenticated handshake failure retires the peer and records the reason;
- successful session construction wires both I/O directions to
  `Dmn_DMesgNet`;
- the environment-trusted adapter connects to a real loopback TCP endpoint,
  exchanges framed payloads, and shuts down without leaking the socket;
- inbound and outbound frames remain bounded by the configured limits of the
  returned `Dmn_DMesgNet` session; the coordinator owns no frame queue;
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
2. **Library-consumer gate:** a small in-tree test client links the installed
   Dmn target, injects a discovery event, creates a fake authenticated
   session, and verifies that no CLI or native backend is required.
3. **Phase 9.1 loopback gate:** a deterministic TCP listener and the
   environment-trusted factory exchange framed DMesgNet payloads over
   loopback, verify `ConnectedUnverified`, loss, retry and shutdown.
4. **Linux process gate:** two independent processes communicate through a
   loopback transport with an injected discovery result and verify message
   exchange, loss, retry and shutdown.
5. **Linux real-network gate:** two hosts advertise and consume the selected
   mDNS service, then establish authenticated DMesgNet communication. Record
   the transport, interface, authentication setup and packet path.
6. **Linux-to-macOS gate:** repeat the real-network test with one host on each
   platform. A discovery-only `READY` result is not sufficient.
7. **macOS-to-macOS gate:** repeat with two macOS hosts when available.

GitHub-hosted CI may run the deterministic suites and single-host process
tests. It must not claim any real-network gate unless the job explicitly has
two reachable hosts and reports the required environment.

## 5. Implementation order

1. Fix the public library boundary and session-factory contract.
2. Add fake discovery, fake transport and fake clock fixtures.
3. Add the admission, identity and lifecycle tests as the first failing
   contract tests.
4. Implement the coordinator with bounded control and peer state.
5. Implement and test the environment-trusted TCP/framing adapter.
6. Add retry, backpressure, authentication-boundary and shutdown tests.
7. Add the Linux loopback process test.
8. Add opt-in real-network applications only after the deterministic and
   process gates pass.
9. Keep Phase 8 mDNS qualification scripts independent; they validate
   discovery and withdrawal, not this feature's transport communication.

