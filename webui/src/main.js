/**
 * main.js — the entry point, and the whole Tauri seam.
 *
 * Everything device-specific about "how do I reach the machine and prove who I
 * am" is decided in this file and nowhere else. The device-hosted bundle infers
 * the host from location and mints a token over same-origin HTTP; a Tauri shell
 * overrides those two things and gets an identical UI. That is the "~15 lines
 * apart" promise, and it only holds because everything above this file is
 * generic — the page renders whatever catalog the hub sends, so it has no other
 * reason to care which machine it is talking to.
 */

import { mount } from 'svelte';
import App from './App.svelte';
import './style.css';
import { connect } from './model/machine.svelte.js';
import { applyTheme, currentThemeId } from './model/theme.js';

// Client preferences, applied before first paint so the page never flashes the
// default palette. These are BROWSER state, not machine state — the
// ground-truth doctrine does not apply and nothing here is sent to the device.
applyTheme(currentThemeId());
try {
  if (localStorage.getItem('ui_hivis') === '1') document.documentElement.classList.add('hivis');
  if (localStorage.getItem('ui_terse') === '1') document.documentElement.classList.add('terse');
} catch (e) { /* private mode: preferences are an optimization, never a requirement */ }

// The device serves this bundle, so it IS the machine — except inside the
// Tauri shell, where the host is the operator's choice and /uitoken minting
// runs through the shell's Rust-side fetch (no browser same-origin rules).
// TAURI_ENV_PLATFORM is set only by the Tauri CLI's build, so the embedded
// bundle compiles this branch away entirely.
const SHELL = !!import.meta.env.TAURI_ENV_PLATFORM;

async function boot() {
  // ?hub=<ip> points THIS page at a hub other than its own origin — the dev
  // loop (vite dev against the live machine; /uitoken stays same-origin-only
  // so such a session lands at watch tier, which is honest). Harmless on the
  // embedded page: absent parameter, origin rules as always.
  const hubOverride = new URLSearchParams(location.search).get('hub');
  let host = hubOverride || location.hostname || '192.168.1.229';
  if (SHELL) {
    const { fetch: tauriFetch } = await import('@tauri-apps/plugin-http');
    const { setHttpGet } = await import('../../../SlopSync/clients/js/index.js');
    setHttpGet(async (url) => {
      const r = await tauriFetch(url, { method: 'GET' });
      return r.ok ? await r.text() : null;
    });
    // Shell chrome: discovery + transport control live OUTSIDE the kernel UI.
    const { default: ShellBar } = await import('./shell/ShellBar.svelte');
    const bar = document.createElement('div');
    document.body.appendChild(bar);
    mount(ShellBar, { target: bar });

    // NO baked-in host: discovery IS the shell's front door (operator ruling,
    // 2026-07-28). Auto-connect only re-joins a hub the operator explicitly
    // chose before (saved by ShellBar on a successful WS connect).
    const saved = localStorage.getItem('shell_host');
    if (saved && (localStorage.getItem('shell_mode') || 'ws') === 'ws') {
      connect({ host: saved });
    }
    return;
  }
  connect({ host });
}
boot();

export default mount(App, { target: document.getElementById('app') });
