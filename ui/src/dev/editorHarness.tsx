/**
 * Dev-only harness: renders one block's editor band on its own, against the
 * mock bridge, at a chosen width — for working on an editor without the rest
 * of the app in the way. Not an entry of `vite build`.
 *
 *   /editor-harness.html?block=delay&w=1180
 *
 * `block` is any chain token (delay, comp2, amp, cab, fx1, split, mix…); `w`
 * is the band's width (the app gives it 1180 at the default window, about 780
 * with the TONE3000 drawer open, 600 at the minimum window with the drawer).
 */
import { StrictMode, useEffect } from "react";
import { createRoot } from "react-dom/client";
import "../theme/global.css";
import type { BlockId } from "../bridge";
import { SPLIT_TRIPLE } from "../chain/structure";
import { Toaster } from "../components";
import { Panel } from "../features/panel";
import { connectStore, useStore } from "../store";

connectStore();

const query = new URLSearchParams(window.location.search);
const block = (query.get("block") ?? "reverb") as BlockId;
const width = Number(query.get("w") ?? 1180);

function Harness() {
  const ready = useStore((st) => st.ready);

  useEffect(() => {
    void useStore
      .getState()
      .hydrate()
      .then(() => {
        const st = useStore.getState();
        if (!st.chainOrder.includes(block)) {
          if ((SPLIT_TRIPLE as readonly string[]).includes(block)) {
            st.setChainOrder([...st.chainOrder, ...SPLIT_TRIPLE], []);
          } else {
            st.addBlock(block);
          }
        }
        useStore.getState().selectBlock(block);
      });
  }, []);

  return (
    <div style={{ padding: 24 }}>
      <div style={{ position: "relative", width, height: 296 }}>{ready && <Panel />}</div>
      <Toaster />
    </div>
  );
}

const root = document.getElementById("root");
if (!root) throw new Error("#root missing");
createRoot(root).render(
  <StrictMode>
    <Harness />
  </StrictMode>,
);
