/**
 * The block editors' shared grammar (ui/DESIGN.md §Editors).
 *
 * Every editor is the same sentence:
 *
 *   header slot   the block's type / mode / switches, beside its name
 *   display       what the effect is doing, drawn; handles for direct edits
 *   lead          (amp, cab, plugin only) what is loaded in the block
 *   controls      knobs in rows: the two or three that matter most are large
 *                 and come first, the rest are small
 *
 *   <HeaderSlot><TypeStepper id="reverb_type" onPick={…} /></HeaderSlot>
 *   <Editor display={<ReverbDisplay block={block} />}>
 *     <Row>
 *       <K id="reverb_decay" size="lg" /> …
 *     </Row>
 *     <Row>
 *       <Group label="Filter"><K id="reverb_lowcut" /> …</Group>
 *     </Row>
 *   </Editor>
 *
 * Rules the pieces enforce so the editors cannot drift apart:
 *  - every parameter has exactly one knob (or switch / menu); the display may
 *    let you drag the same parameter but never prints its number again;
 *  - when the band is too narrow for a display it is dropped, never squeezed
 *    (`data-narrow` on the band, see ./index.tsx);
 *  - labels are sentence case through `paramLabel`, values through the Knob's
 *    own formatting.
 */
import {
  createContext,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  ChevronDownIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  Knob,
  MoreIcon,
  Menu,
  Segmented,
  Toggle,
  useMenu,
  cx,
  type KnobSize,
  type MenuEntry,
} from "../../components";
import { useComboParam, useToggleParam } from "../../hooks";
import type { ComboParamId, SliderParamId, ToggleParamId } from "../../bridge";
import { tween } from "../../theme/motion";
import { paramLabel } from "../../theme/words";
import { PlotDefs } from "./display";
import s from "./ui.module.css";

/* ───────────────────────────── header slot ─────────────────────────────── */

/** The element in the band's header that an editor's own controls portal into. */
export const HeaderSlotContext = createContext<HTMLElement | null>(null);

/** Renders its children in the band's header, right of the block's name. */
export function HeaderSlot({ children }: { children: ReactNode }) {
  const slot = useContext(HeaderSlotContext);
  return slot ? createPortal(children, slot) : null;
}

/** Whether the band is narrow (see `useNarrow`), for controls that change form
 *  rather than size: a segmented choice becomes a menu. */
export const NarrowContext = createContext(false);

/** True while `el` is narrower than `below` px — the band with the TONE3000
 *  drawer open in a small window. Editors drop their display then. */
export function useNarrow(el: HTMLElement | null, below = 780): boolean {
  const [narrow, setNarrow] = useState(false);
  useEffect(() => {
    if (!el) return;
    const measure = (): void => setNarrow(el.clientWidth < below);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [el, below]);
  return narrow;
}

/* ───────────────────────────── choice params ───────────────────────────── */

export interface ChoiceProps {
  id: ComboParamId;
  /** Accessible name, and the visible caption where the control shows one. */
  label: string;
  /** Called after the user picks a different entry (not on automation or recall). */
  onPick?: (index: number) => void;
}

function useChoice({ id, onPick }: ChoiceProps) {
  const param = useComboParam(id);
  const pick = (index: number): void => {
    if (index === param.index || index < 0 || index >= param.choices.length) return;
    param.setIndex(index);
    onPick?.(index);
  };
  return { param, pick };
}

/** How far the type's name travels when an arrow steps it, px. */
const STEP_SHIFT = 6;

/* `custom` is the signed shift: the new name comes in from the side the arrow
   points to, the old one leaves the other way. */
const STEP_VARIANTS = {
  enter: (shift: number) => ({ opacity: 0, x: shift }),
  rest: { opacity: 1, x: 0 },
  exit: (shift: number) => ({ opacity: 0, x: -shift }),
};

/**
 * The block's type: ‹ Plate ›. The arrows step through the types without
 * opening anything (how you audition algorithms), the name opens the list.
 */
export function TypeStepper(props: ChoiceProps) {
  const { param, pick } = useChoice(props);
  const { ref, anchor, open, toggle, close } = useMenu();
  const reduced = useReducedMotion() ?? false;
  /* Which way the name last moved: an arrow slides it that way, a pick from
     the list (or a recall) only crossfades. */
  const [dir, setDir] = useState(0);
  const count = param.choices.length;
  const step = (delta: number): void => {
    if (count === 0) return;
    setDir(delta);
    pick((param.index + delta + count) % count);
  };
  const entries: MenuEntry[] = param.choices.map((choice, index) => ({
    id: `${props.id}:${index}`,
    label: choice,
    selected: index === param.index,
    onSelect: () => {
      setDir(0);
      pick(index);
    },
  }));
  const shift = reduced ? 0 : dir * STEP_SHIFT;

  return (
    <div className={s.stepper} data-param-index={param.parameterIndex} title={param.name}>
      <button
        type="button"
        className={s.stepperArrow}
        aria-label={`Previous ${props.label.toLowerCase()}`}
        onClick={() => step(-1)}
      >
        <ChevronLeftIcon size={14} />
      </button>
      <button
        ref={ref}
        type="button"
        className={s.stepperName}
        aria-label={props.label}
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
      >
        <AnimatePresence mode="popLayout" initial={false} custom={shift}>
          <motion.span
            key={param.text}
            className={s.stepperText}
            custom={shift}
            variants={STEP_VARIANTS}
            initial="enter"
            animate="rest"
            exit="exit"
            transition={tween.micro}
          >
            {param.text || "—"}
          </motion.span>
        </AnimatePresence>
      </button>
      <button
        type="button"
        className={s.stepperArrow}
        aria-label={`Next ${props.label.toLowerCase()}`}
        onClick={() => step(1)}
      >
        <ChevronRightIcon size={14} />
      </button>
      <Menu anchor={anchor} open={open} onClose={close} entries={entries} minWidth={160} />
    </div>
  );
}

/** A choice with few options, all visible: Mono | Stereo | Ping-pong. */
export function ChoiceSegment(props: ChoiceProps & { caption?: boolean }) {
  const narrow = useContext(NarrowContext);
  return narrow ? <ChoiceMenu {...props} bare /> : <ChoiceSegmentWide {...props} />;
}

function ChoiceSegmentWide(props: ChoiceProps & { caption?: boolean }) {
  const { param, pick } = useChoice(props);
  return (
    <div className={s.field} data-param-index={param.parameterIndex} title={param.name}>
      {props.caption && <span className={s.fieldLabel}>{props.label}</span>}
      <Segmented<string>
        aria-label={props.label}
        value={String(param.index)}
        options={param.choices.map((choice, index) => ({ value: String(index), label: choice }))}
        onChange={(value) => pick(Number(value))}
      />
    </div>
  );
}

/** A choice with many options, or one that is rarely changed: caption + menu. */
export function ChoiceMenu(
  props: ChoiceProps & {
    width?: number;
    disabled?: boolean;
    /** No visible caption (the value says enough: "Ping-Pong"). */
    bare?: boolean;
  },
) {
  const { param, pick } = useChoice(props);
  const { ref, anchor, open, toggle, close } = useMenu();
  const entries: MenuEntry[] = param.choices.map((choice, index) => ({
    id: `${props.id}:${index}`,
    label: choice,
    selected: index === param.index,
    onSelect: () => pick(index),
  }));
  return (
    <div
      className={cx(s.field, props.disabled && s.fieldDisabled)}
      data-param-index={param.parameterIndex}
      title={param.name}
    >
      {!props.bare && <span className={s.fieldLabel}>{props.label}</span>}
      <button
        ref={ref}
        type="button"
        className={s.select}
        style={props.width ? { width: props.width } : undefined}
        disabled={props.disabled}
        aria-label={props.label}
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
      >
        <span className={s.selectValue}>{param.text || "—"}</span>
        <ChevronDownIcon size={14} />
      </button>
      <Menu anchor={anchor} open={open} onClose={close} entries={entries} minWidth={props.width ?? 140} />
    </div>
  );
}

/** A boolean parameter: caption + switch. */
export function SwitchField({
  id,
  label,
  disabled,
}: {
  id: ToggleParamId;
  label: string;
  disabled?: boolean;
}) {
  const param = useToggleParam(id);
  return (
    <div
      className={cx(s.field, disabled && s.fieldDisabled)}
      data-param-index={param.parameterIndex}
      title={param.name}
    >
      <span className={cx(s.fieldLabel, s.fieldLabelKeep)}>{label}</span>
      <Toggle checked={param.value} onChange={param.setValue} disabled={disabled} aria-label={label} />
    </div>
  );
}

/** A switch for a view preference (not a plugin parameter), with SwitchField's
 *  look: nothing reaches the host, so there is no parameter index. */
export function ViewSwitch({
  label,
  checked,
  onChange,
  title,
}: {
  label: string;
  checked: boolean;
  onChange(next: boolean): void;
  title?: string;
}) {
  return (
    <div className={s.field} title={title}>
      <span className={cx(s.fieldLabel, s.fieldLabelKeep)}>{label}</span>
      <Toggle checked={checked} onChange={onChange} aria-label={label} />
    </div>
  );
}

/** A view state that latches (Freeze): a short word on a button that reads as
 *  pressed, in the accent, while it is on. */
export function LatchButton({
  label,
  pressed,
  onChange,
  title,
}: {
  label: string;
  pressed: boolean;
  onChange(next: boolean): void;
  title?: string;
}) {
  return (
    <button
      type="button"
      className={cx(s.latch, pressed && s.latchOn)}
      aria-pressed={pressed}
      aria-label={label}
      title={title}
      onClick={() => onChange(!pressed)}
    >
      {label}
    </button>
  );
}

/** Header controls that belong together on one plate, split by hairlines
 *  (the stepper's look): Freeze and its settings read as the analyser's. */
export function ControlPlate({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className={s.plate} role="group" aria-label={label}>
      {children}
    </div>
  );
}

/** Settings that are rarely changed, behind a "More" button (DESIGN.md §Less).
 *  `label` is its accessible name and its tooltip. */
export function MoreMenu({ label, entries, width = 160 }: { label: string; entries: MenuEntry[]; width?: number }) {
  const { ref, anchor, open, toggle, close } = useMenu();
  return (
    <>
      <button
        ref={ref}
        type="button"
        className={s.more}
        aria-label={label}
        title={label}
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
      >
        <MoreIcon />
      </button>
      <Menu anchor={anchor} open={open} onClose={close} entries={entries} align="end" minWidth={width} />
    </>
  );
}

/** A boolean parameter whose two positions have names (Script | Block), where
 *  a bare switch would not say which one is on. */
export function ToggleSegment({
  id,
  label,
  off,
  on,
}: {
  id: ToggleParamId;
  label: string;
  /** Text for the off and on positions. */
  off: string;
  on: string;
}) {
  const param = useToggleParam(id);
  return (
    <div className={s.field} data-param-index={param.parameterIndex} title={param.name}>
      <span className={s.fieldLabel}>{label}</span>
      <Segmented<"off" | "on">
        aria-label={label}
        value={param.value ? "on" : "off"}
        options={[
          { value: "off", label: off },
          { value: "on", label: on },
        ]}
        onChange={(value) => param.setValue(value === "on")}
      />
    </div>
  );
}

/* ───────────────────────────────── layout ──────────────────────────────── */

/**
 * An editor's body: the display (optional) on the left, taking every spare
 * pixel, and the control rows on the right at their natural width. Without a
 * display the rows are centred in the band.
 */
export function Editor({
  lead,
  display,
  children,
}: {
  /** What is loaded in the block (a capture, an IR, a plugin): sits first, at
   *  its natural width, before the display. */
  lead?: ReactNode;
  display?: ReactNode;
  children?: ReactNode;
}) {
  return (
    <div className={cx(s.editor, !display && s.editorPlain)}>
      {lead && <div className={s.lead}>{lead}</div>}
      {display && (
        <div className={s.display}>
          <PlotDefs />
          {display}
        </div>
      )}
      {children && <div className={s.controls}>{children}</div>}
    </div>
  );
}

/** One row of knobs / groups inside an editor. */
export function Row({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cx(s.row, className)}>{children}</div>;
}

/** Knobs that belong together ("Filter", "Sidechain"), under an optional caption. */
export function Group({
  label,
  action,
  children,
}: {
  label?: string;
  /** A control that belongs to the whole group (its on/off switch), on the
   *  caption's line. */
  action?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className={s.group}>
      {(label || action) && (
        <span className={s.groupHead}>
          {label && <span className={s.groupLabel}>{label}</span>}
          {action}
        </span>
      )}
      <div className={s.groupItems}>{children}</div>
    </div>
  );
}

/** A hairline between two groups in a row. Use sparingly: space separates too. */
export function Divider() {
  return <span className={s.divider} aria-hidden />;
}

export interface KProps {
  id: SliderParamId;
  /** Defaults to the backend's parameter name; either way it is sentence-cased. */
  label?: string;
  /** `lg` for the two or three controls that define the effect; `sm` otherwise. */
  size?: KnobSize;
  /** The engine ignores this control right now (another type, sync on). */
  disabled?: boolean;
  /** Replaces the number (a synced time shows its note value). */
  valueText?: string;
  format?: (scaled: number) => string;
  /** A fact about this control that changes how it is used, as a tooltip. */
  tip?: string;
}

/** One knob in a fixed-width cell, so rows line up across editors. */
export function K({ id, label, size = "sm", disabled, valueText, format, tip }: KProps) {
  return (
    <div className={cx(s.cell, size === "lg" && s.cellLg, size === "md" && s.cellMd)}>
      <Knob
        id={id}
        label={label === undefined ? undefined : paramLabel(label)}
        size={size}
        disabled={disabled}
        valueText={valueText}
        format={format}
        title={tip}
      />
    </div>
  );
}
