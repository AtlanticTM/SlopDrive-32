import { defineConfig } from 'vite';  
import { viteSingleFile } from 'vite-plugin-singlefile';  
  
/**  
 * Vite builds the single-file HTML bundle (JS/CSS inlined).  
 * Gzipping is handled by the build_webui.py PlatformIO pre-build script  
 * - keeping the Vite config clean and dependency-light. :3  
 */  
export default defineConfig({  
  plugins: [viteSingleFile()],  
  build: {  
    cssCodeSplit: false,  
    minify: 'esbuild',  
  },  
  envPrefix: 'VITE_',  
}); 
