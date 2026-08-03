#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace tubamp
{
/**
    TONE3000 API client (https://www.tone3000.com/api/v1/...).

    Auth: OAuth 2.0 + PKCE (S256). The publishable key (t3k_pub_..., the OAuth
    client_id — safe to embed client-side) is baked in at compile time via the
    CMake cache entry TUBAMP_T3K_CLIENT_ID, so users need no configuration; a
    key entered in Settings overrides it and is stored in tone3000.json along
    with the refresh token. Access is per-user, per the TONE3000 API ToS: models
    are downloaded only on explicit user request, never bulk-fetched or cached
    beyond the user's own library.

    Browse integration uses the *Select flow*: startSelectFlow() opens the system
    browser at /api/v1/oauth/authorize?prompt=select_tone&format=nam&architecture=2
    &preview=true with a loopback redirect URI (http://127.0.0.1:<port>/callback).
    A tiny local HTTP listener catches the redirect carrying ?code&state&tone_id,
    exchanges the code (PKCE) for tokens, then fetches the tone's models.

    All network calls run on an internal background thread; results are delivered
    on the message thread via the callbacks. Message-thread only API.
*/
class Tone3000Client
{
public:
    Tone3000Client();
    ~Tone3000Client();

    struct Model
    {
        juce::int64 id = 0;
        juce::String name;
        juce::String modelUrl;      // requires Bearer auth to fetch
        juce::String size;          // standard | lite | feather | nano | custom
        juce::String architecture;  // "1" | "2" | "custom" | empty
        juce::String kind;          // "nam" | "wav", derived from the URL extension
    };

    struct ToneModels
    {
        juce::int64 toneId = 0;
        juce::Array<Model> models;
    };

    // --- in-plugin catalog browsing (docs/REACT-UI.md §TONE3000 browser)

    struct Creator
    {
        juce::String username;
        juce::String avatarUrl;     // empty when none
    };

    struct Tone
    {
        juce::int64 id = 0;
        juce::String title, description, gear, format;  // format: "nam" | "ir"
        juce::String imageUrl;      // first image, empty when none
        juce::String createdAt;     // ISO 8601
        Creator creator;
        juce::int64 downloadsCount = 0, favoritesCount = 0;
        bool favorited = false;     // from the cached favorited-ids set
        juce::StringArray makes, tags, sizes;
        int modelsCount = 0;        // a2_models_count (nam) / irs_count (ir)
    };

    struct TonePage
    {
        juce::Array<Tone> tones;
        int page = 1, totalPages = 1;
        juce::int64 total = 0;
    };

    struct BrowseRequest
    {
        juce::String kind;   // "models" -> format=nam&architecture=2 | "irs" -> format=ir
        juce::String shelf;  // "all" -> /tones/search | "favorites" -> /tones/favorited
        juce::String sort;   // "trending" | "newest" | "downloads" (downloads-all-time)
        juce::String query;
        juce::String gear;   // empty = all
        int page = 1;        // 1-based; page_size fixed at 25
    };

    /** Standard OAuth sign-in (system browser + loopback redirect, PKCE), no
        tone selection. On success fetches /user (username) and the favorited
        tone-id set, then calls onSuccess. */
    void startSignIn (std::function<void()> onSuccess,
                      std::function<void (juce::String)> onError);

    /** TONE3000 username once known (persisted with the tokens); empty before. */
    juce::String getUsername() const;

    /** One catalog page. Network on a background thread; callbacks on the
        message thread. Responses are cached in-memory for a few minutes per
        request key (ToS: no persistent catalog caching; search is heavily
        rate-limited). */
    void browse (const BrowseRequest&,
                 std::function<void (TonePage)> onResult,
                 std::function<void (juce::String)> onError);

    /** Downloadable files of one tone: architecture=2 for nam tones (A2 only),
        no architecture filter for ir tones. page_size=300, single page. */
    void listToneModels (juce::int64 toneId, bool isIr,
                         std::function<void (juce::Array<Model>)> onResult,
                         std::function<void (juce::String)> onError);

    /** PUT/DELETE /tones/{id}/favorite; keeps the cached favorited-ids set in
        sync so later browse() results carry the right `favorited` flag. */
    void setFavorite (juce::int64 toneId, bool favorite,
                      std::function<void()> onSuccess,
                      std::function<void (juce::String)> onError);

    // --- configuration (persisted in ~/Library/Application Support/tubamp/tone3000.json)
    void setClientId (const juce::String& publishableKey);
    juce::String getClientId() const;
    bool isConfigured() const;   // client id present
    bool isAuthenticated() const; // has a refresh token

    /** Opens the Select flow in the system browser. When the user picks a tone,
        fetches its A2 models (architecture=2; falls back to architecture=1 when
        the tone has no A2 files) and calls onToneSelected. onError on failure or
        user cancel. */
    void startSelectFlow (std::function<void (ToneModels)> onToneSelected,
                          std::function<void (juce::String)> onError);

    /** Downloads one model file (with Bearer auth) into destDir, slugified name +
        proper extension. Progress in [0,1]. Completion delivers the saved file. */
    void downloadModel (const Model& model, const juce::File& destDir,
                        std::function<void (float)> onProgress,
                        std::function<void (juce::File)> onComplete,
                        std::function<void (juce::String)> onError);

    /** Forget stored tokens (not the client id). */
    void signOut();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Tone3000Client)
};
} // namespace tubamp
