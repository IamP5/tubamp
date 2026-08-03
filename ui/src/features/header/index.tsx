/**
 * Header bar (48px) — docs/REACT-UI.md §UX structure, current-ui-inventory.md
 * §HeaderBar.
 *
 * Order left→right: brand → preset pill (prev | name menu | next | save) →
 * flexible spacer → A/B segmented → T3K status glyph → settings gear.
 *
 * The settings sheet is NOT owned here. The header only holds its two triggers
 * (gear + T3K status glyph) and calls `onOpenSettings`; App.tsx owns the
 * open/close state and renders the sheet, so there is exactly one instance.
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
  ChevronLeftIcon,
  ChevronRightIcon,
  GearIcon,
  IconButton,
  SaveIcon,
  Segmented,
  StarIcon,
  Tooltip,
  TrashIcon,
  cx,
  kit,
  useMenu,
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
      <div className={s.brand}>
        <span className={s.brandDot} />
        <span className={s.wordmark}>tubamp</span>
      </div>

      <PresetPill />

      <div className={s.spacer} />

      <AbControl />
      <T3kGlyph onOpenSettings={onOpenSettings} />
      <Tooltip label="Settings">
        <IconButton aria-label="Settings" onClick={onOpenSettings}>
          <GearIcon />
        </IconButton>
      </Tooltip>
    </div>
  );
}

/* ─────────────────────────────── Preset pill ───────────────────────────── */

function PresetPill() {
  const presets = useStore((st) => st.presets);
  const currentPresetName = useStore((st) => st.currentPresetName);
  const loadPreset = useStore((st) => st.loadPreset);
  const savePresetAs = useStore((st) => st.savePresetAs);
  const setPresetFavorite = useStore((st) => st.setPresetFavorite);
  const deletePreset = useStore((st) => st.deletePreset);
  const {
    ref: presetMenuRef,
    anchor: presetMenuAnchor,
    open: presetMenuOpen,
    toggle: togglePresetMenu,
    close: closePresetMenu,
  } = useMenu<HTMLButtonElement>();

  /** `stepPreset(delta)` — wraps, always, per current-ui-inventory.md. */
  const step = useCallback(
    (delta: number) => {
      const n = presets.length;
      if (n === 0) return;
      const currentIndex = presets.findIndex((p) => p.name === currentPresetName);
      const next = (((currentIndex + delta) % n) + n) % n;
      const target = presets[next];
      if (target) void loadPreset(target.path);
    },
    [presets, currentPresetName, loadPreset],
  );

  return (
    <div className={s.pill}>
      <Tooltip label="Previous preset">
        <IconButton
          aria-label="Previous preset"
          size="sm"
          disabled={presets.length === 0}
          onClick={() => step(-1)}
        >
          <ChevronLeftIcon />
        </IconButton>
      </Tooltip>

      <button
        ref={presetMenuRef}
        type="button"
        className={s.pillName}
        title="Preset"
        onClick={togglePresetMenu}
      >
        {/* Preset switch reads as a crossfade, not a layout shift — pure
            opacity swap, no directional travel (motion-design.md §2.9). */}
        <AnimatePresence mode="popLayout" initial={false}>
          <motion.span
            key={currentPresetName || "unsaved"}
            className={cx(s.pillNameText, !currentPresetName && s.pillNamePlaceholder)}
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: duration.base, ease: ease.inOut }}
          >
            {currentPresetName || "Unsaved"}
          </motion.span>
        </AnimatePresence>
      </button>

      <Tooltip label="Next preset">
        <IconButton
          aria-label="Next preset"
          size="sm"
          disabled={presets.length === 0}
          onClick={() => step(1)}
        >
          <ChevronRightIcon />
        </IconButton>
      </Tooltip>

      <Tooltip label="Save preset">
        <IconButton aria-label="Save preset" size="sm" onClick={() => void savePresetAs()}>
          <SaveIcon />
        </IconButton>
      </Tooltip>

      <PresetMenu
        anchor={presetMenuAnchor}
        open={presetMenuOpen}
        onClose={closePresetMenu}
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
 * Preset dropdown: listing + favorite star + delete affordance per row. The
 * shared `Menu` primitive (components/Menu.tsx) doesn't support per-row
 * secondary actions, so this is a bespoke portal built from the same kit
 * classes (`kit.menu`/`kit.menuScrim`) for visual consistency.
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
    let top = a.bottom + 6;
    if (top + m.height > window.innerHeight - 8) {
      top = Math.max(8, a.top - m.height - 6);
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

  return createPortal(
    <AnimatePresence>
      {open && (
        <>
          <div className={kit.menuScrim} onPointerDown={onClose} />
          <motion.div
            ref={menuRef}
            role="menu"
            className={cx(kit.menu, s.presetMenu)}
            style={{ top: pos.top, left: pos.left }}
            initial={{ opacity: 0, y: -4 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -4 }}
            transition={{ duration: duration.instant, ease: ease.out }}
          >
            {presets.length === 0 && <div className={s.presetEmpty}>No presets</div>}
            {presets.map((p) => (
              <div
                key={p.path}
                className={cx(s.presetRow, p.name === currentPresetName && kit.menuItemSelected)}
              >
                <button
                  type="button"
                  role="menuitem"
                  className={s.presetRowMain}
                  onClick={() => {
                    onSelect(p);
                    onClose();
                  }}
                >
                  <span className={s.presetRowLabel}>{p.name}</span>
                </button>
                <IconButton
                  aria-label={p.favorite ? `Remove ${p.name} from favorites` : `Favorite ${p.name}`}
                  size="sm"
                  className={cx(s.presetRowFavorite, p.favorite && s.presetRowFavoriteOn)}
                  onClick={(e) => {
                    e.stopPropagation();
                    onToggleFavorite(p);
                  }}
                >
                  <StarIcon filled={p.favorite} />
                </IconButton>
                <IconButton
                  aria-label={`Delete ${p.name}`}
                  size="sm"
                  destructive
                  onClick={(e) => {
                    e.stopPropagation();
                    onDelete(p);
                  }}
                >
                  <TrashIcon />
                </IconButton>
              </div>
            ))}
          </motion.div>
        </>
      )}
    </AnimatePresence>,
    document.body,
  );
}

/* ────────────────────────────────── A/B ─────────────────────────────────── */

type AbValue = "a" | "b";

function AbControl() {
  const ab = useStore((st) => st.ab);
  const abRecall = useStore((st) => st.abRecall);
  const abCapture = useStore((st) => st.abCapture);

  const options: SegmentedOption<AbValue>[] = [
    { value: "a", label: "A", dot: ab.aHasState, title: "Recall A - shift-click to capture" },
    { value: "b", label: "B", dot: ab.bHasState, title: "Recall B - shift-click to capture" },
  ];

  const handleChange = useCallback(
    (value: AbValue, event: ReactMouseEvent<HTMLButtonElement>) => {
      const slot: AbSlot = value === "a" ? 0 : 1;
      if (event.shiftKey) void abCapture(slot);
      else void abRecall(slot);
    },
    [abCapture, abRecall],
  );

  return (
    <Segmented
      options={options}
      value={ab.activeSlot === 0 ? "a" : "b"}
      onChange={handleChange}
      aria-label="A/B compare"
    />
  );
}

/* ────────────────────────────── T3K status glyph ────────────────────────── */

function T3kGlyph({ onOpenSettings }: { onOpenSettings(): void }) {
  const t3k = useStore((st) => st.t3k);
  const label = t3k.authenticated
    ? "TONE3000 — signed in"
    : t3k.configured
      ? "TONE3000 — key configured"
      : "TONE3000 — not configured";

  return (
    <Tooltip label={label}>
      <IconButton
        aria-label={label}
        size="sm"
        onClick={onOpenSettings}
        className={cx(s.t3k, t3k.configured && s.t3kConfigured, t3k.authenticated && s.t3kAuthenticated)}
      >
        <T3kIcon />
      </IconButton>
    </Tooltip>
  );
}

function T3kIcon() {
  return (
    <svg width="14" height="14" viewBox="0 0 14 14" fill="none" aria-hidden="true">
      <path
        d="M7 1.2L12.3 4v6L7 12.8 1.7 10V4z"
        stroke="currentColor"
        strokeWidth="1.3"
        strokeLinejoin="round"
      />
      <circle cx="7" cy="7" r="1.5" fill="currentColor" />
    </svg>
  );
}
