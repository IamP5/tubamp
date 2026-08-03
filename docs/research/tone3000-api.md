
# TONE3000 API — Research Notes (for a NAM-model browser/downloader plugin)

Sources used: `github.com/tone-3000/api` README + full source (`src/tone3000-client.ts`, `src/types.ts`, `src/config.ts`, `src/labels.ts`, demo apps), live curl of `POST/GET https://www.tone3000.com/api/v1/*`, and the rendered docs page `https://www.tone3000.com/api` (Introduction/Terms/Authentication/Select/Load Tone/User/Users/Tones/Models/Enums/Deprecated Fields/Types/Example/Design Requirements/Commercial Terms), plus `https://www.tone3000.com/api/terms` (API Terms of Service, effective July 7, 2026).

## 1. Base URL, auth, rate limits

- **Base URL (production):** `https://www.tone3000.com` — all endpoints are under `/api/v1/...`. (NOT `api.tone3000.com` — verified live: `curl -i https://www.tone3000.com/api/v1/tones/search...` returns a real Vercel/Next.js response; the repo's `src/config.ts` also hardcodes this default.) Overridable in the demo repo via `VITE_T3K_API_DOMAIN`.
- **Auth:** OAuth 2.0 with PKCE (S256). No anonymous/API-key-only access to resource endpoints — confirmed live: `GET /api/v1/tones/search` with no Authorization header returns `401 {"error":"Missing or invalid Authorization header"}`.
  - Two key types, generated at TONE3000 → Settings → API Keys:
    - **Publishable key** `t3k_pub_…` — used as `client_id` in OAuth flows; safe client-side.
    - **Secret key** `t3k_cs_…` — server-only Bearer credential for direct API calls without a user-facing OAuth dance (`Authorization: Bearer t3k_cs_…`). Never expose client-side.
  - Access tokens are short-lived; refresh tokens are long-lived. Token response: `{ access_token, refresh_token, expires_in (seconds), token_type: 'bearer', scope }`.
  - Every resource request needs `Authorization: Bearer <access_token>`.
- **Rate limit:** 100 requests/minute by default (explicitly noted that `tones/search` is "heavily rate-limited by default"; TONE3000 recommends the Select OAuth flow over raw search for browsing). Contact `support@tone3000.com` for higher limits / production use of search.
- **Deprecation signaling:** deprecated requests get response headers `Deprecation: true` (RFC 8594), `Link: <…>; rel="deprecation"`, and `X-Tone3000-Deprecations: <comma-separated reasons>`.

## 2. OAuth flows (how you actually get a token)

Building block: PKCE — generate `code_verifier` (random), `code_challenge = base64url(SHA256(code_verifier))`, and a random `state`.

1. `GET https://www.tone3000.com/api/v1/oauth/authorize?client_id=<pub_key>&redirect_uri=...&response_type=code&code_challenge=...&code_challenge_method=S256&state=...&prompt=<...>` — redirects to a TONE3000 login/browse UI, then back to your `redirect_uri` with `?code=...&state=...` (plus `tone_id`/`model_id` depending on flow, or `error=...`).
2. `POST https://www.tone3000.com/api/v1/oauth/token` (`Content-Type: application/x-www-form-urlencoded`), body `grant_type=authorization_code&code=...&code_verifier=...&redirect_uri=...&client_id=...` → `{access_token, refresh_token, expires_in, token_type, scope}`.
3. Refresh: same endpoint, `grant_type=refresh_token&refresh_token=...&client_id=...`. `400 {error:'invalid_grant'}` means refresh token expired — restart flow.

`prompt` values (query param on `/oauth/authorize`):
- *(omitted)* — Standard flow: just connects the user's account; app then calls resource endpoints by ID.
- `select_tone` — **Select flow**: user browses/searches the TONE3000 catalog inside TONE3000's UI and picks a tone; callback includes `tone_id`. Best for a plugin/DAW browse UI — "zero auth UI or tone browser to build."
- `load_tone` (+ required `tone_id`) — **Load Tone flow**: app already knows a `tone_id`; TONE3000 checks access and redirects straight back (or, if private/deleted, lets the user pick a replacement — callback `tone_id` may differ from the one requested).
- `load_model` (+ required `model_id`) — **Load Model flow**: like Load Tone but for a specific model; if inaccessible, redirects back with `error=access_denied` (no replacement offered) — your callback handler must check for this.

Shared optional query params on the authorize URL (apply to `select_tone`/`load_tone`, and to their in-flow "replacement browse" views): `gears` (underscore-separated, e.g. `amp_amp-cab`), `format` (single value, e.g. `nam`), `architecture` (`1`|`2`|`custom` — see §5), `calibrated=true`, `menubar=true` (shows nav bar; also works on the plain sign-in screen), `preview=true` (in-flow audition players, `select_tone` only, needs SharedArrayBuffer), `login_hint=<email>`.

Callback also carries `canceled=true` (menubar close button, no tone/model chosen — `code` may still be present if the user had signed in) and `error=<reason>` (e.g. `access_denied`, `state_mismatch`).

Two other integration patterns documented in the repo (not on the docs page, but in the README as reference architectures): **popup flow** (`startSelectFlowPopup`/`startLoadToneFlowPopup` — same URL, opened in a popup, result relayed back via `postMessage`/`BroadcastChannel('t3k_oauth')`) and a **LAN-relay flow** for headless/embedded devices with no system browser (device shows a QR code pointing at the authorize URL with a LAN `redirect_uri`; user completes auth on their phone; TONE3000 forwards the code to the device's LAN listener via an internal HTTPS bridge; PKCE keeps the code unredeemable by anyone else).

## 3. Resource endpoints (all require `Authorization: Bearer <access_token>`)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/v1/user` | Authenticated user's profile → `User` |
| GET | `/api/v1/users` | Public user directory, sortable → `PaginatedResponse<PublicUser>` |
| GET | `/api/v1/tones/{id}` | Single tone by ID → `Tone` (models NOT embedded) |
| GET | `/api/v1/tones/search` | Search/filter catalog → `PaginatedResponse<Tone>` (heavily rate-limited; prefer the Select flow for browsing) |
| GET | `/api/v1/tones/trending` | Top 10 trending tones (not paginated) → `{data: Tone[]}` |
| GET | `/api/v1/tones/latest` | 10 most recently published tones (not paginated) → `{data: Tone[]}` |
| GET | `/api/v1/tones/created` | Tones created by the authed user → `PaginatedResponse<Tone>` |
| GET | `/api/v1/tones/favorited` | Tones favorited by the authed user → `PaginatedResponse<Tone>` |
| GET | `/api/v1/tones/downloaded` | Tones downloaded by the authed user (dedup'd) → `PaginatedResponse<Tone>` |
| PUT | `/api/v1/tones/{id}/favorite` | Favorite a tone (idempotent, 200) → `Favorite` |
| DELETE | `/api/v1/tones/{id}/favorite` | Unfavorite (idempotent, 204) |
| GET | `/api/v1/tones/{id}/download` | **Partner-only** (403 for other clients) — zip archive URL for ALL of a tone's models |
| GET | `/api/v1/models/{id}` | Single model by ID → `Model` |
| GET | `/api/v1/models?tone_id={id}` | List a tone's models (this is how you actually get download URLs) → `PaginatedResponse<Model>` |

`trending`/`latest` only include public tones that have ≥1 downloadable model, an image, and a description.

### Search Tones — `GET /api/v1/tones/search`

Query params (all optional except none required):
- `query` (string, default `''`)
- `page` (default 1), `page_size` (default 10, **max 25**)
- `sort`: `TonesSort` enum — `best-match` (default when `query` set) | `newest` | `oldest` | `trending` (default when no query) | `downloads-all-time`
- `gears`: `Gear[]`, underscore-separated (e.g. `amp_amp-cab_pedal`)
- `format`: single `Format` value (this is where you filter for NAM — `format=nam`)
- `sizes`: `Size[]`, underscore-separated (e.g. `standard_lite_feather`)
- `tags`: `string[]`, underscore-separated, exact match, OR'd
- `makes`: `string[]`, underscore-separated, exact match, OR'd
- `creators`: `string[]`, **comma**-separated (not underscore — usernames can contain `_`/`-`), exact match on `user.username`, OR'd
- `architecture`: `1`|`2`|`custom` — filters by model architecture; **omitting excludes A2 tones** (see §5)
- `calibrated`: `boolean` — restrict to tones with ≥1 calibrated model

Example request:
```
GET https://www.tone3000.com/api/v1/tones/search?query=fender&page=1&page_size=10&sort=trending&format=nam&gears=amp_amp-cab&architecture=2
Authorization: Bearer <access_token>
```

### List Models — `GET /api/v1/models`

Query params: `tone_id` (required, number), `page` (default 1), `page_size` (default 10, **max 300**), `architecture` (`1`|`2`|`custom`, same default-excludes-A2 behavior).

```
GET https://www.tone3000.com/api/v1/models?tone_id=42&page=1&page_size=50&architecture=2
Authorization: Bearer <access_token>
```

### Get Tone — `GET /api/v1/tones/{id}`

Optional `?architecture=` filters what `models_count` reflects (NAM tones only; ignored for other formats). Regardless of the filter, the tone object **always** returns the per-architecture breakdown fields `a1_models_count`, `a2_models_count`, `custom_models_count`, `irs_count` so you can compute your own visible count client-side.

### Tone-level ZIP download — `GET /api/v1/tones/{id}/download`

**Restricted to approved partners; other API clients get `403`.** Docs explicitly steer normal integrations away from this: "For nearly all integrations, download individual models via the `model_url` field from List Models instead." Response: `{ url, expires_at, filename }` — `url` is a temporary, unauthenticated link valid **1 hour**, request it lazily when the user initiates a download. For `format: 'nam'` tones, **the archive contains A2 files only**; other formats include all models. Returns `400` if the tone has no downloadable models.

## 4. Response shapes (TypeScript, from `types.ts` + docs, cross-checked)

```ts
interface PaginatedResponse<T> {
  data: T[];
  page: number;
  page_size: number;
  total: number;
  total_pages: number;
}

interface EmbeddedUser { id: number; username: string; avatar_url: string | null; url: string; }
interface User extends EmbeddedUser { bio: string | null; links: string[] | null; created_at: string; updated_at: string; }
interface PublicUser {
  id: number; username: string; bio: string | null; links: string[] | null; avatar_url: string | null;
  downloads_count: number; favorites_count: number; models_count: number; tones_count: number; url: string;
}
interface Make { id?: number; name: string; }   // id absent on search-result RPC output
interface Tag  { id?: number; name: string; }   // id absent on search-result RPC output
interface Favorite { id: number; tone_id: number; user_id: string; created_at: string; }

interface Tone {
  id: number;
  user_id: number;
  user: EmbeddedUser;
  created_at: string;   // present on search/created/favorited responses; absent on plain GET /tones/{id} per the repo's types.ts comment
  updated_at: string;
  title: string;
  description: string | null;
  gear: Gear;
  images: string[] | null;
  is_public: boolean | null;
  links: string[] | null;
  format: Format;
  license: License;
  sizes: Size[];
  makes: Make[];
  tags: Tag[];
  models_count: number;
  a1_models_count: number;
  a2_models_count: number;
  custom_models_count: number;
  irs_count: number;
  downloads_count: number;
  favorites_count: number;
  url: string;
}

interface Model {
  id: number;
  created_at: string;
  updated_at: string;
  user_id: number;
  model_url: string;              // download URL — MUST be fetched with Bearer auth, not plain fetch()
  name: string;
  size: Size;
  tone_id: number;
  architecture_version: Architecture | null;  // '1' | '2' | 'custom'; null for non-NAM (e.g. IR)
}
```

## 5. Enums (exact wire values)

```ts
enum Gear { Amp='amp', AmpCab='amp-cab', Pedal='pedal', Outboard='outboard', Cab='cab', Space='space', Experimental='experimental',
  // deprecated inputs, normalized on persist:
  FullRig='full-rig' /* alias → amp-cab */, Ir='ir' /* stripped from gears; use format=ir */ }

enum Format { Nam='nam', Ir='ir', AidaX='aida-x', AaSnapshot='aa-snapshot', Proteus='proteus' }

enum License { T3k='t3k', CcBy='cc-by', CcBySa='cc-by-sa', CcByNc='cc-by-nc', CcByNcSa='cc-by-nc-sa', CcByNd='cc-by-nd', CcByNcNd='cc-by-nc-nd', Cco='cco' }

enum Size { Standard='standard', Lite='lite', Feather='feather', Nano='nano', Custom='custom' }

enum Architecture { A1='1', A2='2', Custom='custom' }   // model_architecture, string-typed

enum TonesSort { BestMatch='best-match', Newest='newest', Oldest='oldest', Trending='trending', DownloadsAllTime='downloads-all-time' }
enum UsersSort { Tones='tones', Downloads='downloads', Favorites='favorites', Models='models' }
```

Deprecated-but-accepted query aliases: `?platform=` → `?format=`; `?gear=` (singular) → `?gears=`; `gears` value `full-rig` → normalizes to `amp-cab` (both are treated as the same logical gear during the transition — querying either returns rows tagged either way); `gears` value `ir` is stripped and, if no explicit `format` was given, `format=ir` is inferred (explicit `format` always wins).

## 6. How A2 models are flagged and filtered — the critical detail

- Each `Model` carries `architecture_version: '1' | '2' | 'custom' | null` (null for non-NAM formats like IR). The UI labels these "A1", "A2", "Custom" respectively (see `labels.ts`/`ModelList.tsx`: `ARCHITECTURE_LABELS = {'1':'A1','2':'A2','custom':'Custom'}`).
- Filtering is via the **`architecture`** query param (accepts `1`, `2`, or `custom`) on:
  - `GET /api/v1/tones/search` (whole-tone filter: only tones with a matching model are returned)
  - `GET /api/v1/models` (returns only matching model rows)
  - `GET /api/v1/tones/{id}` (scopes the `models_count` field only — not what rows come back from other endpoints)
  - The `/oauth/authorize` query string for `select_tone`/`load_tone` prompts (scopes the in-TONE3000 browse UI)
- **⚠️ Important default behavior: omitting `architecture` does NOT return "everything."** The docs state verbatim: *"Omitting returns tones/models with A1 or Custom models (legacy default; excludes A2)."* This is a deliberate legacy-compatibility default "so API users that pre-date A2 don't receive models they can't load." **A plugin that wants A2 models must explicitly pass `architecture=2`** (or omit it and separately request `architecture=custom`/`1` if it wants those too — there's no "all" wildcard value; you'd need three requests, or the tone-level `a1_models_count`/`a2_models_count`/`custom_models_count` fields to decide what to fetch).
- The `Tone` object always exposes `a1_models_count`, `a2_models_count`, `custom_models_count`, `irs_count` regardless of the `architecture` filter used, so a client can compute "does this tone have A2 content" without a second request.
- The bulk `/tones/{id}/download` zip (partner-only) is special-cased: for `format:'nam'` tones it **only ever contains A2 files**, regardless of any architecture param (there isn't one on that endpoint).
- Related site content: TONE3000 publishes a "NAM A2 Guide" (linked in site nav) — worth a quick read if the plugin needs to explain A1 vs A2 to end users, though it wasn't fetched as part of this API research pass.

## 7. Downloading a model file (the actual mechanics)

`model.model_url` is a pre-signed-looking URL but is **not directly fetchable** — it still requires your OAuth Bearer token:

```ts
const res = await fetch(model.model_url, { headers: { Authorization: `Bearer ${accessToken}` } });
if (!res.ok) throw new Error('download failed');
const blob = await res.blob();
// save blob as file (browser: createObjectURL + <a download>; native: write to disk)
```

The reference client (`T3KClient.downloadModel`) additionally: strips the API origin off `model_url` so it can re-prepend base+auth via its own `fetch()` wrapper, derives the file extension from the storage URL's path, and slugifies `model.name` for the saved filename. `.nam` extension = NAM model file; `.wav` = IR file (checked via `getExtension(model.model_url)` in `ModelList.tsx`).

There is a **separate, partner-only** whole-tone ZIP download at `GET /api/v1/tones/{id}/download` (see §3) — normal integrations should not rely on it; per-model `model_url` downloads are the supported path for essentially everyone.

## 8. Licensing / ToS constraints relevant to a downloader plugin

From `https://www.tone3000.com/api/terms` (API Terms of Service, effective 2026-07-07) and the docs page's "Commercial Terms" section:

- **License grant:** limited, non-exclusive, revocable license to build apps/hardware that work *with* TONE3000. Explicitly **forbids**: reselling API access, sublicensing it, white-labeling TONE3000 services, or using the API/catalog to build a competing tone platform.
- **NAM format itself is open/free** (A1 and A2) to implement in any product — these terms govern access to the TONE3000 *API and catalog*, not the NAM file format.
- **Acceptable Use — directly relevant to a "browse and download" plugin:**
  - Respect rate limits, don't circumvent them.
  - **Do not bulk download, scrape, mirror, or cache the catalog.** Only download models when a user explicitly requests them; deliver content per-user, authenticated.
  - **Do not preload catalog content onto devices or bundle it with your product** without written TONE3000 permission.
  - **Do not redistribute/republish/rehost** catalog content outside your integration (no hosting tones on your own servers, no sharing via other platforms, no distributing to users who didn't request them through the API).
  - Do not strip/hide/alter creator names, tone metadata, or license info.
  - Do not collect user data beyond what your integration needs.
- **No white-labeling:** browsing/search/delivery/capture/training must be presented to users as TONE3000, under TONE3000 branding — you cannot present the API/catalog as your own or a third party's service; can't proxy/pool user accounts.
- **Creator content:** tones/models belong to their creators; the API grants **no ownership**, only delivery-to-the-authenticated-requesting-user rights. Each tone carries a creator-chosen `license` (`t3k`, `cc-by`, `cc-by-nc`, etc. — see enum in §5, governed by TONE3000's separate "Tone Sharing Policy" at `/legal` nav). Your app must respect/preserve these licenses including attribution where required, and must not present creator content as your own.
- **Commercial tiers (both in `/api/terms` and the docs page's Commercial Terms section, worded identically):**
  - **Free tier:** allowed if your product is non-commercial (free/open software or hardware, no paid product or upsell — e.g. open source, DIY pedals, research, community tools). **Free tier may ONLY use the OAuth prompt flows (`select_tone`, and what the terms call `get_tone` — matches `load_tone` in the technical docs) and the bounded list endpoints (`favorited`, `downloaded`, `created`, `trending`, `latest`).** No sign-off required, but access can be revoked for guideline violations.
  - **Commercial tier:** required if you charge for the product, or it promotes/accompanies a paid product. Commercial products may use the full API including all CRUD endpoints (this is presumably also the tier that unlocks unrestricted `tones/search`/`models` usage beyond the bounded list endpoints). Requires a commercial agreement and TONE3000 review/sign-off **before public launch or announcement**. Contact `support@tone3000.com`.
- **Branding/attribution requirements** (also detailed as visual wireframes under "Design Requirements" on the docs page): show "Powered by TONE3000" per brand guidelines, link to `https://www.tone3000.com`, never imply endorsement/certification/official partnership without written approval. Design Requirements mandate: an entry point in the signal-chain UI, a partnership splash before first sign-in, TONE3000-branded auth (magic-link email), and tone list/detail views that always show tone image, gear title, gear type, format, creator (username+avatar), and TONE3000 branding, plus a persistent "Browse TONE3000" path and Favorites/Created/Downloaded tabs.
- **Paid/training features:** TONE3000 may add paid tones later (extra terms may apply); there's a pilot cloud-training endpoint (users train NAM captures via TONE3000's infra inside your product) with its own commercial/compute-based terms — not needed for a browse/download plugin but worth knowing it exists.
- **Practical implication for a NAM-browsing plugin:** a typical free/open-source guitar plugin should use the **Select flow** (`prompt=select_tone`, ideally with `preview=true` for in-flow audition and `architecture=2`/`format=nam` to scope to A2 NAM captures) rather than building a custom search UI against `/tones/search`, both because that endpoint is heavily rate-limited and because the free tier's endpoint allowlist doesn't clearly include unrestricted search. If the plugin is a commercial/paid product, a signed commercial agreement with TONE3000 is required before public launch, unlocking full CRUD/search access.

## 9. GitHub org survey (`github.com/orgs/tone-3000`)

| Repo | Notes |
|---|---|
| `tone-3000/api` | **This SDK/demo repo.** MIT-licensed. Contains `src/tone3000-client.ts` (zero-dep OAuth+API helper — the best reference implementation), `src/types.ts`, demo apps (`SelectApp`, `LoadToneApp`, `LoadModelApp`, `FullApiApp`, `LanFlowApp`), live demo at `t3k-api-demo.vercel.app`. |
| `neural-amp-modeler-wasm` | Fork — NAM DSP compiled to WASM/Web (used by the demo repo's `ModelList.tsx` for in-browser audition via `T3kSlimPlayer`). |
| `nam-binary-loader` | Binary file format + loader for NAM `.nam` files — useful if the plugin needs to parse `.nam` files itself rather than just download them. |
| `nam-pedal` | Demo of NAM running on a Daisy Seed embedded board (hardware pedal). |
| `nam-inference-benchmarks` | Microbenchmarks of NAM (WaveNet) inference optimizations. |
| `NeuralAmpModelerPlugin` | Fork — the original NeuralAmpModeler DAW plugin (Iplug2-based); a reference architecture if this project is itself building a NAM plugin. |
| `neural-amp-modeler` | Fork — the original Python NAM training/inference library. |
| `electron-select` | "TONE3000 Select flow integration example in Electron" — directly relevant if the plugin has an Electron-based UI/companion app. |
| `t3k-mushra`, `a2-mushra-data` | Web-based MUSHRA listening-test tooling and A1-vs-A2 test data — background on how TONE3000 validated the A2 architecture. |
| `slimmable-demo`, `tone-manager` | Smaller/less-documented repos (no description); not investigated in depth — low priority for this task. |

No standalone OpenAPI/Swagger spec file was found in the `api` repo (contents: `.gitignore`, `.npmrc`, `LICENSE`, `README.md`, `env.example`, `eslint.config.js`, `index.html`, `package.json`/`package-lock.json`, `tsconfig*.json`, `vercel.json`, `vite.config.ts`, `vite-plugin-lan-bridge.ts`, and the `src/`/`public/` dirs listed above) — the docs page at `tone3000.com/api` is the closest thing to a formal spec, and its Types/Enums sections were fully transcribed above.

## 10. Verified live request/response (ground truth, not docs)

```
$ curl -i "https://www.tone3000.com/api/v1/tones/search?query=fender&page=1&page_size=5"
HTTP/2 401
content-type: application/json
{"error":"Missing or invalid Authorization header"}
```
Confirms: base URL is `www.tone3000.com` (not a separate `api.` subdomain), the search endpoint is real and live, and it hard-requires a Bearer token even for read-only search — there is no anonymous/public tier of the API.
