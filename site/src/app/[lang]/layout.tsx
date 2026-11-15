import type { Metadata, Viewport } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import SmoothScroll from "@/components/SmoothScroll";
import { LANG_TAG, LOCALES, localePath, OG_LOCALE } from "@/i18n/config";
import DictProvider from "@/i18n/DictProvider";
import { getDictionary, getLocale } from "@/i18n/dictionaries";
import "../globals.css";

const geistSans = Geist({ variable: "--font-geist-sans", subsets: ["latin"] });
const geistMono = Geist_Mono({ variable: "--font-geist-mono", subsets: ["latin"] });

export function generateStaticParams() {
  return LOCALES.map((lang) => ({ lang }));
}

export async function generateMetadata(): Promise<Metadata> {
  const locale = await getLocale();
  const { title, description } = (await getDictionary()).meta;
  return {
    metadataBase: new URL(
      process.env.VERCEL_PROJECT_PRODUCTION_URL
        ? `https://${process.env.VERCEL_PROJECT_PRODUCTION_URL}`
        : "http://localhost:3000",
    ),
    title,
    description,
    alternates: {
      canonical: localePath(locale),
      languages: {
        ...Object.fromEntries(LOCALES.map((l) => [LANG_TAG[l], localePath(l)])),
        "x-default": "/",
      },
    },
    openGraph: { title, description, type: "website", locale: OG_LOCALE[locale] },
    twitter: { card: "summary_large_image", title, description },
  };
}

export const viewport: Viewport = { themeColor: "#0b0b0c", colorScheme: "dark" };

export default async function RootLayout({ children }: LayoutProps<"/[lang]">) {
  const locale = await getLocale();
  return (
    <html lang={LANG_TAG[locale]} className={`${geistSans.variable} ${geistMono.variable}`}>
      <body>
        <DictProvider dict={await getDictionary()}>
          <SmoothScroll>{children}</SmoothScroll>
        </DictProvider>
      </body>
    </html>
  );
}
