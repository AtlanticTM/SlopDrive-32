<script>
  /**
   * PairingPane.svelte — the knock-and-approve ceremony, AND the joiner's
   * side of it, both reachable now.
   *
   * The protocol has had all of this working for a while: a joiner sends
   * PAIR_REQ, the hub parks it in a bounded pending list, publishes that list
   * as ordinary STATE, and any `configure` session approves or denies it over
   * the session-admin INTENT. That half shipped first and is unchanged below
   * (see `decide()`). What was missing is the OTHER half: this client itself
   * never sent a PAIR_REQ, so a browser stuck at `watch`/`control` had no way
   * to become the joiner and ask for `configure` — the exact gap this file
   * now closes.
   *
   * ── Why /uitoken can never do this ──────────────────────────────────────
   *
   * `/uitoken` mints a single-use, short-lived `control` credential to
   * anything on the LAN that can HTTP GET — that is deliberate (see
   * `validateToken` in src/comms/SlopSyncHubService.cpp): a credential
   * anything can mint must never be a PERMANENT one, and `configure` is
   * permanent. The ONLY route to `configure` is a pairing ceremony (RFC-027),
   * because a ceremony requires something /uitoken cannot forge: either an
   * operator who already holds `configure` approving you, or physical
   * possession of the machine's power cord.
   *
   * ── The three ceremonies (RFC-027 §12.2), and which this pane can start ──
   *
   *   (a) knock-and-approve — this client sends a bare PAIR_REQ (no proof),
   *       the hub parks it, and it sits until ANY `configure` session
   *       approves or denies it over 0x0009. `sendPairReq()` below is
   *       ALWAYS this shape — this pane never types or shows a PIN.
   *   (b) PIN proof — a keyboard ceremony this pane does not implement.
   *   (c) push-to-pair — a PHYSICAL-PRESENCE proof (three quick power-cycles,
   *       see `checkQuickBootPairingGesture()` in SlopSyncHubService.cpp)
   *       opens a 120s window in which the FIRST bare knock — the exact same
   *       PAIR_REQ this pane already sends for mode (a) — is granted with NO
   *       approval needed. Whether a knock lands as (a) or (c) is decided
   *       entirely by the hub's window state at the moment it arrives; this
   *       pane cannot request one over the other.
   *
   * ── Channel discovery ──────────────────────────────────────────────────────
   *
   * The 0x000A/0x0009 admin surface is located by SPEC-CORE NAME, never by a
   * literal id — these channels are part of the protocol itself and exist on
   * every conforming hub, so binding to their names is portable in exactly
   * the way binding to a device's own channel numbers is not. The pending
   * list requires `configure` to even subscribe, so a `watch`/`control`
   * session never sees it (correct — see the locked-state note below); the
   * live tier/pairing-modes/window-open signals this pane's OWN ceremony
   * relies on instead come off session-level events (welcome/pairGrant/event)
   * that exist for any session, at any tier.
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { runAction } from '../model/shadow.svelte.js';
  import { ACCESS, ACCESS_NAME, NACK, bytesEqual, getInstanceId, setPairedToken } from '../../../../SlopSync/clients/js/index.js';
  import { PAIRING_MODE, PAIRING_MODE_NAME, PAIRING_EVENT_KIND } from '../../../../SlopSync/clients/js/frames.js';

  const entryNamed = (n) => machine.catalog.entries.find((e) => e.name === n) || null;

  const pendingEntry = $derived(entryNamed('pending-pairing'));
  const adminEntry = $derived(entryNamed('session-admin'));
  const sample = $derived(pendingEntry ? machine.samples[pendingEntry.id] : null);

  // ── live tier + pairing-surface state, mirrored from session-level events ──
  //
  // Why this pane keeps its OWN copy instead of reading `machine.link.roles`:
  // hub_impl.hpp upgrades a session's role IN PLACE the instant PAIR_GRANT is
  // sent — no reconnect required — and this pane has to reflect that the
  // moment it happens (GROUND TRUTH: never show a stale tier once the hub has
  // said otherwise). `session.state.roles` is updated immediately by
  // session.js's PAIR_GRANT handler; `liveRoles` here just mirrors it into a
  // Svelte rune so the template reacts to it. Other cards catch up to the new
  // tier on their own next WELCOME (a reconnect), which is a narrower
  // staleness window than this pane can fix on its own — it does not touch
  // model/machine.svelte.js, which other cards read from.
  let liveRoles = $state(machine.link.roles | 0);
  let pairingModes = $state(0);
  let liveWindowOpen = $state(false);
  let boundSession = null;

  $effect(() => {
    // Reactive retrigger: if this pane mounts before connect() has created a
    // session, `getSession()` is null and this effect must run again once the
    // link progresses — reading `machine.link.phase` here is what makes that
    // happen, exactly the way canAdminister already depends on machine.link.
    void machine.link.phase;
    const s = getSession();
    if (!s || s === boundSession) return;
    boundSession = s;

    const syncRoles = () => { liveRoles = s.state.roles || 0; };
    syncRoles();

    const offWelcome = s.on('welcome', (w) => {
      syncRoles();
      pairingModes = w.pairingModes || 0;
      // WELCOME's bitmask is a snapshot at connect time; window_opened/closed
      // EVENTs (below) keep it live for the rest of the session.
      liveWindowOpen = (pairingModes & PAIRING_MODE.push_to_pair) !== 0;
    });
    const offGrant = s.on('grant', syncRoles);
    const offPairGrant = s.on('pairGrant', (g) => { syncRoles(); onPairGrant(g); });
    const offEvent = s.on('event', (evt) => {
      if (evt.channelName !== 'pairing-events') return;
      if (evt.kind === PAIRING_EVENT_KIND.window_opened) liveWindowOpen = true;
      else if (evt.kind === PAIRING_EVENT_KIND.window_closed) liveWindowOpen = false;
      onPairingEvent(evt);
    });
    const offNack = s.on('nack', (n) => {
      if (n.code === NACK.PAIRING_REQUIRED || n.code === NACK.PAIRING_DENIED || n.code === NACK.BUSY) {
        onPairingNack(n);
      }
    });

    return () => {
      offWelcome(); offGrant(); offPairGrant(); offEvent(); offNack();
      if (boundSession === s) boundSession = null;
    };
  });

  const tierName = $derived(ACCESS_NAME[liveRoles] || String(liveRoles));
  const modesOffered = $derived(
    [PAIRING_MODE.knock_approve, PAIRING_MODE.pin_proof, PAIRING_MODE.push_to_pair]
      .filter((bit) => (pairingModes & bit) !== 0)
      .map((bit) => PAIRING_MODE_NAME[bit])
  );

  const canAdminister = $derived(
    adminEntry ? liveRoles >= (adminEntry.access | 0) : false
  );

  // Either signal proves the same underlying hub state; only one is usually
  // reachable at a time (0x000A needs `configure`, so a not-yet-paired
  // session only ever has the WELCOME/0x000B route — exactly the case this
  // pane exists to serve).
  const windowOpenFromRoster = $derived(!!(sample && sample.flags_bits && sample.flags_bits.window_open));
  const windowOpen = $derived(windowOpenFromRoster || liveWindowOpen);

  /** The op-select field and its option labels, read off the catalog. */
  const opField = $derived(adminEntry && adminEntry.schema
    ? adminEntry.schema.find((f) => f.options && f.options.length) : null);
  const opIndex = (label) => (opField && opField.options.indexOf(label)) ?? -1;

  /** Schema keys by name, so we never hardcode a CBOR key number. */
  const keyOf = (name) => {
    const f = adminEntry && adminEntry.schema
      ? adminEntry.schema.find((x) => x.name === name) : null;
    return f ? f.key : null;
  };

  /**
   * The hub publishes each pending slot as a flat run of fields suffixed by
   * slot index — the packed-layout way of expressing an array. Walk them by
   * index rather than assuming a count.
   */
  const knocks = $derived.by(() => {
    if (!sample) return [];
    const out = [];
    const count = sample.count | 0;
    for (let i = 0; i < count; i++) {
      const lo = sample['inst_lo' + i], hi = sample['inst_hi' + i];
      if (lo === undefined || hi === undefined) break;
      out.push({
        slot: i,
        lo, hi,
        name: sample['name' + i] || '(unnamed client)',
        kind: sample['kind' + i],
        expires: sample['expires_s' + i],
      });
    }
    return out;
  });

  // ── control ownership + eviction ("Connected clients", the reachable slice) ─
  //
  // There is no session ROSTER on this hub (0x0002 is allocated in the
  // registry but no catalog builder — device or library — declares it, and a
  // live probe against this machine confirms it never appears on the wire).
  // What the protocol genuinely offers today is control-owner (0x0004,
  // `watch`-visible, spec-core): which session, if any, owns each of the
  // fixed arbiter sources. Its field names (src0/owner0..src3/owner3) are as
  // safe to reference literally as pending-pairing's slot fields above — same
  // spec-core guarantee, same idiom.
  //
  // Eviction is session-admin's `evict` op (RFC-018) — the SAME 0x0009
  // channel and op-select already driving decide() above, because the
  // library treats "who may do what" (approve/deny/evict/revoke) as one
  // admin surface, not four. Reusing canAdminister/opIndex/keyOf here is
  // therefore not a shortcut, it is the correct model: one tier gate for the
  // whole surface.
  //
  // Absent channel -> nothing to draw (guarded by `{#if ownerEntry}` below),
  // the same graceful-degrade every hero widget already does when its roles
  // are absent.
  const ownerEntry = $derived(entryNamed('control-owner'));
  const ownerSample = $derived(ownerEntry ? machine.samples[ownerEntry.id] : null);

  const owners = $derived.by(() => {
    if (!ownerSample) return [];
    const out = [];
    for (let i = 0; i < 4; i++) {
      const src = ownerSample['src' + i];
      const owner = ownerSample['owner' + i];
      if (src === undefined || owner === undefined) break;
      out.push({ src, owner });
    }
    return out;
  });

  let evictBusy = $state(null);
  let evictResult = $state(null);

  async function evict(sessionId) {
    const s = getSession();
    if (!s || !adminEntry) return;
    const op = opIndex('evict');
    if (op < 0) { evictResult = { ok: false, msg: 'this hub does not offer eviction' }; return; }
    evictBusy = sessionId;
    evictResult = null;
    const action = { channelId: adminEntry.id, key: keyOf('op'), label: 'evict session' };
    const res = await runAction(action, op, { [keyOf('session_id')]: sessionId });
    evictResult = res.ok
      ? { ok: true, msg: 'evicted session ' + sessionId }
      : { ok: false, msg: res.error || 'refused' };
    evictBusy = null;
  }

  let busy = $state(null);
  let result = $state(null);

  /** instance_id travels as an 8-byte bstr; the layout splits it into two u32s. */
  function instanceBytes(k) {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setUint32(0, k.lo >>> 0, true);
    new DataView(b.buffer).setUint32(4, k.hi >>> 0, true);
    return b;
  }

  async function decide(k, approve) {
    const s = getSession();
    if (!s || !adminEntry) return;
    const op = opIndex(approve ? 'pair_approve' : 'pair_deny');
    if (op < 0) { result = { ok: false, msg: 'this hub does not offer that operation' }; return; }
    busy = k.slot;
    result = null;
    // session-admin is a spec-core admin surface, not a device-chosen
    // `action.*` role — this ad-hoc action descriptor is the same shape
    // runAction() expects from buildSettingsModel's actions[], per its own
    // doc comment. Approving at `control` is the safe default: it lets a
    // client drive the machine without letting it hand out credentials of
    // its own. instance_id (+ role, when approving) ride the SAME intent as
    // `op` — one atomic admin decision, not three.
    const action = { channelId: adminEntry.id, key: keyOf('op'), label: approve ? 'approve pairing' : 'deny pairing' };
    const extraFields = { [keyOf('instance_id')]: instanceBytes(k) };
    if (approve) extraFields[keyOf('role')] = ACCESS.control;
    const res = await runAction(action, op, extraFields);
    result = res.ok
      ? { ok: true, msg: (approve ? 'approved ' : 'denied ') + k.name }
      : { ok: false, msg: res.error || 'refused' };
    busy = null;
  }

  // ── this client's OWN knock: send, then watch for the answer ────────────

  let claiming = $state(false);
  let claimResult = $state(null); // {ok: true|false|null, msg}
  let claimTimer = null;

  function ownKnockEvent(evt) {
    const id = evt.body && evt.body.instance_id;
    return id instanceof Uint8Array && bytesEqual(id, getInstanceId());
  }

  function finishClaim(res) {
    claiming = false;
    if (claimTimer) { clearTimeout(claimTimer); claimTimer = null; }
    claimResult = res;
  }

  function onPairingEvent(evt) {
    if (!claiming) return;
    if (evt.kind === PAIRING_EVENT_KIND.knocked && ownKnockEvent(evt)) {
      // Still pending — just now CONFIRMED delivered (0x000B is a broadcast,
      // so this is the earliest proof the hub actually queued it).
      claimResult = { ok: null, msg: 'knock delivered — waiting for an operator to approve it, or for the window to close.' };
    } else if (evt.kind === PAIRING_EVENT_KIND.denied && ownKnockEvent(evt)) {
      finishClaim({ ok: false, msg: 'denied by an operator.' });
    } else if (evt.kind === PAIRING_EVENT_KIND.expired && ownKnockEvent(evt)) {
      finishClaim({ ok: false, msg: 'the knock expired unanswered after 120s — nobody approved it.' });
    }
  }

  function onPairingNack(n) {
    if (!claiming) return;
    const msg = n.code === NACK.PAIRING_REQUIRED
      ? 'refused: knock-and-approve is disabled on this hub, and no pairing window is open right now.'
      : n.code === NACK.BUSY
        ? 'refused: the pending-knock list is full — try again shortly.'
        : 'refused: ' + n.name + (n.detail ? ' — ' + n.detail : '');
    finishClaim({ ok: false, msg });
  }

  function onPairGrant(g) {
    if (g.token) setPairedToken(getSession().host, g.token);
    const roleName = g.role != null ? (ACCESS_NAME[g.role] || String(g.role)) : 'a higher tier';
    finishClaim({ ok: true, msg: 'paired at ' + roleName + ' — this browser will use it automatically from now on, including after a reload.' });
  }

  function startClaim() {
    const s = getSession();
    if (!s) return;
    claimResult = null;
    const sent = s.sendPairReq();
    if (!sent) { claimResult = { ok: false, msg: 'not connected yet — try again once the link is live.' }; return; }
    claiming = true;
    if (claimTimer) clearTimeout(claimTimer);
    // Just past the hub's own 120s pending-knock window (limits::pairing_window_default_s):
    // a genuinely silent outcome (nothing lost, nobody home) resolves by then.
    claimTimer = setTimeout(() => {
      if (claiming) finishClaim({ ok: false, msg: 'no response within the pairing window. Nobody approved it, or no window was open — see the instructions below.' });
    }, 125000);
  }
</script>

<section class="pairing">
  <header>
    <h2>Pairing</h2>
    {#if windowOpen}
      <span class="badge open">association window open</span>
    {/if}
  </header>

  <div class="tier">
    <p>
      This session is at <strong>{tierName}</strong>.
      {#if liveRoles >= ACCESS.configure}
        It already holds <strong>configure</strong> — nothing below is needed.
      {:else}
        Pairing is the <strong>only</strong> way to reach <strong>configure</strong> on this
        machine — the <code>/uitoken</code> bootstrap a browser normally gets is deliberately
        capped at <strong>control</strong>, because it is a credential anything on the LAN can
        mint, and a credential like that must never be permanent.
      {/if}
    </p>
    <p class="modes">
      This hub currently offers:
      {#if modesOffered.length}
        {#each modesOffered as m, i}<code>{m}</code>{i < modesOffered.length - 1 ? ', ' : ''}{/each}
      {:else}
        <span class="note">nothing right now</span>
      {/if}
      {#if !modesOffered.includes('push_to_pair')}
        <span class="note"> — no physical-presence window is currently open.</span>
      {/if}
    </p>
  </div>

  {#if liveRoles < ACCESS.configure}
    <div class="claim">
      <button class="pair-btn" disabled={claiming} onclick={startClaim}>
        {claiming ? 'Waiting for a response…' : 'Pair this client'}
      </button>
      {#if claimResult}
        <p class="result" class:bad={claimResult.ok === false} class:pending={claimResult.ok === null} role="status">
          {claimResult.msg}
        </p>
      {/if}

      <details class="howto">
        <summary>How to open a pairing window (claims <strong>configure</strong> with no approval needed)</summary>
        <ol>
          <li>Power the machine off, then on again — <strong>three times in a row</strong>, each
            power-cycle starting within about 10 seconds of the previous boot. (Let it run
            normally for more than ~10s between cycles and the count resets — start over.)</li>
          <li>On the third quick cycle, the machine opens a 120-second pairing window and lights
            its pairing indicator.</li>
          <li>With this page connected (it will reconnect on its own after each power-cycle),
            click <strong>Pair this client</strong> above within those 120 seconds.</li>
        </ol>
        <p class="note">
          If nobody has ever paired <strong>configure</strong> on this machine before, the very
          first knock inside that window becomes <strong>configure</strong> automatically —
          possession of the power cord is treated as ownership. If this machine has already been
          claimed once, the same window instead grants <strong>control</strong>, and reaching
          <strong>configure</strong> needs an existing configure session to approve your knock
          below via <code>session-admin</code> instead.
        </p>
      </details>
    </div>
  {/if}

  {#if !pendingEntry || !adminEntry}
    <p class="note">This hub does not advertise a pairing surface.</p>

  {:else if !canAdminister}
    <!-- The honest version. An empty list here would read as "nobody is
         knocking", when the truth is "you are not allowed to be told". -->
    <div class="locked">
      <p class="lead">This session cannot approve OTHER clients' pairings.</p>
      <p>
        Approving requires the <strong>configure</strong> tier, which this session does not
        currently hold — see above for how to claim it.
      </p>
    </div>

  {:else if !knocks.length}
    <p class="note">
      Nobody is waiting. When a client knocks it appears here with its name and
      the way it asked, and stays for the length of the pairing window.
    </p>

  {:else}
    <ul class="knocks">
      {#each knocks as k (k.slot)}
        <li>
          <div class="who">
            <span class="name">{k.name}</span>
            <span class="meta">expires in {k.expires}s</span>
          </div>
          <div class="acts">
            <button class="deny" disabled={busy !== null} onclick={() => decide(k, false)}>Deny</button>
            <button class="approve" disabled={busy !== null} onclick={() => decide(k, true)}>Approve</button>
          </div>
        </li>
      {/each}
    </ul>
  {/if}

  {#if result}
    <p class="result" class:bad={!result.ok} role="status">{result.msg}</p>
  {/if}
</section>

{#if ownerEntry}
  <section class="ownership">
    <header>
      <h2>Control ownership</h2>
    </header>

    {#if !owners.length}
      <p class="note">No motion source is currently owned by any session.</p>
    {:else}
      <ul class="owners">
        {#each owners as o (o.src)}
          <li>
            <div class="who">
              <span class="src mono">source {o.src}</span>
              {#if o.owner}
                <span class="sess mono">session {o.owner}</span>
                {#if o.owner === machine.link.sessionId}<span class="you">this session</span>{/if}
              {:else}
                <span class="unowned">unowned</span>
              {/if}
            </div>
            {#if o.owner}
              <div class="acts">
                <button class="deny" disabled={!canAdminister || evictBusy !== null}
                        title={!canAdminister ? 'Requires the configure tier to evict a session' : ''}
                        onclick={() => evict(o.owner)}>
                  {evictBusy === o.owner ? 'Evicting…' : 'Evict'}
                </button>
              </div>
            {/if}
          </li>
        {/each}
      </ul>
    {/if}

    {#if evictResult}
      <p class="result" class:bad={!evictResult.ok} role="status">{evictResult.msg}</p>
    {/if}
  </section>
{/if}

<style>
  .pairing, .ownership { background: var(--bg-card); border: 1px solid var(--line); border-radius: var(--r); padding: var(--gap); }
  .ownership { margin-top: var(--gap); }
  header { display: flex; align-items: center; gap: var(--gap); margin-bottom: var(--gap); }
  h2 { font-size: 1rem; font-weight: 500; }
  .badge.open { font-size: .75rem; color: var(--good); border: 1px solid var(--good); border-radius: 999px; padding: 1px 8px; }
  .note, .locked p { color: var(--ink-dim); font-size: .875rem; }
  .lead { color: var(--ink); font-weight: 500; }
  .locked p + p { margin-top: .5rem; }
  code { font-family: var(--mono); font-size: .85em; }
  .tier { font-size: .875rem; margin-bottom: var(--gap); }
  .tier p { margin: 0; }
  .tier p + p { margin-top: .35rem; }
  .tier .modes { color: var(--ink-dim); }
  .claim { margin-bottom: var(--gap); padding-bottom: var(--gap); border-bottom: 1px solid var(--line-soft); }
  .pair-btn { min-height: var(--tap); border-radius: var(--r-s); border: 1px solid var(--line);
              background: color-mix(in srgb, var(--good) 12%, transparent); padding: 0 16px; }
  .pair-btn:disabled { opacity: .7; }
  .howto { margin-top: .6rem; font-size: .8rem; color: var(--ink-dim); }
  .howto summary { cursor: pointer; color: var(--ink); }
  .howto ol { margin: .5rem 0 .5rem 1.1rem; padding: 0; }
  .howto li + li { margin-top: .3rem; }
  .knocks { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
  .knocks li { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; justify-content: space-between;
               background: var(--bg-raised); border: 1px solid var(--line-soft); border-radius: var(--r-s); padding: 10px; }
  .name { font-weight: 500; }
  .meta { color: var(--ink-faint); font-size: .8rem; margin-left: 8px; font-family: var(--mono); }
  .acts { display: flex; gap: 8px; }
  .acts button { min-height: var(--tap); min-width: 88px; border-radius: var(--r-s); border: 1px solid var(--line); }
  .approve { background: color-mix(in srgb, var(--good) 18%, transparent); border-color: var(--good); }
  .deny { background: transparent; color: var(--ink-dim); }
  .result { margin-top: var(--gap); font-size: .875rem; color: var(--good); }
  .result.bad { color: var(--bad); }
  .result.pending { color: var(--ink-dim); }

  .owners { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
  .owners li { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; justify-content: space-between;
               background: var(--bg-raised); border: 1px solid var(--line-soft); border-radius: var(--r-s); padding: 10px; }
  .owners .who { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
  .owners .src { color: var(--ink-dim); }
  .owners .sess { color: var(--ink); }
  .owners .you { font-size: .75rem; color: var(--good); border: 1px solid var(--good); border-radius: 999px; padding: 1px 8px; }
  .owners .unowned { color: var(--ink-faint); font-size: .875rem; }
</style>
