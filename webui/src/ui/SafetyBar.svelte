<script>
  /**
   * SafetyBar.svelte — the persistent, always-reachable safety dock.
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
   * exact same data the hub gates on, so this dock and the hub cannot disagree
   * about what a given session may press. The e-stop itself renders OUTSIDE
   * the scrolling op groups as the dock's one fixed, oversized control — it
   * must never be the button that happens to be off-screen when it is needed.
   *
   * GLOBAL REFUSAL SURFACE: this dock is pinned to the viewport, so it is the
   * one place a refusal from ANY control (a settings slider, an action
   * button, the rail's move tape — any of shadow.svelte.js's three entry
   * points) is guaranteed to be visible even after the control that sent it
   * has scrolled off or unmounted. `lastRefusal` + `remedyForLastRefusal()`
   * come from shadow.svelte.js, which is also where the NACK-code -> action-
   * role table lives (`NOT_HOMED` -> `action.home`, `ESTOP_ACTIVE` ->
   * `action.safety`'s `estop_clear` op) — this component only renders it.
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { runAction, lastRefusal, remedyForLastRefusal, clearLastRefusal } from '../model/shadow.svelte.js';
  import { SAFETY_OP, HOME_OP } from '../../../../SlopSync/clients/js/index.js';
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

  const isSafetyRole = (a) => typeof a.role === 'string' && a.role.startsWith('action.safety');
  const isHomeRole = (a) => typeof a.role === 'string' && a.role.startsWith('action.home');

  /**
   * The e-stop, pulled out of its op group and rendered as the dock's one
   * oversized fixed control. Matched by wire value ONLY within a safety-role
   * action — SAFETY_OP numbers are the safety op table's; the same integer in
   * a home channel is a different verb entirely.
   */
  const estopCtl = $derived.by(() => {
    for (const a of actions) {
      if (!isSafetyRole(a) || !a.options || !a.options.length) continue;
      if (SAFETY_OP.estop < a.options.length) {
        const label = a.options[SAFETY_OP.estop] || 'estop';
        return { action: a, value: SAFETY_OP.estop, label, key: a.uid + ':' + SAFETY_OP.estop };
      }
    }
    return null;
  });

  /**
   * Group caption from the ROLE that put the action in this dock — registry
   * vocabulary, not device knowledge. An action here by any other role prefix
   * falls back to its own catalog label.
   */
  function groupLabel(a) {
    if (isSafetyRole(a)) return 'safety';
    if (typeof a.role === 'string' && a.role.startsWith('action.home')) return 'home';
    return a.label || a.name || '';
  }

  /**
   * REGISTRY VOCABULARY — icon + subtitle keyed by SAFETY_OP/HOME_OP wire
   * value. Duplicated from TransportBar.svelte (icon path strings copied
   * VERBATIM from the OG's ui.js ICONS table, Lucide MIT) — this task's
   * edit scope is limited to these two files, so there is no shared module
   * to hoist this into yet; do that if a third consumer needs it. Same
   * two-namespace split as TransportBar's copy: a SAFETY_OP value and a
   * HOME_OP value are different verbs, so one flat table keyed by raw
   * number would risk a silent cross-namespace collision. pause/stop/home
   * never actually reach this dock's option groups (see optionButtons'
   * filters below) — they render in TransportBar now — so in practice only
   * a future op could ever match here; absence still means label-only.
   */
  const SAFETY_META = {
    [SAFETY_OP.pause]: {
      icon: '<rect x="6" y="4" width="4" height="16" rx="1"/><rect x="14" y="4" width="4" height="16" rx="1"/>',
      subtitle: 'hold position',
    },
    [SAFETY_OP.stop]: {
      icon: '<rect x="5" y="5" width="14" height="14" rx="2"/>',
      subtitle: 'stop motion',
    },
    [SAFETY_OP.estop]: {
      icon: '<path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
      subtitle: 'cut power',
    },
  };
  const HOME_META = {
    [HOME_OP.home]: {
      icon: '<path d="M3 9l9-7 9 7v11a2 2 0 01-2 2H5a2 2 0 01-2-2z"/><path d="M9 22V12h6v10"/>',
      subtitle: 'seek home',
    },
  };
  function metaFor(action, value) {
    if (isSafetyRole(action)) return SAFETY_META[value];
    if (isHomeRole(action)) return HOME_META[value];
    return undefined;
  }
  /** Wrap an OG-derived path string in the exact svg attrs its ICONS table uses. */
  function iconMarkup(paths) {
    return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>`;
  }
  /**
   * Presentation only: swap the hub's own '_' for a space so multi-word ops
   * ("override_on") wrap and read naturally; CSS text-transform:capitalize
   * does the casing. Not inventing text — this is the catalog's own label
   * string, reformatted for display.
   */
  function displayLabel(label) {
    return String(label).replace(/_/g, ' ');
  }

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
    if (!linkUp) return 'no hub link';
    if (!canFire(action, value)) return 'this session is not authorized for this op';
    return '';
  }

  let busy = $state({});
  let lastResult = $state(null); // { ok, label, error, at } — this dock's OWN last press

  async function fire(action, value, label, btnKey) {
    busy = { ...busy, [btnKey]: true };
    const result = await runAction(action, value);
    busy = { ...busy, [btnKey]: false };
    lastResult = { ok: result.ok, label, error: result.error || null, at: Date.now() };
    // The global refusal banner is driven by shadow.svelte.js's `lastRefusal`
    // — runAction() already updated it on failure, so there is nothing left
    // to do here for that surface.
  }

  /**
   * Order by the access each op REQUIRES, lowest first.
   *
   * This is not cosmetic. RFC-025b makes stop and estop role-exempt, and the
   * catalog encodes that as an `option_access` of `watch` — the lowest tier
   * there is. So sorting ascending by required access puts the emergency ops
   * at the head of the row on every conforming hub, without this component
   * knowing which ops those are.
   */
  /**
   * WIRE VALUE 0 IS NOT RENDERED. RFC-034 (registry.yaml `field_roles`
   * doctrine) is normative: for a select field carrying an `action.*` role,
   * value 0 is NEVER an operation — every op table numbers its real ops from
   * 1, and 0 exists only to keep the option array index-aligned. This dock
   * used to gray it instead (an index-completeness argument borrowed from
   * listbox semantics), which put a permanently dead button labeled
   * "reserved" in the operator's face — a wire-format alignment artifact
   * rendered as chrome. Operator ruling 2026-07-28: drop it. These are
   * buttons, not an index-addressed listbox; nothing an operator can do
   * refers to option INDEX, so omitting the placeholder loses nothing.
   * Real ops a session merely lacks access to stay GRAYED, never hidden —
   * that doctrine is unchanged.
   *
   * The e-stop is also filtered from its group here — it renders separately
   * as the dock's fixed control, and drawing it twice would be worse than
   * either rendering alone.
   *
   * Operator ruling 2026-07-28: pause/stop and home also leave this group —
   * they render in TransportBar now (see below). `force_home` is dev-only
   * and stays here for now, unfiltered, until a dev affordance exists to
   * hide/disable it properly.
   */
  function optionButtons(action) {
    const floor = action.access | 0;
    const accessOf = (i) => {
      const a = action.optionAccess && action.optionAccess[i];
      return a == null ? floor : a;
    };
    return (action.options || [])
      .map((label, i) => ({ label: label || String(i), value: i, access: accessOf(i) }))
      .filter((o) => o.value !== 0)
      .filter((o) => !(isSafetyRole(action) && o.value === SAFETY_OP.estop))
      // Operator ruling 2026-07-28: pause/stop (safety-role) and home
      // (home-role) moved out of the dock into TransportBar, the top-of-page
      // hero-row transport row — they render there now, not here.
      .filter((o) => !(isSafetyRole(action) && (o.value === SAFETY_OP.pause || o.value === SAFETY_OP.stop)))
      .filter((o) => !(isHomeRole(action) && o.value === HOME_OP.home))
      .sort((a, b) => a.access - b.access);
  }

</script>

<div class="safetydock" role="group" aria-label="Safety controls">
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
    <div class="dock">
      {#if estopCtl}
        <button
          type="button"
          class="btn btn-estop"
          disabled={!canFire(estopCtl.action, estopCtl.value)}
          title={reasonFor(estopCtl.action, estopCtl.value) || estopCtl.label}
          onclick={() => fire(estopCtl.action, estopCtl.value, estopCtl.label, estopCtl.key)}
        >
          <span class="row">
            <span class="ico" aria-hidden="true">{@html iconMarkup(SAFETY_META[SAFETY_OP.estop].icon)}</span>
            <span class="lbl">{busy[estopCtl.key] ? '…' : estopCtl.label}</span>
          </span>
          <small>{SAFETY_META[SAFETY_OP.estop].subtitle}</small>
        </button>
      {/if}

      <div class="groups">
        {#each actions as action (action.uid)}
          {#if action.options && action.options.length}
            {@const opts = optionButtons(action)}
            {#if opts.length}
              <div class="grp">
                <span class="grp-lbl">{groupLabel(action)}</span>
                <div class="grp-btns">
                  {#each opts as opt (action.uid + ':' + opt.value)}
                    {@const key = action.uid + ':' + opt.value}
                    {@const meta = metaFor(action, opt.value)}
                    <button
                      type="button"
                      class="btn"
                      disabled={!canFire(action, opt.value)}
                      title={reasonFor(action, opt.value) || opt.label}
                      onclick={() => fire(action, opt.value, opt.label, key)}
                    >
                      <span class="row">
                        {#if meta}<span class="ico" aria-hidden="true">{@html iconMarkup(meta.icon)}</span>{/if}
                        <span class="lbl">{busy[key] ? '…' : displayLabel(opt.label)}</span>
                      </span>
                      {#if meta}<small>{meta.subtitle}</small>{/if}
                    </button>
                  {/each}
                </div>
              </div>
            {/if}
          {:else}
            {@const meta = metaFor(action, 1)}
            <div class="grp">
              <span class="grp-lbl">{groupLabel(action)}</span>
              <div class="grp-btns">
                <button
                  type="button"
                  class="btn"
                  disabled={!canFire(action, 1)}
                  title={reasonFor(action, 1) || action.label}
                  onclick={() => fire(action, 1, action.label, action.uid)}
                >
                  <span class="row">
                    {#if meta}<span class="ico" aria-hidden="true">{@html iconMarkup(meta.icon)}</span>{/if}
                    <span class="lbl">{busy[action.uid] ? '…' : displayLabel(action.label)}</span>
                  </span>
                  {#if meta}<small>{meta.subtitle}</small>{/if}
                </button>
              </div>
            </div>
          {/if}
        {/each}
      </div>

      {#if lastResult && !lastResult.ok}
        <p class="err" role="status">refused ({lastResult.label}): {lastResult.error}</p>
      {/if}
    </div>
  {/if}
</div>

<style>
  .safetydock {
    left: 0;
    right: 0;
    /* Safety chrome always wins the bottom edge, and that ownership is now
       EXCLUSIVE: ShellBar's chrome moved to the top of the page (see
       ShellBar.svelte), so nothing else ever competes for this z-index. */
    z-index: 30;
    background: var(--bg-raised);
    border-top: 1px solid var(--line);
    padding: 6px var(--gap);
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  /* Breakpoint matches App.svelte's `isDesktop` matchMedia (960px) — the two
     rules below are the mobile/desktop halves of one positioning decision. */
  @media (max-width: 959px) {
    .safetydock {
      position: fixed;
      bottom: 0;
      padding-bottom: calc(6px + env(safe-area-inset-bottom, 0px));
    }
  }
  @media (min-width: 960px) {
    .safetydock {
      /* .app is a 100dvh flex column (style.css) and this is its last row —
         a normal flex item, always on screen without being pinned. */
      position: static;
    }
  }

  .dock {
    display: flex;
    align-items: stretch;
    gap: 12px;
  }

  /* ---- the e-stop: OG hazard-stripe wash (tag webui-prerefactor) ---------
     No fill, no glow, no uppercase/bold override — pixel-checked against
     webui/test/evidence/og-ref/og-full.png: a quiet two-line chip like
     every other transport button, whose only hazard cue is the diagonal
     stripe wash in the safety red plus the alert-triangle icon; text and
     icon stay the default ink color at rest and only redden on hover/active.
     Same visual as TransportBar's copy (operator requirement: shared look).
     Fixed OUTSIDE .groups: the one control that must never scroll away. */
  .btn-estop {
    align-self: stretch;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0;
    min-width: 96px;
    padding: 0 14px;
    background-image: repeating-linear-gradient(135deg, rgba(255, 71, 87, .09) 0 5px, rgba(255, 71, 87, .012) 5px 10px);
    border-color: var(--line-2);
    color: var(--ink);
  }
  .btn-estop:not(:disabled):hover {
    border-color: var(--bad);
  }
  .btn-estop:not(:disabled):active {
    border-color: var(--bad);
    color: var(--bad);
  }

  /* Operator ruling 2026-07-28: desktop shows the e-stop in TransportBar
     (top of page) instead — hide the dock's copy there. The dock keeps
     rendering it below 960px, wherever the page scrolls, so it is never the
     button that happens to be off-screen when it is needed. Breakpoint
     matches App.svelte's `isDesktop` matchMedia. */
  @media (min-width: 960px) {
    /* Compound selector on purpose: the base `.btn` rule also sets display
       and sits later in this block — a bare `.btn-estop` ties on specificity
       and loses the cascade to it. `.btn.btn-estop` outranks both regardless
       of rule order. */
    .btn.btn-estop { display: none; }
  }

  /* ---- op groups: labeled clusters, one scrolling row --------------------
     ONE ROW, always. A wrapping dock grows as the machine advertises more
     ops, and a dock that grows upward covers the page. Scrolling keeps the
     height constant; the e-stop sits outside this scroll area entirely. */
  .groups {
    display: flex;
    align-items: flex-end;
    gap: 14px;
    overflow-x: auto;
    scrollbar-width: none;
    min-width: 0;
  }
  .groups::-webkit-scrollbar { display: none; }

  .grp {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }
  .grp-lbl {
    /* OG .pidx-label voice (CSS-diff audit 2026-07-29): quiet mono, no
       uppercase shouting — the label is a waypoint, not a heading. */
    font-family: var(--mono);
    font-size: .62rem;
    letter-spacing: .08em;
    color: var(--tx-faint);
    padding-left: 1px;
  }
  .grp-btns {
    display: flex;
    gap: 6px;
  }

  .btn {
    /* Never shrink: flex would otherwise squeeze the labels and clip them
       mid-word ("estop_cle"). */
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0;
    min-height: 36px;
    min-width: 56px;
    padding: 0 12px;
    background: transparent;
    border: 1px solid var(--line-2);
    border-radius: var(--r-s);
    color: var(--ink);
    font-weight: 500;
    font-size: .72rem;
    white-space: nowrap;
    transition: border-color .12s, color .12s;
  }
  .btn:disabled { opacity: 0.4; }
  .btn:not(:disabled):hover { border-color: var(--line-4); }
  .btn:not(:disabled):active { border-color: var(--reality); color: var(--reality); }

  /* ---- two-line treatment (TransportBar's .tbtn, shared here) ------------
     Labels are the hub's own catalog strings — capitalize is presentation
     only (see displayLabel()), never a hardcoded string. Ops absent from
     SAFETY_META/HOME_META render no .ico/small — label-only, single line,
     same as before this pass; a future hub op must not break this dock. */
  .btn .row { display: flex; align-items: center; gap: 4px; }
  .btn .lbl { text-transform: capitalize; }
  .btn .ico { width: 14px; height: 14px; display: inline-grid; }
  .btn .ico :global(svg) { width: 14px; height: 14px; }
  .btn small {
    font-size: .56rem;
    color: var(--tx-mut);
    font-weight: 400;
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
    align-self: center;
    font-size: 12px;
    color: var(--bad);
  }

  @media (prefers-reduced-motion: reduce) {
    .btn { transition: none; }
  }
</style>
