#pragma once

// GraphPage — the slopsim analyzer page, embedded so the exe stays standalone.
// Constraints:
//   Served at GET /graph by HttpFacade; data comes from GET /api/trace.bin
//   (20-byte header {u32 n, f32 max_rail, f32 win_min, f32 win_max, u32
//   stride} + n x stride f32 LE, stride = 6: {t, pos, tgt, vel, cmd_norm,
//   raw_norm}, query ?since=<t_s> for incremental polls). A dev-tool page,
//   not the device WebUI — the firmware's compile-time asset pipeline rules
//   don't apply.
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
  :root { --bg:#1a1915; --panel:#211f1a; --chrome:#6b675c; --fg:#e8e6e0;
          --accent:#e8956b; --pos:#7ec87e; --tgt:#e0c96b; --vel:#a98fd6;
          --cmd:#c9a6ff; --raw:#5fb8c9; }
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
</style></head><body>
<header>
  <span class="logo">&#10035; slopsim</span><span>analyzer</span>
  <button id="live" class="on">live</button>
  <button id="fit">fit</button>
  <button id="cmd" class="on" title="commanded target (wire, normalized) mapped through the stroke window">cmd</button>
  <button id="raw" class="on" title="the SENDER&apos;S OWN CURVE through the knots - what the client asked for, before the planner">raw</button>
  <button id="clr" title="discard all captured samples and start over (also the cure for a session that has grown sluggish)">clear</button>
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
  <span id="stats"></span>
</header>
<div id="plots">
  <div id="poswrap"><canvas id="pos"></canvas></div>
  <div id="velwrap"><canvas id="vel"></canvas></div>
</div>
<div id="timelinewrap"><canvas id="timeline"></canvas></div>
<div id="readout"></div>
<footer><span class="k">wheel</span> zoom &nbsp; <span class="k">drag</span> pan &nbsp;
<span class="k">double-click</span> fit &nbsp; <span class="k">hover</span> inspect
&nbsp;&nbsp;<span class="dim">pos <span style="color:var(--pos)">&#9644;</span>
tgt <span style="color:var(--tgt)">&#9644;</span>
vel <span style="color:var(--vel)">&#9644;</span>
cmd <span style="color:var(--cmd)">&#9644;</span>
raw <span style="color:var(--raw)">&#9644;</span>
<span class="dim">(raw = the sender&apos;s own curve. raw&ne;tgt is what the PLANNER could not deliver; tgt&ne;pos is what the MACHINE could not track)</span></span></footer>
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
let COL = [P, G, V, C, R];       // pyramid series order; 3 and 4 are normalized
const SER_P = 0, SER_G = 1, SER_V = 2, SER_C = 3, SER_R = 4;
// Pyramided series count. Every bucket loop below reads THIS — adding a column
// means bumping one constant, not finding four hardcoded 4s.
const NSER = 5;

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
        v=new Float32Array(c), m=new Float32Array(c), r=new Float32Array(c);
  t.set(T.subarray(0,N)); p.set(P.subarray(0,N)); g.set(G.subarray(0,N));
  v.set(V.subarray(0,N)); m.set(C.subarray(0,N)); r.set(R.subarray(0,N));
  T=t; P=p; G=g; V=v; C=m; R=r; COL=[P,G,V,C,R]; CAP=c;
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
// rather than every poll, so the amortised cost is constant and the session
// stops degrading. ~33 min of 1 kHz scrollback is kept, well past the sim's own
// 240 s ring, so nothing the sim can still serve is ever discarded early.
const MAXN = 2000000;
function compact(){
  const keep = MAXN >> 1, from = N - keep;
  T.copyWithin(0,from,N); P.copyWithin(0,from,N); G.copyWithin(0,from,N);
  V.copyWithin(0,from,N); C.copyWithin(0,from,N); R.copyWithin(0,from,N);
  N = keep;
  for (let l=0;l<MAXLV;l++) NB[l]=0;   // bucket boundaries all moved
  pyramidAppend(0);
}

// Drop everything. The next poll asks with since=-1 (because N is 0), so the
// sim replays whatever is still in its own ring and the view refills itself.
function resetCapture(){
  N = 0; lastT = 0;
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
for (const k of ["pos","tgt","vel","cmd","raw","accent","bg"]) CLR[k]=_cs.getPropertyValue("--"+k).trim();

// ---- polling ---------------------------------------------------------------
async function poll(){
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
  if (live && N){
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
    // COMMANDED, normalized -> window. Drawn FIRST and FAT, so tgt lands inside
    // it: perfect agreement reads as a purple halo hugging the yellow line, and
    // any divergence separates into two visibly distinct bands. That is also
    // why it is not dashed — a dashed stroke over ~1500 min/max columns is
    // MEASURABLY the most expensive thing on the canvas under software
    // rasterisation (176 ms/frame vs 17 ms with it solid, headless, 3.6 M
    // samples). A halo says the same thing for free.
    if (showCmd)
      drawSeries(ctx,w,h,SER_C,0,meta.rail,CLR.cmd,3.2*d,null,
                 (meta.wmax-meta.wmin), meta.wmin);
    // Raw is stroked BEFORE tgt so the planner's line sits on top of it: where
    // the planner delivered what was asked, raw disappears underneath, and what
    // stays visible is exactly the part it could not deliver.
    if (showRaw)
      drawSeries(ctx,w,h,SER_R,0,meta.rail,CLR.raw,1.6*d,null,
                 (meta.wmax-meta.wmin), meta.wmin);
    drawSeries(ctx,w,h,SER_G,0,meta.rail,CLR.tgt,1*d);
    drawSeries(ctx,w,h,SER_P,0,meta.rail,CLR.pos,1.6*d);
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

poll();
requestAnimationFrame(frame);
</script></body></html>
)HTML";

}  // namespace slopsim
