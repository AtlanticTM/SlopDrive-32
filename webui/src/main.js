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
} catch (e) { /* private mode: preferences are an optimization, never a requirement */ }

// The device serves this bundle, so it IS the machine. A Tauri build replaces
// this line with the operator's chosen host and calls setHttpGet() with a
// native fetch that is not bound by browser same-origin rules.
const host = location.hostname || '192.168.1.229';

connect({ host });

export default mount(App, { target: document.getElementById('app') });
