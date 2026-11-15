import { NextResponse, type NextRequest } from "next/server";
import { DEFAULT_LOCALE, LOCALE_COOKIE, localePath, pickLocale } from "@/i18n/config";

/**
 * English lives at `/` and is rendered by `app/[lang]` as `/en`. A visitor
 * from Brazil who has not picked a language is sent to `/pt-br`; the footer's
 * language switch sets a cookie that wins over the country from then on.
 */
export function proxy(request: NextRequest) {
  const { pathname } = request.nextUrl;
  const url = request.nextUrl.clone();

  // `/en` is the internal name of `/`; keep one address for the English page.
  if (pathname === `/${DEFAULT_LOCALE}`) {
    url.pathname = "/";
    return NextResponse.redirect(url, 308);
  }

  const locale = pickLocale({
    choice: request.cookies.get(LOCALE_COOKIE)?.value,
    // Set by Vercel on every request; absent in local development.
    country: request.headers.get("x-vercel-ip-country"),
  });

  if (locale === DEFAULT_LOCALE) {
    url.pathname = `/${DEFAULT_LOCALE}`;
    return NextResponse.rewrite(url);
  }
  url.pathname = localePath(locale);
  return NextResponse.redirect(url, 307);
}

export const config = { matcher: ["/", "/en"] };
