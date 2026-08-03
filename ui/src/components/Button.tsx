import type { ButtonHTMLAttributes, ReactNode } from "react";
import s from "./kit.module.css";
import { cx } from "./cx";

export type ButtonVariant = "default" | "primary" | "ghost" | "danger";
export type ButtonSize = "sm" | "md" | "lg";

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  /** Leading glyph (16px SVG). */
  icon?: ReactNode;
  block?: boolean;
}

const variantClass: Record<ButtonVariant, string | undefined> = {
  default: undefined,
  primary: s.buttonPrimary,
  ghost: s.buttonGhost,
  danger: s.buttonDanger,
};

const sizeClass: Record<ButtonSize, string | undefined> = {
  sm: s.buttonSm,
  md: undefined,
  lg: s.buttonLg,
};

export function Button({
  variant = "default",
  size = "md",
  icon,
  block,
  className,
  children,
  type = "button",
  ...rest
}: ButtonProps) {
  return (
    <button
      type={type}
      className={cx(
        s.button,
        variantClass[variant],
        sizeClass[size],
        block && s.buttonBlock,
        className,
      )}
      {...rest}
    >
      {icon}
      {children}
    </button>
  );
}

export interface IconButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /** 16px SVG glyph. */
  children: ReactNode;
  /** Rounded background plate behind the glyph. */
  plate?: boolean;
  size?: "sm" | "md";
  destructive?: boolean;
  /** Required — icon-only controls must still be named for a11y and tooltips. */
  "aria-label": string;
}

export function IconButton({
  plate,
  size = "md",
  destructive,
  className,
  children,
  type = "button",
  ...rest
}: IconButtonProps) {
  return (
    <button
      type={type}
      className={cx(
        s.iconButton,
        plate && s.iconButtonPlate,
        size === "sm" && s.iconButtonSm,
        destructive && s.iconButtonDanger,
        className,
      )}
      {...rest}
    >
      {children}
    </button>
  );
}
