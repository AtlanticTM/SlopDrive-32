<script>
  /**
   * PairingPane.svelte — the knock-and-approve ceremony, finally reachable.
   *
   * The protocol has had all of this working for a while: a joiner sends
   * PAIR_REQ, the hub parks it in a bounded pending list, publishes that list
   * as ordinary STATE, and any `configure` session approves or denies it over
   * the session-admin INTENT. Every piece shipped. Nothing ever RENDERED it, so
   * the MFP plugin's knock went into a void and the operator saw nothing.
   *
   * ── Why this pane can be honest about being unusable ───────────────────────
   *
   * The pending-pairing channel requires `configure` to even subscribe to, and
   * a browser that bootstrapped on /uitoken gets `control` — deliberately, so a
   * credential anything on the LAN can mint cannot hand out permanent ones.
   * That means this pane is often NOT usable, and the correct behaviour is to
   * say so and explain the way out, rather than render an empty list that looks
   * like "nobody is asking".
   *
   * ── Channel discovery ──────────────────────────────────────────────────────
   *
   * Located by SPEC-CORE NAME, never by a literal id. These channels are part
   * of the protocol itself and exist on every conforming hub, so binding to
   * their names is portable in exactly the way binding to a device's own
   * channel numbers is not.
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { ACCESS } from '../core/slopsync/index.js';

  const entryNamed = (n) => machine.catalog.entries.find((e) => e.name === n) || null;

  const pendingEntry = $derived(entryNamed('pending-pairing'));
  const adminEntry = $derived(entryNamed('session-admin'));
  const sample = $derived(pendingEntry ? machine.samples[pendingEntry.id] : null);

  const canAdminister = $derived(
    adminEntry ? (machine.link.roles | 0) >= (adminEntry.access | 0) : false
  );

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

  const windowOpen = $derived(!!(sample && sample.flags_bits && sample.flags_bits.window_open));

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
    const fields = { [keyOf('op')]: op, [keyOf('instance_id')]: instanceBytes(k) };
    // Grant the tier the operator chose. Approving at `control` is the safe
    // default: it lets a client drive the machine without letting it hand out
    // credentials of its own.
    if (approve) fields[keyOf('role')] = ACCESS.control;
    try {
      await s.sendIntent(adminEntry.id, fields);
      result = { ok: true, msg: (approve ? 'approved ' : 'denied ') + k.name };
    } catch (e) {
      result = { ok: false, msg: (e && (e.name || e.message)) || 'refused' };
    } finally {
      busy = null;
    }
  }
</script>

<section class="pairing">
  <header>
    <h2>Pairing</h2>
    {#if windowOpen}
      <span class="badge open">association window open</span>
    {/if}
  </header>

  {#if !pendingEntry || !adminEntry}
    <p class="note">This hub does not advertise a pairing surface.</p>

  {:else if !canAdminister}
    <!-- The honest version. An empty list here would read as "nobody is
         knocking", when the truth is "you are not allowed to be told". -->
    <div class="locked">
      <p class="lead">This session cannot approve pairings.</p>
      <p>
        Approving requires the <strong>configure</strong> tier. A browser that
        bootstrapped over <code>/uitoken</code> is granted <strong>control</strong>
        on purpose — a credential anything on the network can mint must not be
        able to issue permanent ones.
      </p>
      <p>
        To claim configure on a machine that has never issued it, open a
        physical-presence window at the machine and pair from this client while
        it is open. Possession of the hardware is the root credential.
      </p>
      {#if knocks.length}
        <p class="note">{knocks.length} client(s) are waiting, but their details are withheld at this tier.</p>
      {/if}
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

<style>
  .pairing { background: var(--bg-card); border: 1px solid var(--line); border-radius: var(--r); padding: var(--gap); }
  header { display: flex; align-items: center; gap: var(--gap); margin-bottom: var(--gap); }
  h2 { font-size: 1rem; font-weight: 500; }
  .badge.open { font-size: .75rem; color: var(--good); border: 1px solid var(--good); border-radius: 999px; padding: 1px 8px; }
  .note, .locked p { color: var(--ink-dim); font-size: .875rem; }
  .lead { color: var(--ink); font-weight: 500; }
  .locked p + p { margin-top: .5rem; }
  code { font-family: var(--mono); font-size: .85em; }
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
</style>
