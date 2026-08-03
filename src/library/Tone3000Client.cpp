#include "Tone3000Client.h"

#include "ModelLibrary.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cstring>
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
} // namespace

//==============================================================================
struct Tone3000Client::Impl
{
    Impl() { loadConfig(); }
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
    }

    void saveConfig() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("clientId", clientId);
        obj->setProperty ("refreshToken", refreshToken);

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

    /** Blocking; call from a background thread only. */
    juce::Array<Model> fetchModels (juce::int64 toneId, const juce::String& architecture)
    {
        juce::String token;
        { const juce::ScopedLock sl (lock); token = accessToken; }

        juce::URL url (juce::String (kApiBase) + "/models");
        url = url.withParameter ("tone_id", juce::String (toneId))
                 .withParameter ("page_size", "100")
                 .withParameter ("architecture", architecture);

        int statusCode = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                          .withExtraHeaders ("Authorization: Bearer " + token + "\r\n")
                          .withConnectionTimeoutMs (15000)
                          .withStatusCode (&statusCode));

        juce::Array<Model> result;
        if (stream == nullptr || statusCode != 200)
            return result;

        auto json = juce::JSON::parse (stream->readEntireStreamAsString());
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
                result.add (m);
            }
        }

        return result;
    }

    // Forward-declared here (defined below, out-of-line) rather than at namespace
    // scope: both need access to Impl's private token/lock state, and nesting
    // them inside Impl (a private member of Tone3000Client) keeps that access
    // implicit instead of requiring a public accessor surface.
    class SelectFlowThread;
    class DownloadThread;

    void finishSelect (SelectFlowThread* t);
    void finishDownload (DownloadThread* t);

    juce::CriticalSection lock;
    juce::String clientId, refreshToken, accessToken;
    juce::Time accessTokenExpiry;

    std::unique_ptr<SelectFlowThread> selectThread;
    std::vector<std::unique_ptr<DownloadThread>> downloadThreads;

    // Guards the deferred cleanup lambdas in finishSelect/finishDownload below,
    // which capture `this` and run later on the message thread: if Impl is
    // destroyed before a queued cleanup dispatches, the flag (kept alive by the
    // shared_ptr copy captured in the lambda) tells it to no-op instead of
    // touching freed memory.
    std::shared_ptr<bool> alive { std::make_shared<bool> (true) };
};

//==============================================================================
/** Runs the Select flow end-to-end: PKCE, loopback listener, system-browser
    launch, code exchange, model listing. One-shot; deletes itself (via the
    owning Impl, on the message thread) once done. */
class Tone3000Client::Impl::SelectFlowThread : public juce::Thread
{
public:
    SelectFlowThread (Tone3000Client::Impl& implIn,
                      std::function<void (Tone3000Client::ToneModels)> onSelectedIn,
                      std::function<void (juce::String)> onErrorIn)
        : juce::Thread ("Tone3000 Select"), impl (implIn),
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
                                .withParameter ("state", pkce.state)
                                .withParameter ("prompt", "select_tone")
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
            fail ("Tone selection was canceled.");
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

        if (toneIdStr.isEmpty())
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

        const juce::int64 toneId = toneIdStr.getLargeIntValue();
        auto models = impl.fetchModels (toneId, "2");
        if (models.isEmpty())
            models = impl.fetchModels (toneId, "1"); // tone has no A2 files - fall back

        Tone3000Client::ToneModels result;
        result.toneId = toneId;
        result.models = models;

        auto cb = onSelected;
        juce::MessageManager::callAsync ([cb, result] { if (cb) cb (result); });
        impl.finishSelect (this);
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
        impl.finishSelect (this);
    }

    Tone3000Client::Impl& impl;
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
void Tone3000Client::Impl::finishSelect (SelectFlowThread* t)
{
    juce::MessageManager::callAsync ([this, t, alive = alive]
    {
        if (! *alive)
            return;
        if (selectThread.get() == t)
            selectThread.reset();
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

    if (selectThread != nullptr)
    {
        selectThread->cancel();
        selectThread->stopThread (4000);
        selectThread.reset();
    }

    for (auto& t : downloadThreads)
    {
        t->signalThreadShouldExit();
        t->stopThread (4000);
    }
    downloadThreads.clear();
}

//==============================================================================
Tone3000Client::Tone3000Client() : impl (std::make_unique<Impl>()) {}
Tone3000Client::~Tone3000Client() = default;

void Tone3000Client::setClientId (const juce::String& publishableKey)
{
    const juce::ScopedLock sl (impl->lock);
    impl->clientId = publishableKey;
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

    if (impl->selectThread != nullptr)
    {
        if (onError) onError ("A TONE3000 selection is already in progress.");
        return;
    }

    impl->selectThread = std::make_unique<Impl::SelectFlowThread> (*impl, std::move (onToneSelected), std::move (onError));
    impl->selectThread->startThread();
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
    impl->saveConfig();
}
} // namespace tubamp
