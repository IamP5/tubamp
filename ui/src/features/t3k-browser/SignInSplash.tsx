/**
 * The partnership splash the TONE3000 terms require before first sign-in
 * (docs/research/tone3000-api.md §Design Requirements).
 *
 * Sign-in is a system-browser round-trip: `t3kSignIn` resolves immediately and
 * the outcome arrives as `t3kStatus` (the drawer swaps to the catalog) or
 * `t3kError` (the toast fires and the button comes back).
 */
import { useEffect, useState } from "react";
import { Button, CloseIcon, IconButton, Spinner } from "../../components";
import { bridge } from "../../bridge";
import { useStore } from "../../store";
import s from "./t3k.module.css";

export function SignInSplash({ onClose }: { onClose(): void }) {
  const configured = useStore((st) => st.t3k.configured);
  const t3kConfigure = useStore((st) => st.t3kConfigure);
  const [waiting, setWaiting] = useState(false);

  useEffect(() => {
    const off = bridge.on("t3kError", () => setWaiting(false));
    return off;
  }, []);

  return (
    <>
      <header className={s.head}>
        <span className={s.title}>TONE3000</span>
        <span className={s.headSpacer} />
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon />
        </IconButton>
      </header>

      <div className={s.splash}>
        <h2 className={s.splashTitle}>Captures and IRs from TONE3000</h2>
        <p className={s.splashCopy}>
          {configured
            ? "Sign in to browse the community’s amp captures and cab IRs and load them straight into the chain. Sign-in opens in your browser."
            : "Add a TONE3000 publishable key first. Create one at tone3000.com under Settings, API Keys."}
        </p>
        {configured ? (
          <Button
            variant="primary"
            icon={waiting ? <Spinner size={14} /> : undefined}
            disabled={waiting}
            onClick={() => {
              setWaiting(true);
              void bridge.t3kSignIn();
            }}
          >
            {waiting ? "Waiting for browser…" : "Sign in to TONE3000"}
          </Button>
        ) : (
          <Button variant="primary" onClick={() => void t3kConfigure()}>
            Set publishable key…
          </Button>
        )}
      </div>
    </>
  );
}
