<script>
  /**
   * DashGrid.svelte — the dashboard container: a 12-column grid that arranges
   * caller-supplied items and lets the operator drag to reorder, drag a
   * corner to resize, or do either from the keyboard.
   *
   * Contract (see model/dashboard.svelte.js for the persistence half):
   *   <DashGrid viewId="cat2" items={items} />
   * `items`: [{ id, title, snippet }], `id` a STABLE string (never an index),
   * `snippet` a Svelte 5 snippet rendered as the item body.
   *
   * This component owns the interaction choreography; DashItem only reports
   * raw pointer positions / key presses for the item IT is attached to, and
   * everything here is keyed by item id so a machine swap can never scramble
   * which callback affects which widget.
   *
   * Drag preview vs. commit: while a drag or resize is in flight, the visual
   * order/span lives in local, ephemeral state (previewIds / resizePreviewSpan)
   * — nothing is written to the persisted layout until pointer-up. That keeps
   * every intermediate frame of a drag purely cosmetic and trivially
   * cancellable, and it means layout.arrange() (the persisted source of
   * truth) never needs to know a drag is happening.
   */
  import DashItem from './DashItem.svelte';
  import { dashboardLayout } from '../../model/dashboard.svelte.js';

  let { viewId, items } = $props();

  // $derived (not a plain const) so a DashGrid instance whose `viewId` prop
  // changes in place — e.g. a caller that swaps tabs without remounting —
  // picks up the right view's saved layout instead of freezing on whichever
  // viewId it first mounted with.
  const layout = $derived(dashboardLayout(viewId));
  const baseArranged = $derived(layout.arrange(items));

  let dragId = $state(null);
  let previewIds = $state(null);
  let resizeId = $state(null);
  let resizePreviewSpan = $state(null);
  let announceMsg = $state('');
  let editing = $state(false);

  // What actually renders: the committed arrangement, overlaid with whatever
  // drag/resize preview is currently in flight (if any).
  const displayList = $derived.by(() => {
    let list = baseArranged;
    if (previewIds) {
      const byId = new Map(baseArranged.map((it) => [it.id, it]));
      list = previewIds.map((id) => byId.get(id)).filter(Boolean);
    }
    if (resizeId != null && resizePreviewSpan != null) {
      list = list.map((it) => (it.id === resizeId ? { ...it, span: resizePreviewSpan } : it));
    }
    return list;
  });

  // Plain (non-reactive) map of item id -> its cell element, kept only for
  // pointer-position hit-testing during drag. Never read during render.
  /** @type {Map<string, HTMLElement>} */
  const cellEls = new Map();
  function registerCell(node, id) {
    cellEls.set(id, node);
    return {
      destroy() {
        if (cellEls.get(id) === node) cellEls.delete(id);
      },
    };
  }

  function announce(msg) {
    announceMsg = msg;
  }

  // ---- drag-to-reorder ----------------------------------------------------
  function dragStart(id) {
    dragId = id;
    previewIds = baseArranged.map((it) => it.id);
  }
  function dragMove(id, clientX, clientY) {
    if (dragId !== id || !previewIds) return;
    let hit = null;
    for (const [otherId, el] of cellEls) {
      if (otherId === id) continue;
      const r = el.getBoundingClientRect();
      if (clientX >= r.left && clientX <= r.right && clientY >= r.top && clientY <= r.bottom) {
        hit = { id: otherId, before: clientX < r.left + r.width / 2 };
        break;
      }
    }
    if (!hit) return;
    const ids = previewIds.filter((x) => x !== id);
    let idx = ids.indexOf(hit.id);
    if (idx < 0) return;
    if (!hit.before) idx += 1;
    ids.splice(idx, 0, id);
    previewIds = ids;
  }
  function dragEnd(id) {
    if (dragId === id && previewIds) {
      layout.commitOrder(previewIds);
      const idx = previewIds.indexOf(id);
      const it = baseArranged.find((x) => x.id === id);
      if (it && idx >= 0) announce(it.title + ' moved to position ' + (idx + 1) + ' of ' + previewIds.length);
    }
    dragId = null;
    previewIds = null;
  }

  // ---- drag-to-resize -------------------------------------------------------
  function resizeStart(id) {
    resizeId = id;
  }
  function resizePreview(id, span) {
    if (resizeId === id) resizePreviewSpan = span;
  }
  function resizeEnd(id, span) {
    if (resizeId === id) {
      layout.setSpan(id, span);
      const it = baseArranged.find((x) => x.id === id);
      if (it) announce(it.title + ' resized to ' + span + ' of 12 columns');
    }
    resizeId = null;
    resizePreviewSpan = null;
  }

  // ---- keyboard move / resize ----------------------------------------------
  function keyMove(id, dir) {
    const ids = baseArranged.map((it) => it.id);
    const idx = ids.indexOf(id);
    if (idx < 0) return;
    const to = idx + dir;
    if (to < 0 || to >= ids.length) return;
    const next = ids.slice();
    [next[idx], next[to]] = [next[to], next[idx]];
    layout.commitOrder(next);
    const it = baseArranged.find((x) => x.id === id);
    if (it) announce(it.title + ' moved to position ' + (to + 1) + ' of ' + next.length);
  }
  function keyResize(id, delta) {
    const it = baseArranged.find((x) => x.id === id);
    if (!it) return;
    const next = Math.min(12, Math.max(1, it.span + delta));
    layout.setSpan(id, next);
    announce(it.title + ' resized to ' + next + ' of 12 columns');
  }

  function resetLayout() {
    layout.reset();
    dragId = null;
    previewIds = null;
    resizeId = null;
    resizePreviewSpan = null;
    announce('Layout reset to default');
  }

  function enterEditing() {
    editing = true;
    announce('Layout edit mode on');
  }
  function doneEditing() {
    editing = false;
    dragId = null;
    previewIds = null;
    resizeId = null;
    resizePreviewSpan = null;
    announce('Layout edit mode off');
  }
</script>

<div class="dash-wrap">
  <div class="dash-toolbar">
    {#if editing}
      <button type="button" class="reset-btn og-btn sm" onclick={resetLayout}>Reset layout</button>
      <button type="button" class="reset-btn done-btn og-btn sm" onclick={doneEditing}>Done</button>
    {:else}
      <button type="button" class="reset-btn og-btn sm" onclick={enterEditing}>Edit layout</button>
    {/if}
  </div>

  <div class="dash-grid">
    {#each displayList as item, i (item.id)}
      <div class="dash-cell" style={'--span:' + item.span} use:registerCell={item.id}>
        <DashItem
          {item}
          span={item.span}
          pidx={String(i + 1).padStart(2, '0')}
          editing={editing}
          dragging={dragId === item.id}
          ongrabstart={() => dragStart(item.id)}
          ongrabmove={(x, y) => dragMove(item.id, x, y)}
          ongrabend={() => dragEnd(item.id)}
          onresizestart={() => resizeStart(item.id)}
          onresizepreview={(s) => resizePreview(item.id, s)}
          onresizeend={(s) => resizeEnd(item.id, s)}
          onkeymove={(dir) => keyMove(item.id, dir)}
          onkeyresize={(delta) => keyResize(item.id, delta)}
        />
      </div>
    {/each}
  </div>

  <!-- Keyboard reorder/resize is the only interaction that needs an explicit
       announcement — pointer drags are already visible to a sighted operator
       watching the live preview. -->
  <div class="sr-only" aria-live="polite">{announceMsg}</div>
</div>

<style>
  .dash-wrap {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .dash-toolbar {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
  /* Base chrome (border, padding, font) comes from .og-btn.sm — only the
     Done state's distinguishing color is layered on top here. */
  .done-btn {
    color: var(--ink-hi);
    border-color: var(--line-4);
  }

  .dash-grid {
    display: grid;
    grid-template-columns: repeat(12, 1fr);
    /* .og-panel's outer outline paints 4px OUTSIDE each card's border box
       (outline-offset), so the grid needs room on both axes: a gap wide
       enough that neighboring outlines never touch, and edge padding so
       outlines on the outermost row/column never clip against this
       container. */
    gap: max(var(--gap), 14px);
    padding: 5px;
    align-items: start;
  }

  .dash-cell {
    grid-column: span var(--span, 12);
    min-width: 0;
  }

  /* Phone: every item forced full width. A 3-column layout on a 360px
     screen is unusable, so saved spans are deliberately ignored here — only
     the grid-column changes; reordering (position in the DOM) still applies. */
  @media (max-width: 640px) {
    .dash-cell { grid-column: 1 / -1; }
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
    border: 0;
  }

  /* Reduced motion: style.css already forces `* { transition: none !important }`
     globally under prefers-reduced-motion, which covers every transition this
     component or DashItem might use — no local overrides needed here. */
</style>
