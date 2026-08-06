#pragma once

// GraphPage — the slopsim analyzer page, embedded so the exe stays standalone.
// Constraints:
//   Served at GET /graph by HttpFacade. A dev-tool page, not the device WebUI —
//   the firmware's compile-time asset pipeline rules don't apply.
//
//   THREE SAMPLE SOURCES, ONE FORMAT. Every feed is the same 20-byte header
//   {u32 n, f32 max_rail, f32 win_min, f32 win_max, u32 stride} + n x stride
//   f32 LE, stride = 6: {t, pos, tgt, vel, cmd_norm, raw_norm}. That is why the
//   async-tune mode cost the ingest path almost nothing:
//     GET /api/trace.bin  ?since=<t_s>  — LIVE, incremental poll of the running
//                                          machine's ring.
//     GET /api/replay.bin ?rec=&<tuning> — RECOMPUTED: a saved wire recording
//                                          re-run through a fresh engine.
//     GET /api/run.bin    ?name=         — RECALLED: a frozen result, drawn as
//                                          stored, no engine involved.
//   The page reads `stride` from the header rather than assuming it, so an older
//   page against a newer sim degrades to the columns it knows.
//   PERFORMANCE (read before editing the embedded script): the trace feed
//   is 1 kHz and a session runs for hours, so the client-side sample count
//   is UNBOUNDED. Every per-frame cost in the script must be O(canvas
//   pixels), never O(samples) — the min/max pyramid inside the script is
//   what makes that true; do not replace it with sub-sampling or averaging.

namespace slopsim {

inline constexpr const char kGraphPageHtml[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>slopsim analyzer</title>
<style>
  /* Series colors are SlopScope's (tools/slopscope.py PALETTES "product"),
     which are in turn the machine's own webui tokens. One semantic split, drawn
     as a hue boundary:
       PURPLE = asked. What the CLIENT wanted - the wire targets (cmd) and the
                continuous curve they describe (raw).
       BLUE   = machine. What the machine decided (tgt, the accepted intent
                post-arbiter/clamp/window) and what it measured (pos). `tgt` is
                deliberately a DEEPER STEP OF THE SAME BLUE, not a new hue:
                it is already across the boundary and on reality's side, and
                lightness is the one channel every CVD type keeps.
     `pos` was green, which put measured truth in a third family and made it read
     as unrelated to the setpoint it is supposed to be tracking. */
  :root { --bg:#1a1915; --panel:#211f1a; --chrome:#6b675c; --fg:#e8e6e0;
          --accent:#e8956b; --pos:#4DA6FF; --tgt:#2E7FD6; --vel:#4DA6FF;
          --cmd:#A78BFA; --raw:#C4A0FF; --base:#d4736e; }
  * { box-sizing:border-box; margin:0; }
  body { background:var(--bg); color:var(--fg); font:13px/1.5 ui-monospace,Consolas,monospace;
         display:flex; flex-direction:column; height:100vh; overflow:hidden; }
  header { display:flex; align-items:center; gap:14px; padding:8px 14px;
           border-bottom:1px solid #2e2c26; }
  header .logo { color:var(--accent); font-weight:bold; }
  header .dim { color:var(--chrome); }
  header button { background:var(--panel); color:var(--fg); border:1px solid #3a372f;
                  border-radius:6px; padding:3px 12px; font:inherit; cursor:pointer; }
  header button:hover { border-color:var(--accent); }
  header button.on { color:var(--accent); border-color:var(--accent); }
  #cmd.on { color:var(--cmd); border-color:var(--cmd); }
  #raw.on { color:var(--raw); border-color:var(--raw); }
  #stats { margin-left:auto; color:var(--chrome); }
  #plots { flex:1; display:flex; flex-direction:column; min-height:0; padding:6px 10px 2px; }
  canvas { width:100%; display:block; }
  #poswrap { flex:3; min-height:0; } #velwrap { flex:1; min-height:0; margin-top:4px; }
  #poswrap,#velwrap { position:relative; }
  #poswrap canvas,#velwrap canvas { position:absolute; inset:0; height:100%; }
  #readout { position:fixed; pointer-events:none; background:#26241d; border:1px solid #3a372f;
             border-radius:6px; padding:6px 10px; display:none; z-index:5; white-space:pre; }
  footer { padding:4px 14px 8px; color:var(--chrome); }
  .k { color:var(--accent); }
  #timelinewrap { flex:none; height:48px; margin:6px 14px 0; position:relative; }
  #timelinewrap canvas { position:absolute; inset:0; height:100%; cursor:pointer; }
  .expwrap { position:relative; }
  .ddmenu { display:none; position:absolute; top:100%; left:0; margin-top:4px;
            background:var(--panel); border:1px solid #3a372f; border-radius:6px;
            padding:8px; z-index:6; white-space:nowrap; }
  .ddmenu.open { display:block; }
  .ddrow { display:flex; align-items:center; gap:10px; margin:5px 0; }
  .ddrow label { display:flex; align-items:center; gap:4px; cursor:pointer; }
  .ddmenu select { background:var(--bg); color:var(--fg); border:1px solid #3a372f;
                   border-radius:4px; font:inherit; }
  /* ---- async-tune bench ---- */
  #main { flex:1; display:flex; min-height:0; }
  #tunepanel { display:none; width:330px; flex:none; overflow-y:auto; padding:8px 12px 16px;
               border-left:1px solid #2e2c26; background:var(--panel); }
  body.tuning #tunepanel { display:block; }
  #tune.on { color:var(--base); border-color:var(--base); }
  #tunepanel h3 { font-size:12px; color:var(--accent); margin:12px 0 4px; font-weight:bold;
                  text-transform:uppercase; letter-spacing:.06em; }
  #tunepanel h3:first-child { margin-top:2px; }
  .trow { display:flex; align-items:center; gap:6px; margin:3px 0; }
  .trow label { flex:1; color:var(--fg); font-size:12px; }
  .trow input[type=number], .trow select { width:96px; background:var(--bg); color:var(--fg);
      border:1px solid #3a372f; border-radius:4px; font:inherit; font-size:12px; padding:1px 4px; }
  .trow input[type=range] { width:96px; accent-color:var(--accent); }
  .trow input[type=checkbox] { accent-color:var(--accent); }
  .tag { font-size:9px; padding:0 3px; border-radius:3px; border:1px solid; opacity:.75;
         letter-spacing:.04em; }
  .tag.lab { color:#e88; border-color:#844; }
  .tag.bench { color:#8b8; border-color:#474; }
  .tag.wire { color:#89b; border-color:#456; }
  #tunepanel .bar { display:flex; gap:6px; margin:8px 0; flex-wrap:wrap; }
  #tunepanel button, #tunepanel select.wide { background:var(--bg); color:var(--fg);
      border:1px solid #3a372f; border-radius:6px; padding:3px 10px; font:inherit;
      font-size:12px; cursor:pointer; }
  #tunepanel button:hover { border-color:var(--accent); }
  #tunepanel select.wide { width:100%; cursor:pointer; }
  #metrics { font-size:11px; line-height:1.65; color:var(--chrome); white-space:pre-wrap;
             border:1px solid #2e2c26; border-radius:6px; background:var(--bg);
             margin:6px 0 2px; padding:6px 8px; }
  #metrics:empty { display:none; }
  #metrics b { color:var(--fg); font-weight:normal; }
  #metrics .warn { color:#e07a6a; }
  #tunemsg { font-size:11px; color:var(--accent); min-height:1.5em; }
  #tunemsg.same { color:var(--chrome); }
  /* A replay in flight. The panel dims rather than blocking, because the whole
     point is that a recompute is fast enough not to interrupt you. */
  body.replaying #tunepanel { opacity:.72; }
  #busy { display:none; color:var(--accent); }
  body.replaying #busy { display:inline; }
  #toast { color:var(--accent); }
  #toast.bad { color:#e07a6a; }
  #shelfdir.bad { color:#e07a6a; }
  .hint { font-size:10px; color:var(--chrome); margin:2px 0 6px; line-height:1.5; }
</style></head><body>
<header>
  <span class="logo">&#10035; slopsim</span><span>analyzer</span>
  <button id="live" class="on">live</button>
  <button id="fit">fit</button>
  <button id="cmd" class="on" title="commanded target (wire, normalized) mapped through the stroke window">cmd</button>
  <button id="raw" class="on" title="the SENDER&apos;S OWN CURVE through the knots - what the client asked for, before the planner">raw</button>
  <button id="clr" title="discard all captured samples and start over (also the cure for a session that has grown sluggish)">clear</button>
  <button id="tune" title="ASYNC TUNE: stop following the live machine and re-render a saved recording under settings you control. The sim keeps running untouched.">async tune</button>
  <button id="saveclip" title="save the commands in the VISIBLE range as a new recording. In live mode that clips what the machine just did; in tune mode it re-clips the recording being replayed.">save clip</button>
  <div class="expwrap">
    <button id="exp">csv &#9662;</button>
    <div id="expmenu" class="ddmenu">
      <div class="ddrow">
        <label><input type="radio" name="exprange" value="view" checked> view</label>
        <label><input type="radio" name="exprange" value="all"> all</label>
      </div>
      <div class="ddrow">
        <label>decimate
          <select id="expdecim">
            <option value="1">1x</option>
            <option value="10">10x</option>
            <option value="100">100x</option>
          </select>
        </label>
      </div>
      <div class="ddrow">
        <span id="expsize" class="dim"></span>
        <button id="expgo">export</button>
      </div>
    </div>
  </div>
  <button id="png">png</button>
  <span id="busy">recomputing...</span>
  <span id="toast"></span>
  <span id="stats"></span>
</header>
<div id="main">
  <div style="flex:1;display:flex;flex-direction:column;min-height:0;min-width:0">
    <div id="plots">
      <div id="poswrap"><canvas id="pos"></canvas></div>
      <div id="velwrap"><canvas id="vel"></canvas></div>
    </div>
    <div id="timelinewrap"><canvas id="timeline"></canvas></div>
  </div>
  <aside id="tunepanel"></aside>
</div>
<div id="readout"></div>
<footer><span class="k">wheel</span> zoom &nbsp; <span class="k">drag</span> pan &nbsp;
<span class="k">double-click</span> fit &nbsp; <span class="k">hover</span> inspect
&nbsp;&nbsp;<span class="dim">ASKED
<span style="color:var(--cmd)">&#9644;</span>cmd
<span style="color:var(--raw)">&#9644;</span>raw
&nbsp; MACHINE
<span style="color:var(--tgt)">&#9644;</span>tgt
<span style="color:var(--pos)">&#9644;</span>pos
<span style="color:var(--vel)">&#9644;</span>vel
<span style="color:var(--base)">&#9644;</span>base</span>
<span class="dim">&nbsp;&nbsp;cmd = wire targets, held &middot; raw = the curve they describe &middot;
tgt = the setpoint the machine chose (post planner, window clamp and ceilings) &middot;
pos = where the carriage measurably is.
<b>raw&ne;tgt is what the PLANNER would not deliver; tgt&ne;pos is what the FOLLOWER could not track.</b></span></footer>
<script>
"use strict";

// ============================================================================
// Column store — typed arrays, capacity-doubled. A plain JS array of doubles
// costs the same 8 B/element but carries per-array bookkeeping and can't be
// handed to the pyramid builder as a flat buffer; more importantly the OLD
// page kept t/pos/tgt/vel in four growing Arrays and then walked EVERY sample
// in view, every frame. The store is only half the fix — the pyramid is the
// other half.
// ============================================================================
let CAP = 1 << 16, N = 0;
let T = new Float64Array(CAP);   // sim seconds (f64: at t=3600 s an f32 ulp is
                                 // 0.43 ms, i.e. coarser than the 1 ms grid)
let P = new Float32Array(CAP);   // ACHIEVED position, mm
let G = new Float32Array(CAP);   // planner setpoint fed to the stepper, mm
let V = new Float32Array(CAP);   // ACHIEVED velocity, mm/s
let C = new Float32Array(CAP);   // COMMANDED target, NORMALIZED 0..1 (NaN = none yet)
let R = new Float32Array(CAP);   // SENDER CURVE, NORMALIZED 0..1 (NaN = none yet)
// BASELINE position, mm (NaN = no baseline loaded). A frozen run recalled from
// the shelf, resampled onto THIS store's grid at load time so that every
// per-frame cost stays O(canvas pixels) — it is a pyramided column like any
// other, never a second array walked while drawing.
let B = new Float32Array(CAP);
let COL = [P, G, V, C, R, B];    // pyramid series order; 3 and 4 are normalized
const SER_P = 0, SER_G = 1, SER_V = 2, SER_C = 3, SER_R = 4, SER_B = 5;
// Pyramided series count. Every bucket loop below reads THIS — adding a column
// means bumping one constant, not finding four hardcoded 4s.
const NSER = 6;

let meta = {rail:500, wmin:0, wmax:500};
let view = {t0:0, t1:12};
let live = true, dragging = false, dragX = 0, dragT0 = 0, dragT1 = 0;
let lastT = 0, lastRxPerf = 0;
const LIVE_LAG = 0.25;
let showCmd = true, showRaw = true;

function ensureCap(need){
  if (need <= CAP) return;
  let c = CAP; while (c < need) c *= 2;
  const t=new Float64Array(c), p=new Float32Array(c), g=new Float32Array(c),
        v=new Float32Array(c), m=new Float32Array(c), r=new Float32Array(c),
        b=new Float32Array(c);
  t.set(T.subarray(0,N)); p.set(P.subarray(0,N)); g.set(G.subarray(0,N));
  v.set(V.subarray(0,N)); m.set(C.subarray(0,N)); r.set(R.subarray(0,N));
  b.set(B.subarray(0,N));
  T=t; P=p; G=g; V=v; C=m; R=r; B=b; COL=[P,G,V,C,R,B]; CAP=c;
}

// ---- retention -------------------------------------------------------------
// WHY THIS EXISTS. The pyramid made DRAWING constant-cost, but the raw columns
// still grew without bound: ~28 B/sample across six series is ~100 MB per hour
// at 1 kHz, and each ensureCap doubling copies the whole capture again. That is
// the residual "it gets sluggish until I restart the machine" — restarting only
// helped because it threw the arrays away.
//
// MAXN bounds it: past the cap the OLDEST HALF is dropped and the pyramid is
// rebuilt once. That rebuild is O(N), but it happens once per MAXN/2 samples
// rather than every poll, so the amortized cost is constant and the session
// stops degrading. ~33 min of 1 kHz scrollback is kept, well past the sim's own
// 240 s ring, so nothing the sim can still serve is ever discarded early.
const MAXN = 2000000;
function compact(){
  const keep = MAXN >> 1, from = N - keep;
  T.copyWithin(0,from,N); P.copyWithin(0,from,N); G.copyWithin(0,from,N);
  V.copyWithin(0,from,N); C.copyWithin(0,from,N); R.copyWithin(0,from,N);
  B.copyWithin(0,from,N);
  N = keep;
  for (let l=0;l<MAXLV;l++) NB[l]=0;   // bucket boundaries all moved
  pyramidAppend(0);
}

// Drop everything. The next poll asks with since=-1 (because N is 0), so the
// sim replays whatever is still in its own ring and the view refills itself.
function resetCapture(){
  N = 0; lastT = 0;
  BASE = null;
  for (let l=0;l<MAXLV;l++){ NB[l]=0; MN[l]=null; MX[l]=null; BCAP[l]=0; }
  view.t0 = 0; view.t1 = 12; live = true;
  $("live").classList.toggle("on", true);
}

// ============================================================================
// Min/max pyramid — the reason this page stays smooth after an hour.
//
// Level 0: one bucket per BS0 raw samples, holding the EXACT {min,max} of that
// bucket for each series. Level l>0: one bucket per FAN buckets of level l-1,
// min-of-mins / max-of-maxes. Extremes are therefore exact at every level: a
// single-sample excursion is the max of its level-0 bucket, and stays the max
// all the way up. Nothing is averaged, nothing is sampled away.
//
// Build cost is incremental: new data only dirties the tail bucket of each
// level, so a poll costs O(new samples + levels), not O(total).
// Storage is ~0.7 B per sample (all levels, every series) — noise
// next to the 24 B/sample the raw columns already cost.
// ============================================================================
const BS0 = 32, FAN = 4, MAXLV = 14;
const BSZ = []; for (let l=0,b=BS0;l<MAXLV;l++,b*=FAN) BSZ.push(b);
const MN = [], MX = [], NB = [], BCAP = [];
for (let l=0;l<MAXLV;l++){ MN.push(null); MX.push(null); NB.push(0); BCAP.push(0); }

function ensureBuckets(l, need){
  if (need <= BCAP[l]) return;
  let c = Math.max(1024, BCAP[l] || 1024); while (c < need) c *= 2;
  const mn=[], mx=[];
  for (let s=0;s<NSER;s++){
    const a=new Float32Array(c), b=new Float32Array(c);
    if (MN[l]){ a.set(MN[l][s].subarray(0,NB[l])); b.set(MX[l][s].subarray(0,NB[l])); }
    mn.push(a); mx.push(b);
  }
  MN[l]=mn; MX[l]=mx; BCAP[l]=c;
}

// Rebuild every bucket that can contain a sample at or after `from`.
// NaN handling is free and deliberate: seeding lo=+Inf / hi=-Inf means a NaN
// fails both comparisons and is simply skipped, so the "no command yet" gap in
// C never drags the envelope to a value the machine was never asked for.
function pyramidAppend(from){
  if (N === 0) return;
  let d = (from / BS0) | 0;
  {
    const b1 = ((N-1) / BS0) | 0;
    ensureBuckets(0, b1+1);
    const mn=MN[0], mx=MX[0];
    for (let b=d;b<=b1;b++){
      const i0=b*BS0, i1=Math.min(N, i0+BS0);
      for (let s=0;s<NSER;s++){
        const col=COL[s]; let lo=Infinity, hi=-Infinity;
        for (let i=i0;i<i1;i++){ const v=col[i]; if(v<lo)lo=v; if(v>hi)hi=v; }
        mn[s][b]=lo; mx[s][b]=hi;
      }
    }
    NB[0]=b1+1;
  }
  for (let l=1;l<MAXLV;l++){
    const cb=NB[l-1];
    if (cb<=1){ NB[l]=0; break; }                 // no point above a single bucket
    const b1=((cb-1)/FAN)|0;
    d=(d/FAN)|0;
    ensureBuckets(l, b1+1);
    const pmn=MN[l-1], pmx=MX[l-1], mn=MN[l], mx=MX[l];
    for (let b=d;b<=b1;b++){
      const c0=b*FAN, c1=Math.min(cb, c0+FAN);
      for (let s=0;s<NSER;s++){
        let lo=Infinity, hi=-Infinity;
        const a=pmn[s], z=pmx[s];
        for (let c=c0;c<c1;c++){ if(a[c]<lo)lo=a[c]; if(z[c]>hi)hi=z[c]; }
        mn[s][b]=lo; mx[s][b]=hi;
      }
    }
    NB[l]=b1+1;
  }
}

// Largest level whose bucket is at most half a pixel wide, so every pixel
// column aggregates at least two buckets (no gaps) and the bucket overhang at
// a column edge is under half a column. -1 = draw from raw samples.
function pickLevel(samplesPerPixel){
  const want = samplesPerPixel * 0.5;
  for (let l=MAXLV-1;l>=0;l--) if (NB[l] > 0 && BSZ[l] <= want) return l;
  return -1;
}

// EXACT min/max over raw index range [i0,i1) — greedy largest-aligned-bucket
// decomposition, O(BS0 + levels) instead of O(range). Used for the visible-range
// stats and the velocity autoscale, which used to be two full linear scans of
// every visible sample on every single frame.
const _mm = [0,0];
function rangeMM(s, i0, i1){
  let lo=Infinity, hi=-Infinity;
  let i=Math.max(0,i0|0); const end=Math.min(N,i1|0);
  const col=COL[s];
  while (i < end){
    let l=-1;
    for (let k=MAXLV-1;k>=0;k--){
      const bs=BSZ[k];
      if (bs <= end-i && (i % bs) === 0 && NB[k] > (i/bs|0)) { l=k; break; }
    }
    if (l < 0){ const v=col[i]; if(v<lo)lo=v; if(v>hi)hi=v; i++; }
    else {
      const b=(i/BSZ[l])|0, a=MN[l][s][b], z=MX[l][s][b];
      if(a<lo)lo=a; if(z>hi)hi=z; i+=BSZ[l];
    }
  }
  _mm[0]=lo; _mm[1]=hi; return _mm;
}

const $ = id => document.getElementById(id);
const posC = $("pos"), velC = $("vel"), readout = $("readout");

// CSS custom properties are constants here; reading them through
// getComputedStyle inside draw() forced a style recalc four times a frame.
const _cs = getComputedStyle(document.body);
const CLR = {};
for (const k of ["pos","tgt","vel","cmd","raw","base","accent","bg"]) CLR[k]=_cs.getPropertyValue("--"+k).trim();

// ---- polling ---------------------------------------------------------------
async function poll(){
  // ASYNC TUNE owns the store while it is on: the live feed would append the
  // running machine's samples on top of a recomputed script and silently mix
  // two different motions into one set of lines.
  if (MODE === "tune"){ setTimeout(poll, 500); return; }
  const since = N ? T[N-1] : -1;
  try{
    const r = await fetch(`/api/trace.bin?since=${since}`);
    const buf = await r.arrayBuffer();
    if (buf.byteLength >= 16){
      const dv = new DataView(buf);
      const n = dv.getUint32(0,true);
      meta.rail=dv.getFloat32(4,true); meta.wmin=dv.getFloat32(8,true); meta.wmax=dv.getFloat32(12,true);
      // stride is self-describing from the 20-byte header; a 16-byte header is
      // an older sim and means the 4-column layout with no commanded column.
      let hdr=16, stride=4;
      if (buf.byteLength >= 20){ stride=dv.getUint32(16,true)||4; hdr=20; }
      if (n){
        const f = new Float32Array(buf, hdr, n*stride);
        const from = N;
        ensureCap(N+n);
        for (let i=0;i<n;i++){
          const o=i*stride, j=N+i;
          T[j]=f[o]; P[j]=f[o+1]; G[j]=f[o+2]; V[j]=f[o+3];
          C[j]= stride>4 ? (f[o+4]<0 ? NaN : f[o+4]) : NaN;
          // <0 is the sim's "no sender curve yet" sentinel, same convention as
          // cmd_norm; NaN keeps it out of the pyramid envelope entirely.
          R[j]= stride>5 ? (f[o+5]<0 ? NaN : f[o+5]) : NaN;
          B[j]= NaN;   // baseline is loaded separately, never streamed
        }
        N += n;
        // compact() rebuilds the whole pyramid itself, so the incremental
        // append is the ELSE branch, never both.
        if (N > MAXN) compact(); else pyramidAppend(from);
        lastT=T[N-1]; lastRxPerf=performance.now();
        dirty=true;
      }
    }
  }catch(e){ /* sim offline; keep whatever we have */ }
  setTimeout(poll, live?200:1000);
}

// ---- frame loop ------------------------------------------------------------
// Redraw is DEMAND-DRIVEN. The old loop called draw() on every animation frame
// forever, whether or not anything had changed; now interactions and new data
// raise `dirty` and the rAF loop paints only when there is something to paint.
let dirty = true;
function requestDraw(){ dirty = true; }
function forceDraw(){ dirty=false; draw(); }

function frame(){
  if (live && N && MODE === "live"){
    const span=view.t1-view.t0;
    const edge=Math.min(lastT, (lastT-LIVE_LAG)+(performance.now()-lastRxPerf)/1000);
    // A stalled feed pins `edge` at lastT: the view stops moving, so there is
    // nothing new to paint and the loop must go quiet rather than redraw the
    // same frame 60 times a second.
    if (Math.abs(edge-view.t1) > 1e-9){ view.t1=edge; view.t0=edge-span; dirty=true; }
  }
  if (dirty){ dirty=false; draw(); }
  requestAnimationFrame(frame);
}

function fit(){ if(N){ view.t0=T[0]; view.t1=T[N-1]; } requestDraw(); }

// index of first sample with t >= x (binary search; T is sorted)
function lower(x){ let a=0,b=N; while(a<b){ const m=(a+b)>>1; T[m]<x?a=m+1:b=m; } return a; }

// Canvas backing-store size is cached; getBoundingClientRect() in the draw path
// forces layout, and it was being called three times per frame.
const _sz = new WeakMap();
function sizeCanvas(c){
  let s=_sz.get(c);
  if (!s || s.stale){
    const r=c.parentElement.getBoundingClientRect(), d=devicePixelRatio||1;
    const w=Math.max(1,Math.round(r.width*d)), h=Math.max(1,Math.round(r.height*d));
    if (c.width!==w||c.height!==h){ c.width=w; c.height=h; }
    s={w,h,d,stale:false}; _sz.set(c,s);
  }
  return s;
}
function invalidateSizes(){ for (const c of [posC,velC,$("timeline")]){ const s=_sz.get(c); if(s) s.stale=true; } }

function niceStep(range,px,minPx){
  const target=range/Math.max(1,px/minPx);
  const p=Math.pow(10,Math.floor(Math.log10(target)));
  for (const m of [1,2,5,10]) if (m*p>=target) return m*p;
  return 10*p;
}

// Column -> raw-index boundaries for a given canvas width and time range.
// Every series on a canvas needs the same w+1 boundaries, and both plots share
// a view, so this is computed once per (width, range) and reused: w+1 binary
// searches a frame instead of 2 per column per series. Two caches, because the
// minimap always looks at the whole session while the plots look at `view`.
function _bounds(cache, w, t0, t1){
  if (cache.w===w && cache.t0===t0 && cache.t1===t1 && cache.n===N) return cache.a;
  if (!cache.a || cache.a.length!==w+1) cache.a=new Int32Array(w+1);
  const a=cache.a, span=t1-t0;
  for (let x=0;x<=w;x++) a[x]=lower(t0+span*x/w);
  cache.w=w; cache.t0=t0; cache.t1=t1; cache.n=N;
  return a;
}
const _cbPlot={w:-1,t0:NaN,t1:NaN,n:-1,a:null};
const _cbMini={w:-1,t0:NaN,t1:NaN,n:-1,a:null};

// ---- series drawing --------------------------------------------------------
// Per-pixel min/max envelope, sourced from the pyramid when zoomed out and from
// raw samples when zoomed in far enough that raw IS cheap. Either way the
// column's drawn extremes are the true extremes of the samples under it.
// `mul`/`add` map a stored value into plot units (the commanded series is
// stored NORMALIZED and mapped through the live stroke window right here — the
// mapping is the thing under test, so it happens at draw time, not at ingest).
function drawSeries(ctx,w,h,s,y0,y1,color,lw,dash,mul,add){
  const span=view.t1-view.t0; if(span<=0||N===0) return;
  mul = (mul===undefined)?1:mul; add = (add===undefined)?0:add;
  const i0=lower(view.t0), i1=lower(view.t1);
  if (i1<=i0) return;
  const lvl=pickLevel((i1-i0)/Math.max(1,w));
  const ky=h/(y1-y0);
  const kx=w/span, x0=view.t0;

  ctx.strokeStyle=color; ctx.lineWidth=lw;
  if (dash) ctx.setLineDash(dash); else ctx.setLineDash([]);
  ctx.beginPath();

  // STROKED, not filled, and that was MEASURED both ways. Emitting the same
  // min/max envelope as one filled polygon rasterizes a 4.7x smaller path in
  // isolation but is SLOWER in situ (42.6 ms -> 51.8 ms/frame): zoomed out the
  // envelope spans most of the canvas height, so a fill paints the whole band
  // while a stroke paints only its 2 px outline. The bottleneck here is fill
  // RATE on a 2.3 MP canvas, not path complexity — building every path in JS is
  // 0.67 ms of a 51.8 ms frame.
  let started=false, prevY=0, curX=-1, lo=Infinity, hi=-Infinity;
  const flush=()=>{
    if (curX<0 || !isFinite(lo)) return;
    const yl=h-(lo*mul+add-y0)*ky, yh=h-(hi*mul+add-y0)*ky;
    if(!started){ ctx.moveTo(curX+0.5,(yl+yh)/2); started=true; }
    else ctx.lineTo(curX+0.5,prevY);
    ctx.lineTo(curX+0.5,yh); ctx.lineTo(curX+0.5,yl);
    prevY=(yl+yh)/2;
  };

  if (lvl < 0){
    const col=COL[s];
    for (let i=i0;i<i1;i++){
      let x=((T[i]-x0)*kx)|0; if(x<0)x=0; else if(x>=w)x=w-1;
      if (x!==curX){ flush(); curX=x; lo=Infinity; hi=-Infinity; }
      const v=col[i]; if(v<lo)lo=v; if(v>hi)hi=v;
    }
  } else {
    // COLUMN-DRIVEN, not bucket-driven, and that distinction is load-bearing:
    // each column takes every bucket that OVERLAPS its time range, so the drawn
    // envelope is a strict SUPERSET of the column's true extremes. Assigning a
    // bucket to the column of its start time instead loses the guarantee — a
    // one-sample excursion late in a bucket that straddles a column boundary
    // gets drawn one pixel away from where it happened. (Caught by the
    // spike-injection acceptance test: 11 of 12 injected single-sample spikes
    // landed in their own column, one landed next door.) Overlap costs an extra
    // bucket per column and buys exactness back.
    const bs=BSZ[lvl], mn=MN[lvl][s], mx=MX[lvl][s], nbk=NB[lvl];
    const cb=_bounds(_cbPlot, w, view.t0, view.t1);
    for (let x=0;x<w;x++){
      const ia=cb[x], ib=cb[x+1];
      if (ib<=ia) continue;
      let b=(ia/bs)|0; const b1=Math.min(nbk-1, ((ib-1)/bs)|0);
      if (b>b1) continue;
      curX=x; lo=Infinity; hi=-Infinity;
      for (;b<=b1;b++){ if(mn[b]<lo)lo=mn[b]; if(mx[b]>hi)hi=mx[b]; }
      flush();
      curX=-1;                       // already flushed; don't flush twice below
    }
  }
  flush();
  ctx.stroke();
  ctx.setLineDash([]);
}

function gridX(ctx,w,h,d){
  const span=view.t1-view.t0, step=niceStep(span,w,90*d);
  ctx.strokeStyle="#2b2922"; ctx.fillStyle="#6b675c"; ctx.font=`${11*d}px ui-monospace`;
  for(let t=Math.ceil(view.t0/step)*step; t<=view.t1; t+=step){
    const x=(t-view.t0)/span*w;
    ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke();
    ctx.fillText(t.toFixed(step<1?2:step<10?1:0)+"s", x+4*d, h-6*d);
  }
}

function gridY(ctx,w,h,d,y0,y1,unit){
  const step=niceStep(y1-y0,h,34*d);
  ctx.strokeStyle="#2b2922"; ctx.fillStyle="#6b675c"; ctx.font=`${11*d}px ui-monospace`;
  for(let v=Math.ceil(y0/step)*step; v<=y1; v+=step){
    const y=h-(v-y0)/(y1-y0)*h;
    ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke();
    ctx.fillText(v.toFixed(step<1?1:0)+unit, 6*d, y-3*d);
  }
}

function visVelRange(){
  const r=rangeMM(SER_V, lower(view.t0), lower(view.t1));
  const m=Math.max(50, Math.abs(r[0]), Math.abs(r[1]));
  return m*1.15;
}

// ---- session minimap -------------------------------------------------------
// Was a full re-bucket of EVERY sample, gated to "at most once a second" —
// which at 1.8 M samples is a ~20 ms stall once a second, forever. Now it is a
// pyramid walk like every other series: O(pixels), cheap enough to do per frame
// with no cache and no staleness window.
function drawTimeline(){
  const c=$("timeline"); const {w,h,d}=sizeCanvas(c); const ctx=c.getContext("2d");
  ctx.clearRect(0,0,w,h);
  if (!N) return;
  const t0=T[0], t1=T[N-1], span=Math.max(t1-t0,1e-6);
  const lvl=pickLevel(N/Math.max(1,w));
  ctx.strokeStyle=CLR.pos; ctx.lineWidth=1*d; ctx.globalAlpha=0.6; ctx.beginPath();
  let curX=-1, lo=Infinity, hi=-Infinity;
  const flush=()=>{
    if (curX<0||!isFinite(lo)) return;
    ctx.moveTo(curX+0.5, h-(lo/meta.rail)*h); ctx.lineTo(curX+0.5, h-(hi/meta.rail)*h);
  };
  if (lvl<0){
    for (let i=0;i<N;i++){
      let x=((T[i]-t0)/span*w)|0; if(x<0)x=0; else if(x>=w)x=w-1;
      if(x!==curX){ flush(); curX=x; lo=Infinity; hi=-Infinity; }
      const v=P[i]; if(v<lo)lo=v; if(v>hi)hi=v;
    }
  } else {
    // column-driven, for the same superset guarantee as drawSeries
    const bs=BSZ[lvl], mn=MN[lvl][SER_P], mx=MX[lvl][SER_P], nbk=NB[lvl];
    const cb=_bounds(_cbMini, w, t0, t1);
    for (let x=0;x<w;x++){
      const ia=cb[x], ib=(x===w-1)?N:cb[x+1];
      if (ib<=ia) continue;
      let b=(ia/bs)|0; const b1=Math.min(nbk-1, ((ib-1)/bs)|0);
      if (b>b1) continue;
      curX=x; lo=Infinity; hi=-Infinity;
      for (;b<=b1;b++){ if(mn[b]<lo)lo=mn[b]; if(mx[b]>hi)hi=mx[b]; }
      flush();
      curX=-1;
    }
  }
  flush();
  ctx.stroke(); ctx.globalAlpha=1;
  const vx0=Math.min(w,Math.max(0,(view.t0-t0)/span*w));
  const vx1=Math.min(w,Math.max(0,(view.t1-t0)/span*w));
  ctx.fillStyle="rgba(232,149,107,0.18)";
  ctx.fillRect(vx0,0,Math.max(1*d,vx1-vx0),h);
  ctx.strokeStyle=CLR.accent; ctx.lineWidth=1.5*d;
  ctx.strokeRect(vx0+0.75*d,0.75*d,Math.max(1*d,vx1-vx0-1.5*d),h-1.5*d);
}
function tlRange(){ return N?{t0:T[0],t1:T[N-1]}:{t0:0,t1:1}; }

function draw(){
  { const {w,h,d}=sizeCanvas(posC); const ctx=posC.getContext("2d");
    ctx.clearRect(0,0,w,h);
    const yA=h-(meta.wmin/meta.rail)*h, yB=h-(meta.wmax/meta.rail)*h;
    ctx.fillStyle="rgba(126,166,205,0.07)"; ctx.fillRect(0,yB,w,yA-yB);
    gridX(ctx,w,h,d); gridY(ctx,w,h,d,0,meta.rail,"mm");
    ctx.strokeStyle="#3d5a75"; ctx.setLineDash([4*d,4*d]);
    ctx.beginPath(); ctx.moveTo(0,yA); ctx.lineTo(w,yA); ctx.moveTo(0,yB); ctx.lineTo(w,yB); ctx.stroke();
    ctx.setLineDash([]);
    // ---- STACKING ORDER IS THE ARGUMENT ---------------------------------
    // Bottom to top: what was ASKED, then what the machine PLANNED, then what
    // it MEASURED. Each layer is what the one under it turned into, so the
    // topmost line is always the answer and never something the operator has
    // to hunt for behind a fatter stroke.
    //
    // COMMANDED, normalized -> window. FAT and FAINT: a wide low-alpha halo,
    // not a line. Perfect agreement reads as a soft purple band the machine's
    // line sits inside; divergence separates into two distinct bands. It is
    // deliberately the least intense thing on the canvas — it is context, not
    // a measurement, and at full strength it dominated the plot.
    //
    // It is also not dashed, which is a PERFORMANCE constraint rather than a
    // taste one: a dashed stroke over ~1500 min/max columns is measurably the
    // most expensive thing on this canvas under software rasterization
    // (176 ms/frame vs 17 ms solid, headless, 3.6 M samples). The halo carries
    // the same meaning for free.
    if (showCmd){
      ctx.globalAlpha=0.30;
      drawSeries(ctx,w,h,SER_C,0,meta.rail,CLR.cmd,3.6*d,null,
                 (meta.wmax-meta.wmin), meta.wmin);
      ctx.globalAlpha=1;
    }
    // RAW — the sender's own curve through those points. Same asked-family
    // purple, drawn as a real line because it IS the reference the machine is
    // judged against.
    if (showRaw){
      ctx.globalAlpha=0.75;
      drawSeries(ctx,w,h,SER_R,0,meta.rail,CLR.raw,1.3*d,null,
                 (meta.wmax-meta.wmin), meta.wmin);
      ctx.globalAlpha=1;
    }
    // BASELINE — a frozen run recalled from the shelf. Dashed and drawn under
    // the live position so the two read as "then" and "now": where a tuning
    // change did nothing the baseline hides completely beneath it.
    if (BASE) drawSeries(ctx,w,h,SER_B,0,meta.rail,CLR.base,1.4*d,[5*d,4*d]);
    // PLANNED then MEASURED, both machine-side blue. Where the follower tracked
    // its setpoint the deeper blue vanishes under the brighter one, so any
    // visible tgt IS following error.
    drawSeries(ctx,w,h,SER_G,0,meta.rail,CLR.tgt,1.2*d);
    drawSeries(ctx,w,h,SER_P,0,meta.rail,CLR.pos,2.0*d);
  }
  { const {w,h,d}=sizeCanvas(velC); const ctx=velC.getContext("2d");
    ctx.clearRect(0,0,w,h);
    const vr=visVelRange();
    gridX(ctx,w,h,d); gridY(ctx,w,h,d,-vr,vr,"");
    ctx.strokeStyle="#3a372f"; ctx.beginPath(); ctx.moveTo(0,h/2); ctx.lineTo(w,h/2); ctx.stroke();
    drawSeries(ctx,w,h,SER_V,-vr,vr,CLR.vel,1.2*d);
  }
  drawTimeline();
  const i0=lower(view.t0), i1=lower(view.t1);
  if (i1>i0){
    const rv=rangeMM(SER_V,i0,i1), pk=Math.max(Math.abs(rv[0]),Math.abs(rv[1]));
    const rp=rangeMM(SER_P,i0,i1);
    $("stats").textContent=`${(view.t1-view.t0).toFixed(2)}s · ${i1-i0} samples · pos ${rp[0].toFixed(1)}–${rp[1].toFixed(1)}mm · peak ${pk.toFixed(0)}mm/s`;
  }
}

// ---- interactions ----------------------------------------------------------
for (const c of [posC,velC]){
  c.addEventListener("wheel",ev=>{
    ev.preventDefault();
    const r=c.getBoundingClientRect(), fx=(ev.clientX-r.left)/r.width;
    const span=view.t1-view.t0, at=view.t0+span*fx;
    const z=ev.deltaY>0?1.25:0.8, ns=Math.min(Math.max(span*z,0.05),36000);
    view.t0=at-ns*fx; view.t1=at+ns*(1-fx); live=false; $("live").classList.remove("on");
    requestDraw();
  },{passive:false});
  c.addEventListener("mousedown",ev=>{ dragging=true; dragX=ev.clientX; dragT0=view.t0; dragT1=view.t1; });
  c.addEventListener("mousemove",ev=>{
    if (dragging){
      const r=c.getBoundingClientRect(), dt=(ev.clientX-dragX)/r.width*(dragT1-dragT0);
      view.t0=dragT0-dt; view.t1=dragT1-dt; live=false; $("live").classList.remove("on");
      requestDraw();
    }
    const r=c.getBoundingClientRect(), fx=(ev.clientX-r.left)/r.width;
    const t=view.t0+(view.t1-view.t0)*fx, i=Math.min(lower(t),N-1);
    if (i>=0&&N){
      const cn=C[i];
      const cmm=isNaN(cn)?null:(meta.wmin+cn*(meta.wmax-meta.wmin));
      readout.style.display="block";
      readout.style.left=(ev.clientX+16)+"px"; readout.style.top=(ev.clientY+12)+"px";
      readout.textContent=`t   ${T[i].toFixed(3)}s\npos ${P[i].toFixed(2)}mm\ntgt ${G[i].toFixed(2)}mm\n`
        +`cmd ${cmm===null?"   —":cmm.toFixed(2)+"mm"}${cmm===null?"":"  ("+cn.toFixed(4)+" norm)"}\n`
        +`vel ${V[i].toFixed(1)}mm/s`;
    }
  });
  c.addEventListener("mouseleave",()=>{ dragging=false; readout.style.display="none"; });
  c.addEventListener("mouseup",()=>{ dragging=false; });
  c.addEventListener("dblclick",fit);
}

const tlC=$("timeline");
let tlDragging=false, tlDragX=0, tlDragT0=0, tlDragT1=0;
tlC.addEventListener("mousedown",ev=>{
  const r=tlC.getBoundingClientRect(), {t0,t1}=tlRange(), span=Math.max(t1-t0,1e-6);
  const fx=(ev.clientX-r.left)/r.width;
  const vx0=(view.t0-t0)/span, vx1=(view.t1-t0)/span;
  if (fx<vx0||fx>vx1){
    const vs=view.t1-view.t0, at=t0+span*Math.min(Math.max(fx,0),1);
    view.t0=at-vs/2; view.t1=at+vs/2; live=false; $("live").classList.remove("on");
  }
  tlDragging=true; tlDragX=ev.clientX; tlDragT0=view.t0; tlDragT1=view.t1;
  requestDraw();
});
tlC.addEventListener("mousemove",ev=>{
  if (!tlDragging) return;
  const r=tlC.getBoundingClientRect(), {t0,t1}=tlRange(), span=Math.max(t1-t0,1e-6);
  const dt=(ev.clientX-tlDragX)/r.width*span;
  view.t0=tlDragT0+dt; view.t1=tlDragT1+dt; live=false; $("live").classList.remove("on");
  requestDraw();
});
tlC.addEventListener("mouseup",()=>{ tlDragging=false; });
tlC.addEventListener("mouseleave",()=>{ tlDragging=false; });

$("live").onclick=()=>{ live=!live; $("live").classList.toggle("on",live); requestDraw(); };
$("fit").onclick=fit;
$("cmd").onclick=()=>{ showCmd=!showCmd; $("cmd").classList.toggle("on",showCmd); requestDraw(); };
$("raw").onclick=()=>{ showRaw=!showRaw; $("raw").classList.toggle("on",showRaw); requestDraw(); };
$("clr").onclick=()=>{ resetCapture(); requestDraw(); };

// ---- range export ----------------------------------------------------------
const expBtn=$("exp"), expMenu=$("expmenu");
function exportSel(){
  const range=document.querySelector('input[name="exprange"]:checked').value;
  const decim=parseInt($("expdecim").value,10)||1;
  const i0 = range==="view" ? Math.min(N,Math.max(0,lower(view.t0))) : 0;
  const i1 = range==="view" ? Math.min(N,lower(view.t1)) : N;
  return {range,decim,i0,i1};
}
function updateExportEstimate(){
  const sel=exportSel();
  const rows=Math.max(0,Math.ceil((sel.i1-sel.i0)/sel.decim));
  const bytes=rows*38;
  const size=bytes<1024?bytes+"B":bytes<1048576?(bytes/1024).toFixed(1)+"KB":(bytes/1048576).toFixed(1)+"MB";
  $("expsize").textContent=`${rows.toLocaleString()} rows ~ ${size}`;
}
expBtn.onclick=ev=>{ ev.stopPropagation(); expMenu.classList.toggle("open"); updateExportEstimate(); };
expMenu.addEventListener("click",ev=>ev.stopPropagation());
expMenu.addEventListener("change",updateExportEstimate);
addEventListener("click",()=>expMenu.classList.remove("open"));
$("expgo").onclick=()=>{
  const sel=exportSel();
  const span=meta.wmax-meta.wmin;
  const parts=["t_s,pos_mm,tgt_mm,vel_mm_s,cmd_norm,cmd_mm,raw_norm,raw_mm\n"];
  for(let i=sel.i0;i<sel.i1;i+=sel.decim){
    const cn=C[i], ok=!isNaN(cn);
    const rn=R[i], rok=!isNaN(rn);
    parts.push(`${T[i].toFixed(3)},${P[i].toFixed(3)},${G[i].toFixed(3)},${V[i].toFixed(2)},`
      +`${ok?cn.toFixed(4):""},${ok?(meta.wmin+cn*span).toFixed(3):""},${rok?rn.toFixed(4):""},${rok?(meta.wmin+rn*span).toFixed(3):""}\n`);
  }
  const name=`slopsim-trace-${sel.range}${sel.decim>1?"-"+sel.decim+"x":""}.csv`;
  const a=document.createElement("a");
  a.href=URL.createObjectURL(new Blob(parts,{type:"text/csv"})); a.download=name; a.click();
  expMenu.classList.remove("open");
};
$("png").onclick=()=>{
  const d=devicePixelRatio||1, w=posC.width, h=posC.height+velC.height+8*d;
  const c=document.createElement("canvas"); c.width=w; c.height=h;
  const ctx=c.getContext("2d");
  ctx.fillStyle=CLR.bg||"#1a1915"; ctx.fillRect(0,0,w,h);
  ctx.drawImage(posC,0,0); ctx.drawImage(velC,0,posC.height+8*d);
  const a=document.createElement("a"); a.href=c.toDataURL("image/png"); a.download="slopsim-trace.png"; a.click();
};
addEventListener("resize",()=>{ invalidateSizes(); requestDraw(); });
if (window.ResizeObserver){
  const ro=new ResizeObserver(()=>{ invalidateSizes(); requestDraw(); });
  ro.observe($("poswrap")); ro.observe($("velwrap")); ro.observe($("timelinewrap"));
}

// Bench/QA hook: grow the client store the way an N-hour session would, by
// tiling the data already held (spikes and all) forward in time. Never called
// by the UI; it exists so a performance claim about this page can be MEASURED
// at a realistic sample count instead of asserted.
function __growForBench(target){
  if (!N) return 0;
  const n0=N, dt=(T[n0-1]-T[0])/Math.max(1,n0-1), span=(T[n0-1]-T[0])+dt;
  let k=0;
  while (N < target){
    k++;
    const base=span*k, room=Math.min(n0, target-N), from=N;
    ensureCap(N+room);
    for (let i=0;i<room;i++){
      const j=N+i; T[j]=T[i]+base; P[j]=P[i]; G[j]=G[i]; V[j]=V[i]; C[j]=C[i];
    }
    N+=room;
    pyramidAppend(from);
  }
  lastT=T[N-1]; dirty=true;
  return N;
}
window.__growForBench=__growForBench;

// ============================================================================
// ASYNC TUNE — the bench.
//
// WHAT IT IS. Live mode follows the running machine at 1:1. Tune mode stops
// following and instead re-renders a SAVED RECORDING (a wire log of real
// 0x0084/0x0085 commands) through a fresh slopmotion engine under settings this
// panel owns, as fast as the host will go. The sim keeps running the whole time
// and never sees any of it - nothing here goes over SlopSync, nothing here
// writes machine state. It is a laboratory, not a remote control.
//
// WHY THE WHOLE SCRIPT RE-RUNS ON EVERY EDIT rather than seeking to the visible
// region: engine state is not memoryless. Most of it decays within a segment
// (each plan is rebuilt from actual state), but the CENTERING DEBT accumulates
// across strokes, so a window entered mid-take would render a centering
// behavior the machine would never have had. Measured at ~35000x realtime, so
// simulating from the top and emitting only the window costs nothing worth
// saving.
//
// TWO KINDS OF THING ON THE SHELF, and the difference is load-bearing:
//   recording - a wire log. REPLAYS: re-runs under whatever the engine does now.
//   run       - frozen samples plus the settings that made them. RECALLS: the
//               stored points are drawn with no engine involved, which is what
//               makes it usable as a regression reference. A recomputed
//               baseline is not a baseline.
// ============================================================================
let MODE = "live";        // "live" | "tune"
let REC = null;           // selected recording name
let BASE = null;          // {t0ms, pos:Float32Array} - recalled run, by ms
let SHELF = [];           // /api/recordings listing
let shelfDir = "";
let scopeView = false;    // emit only the visible range (metrics follow it)
let lastSig = null;       // signature of the last replay, for "output identical"

// The control surface. ONE table drives the panel markup, the query string, and
// the reach tags - adding a knob is one row here, never four edits.
//
// `reach` answers "could I set this on the real machine?", which is half the
// point of running the bench at all:
//   wire  - reachable today over SlopSync (and via /api/slopmotion on device).
//   lab   - exists in slopmotion::Config and NOWHERE ELSE. No wire channel, no
//           HTTP field, no CLI flag. If one of these turns out to matter, that
//           finding IS the deliverable: it is a knob that needs exposing.
//   bench - a property of the replay itself, not a machine setting. Never ships.
const TUNE = [
 ["Waveform", [
  {k:"curve", lab:"machine curve policy", t:"sel", o:["follow","c1","c2"], reach:"wire"},
  // What the SENDER declared (RFC-030), for what-if runs. "recording" = replay
  // it as it was actually sent. Only bites under policy `follow`.
  {k:"client_curve", lab:"client declared (follow only)", t:"sel",
   o:["recording","unspecified","c1","c2","step"], reach:"bench"},
  {k:"policy", lab:"infeasible policy", t:"sel",
   o:["blend","reshape","scale","stretch","prio-amplitude","prio-smooth"], reach:"wire"},
  // THE one slider: what an infeasible segment gives up. 0 = surrender reach and
  // keep the sender's shape, 1 = keep reach and flatten toward the chord. Only
  // bites under policy `blend`; the other four spend one axis to exhaustion.
  {k:"blend", lab:"amplitude ↔ shape (blend only)", t:"rng", min:0, max:1, step:0.025, reach:"wire"},
  {k:"scale_margin", lab:"scale margin", t:"num", min:0.5, max:1, step:0.01, reach:"wire"},
  {k:"reshape_steps", lab:"reshape steps", t:"num", min:0, max:8, step:1, reach:"wire"},
  {k:"smooth_budget", lab:"smooth budget", t:"rng", min:0, max:1, step:0.01, reach:"wire"},
  {k:"amplitude_budget", lab:"amplitude budget", t:"rng", min:0, max:1, step:0.01, reach:"wire"},
  {k:"blend_steps", lab:"blend steps", t:"num", min:1, max:10, step:1, reach:"wire"}
 ]],
 ["Sharpness", [
  {k:"soften", lab:"soften", t:"chk", reach:"lab"},
  {k:"soften_floor", lab:"soften floor", t:"num", min:0.001, max:1, step:0.001, reach:"lab"},
  {k:"soften_steps", lab:"soften steps", t:"num", min:0, max:10, step:1, reach:"lab"}
 ]],
 ["Handoff &amp; centering", [
  {k:"handoff_chord", lab:"handoff chord k", t:"num", min:0, max:8, step:0.1, reach:"lab"},
  {k:"overshoot_guard", lab:"overshoot guard (0=off)", t:"num", min:0, max:20, step:0.1, reach:"lab"},
  {k:"chord_slack", lab:"overshoot chord slack", t:"num", min:0, max:5, step:0.05, reach:"lab"},
  {k:"bridge_ratio", lab:"bad-move bridge ratio (0=off)", t:"num", min:0, max:50, step:0.5, reach:"lab"},
  {k:"wave_centering", lab:"centering", t:"chk", reach:"wire"},
  {k:"wave_centering_gain", lab:"centering gain", t:"rng", min:0, max:1, step:0.01, reach:"wire"},
  {k:"settle_grace_ms", lab:"settle grace ms", t:"num", min:0, max:200, step:1, reach:"wire"}
 ]],
 ["Chase", [
  {k:"chase_ff", lab:"feedforward", t:"chk", reach:"wire"},
  {k:"chase_gain", lab:"ff gain", t:"num", min:0, max:2, step:0.05, reach:"wire"},
  {k:"chase_lookahead", lab:"lookahead", t:"num", min:0, max:20, step:0.25, reach:"wire"},
  {k:"chase_accel_ff", lab:"accel ff", t:"chk", reach:"wire"},
  {k:"chase_aim_extrap", lab:"aim extrap", t:"chk", reach:"wire"},
  {k:"chase_dense_ms", lab:"dense ms", t:"num", min:1, max:1000, step:1, reach:"wire"},
  {k:"chase_stale_ms", lab:"stale ms", t:"num", min:1, max:5000, step:10, reach:"wire"}
 ]],
 ["Ceilings (normalized)", [
  // 0 = DERIVE from the window span, exactly as the machine does. A non-zero
  // value is an OVERRIDE and the ceiling warnings below police it. Defaulting
  // these to the live sim's derived numbers was the bug: moving the window in
  // this panel then did NOT move the ceilings, so the bench planned at a
  // machine that does not exist.
  {k:"input_jerk", lab:"input jerk (mm/s3)", t:"num", min:1, max:100000000, step:100000, reach:"wire"},
  {k:"vmax", lab:"vmax norm (0=derive)", t:"num", min:0, max:100, step:0.05, reach:"wire"},
  {k:"amax", lab:"amax norm (0=derive)", t:"num", min:0, max:5000, step:1, reach:"wire"},
  {k:"jmax", lab:"jmax norm (0=derive)", t:"num", min:0, max:1000000, step:100, reach:"wire"}
 ]],
 ["Machine", [
  {k:"win_min", lab:"window min mm", t:"num", min:0, max:2000, step:1, reach:"wire"},
  {k:"win_max", lab:"window max mm", t:"num", min:0, max:2000, step:1, reach:"wire"},
  {k:"ceiling", lab:"hard ceiling mm", t:"num", min:0, max:2000, step:1, reach:"wire"},
  {k:"input_speed", lab:"input speed", t:"num", min:1, max:10000, step:10, reach:"wire"},
  {k:"input_accel", lab:"input accel", t:"num", min:1, max:100000, step:100, reach:"wire"},
  {k:"user_speed", lab:"user speed", t:"num", min:1, max:10000, step:10, reach:"wire"},
  {k:"user_accel", lab:"user accel", t:"num", min:1, max:100000, step:100, reach:"wire"},
  {k:"matched", lab:"velocity-matched feed", t:"chk", reach:"wire"},
  {k:"gentle_accel_outside", lab:"gentle ACCEL outside window", t:"chk", reach:"lab"},
  {k:"safety_filter", lab:"safety filter (braking invariant)", t:"chk", reach:"lab"}
 ]],
 ["Bench", [
  {k:"p0", lab:"start pos (norm, -1 = first target)", t:"num", min:-1, max:1, step:0.01, reach:"bench"},
  {k:"tail_s", lab:"tail seconds", t:"num", min:0, max:30, step:0.5, reach:"bench"}
 ]]
];
const TUNE_FLAT = TUNE.flatMap(g => g[1]);

// Seeded from the sim's LIVE config on first open, so the panel starts on what
// the machine is actually running rather than on compile-time defaults.
let tuneVals = {};

function buildPanel(){
  const el = $("tunepanel");
  let h = '<h3>recording</h3><select id="recsel" class="wide"></select>'
        + '<div class="hint">a <b>recording</b> replays through the engine; a '
        + '<b>run</b> recalls frozen points and cannot change.<br>'
        + 'shelf: <span id="shelfdir"></span></div>'
        + '<div class="bar"><button id="reload">refresh</button>'
        + '<button id="scope" title="emit and measure only the visible range. '
        + 'The whole script is always SIMULATED - see the centering-debt note.">scope: all</button></div>'
        + '<h3>baseline</h3><select id="basesel" class="wide"></select>'
        + '<div class="hint">a saved run drawn under the live line, so you can see '
        + 'what a change did rather than only where it ended up.</div>'
        + '<div class="bar"><button id="saverun">freeze this as a run</button></div>'
        + '<div id="tunemsg"></div>'
        + '<div id="metrics"></div>';
  for (const grp of TUNE){
    h += "<h3>" + grp[0] + "</h3>";
    for (const c of grp[1]){
      const tag = '<span class="tag ' + c.reach + '">' + c.reach + '</span>';
      let inp;
      if (c.t === "sel")
        inp = '<select data-k="'+c.k+'">' + c.o.map(o=>"<option>"+o+"</option>").join("") + '</select>';
      else if (c.t === "chk")
        inp = '<input type="checkbox" data-k="'+c.k+'">';
      else if (c.t === "rng")
        inp = '<input type="range" data-k="'+c.k+'" min="'+c.min+'" max="'+c.max+'" step="'+c.step+'">'
            + '<span class="dim" id="v_'+c.k+'"></span>';
      else
        inp = '<input type="number" data-k="'+c.k+'" min="'+c.min+'" max="'+c.max+'" step="'+c.step+'">';
      h += '<div class="trow"><label title="'+c.k+'">'+c.lab+'</label>'+tag+inp+'</div>';
    }
  }
  el.innerHTML = h;

  el.querySelectorAll("[data-k]").forEach(i=>{
    i.addEventListener("input", ()=>{
      const k = i.dataset.k;
      tuneVals[k] = i.type === "checkbox" ? (i.checked?1:0) : i.value;
      const v = $("v_"+k); if (v) v.textContent = (+i.value).toFixed(2);
      scheduleReplay();
    });
  });
  $("reload").onclick = ()=>loadShelf(true);
  $("recsel").onchange = ()=>{ REC = $("recsel").value || null; lastSig = null; runReplay(true); };
  $("basesel").onchange = ()=>loadBaseline($("basesel").value);
  $("scope").onclick = ()=>{
    scopeView = !scopeView;
    $("scope").textContent = "scope: " + (scopeView?"view":"all");
    $("scope").classList.toggle("on", scopeView);
    runReplay(false);
  };
  $("saverun").onclick = saveRun;
}

function applyVals(){
  for (const c of TUNE_FLAT){
    const i = document.querySelector('#tunepanel [data-k="'+c.k+'"]');
    if (!i || !(c.k in tuneVals)) continue;
    if (i.type === "checkbox") i.checked = +tuneVals[c.k] !== 0;
    else i.value = tuneVals[c.k];
    const v = $("v_"+c.k); if (v) v.textContent = (+tuneVals[c.k]).toFixed(2);
  }
}

// Seed from the sim's own /api/slopmotion tuning block. Keys that endpoint does
// not carry (the `lab` ones, and the geometry) fall back to the engine's own
// documented defaults, and the panel says which is which via the tag rather
// than pretending the machine reported them.
async function seedFromSim(){
  const d = {soften:1, soften_floor:0.02, soften_steps:6, handoff_chord:1.5,
             chase_stale_ms:400, p0:-1, tail_s:1, overshoot_guard:1, chord_slack:0.25, bridge_ratio:0,
             client_curve:"recording", blend:0.5,
             // 0 = derive from the window, which is what the machine does.
             vmax:0, amax:0, jmax:0, input_jerk:2000000,
             win_min:0, win_max:500, input_speed:1000, input_accel:50000,
             user_speed:100, user_accel:2000, matched:0, gentle_accel_outside:1,
             safety_filter:0};
  try{
    const j = await (await fetch("/api/slopmotion")).json();
    const t = j.tuning || {};
    Object.assign(d, {
      curve:t.curve_policy, policy:t.infeasible_policy,
      scale_margin:t.infeasible_scale_margin, reshape_steps:t.reshape_steps,
      smooth_budget:t.smooth_budget, amplitude_budget:t.amplitude_budget,
      blend_steps:t.blend_steps, settle_grace_ms:t.settle_grace_ms,
      chase_ff:t.chase_ff?1:0, chase_gain:t.chase_gain,
      chase_lookahead:t.chase_lookahead, chase_accel_ff:t.chase_accel_ff?1:0,
      chase_aim_extrap:t.chase_aim_accel_extrap?1:0, chase_dense_ms:t.chase_dense_ms,
      wave_centering:t.wave_centering?1:0, wave_centering_gain:t.wave_centering_gain,
      matched:(j.simstats&&j.simstats.stream_speed_mode)===1?1:0});
  }catch(e){ /* sim offline: the documented defaults above still give a panel */ }
  // The ARBITER GEOMETRY comes from the machine itself (/api/recordings carries
  // it) rather than from the constants above. Seeding it from constants was a
  // ground-truth defect: the panel showed user_speed 100 while the sim ran 50,
  // so every replay silently answered a question about a machine that does not
  // exist. Caught by the baseline diff line, which is exactly what it is for.
  try{
    const g = (await (await fetch("/api/recordings")).json()).geom;
    if (g) Object.assign(d, g);
  }catch(e){ /* fall through to the constants */ }
  for (const k of Object.keys(d)) if (d[k]!==undefined && !(k in tuneVals)) tuneVals[k]=d[k];
  applyVals();
}

function tuneQuery(){
  const q = [];
  for (const c of TUNE_FLAT){
    const v = tuneVals[c.k];
    if (v === undefined || v === "") continue;
    q.push(c.k + "=" + encodeURIComponent(v));
  }
  if (scopeView && N) q.push("t0="+view.t0.toFixed(4)+"&t1="+view.t1.toFixed(4));
  return q.join("&");
}

// THE PICKER WATCHES THE SHELF; it is not a snapshot taken when the panel opened.
// It used to rebuild only on `refresh`, on entering tune mode, or after a save
// made through this page - so a recording saved from ANYWHERE ELSE never
// appeared. That is not an edge case, it is the main workflow: `/rec.save` in
// the TUI, `slopsim replay --emit`, or a file dropped in the folder, then
// alt-tab to the analyzer already sitting open on the other monitor. It looked
// exactly like the save had failed.
//
// So: poll while tune mode is open, and refresh on window focus (the alt-tab).
// Cheap on both ends - the response is a small JSON, and the row counts behind
// it are memoized on the server by file mtime.
let shelfSig = "";
async function loadShelf(announce){
  let j = null;
  try{
    j = await (await fetch("/api/recordings")).json();
    SHELF = j.items || []; shelfDir = j.dir || "";
  }catch(e){ return; }   // sim offline: keep the list we have rather than blanking it

  const recs = SHELF.filter(a=>a.kind==="recording"), runs = SHELF.filter(a=>a.kind==="run");
  const sig = SHELF.map(a=>a.kind+":"+a.name+":"+a.rows).join("|");
  if (sig === shelfSig) return;              // nothing moved; leave the DOM alone
  // Never rebuild the list out from under an OPEN dropdown - the option the
  // operator is reaching for would shift as they click.
  const open = document.activeElement;
  if (shelfSig && (open === $("recsel") || open === $("basesel"))) return;

  const fresh = shelfSig
      ? recs.filter(a=>shelfSig.indexOf("recording:"+a.name+":") < 0).map(a=>a.name)
      : [];
  shelfSig = sig;

  const keepBase = $("basesel").value;
  $("recsel").innerHTML = recs.length
    ? recs.map(a=>'<option value="'+a.name+'">'+a.name+" ("+a.rows+" cmds)</option>").join("")
    : '<option value="">- nothing on the shelf yet -</option>';
  $("basesel").innerHTML = '<option value="">- no baseline -</option>'
    + runs.map(a=>'<option value="'+a.name+'">'+a.name+" ("+a.rows+" pts)</option>").join("");
  if (REC && recs.some(a=>a.name===REC)) $("recsel").value = REC;
  else REC = $("recsel").value || null;
  if (runs.some(a=>a.name===keepBase)) $("basesel").value = keepBase;
  $("shelfdir").textContent = shelfDir;
  $("shelfdir").classList.toggle("bad", j && j.writable === false);
  if (j && j.writable === false)
    toast("shelf is NOT WRITABLE: "+shelfDir+" - relaunch from a writable directory "
          + "or pass --recordings <dir>", true);

  if (!recs.length)
    msg("no recordings in " + (shelfDir||"the shelf") +
        " - stream into the sim, then save a clip (or /rec.save in the TUI)");
  else if (announce && fresh.length)
    toast(fresh.length === 1 ? 'new recording on the shelf: "'+fresh[0]+'"'
                             : fresh.length+" new recordings on the shelf");
}

// Watchers. Both are no-ops outside tune mode, where the picker is not visible
// and the live feed owns the store.
setInterval(()=>{ if (MODE === "tune" && !document.hidden) loadShelf(true); }, 2500);
addEventListener("focus", ()=>{ if (MODE === "tune") loadShelf(true); });

function msg(t, same){
  const e=$("tunemsg");
  if(!e) return;
  e.textContent = t;
  e.classList.toggle("same", !!same);
}
// Operator-action feedback, held long enough to actually read. Kept separate
// from msg() because the replay status line overwrites itself constantly, and a
// "saved 47 commands" that survives 60 ms is the same as no feedback at all.
let toastTimer = 0;
function toast(t, bad){
  const e = $("toast");
  e.textContent = t;
  e.classList.toggle("bad", !!bad);
  clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>{ if (e.textContent === t) e.textContent = ""; }, 7000);
}

// Replace the whole store from a trace.bin-shaped buffer. Used by replay and by
// run recall alike, because all three feeds share that one format.
function ingest(buf, keepView){
  const dv = new DataView(buf);
  const n = dv.getUint32(0,true);
  meta.rail=dv.getFloat32(4,true); meta.wmin=dv.getFloat32(8,true); meta.wmax=dv.getFloat32(12,true);
  const stride = buf.byteLength>=20 ? (dv.getUint32(16,true)||6) : 6;
  const f = new Float32Array(buf, 20, n*stride);
  N = 0; lastT = 0;
  for (let l=0;l<MAXLV;l++){ NB[l]=0; MN[l]=null; MX[l]=null; BCAP[l]=0; }
  ensureCap(n||1);
  for (let i=0;i<n;i++){
    const o=i*stride;
    T[i]=f[o]; P[i]=f[o+1]; G[i]=f[o+2]; V[i]=f[o+3];
    C[i]= stride>4 ? (f[o+4]<0 ? NaN : f[o+4]) : NaN;
    R[i]= stride>5 ? (f[o+5]<0 ? NaN : f[o+5]) : NaN;
    B[i]= NaN;
  }
  N = n;
  // Cheap change-detector for "did this edit actually alter the motion". Sums
  // position and velocity with an index weight so a pure reordering or a shift
  // in time cannot alias to the same value; exact equality is all that is
  // claimed, and only ever used to word a status line.
  { let a=0,b=0;
    for (let i=0;i<n;i++){ a += P[i]*(1+(i&1023)); b += V[i]; }
    lastSig = a.toFixed(4)+"|"+b.toFixed(4)+"|"+n; }
  if (BASE) fillBaseline();
  pyramidAppend(0);
  if (!keepView && N){ view.t0=T[0]; view.t1=T[N-1]; }
  lastT = N?T[N-1]:0;
  dirty = true;
}

let replayTimer = 0, replayBusy = false, replayAgain = false, replayEdited = false;
// Called only from the control listeners: an operator moved something.
function scheduleReplay(){
  replayEdited = true;
  clearTimeout(replayTimer);
  replayTimer = setTimeout(()=>runReplay(false), 60);
}

async function runReplay(refit){
  if (MODE !== "tune" || !REC) return;
  // One in flight at a time. A slider drag fires far faster than a round trip,
  // and letting them race means the LAST response painted is not necessarily
  // the LAST settings chosen - the graph would end up showing a config the
  // panel is no longer displaying, which is exactly the sort of quiet lie this
  // tool exists to catch everywhere else.
  if (replayBusy){ replayAgain = true; return; }
  replayBusy = true;
  const edited = replayEdited;
  replayEdited = false;
  document.body.classList.add("replaying");
  try{
    const r = await fetch("/api/replay.bin?rec="+encodeURIComponent(REC)+"&"+tuneQuery());
    if (!r.ok){ msg("replay failed: " + (await r.text())); return; }
    let st = {};
    try{ st = JSON.parse(r.headers.get("X-Replay-Stats")||"{}"); }catch(e){}
    const prev = lastSig;
    ingest(await r.arrayBuffer(), !refit);
    showMetrics(st);
    // "I moved a knob and the line did not move" is ambiguous between a working
    // inert knob and a broken tool, and silence resolves it the wrong way. So
    // say which it was. An unchanged result is a REAL FINDING, not a failure:
    // it means this control does nothing on THIS recording - most of the
    // waveform knobs only bite on segments the planner cannot meet, so a take
    // with no infeasible segment is genuinely immune to them.
    if (edited && prev !== null && lastSig === prev)
      msg("settings changed - output identical (this knob does nothing on this recording)", true);
    else
      msg("");
  }catch(e){ msg("replay error: " + e); }
  finally{
    replayBusy = false;
    document.body.classList.remove("replaying");
    if (replayAgain){ replayAgain = false; replayEdited = replayEdited || edited; scheduleReplay(); }
  }
}

// The bench DERIVES the engine's normalized ceilings the same way the device
// does (MotionCore.h deriveLimits) whenever these are left at 0. A non-zero
// value is an explicit OVERRIDE — "what if jmax were half that" is a real job —
// but an override also lets an operator plan a trajectory the follower is not
// permitted to execute. The carriage then runs permanently speed-saturated, lags
// its own setpoint, and BALLISTICALLY OVERSHOOTS the stroke window at every
// reversal: measured on GoogleCat, vmax 3.33 (= input_speed/span) left the window
// untouched while vmax 4.0 spent 9.2 s outside it and reached 407 mm on a
// 100-400 window. That state is unreachable on hardware, so it must never be
// mistaken for something the machine would do.
function ceilingWarnings(){
  const span = Math.max(1, (+tuneVals.win_max) - (+tuneVals.win_min));
  const out = [];
  const chk = (nk, mk, label, unit) => {
    const norm = +tuneVals[nk], mm = +tuneVals[mk];
    if (!isFinite(norm) || !isFinite(mm) || mm <= 0) return;
    if (!(norm > 0)) return;   // 0 = derived, and a derivation cannot exceed itself
    if (norm * span > mm * 1.001)
      out.push(label+" "+(norm*span).toFixed(0)+unit+" exceeds "+mm.toFixed(0)+unit
               +" (device would derive "+(mm/span).toFixed(2)+")");
  };
  chk("vmax", "input_speed", "planned vmax", " mm/s");
  chk("amax", "input_accel", "planned amax", " mm/s2");
  return out;
}

function showMetrics(m){
  const el = $("metrics");
  if (!el) return;
  if (!m || !m.samples){ el.textContent = ""; return; }
  const bk = m.anomalies_by_kind || {};
  const kinds = Object.keys(bk).filter(k=>bk[k]>0 && k!=="none")
      .map(k=>k+" "+bk[k]).join(", ") || "none";
  el.innerHTML =
    "<b>follow</b>  rms "+m.follow_rms_mm.toFixed(2)+"  max "+m.follow_max_mm.toFixed(2)+" mm\n"
  + "<b>sender</b>  rms "+m.sender_rms_mm.toFixed(2)+"  max "+m.sender_max_mm.toFixed(2)+" mm\n"
  + "<b>band dc</b> "+(m.band_center_err_mm>=0?"+":"")+m.band_center_err_mm.toFixed(2)+" mm\n"
  // SHAPE, separated from magnitude — rms alone scores a stroke the machine
  // FLATTENED the same as one it tracked slightly late. reach = how much of the
  // sender's travel came back; shape = did it take the same journey (1.00 =
  // yes, whatever the size); flat = share of moving time the plan held a
  // constant velocity, which IS a straight line in position.
  + "<b>reach</b> "+(m.reach_ratio!==undefined?m.reach_ratio.toFixed(3):"-")
  + "  <b>shape</b> "+(m.shape_corr!==undefined?m.shape_corr.toFixed(4):"-")
  + "  <b>flat</b> "+(m.flat_frac!==undefined?(100*m.flat_frac).toFixed(1)+"%":"-")+"\n"
  // PER-SEGMENT excursion. `travel` below is take-level and is blind to a short
  // move that flies past its own endpoint whenever a longer stroke reaches
  // further; ratio > 1 means it went further past the target than the whole move.
  + "<b>seg over</b> "+(m.seg_over_mean_mm!==undefined?m.seg_over_mean_mm.toFixed(2):"-")
  + " avg  "+(m.seg_over_max_mm!==undefined?m.seg_over_max_mm.toFixed(2):"-")
  + " max mm  x"+(m.seg_over_ratio_max!==undefined?m.seg_over_ratio_max.toFixed(2):"-")
  + " ("+(m.seg_scored!==undefined?m.seg_scored:"-")+" seg)\n"
  + "<b>travel</b>  "+m.pos_min_mm.toFixed(1)+"-"+m.pos_max_mm.toFixed(1)
  + " (cmd "+m.cmd_min_mm.toFixed(1)+"-"+m.cmd_max_mm.toFixed(1)+")\n"
  + "<b>peak</b>    "+m.peak_vel_mm_s.toFixed(0)+" mm/s  "+m.peak_acc_mm_s2.toFixed(0)+" mm/s2\n"
  + "<b>plans</b>   "+m.plans+" ok, "+m.plan_rejected+" rejected\n"
  + "<b>anom</b>    "+kinds+"\n"
  + '<span class="dim">'+m.samples+" samples in "+m.compute_ms.toFixed(1)+" ms</span>";
  // Built with String.fromCharCode(10) rather than a "\n" escape ON PURPOSE:
  // this whole page is a C++ raw string literal that gets rewritten by patch
  // scripts, and an escape sequence that survives one layer of quoting but not
  // the next produced a REAL newline inside a JS string literal here — a syntax
  // error that killed the entire script and left the page half-rendered with no
  // graph. No escape, nothing to mangle.
  const warn = ceilingWarnings();
  if (warn.length) {
    const NL = String.fromCharCode(10);
    el.innerHTML += NL + '<span class="warn">! ' + warn.join(NL + "! ")
                  + NL + "  the follower cannot execute this plan; overshoot past"
                  + NL + "  the window here is a bench artifact, not machine behavior</span>";
  }
  if (m.warn) msg(m.warn);
}

// ---- baseline --------------------------------------------------------------
// Indexed by MILLISECOND rather than searched: both feeds live on the same 1 ms
// grid, so the lookup is arithmetic and re-aligning a baseline onto a re-emitted
// window costs one O(N) pass at load, never anything per frame.
async function loadBaseline(name){
  if (!name){
    BASE = null;
    for (let i=0;i<N;i++) B[i]=NaN;
    rebuildPyramid(); msg("baseline cleared"); return;
  }
  try{
    const r = await fetch("/api/run.bin?name="+encodeURIComponent(name));
    if (!r.ok){ msg("baseline load failed"); return; }
    const buf = await r.arrayBuffer();
    const dv = new DataView(buf);
    const n = dv.getUint32(0,true);
    const stride = dv.getUint32(16,true)||6;
    if (!n){ msg("baseline is empty"); return; }
    const f = new Float32Array(buf, 20, n*stride);
    const t0ms = Math.round(f[0]*1000);
    const lastms = Math.round(f[(n-1)*stride]*1000);
    const pos = new Float32Array(Math.max(1, lastms-t0ms+1)).fill(NaN);
    for (let i=0;i<n;i++){
      const ms = Math.round(f[i*stride]*1000) - t0ms;
      if (ms>=0 && ms<pos.length) pos[ms] = f[i*stride+1];
    }
    BASE = {t0ms:t0ms, pos:pos};
    fillBaseline(); rebuildPyramid();
    let st = {};
    try{ st = JSON.parse(r.headers.get("X-Run-Settings")||"{}"); }catch(e){}
    msg('baseline "'+name+'" - '+describeDiff(st));
  }catch(e){ msg("baseline error: " + e); }
}

function fillBaseline(){
  if (!BASE) return;
  for (let i=0;i<N;i++){
    const ms = Math.round(T[i]*1000) - BASE.t0ms;
    B[i] = (ms>=0 && ms<BASE.pos.length) ? BASE.pos[ms] : NaN;
  }
}
function rebuildPyramid(){
  for (let l=0;l<MAXLV;l++) NB[l]=0;
  pyramidAppend(0);
  requestDraw();
}

// What differs between the baseline's stored settings and the panel's.
// Keys the run does not carry are reported as ABSENT, never filled in from
// today's values: a run saved before a knob existed must not read as though it
// was taken with that knob at its current setting.
function describeDiff(st){
  const diffs = [], absent = [];
  for (const c of TUNE_FLAT){
    if (c.reach === "bench") continue;
    const mine = String(tuneVals[c.k] === undefined ? "" : tuneVals[c.k]);
    if (!(c.k in st)){ absent.push(c.k); continue; }
    const theirs = String(st[c.k]);
    if (mine === "") continue;
    const bothNum = !isNaN(+theirs) && !isNaN(+mine);
    if (bothNum ? Math.abs(+theirs - +mine) > 1e-9 : theirs !== mine)
      diffs.push(c.k+" "+theirs+">"+mine);
  }
  const extra = Object.keys(st).filter(k=>k.indexOf("metric.")!==0 && k!=="recording"
      && !TUNE_FLAT.some(c=>c.k===k));
  let out = diffs.length ? diffs.join(", ") : "settings identical";
  if (absent.length) out += " | not recorded in that run: " + absent.join(", ");
  if (extra.length) out += " | unknown to this build: " + extra.join(", ");
  return out;
}

// ---- saves -----------------------------------------------------------------
// WHAT GETS CLIPPED FOLLOWS WHAT IS ON SCREEN. In live mode the graph is the
// machine's own wire log, so `live` is the source; in tune mode it is the
// recording being replayed, so that recording is. Deciding this from MODE rather
// than from "is a recording selected" is the fix for a real trap: once any
// recording had been picked, the old rule could never clip the live trace again,
// which is exactly the stream-a-scene-then-keep-the-good-part move.
async function saveClip(){
  const src = MODE === "tune" && REC ? REC : "live";
  if (!N){ toast("nothing captured to clip", true); return; }
  const name = prompt("save commands "+view.t0.toFixed(2)+"-"+view.t1.toFixed(2)+
                      's from "'+src+'" as:');
  if (!name) return;
  const u = "/api/rec/save?name="+encodeURIComponent(name)+"&src="+encodeURIComponent(src)
          + "&t0="+view.t0.toFixed(4)+"&t1="+view.t1.toFixed(4);
  try{
    const j = await (await fetch(u)).json();
    if (j.error || !j.saved){
      toast("save failed: "+(j.error||"nothing written"), true);
      return;                       // do NOT select a recording that is not there
    }
    toast("saved "+j.saved+' commands as "'+j.name+'"');
    // Select and LOAD what was just saved. Leaving it merely present in the
    // dropdown is how the old build managed to save correctly and still look
    // like it had done nothing at all.
    if (MODE !== "tune"){
      MODE = "live";
      $("tune").click();                 // enter tune mode on the new clip
      await new Promise(r=>setTimeout(r,50));
    }
    REC = j.name;
    await loadShelf();
    $("recsel").value = j.name;
    await runReplay(true);
    toast('now tuning "'+j.name+'"');
  }catch(e){ toast("save error: "+e, true); }
}

async function saveRun(){
  if (MODE !== "tune" || !REC){ toast("pick a recording first", true); return; }
  const name = prompt("freeze this result (samples + settings) as:");
  if (!name) return;
  try{
    const j = await (await fetch("/api/run/save?name="+encodeURIComponent(name)
                    + "&rec="+encodeURIComponent(REC)+"&"+tuneQuery())).json();
    if (j.error || !j.saved){ toast("save failed: "+(j.error||"nothing written"), true); return; }
    toast("froze "+j.saved+' samples as run "'+j.name+'" - pick it as a baseline to compare against');
    await loadShelf();
  }catch(e){ toast("save error: "+e, true); }
}

// ---- the toggle ------------------------------------------------------------
$("saveclip").onclick = saveClip;
$("tune").onclick = async ()=>{
  MODE = MODE === "tune" ? "live" : "tune";
  document.body.classList.toggle("tuning", MODE === "tune");
  $("tune").classList.toggle("on", MODE === "tune");
  if (MODE === "tune"){
    if (!$("tunepanel").innerHTML){ buildPanel(); await seedFromSim(); }
    await loadShelf();
    live = false; $("live").classList.remove("on");
    await runReplay(true);
  } else {
    // Back to following the machine: drop the recomputed samples and let the
    // poll refill from the sim's own ring, which still holds its last 240 s.
    resetCapture();
  }
  invalidateSizes(); requestDraw();
};

poll();
requestAnimationFrame(frame);
</script></body></html>
)HTML";

}  // namespace slopsim
