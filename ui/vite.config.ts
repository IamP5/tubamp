import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";

/**
 * Vite stamps `crossorigin` on the emitted module script and stylesheet, which
 * puts those sub-resource requests in CORS mode. In a ship build the page's
 * origin is the custom scheme `juce://juce.backend` and JUCE's resource
 * provider only emits `Access-Control-Allow-Origin` when an `allowedOrigin` is
 * configured — which ship builds deliberately do not do
 * (docs/research/juce-webview.md §5.4). The requests are same-origin so they
 * *should* skip the CORS check, but a blank WebView is an unrecoverable failure
 * mode in a plugin and the attribute buys nothing here: drop it.
 */
function stripCrossorigin(): Plugin {
  return {
    name: "tubamp-strip-crossorigin",
    enforce: "post",
    apply: "build",
    transformIndexHtml(html) {
      return html.replace(/\s+crossorigin(?==|\s|>)/g, "");
    },
  };
}

// The bundle is served from `juce://juce.backend/` by WebEditor's resource
// provider (see docs/research/juce-webview.md §2.7), so every asset URL must be
// relative. `target: safari14` matches the WKWebView shipped on the plugin's
// minimum macOS deployment target.
export default defineConfig({
  plugins: [react(), stripCrossorigin()],
  base: "./",
  server: { port: 5173, strictPort: true },
  build: {
    outDir: "dist",
    assetsDir: "assets",
    target: "safari14",
    sourcemap: false,
    emptyOutDir: true,
    // Fonts must be real files (not data URIs) so the resource provider can
    // serve them with a font MIME type; keep everything else inlined-by-default.
    assetsInlineLimit: 4096,
  },
});
