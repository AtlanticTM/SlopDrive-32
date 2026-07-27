<script>
  /**
   * SafetyBar.svelte — the persistent, always-reachable safety controls.
   *
   * Discovers what to render entirely from the catalog: any INTENT action
   * whose RFC-019 role starts with `action.safety` or `action.home` lands
   * here, identified by role — never by channel id (CLAUDE.md 3 / the task
   * contract). The safety-intents channel's `op` field is a single select
   * schema field whose `.options` carries every op the hub knows (RFC-025b):
   * each option is a distinct wire value and renders as its own button.
   *
   * E-STOP REACHABILITY: the protocol makes `stop`/`estop` role-exempt — any
   * connected session, including a bare watch-tier viewer, may fire them. We
   * never guess which ops are exempt; `session.canUse(channelId, key, value)`
   * asks the catalog's own per-option `option_access` (RFC-009 key 17), the
   * exact same data the hub gates on, so this bar and the hub cannot disagree
   * about what a given session may press.
   *
   * E-STOP RECOVERY: a refusal whose NACK is ESTOP_ACTIVE gets a standing
   * recovery hint offering the safety channel's `estop_clear` op — found by
   * the registry's own SAFETY_OP.estop_clear index into that same action's
   * `.options`, so the button's label is always whatever THIS hub calls it,
   * never a string we invented.
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { runAction } from '../model/shadow.svelte.js';
  import { NACK, NACK_NAME, SAFETY_OP } from '../core/slopsync/index.js';
  import { optionLabel } from '../model/format.js';

  const roleActions = $derived(
    ((machine.catalog.model && machine.catalog.model.actions) || []).filter(
      (a) => typeof a.role === 'string' && (a.role.startsWith('action.safety') || a.role.startsWith('action.home'))
    )
  );

  /**
   * THE E-STOP MUST NOT DEPEND ON AN OPTIONAL ANNOTATION.
   *
   * `action.*` roles are how DEVICE-DEFINED verbs get discovered, and that is
   * the right mechanism for them. But safety-intents is a SPEC-CORE channel:
   * every conforming hub has it, and RFC-025b makes its stop/estop ops
   * role-exempt precisely so anyone connected can halt the machine. Finding it
   * by role alone means a hub that simply never annotated it loses its e-stop
   * button — which is what happened the first time this ran against a
   * simulator whose catalog omitted the role. A missing garnish must never
   * cost the emergency stop.
   *
   * So: locate it by its protocol identity (the spec-defined channel name),
   * and treat any role tag as an additional discovery path rather than the
   * only one.
   */
  const specSafety = $derived.by(() => {
    const e = machine.catalog.entries.find((x) => x.name === 'safety-intents');
    if (!e || !e.schema) return null;
    const f = e.schema.find((x) => x.options && x.options.length);
    if (!f) return null;
    return {
      uid: e.id + ':' + f.key, channelId: e.id, channelName: e.name,
      key: f.key, name: f.name, label: f.name, desc: f.desc || '',
      role: 'action.safety', options: f.options,
      optionAccess: f.optionAccess || null,
      access: f.access != null ? f.access : e.access,
    };
  });

  // De-duplicate: if the hub DID annotate its safety channel, the role-derived
  // action and the spec-derived one are the same field.
  const actions = $derived(
    specSafety && !roleActions.some((a) => a.uid === specSafety.uid)
      ? [specSafety, ...roleActions]
      : roleActions
  );
  const safetyAction = $derived(actions.find((a) => a.role === 'action.safety') || null);

  // The registry-defined safety op that clears a latch. SAFETY_OP is the
  // registry's own enum (core/slopsync/frames.js), so naming a member of it is
  // protocol vocabulary — portable to every conforming hub — not knowledge of
  // this particular machine.
  const RECOVERY_OP = SAFETY_OP.estop_clear ?? null;

  const linkUp = $derived(machine.link.phase === 'live');

  /** May THIS session fire this exact op, per the catalog's own access data? */
  function canFire(action, value) {
    // Reading these keeps the check reactive to auth/catalog changes even
    // though the session object itself lives outside Svelte's reactivity.
    void machine.link.roles; void machine.link.phase; void machine.catalog.ready;
    const session = getSession();
    return !!session && session.isLive && session.canUse(action.channelId, action.key, value);
  }

  function reasonFor(action, value) {
    if (!linkUp) return 'no hub link';
    if (!canFire(action, value)) return 'this session is not authorised for this op';
    return '';
  }

  let busy = $state({});
  let lastResult = $state(null); // { ok, label, error, at }
  let estopActive = $state(false);

  async function fire(action, value, label, btnKey) {
    busy = { ...busy, [btnKey]: true };
    const result = await runAction(action, value);
    busy = { ...busy, [btnKey]: false };
    lastResult = { ok: result.ok, label, error: result.error || null, at: Date.now() };
    if (!result.ok) {
      if (result.error === NACK_NAME[NACK.ESTOP_ACTIVE]) estopActive = true;
    } else {
      estopActive = false;
    }
  }

  /**
   * Option-select INTENT fields are index-aligned with their wire value, and
   * the registry's op tables all start numbering at 1 — so index 0 exists only
   * to keep the array aligned and carries a placeholder label ("reserved").
   * Rendering it produces a button that means nothing and, if pressed, earns a
   * NACK.
   *
   * Filtered by LABEL rather than by "always drop index 0", because a
   * device-defined action channel is entitled to use 0 as a real operation.
   * Worst case of the heuristic is that a button labelled "reserved" stays
   * hidden, which is what anyone would want anyway.
   *
   * The cleaner fix is machine-side: gate the placeholder with an
   * `option_access` level nobody holds, the way 0x0009 session-admin already
   * does. Noted in docs/webui-architecture.md.
   */
  const PLACEHOLDER = /^(reserved|none|unused|n\/a|-|—)$/i;

  /**
   * Order by the access each op REQUIRES, lowest first.
   *
   * This is not cosmetic. RFC-025b makes stop and estop role-exempt, and the
   * catalog encodes that as an `option_access` of `watch` — the lowest tier
   * there is. So sorting ascending by required access puts the emergency ops
   * at the head of the row on every conforming hub, without this component
   * knowing which ops those are.
   *
   * It fixes a real defect: authoring order put `estop` sixth, which on a
   * phone left it off the right-hand edge of a horizontally scrolling bar. The
   * one control that must always be reachable was the one you had to go
   * looking for.
   */
  function optionButtons(action) {
    const floor = action.access | 0;
    const accessOf = (i) => {
      const a = action.optionAccess && action.optionAccess[i];
      return a == null ? floor : a;
    };
    return (action.options || [])
      .map((label, i) => ({ label: label || String(i), value: i, access: accessOf(i) }))
      .filter((o) => !PLACEHOLDER.test(o.label.trim()))
      .sort((a, b) => a.access - b.access);
  }
</script>

<div class="safetybar" role="group" aria-label="Safety controls">
  {#if estopActive && safetyAction && RECOVERY_OP != null}
    <div class="recovery" role="alert">
      <span>E-stop refused the last command — the machine is latched.</span>
      <button
        type="button"
        class="btn recover"
        disabled={!canFire(safetyAction, RECOVERY_OP)}
        title={reasonFor(safetyAction, RECOVERY_OP)}
        onclick={() => fire(safetyAction, RECOVERY_OP, 'clear ' + optionLabel(safetyAction, RECOVERY_OP), 'recover')}
      >
        {busy.recover ? '…' : 'Clear: ' + optionLabel(safetyAction, RECOVERY_OP)}
      </button>
    </div>
  {/if}

  {#if !machine.catalog.ready}
    <p class="empty">Safety controls unavailable — no catalog yet.</p>
  {:else if !actions.length}
    <p class="empty">This hub advertises no safety or home actions.</p>
  {:else}
    <div class="buttons">
      {#each actions as action (action.uid)}
        {#if action.options && action.options.length}
          {#each optionButtons(action) as opt (action.uid + ':' + opt.value)}
            {@const key = action.uid + ':' + opt.value}
            <button
              type="button"
              class="btn"
              class:estop={opt.value === SAFETY_OP.estop}
              disabled={!canFire(action, opt.value)}
              title={reasonFor(action, opt.value) || opt.label}
              onclick={() => fire(action, opt.value, opt.label, key)}
            >
              {busy[key] ? '…' : opt.label}
            </button>
          {/each}
        {:else}
          <button
            type="button"
            class="btn"
            disabled={!canFire(action, 1)}
            title={reasonFor(action, 1) || action.label}
            onclick={() => fire(action, 1, action.label, action.uid)}
          >
            {busy[action.uid] ? '…' : action.label}
          </button>
        {/if}
      {/each}
    </div>
  {/if}

  {#if lastResult && !lastResult.ok}
    <p class="err" role="status">refused ({lastResult.label}): {lastResult.error}</p>
  {/if}
</div>

<style>
  .safetybar {
    position: fixed;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 30;
    background: var(--bg-raised);
    border-top: 1px solid var(--line);
    padding: 8px var(--gap);
    padding-bottom: calc(8px + env(safe-area-inset-bottom));
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  /* ONE ROW, always. A wrapping safety bar grows as the machine advertises
     more ops, and a bar that grows upward covers the tab bar and the bottom of
     the page — which is exactly what it did. Scrolling horizontally keeps the
     bar's height constant and predictable, so the page can reserve space for
     it without measuring. */
  .buttons {
    display: flex;
    flex-wrap: nowrap;
    gap: 6px;
    overflow-x: auto;
    scrollbar-width: none;
  }
  .buttons::-webkit-scrollbar { display: none; }

  .btn {
    /* Never shrink: in a nowrap scrolling row flex would otherwise squeeze the
       labels and clip them mid-word ("estop_cle"). */
    flex: 0 0 auto;
    min-height: var(--tap);
    min-width: 64px;
    padding: 0 12px;
    border-radius: var(--r-s);
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink);
    font-size: 13px;
    font-weight: 500;
    white-space: nowrap;
  }
  .btn:disabled { opacity: 0.4; }
  .btn:not(:disabled):active { background: var(--line-soft); }
  /* E-stop is pinned to the left edge and stays put while the rest of the row
     scrolls under it. Any connected session may assert it (RFC-025b makes
     stop/estop role-exempt), so it must never be the button that happens to be
     off-screen when it is needed. */
  .btn.estop {
    background: color-mix(in srgb, var(--estop) 22%, var(--bg-card));
    border-color: var(--estop);
    color: var(--ink);
    font-weight: 700;
    position: sticky;
    left: 0;
    z-index: 1;
    box-shadow: 6px 0 8px -4px var(--bg-raised);
  }

  .recovery {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    padding: 6px 8px;
    border-radius: var(--r-s);
    background: color-mix(in srgb, var(--bad) 14%, var(--bg-card));
    border: 1px solid color-mix(in srgb, var(--bad) 45%, var(--line));
    font-size: 12.5px;
    color: var(--ink);
  }
  .btn.recover {
    background: var(--bad);
    border-color: var(--bad);
    color: var(--bg);
    font-weight: 700;
  }
  .btn.recover:disabled { opacity: 0.5; }

  .empty {
    font-size: 12.5px;
    color: var(--ink-faint);
    margin: 0;
  }

  .err {
    margin: 0;
    font-size: 12px;
    color: var(--bad);
  }

  @media (prefers-reduced-motion: reduce) {
    .btn { transition: none; }
  }
</style>
