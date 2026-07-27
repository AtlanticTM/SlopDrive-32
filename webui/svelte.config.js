import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

export default {
  preprocess: vitePreprocess(),
  compilerOptions: {
    // Svelte 5 runes mode everywhere. The model layer uses $state in .svelte.js
    // modules, and mixing rune and legacy reactivity in one project is a source
    // of very confusing bugs — so it is on globally, not per-file.
    runes: true,
  },
};
