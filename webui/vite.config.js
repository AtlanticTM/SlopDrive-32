import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

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
 */
export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    cssCodeSplit: false,
    minify: 'esbuild',
    assetsInlineLimit: 100 * 1024, // 100KB — fonts (~21KB max) inline as data URIs
  },
  envPrefix: 'VITE_',
});