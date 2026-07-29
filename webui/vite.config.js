import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { execSync } from 'node:child_process';

/**
 * Vite builds the single-file HTML bundle (JS/CSS/fonts inlined).
 *
 * The SlopDrive-32 LittleFS pipeline (build_webui.py) copies ONLY
 * webui/dist/index.html into data/ — separately-emitted asset files never
 * reach the device. So the self-hosted WOFF2 fonts MUST be inlined as base64
 * data URIs. assetsInlineLimit is raised above the largest subsetted font
 * (~21KB for MartianMono.woff2) so Vite emits them inline rather than as
 * separate files. 100KB gives comfortable headroom.
 *
 * Gzipping is handled by build_webui.py (PlatformIO pre-build script).
 *
 * SVELTE 5: the plugin is pinned to the v4 line because it is the last one that
 * peers against Vite 5, and both vite-plugin-singlefile and build_webui.py are
 * tuned to Vite 5's asset emission. Svelte compiles to direct DOM operations,
 * so the framework's runtime cost on a page served off LittleFS stays small.
 */

// UI bundle build identifier — the footer "ui" chip (§1.6h). Short git hash
// when available (matches how the firmware's own fw_version tracks commits);
// falls back to a UTC build timestamp in a repo-less checkout so the chip is
// never a hand-maintained literal.
function uiBuildId() {
  try {
    return execSync('git rev-parse --short HEAD', { cwd: __dirname }).toString().trim();
  } catch (e) {
    return 'b' + new Date().toISOString().slice(0, 16).replace(/[-:T]/g, '');
  }
}

export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  define: {
    __UI_BUILD__: JSON.stringify(uiBuildId()),
  },
  build: {
    cssCodeSplit: false,
    minify: 'esbuild',
    assetsInlineLimit: 100 * 1024, // 100KB — fonts (~21KB max) inline as data URIs
  },
  server: {
    // src-tauri/target is cargo's build dir — watching it EBUSY-crashes vite
    // on Windows when cargo holds a build-script exe open.
    watch: { ignored: ['**/src-tauri/**'] },
  },
  // TAURI_ENV_* exists only when the Tauri CLI drives the build — main.js's
  // shell branch keys on it, so the embedded bundle dead-code-eliminates the
  // entire shell path (delivery accord: divergence is build config, not code).
  envPrefix: ['VITE_', 'TAURI_ENV_'],
});