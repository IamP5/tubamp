#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace tubamp
{
/**
    TONE3000 API client (https://www.tone3000.com/api/v1/...).

    Auth: OAuth 2.0 + PKCE (S256). The user supplies their publishable key
    (t3k_pub_...) in Settings; it is stored in the plugin's properties file along
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
    };

    struct ToneModels
    {
        juce::int64 toneId = 0;
        juce::Array<Model> models;
    };

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
