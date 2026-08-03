import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";
import reactHooks from "eslint-plugin-react-hooks";

export default tseslint.config(
  {
    // Vendored JUCE frontend lib + build output are not ours to lint. `.vite` is
    // the dev server's dependency-optimizer cache, which appears after the first
    // `npm run dev` and is pre-bundled third-party code.
    ignores: [
      "dist",
      ".vite",
      "src/juce/index.js",
      "src/juce/check_native_interop.js",
    ],
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["**/*.{ts,tsx}"],
    languageOptions: {
      ecmaVersion: 2022,
      globals: globals.browser,
    },
    plugins: { "react-hooks": reactHooks },
    rules: {
      ...reactHooks.configs.recommended.rules,
      "@typescript-eslint/no-unused-vars": [
        "error",
        { argsIgnorePattern: "^_", varsIgnorePattern: "^_" },
      ],
      "no-restricted-syntax": [
        "error",
        {
          selector: "CallExpression[callee.name='setInterval']",
          message:
            "Use the shared rAF driver (src/hooks/useMeters.ts) instead of setInterval — timers drift under WKWebView throttling.",
        },
      ],
    },
  },
);
