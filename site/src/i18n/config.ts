/**
 * The locales the page is written in. The value is the URL segment
 * (`/pt-br`); English is served at `/` and has no segment a visitor sees.
 */
export const LOCALES = ["en", "pt-br"] as const;
export type Locale = (typeof LOCALES)[number];

export const DEFAULT_LOCALE: Locale = "en";

export const hasLocale = (value: string): value is Locale => (LOCALES as readonly string[]).includes(value);

/** BCP 47 tag for `<html lang>`, hreflang and Intl. */
export const LANG_TAG: Record<Locale, string> = { en: "en", "pt-br": "pt-BR" };

/** Open Graph wants a territory: `en_US`, `pt_BR`. */
export const OG_LOCALE: Record<Locale, string> = { en: "en_US", "pt-br": "pt_BR" };

/** Each language named in itself, for the switch in the footer. */
export const LOCALE_NAME: Record<Locale, string> = { en: "English", "pt-br": "Português (Brasil)" };

/** Where a locale's page lives. */
export const localePath = (locale: Locale) => (locale === DEFAULT_LOCALE ? "/" : `/${locale}`);

/** Set by the footer's language switch; a choice made by hand beats the country. */
export const LOCALE_COOKIE = "locale";

/** ISO 3166-1 country code to the locale its visitors get when they have not chosen one. */
const BY_COUNTRY: Record<string, Locale> = { BR: "pt-br" };

/** Which locale `/` shows: the visitor's own choice, else their country's, else English. */
export function pickLocale({ choice, country }: { choice?: string | null; country?: string | null }): Locale {
  if (choice && hasLocale(choice)) return choice;
  return (country && BY_COUNTRY[country.toUpperCase()]) || DEFAULT_LOCALE;
}

/** Fills `{name}` slots in a dictionary string. */
export const fill = (template: string, values: Record<string, string | number>) =>
  template.replace(/\{(\w+)\}/g, (_, key: string) => String(values[key]));
