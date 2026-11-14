/**
 * Plugin slot editor — one external Audio Unit slot (docs/AU-SLOTS.md,
 * docs/REACT-UI.md §External AudioUnit slots).
 *
 * Everything it shows is driven by `FxSlotState`, in this order:
 *
 *   not supported   one line, nothing to do
 *   empty           "No plugin in this slot" + Choose plugin (the one primary)
 *   loading         a spinner and the plugin's name
 *   missing/failed  why, in one line, + Choose another; Remove in the header
 *   live            header: the plugin menu (picking replaces it), Open editor,
 *                   Remove; body: its parameters as small knobs, paged
 *
 * The picker is mouse-only, with no search field: Logic gives the WebView no
 * keyboard, so a typed filter would be dead UI in the host that matters most.
 *
 * This body is also where the plugin's own editor is mounted into our window
 * ("Open editor") and, crucially, where it is unmounted again: the embed is a
 * native view that would otherwise outlive the panel that asked for it.
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import {
  Button,
  ChevronDownIcon,
  IconButton,
  Menu,
  Spinner,
  Tooltip,
  cx,
  useMenu,
  type ButtonVariant,
  type IconProps,
  type MenuEntry,
} from "../../components";
import { bridge, type FxPluginEntry, type FxSlotIndex, type FxSlotState } from "../../bridge";
import { useStore } from "../../store";
import { FxParamGrid } from "./FxParamGrid";
import { HeaderSlot } from "./ui";
import f from "./fxSlot.module.css";

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

/* ──────────────────────────────── the catalog ──────────────────────────── */

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
  current: string,
  onPick: (identifier: string) => void,
): MenuEntry[] {
  if (plugins === null)
    return [{ id: "scanning", label: "Scanning installed plugins…", disabled: true }];
  if (plugins.length === 0)
    return [{ id: "empty", label: "No Audio Unit effects installed", disabled: true }];

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
        selected: plugin.identifier === current,
        onSelect: () => onPick(plugin.identifier),
      });
    }
  }
  return entries;
}

/** A button that opens the catalog. */
function PickerButton({
  label,
  variant,
  onPick,
}: {
  label: string;
  variant?: ButtonVariant;
  onPick(identifier: string): void;
}) {
  const { ref, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();
  const plugins = useFxCatalog();
  const entries = useMemo(() => catalogEntries(plugins, "", onPick), [plugins, onPick]);
  return (
    <>
      <span ref={ref} className={f.anchor}>
        <Button variant={variant} onClick={toggle} aria-haspopup="menu" aria-expanded={open}>
          {label}
        </Button>
      </span>
      <Menu anchor={anchor} open={open} onClose={close} entries={entries} minWidth={280} />
    </>
  );
}

/**
 * The loaded plugin as a select: its name on the trigger, the catalog in the
 * menu (picking another replaces it), then the plugin's own window and Remove
 * (which also has its own button, dropped from a narrow band's header).
 */
function PluginMenu({
  state,
  onPick,
  onOpenWindow,
  onRemove,
}: {
  state: FxSlotState;
  onPick(identifier: string): void;
  onOpenWindow(): void;
  onRemove(): void;
}) {
  const { ref, anchor, open, toggle, close } = useMenu();
  const plugins = useFxCatalog();
  const entries = useMemo<MenuEntry[]>(
    () => [
      ...catalogEntries(plugins, state.identifier, onPick),
      { kind: "separator", id: "sep" },
      { id: "window", label: "Open in its own window", onSelect: onOpenWindow },
      { id: "remove", label: "Remove plugin", destructive: true, onSelect: onRemove },
    ],
    [onOpenWindow, onPick, onRemove, plugins, state.identifier],
  );
  return (
    <>
      <button
        ref={ref}
        type="button"
        className={f.select}
        title={state.manufacturer ? `${state.name} · ${state.manufacturer}` : state.name}
        aria-label="Plugin"
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
      >
        <span className={f.selectValue}>{state.name}</span>
        <ChevronDownIcon size={14} />
      </button>
      <Menu anchor={anchor} open={open} onClose={close} entries={entries} minWidth={280} />
    </>
  );
}

/** Unload: an eject mark, since the band's header already has a trash for
 *  removing the block itself. */
const EjectIcon = (p: IconProps) => (
  <svg
    width={p.size ?? 16}
    height={p.size ?? 16}
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.5}
    strokeLinecap="round"
    strokeLinejoin="round"
    aria-hidden="true"
    focusable="false"
  >
    <path d="M8 3.5l4.5 5h-9z" />
    <path d="M3.5 12h9" />
  </svg>
);

function RemoveButton({ onRemove, droppable }: { onRemove(): void; droppable?: boolean }) {
  return (
    <Tooltip label="Remove plugin" className={cx(f.fixed, droppable && f.droppable)}>
      <IconButton aria-label="Remove plugin" onClick={onRemove}>
        <EjectIcon />
      </IconButton>
    </Tooltip>
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

  /* The parameters fade in when this body saw the plugin load, and are simply
     there when the editor opens on one already running. */
  const [sawLoading, setSawLoading] = useState(false);
  if (state.loading && !sawLoading) setSawLoading(true);

  const pick = useCallback(
    (identifier: string) => void loadFxPlugin(slot, identifier),
    [loadFxPlugin, slot],
  );
  const remove = useCallback(() => void clearFxPlugin(slot), [clearFxPlugin, slot]);
  const openWindow = useCallback(() => void openFxEditor(slot), [openFxEditor, slot]);
  const toggleEmbed = useCallback(() => {
    if (embedded) void unembedFxEditor();
    else void embedFxEditor(slot);
  }, [embedFxEditor, embedded, slot, unembedFxEditor]);

  if (!supported) {
    return (
      <div className={f.state}>
        <span className={f.message}>Plugin hosting is not available in this build</span>
      </div>
    );
  }

  if (!state.occupied) {
    return (
      <div className={f.state}>
        <span className={f.message}>No plugin in this slot</span>
        <span className={f.action}>
          <PickerButton label="Choose plugin" variant="primary" onPick={pick} />
        </span>
      </div>
    );
  }

  if (state.loading) {
    return (
      <div className={f.state}>
        <span className={f.loading}>
          <Spinner size={16} />
          {state.name}
        </span>
      </div>
    );
  }

  if (!state.live) {
    /* Assigned but not running: this machine does not have it (its settings
       are kept and saved back untouched), or it refused to start. */
    return (
      <>
        <HeaderSlot>
          <RemoveButton onRemove={remove} />
        </HeaderSlot>
        <div className={f.state} role="status">
          {state.missing ? (
            <>
              <span className={f.message}>{state.name || "This plugin"} is not installed</span>
              <span className={f.hint}>Its settings are kept and saved with the session</span>
            </>
          ) : (
            <span className={cx(f.message, f.danger)}>
              {state.error || `${state.name || "The plugin"} could not be started`}
            </span>
          )}
          <span className={f.action}>
            <PickerButton label="Choose another" onPick={pick} />
          </span>
        </div>
      </>
    );
  }

  return (
    <>
      <HeaderSlot>
        <span className={f.headerControls}>
          <PluginMenu state={state} onPick={pick} onOpenWindow={openWindow} onRemove={remove} />
          <Button className={f.fixed} onClick={toggleEmbed} aria-pressed={embedded}>
            {embedded ? "Close editor" : "Open editor"}
          </Button>
          <RemoveButton onRemove={remove} droppable />
        </span>
      </HeaderSlot>
      {state.params.length > 0 ? (
        <FxParamGrid
          key={state.identifier}
          slot={slot}
          params={state.params}
          latencySamples={state.latencySamples}
          appear={sawLoading}
        />
      ) : (
        <div className={f.state}>
          <span className={f.hint}>No parameters</span>
          {state.latencySamples > 0 && (
            <span className={f.barNote}>Latency {state.latencySamples} samples</span>
          )}
        </div>
      )}
    </>
  );
}
