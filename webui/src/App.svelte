<script>
  /**
   * App.svelte — the composition root.
   *
   * Reads the settings model, lets hero widgets CLAIM the fields they can draw
   * better, and hands everything left over to the generic renderer. The nav
   * set is the machine's own category list, so a hub with categories we have
   * never heard of gets nav entries we have never written.
   *
   * The sharpest test of this refactor lives here: there is no per-channel code
   * below. Add a settings channel to the firmware and it appears.
   *
   * ONE NAV MODEL, TWO RENDERINGS. The same tabs array (and the same `active`
   * id) drives a sidebar rail on desktop and a horizontal tab strip on a
   * phone — resizing the window mid-session must never lose the operator's
   * place or fork the nav logic.
   */
  import Field from './ui/Field.svelte';
  import LinkBar from './ui/LinkBar.svelte';
  import FootStrip from './ui/FootStrip.svelte';
  import SafetyBar from './ui/SafetyBar.svelte';
  import TransportBar from './ui/TransportBar.svelte';
  import SlopSyncPane from './ui/SlopSyncPane.svelte';
  import LogPane from './ui/LogPane.svelte';
  import PairingPane from './ui/PairingPane.svelte';
  import ThemePicker from './ui/ThemePicker.svelte';
  import HeroStrip from './ui/HeroStrip.svelte';
  import TelemetryChart from './ui/widgets/TelemetryChart.svelte';
  import DashGrid from './ui/dash/DashGrid.svelte';
  import { machine } from './model/machine.svelte.js';
  import { withoutClaimed } from './model/roles.js';
  import { heroClaims } from './ui/heroes.js';

  const model = $derived(machine.catalog.model);

  // Hero widgets get first refusal on the fields they understand. Whatever they
  // take is removed from the generic tree so no value is drawn twice.
  const heroes = $derived(model ? heroClaims(model.byRole) : { widgets: [], claimed: new Set() });
  const categories = $derived(model ? withoutClaimed(model.categories, heroes.claimed) : []);

  // heroes.js's zone split: 'instrument' heroes are pinned chrome (the hero
  // strip, below); 'card' heroes render as ordinary Overview cards instead
  // (folded into machineItems below).
  const instrumentHeroes = $derived(heroes.widgets.filter((h) => h.zone === 'instrument'));

  // Nav: the machine's categories, plus our own fixed views. The fixed ones are
  // about the LINK and the BROWSER rather than the machine, which is why they
  // are the only hardcoded entries in the page.
  const machineTabs = $derived([
    { id: 'machine', label: 'Overview' },
    ...categories.map((c) => ({ id: 'cat' + c.key, label: c.label, cat: c })),
  ]);
  const consoleTabs = [
    { id: 'pairing', label: 'Pairing' },
    { id: 'slopsync', label: 'SlopSync' },
    { id: 'log', label: 'Log' },
    { id: 'display', label: 'Display' },
  ];
  const tabs = $derived([...machineTabs, ...consoleTabs]);
  const navSections = $derived([
    { label: 'Machine', tabs: machineTabs },
    { label: 'Console', tabs: consoleTabs },
  ]);

  let active = $state('machine');
  const current = $derived(tabs.find((t) => t.id === active) || tabs[0]);

  // Desktop vs phone decides WHICH rendering of the nav mounts (rail vs tab
  // strip) — a matchMedia subscription, not a resize listener, so it costs
  // nothing between actual breakpoint crossings.
  let isDesktop = $state(false);
  $effect(() => {
    const mq = window.matchMedia('(min-width: 960px)');
    const apply = () => { isDesktop = mq.matches; };
    apply();
    mq.addEventListener('change', apply);
    return () => mq.removeEventListener('change', apply);
  });

  // NAV-SWITCH VISIBILITY. A tab switch must always produce visible change
  // (operator-reported defect: a pane switched below the fold, with nothing
  // on screen indicating anything had happened).
  //
  // Desktop needs no scrollIntoView: .content is the scroll container and a
  // freshly rendered pane starts at its own top by construction. It DOES
  // need its scroll position reset on every switch, though, or a pane opened
  // while scrolled halfway down the previous one inherits that scroll offset.
  let contentEl = $state(null);
  $effect(() => {
    active; // dependency: re-run this on every tab switch
    if (isDesktop && contentEl) contentEl.scrollTop = 0;
  });

  // Mobile: the tab strip itself must scroll to the top of the viewport on
  // activation, since the page (not a bounded region) is what scrolls here.
  let tabsNav = $state(null);
  function selectTab(id) {
    active = id;
    tabsNav?.scrollIntoView({ block: 'start', behavior: 'auto' });
  }

  // Rail collapse is a browser preference. Collapsed entries show a two-glyph
  // abbreviation DERIVED from the machine's own label — never an icon table,
  // which would be device knowledge dressed as art.
  let railMini = $state(loadRailPref());
  function loadRailPref() {
    try { return localStorage.getItem('sd32.navMini') === '1'; } catch (e) { return false; }
  }
  function toggleRail() {
    railMini = !railMini;
    try { localStorage.setItem('sd32.navMini', railMini ? '1' : '0'); } catch (e) { /* private mode */ }
  }
  function glyph(label) {
    return (label || '?').slice(0, 2).toUpperCase();
  }

  // Card-zone hero titles come from OUR registry ids (heroes.js), never a
  // device string — a plain first-letter capitalization is all that needs.
  function capitalize(s) {
    return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
  }

  /**
   * ADVANCED DISCLOSURE.
   *
   * RFC-009 ships an `advanced` flag on settings and nothing used it beyond a
   * small label. That stopped being cosmetic once this machine started
   * advertising 44 advanced pattern-modifier settings on top of 20 advanced
   * tuning knobs: the honest generic rendering of that is a wall of sliders
   * that buries the six controls anyone actually reaches for.
   *
   * Collapsed by default, per page, with the hidden count stated plainly.
   * Nothing is removed and nothing is hidden silently.
   *
   * Browser preference. Never sent to the machine.
   */
  let showAdvanced = $state(loadAdvancedPref());

  function loadAdvancedPref() {
    try { return localStorage.getItem('sd32.showAdvanced') === '1'; } catch (e) { return false; }
  }
  function toggleAdvanced() {
    showAdvanced = !showAdvanced;
    try { localStorage.setItem('sd32.showAdvanced', showAdvanced ? '1' : '0'); } catch (e) { /* private mode */ }
  }

  const visibleGroups = $derived.by(() => {
    if (!current || !current.cat) return { groups: [], hidden: 0 };
    let hidden = 0;
    const groups = [];
    for (const g of current.cat.groups) {
      const fields = g.fields.filter((f) => {
        if (showAdvanced || !f.flagBits.advanced) return true;
        hidden++;
        return false;
      });
      if (fields.length) groups.push({ ...g, fields });
    }
    return { groups, hidden };
  });

  /**
   * Dashboard items.
   *
   * `id` is a STABLE STRING built from the machine's own category id and group
   * name — never an array index. That is what lets a saved arrangement survive
   * a firmware update that adds a card, and lets the same browser talk to a
   * different machine without scrambling either layout.
   */
  const settingItems = $derived(
    visibleGroups.groups.map((g) => ({
      id: 'group:' + current.cat.id + ':' + (g.name || 'ungrouped'),
      title: g.name || 'Settings',
      snippet: groupCard,
      group: g,
    }))
  );

  const machineItems = $derived([
    { id: 'widget:telemetry', title: 'Telemetry', snippet: telemetryCard },
    // Card-zone heroes (heroes.js) are ordinary Overview cards, not pinned
    // chrome — same component, generic DashGrid treatment (drag/resize/etc).
    ...heroes.widgets
      .filter((h) => h.zone === 'card')
      .map((h) => ({ id: 'hero:' + h.id, title: capitalize(h.id), snippet: heroCard, hero: h })),
  ]);
</script>

{#snippet groupCard(item)}
  <div class="card-body">
    {#each item.group.fields as f (f.uid)}
      <Field field={f} />
    {/each}
  </div>
{/snippet}

{#snippet telemetryCard()}
  <TelemetryChart />
{/snippet}

{#snippet heroCard(item)}
  <item.hero.component fields={item.hero.fields} />
{/snippet}

{#snippet pane()}
  <main class="pane">
    {#if current.id === 'machine'}
      <DashGrid viewId="machine" items={machineItems} />
    {:else if current.cat}
      <DashGrid viewId={current.id} items={settingItems} />
      {#if visibleGroups.hidden || showAdvanced}
        <button class="adv-toggle" type="button" onclick={toggleAdvanced}
                aria-expanded={showAdvanced}>
          {#if showAdvanced}
            Hide advanced settings
          {:else}
            Show {visibleGroups.hidden} advanced setting{visibleGroups.hidden === 1 ? '' : 's'}
          {/if}
        </button>
      {/if}
    {:else if current.id === 'pairing'}
      <PairingPane />
    {:else if current.id === 'slopsync'}
      <SlopSyncPane />
    {:else if current.id === 'log'}
      <LogPane />
    {:else if current.id === 'display'}
      <ThemePicker />
    {/if}
  </main>
{/snippet}

<div class="app">
  <LinkBar />

  <!-- Only INSTRUMENT-zone heroes (heroes.js) render here, pinned above every
       view's PANE and never inside one: losing sight of the carriage because
       you opened a settings tab would be a regression from the old page. On
       desktop they run full width above the nav+pane frame; on a phone they
       sit above the tab strip. CARD-zone heroes render as ordinary Overview
       dashboard cards instead (see machineItems). -->
  {#if !machine.catalog.ready}
    <section class="boot">
      <p class="boot-msg">
        {#if machine.link.phase === 'live'}
          Adopting catalog…
        {:else if machine.link.phase === 'retrying'}
          Link lost — reconnecting. {machine.link.closeReason}
        {:else if machine.link.phase === 'failed'}
          No hub link. {machine.link.closeReason || 'The machine is not answering on the SlopSync port.'}
        {:else}
          Connecting to the hub…
        {/if}
      </p>
      <!-- Deliberately no fallback control path. HTTP is read-only since fw
           2.1.73, so a page with no hub link genuinely cannot drive anything,
           and pretending otherwise would be the exact lie the doctrine forbids. -->
    </section>
  {:else if isDesktop}
    <div class="instrument">
      <TransportBar />
      <HeroStrip heroes={instrumentHeroes} />
    </div>
    <div class="frame">
      <!-- The tablist role lives on an inner div: <nav> is a landmark, and ARIA
           forbids giving a non-interactive landmark an interactive role. -->
      <nav class="rail" class:mini={railMini} aria-label="Sections">
        <button type="button" class="rail-collapse" onclick={toggleRail}
                aria-expanded={!railMini}
                title={railMini ? 'Expand navigation' : 'Collapse navigation'}>
          <span aria-hidden="true">{railMini ? '»' : '«'}</span>
        </button>
        <div role="tablist" aria-orientation="vertical">
          {#each navSections as sec (sec.label)}
            <div class="rail-sec">
              {#if !railMini}<span class="rail-lbl">{sec.label}</span>{/if}
              {#each sec.tabs as t (t.id)}
                <button role="tab" class="rail-tab"
                        aria-selected={current && current.id === t.id}
                        class:on={current && current.id === t.id}
                        title={t.label}
                        onclick={() => (active = t.id)}>
                  <span class="rail-glyph mono" aria-hidden="true">{glyph(t.label)}</span>
                  {#if !railMini}<span class="rail-name">{t.label}</span>{/if}
                </button>
              {/each}
            </div>
          {/each}
        </div>
      </nav>
      <div class="content" bind:this={contentEl}>
        {@render pane()}
      </div>
    </div>
  {:else}
    <div class="instrument">
      <TransportBar />
      <HeroStrip heroes={instrumentHeroes} />
    </div>
    <nav class="tabs" aria-label="Sections" bind:this={tabsNav}>
      <div role="tablist">
        {#each tabs as t (t.id)}
          <button role="tab" aria-selected={current && current.id === t.id}
                  class:on={current && current.id === t.id}
                  onclick={() => selectTab(t.id)}>{t.label}</button>
        {/each}
      </div>
    </nav>
    {@render pane()}
  {/if}

  <FootStrip />
  <SafetyBar />
</div>

<style>
  /* ---- instrument zone: hero strip + transport row -----------------------
     TransportBar is the OG's `.spine-transport` (Pause/Halt/E-Stop/Home),
     promoted out of the safety dock into the hero row (operator ruling
     2026-07-28). Desktop overlays it top-right over the hero strip, the way
     the OG's hero row carried it; a phone's page scrolls instead, so it gets
     its own full-width row ABOVE the hero strip (OG mobile behavior), each
     button sharing the row equally. Breakpoint matches the `isDesktop`
     matchMedia above and SafetyBar's own 960px rule. */
  .instrument {
    position: relative;
  }
  @media (min-width: 960px) {
    .instrument :global(.transportbar) {
      position: absolute;
      top: 0;
      right: 0;
      z-index: 5;
    }
  }
  @media (max-width: 959px) {
    .instrument {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .instrument :global(.transportbar) {
      width: 100%;
    }
    .instrument :global(.transportbar .tbtn) {
      flex: 1 1 0;
    }
  }

  /* ---- desktop frame: rail + pane ----------------------------------------
     The one non-scrolling row of the desktop column (style.css's .app):
     bounded to whatever height is left after LinkBar/hero-strip/FootStrip/
     SafetyBar, with no overflow of its own — .content is the only region
     that scrolls. min-height:0 is required for a flex child to shrink below
     its content's natural height instead of forcing the column to overflow. */
  .frame {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: var(--gap);
    padding-top: var(--gap);
    flex: 1 1 0;
    min-height: 0;
    overflow: hidden;
  }
  .content {
    min-width: 0;
    min-height: 0;
    overflow-y: auto;
  }

  .rail {
    width: 188px;
    /* No longer sticky: its container (.frame) doesn't scroll on desktop —
       only .content does — so sticky positioning had nothing to stick
       against. Grid's default stretch gives it the frame's full height. */
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: 6px;
    background: var(--bg-raised);
    border: 1px solid var(--line-0);
    border-radius: var(--radius);
    /* Independent scroll for a catalog with many categories. */
    overflow-y: auto;
  }
  .rail.mini { width: 56px; }

  .rail-collapse {
    align-self: flex-end;
    min-width: 28px;
    min-height: 28px;
    color: var(--ink-faint);
    border-radius: var(--radius);
    font-size: 13px;
  }
  .rail-collapse:hover,
  .rail-collapse:focus-visible { color: var(--ink); background: var(--line-soft); }

  .rail-sec {
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding-bottom: 8px;
  }
  .rail-sec + .rail-sec {
    border-top: 1px solid var(--line-0);
    padding-top: 8px;
  }
  .rail-lbl {
    padding: 2px 8px 4px;
    font-size: 9.5px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: .1em;
    color: var(--ink-faint);
  }

  .rail-tab {
    display: flex;
    align-items: center;
    gap: 8px;
    min-height: 34px;
    padding: 0 8px;
    border-radius: var(--radius);
    border: 1px solid transparent;
    color: var(--ink-dim);
    font-size: .85rem;
    font-weight: 500;
    text-align: left;
    white-space: nowrap;
  }
  .rail-tab:hover { color: var(--ink); background: var(--line-soft); }
  .rail-tab.on {
    color: var(--ink-hi);
    background: var(--bg-card);
    border-color: var(--line-1);
  }
  /* Active marker: a reality-blue tick on the leading edge — the same accent
     that means "measured truth" everywhere else marks "you are here". */
  .rail-tab.on .rail-glyph { color: var(--reality); }

  .rail-glyph {
    flex: 0 0 auto;
    width: 24px;
    font-size: 10.5px;
    font-weight: 600;
    letter-spacing: .04em;
    color: var(--ink-faint);
    text-align: center;
  }
  .rail-name {
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }

  /* ---- phone: horizontal tab strip ---------------------------------------
     Sticky just below the link bar, because on a phone the settings list is
     long and losing the tab bar means scrolling all the way back up to change
     section. Horizontally scrollable rather than wrapping: a machine may
     publish more categories than fit, and a wrapping tab bar that grows to
     three rows pushes the actual content off-screen. */
  .tabs {
    position: sticky;
    top: var(--linkbar-h, 0px);
    z-index: 15;
    margin: 0 calc(var(--gap) * -1);
    padding: 6px var(--gap);
    background: color-mix(in srgb, var(--bg) 92%, transparent);
    backdrop-filter: blur(8px);
    border-bottom: 1px solid var(--line-0);
  }
  .tabs > div {
    display: flex;
    gap: 4px;
    overflow-x: auto;
    scrollbar-width: none;
  }
  .tabs > div::-webkit-scrollbar { display: none; }
  .tabs button {
    flex: 0 0 auto;
    min-height: var(--tap);
    padding: 0 14px;
    border-radius: var(--radius);
    color: var(--ink-dim);
    font-weight: 500;
    white-space: nowrap;
    border: 1px solid transparent;
  }
  .tabs button.on {
    color: var(--ink-hi);
    background: var(--bg-card);
    border-color: var(--line-1);
  }

  .pane { padding: var(--gap) 0; min-width: 0; }

  .card-body { display: grid; gap: 14px; }

  .boot {
    display: grid;
    place-items: center;
    min-height: 40vh;
    padding: var(--gap);
    text-align: center;
  }
  .boot-msg { color: var(--ink-dim); max-width: 40ch; }

  .adv-toggle {
    display: block;
    width: 100%;
    margin-top: var(--gap);
    min-height: var(--tap);
    border: 1px dashed var(--line-2);
    border-radius: var(--radius);
    color: var(--ink-dim);
    font-size: .85rem;
    letter-spacing: .04em;
  }
  .adv-toggle:hover { color: var(--ink); border-color: var(--line-3); }
</style>
