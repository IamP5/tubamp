#include "Tone3000Client.h"

#include "ModelLibrary.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------
// TONE3000 OAuth 2.0 + PKCE client. See docs/research/tone3000-api.md for the
// exact wire contract this implements.
//
// juce_cryptography is not linked into this target (see CMakeLists.txt), so
// PKCE's SHA-256 step uses a small self-contained implementation below rather
// than juce::SHA256, to avoid adding a new module dependency outside this
// agent's remit. Functionally identical (FIPS 180-4 SHA-256).
// ---------------------------------------------------------------------------

namespace tubamp
{
namespace
{
//==============================================================================
// Minimal SHA-256 (public-domain style, single translation unit, no deps).
struct Sha256
{
    static void hash (const void* data, size_t len, uint8_t out[32])
    {
        uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

        std::vector<uint8_t> msg (static_cast<const uint8_t*> (data), static_cast<const uint8_t*> (data) + len);
        const uint64_t bitLen = static_cast<uint64_t> (len) * 8;

        msg.push_back (0x80);
        while (msg.size() % 64 != 56)
            msg.push_back (0);

        for (int i = 7; i >= 0; --i)
            msg.push_back (static_cast<uint8_t> ((bitLen >> (i * 8)) & 0xff));

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
            processChunk (&msg[chunk], h);

        for (int i = 0; i < 8; ++i)
        {
            out[i * 4 + 0] = static_cast<uint8_t> ((h[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<uint8_t> ((h[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t> ((h[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t> (h[i] & 0xff);
        }
    }

private:
    static uint32_t rotr (uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    static void processChunk (const uint8_t* chunk, uint32_t h[8])
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t> (chunk[i * 4]) << 24) | (static_cast<uint32_t> (chunk[i * 4 + 1]) << 16)
                 | (static_cast<uint32_t> (chunk[i * 4 + 2]) << 8) | static_cast<uint32_t> (chunk[i * 4 + 3]);

        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i)
        {
            uint32_t s1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};

//==============================================================================
juce::String toBase64Url (const void* data, size_t size)
{
    auto s = juce::Base64::toBase64 (data, size);
    s = s.replaceCharacter ('+', '-').replaceCharacter ('/', '_');
    while (s.endsWithChar ('='))
        s = s.dropLastCharacters (1);
    return s;
}

struct Pkce
{
    juce::String verifier, challenge, state;
};

Pkce generatePkce()
{
    Pkce pkce;
    auto& rng = juce::Random::getSystemRandom();

    uint8_t verifierBytes[64];
    rng.fillBitsRandomly (verifierBytes, sizeof (verifierBytes));
    pkce.verifier = toBase64Url (verifierBytes, sizeof (verifierBytes));

    auto verifierUtf8 = pkce.verifier.toUTF8();
    uint8_t digest[32];
    Sha256::hash (verifierUtf8.getAddress(), std::strlen (verifierUtf8.getAddress()), digest);
    pkce.challenge = toBase64Url (digest, sizeof (digest));

    uint8_t stateBytes[16];
    rng.fillBitsRandomly (stateBytes, sizeof (stateBytes));
    pkce.state = toBase64Url (stateBytes, sizeof (stateBytes));

    return pkce;
}

juce::String slugify (const juce::String& name)
{
    const juce::String lower = name.trim().toLowerCase();
    juce::String out;
    bool lastWasDash = false;

    for (auto c : lower)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c))
        {
            out += c;
            lastWasDash = false;
        }
        else if (! lastWasDash && out.isNotEmpty())
        {
            out += '-';
            lastWasDash = true;
        }
    }

    while (out.endsWithChar ('-'))
        out = out.dropLastCharacters (1);

    return out.isEmpty() ? juce::String ("model") : out;
}

juce::File declash (const juce::File& target)
{
    if (! target.exists())
        return target;

    const auto dir = target.getParentDirectory();
    const auto base = target.getFileNameWithoutExtension();
    const auto ext = target.getFileExtension();

    for (int n = 2; n < 1000; ++n)
    {
        auto candidate = dir.getChildFile (base + " " + juce::String (n) + ext);
        if (! candidate.exists())
            return candidate;
    }

    return dir.getChildFile (base + " " + juce::String (juce::Time::getCurrentTime().toMilliseconds()) + ext);
}

juce::String describeHttpError (const juce::var& json, int statusCode)
{
    if (json.getDynamicObject() != nullptr)
    {
        const auto err = json.getProperty ("error", juce::var()).toString();
        const auto desc = json.getProperty ("error_description", juce::var()).toString();

        if (desc.isNotEmpty())
            return err.isNotEmpty() ? (err + ": " + desc) : desc;
        if (err.isNotEmpty())
            return err;
    }

    return "HTTP " + juce::String (statusCode);
}

constexpr int kLoopbackPortFirst = 53682;
constexpr int kLoopbackPortLast = 53690;
constexpr const char* kApiBase = "https://www.tone3000.com/api/v1";

constexpr int kBrowsePageSize = 25;    // API max for /tones/search
constexpr int kModelPageSize = 300;    // API max for /models: one request per tone
constexpr int kMaxFavoritePages = 40;  // safety cap on paging the favorited list
constexpr double kBrowseCacheMinutes = 4.0;

/** Search sort wire value. best-match is the API's own default for a text
    query, so a "trending" pick with a query in flight becomes relevance. */
juce::String searchSort (const Tone3000Client::BrowseRequest& req)
{
    if (req.sort == "newest")
        return "newest";
    if (req.sort == "downloads")
        return "downloads-all-time";

    return req.query.isNotEmpty() ? "best-match" : "trending";
}

juce::StringArray namesOf (const juce::var& array)
{
    juce::StringArray out;
    if (auto* arr = array.getArray())
        for (auto& item : *arr)
            out.add (item.getProperty ("name", juce::var()).toString());
    return out;
}

juce::StringArray stringsOf (const juce::var& array)
{
    juce::StringArray out;
    if (auto* arr = array.getArray())
        for (auto& item : *arr)
            out.add (item.toString());
    return out;
}

Tone3000Client::Tone parseTone (const juce::var& item)
{
    Tone3000Client::Tone t;
    t.id = static_cast<juce::int64> (item.getProperty ("id", juce::var (0)));
    t.title = item.getProperty ("title", juce::var()).toString();
    t.description = item.getProperty ("description", juce::var()).toString();
    t.gear = item.getProperty ("gear", juce::var()).toString();
    t.format = item.getProperty ("format", juce::var()).toString();
    t.createdAt = item.getProperty ("created_at", juce::var()).toString();

    if (auto* images = item.getProperty ("images", juce::var()).getArray())
        if (! images->isEmpty())
            t.imageUrl = images->getReference (0).toString();

    const auto user = item.getProperty ("user", juce::var());
    t.creator.username = user.getProperty ("username", juce::var()).toString();
    t.creator.avatarUrl = user.getProperty ("avatar_url", juce::var()).toString();

    t.downloadsCount = static_cast<juce::int64> (item.getProperty ("downloads_count", juce::var (0)));
    t.favoritesCount = static_cast<juce::int64> (item.getProperty ("favorites_count", juce::var (0)));
    t.makes = namesOf (item.getProperty ("makes", juce::var()));
    t.tags = namesOf (item.getProperty ("tags", juce::var()));
    t.sizes = stringsOf (item.getProperty ("sizes", juce::var()));

    // models_count follows whatever architecture filter the request carried;
    // the per-architecture breakdown is always present, so count from it.
    t.modelsCount = static_cast<int> (item.getProperty (t.format == "ir" ? "irs_count" : "a2_models_count", juce::var (0)));

    return t;
}
} // namespace

//==============================================================================
struct Tone3000Client::Impl
{
    /** Compile-time default client_id (CMake TUBAMP_T3K_CLIENT_ID). Publishable
        keys are safe to embed — they only identify the app; auth is per-user
        via PKCE. A key stored from Settings overrides it. */
    static juce::String builtinClientId()
    {
       #ifdef TUBAMP_T3K_CLIENT_ID
        return TUBAMP_T3K_CLIENT_ID;
       #else
        return {};
       #endif
    }

    Impl()
    {
        loadConfig();
        if (clientId.isEmpty())
            clientId = builtinClientId();
    }

    ~Impl();

    static juce::File getConfigFile() { return ModelLibrary::getRootDir().getChildFile ("tone3000.json"); }

    void loadConfig()
    {
        const auto file = getConfigFile();
        if (! file.existsAsFile())
            return;

        auto parsed = juce::JSON::parse (file);
        if (parsed.getDynamicObject() == nullptr)
            return;

        clientId = parsed.getProperty ("clientId", juce::var()).toString();
        refreshToken = parsed.getProperty ("refreshToken", juce::var()).toString();
        username = parsed.getProperty ("username", juce::var()).toString();
    }

    void saveConfig() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("clientId", clientId);
        obj->setProperty ("refreshToken", refreshToken);
        obj->setProperty ("username", username);

        ModelLibrary::getRootDir().createDirectory();
        getConfigFile().replaceWithText (juce::JSON::toString (juce::var (obj)));
    }

    /** Blocking; call from a background thread only. Refreshes the access
        token if it is missing or close to expiry. */
    bool ensureValidAccessToken (juce::String& errorOut)
    {
        {
            const juce::ScopedLock sl (lock);
            if (accessToken.isNotEmpty() && juce::Time::getCurrentTime() < accessTokenExpiry)
                return true;
            if (refreshToken.isEmpty())
            {
                errorOut = "Not signed in to TONE3000.";
                return false;
            }
        }

        juce::String currentClientId, currentRefreshToken;
        {
            const juce::ScopedLock sl (lock);
            currentClientId = clientId;
            currentRefreshToken = refreshToken;
        }

        juce::URL url (juce::String (kApiBase) + "/oauth/token");
        url = url.withParameter ("grant_type", "refresh_token")
                 .withParameter ("refresh_token", currentRefreshToken)
                 .withParameter ("client_id", currentClientId);

        int statusCode = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                          .withConnectionTimeoutMs (15000)
                          .withStatusCode (&statusCode));

        if (stream == nullptr)
        {
            errorOut = "Could not reach TONE3000 to refresh the session.";
            return false;
        }

        auto json = juce::JSON::parse (stream->readEntireStreamAsString());

        if (statusCode != 200)
        {
            errorOut = "TONE3000 session expired; please sign in again (" + describeHttpError (json, statusCode) + ").";
            const juce::ScopedLock sl (lock);
            accessToken.clear();
            refreshToken.clear();
            saveConfig();
            return false;
        }

        const juce::ScopedLock sl (lock);
        accessToken = json.getProperty ("access_token", juce::var()).toString();
        const auto newRefresh = json.getProperty ("refresh_token", juce::var()).toString();
        if (newRefresh.isNotEmpty())
            refreshToken = newRefresh;
        const double expiresIn = static_cast<double> (json.getProperty ("expires_in", juce::var (3600)));
        accessTokenExpiry = juce::Time::getCurrentTime() + juce::RelativeTime::seconds (juce::jmax (30.0, expiresIn - 30.0));
        saveConfig();
        return true;
    }

    /** Blocking; call from a background thread only. Authenticated API call
        returning the parsed body; one forced refresh + retry when the access
        token is rejected. Accepts the 204 the favorite endpoints answer with. */
    bool apiRequest (const juce::URL& url, const juce::String& verb, juce::var& jsonOut, juce::String& errorOut)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (! ensureValidAccessToken (errorOut))
                return false;

            juce::String token;
            { const juce::ScopedLock sl (lock); token = accessToken; }

            int statusCode = 0;
            auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                              .withExtraHeaders ("Authorization: Bearer " + token + "\r\n")
                              .withHttpRequestCmd (verb)
                              .withConnectionTimeoutMs (15000)
                              .withStatusCode (&statusCode));

            if (stream == nullptr)
            {
                errorOut = "Could not reach TONE3000.";
                return false;
            }

            jsonOut = juce::JSON::parse (stream->readEntireStreamAsString());

            if (statusCode == 200 || statusCode == 201 || statusCode == 204)
                return true;

            if (statusCode == 401 && attempt == 0)
            {
                // Token rejected before its stated expiry: drop it so the next
                // pass refreshes rather than replaying the same dead token.
                const juce::ScopedLock sl (lock);
                accessToken.clear();
                accessTokenExpiry = {};
                continue;
            }

            errorOut = statusCode == 401
                         ? juce::String ("TONE3000 session expired; please sign in again.")
                         : "TONE3000 request failed (" + describeHttpError (jsonOut, statusCode) + ").";
            return false;
        }

        return false;
    }

    /** Blocking; call from a background thread only. Empty `architecture` means
        no filter (ir tones); "2" is mandatory for nam - omitting it excludes A2. */
    juce::Array<Model> fetchModels (juce::int64 toneId, const juce::String& architecture,
                                    juce::String* errorOut = nullptr)
    {
        juce::URL url (juce::String (kApiBase) + "/models");
        url = url.withParameter ("tone_id", juce::String (toneId))
                 .withParameter ("page_size", juce::String (kModelPageSize));

        if (architecture.isNotEmpty())
            url = url.withParameter ("architecture", architecture);

        juce::Array<Model> result;
        juce::var json;
        juce::String error;

        if (! apiRequest (url, "GET", json, error))
        {
            if (errorOut != nullptr)
                *errorOut = error;
            return result;
        }

        if (auto* arr = json.getProperty ("data", juce::var()).getArray())
        {
            for (auto& item : *arr)
            {
                Model m;
                m.id = static_cast<juce::int64> (item.getProperty ("id", juce::var (0)));
                m.name = item.getProperty ("name", juce::var()).toString();
                m.modelUrl = item.getProperty ("model_url", juce::var()).toString();
                m.size = item.getProperty ("size", juce::var()).toString();
                m.architecture = item.getProperty ("architecture_version", juce::var()).toString();
                m.kind = juce::URL (m.modelUrl).getFileName().endsWithIgnoreCase (".wav") ? "wav" : "nam";
                result.add (m);
            }
        }

        return result;
    }

    /** Blocking; call from a background thread only. */
    bool fetchUsername (juce::String& errorOut)
    {
        juce::var json;
        if (! apiRequest (juce::URL (juce::String (kApiBase) + "/user"), "GET", json, errorOut))
            return false;

        const juce::ScopedLock sl (lock);
        username = json.getProperty ("username", juce::var()).toString();
        saveConfig();
        return true;
    }

    /** Blocking; call from a background thread only. Pages the whole favorited
        list (bounded, unlike search) and rebuilds the cached favorited-id set
        from it; `tonesOut` is the same data, for the favorites shelf. */
    bool fetchFavorited (juce::Array<Tone>& tonesOut, juce::String& errorOut)
    {
        std::set<juce::int64> ids;

        for (int page = 1; page <= kMaxFavoritePages; ++page)
        {
            const auto url = juce::URL (juce::String (kApiBase) + "/tones/favorited")
                                 .withParameter ("page", juce::String (page))
                                 .withParameter ("page_size", juce::String (kBrowsePageSize));

            juce::var json;
            if (! apiRequest (url, "GET", json, errorOut))
                return false;

            if (auto* arr = json.getProperty ("data", juce::var()).getArray())
            {
                for (auto& item : *arr)
                {
                    auto tone = parseTone (item);
                    ids.insert (tone.id);
                    tonesOut.add (tone);
                }
            }

            if (page >= static_cast<int> (json.getProperty ("total_pages", juce::var (1))))
                break;
        }

        const juce::ScopedLock sl (lock);
        favoritedIds = std::move (ids);
        return true;
    }

    /** Blocking; call from a background thread only. */
    bool browseSearch (const BrowseRequest& req, TonePage& pageOut, juce::String& errorOut)
    {
        auto url = juce::URL (juce::String (kApiBase) + "/tones/search")
                       .withParameter ("page", juce::String (juce::jmax (1, req.page)))
                       .withParameter ("page_size", juce::String (kBrowsePageSize))
                       .withParameter ("sort", searchSort (req));

        if (req.query.isNotEmpty())
            url = url.withParameter ("query", req.query);

        if (req.kind == "irs")
        {
            url = url.withParameter ("format", "ir");
        }
        else
        {
            // architecture=2 is not optional here: omitting it excludes every A2
            // tone from the results (tone3000-api.md §6).
            url = url.withParameter ("format", "nam").withParameter ("architecture", "2");

            if (req.gear.isNotEmpty())
                url = url.withParameter ("gears", req.gear);
        }

        juce::var json;
        if (! apiRequest (url, "GET", json, errorOut))
            return false;

        if (auto* arr = json.getProperty ("data", juce::var()).getArray())
            for (auto& item : *arr)
                pageOut.tones.add (parseTone (item));

        pageOut.page = static_cast<int> (json.getProperty ("page", juce::var (1)));
        pageOut.totalPages = juce::jmax (1, static_cast<int> (json.getProperty ("total_pages", juce::var (1))));
        pageOut.total = static_cast<juce::int64> (json.getProperty ("total", juce::var (0)));
        return true;
    }

    /** Blocking; call from a background thread only. The favorited endpoint has
        no filter/sort/page-size params of its own, so all of that is client-side. */
    bool browseFavorites (const BrowseRequest& req, TonePage& pageOut, juce::String& errorOut)
    {
        juce::Array<Tone> all;
        if (! fetchFavorited (all, errorOut))
            return false;

        const juce::String format = req.kind == "irs" ? "ir" : "nam";
        juce::Array<Tone> matching;

        for (auto& t : all)
        {
            if (t.format != format)
                continue;
            if (req.gear.isNotEmpty() && t.gear != req.gear)
                continue;
            if (req.query.isNotEmpty() && ! (t.title.containsIgnoreCase (req.query)
                                             || t.creator.username.containsIgnoreCase (req.query)))
                continue;

            matching.add (t);
        }

        // No trending signal exists client-side, so "trending" keeps the
        // endpoint's own order (most recently favorited first).
        if (req.sort == "newest")
            std::stable_sort (matching.begin(), matching.end(),
                              [] (const Tone& a, const Tone& b) { return a.createdAt > b.createdAt; });
        else if (req.sort == "downloads")
            std::stable_sort (matching.begin(), matching.end(),
                              [] (const Tone& a, const Tone& b) { return a.downloadsCount > b.downloadsCount; });

        const int total = matching.size();
        const int totalPages = juce::jmax (1, (total + kBrowsePageSize - 1) / kBrowsePageSize);
        const int page = juce::jlimit (1, totalPages, req.page);
        const int first = (page - 1) * kBrowsePageSize;

        for (int i = first; i < juce::jmin (total, first + kBrowsePageSize); ++i)
            pageOut.tones.add (matching.getReference (i));

        pageOut.page = page;
        pageOut.totalPages = totalPages;
        pageOut.total = total;
        return true;
    }

    static juce::String cacheKey (const BrowseRequest& req)
    {
        return req.kind + "|" + req.shelf + "|" + req.sort + "|" + req.gear + "|"
             + juce::String (req.page) + "|" + req.query;
    }

    /** Blocking; call from a background thread only. Short-lived in-memory cache
        only: the ToS forbids persisting the catalog, and search is heavily
        rate-limited, so a page the user pages back to must not re-hit the API. */
    bool browseBlocking (const BrowseRequest& req, TonePage& pageOut, juce::String& errorOut)
    {
        const auto key = cacheKey (req);

        {
            const juce::ScopedLock sl (lock);
            const auto entry = browseCache.find (key);
            if (entry != browseCache.end() && juce::Time::getCurrentTime() < entry->second.expiry)
            {
                pageOut = entry->second.page;
                return true;
            }
        }

        const bool ok = req.shelf == "favorites" ? browseFavorites (req, pageOut, errorOut)
                                                 : browseSearch (req, pageOut, errorOut);
        if (! ok)
            return false;

        const juce::ScopedLock sl (lock);

        for (auto& t : pageOut.tones)
            t.favorited = favoritedIds.count (t.id) > 0;

        const auto now = juce::Time::getCurrentTime();
        for (auto it = browseCache.begin(); it != browseCache.end();)
            it = it->second.expiry < now ? browseCache.erase (it) : std::next (it);

        browseCache[key] = { now + juce::RelativeTime::minutes (kBrowseCacheMinutes), pageOut,
                             req.shelf == "favorites" };
        return true;
    }

    /** Caller holds `lock`. Favoriting changes what the favorites shelf contains,
        which no TTL can predict. */
    void dropFavoritesCacheEntries()
    {
        for (auto it = browseCache.begin(); it != browseCache.end();)
            it = it->second.favoritesShelf ? browseCache.erase (it) : std::next (it);
    }

    // Forward-declared here (defined below, out-of-line) rather than at namespace
    // scope: they need access to Impl's private token/lock state, and nesting
    // them inside Impl (a private member of Tone3000Client) keeps that access
    // implicit instead of requiring a public accessor surface.
    class AuthFlowThread;
    class DownloadThread;
    class TaskThread;

    void finishAuth (AuthFlowThread* t);
    void finishDownload (DownloadThread* t);
    void finishTask (TaskThread* t);

    /** Message-thread only: runs one blocking job on a tracked background thread. */
    void runTask (const char* threadName, std::function<void()> body);

    struct CacheEntry
    {
        juce::Time expiry;
        TonePage page;
        bool favoritesShelf = false;
    };

    juce::CriticalSection lock;
    juce::String clientId, refreshToken, accessToken, username;
    juce::Time accessTokenExpiry;
    std::set<juce::int64> favoritedIds;
    std::map<juce::String, CacheEntry> browseCache;

    std::unique_ptr<AuthFlowThread> authThread;
    std::vector<std::unique_ptr<DownloadThread>> downloadThreads;
    std::vector<std::unique_ptr<TaskThread>> taskThreads;

    // Guards the deferred cleanup lambdas in finishAuth/finishDownload/finishTask below,
    // which capture `this` and run later on the message thread: if Impl is
    // destroyed before a queued cleanup dispatches, the flag (kept alive by the
    // shared_ptr copy captured in the lambda) tells it to no-op instead of
    // touching freed memory.
    std::shared_ptr<bool> alive { std::make_shared<bool> (true) };
};

//==============================================================================
/** Runs a browser auth flow end-to-end: PKCE, loopback listener, system-browser
    launch, code exchange, then either the picked tone's models (Select) or the
    profile + favorites priming (Sign in). One-shot; deletes itself (via the
    owning Impl, on the message thread) once done. */
class Tone3000Client::Impl::AuthFlowThread : public juce::Thread
{
public:
    enum class Mode { signIn, selectTone };

    /** Sign-in: plain /oauth/authorize, no tone is picked. */
    AuthFlowThread (Tone3000Client::Impl& implIn,
                    std::function<void()> onSignedInIn,
                    std::function<void (juce::String)> onErrorIn)
        : juce::Thread ("Tone3000 Sign-in"), mode (Mode::signIn), impl (implIn),
          onSignedIn (std::move (onSignedInIn)), onError (std::move (onErrorIn))
    {
    }

    /** Select flow: the authorize URL carries prompt=select_tone and the catalog
        scoping params, and the callback carries the chosen tone_id. */
    AuthFlowThread (Tone3000Client::Impl& implIn,
                    std::function<void (Tone3000Client::ToneModels)> onSelectedIn,
                    std::function<void (juce::String)> onErrorIn)
        : juce::Thread ("Tone3000 Select"), mode (Mode::selectTone), impl (implIn),
          onSelected (std::move (onSelectedIn)), onError (std::move (onErrorIn))
    {
    }

    /** Message-thread only: unblocks a pending accept() and asks the thread to stop. */
    void cancel()
    {
        canceled = true;
        signalThreadShouldExit();
        listener.close();
    }

    void run() override
    {
        const auto pkce = generatePkce();

        int port = 0;
        for (int p = kLoopbackPortFirst; p <= kLoopbackPortLast; ++p)
        {
            if (listener.createListener (p, "127.0.0.1"))
            {
                port = p;
                break;
            }
        }

        if (port == 0)
        {
            fail ("Could not open a local port (53682-53690) for the TONE3000 sign-in callback. "
                  "Close any other app using those ports and try again.");
            return;
        }

        const juce::String redirectUri = "http://127.0.0.1:" + juce::String (port) + "/callback";

        juce::String clientId;
        { const juce::ScopedLock sl (impl.lock); clientId = impl.clientId; }

        auto authorizeUrl = juce::URL (juce::String (kApiBase) + "/oauth/authorize")
                                .withParameter ("client_id", clientId)
                                .withParameter ("redirect_uri", redirectUri)
                                .withParameter ("response_type", "code")
                                .withParameter ("code_challenge", pkce.challenge)
                                .withParameter ("code_challenge_method", "S256")
                                .withParameter ("state", pkce.state);

        if (mode == Mode::selectTone)
            authorizeUrl = authorizeUrl.withParameter ("prompt", "select_tone")
                                       .withParameter ("format", "nam")
                                       .withParameter ("architecture", "2")
                                       .withParameter ("preview", "true");

        if (! authorizeUrl.launchInDefaultBrowser())
        {
            fail ("Could not open the system browser for TONE3000 sign-in.");
            return;
        }

        std::unique_ptr<juce::StreamingSocket> connection (listener.waitForNextConnection());
        if (connection == nullptr)
        {
            if (! canceled)
                fail ("TONE3000 sign-in was interrupted before completing.");
            return;
        }

        char buffer[8192];
        connection->waitUntilReady (true, 5000);
        const int bytesRead = connection->read (buffer, static_cast<int> (sizeof (buffer)) - 1, false);

        if (bytesRead <= 0)
        {
            fail ("No response was received from the browser.");
            return;
        }

        buffer[bytesRead] = 0;
        const auto request = juce::String::fromUTF8 (buffer, bytesRead);
        const auto requestLine = request.upToFirstOccurrenceOf ("\r\n", false, false);
        const auto tokens = juce::StringArray::fromTokens (requestLine, " ", "");
        const auto target = tokens.size() > 1 ? tokens[1] : juce::String();
        const juce::URL callback ("http://127.0.0.1" + target);

        respondToBrowser (*connection);
        connection->close();
        listener.close();

        auto param = [&callback] (const juce::String& name) -> juce::String
        {
            const auto idx = callback.getParameterNames().indexOf (name);
            return idx >= 0 ? callback.getParameterValues()[idx] : juce::String();
        };

        if (param ("canceled") == "true")
        {
            fail (mode == Mode::selectTone ? "Tone selection was canceled." : "TONE3000 sign-in was canceled.");
            return;
        }

        const auto errorParam = param ("error");
        if (errorParam.isNotEmpty())
        {
            fail ("TONE3000 sign-in failed: " + errorParam);
            return;
        }

        const auto code = param ("code");
        const auto state = param ("state");
        const auto toneIdStr = param ("tone_id");

        if (code.isEmpty())
        {
            fail ("No authorization code was returned by TONE3000.");
            return;
        }

        if (state != pkce.state)
        {
            fail ("TONE3000 sign-in failed a security check (state mismatch). Please try again.");
            return;
        }

        if (mode == Mode::selectTone && toneIdStr.isEmpty())
        {
            fail ("No tone was selected.");
            return;
        }

        juce::String tokenError;
        if (! exchangeCode (code, pkce.verifier, redirectUri, clientId, tokenError))
        {
            fail (tokenError);
            return;
        }

        if (mode == Mode::signIn)
        {
            juce::String error;
            if (! impl.fetchUsername (error))
            {
                fail (error);
                return;
            }

            // Primes the favorited-id set browse() stamps its results with; a
            // failure here is not worth failing the sign-in over.
            juce::Array<Tone3000Client::Tone> favorited;
            impl.fetchFavorited (favorited, error);

            auto signedIn = onSignedIn;
            juce::MessageManager::callAsync ([signedIn] { if (signedIn) signedIn(); });
            impl.finishAuth (this);
            return;
        }

        const juce::int64 toneId = toneIdStr.getLargeIntValue();
        auto models = impl.fetchModels (toneId, "2");
        if (models.isEmpty())
            models = impl.fetchModels (toneId, "1"); // tone has no A2 files - fall back

        Tone3000Client::ToneModels result;
        result.toneId = toneId;
        result.models = models;

        auto cb = onSelected;
        juce::MessageManager::callAsync ([cb, result] { if (cb) cb (result); });
        impl.finishAuth (this);
    }

private:
    static void respondToBrowser (juce::StreamingSocket& connection)
    {
        static const char* body =
            "<!doctype html><html><head><title>tubamp</title></head>"
            "<body style=\"font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
            "text-align:center;padding-top:3em;color:#333\">"
            "<p>You can return to tubamp.</p></body></html>";

        const auto bodyLen = static_cast<int> (std::strlen (body));

        juce::String response;
        response << "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: " << bodyLen << "\r\n"
                    "Connection: close\r\n\r\n" << body;

        const auto utf8 = response.toUTF8();
        connection.write (utf8.getAddress(), static_cast<int> (std::strlen (utf8.getAddress())));
    }

    bool exchangeCode (const juce::String& code, const juce::String& verifier,
                       const juce::String& redirectUri, const juce::String& clientId,
                       juce::String& errorOut)
    {
        auto url = juce::URL (juce::String (kApiBase) + "/oauth/token")
                       .withParameter ("grant_type", "authorization_code")
                       .withParameter ("code", code)
                       .withParameter ("code_verifier", verifier)
                       .withParameter ("redirect_uri", redirectUri)
                       .withParameter ("client_id", clientId);

        int statusCode = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                          .withConnectionTimeoutMs (15000)
                          .withStatusCode (&statusCode));

        if (stream == nullptr)
        {
            errorOut = "Could not reach TONE3000 to complete sign-in.";
            return false;
        }

        auto json = juce::JSON::parse (stream->readEntireStreamAsString());

        if (statusCode != 200)
        {
            errorOut = "TONE3000 sign-in failed (" + describeHttpError (json, statusCode) + ").";
            return false;
        }

        const juce::ScopedLock sl (impl.lock);
        impl.accessToken = json.getProperty ("access_token", juce::var()).toString();
        impl.refreshToken = json.getProperty ("refresh_token", juce::var()).toString();
        const double expiresIn = static_cast<double> (json.getProperty ("expires_in", juce::var (3600)));
        impl.accessTokenExpiry = juce::Time::getCurrentTime() + juce::RelativeTime::seconds (juce::jmax (30.0, expiresIn - 30.0));
        impl.saveConfig();
        return true;
    }

    void fail (const juce::String& message)
    {
        auto cb = onError;
        juce::MessageManager::callAsync ([cb, message] { if (cb) cb (message); });
        impl.finishAuth (this);
    }

    const Mode mode;
    Tone3000Client::Impl& impl;
    std::function<void()> onSignedIn;
    std::function<void (Tone3000Client::ToneModels)> onSelected;
    std::function<void (juce::String)> onError;
    juce::StreamingSocket listener;
    std::atomic<bool> canceled { false };
};

//==============================================================================
/** Downloads one model file with Bearer auth, streaming to disk with progress.
    One-shot; deletes itself (via the owning Impl, on the message thread) once done. */
class Tone3000Client::Impl::DownloadThread : public juce::Thread
{
public:
    DownloadThread (Tone3000Client::Impl& implIn, Tone3000Client::Model modelIn, juce::File destDirIn,
                    std::function<void (float)> onProgressIn,
                    std::function<void (juce::File)> onCompleteIn,
                    std::function<void (juce::String)> onErrorIn)
        : juce::Thread ("Tone3000 Download"), impl (implIn), model (std::move (modelIn)),
          destDir (std::move (destDirIn)), onProgress (std::move (onProgressIn)),
          onComplete (std::move (onCompleteIn)), onError (std::move (onErrorIn))
    {
    }

    void run() override
    {
        juce::String tokenError;
        if (! impl.ensureValidAccessToken (tokenError))
        {
            fail (tokenError);
            return;
        }

        juce::String token;
        { const juce::ScopedLock sl (impl.lock); token = impl.accessToken; }

        juce::URL url (model.modelUrl);
        int statusCode = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                          .withExtraHeaders ("Authorization: Bearer " + token + "\r\n")
                          .withConnectionTimeoutMs (20000)
                          .withStatusCode (&statusCode));

        if (stream == nullptr || statusCode != 200)
        {
            fail ("Could not download the model from TONE3000 (HTTP " + juce::String (statusCode) + ").");
            return;
        }

        destDir.createDirectory();

        juce::String extension = juce::URL (model.modelUrl).getFileName().fromLastOccurrenceOf (".", false, false);
        if (extension.isEmpty())
            extension = "nam";

        const auto destFile = declash (destDir.getChildFile (slugify (model.name) + "." + extension));

        const auto total = stream->getTotalLength();
        juce::int64 readSoFar = 0;

        {
            juce::FileOutputStream out (destFile);
            if (! out.openedOk())
            {
                fail ("Could not create the model file in the library.");
                return;
            }

            char buffer[1 << 16];

            for (;;)
            {
                if (threadShouldExit())
                {
                    out.flush();
                    destFile.deleteFile();
                    // Report the cancel like every other outcome: a silent return
                    // leaves the UI's progress row for this model up forever.
                    fail ("Download canceled.");
                    return;
                }

                const auto n = stream->read (buffer, static_cast<int> (sizeof (buffer)));
                if (n <= 0)
                    break;

                out.write (buffer, static_cast<size_t> (n));
                readSoFar += n;

                if (total > 0)
                {
                    const float progress = juce::jlimit (0.0f, 1.0f,
                        static_cast<float> (static_cast<double> (readSoFar) / static_cast<double> (total)));
                    auto cb = onProgress;
                    juce::MessageManager::callAsync ([cb, progress] { if (cb) cb (progress); });
                }
            }

            out.flush();
        }

        if (total > 0 && readSoFar != total)
        {
            destFile.deleteFile();
            fail ("The model download was interrupted before completing.");
            return;
        }

        auto cb = onComplete;
        juce::MessageManager::callAsync ([cb, destFile] { if (cb) cb (destFile); });
        impl.finishDownload (this);
    }

private:
    void fail (const juce::String& message)
    {
        auto cb = onError;
        juce::MessageManager::callAsync ([cb, message] { if (cb) cb (message); });
        impl.finishDownload (this);
    }

    Tone3000Client::Impl& impl;
    Tone3000Client::Model model;
    juce::File destDir;
    std::function<void (float)> onProgress;
    std::function<void (juce::File)> onComplete;
    std::function<void (juce::String)> onError;
};

//==============================================================================
/** One blocking API job (browse / list models / favorite) on its own thread.
    One-shot; deletes itself (via the owning Impl, on the message thread) once
    done. The body delivers its own result before returning. */
class Tone3000Client::Impl::TaskThread : public juce::Thread
{
public:
    TaskThread (Tone3000Client::Impl& implIn, const char* threadName, std::function<void()> bodyIn)
        : juce::Thread (threadName), impl (implIn), body (std::move (bodyIn))
    {
    }

    void run() override
    {
        if (body)
            body();

        impl.finishTask (this);
    }

private:
    Tone3000Client::Impl& impl;
    std::function<void()> body;
};

//==============================================================================
void Tone3000Client::Impl::runTask (const char* threadName, std::function<void()> body)
{
    auto thread = std::make_unique<TaskThread> (*this, threadName, std::move (body));
    auto* raw = thread.get();
    taskThreads.push_back (std::move (thread));
    raw->startThread();
}

void Tone3000Client::Impl::finishAuth (AuthFlowThread* t)
{
    juce::MessageManager::callAsync ([this, t, alive = alive]
    {
        if (! *alive)
            return;
        if (authThread.get() == t)
            authThread.reset();
    });
}

void Tone3000Client::Impl::finishTask (TaskThread* t)
{
    juce::MessageManager::callAsync ([this, t, alive = alive]
    {
        if (! *alive)
            return;
        taskThreads.erase (std::remove_if (taskThreads.begin(), taskThreads.end(),
            [t] (const std::unique_ptr<TaskThread>& up) { return up.get() == t; }), taskThreads.end());
    });
}

void Tone3000Client::Impl::finishDownload (DownloadThread* t)
{
    juce::MessageManager::callAsync ([this, t, alive = alive]
    {
        if (! *alive)
            return;
        downloadThreads.erase (std::remove_if (downloadThreads.begin(), downloadThreads.end(),
            [t] (const std::unique_ptr<DownloadThread>& up) { return up.get() == t; }), downloadThreads.end());
    });
}

Tone3000Client::Impl::~Impl()
{
    *alive = false;

    if (authThread != nullptr)
    {
        authThread->cancel();
        authThread->stopThread (4000);
        authThread.reset();
    }

    for (auto& t : downloadThreads)
    {
        t->signalThreadShouldExit();
        t->stopThread (4000);
    }
    downloadThreads.clear();

    for (auto& t : taskThreads)
    {
        t->signalThreadShouldExit();
        t->stopThread (8000);
    }
    taskThreads.clear();
}

//==============================================================================
Tone3000Client::Tone3000Client() : impl (std::make_unique<Impl>()) {}
Tone3000Client::~Tone3000Client() = default;

void Tone3000Client::setClientId (const juce::String& publishableKey)
{
    const juce::ScopedLock sl (impl->lock);
    // Clearing the override falls back to the built-in key, never to "unconfigured".
    impl->clientId = publishableKey.isNotEmpty() ? publishableKey
                                                 : Impl::builtinClientId();
    impl->saveConfig();
}

juce::String Tone3000Client::getClientId() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->clientId;
}

bool Tone3000Client::isConfigured() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->clientId.isNotEmpty();
}

bool Tone3000Client::isAuthenticated() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->refreshToken.isNotEmpty();
}

void Tone3000Client::startSelectFlow (std::function<void (ToneModels)> onToneSelected,
                                      std::function<void (juce::String)> onError)
{
    if (! isConfigured())
    {
        if (onError) onError ("TONE3000 is not configured. Add your publishable key in Settings.");
        return;
    }

    if (impl->authThread != nullptr)
    {
        if (onError) onError ("A TONE3000 selection is already in progress.");
        return;
    }

    impl->authThread = std::make_unique<Impl::AuthFlowThread> (*impl, std::move (onToneSelected), std::move (onError));
    impl->authThread->startThread();
}

void Tone3000Client::startSignIn (std::function<void()> onSuccess,
                                  std::function<void (juce::String)> onError)
{
    if (! isConfigured())
    {
        if (onError) onError ("TONE3000 is not configured. Add your publishable key in Settings.");
        return;
    }

    // One browser flow at a time: both kinds share the loopback port range.
    if (impl->authThread != nullptr)
    {
        if (onError) onError ("A TONE3000 sign-in is already in progress.");
        return;
    }

    impl->authThread = std::make_unique<Impl::AuthFlowThread> (*impl, std::move (onSuccess), std::move (onError));
    impl->authThread->startThread();
}

juce::String Tone3000Client::getUsername() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->username;
}

void Tone3000Client::browse (const BrowseRequest& request,
                             std::function<void (TonePage)> onResult,
                             std::function<void (juce::String)> onError)
{
    if (! isAuthenticated())
    {
        if (onError) onError ("Sign in to browse TONE3000.");
        return;
    }

    auto* state = impl.get();
    impl->runTask ("Tone3000 Browse", [state, request, onResult, onError]
    {
        TonePage page;
        juce::String error;

        if (state->browseBlocking (request, page, error))
            juce::MessageManager::callAsync ([onResult, page] { if (onResult) onResult (page); });
        else
            juce::MessageManager::callAsync ([onError, error] { if (onError) onError (error); });
    });
}

void Tone3000Client::listToneModels (juce::int64 toneId, bool isIr,
                                     std::function<void (juce::Array<Model>)> onResult,
                                     std::function<void (juce::String)> onError)
{
    if (! isAuthenticated())
    {
        if (onError) onError ("Sign in to browse TONE3000.");
        return;
    }

    auto* state = impl.get();
    impl->runTask ("Tone3000 Models", [state, toneId, isIr, onResult, onError]
    {
        juce::String error;
        auto models = state->fetchModels (toneId, isIr ? juce::String() : juce::String ("2"), &error);

        if (error.isNotEmpty())
            juce::MessageManager::callAsync ([onError, error] { if (onError) onError (error); });
        else
            juce::MessageManager::callAsync ([onResult, models] { if (onResult) onResult (models); });
    });
}

void Tone3000Client::setFavorite (juce::int64 toneId, bool favorite,
                                  std::function<void()> onSuccess,
                                  std::function<void (juce::String)> onError)
{
    if (! isAuthenticated())
    {
        if (onError) onError ("Sign in to favorite tones on TONE3000.");
        return;
    }

    auto* state = impl.get();
    impl->runTask ("Tone3000 Favorite", [state, toneId, favorite, onSuccess, onError]
    {
        const juce::URL url (juce::String (kApiBase) + "/tones/" + juce::String (toneId) + "/favorite");
        juce::var json;
        juce::String error;

        if (! state->apiRequest (url, favorite ? "PUT" : "DELETE", json, error))
        {
            juce::MessageManager::callAsync ([onError, error] { if (onError) onError (error); });
            return;
        }

        {
            const juce::ScopedLock sl (state->lock);

            if (favorite)
                state->favoritedIds.insert (toneId);
            else
                state->favoritedIds.erase (toneId);

            state->dropFavoritesCacheEntries();
        }

        juce::MessageManager::callAsync ([onSuccess] { if (onSuccess) onSuccess(); });
    });
}

void Tone3000Client::downloadModel (const Model& model, const juce::File& destDir,
                                    std::function<void (float)> onProgress,
                                    std::function<void (juce::File)> onComplete,
                                    std::function<void (juce::String)> onError)
{
    if (! isAuthenticated())
    {
        if (onError) onError ("Not signed in to TONE3000.");
        return;
    }

    auto thread = std::make_unique<Impl::DownloadThread> (*impl, model, destDir,
        std::move (onProgress), std::move (onComplete), std::move (onError));
    auto* raw = thread.get();
    impl->downloadThreads.push_back (std::move (thread));
    raw->startThread();
}

void Tone3000Client::signOut()
{
    const juce::ScopedLock sl (impl->lock);
    impl->accessToken.clear();
    impl->refreshToken.clear();
    impl->accessTokenExpiry = {};
    impl->username.clear();
    impl->favoritedIds.clear();
    impl->browseCache.clear();
    impl->saveConfig();
}
} // namespace tubamp
