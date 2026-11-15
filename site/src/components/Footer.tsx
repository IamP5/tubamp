import { SITE } from "@/data/site";
import { LANG_TAG, LOCALE_NAME, LOCALES, localePath } from "@/i18n/config";
import { getDictionary, getLocale } from "@/i18n/dictionaries";
import LocaleLink from "./LocaleLink";
import { RELEASES_URL } from "@/lib/release-schema";

const link =
  "text-bone-2 underline decoration-line-strong underline-offset-4 transition-colors duration-150 ease-ui hover:text-bone hover:decoration-bone";

export default async function Footer() {
  const locale = await getLocale();
  const t = await getDictionary();
  return (
    <footer className="gutter border-t border-line bg-ink py-10 text-sm">
      <div className="mx-auto flex max-w-[110rem] flex-col gap-8 md:flex-row md:items-start md:justify-between">
        <div className="flex flex-col gap-3">
          <p className="text-xl font-semibold tracking-[-0.04em] text-bone">{SITE.name}</p>
          <p className="max-w-[24rem] text-bone-3">{t.hero.line}</p>
        </div>
        <ul className="flex flex-wrap gap-x-6 gap-y-2">
          <li>
            <a href={SITE.repoUrl} className={link}>
              {t.footer.source}
            </a>
          </li>
          <li>
            <a href={RELEASES_URL} className={link}>
              {t.footer.releases}
            </a>
          </li>
          {LOCALES.filter((l) => l !== locale).map((l) => (
            <li key={l}>
              <LocaleLink locale={l} href={localePath(l)} lang={LANG_TAG[l]} aria-label={`${t.footer.language}: ${LOCALE_NAME[l]}`} className={link}>
                {LOCALE_NAME[l]}
              </LocaleLink>
            </li>
          ))}
        </ul>
        <ul className="flex max-w-[26rem] flex-wrap gap-x-5 gap-y-1.5 text-bone-3 md:justify-end">
          {t.footer.credits.map((credit) => (
            <li key={credit}>{credit}</li>
          ))}
        </ul>
      </div>
    </footer>
  );
}
