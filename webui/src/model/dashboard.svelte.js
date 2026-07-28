/**
 * dashboard.svelte.js — Home-Assistant-style dashboard layout: state + persistence.
 *
 * Pure layout math over caller-supplied items — {id, title, snippet}. This
 * file has never heard of a channel, a field, or a machine; it only knows
 * "some string id wants a span and a position." That is deliberate: the
 * device-knowledge checker (test/check-device-knowledge.mjs) enforces zero
 * wire vocabulary above core/slopsync/, and a dashboard-layout engine has no
 * business needing any.
 *
 * ── STABILITY ACROSS MACHINES IS THE KEY REQUIREMENT ────────────────────────
 * The saved layout is a MAP keyed by item id — { [id]: {span, order} } — never
 * an array. That is the whole reason ids are stable strings and not indices:
 * an array position means nothing once the set of widgets changes, but a map
 * entry either matches an id or it doesn't.
 *   - An id present in storage but ABSENT from the current `items` (a
 *     different machine, a hidden feature) is simply never visited by
 *     arrange() below — it stays in storage, inert, in case that machine
 *     comes back, but it can never crash or misplace anything live.
 *   - An id present in `items` but ABSENT from storage (a new machine, a
 *     freshly added widget) gets DEFAULT_SPAN and appends after every item
 *     storage does know about, in the order `items` was handed to us.
 * Connecting to a different machine therefore never scrambles a saved layout
 * and never throws — worst case, some items you don't recognize sit unused in
 * localStorage and some items you do have land at the end with a default span.
 *
 * All localStorage access is wrapped in try/catch: private browsing, a full
 * quota, or storage disabled outright must degrade to "layout doesn't
 * persist," never to a broken page.
 */

const DEFAULT_SPAN = 12;
const MIN_SPAN = 1;
const MAX_SPAN = 12;

function storageKey(viewId) {
  return 'sd32.dash.' + viewId;
}

function loadLayout(viewId) {
  try {
    const raw = localStorage.getItem(storageKey(viewId));
    if (!raw) return {};
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch (e) {
    return {};
  }
}

function saveLayout(viewId, map) {
  try {
    localStorage.setItem(storageKey(viewId), JSON.stringify(map));
  } catch (e) {
    // Private mode / quota exceeded / storage disabled. The in-memory layout
    // still works for this page load; it just won't survive a reload.
  }
}

function clampSpan(span) {
  const n = Math.round(Number(span));
  if (!Number.isFinite(n)) return DEFAULT_SPAN;
  return Math.min(MAX_SPAN, Math.max(MIN_SPAN, n));
}

// One reactive layout store per viewId, cached so switching tabs and coming
// back keeps in-memory edits and doesn't re-read localStorage every time.
const registry = new Map();

/**
 * Get (or lazily create) the reactive layout controller for one dashboard
 * view. `viewId` namespaces persistence, e.g. "cat2" for the machine's
 * second settings category — each view keeps its own arrangement.
 */
export function dashboardLayout(viewId) {
  let l = registry.get(viewId);
  if (!l) {
    l = createLayout(viewId);
    registry.set(viewId, l);
  }
  return l;
}

function createLayout(viewId) {
  /** @type {Record<string, {span:number, order:number}>} */
  const map = $state(loadLayout(viewId));

  function persist() {
    saveLayout(viewId, { ...map });
  }

  function nextOrder() {
    let max = -1;
    for (const k in map) {
      if (map[k] && Number.isFinite(map[k].order) && map[k].order > max) max = map[k].order;
    }
    return max + 1;
  }

  /**
   * Merge the caller's live `items` with the saved map into a sorted,
   * fully-specified render list: [{id, title, snippet, span, order}, ...].
   *
   * PURE — never writes to `map`. Safe to call from a $derived on every
   * render; the "give unknown ids a default and append them" behavior
   * described above happens here at read time rather than by mutating
   * storage, so merely *looking* at a layout never persists anything.
   */
  function arrange(items) {
    let maxOrder = -1;
    for (const it of items) {
      const e = map[it.id];
      if (e && Number.isFinite(e.order) && e.order > maxOrder) maxOrder = e.order;
    }
    const out = items.map((it) => {
      const e = map[it.id];
      if (e) return { ...it, span: clampSpan(e.span ?? DEFAULT_SPAN), order: e.order };
      maxOrder += 1;
      return { ...it, span: DEFAULT_SPAN, order: maxOrder };
    });
    out.sort((a, b) => (a.order - b.order) || a.id.localeCompare(b.id));
    return out;
  }

  /** Set one item's column span (clamped 1..12), persisted immediately. */
  function setSpan(id, span) {
    const cur = map[id];
    map[id] = { span: clampSpan(span), order: cur ? cur.order : nextOrder() };
    persist();
  }

  /**
   * Commit a full new ordering: an array of item ids, first = position 0.
   * Spans are preserved for ids already in the map; ids seen for the first
   * time (still on their default position) get DEFAULT_SPAN. Used by both
   * pointer-drag drop and keyboard reorder — the only two places an order
   * actually changes.
   */
  function commitOrder(orderedIds) {
    orderedIds.forEach((id, i) => {
      const cur = map[id];
      map[id] = { span: cur ? cur.span : DEFAULT_SPAN, order: i };
    });
    persist();
  }

  /** Clear this view's saved layout — the "reset layout" affordance. */
  function reset() {
    for (const k of Object.keys(map)) delete map[k];
    try {
      localStorage.removeItem(storageKey(viewId));
    } catch (e) {
      // already degraded to in-memory-only; nothing further to clean up
    }
  }

  return { arrange, setSpan, commitOrder, reset };
}
