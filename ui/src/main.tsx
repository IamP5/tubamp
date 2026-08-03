import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./theme/global.css";
import { App } from "./App";
import { connectStore } from "./store";

// Subscribe the store to bridge events before the first render, so an event
// that fires during hydration is never dropped.
connectStore();

const root = document.getElementById("root");
if (!root) throw new Error("#root missing from index.html");

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
