/**
 * Choice-parameter control (`amp_out_mode`, `mod_type`) — the React port of the
 * native `ComboBoxAttachment` combos. A menu, never a native <select>: the
 * plugin WebView has no reliable keyboard in Logic, and the Menu primitive is
 * the app-wide dropdown.
 *
 * `choices` arrives with the async properties push, so an empty list renders an
 * empty menu rather than assuming three items.
 */
import { Menu, useMenu, ChevronDownIcon, type MenuEntry } from "../../components";
import { useComboParam } from "../../hooks";
import type { ComboParamId } from "../../bridge";
import s from "./panel.module.css";

export interface ParamMenuProps {
  id: ComboParamId;
  /** Micro-label to the left of the trigger. */
  label: string;
  minWidth?: number;
  /** Trigger width; defaults to filling the row. */
  width?: number;
}

export function ParamMenu({ id, label, minWidth = 168, width }: ParamMenuProps) {
  const param = useComboParam(id);
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu();

  const entries: MenuEntry[] = param.choices.map((choice, index) => ({
    id: `${id}:${index}`,
    label: choice,
    selected: index === param.index,
    onSelect: () => param.setIndex(index),
  }));

  return (
    <div className={s.field} data-param-index={param.parameterIndex}>
      <span className={s.fieldLabel}>{label}</span>
      <button
        ref={anchorRef}
        type="button"
        className={s.select}
        style={width ? { width } : undefined}
        onClick={toggle}
        title={param.name}
        aria-haspopup="menu"
        aria-expanded={open}
      >
        <span className={s.selectValue}>{param.text || "—"}</span>
        <ChevronDownIcon size={14} />
      </button>
      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        minWidth={minWidth}
      />
    </div>
  );
}
