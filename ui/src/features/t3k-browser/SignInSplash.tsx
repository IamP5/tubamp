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
import { T3kMark } from "./parts";
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
        <T3kMark compact />
        <span className={s.headSpacer} />
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon size={14} />
        </IconButton>
      </header>

      <div className={s.splash}>
        <h2 className={s.splashTitle}>Thousands of captures, in the chain</h2>
        <p className={s.splashCopy}>
          TONE3000 hosts the community’s NAM amp captures and cab impulse
          responses. Sign in with your TONE3000 account to browse them here and
          load one straight into this plugin — the files land in your own
          library.
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
            {waiting ? "Waiting for browser…" : "Sign in with TONE3000"}
          </Button>
        ) : (
          <Button variant="primary" onClick={() => void t3kConfigure()}>
            Set publishable key…
          </Button>
        )}

        <p className={s.splashNote}>
          {configured
            ? "Sign-in opens in your browser; come back when it is done."
            : "Create a publishable key at tone3000.com → Settings → API Keys."}
        </p>
      </div>
    </>
  );
}
