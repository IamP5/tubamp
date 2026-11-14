// The production Environment for Updater(). Compiled into the plugin only, because it
// needs JUCE: the message thread, the browser, and what kind of wrapper loaded us.

#include "../Updater.h"
#include "../detail/Environment.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <dlfcn.h>
#include <pwd.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef TUBAMP_UPDATE_DEV_ROOTS
 #define TUBAMP_UPDATE_DEV_ROOTS 0
#endif

namespace tubamp::update
{
namespace
{
    namespace fs = std::filesystem;

    /** The Mach-O this code was loaded from: the plugin binary, not the host. */
    fs::path runningImage()
    {
        Dl_info info {};
        if (dladdr (reinterpret_cast<const void*> (&runningImage), &info) == 0 || info.dli_fname == nullptr)
            return {};
        return fs::path (info.dli_fname);
    }

    /** The account's real home, also inside a host whose $HOME points at a container. */
    fs::path accountHome()
    {
        const auto* entry = getpwuid (getuid());
        return entry != nullptr && entry->pw_dir != nullptr ? fs::path (entry->pw_dir) : fs::path();
    }

    detail::OsVersion thisMac()
    {
        char text[32] {};
        auto size = sizeof text;
        if (sysctlbyname ("kern.osproductversion", text, &size, nullptr, 0) != 0)
            return {};
        return detail::OsVersion::parse (text).value_or (detail::OsVersion {});
    }

    detail::Environment production()
    {
        detail::Environment env;
        env.running = Version::parse (JucePlugin_VersionString).value_or (Version {});
        env.trust = &detail::TrustRoot::release();
        env.fetcher = detail::makeSystemFetcher (env.running);
        env.os = thisMac();
        env.now = [] { return std::chrono::time_point_cast<std::chrono::seconds> (std::chrono::system_clock::now()); };
        env.post = [] (std::function<void()> job) { juce::MessageManager::callAsync (std::move (job)); };
        env.openInBrowser = [] (const detail::Url& url) { juce::URL (juce::String (url.text())).launchInDefaultBrowser(); };

        const bool standalone = juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone;
        auto home = accountHome();
        fs::path system = detail::defaultSystemComponents;
        bool rootsKnown = ! home.empty() && Version::parse (JucePlugin_VersionString).has_value();

       #if TUBAMP_UPDATE_DEV_ROOTS
        const auto* devHome = std::getenv ("TUBAMP_UPDATE_HOME");
        const auto* devSystem = std::getenv ("TUBAMP_UPDATE_SYSTEM_COMPONENTS");
        rootsKnown = rootsKnown && devHome != nullptr && *devHome != '\0';
        if (rootsKnown)
        {
            home = devHome;
            system = devSystem != nullptr && *devSystem != '\0' ? fs::path (devSystem) : home / "SystemComponents";
        }
       #endif

        env.site = detail::InstallSite::detect (runningImage(), home, rootsKnown, standalone, system);
        return env;
    }
}

Updater::Updater() : Updater (production()) {}
} // namespace tubamp::update
