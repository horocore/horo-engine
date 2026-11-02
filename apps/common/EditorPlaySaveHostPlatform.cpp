#include "EditorPlaySaveHost.h"
#include "Horo/Platform/SecureRandom.h"

#include <cstdlib>
#include <string_view>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Horo::Application::Internal {
    namespace {
        /** @brief Narrow actual process environment capability used solely during approved OS save-root resolution. */
        class PlaySaveRootEnvironment final : public ProcessService {
        public:
            /** @copydoc ProcessService::CurrentProcess */
            ProcessMetadata CurrentProcess() const override {
#if defined(_WIN32)
                return {static_cast<std::uint64_t>(_getpid()), "HoroEditor"};
#else
                return {static_cast<std::uint64_t>(getpid()), "HoroEditor"};
#endif
            }

            /** @brief Reads only the OS root variables consumed by the existing canonical save-root resolver. */
            std::optional<std::string> EnvironmentValue(std::string_view name) const override {
                if (name != "HOME" && name != "LOCALAPPDATA" && name != "XDG_STATE_HOME")
                    return std::nullopt;
                const std::string key{name};
                const auto *value = std::getenv(key.c_str());
                return value ? std::optional<std::string>{value} : std::nullopt;
            }
        };

        /** @brief Chooses the actual compiled host convention; Test roots remain explicitly injected only by tests. */
        Runtime::SaveRootPlatform RootPlatform() noexcept {
#if defined(_WIN32)
            return Runtime::SaveRootPlatform::Windows;
#elif defined(__APPLE__)
            return Runtime::SaveRootPlatform::MacOS;
#else
            return Runtime::SaveRootPlatform::Linux;
#endif
        }
    }  // namespace

    /** @copydoc MakeNativeEditorPlaySaveFactory */
    Result<EditorPlaySaveFactory> MakeNativeEditorPlaySaveFactory(JobSystem &jobs, OperationStore &operations, Clock &clock,
                                                                  WallClock &wallClock) {
        const PlaySaveRootEnvironment environment;
        return MakeEditorPlaySaveFactory(jobs, operations, clock, wallClock, environment, RootPlatform(),
                                         Platform::CreateNativeSecureRandomSource());
    }
}  // namespace Horo::Application::Internal
