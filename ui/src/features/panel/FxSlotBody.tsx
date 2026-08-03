/**
 * FX slot body — the editor for one external AudioUnit slot.
 *
 * Same shape as CabBody (an action row over a body), but everything it shows is
 * driven by `FxSlotState` rather than by parameters: a slot can be empty,
 * loading, live, missing (assigned to a plugin this machine does not have) or
 * simply broken, and each of those has to be legible on its own.
 *
 * Two constraints shape the interaction:
 *  - The picker is mouse-only, with no search field. Logic gives the WebView no
 *    keyboard, so a typed filter would be dead UI in the host that matters most.
 *  - The plugin's own parameters scroll inside this body. The dock is a fixed
 *    232px and a hosted plugin can expose a hundred parameters; the dock does not
 *    grow for them.
 *
 * This body is also where the plugin's own editor is mounted into our window
 * ("Show editor here") and, crucially, where it is unmounted again: the embed is
 * a native view that would otherwise outlive the panel that asked for it.
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import {
  AlertIcon,
  Button,
  EmptyState,
  Menu,
  Spinner,
  Tooltip,
  useMenu,
  type ButtonVariant,
  type MenuEntry,
} from "../../components";
import {
  bridge,
  type FxPluginEntry,
  type FxSlotIndex,
  type FxSlotState,
} from "../../bridge";
import { useStore } from "../../store";
import { FxParamGrid } from "./FxParamGrid";
import s from "./panel.module.css";

/* ─────────────────────────── metering the visible slot ─────────────────── */

/**
 * The plugin only pushes `fxParamValues` for one slot, and the dock keeps an
 * outgoing body mounted while it fades out — so a late cleanup must not clear a
 * watch that its successor has already claimed. The module-level owner makes the
 * unmount order irrelevant.
 */
let watchedSlot: FxSlotIndex | -1 = -1;

function useWatchedSlot(slot: FxSlotIndex): void {
  useEffect(() => {
    watchedSlot = slot;
    void bridge.fxWatchSlot(slot);
    return () => {
      if (watchedSlot !== slot) return;
      watchedSlot = -1;
      void bridge.fxWatchSlot(-1);
    };
  }, [slot]);
}

/* ────────────────────── the plugin's editor, in our window ─────────────────── */

/**
 * Tears the embed down when the body that asked for it goes away — selection
 * moved to another block, the chain lost this one, or the dock closed.
 *
 * Guarded by ownership (`fxEmbedRequestedBy`) rather than by "is anything
 * embedded", because the dock keeps an outgoing body mounted while it fades: the
 * order bodies unmount in is not the order the user acted in, and a late cleanup
 * must not tear down an embed its successor has already asked for.
 */
function useEmbedTeardown(slot: FxSlotIndex): void {
  useEffect(() => {
    return () => {
      const store = useStore.getState();
      if (store.fxEmbedRequestedBy !== slot) return;
      void store.unembedFxEditor();
    };
  }, [slot]);
}

/* ──────────────────────────────── the picker ───────────────────────────── */

/** The installed-AU list is asked for once per editor: the scan is only a
 *  component-registry walk, but its answer cannot change while we are open. */
let catalogRequest: Promise<{ plugins: FxPluginEntry[] }> | null = null;

/** null while the scan is in flight. */
function useFxCatalog(): FxPluginEntry[] | null {
  const [plugins, setPlugins] = useState<FxPluginEntry[] | null>(null);

  useEffect(() => {
    let alive = true;
    /* Drop a rejected request so the next panel retries. Caching the rejection
       would leave every picker for the rest of the session stuck on "scanning". */
    catalogRequest ??= bridge.fxListPlugins().catch((error: unknown) => {
      catalogRequest = null;
      throw error;
    });
    void catalogRequest.then(
      (result) => {
        if (alive) setPlugins(result.plugins);
      },
      () => {
        if (alive) setPlugins([]);
      },
    );
    return () => {
      alive = false;
    };
  }, []);

  return plugins;
}

/** Grouped by manufacturer — the only grouping a plugin list reliably carries,
 *  and the one users navigate by when they cannot type. */
function catalogEntries(
  plugins: FxPluginEntry[] | null,
  onPick: (identifier: string) => void,
): MenuEntry[] {
  if (plugins === null)
    return [{ id: "scanning", label: "Scanning installed plugins…", disabled: true }];
  if (plugins.length === 0)
    return [{ id: "empty", label: "No AudioUnit effects installed", disabled: true }];

  const byMaker = new Map<string, FxPluginEntry[]>();
  for (const plugin of plugins) {
    const maker = plugin.manufacturer || "Unknown";
    const list = byMaker.get(maker);
    if (list) list.push(plugin);
    else byMaker.set(maker, [plugin]);
  }

  const entries: MenuEntry[] = [];
  for (const maker of [...byMaker.keys()].sort((a, b) => a.localeCompare(b))) {
    entries.push({ kind: "section", id: `maker:${maker}`, label: maker });
    const list = byMaker.get(maker) ?? [];
    for (const plugin of [...list].sort((a, b) => a.name.localeCompare(b.name))) {
      entries.push({
        id: plugin.identifier,
        label: plugin.name,
        hint: plugin.version,
        onSelect: () => onPick(plugin.identifier),
      });
    }
  }
  return entries;
}

interface FxPickerProps {
  label: string;
  variant?: ButtonVariant;
  onPick(identifier: string): void;
}

function FxPicker({ label, variant, onPick }: FxPickerProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();
  const plugins = useFxCatalog();
  const entries = useMemo(() => catalogEntries(plugins, onPick), [plugins, onPick]);

  return (
    <>
      <span ref={anchorRef} className={s.fxAnchor}>
        <Button
          size="sm"
          variant={variant}
          onClick={toggle}
          aria-haspopup="menu"
          aria-expanded={open}
        >
          {label}
        </Button>
      </span>
      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        minWidth={280}
      />
    </>
  );
}

/* ────────────────────────────────── body ──────────────────────────────── */

export interface FxSlotBodyProps {
  slot: FxSlotIndex;
}

export function FxSlotBody({ slot }: FxSlotBodyProps) {
  const state = useStore((st) => st.fxSlots[slot]);
  const supported = useStore((st) => st.fxSupported);
  const loadFxPlugin = useStore((st) => st.loadFxPlugin);
  const clearFxPlugin = useStore((st) => st.clearFxPlugin);
  const openFxEditor = useStore((st) => st.openFxEditor);
  const embedFxEditor = useStore((st) => st.embedFxEditor);
  const unembedFxEditor = useStore((st) => st.unembedFxEditor);
  const embedded = useStore((st) => st.fxEmbed.slot === slot);

  useWatchedSlot(slot);
  useEmbedTeardown(slot);

  const pick = useCallback(
    (identifier: string) => void loadFxPlugin(slot, identifier),
    [loadFxPlugin, slot],
  );
  const remove = useCallback(() => void clearFxPlugin(slot), [clearFxPlugin, slot]);

  const toggleEmbed = useCallback(() => {
    if (embedded) void unembedFxEditor();
    else void embedFxEditor(slot);
  }, [embedFxEditor, embedded, slot, unembedFxEditor]);

  if (!supported) {
    return (
      <div className={s.fxCentre}>
        <EmptyState
          message="Plugin hosting is not available"
          hint="This build of tubamp was compiled without AudioUnit hosting, so the FX slots stay pass-throughs."
        />
      </div>
    );
  }

  if (!state.occupied) {
    return (
      <div className={s.fxCentre}>
        <EmptyState
          message="No plugin in this slot"
          hint="The slot passes audio through untouched until you load one."
          action={
            <FxPicker label="Choose a plugin" variant="primary" onPick={pick} />
          }
        />
      </div>
    );
  }

  if (state.loading) {
    return (
      <div className={s.fxCentre}>
        <div className={s.fxLoading}>
          <Spinner size={16} />
          <span className={s.hint}>Loading {state.name}…</span>
        </div>
      </div>
    );
  }

  if (!state.live) return <FxUnavailable state={state} onPick={pick} onRemove={remove} />;

  return (
    <div className={s.fxBody}>
      <div className={s.fxHead}>
        <div className={s.fxIdentity}>
          <span className={s.fxName} title={state.name}>
            {state.name}
          </span>
          <span className={s.fxMaker}>
            {state.manufacturer}
            {state.latencySamples > 0 &&
              ` · ${state.latencySamples} smp latency`}
          </span>
        </div>

        <div className={s.actionRow}>
          <Tooltip
            label={
              embedded
                ? "Give the board back and put this editor away."
                : "Draw this plugin's editor on the board. It is a native view over the page, so menus and dialogs will hide it while they are open."
            }
          >
            <Button size="sm" variant="primary" onClick={toggleEmbed}>
              {embedded ? "Hide editor" : "Show editor here"}
            </Button>
          </Tooltip>
          <Tooltip
            label={
              state.hasEditor
                ? "Open the plugin's own window — the only place its text fields get a keyboard inside a host."
                : "Open a generic editor window for this plugin."
            }
          >
            <Button size="sm" onClick={() => void openFxEditor(slot)}>
              Open plugin window
            </Button>
          </Tooltip>
          <FxPicker label="Replace" onPick={pick} />
          <Button size="sm" variant="ghost" onClick={remove}>
            Remove
          </Button>
        </div>
      </div>

      {state.params.length > 0 ? (
        <FxParamGrid slot={slot} params={state.params} />
      ) : (
        <div className={s.fxCentre}>
          <span className={s.hint}>
            This plugin exposes no parameters — use its own window.
          </span>
        </div>
      )}
    </div>
  );
}

/* ───────────────────── assigned but not running ────────────────────────── */

interface FxUnavailableProps {
  state: FxSlotState;
  onPick(identifier: string): void;
  onRemove(): void;
}

/**
 * The slot holds a plugin that is not running: either this machine does not have
 * it (`missing`) or it refused to start. Both keep the assignment, and the
 * missing case also keeps the plugin's saved settings — which is the one thing
 * the user needs to be told before they reach for Remove.
 */
function FxUnavailable({ state, onPick, onRemove }: FxUnavailableProps) {
  return (
    <div className={s.fxBody}>
      <div className={s.fxNotice} role="status">
        <AlertIcon size={14} className={s.fxNoticeIcon} />
        <div className={s.fxNoticeCopy}>
          <span className={s.fxNoticeTitle}>
            {state.name || "This plugin"}
            {state.missing ? " is not installed" : " is not running"}
            {state.manufacturer && ` — ${state.manufacturer}`}
          </span>
          <span className={s.fxNoticeText}>
            {state.missing
              ? "This machine does not have it. Its settings are preserved and saved back untouched, so the preset restores in full anywhere the plugin exists."
              : state.error || "The plugin could not be started."}
          </span>
        </div>
      </div>

      <div className={s.actionRow}>
        <FxPicker label="Replace" variant="primary" onPick={onPick} />
        <Button size="sm" variant="ghost" onClick={onRemove}>
          Remove
        </Button>
      </div>
    </div>
  );
}
