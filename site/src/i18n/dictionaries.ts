import { notFound } from "next/navigation";
import { lang } from "next/root-params";
import { hasLocale, type Locale } from "./config";
import { en, type Dictionary } from "./en";
import { ptBR } from "./pt-br";

export const DICTIONARIES: Record<Locale, Dictionary> = { en, "pt-br": ptBR };

/** The locale of the page being rendered, from the `[lang]` segment. Server Components only. */
export async function getLocale(): Promise<Locale> {
  const locale = await lang();
  if (!hasLocale(locale)) notFound();
  return locale;
}

/** The page's strings. Client Components read the same object with `useDict()`. */
export async function getDictionary(): Promise<Dictionary> {
  return DICTIONARIES[await getLocale()];
}
