import { getLatestRelease } from "@/lib/release";
import { isArch, RELEASES_URL } from "@/lib/release-schema";

export async function GET(_request: Request, ctx: RouteContext<"/download/[arch]">) {
  const { arch } = await ctx.params;
  if (!isArch(arch)) return new Response("Unknown architecture", { status: 404 });
  const release = await getLatestRelease();
  return Response.redirect(release ? release.downloads[arch].url : RELEASES_URL, 302);
}
