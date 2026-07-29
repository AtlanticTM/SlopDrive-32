<script>
  /**
   * TransportBar.svelte — the OG transport subset, promoted to the hero row.
   *
   * Operator ruling 2026-07-28: Pause / Halt / E-Stop / Home move out of the
   * safety dock and into the instrument zone, top-right, the way the OG's
   * `.spine-transport` sat in its hero row. This renders ONLY that subset —
   * registry op constants (SAFETY_OP.pause/stop/estop, HOME_OP.home), the same
   * vocabulary ui/SafetyBar.svelte already discovers by. Everything else
   * (hold/resume/override/bypass/force_home/estop_clear, and the refusal
   * surface) stays in the safety dock — see SafetyBar.svelte. Manual is absent
   * because manual mode has no protocol role yet (see RailWidget's header).
   */
  import { machine, getSession } from '../model/machine.svelte.js';
  import { runAction } from '../model/shadow.svelte.js';
  import { SAFETY_OP, HOME_OP } from '../../../../SlopSync/clients/js/index.js';

  const roleActions = $derived(
    ((machine.catalog.model && machine.catalog.model.actions) || []).filter(
      (a) => typeof a.role === 'string' && (a.role.startsWith('action.safety') || a.role.startsWith('action.home'))
    )
  );

  /**
   * THE E-STOP MUST NOT DEPEND ON AN OPTIONAL ANNOTATION (copied from
   * SafetyBar.svelte, this bar's sibling surface — same catalog, same reason).
   * `action.*` roles are the right discovery path for device-defined verbs,
   * but safety-intents is a SPEC-CORE channel every conforming hub has, so it
   * is ALSO located by its protocol identity — a hub that forgot to annotate
   * it must not lose its stop/estop buttons.
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

  const actions = $derived(
    specSafety && !roleActions.some((a) => a.uid === specSafety.uid)
      ? [specSafety, ...roleActions]
      : roleActions
  );
  const linkUp = $derived(machine.link.phase === 'live');

  const isSafetyRole = (a) => typeof a.role === 'string' && a.role.startsWith('action.safety');
  const isHomeRole = (a) => typeof a.role === 'string' && a.role.startsWith('action.home');

  /**
   * Find one op by its registry wire value within the first matching action
   * that actually advertises it (an `options` array long enough to carry that
   * index). Absent op = null = absent button; never a hardcoded label.
   */
  function findOp(rolePred, value, fallback) {
    for (const a of actions) {
      if (!rolePred(a) || !a.options || !a.options.length) continue;
      if (value < a.options.length) {
        const label = a.options[value] || fallback;
        return { action: a, value, label, key: a.uid + ':' + value };
      }
    }
    return null;
  }

  const pauseCtl = $derived.by(() => findOp(isSafetyRole, SAFETY_OP.pause, 'pause'));
  const stopCtl = $derived.by(() => findOp(isSafetyRole, SAFETY_OP.stop, 'stop'));
  const estopCtl = $derived.by(() => findOp(isSafetyRole, SAFETY_OP.estop, 'estop'));
  const homeCtl = $derived.by(() => findOp(isHomeRole, HOME_OP.home, 'home'));

  /** May THIS session fire this exact op, per the catalog's own access data? */
  function canFire(action, value) {
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

  async function fire(action, value, label, btnKey) {
    busy = { ...busy, [btnKey]: true };
    await runAction(action, value);
    busy = { ...busy, [btnKey]: false };
    // Refusals surface globally via shadow.svelte.js's `lastRefusal`, rendered
    // by the safety dock — nothing left for this bar to do on failure.
  }
</script>

<div class="transportbar" role="group" aria-label="Transport controls">
  {#if pauseCtl}
    <button
      type="button"
      class="tbtn"
      disabled={!canFire(pauseCtl.action, pauseCtl.value)}
      title={reasonFor(pauseCtl.action, pauseCtl.value) || pauseCtl.label}
      onclick={() => fire(pauseCtl.action, pauseCtl.value, pauseCtl.label, pauseCtl.key)}
    >
      {busy[pauseCtl.key] ? '…' : pauseCtl.label}
    </button>
  {/if}

  {#if stopCtl}
    <button
      type="button"
      class="tbtn"
      disabled={!canFire(stopCtl.action, stopCtl.value)}
      title={reasonFor(stopCtl.action, stopCtl.value) || stopCtl.label}
      onclick={() => fire(stopCtl.action, stopCtl.value, stopCtl.label, stopCtl.key)}
    >
      {busy[stopCtl.key] ? '…' : stopCtl.label}
    </button>
  {/if}

  {#if estopCtl}
    <button
      type="button"
      class="tbtn btn-estop"
      disabled={!canFire(estopCtl.action, estopCtl.value)}
      title={reasonFor(estopCtl.action, estopCtl.value) || estopCtl.label}
      onclick={() => fire(estopCtl.action, estopCtl.value, estopCtl.label, estopCtl.key)}
    >
      <span class="estop-ico" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/>
          <path d="M12 9v4"/>
          <path d="M12 17h.01"/>
        </svg>
      </span>
      {busy[estopCtl.key] ? '…' : estopCtl.label}
    </button>
  {/if}

  {#if homeCtl}
    <button
      type="button"
      class="tbtn"
      disabled={!canFire(homeCtl.action, homeCtl.value)}
      title={reasonFor(homeCtl.action, homeCtl.value) || homeCtl.label}
      onclick={() => fire(homeCtl.action, homeCtl.value, homeCtl.label, homeCtl.key)}
    >
      {busy[homeCtl.key] ? '…' : homeCtl.label}
    </button>
  {/if}
</div>

<style>
  .transportbar {
    display: flex;
    gap: 4px;
    justify-content: flex-end;
  }

  /* ---- OG transport button (tag webui-prerefactor's .spine-transport) ---- */
  .tbtn {
    min-height: 36px;
    min-width: 48px;
    padding: 4px 10px;
    background: transparent;
    border: 1px solid var(--line-2);
    border-radius: var(--radius);
    color: var(--ink);
    font-family: var(--font);
    font-size: .72rem;
    font-weight: 500;
    white-space: nowrap;
    transition: border-color .12s, color .12s;
  }
  .tbtn:disabled { opacity: .45; }
  .tbtn:not(:disabled):hover { border-color: var(--line-4); }
  .tbtn:not(:disabled):active { border-color: var(--reality); color: var(--reality); }

  /* ---- the e-stop: OG hazard-stripe wash (copied from SafetyBar's
     .btn-estop — same visual, same constraint). No fill, no glow: a quiet
     outline chip whose hazard cue is a diagonal stripe wash in the safety
     red. */
  .btn-estop {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    background-image: repeating-linear-gradient(135deg, rgba(255, 71, 87, .09) 0 5px, rgba(255, 71, 87, .012) 5px 10px);
    border-color: var(--line-2);
    color: var(--ink);
    font-weight: 700;
    letter-spacing: .05em;
    text-transform: uppercase;
  }
  .btn-estop:not(:disabled):hover {
    border-color: var(--bad);
  }
  .btn-estop:not(:disabled):active {
    border-color: var(--bad);
    color: var(--bad);
  }
  .estop-ico {
    width: 14px;
    height: 14px;
    display: inline-grid;
    color: var(--bad);
  }
  .estop-ico svg {
    width: 14px;
    height: 14px;
  }

  /* MOBILE/DESKTOP E-STOP SPLIT. Breakpoint matches App.svelte's `isDesktop`
     matchMedia and SafetyBar's own 960px rule (the two are one positioning
     decision). Below it the page scrolls, so the fixed safety dock — never
     scrollable-away — owns the e-stop instead; this bar's copy only exists
     at desktop widths. */
  @media (max-width: 959px) {
    .btn-estop { display: none; }
  }

  @media (prefers-reduced-motion: reduce) {
    .tbtn { transition: none; }
  }
</style>
