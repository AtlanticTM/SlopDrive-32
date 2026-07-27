// THROWAWAY vite config for build-verifying TelemetryChart/PlanStrip without
// touching the real vite.config.js. Deleted before task completion.
import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  build: {
    outDir: 'dist-widget-harness',
    rollupOptions: { input: 'widget-harness.html' },
  },
});
