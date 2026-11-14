/**
 * Header (44px) — ui/DESIGN.md §Layout.
 *
 * Left to right: wordmark, preset control (previous | name menu | next, then
 * "Save as"), flexible space, A/B compare with its "Save to" menu, TONE3000
 * (opens the catalog drawer), Settings.
 *
 * The settings sheet is NOT owned here: the header calls `onOpenSettings` and
 * App.tsx owns the open state and renders the sheet, so there is one instance.
 */
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type MouseEvent as ReactMouseEvent,
} from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import s from "./header.module.css";
import {
  Button,
  CheckIcon,
  ChevronDownIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  DownloadIcon,
  GearIcon,
  IconButton,
  Menu,
  Scrim,
  Segmented,
  Tooltip,
  HeartIcon, TrashIcon,
  cx,
  kit,
  useMenu,
  type MenuEntry,
  type SegmentedOption,
} from "../../components";
import { useStore } from "../../store";
import type { AbSlot, PresetInfo } from "../../bridge";
import { duration, ease } from "../../theme/motion";

export interface HeaderProps {
  /** Opens the settings sheet; App.tsx owns the sheet's state. */
  onOpenSettings(): void;
}

export function Header({ onOpenSettings }: HeaderProps) {
  return (
    <div className={s.bar}>
      <span className={s.wordmark}>tubamp</span>

      <PresetControl />

      <div className={s.spacer} />

      <AbControl />
      <T3kButton />
      <SettingsButton onOpenSettings={onOpenSettings} />
    </div>
  );
}

/* ───────────────────────────── Settings button ─────────────────────────── */

/**
 * A dot while a newer version waits for a decision the user has not yet seen
 * in Settings: it changes what they do next (open Settings). It goes once the
 * offer has been seen, and never marks a pending restart, which Settings
 * cannot act on.
 */
function SettingsButton({ onOpenSettings }: HeaderProps) {
  const pending = useStore((st) => {
    const { phase } = st.update;
    const offered = phase.kind === "available" || phase.kind === "manual";
    return offered && st.updateSeen !== phase.version ? phase.version : null;
  });
  return (
    <Tooltip label={pending ? `Settings · ${pending} available` : "Settings"} placement="bottom">
      <IconButton aria-label="Settings" className={s.settings} onClick={onOpenSettings}>
        <GearIcon />
        {pending && <span className={s.settingsDot} aria-hidden />}
      </IconButton>
    </Tooltip>
  );
}

/* ───────────────────────────── Preset control ──────────────────────────── */

function PresetControl() {
  const presets = useStore((st) => st.presets);
  const currentPresetName = useStore((st) => st.currentPresetName);
  const loadPreset = useStore((st) => st.loadPreset);
  const savePresetAs = useStore((st) => st.savePresetAs);
  const setPresetFavorite = useStore((st) => st.setPresetFavorite);
  const deletePreset = useStore((st) => st.deletePreset);
  const {
    ref: menuAnchorRef,
    anchor: menuAnchor,
    open: menuOpen,
    toggle: toggleMenu,
    close: closeMenu,
  } = useMenu<HTMLSpanElement>();

  /** `stepPreset(delta)` — wraps, always, per current-ui-inventory.md. */
  const step = useCallback(
    (delta: number) => {
      const n = presets.length;
      if (n === 0) return;
      const currentIndex = presets.findIndex(
        (p) => p.name === currentPresetName,
      );
      const next = (((currentIndex + delta) % n) + n) % n;
      const target = presets[next];
      if (target) void loadPreset(target.path);
    },
    [presets, currentPresetName, loadPreset],
  );

  return (
    <div className={s.preset}>
      <div className={s.presetNav}>
        <Tooltip label="Previous preset" placement="bottom">
          <IconButton
            aria-label="Previous preset"
            disabled={presets.length === 0}
            onClick={() => step(-1)}
          >
            <ChevronLeftIcon />
          </IconButton>
        </Tooltip>

        {/* The kit Button takes no ref; the menu anchors to this wrapper. */}
        <span ref={menuAnchorRef} className={s.anchor}>
          <Button
            className={s.presetTrigger}
            title="Preset"
            aria-haspopup="menu"
            aria-expanded={menuOpen}
            onClick={toggleMenu}
          >
            {/* A preset switch reads as a crossfade, not a layout shift. */}
            <AnimatePresence mode="popLayout" initial={false}>
              <motion.span
                key={currentPresetName || "unsaved"}
                className={cx(
                  s.presetName,
                  !currentPresetName && s.presetNameEmpty,
                )}
                initial={{ opacity: 0 }}
                animate={{ opacity: 1 }}
                exit={{ opacity: 0 }}
                transition={{ duration: duration.fast, ease: ease.inOut }}
              >
                {currentPresetName || "Unsaved"}
              </motion.span>
            </AnimatePresence>
            <ChevronDownIcon className={s.presetChevron} />
          </Button>
        </span>

        <Tooltip label="Next preset" placement="bottom">
          <IconButton
            aria-label="Next preset"
            disabled={presets.length === 0}
            onClick={() => step(1)}
          >
            <ChevronRightIcon />
          </IconButton>
        </Tooltip>
      </div>

      <Button
        variant="ghost"
        aria-label="Save preset"
        onClick={() => void savePresetAs()}
      >
        Save as
      </Button>

      <PresetMenu
        anchor={menuAnchor}
        open={menuOpen}
        onClose={closeMenu}
        presets={presets}
        currentPresetName={currentPresetName}
        onSelect={(p) => void loadPreset(p.path)}
        onToggleFavorite={(p) => void setPresetFavorite(p.path, !p.favorite)}
        onDelete={(p) => void deletePreset(p.path)}
      />
    </div>
  );
}

/**
 * Preset dropdown. The shared `Menu` has no per-row secondary actions, so this
 * is a portal built from the same kit classes (`kit.menu`, `kit.menuScrim`,
 * `kit.menuSectionLabel`) so it reads as the same menu.
 */
interface PresetMenuProps {
  anchor: HTMLElement | null;
  open: boolean;
  onClose(): void;
  presets: PresetInfo[];
  currentPresetName: string;
  onSelect(preset: PresetInfo): void;
  onToggleFavorite(preset: PresetInfo): void;
  onDelete(preset: PresetInfo): void;
}

function PresetMenu({
  anchor,
  open,
  onClose,
  presets,
  currentPresetName,
  onSelect,
  onToggleFavorite,
  onDelete,
}: PresetMenuProps) {
  const menuRef = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ top: 0, left: 0 });

  const place = useCallback(() => {
    if (!anchor || !menuRef.current) return;
    const a = anchor.getBoundingClientRect();
    const m = menuRef.current.getBoundingClientRect();
    let top = a.bottom + 4;
    if (top + m.height > window.innerHeight - 8) {
      top = Math.max(8, a.top - m.height - 4);
    }
    const left = Math.min(Math.max(8, a.left), window.innerWidth - m.width - 8);
    setPos({ top, left });
  }, [anchor]);

  useLayoutEffect(() => {
    if (open) place();
  }, [open, place, presets.length]);

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    window.addEventListener("resize", place);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", place);
    };
  }, [open, onClose, place]);

  const favorites = presets.filter((p) => p.favorite);
  const others = presets.filter((p) => !p.favorite);

  const row = (p: PresetInfo) => (
    <PresetRow
      key={p.path}
      preset={p}
      current={p.name === currentPresetName}
      onSelect={() => {
        onSelect(p);
        onClose();
      }}
      onToggleFavorite={() => onToggleFavorite(p)}
      onDelete={() => onDelete(p)}
    />
  );

  return createPortal(
    <AnimatePresence>
      {open && (
        <>
          <Scrim className={kit.menuScrim} onPointerDown={onClose} />
          <motion.div
            ref={menuRef}
            role="menu"
            aria-label="Presets"
            className={cx(kit.menu, s.presetMenu)}
            style={{ top: pos.top, left: pos.left }}
            initial={{ opacity: 0, y: -4 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -4 }}
            transition={{ duration: duration.instant, ease: ease.out }}
          >
            {presets.length === 0 && (
              <div className={s.presetEmpty}>No presets</div>
            )}
            {favorites.length > 0 && (
              <>
                <div className={kit.menuSectionLabel}>Favorites</div>
                {favorites.map(row)}
                {others.length > 0 && (
                  <div className={kit.menuSectionLabel}>Presets</div>
                )}
              </>
            )}
            {others.map(row)}
          </motion.div>
        </>
      )}
    </AnimatePresence>,
    document.body,
  );
}

interface PresetRowProps {
  preset: PresetInfo;
  current: boolean;
  onSelect(): void;
  onToggleFavorite(): void;
  onDelete(): void;
}

function PresetRow({
  preset: p,
  current,
  onSelect,
  onToggleFavorite,
  onDelete,
}: PresetRowProps) {
  return (
    <div className={s.row}>
      <button
        type="button"
        role="menuitem"
        aria-current={current || undefined}
        className={s.rowMain}
        onClick={onSelect}
      >
        <span className={s.rowCheck}>{current && <CheckIcon />}</span>
        <span className={s.rowLabel}>{p.name}</span>
      </button>
      <div className={s.rowActions}>
        <IconButton
          aria-label={
            p.favorite
              ? `Remove ${p.name} from favorites`
              : `Favorite ${p.name}`
          }
          aria-pressed={p.favorite}
          size="sm"
          className={cx(s.rowAction, p.favorite && s.rowActionOn)}
          onClick={(e) => {
            e.stopPropagation();
            onToggleFavorite();
          }}
        >
          <HeartIcon filled={p.favorite} />
        </IconButton>
        <IconButton
          aria-label={`Delete ${p.name}`}
          size="sm"
          destructive
          className={s.rowAction}
          onClick={(e) => {
            e.stopPropagation();
            onDelete();
          }}
        >
          <TrashIcon />
        </IconButton>
      </div>
    </div>
  );
}

/* ────────────────────────────────── A/B ─────────────────────────────────── */

type AbValue = "a" | "b";

/**
 * Click recalls a slot, shift-click captures into it. Logic gives the WebView
 * no keyboard, so storing also has a pointer-only path: the "Save to" menu.
 */
function AbControl() {
  const ab = useStore((st) => st.ab);
  const abRecall = useStore((st) => st.abRecall);
  const abCapture = useStore((st) => st.abCapture);
  const {
    ref: saveAnchorRef,
    anchor: saveAnchor,
    open: saveOpen,
    toggle: toggleSave,
    close: closeSave,
  } = useMenu<HTMLSpanElement>();

  const options: SegmentedOption<AbValue>[] = [
    {
      value: "a",
      label: "A",
      dot: ab.aHasState,
      title: "Recall A - shift-click to capture",
    },
    {
      value: "b",
      label: "B",
      dot: ab.bHasState,
      title: "Recall B - shift-click to capture",
    },
  ];

  const handleChange = useCallback(
    (value: AbValue, event: ReactMouseEvent<HTMLButtonElement>) => {
      const slot: AbSlot = value === "a" ? 0 : 1;
      if (event.shiftKey) void abCapture(slot);
      else void abRecall(slot);
    },
    [abCapture, abRecall],
  );

  const entries: MenuEntry[] = [
    { id: "save-a", label: "Save to A", onSelect: () => void abCapture(0) },
    { id: "save-b", label: "Save to B", onSelect: () => void abCapture(1) },
  ];

  return (
    <div className={s.ab}>
      <Segmented
        options={options}
        value={ab.activeSlot === 0 ? "a" : "b"}
        onChange={handleChange}
        aria-label="A/B compare"
      />
      <span ref={saveAnchorRef} className={s.anchor}>
        <Button
          variant="ghost"
          size="sm"
          aria-label="Save to A or B"
          aria-haspopup="menu"
          aria-expanded={saveOpen}
          onClick={toggleSave}
        >
          Save to
          <ChevronDownIcon className={s.chevronSm} />
        </Button>
      </span>
      <Menu
        anchor={saveAnchor}
        open={saveOpen}
        onClose={closeSave}
        entries={entries}
        align="end"
      />
    </div>
  );
}

/* ──────────────────────────────── TONE3000 ──────────────────────────────── */

/** Opens (or closes) the catalog drawer; sign-in state lives in the tooltip. */
function T3kButton() {
  const t3k = useStore((st) => st.t3k);
  const open = useStore((st) => st.browser.open);
  const kind = useStore((st) => st.browser.kind);
  const openBrowser = useStore((st) => st.openT3kBrowser);
  const closeBrowser = useStore((st) => st.closeT3kBrowser);

  const status = t3k.authenticated
    ? `Signed in${t3k.username ? ` as ${t3k.username}` : ""}`
    : t3k.configured
      ? "Key configured"
      : "Not configured";

  return (
    <Tooltip label={`Browse TONE3000 · ${status}`} placement="bottom">
      <Button
        aria-label="TONE3000"
        aria-pressed={open}
        icon={<DownloadIcon />}
        onClick={() => (open ? closeBrowser() : openBrowser(kind))}
      >
        TONE3000
      </Button>
    </Tooltip>
  );
}
