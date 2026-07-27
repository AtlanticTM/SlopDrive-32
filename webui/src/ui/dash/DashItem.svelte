<script>
  /**
   * DashItem.svelte — one dashboard widget's frame: a header with a grab
   * handle, the widget's own snippet as the body, and a resize handle.
   *
   * Purely presentational + interaction capture. It knows nothing about
   * other items, ordering, or persistence — that all lives in DashGrid,
   * which alone sees the whole arrangement. This component only reports what
   * the operator did (pointer positions, key presses) through callback
   * props, bound per-item by the parent's #each loop.
   *
   * The grab handle is the ONLY draggable surface — dashboard cards hold
   * sliders, buttons and toggles, and a draggable card body would steal
   * pointer gestures from every control inside it. `touch-action: none` is
   * scoped to the two handles alone so the page still scrolls normally on a
   * phone when you touch anywhere else on a card.
   *
   * Resize is self-contained: on resize-start this measures its OWN rendered
   * width (which equals the grid cell's content width) and divides by the
   * current span to get a per-column pixel size, then reports whole-column
   * deltas as the pointer moves. DashGrid never has to hand this component
   * grid metrics.
   */
  let {
    item,
    span,
    dragging = false,
    ongrabstart,
    ongrabmove,
    ongrabend,
    onresizestart,
    onresizepreview,
    onresizeend,
    onkeymove,
    onkeyresize,
  } = $props();

  let rootEl;
  let resizing = $state(false);
  let colPx = 0;
  let resizeStartX = 0;
  let resizeStartSpan = 1;

  function clamp(n) {
    return Math.min(12, Math.max(1, n));
  }

  // ---- grab handle: drag to reorder --------------------------------------
  function onGrabPointerDown(e) {
    if (e.button !== undefined && e.button !== 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    ongrabstart && ongrabstart();
  }
  function onGrabPointerMove(e) {
    if (!e.currentTarget.hasPointerCapture(e.pointerId)) return;
    ongrabmove && ongrabmove(e.clientX, e.clientY);
  }
  function onGrabPointerUp(e) {
    if (e.currentTarget.hasPointerCapture(e.pointerId)) {
      e.currentTarget.releasePointerCapture(e.pointerId);
    }
    ongrabend && ongrabend();
  }
  function onGrabKeyDown(e) {
    const key = e.key;
    if (key !== 'ArrowLeft' && key !== 'ArrowRight' && key !== 'ArrowUp' && key !== 'ArrowDown') return;
    e.preventDefault();
    if (e.shiftKey) {
      onkeyresize && onkeyresize(key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1);
    } else {
      onkeymove && onkeymove(key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1);
    }
  }

  // ---- resize handle: drag a corner to change span -----------------------
  function onResizePointerDown(e) {
    if (e.button !== undefined && e.button !== 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    const rect = rootEl.getBoundingClientRect();
    colPx = rect.width / Math.max(1, span);
    resizeStartX = e.clientX;
    resizeStartSpan = span;
    resizing = true;
    onresizestart && onresizestart();
  }
  function onResizePointerMove(e) {
    if (!resizing || !e.currentTarget.hasPointerCapture(e.pointerId)) return;
    const deltaCols = colPx > 0 ? Math.round((e.clientX - resizeStartX) / colPx) : 0;
    onresizepreview && onresizepreview(clamp(resizeStartSpan + deltaCols));
  }
  function onResizePointerUp(e) {
    if (e.currentTarget.hasPointerCapture(e.pointerId)) {
      e.currentTarget.releasePointerCapture(e.pointerId);
    }
    if (resizing) {
      const deltaCols = colPx > 0 ? Math.round((e.clientX - resizeStartX) / colPx) : 0;
      resizing = false;
      onresizeend && onresizeend(clamp(resizeStartSpan + deltaCols));
    }
  }
  function onResizeKeyDown(e) {
    const key = e.key;
    if (key !== 'ArrowLeft' && key !== 'ArrowRight') return;
    e.preventDefault();
    onkeyresize && onkeyresize(key === 'ArrowLeft' ? -1 : 1);
  }
</script>

<div class="dash-item" class:dragging bind:this={rootEl}>
  <div class="dash-head">
    <button type="button" class="handle grab"
            aria-label={'Drag to reorder ' + item.title + '. Arrow keys move it; shift plus arrow keys resize it.'}
            title="Drag to reorder — arrow keys move, shift+arrow resizes"
            onpointerdown={onGrabPointerDown}
            onpointermove={onGrabPointerMove}
            onpointerup={onGrabPointerUp}
            onpointercancel={onGrabPointerUp}
            onkeydown={onGrabKeyDown}>
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <circle cx="4" cy="4" r="1.3" /><circle cx="10" cy="4" r="1.3" />
        <circle cx="4" cy="8" r="1.3" /><circle cx="10" cy="8" r="1.3" />
        <circle cx="4" cy="12" r="1.3" /><circle cx="10" cy="12" r="1.3" />
      </svg>
    </button>
    <h3 class="dash-title">{item.title}</h3>
  </div>

  <div class="dash-body">
    <!-- The item is passed back to its own snippet so a CALLER can share one
         snippet across many items and switch on the item's payload. Snippets
         are declared statically in a template and cannot be manufactured per
         element in script, so without this argument every distinct card would
         need its own hand-written snippet — which defeats rendering a machine's
         settings generically. -->
    {@render item.snippet(item)}
  </div>

  <button type="button" class="handle resize"
          aria-label={'Resize ' + item.title + ' — currently ' + span + ' of 12 columns. Arrow keys shrink or grow it.'}
          title="Drag to resize — arrow keys shrink/grow"
          onpointerdown={onResizePointerDown}
          onpointermove={onResizePointerMove}
          onpointerup={onResizePointerUp}
          onpointercancel={onResizePointerUp}
          onkeydown={onResizeKeyDown}>
    <svg viewBox="0 0 10 10" aria-hidden="true" focusable="false">
      <path d="M9 1 L1 9 M9 5 L5 9 M9 9 L9 9" />
    </svg>
  </button>
</div>

<style>
  .dash-item {
    position: relative;
    display: flex;
    flex-direction: column;
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--radius);
    min-width: 0;
  }
  .dash-item.dragging {
    /* intent purple already means "commanded, not yet settled" everywhere
       else in this instrument — a card mid-move is exactly that. */
    box-shadow: inset 0 0 0 1.5px var(--intent);
    opacity: 0.9;
  }

  .dash-head {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 4px 8px 4px 4px;
    border-bottom: 1px solid var(--line);
    min-width: 0;
  }
  .dash-title {
    font-size: .8rem;
    font-weight: 500;
    letter-spacing: .04em;
    color: var(--ink-dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }

  .dash-body {
    padding: var(--gap);
    min-width: 0;
  }

  /* ---- handles ----
     Near-invisible until hover/focus — the frame should read as quiet
     instrument chassis, not a toy with visible chrome everywhere. Both meet
     --tap as a hit target even though the glyph inside is small. */
  .handle {
    display: grid;
    place-items: center;
    color: var(--ink-faint);
    border-radius: var(--radius);
    touch-action: none;
    -webkit-user-select: none;
    user-select: none;
  }
  .handle:hover,
  .handle:focus-visible {
    color: var(--ink);
    background: var(--line-soft);
  }
  .handle svg {
    width: 14px;
    height: 14px;
    fill: currentColor;
    stroke: currentColor;
    stroke-width: 1.4;
    stroke-linecap: round;
  }

  .handle.grab {
    flex: 0 0 auto;
    width: var(--tap);
    height: var(--tap);
    margin: -4px 0 -4px -4px;
    cursor: grab;
  }
  .handle.grab:active { cursor: grabbing; }

  .handle.resize {
    position: absolute;
    right: 0;
    bottom: 0;
    width: var(--tap);
    height: var(--tap);
    background: transparent;
    cursor: nwse-resize;
  }
  .handle.resize svg {
    width: 9px;
    height: 9px;
  }

  /* Span is a desktop/tablet concept — on a phone every item is forced full
     width by DashGrid, so resizing has nothing to do. */
  @media (max-width: 640px) {
    .handle.resize { display: none; }
  }
</style>
