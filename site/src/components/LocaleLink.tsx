"use client";

import type { ComponentProps } from "react";
import { LOCALE_COOKIE, type Locale } from "@/i18n/config";

const YEAR = 60 * 60 * 24 * 365;

/**
 * A link to the page in another language. It records the choice, so `/` keeps
 * showing that language whatever country the visitor is in.
 */
export default function LocaleLink({ locale, ...props }: ComponentProps<"a"> & { locale: Locale }) {
  return (
    <a
      {...props}
      hrefLang={props.lang}
      onClick={() => {
        document.cookie = `${LOCALE_COOKIE}=${locale}; path=/; max-age=${YEAR}; samesite=lax`;
      }}
    />
  );
}
