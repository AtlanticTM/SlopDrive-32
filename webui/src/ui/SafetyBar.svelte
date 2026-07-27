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
   * GLOBAL REFUSAL SURFACE: this bar is pinned to the viewport, so it is the
   * one place a refusal from ANY control (a settings slider, an action
   * button, the rail's move tape — any of shadow.svelte.js's three entry
   * points) is guaranteed to be visible even after the control that sent it
   * has scrolled off or unmounted. `lastRefusal` + `remedyForLastRefusal()`
   * come from shadow.svelte.js, which is also where the NACK-code -> action-
   * role table lives (`NOT_HOMED` -> `action.home`, `ESTOP_ACTIVE` ->
   * `action.safety`'s `estop_clear` op) — this component only renders it.
   * This REPLACES the old estop-only local recovery banner: same mechanism,
   * generalised to every refusal instead of hardcoded to one NACK.
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { runAction, lastRefusal, remedyForLastRefusal, clearLastRefusal } from '../model/shadow.svelte.js';
  import { SAFETY_OP } from '../core/slopsync/index.js';
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
  const linkUp = $derived(machine.link.phase === 'live');

  /**
   * The remedy for the CURRENT global refusal, if this hub advertises one.
   * Reactive to `lastRefusal` (a new refusal anywhere in the app) and to the
   * catalog (the action has to actually exist on THIS hub) — both reads
   * happen inside remedyForLastRefusal() itself, which is enough for Svelte's
   * fine-grained tracking to pick them up through this $derived.by.
   */
  const remedy = $derived.by(() => remedyForLastRefusal());
  let remedyBusy = $state(false);

  async function fireRemedy() {
    if (!remedy) return;
    remedyBusy = true;
    const result = await runAction(remedy.action, remedy.op);
    remedyBusy = false;
    // Ground truth: only clear the banner once the ECHO confirms the remedy
    // was actually applied — never optimistically on the mere act of tapping.
    if (result.ok) clearLastRefusal();
  }

  /** May THIS session fire this exact op, per the catalog's own access data? */
  function canFire(action, value) {
    // Reading these keeps the check reactive to auth/catalog changes even
    // though the session object itself lives outside Svelte's reactivity.
    void machine.link.roles; void machine.link.phase; void machine.catalog.ready;
    const session = getSession();
    return !!session && session.isLive && session.canUse(action.channelId, action.key, value);
  }

  function reasonFor(action, value) {
    if (value === 0) return 'wire value 0 is reserved, never an operation (RFC-034)';
    if (!linkUp) return 'no hub link';
    if (!canFire(action, value)) return 'this session is not authorised for this op';
    return '';
  }

  let busy = $state({});
  let lastResult = $state(null); // { ok, label, error, at } — this button's OWN last press

  async function fire(action, value, label, btnKey) {
    busy = { ...busy, [btnKey]: true };
    const result = await runAction(action, value);
    busy = { ...busy, [btnKey]: false };
    lastResult = { ok: result.ok, label, error: result.error || null, at: Date.now() };
    // The global refusal banner below is driven by shadow.svelte.js's
    // `lastRefusal` — runAction() already updated it on failure, so there is
    // nothing left to do here for that surface.
  }

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
  /**
   * Option-select INTENT fields are index-aligned with their wire value, and
   * RFC-034 made it NORMATIVE (registry.yaml, `field_roles` doctrine): for a
   * select field carrying an `action.*` role, wire value 0 is NEVER an
   * operation — every op table numbers its real ops from 1, and 0 exists only
   * to keep the array aligned. This used to be guessed from the option's own
   * LABEL text (a regex for "reserved"/"none"/"unused"/...), which is exactly
   * the kind of device-knowledge-shaped heuristic this layer is supposed to
   * refuse: a machine that spelled its placeholder differently, or in another
   * language, sailed straight through and rendered a button that meant
   * nothing and earned a NACK if pressed.
   *
   * GREY, never hide — same doctrine as option_access gating below it (which
   * stays as defense in depth for sub-configure sessions; a hub CAN gate
   * index 0 itself the way 0x0009 session-admin does, and if it does, this
   * still greys it). Hiding index 0 outright would have been the simpler fix
   * but breaks a keyboard/screen-reader user's expectation that the option
   * list is index-complete.
   */
  function optionButtons(action) {
    const floor = action.access | 0;
    const accessOf = (i) => {
      const a = action.optionAccess && action.optionAccess[i];
      return a == null ? floor : a;
    };
    return (action.options || [])
      .map((label, i) => ({ label: label || String(i), value: i, access: accessOf(i), reserved: i === 0 }))
      .sort((a, b) => a.access - b.access);
  }
</script>

<div class="safetybar" role="group" aria-label="Safety controls">
  {#if lastRefusal.code != null}
    <!-- THE GLOBAL REFUSAL SURFACE. Any of shadow.svelte.js's three write
         paths — a settings slider, an action button, the rail's move tape —
         lands here the instant the hub refuses it, whether or not the
         control that sent it is still on screen (CLAUDE.md 3, Ground Truth
         Doctrine: a swallowed refusal misrepresents machine state). The
         remedy button only appears when THIS hub's catalog actually
         advertises the action that clears it. -->
    <div class="recovery" role="alert">
      <span>
        refused{lastRefusal.label ? ' (' + lastRefusal.label + ')' : ''}:
        {lastRefusal.codeName}{lastRefusal.detail ? ' — ' + lastRefusal.detail : ''}
      </span>
      {#if remedy}
        <button
          type="button"
          class="btn recover"
          disabled={!canFire(remedy.action, remedy.op)}
          title={reasonFor(remedy.action, remedy.op)}
          onclick={fireRemedy}
        >
          {remedyBusy ? '…' : 'Fix: ' + optionLabel(remedy.action, remedy.op)}
        </button>
      {/if}
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
              disabled={opt.reserved || !canFire(action, opt.value)}
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
