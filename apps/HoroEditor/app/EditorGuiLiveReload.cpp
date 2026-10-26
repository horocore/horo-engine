#include "EditorGuiLiveReload.h"

#if defined(HORO_EDITOR_LIVE_UI)
#include "GuiRuntimeBridge.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ICompilerLogger.h"
#include "IFileChangeNotifier.h"
#include "RuntimeObjectSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#endif

namespace Horo::Editor {
#if defined(HORO_EDITOR_LIVE_UI)
    namespace {
        /** @brief Bounds compiler diagnostics before forwarding them to the existing log authority. */
        void LogCompiler(const Log::Level level, const char *format, std::va_list arguments) {
            std::array<char, 4096> text{};
            static_cast<void>(std::vsnprintf(text.data(), text.size(), format, arguments));
            char *line = text.data();
            for (char *cursor = line;; ++cursor) {
                const char delimiter = *cursor;
                if (delimiter != '\r' && delimiter != '\n' && delimiter != '\0')
                    continue;
                *cursor = '\0';
                if (cursor != line) {
                    if (level == Log::Level::Error)
                        LOG_ERROR("editor.live_ui", "%s", line);
                    else if (level == Log::Level::Warn)
                        LOG_WARN("editor.live_ui", "%s", line);
                    else
                        LOG_INFO("editor.live_ui", "%s", line);
                }
                if (delimiter == '\0')
                    break;
                line = cursor + 1;
            }
        }

        struct CompilerLogger final : ICompilerLogger {
            void LogError(const char *format, ...) override {
                std::va_list arguments;
                va_start(arguments, format);
                LogCompiler(Log::Level::Error, format, arguments);
                va_end(arguments);
            }

            void LogWarning(const char *format, ...) override {
                std::va_list arguments;
                va_start(arguments, format);
                LogCompiler(Log::Level::Warn, format, arguments);
                va_end(arguments);
            }

            void LogInfo(const char *format, ...) override {
                std::va_list arguments;
                va_start(arguments, format);
                LogCompiler(Log::Level::Info, format, arguments);
                va_end(arguments);
            }
        };

    }  // namespace

    struct EditorGuiLiveReload::State {
        CompilerLogger logger;
        SystemTable system;
        // Destroy runtime first, while its logger and address table remain alive.
        std::unique_ptr<RuntimeObjectSystem> runtime;
    };
#else
    struct EditorGuiLiveReload::State {};
#endif

    EditorGuiLiveReload::EditorGuiLiveReload() = default;
    EditorGuiLiveReload::~EditorGuiLiveReload() = default;

    /** @copydoc EditorGuiLiveReload::Start */
    Result<void> EditorGuiLiveReload::Start() {
#if defined(HORO_EDITOR_LIVE_UI)
        Development::LinkGuiModules();
        state_ = std::make_unique<State>();
        state_->runtime = std::make_unique<RuntimeObjectSystem>();
        if (!state_->runtime->Initialise(&state_->logger, &state_->system)) {
            const ErrorCodeDescriptor failed{.domain = ErrorDomainId{"horo.editor.live_ui"},
                                             .code = ErrorCode{"live_ui.initialization_failed"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "RCC++ development UI initialization failed."};
            return Result<void>::Failure(MakeError(failed));
        }
        std::error_code directoryError;
        // RCC++ does not track every transitive header. Never reuse objects from
        // a different host build after a signature or object-layout change.
        const auto hostRevision = std::filesystem::last_write_time(HORO_GUI_LIVE_HOST_IMAGE, directoryError);
        const auto intermediate = std::filesystem::path{HORO_LIVE_UI_BUILD_DIR} / std::to_string(hostRevision.time_since_epoch().count());
        if (!directoryError)
            std::filesystem::create_directories(intermediate, directoryError);
        if (directoryError) {
            const ErrorCodeDescriptor failed{.domain = ErrorDomainId{"horo.editor.live_ui"},
                                             .code = ErrorCode{"live_ui.build_directory_unavailable"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Unable to create the RCC++ development build directory."};
            return Result<void>::Failure(MakeError(failed));
        }
        state_->runtime->SetIntermediateDir(intermediate.string().c_str());
        std::ifstream includes{HORO_GUI_LIVE_INCLUDE_FILE};
        std::string include;
        while (std::getline(includes, include)) {
            if (!include.empty())
                state_->runtime->AddIncludeDir(include.c_str());
        }
        state_->runtime->SetAdditionalCompileOptions("@\"" HORO_GUI_LIVE_FLAGS_FILE "\"");
        state_->runtime->SetAdditionalLinkOptions("\"" HORO_GUI_LIVE_HOST_IMPORT "\"");
        LOG_INFO("editor.live_ui", "RCC++ GUI drawing active; edit registered GUI source files.");
#endif
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiLiveReload::Update */
    void EditorGuiLiveReload::Update(const double elapsedSeconds) {
#if defined(HORO_EDITOR_LIVE_UI)
        if (!state_ || !state_->runtime)
            return;
        auto &runtime = *state_->runtime;
        if (runtime.GetIsCompiledComplete()) {
            if (runtime.LoadCompiledModule())
                LOG_INFO("editor.live_ui", "Installed recompiled development UI at a frame boundary.");
            else
                LOG_WARN("editor.live_ui", "Development UI replacement failed; retaining the previous object.");
        }
        if (!runtime.GetIsCompiling())
            runtime.GetFileChangeNotifier()->Update(static_cast<float>((std::clamp)(elapsedSeconds, 0.0, 1.0)));
#else
        static_cast<void>(elapsedSeconds);
#endif
    }

    /** @copydoc EditorGuiLiveReload::Registry */
    const GuiLive::IRegistry *EditorGuiLiveReload::Registry() const noexcept {
#if defined(HORO_EDITOR_LIVE_UI)
        return this;
#else
        return nullptr;
#endif
    }

    /** @copydoc EditorGuiLiveReload::Find */
    GuiLive::Binding EditorGuiLiveReload::Find(GuiLive::Id route, GuiLive::Id signature) const noexcept {
#if defined(HORO_EDITOR_LIVE_UI)
        if (state_) {
            for (const auto *module : state_->system.modules) {
                if (module) {
                    if (const auto binding = module->Find(route, signature); binding.invoke)
                        return binding;
                }
            }
        }
#endif
        return {};
    }
}  // namespace Horo::Editor
