<!-- GENERATED FILE. DO NOT EDIT. Source: docs-site/dictionary.yaml via docs-site/tools/gen_docs_tables.py -->
<!-- Appended to every page by pymdownx.snippets.auto_append, so a
     defined term shows its Dictionary definition on hover. -->

*[capability discovery]: Learning what a machine can do by reading its catalog, rather than from a separate feature list.
*[conformance profile]: A named subset of the specification an implementation may claim to satisfy.
*[Constrained client]: A client that ships a compiled-in catalog and pre-encoded templates instead of a CBOR parser.
*[in-process binding]: The transport binding that connects a hub and a client inside one process, with injected faults and a deterministic mode.
*[recognized-pending]: The rule that an observed client-version change drops a paired device to recognized-pending until re-approved.
*[setting categories]: The tab a settings field belongs in, chosen from a registered list so placement is consistent across hubs.
*[synthetic sessions]: A hub-side session object that wraps a legacy text-protocol edge so it obeys the same rules as a native client.
*[knock and approve]: The primary pairing mode, a client knocks with no proof, and any configure session approves it.
*[knock-and-approve]: The primary pairing mode, a client knocks with no proof, and any configure session approves it.
*[served-page token]: A single-use token the hub mints for its own served web page, so that page reaches control without a pairing ceremony.
*[slopsync_probe.py]: The reference verifier, which runs a scripted session against a hub and prints a pass-or-fail transcript per stage.
*[synthetic session]: A hub-side session object that wraps a legacy text-protocol edge so it obeys the same rules as a native client.
*[welcome signature]: An optional hub signature over client-supplied entropy, proving you reached the machine you paired with.
*[idempotency ring]: The hub's per-session record of the last 32 intent ids and the echoes they produced.
*[sequence numbers]: A per-channel, per-direction counter that lets a receiver reject anything older than what it already holds.
*[setting category]: The tab a settings field belongs in, chosen from a registered list so placement is consistent across hubs.
*[source ownership]: The rule that each arbiter source has at most one owning session at a time.
*[change tripwire]: The rule that an observed client-version change drops a paired device to recognized-pending until re-approved.
*[channel classes]: One of STATE, STREAM, INTENT, EVENT or STORE.
*[parser totality]: The requirement that every parser maps any byte string to accept or reject, with no crash in between.
*[pending pairing]: The bounded list of unanswered pairing knocks, published as ordinary protocol state.
*[sequence number]: A per-channel, per-direction counter that lets a receiver reject anything older than what it already holds.
*[configure tier]: The highest access tier; a configure session changes device configuration and administers pairing.
*[golden vectors]: A byte-exact recorded frame that every implementation must encode and decode identically.
*[motion anomaly]: A device-authored event naming what the motion core had to do differently, and why.
*[never-shed set]: The traffic that is never dropped under congestion, intents, echoes, NACKs, grants, ESTOP, and safety state.
*[priority class]: The shedding rank of a subscription, from background up to critical.
*[publish grants]: The hub's applied answer to a client's wish to send on an inbound STREAM channel.
*[retained value]: The latest value of a STATE channel, which the hub keeps and pushes immediately on grant.
*[CATALOG_READY]: The rule that a session's frames are refused until it confirms which catalog it has adopted.
*[Catalog entry]: One channel's description inside the catalog, encoded as an independent self-delimiting document.
*[MotionArbiter]: The single component that commands the motor driver for positioning.
*[active source]: The motion arbiter input currently driving motion.
*[channel class]: One of STATE, STREAM, INTENT, EVENT or STORE.
*[control plane]: The frames that negotiate, command and confirm, carrying CBOR map payloads.
*[control-plane]: The frames that negotiate, command and confirm, carrying CBOR map payloads.
*[golden vector]: A byte-exact recorded frame that every implementation must encode and decode identically.
*[liveness ping]: A tiny frame a client sends during silence to prove it is still there.
*[publish grant]: The hub's applied answer to a client's wish to send on an inbound STREAM channel.
*[registry.yaml]: The single machine-readable file that decides every number SlopSync puts on the wire.
*[setting flags]: A per-field annotation marking a setting advanced, restart-required, or secret.
*[stroke window]: The operator-set lower and upper position bounds that motion stays inside.
*[Access level]: The tier a session holds, which gates what channels it may read and write.
*[Ground truth]: The doctrine that a client never displays machine state that differs from the device's, in either direction.
*[catalog_etag]: The first eight bytes of a SHA-256 over the deterministically encoded catalog, naming exactly which catalog a hub exposes.
*[control tier]: The middle access tier; a control session drives the machine and may own a motion source.
*[ground truth]: The doctrine that a client never displays machine state that differs from the device's, in either direction.
*[idle reaping]: Closing a non-owning session that has gone silent, to reclaim its slot.
*[precondition]: The expected cfg_gen a client attaches to an intent, turning it into a compare-and-swap.
*[push-to-pair]: The pairing mode where physical presence, rather than a proof or an approval, opens a short single-grant window.
*[safety cause]: Why a stop or an e-stop is latched, user, deadman, fault, relay, or session loss.
*[setting flag]: A per-field annotation marking a setting advanced, restart-required, or secret.
*[token bucket]: The ingress rate limiter, a bucket that refills at the granted sample rate and holds one burst's worth of tokens.
*[trust ledger]: The hub's stored list of paired devices, their roles, and how each one presents its token.
*[wire numbers]: Any value that appears on the wire and must therefore mean the same thing to every implementation.
*[RFC process]: The way a change to SlopSync is proposed, argued and either bound into the specification or refused.
*[`configure`]: The highest access tier; a configure session changes device configuration and administers pairing.
*[field roles]: A text tag on a catalog field that says what the value semantically is.
*[instance id]: Eight bytes a client generates once and persists, saying who it durably is.
*[instance_id]: Eight bytes a client generates once and persists, saying who it durably is.
*[scope trace]: A self-describing JSONL capture of one motion session, plus the header that makes it readable forever.
*[setting_key]: The catalog annotation naming which key of the paired INTENT channel writes this displayed field.
*[stream kind]: The catalog property saying whether a STREAM sample reports a value at an instant, or commands a time extent.
*[stream_kind]: The catalog property saying whether a STREAM sample reports a value at an instant, or commands a time extent.
*[wire number]: Any value that appears on the wire and must therefore mean the same thing to every implementation.
*[Conflation]: Keeping at most one queued unsent frame per channel and subscriber, so a newer snapshot replaces an older one.
*[conflation]: Keeping at most one queued unsent frame per channel and subscriber, so a newer snapshot replaces an older one.
*[data plane]: The frames that carry machine values at rate, carrying packed struct payloads.
*[data-plane]: The frames that carry machine values at rate, carrying packed struct payloads.
*[decimation]: What the hub drops, and in what order, when a link cannot carry everything granted.
*[field role]: A text tag on a catalog field that says what the value semantically is.
*[post-clamp]: Replacing a requested value with the nearest value the machine's limits allow.
*[ready gate]: The rule that a session's frames are refused until it confirms which catalog it has adopted.
*[session_id]: A random non-zero 32-bit number the hub assigns to one association, unique within a hub boot.
*[watch tier]: The lowest access tier; a watch session observes state and may still stop the machine.
*[SlopScope]: The motion-pipeline oscilloscope, which graphs asked against planned against achieved.
*[`control`]: The middle access tier; a control session drives the machine and may own a motion source.
*[conflated]: Keeping at most one queued unsent frame per channel and subscriber, so a newer snapshot replaces an older one.
*[fuzz gate]: The continuous-integration job that feeds mutated input to every parser and fails on any memory-safety error.
*[limit set]: A named group of speed, acceleration and jerk ceilings the arbiter selects per source.
*[publishes]: The hub's applied answer to a client's wish to send on an inbound STREAM channel.
*[simulator]: A desktop binary that behaves like a machine, embedding the real hub, motion engine and catalog behind a real transport.
*[slopscope]: The motion-pipeline oscilloscope, which graphs asked against planned against achieved.
*[Registry]: The single machine-readable file that decides every number SlopSync puts on the wire.
*[Takeover]: Re-issuing an activating intent with the takeover flag set, to transfer source ownership.
*[channels]: A named, numbered, typed data flow declared in the catalog.
*[clamping]: Replacing a requested value with the nearest value the machine's limits allow.
*[dead-man]: The silence window bound to an active source, after which the source's loss policy fires.
*[decimate]: What the hub drops, and in what order, when a link cannot carry everything granted.
*[eviction]: The hub closing a session it has decided it cannot keep.
*[registry]: The single machine-readable file that decides every number SlopSync puts on the wire.
*[retained]: The latest value of a STATE channel, which the hub keeps and pushes immediately on grant.
*[segments]: One STREAM data point that commands a time extent, carrying its own duration.
*[sessions]: The stateful association between one client and the hub.
*[shedding]: What the hub drops, and in what order, when a link cannot carry everything granted.
*[takeover]: Re-issuing an activating intent with the takeover flag set, to transfer source ownership.
*[teardown]: The single code path every session end runs, whatever killed the session.
*[waveform]: The planner mode that turns one timed segment into one quintic over exactly the commanded duration.
*[Arbiter]: The single component that commands the motor driver for positioning.
*[Catalog]: The hub's machine-readable description of every channel it exposes.
*[Channel]: A named, numbered, typed data flow declared in the catalog.
*[Deadman]: The silence window bound to an active source, after which the source's loss policy fires.
*[Pairing]: The ceremony that proves a client should be trusted, ending in a token and a role.
*[Quintic]: A fifth-order polynomial trajectory matched to position, velocity and acceleration at both ends.
*[Session]: The stateful association between one client and the hub.
*[`watch`]: The lowest access tier; a watch session observes state and may still stop the machine.
*[arbiter]: The single component that commands the motor driver for positioning.
*[boot_id]: A random 32-bit number the hub generates at every boot, naming which incarnation of the hub you are talking to.
*[bundles]: One STREAM frame carrying a base timestamp and up to 32 samples.
*[catalog]: The hub's machine-readable description of every channel it exposes.
*[cfg_gen]: A 16-bit generation counter that increments whenever applied configuration content changes.
*[channel]: A named, numbered, typed data flow declared in the catalog.
*[clamped]: Replacing a requested value with the nearest value the machine's limits allow.
*[clients]: Any endpoint that establishes a session with the hub.
*[deadman]: The silence window bound to an active source, after which the source's loss policy fires.
*[evicted]: The hub closing a session it has decided it cannot keep.
*[intents]: A channel class, and the only way a client changes anything.
*[latched]: A safety condition that stays true in state until something explicitly clears it.
*[pairing]: The ceremony that proves a client should be trusted, ending in a token and a role.
*[quintic]: A fifth-order polynomial trajectory matched to position, velocity and acceleration at both ends.
*[segment]: One STREAM data point that commands a time extent, carrying its own duration.
*[session]: The stateful association between one client and the hub.
*[Client]: Any endpoint that establishes a session with the hub.
*[E-stop]: Emergency stop, an immediate driver-level stop that latches and prohibits motion until explicitly cleared.
*[INTENT]: A channel class, and the only way a client changes anything.
*[STREAM]: A channel class carrying timestamped sample bundles, in either direction.
*[Shadow]: The client-side replica of subscribed state, maintained exclusively from hub frames.
*[bundle]: One STREAM frame carrying a base timestamp and up to 32 samples.
*[client]: Any endpoint that establishes a session with the hub.
*[e-stop]: Emergency stop, an immediate driver-level stop that latches and prohibits motion until explicitly cleared.
*[grants]: The hub's applied answer to a subscription request, which channel, at what rate, at what priority.
*[intent]: A channel class, and the only way a client changes anything.
*[relays]: A forwarding node between the hub and clients on a transport the hub cannot reach directly.
*[shadow]: The client-side replica of subscribed state, maintained exclusively from hub frames.
*[ESTOP]: Emergency stop, an immediate driver-level stop that latches and prohibits motion until explicitly cleared.
*[EVENT]: A channel class carrying discrete occurrences, edges, not levels.
*[Grant]: The hub's applied answer to a subscription request, which channel, at what rate, at what priority.
*[PAUSE]: Suspend the pattern generator at a safe phase and park position.
*[Relay]: A forwarding node between the hub and clients on a transport the hub cannot reach directly.
*[STATE]: A channel class carrying idempotent full snapshots of a coherent group of fields.
*[STORE]: A channel class declaring a set of numbered slots holding opaque documents.
*[blobs]: An opaque byte document the protocol transfers in chunks and never decodes.
*[chase]: The planner mode that replans to each newly arrived point from the machine's own sampled state.
*[clamp]: Replacing a requested value with the nearest value the machine's limits allow.
*[grant]: The hub's applied answer to a subscription request, which channel, at what rate, at what priority.
*[latch]: A safety condition that stays true in state until something explicitly clears it.
*[relay]: A forwarding node between the hub and clients on a transport the hub cannot reach directly.
*[Blob]: An opaque byte document the protocol transfers in chunks and never decodes.
*[CBOR]: A compact binary encoding of maps, arrays and numbers, used here in a deterministic profile.
*[ECHO]: The hub's mandatory, truthful reply to an intent, carrying the values actually in effect after clamping.
*[Etag]: The first eight bytes of a SHA-256 over the deterministically encoded catalog, naming exactly which catalog a hub exposes.
*[HOLD]: Decelerate, then actively hold position, with the source suspended.
*[Jerk]: The rate of change of acceleration.
*[STOP]: A controlled stop, decelerate to zero at the configured rate and deactivate the source.
*[blob]: An opaque byte document the protocol transfers in chunks and never decodes.
*[etag]: The first eight bytes of a SHA-256 over the deterministically encoded catalog, naming exactly which catalog a hub exposes.
*[hubs]: The single authoritative endpoint of one machine.
*[jerk]: The rate of change of acceleration.
*[Hub]: The single authoritative endpoint of one machine.
*[hub]: The single authoritative endpoint of one machine.
