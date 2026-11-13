#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <cmath>
#include <filesystem>
#include <mutex>
#include <optional>

namespace Horo::Editor::Theme {
    namespace {
        struct Stamp {
            std::filesystem::file_time_type modified;
            std::uintmax_t size{};
            bool operator==(const Stamp &) const = default;
        };

        struct Inspection {
            std::uint64_t generation{};
            std::optional<Stamp> stamp;
            std::optional<ThemeEntry> candidate;
            bool unchanged{};
        };

        struct Completion {
            std::mutex mutex;
            std::optional<Inspection> result;
        };

        /** @brief Reads metadata only on the I/O lane; missing atomic-save intermediates remain retryable. */
        std::optional<Stamp> InspectStamp(const std::filesystem::path &path) {
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(path, error);
            if (error)
                return {};
            const auto size = std::filesystem::file_size(path, error);
            return error ? std::nullopt : std::optional{Stamp{modified, size}};
        }

        /** @brief Parses a bounded candidate only after its source metadata changes. */
        Inspection Inspect(const std::string &source, const std::uint64_t generation, const std::optional<Stamp> &previous) {
            Inspection inspected{.generation = generation, .stamp = InspectStamp(source)};
            if (!inspected.stamp)
                return inspected;
            if (inspected.stamp == previous) {
                inspected.unchanged = true;
                return inspected;
            }
            ThemeEntry candidate;
            if (LoadThemeFromJson(source.c_str(), candidate) && inspected.stamp == InspectStamp(source))
                inspected.candidate = std::move(candidate);
            return inspected;
        }
    }  // namespace

    struct LiveReload::State {
        JobSystem &jobs;
        EditorDataBus &events;
        // One worker writes completion; only the GUI owner consumes it after terminal publication.
        std::shared_ptr<Completion> completion{std::make_shared<Completion>()};
        std::optional<JobHandle> job;
        std::optional<Stamp> observed;
        std::string source;
        std::uint64_t generation{};
        double elapsed{0.5};
        bool rejected{};
        bool stopped{};
    };

    /** @copydoc LiveReload::LiveReload */
    LiveReload::LiveReload(JobSystem &jobs, EditorDataBus &events) : state_(std::make_unique<State>(State{jobs, events})) {}

    LiveReload::~LiveReload() {
        Shutdown();
    }

    /** @copydoc LiveReload::Update */
    void LiveReload::Update(const double elapsedSeconds) {
        auto &state = *state_;
        if (state.stopped)
            return;
        const auto &themes = GetThemeList();
        const int index = GetActiveThemeIndex();
        const std::string_view source = index >= 0 && static_cast<std::size_t>(index) < themes.size()
                                            ? std::string_view{themes[static_cast<std::size_t>(index)].sourcePath}
                                            : std::string_view{};
        if (source != state.source) {
            state.source = source;
            ++state.generation;
            state.observed.reset();
            state.rejected = false;
            state.elapsed = 0.5;
        }
        if (state.job) {
            const auto snapshot = state.job->Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                return;
            std::optional<Inspection> completed;
            {
                std::lock_guard lock(state.completion->mutex);
                completed = std::move(state.completion->result);
                state.completion->result.reset();
            }
            state.job.reset();
            if (completed && completed->generation == state.generation && !completed->unchanged) {
                state.observed = completed->stamp;
                if (completed->candidate && ApplyReloadedTheme(std::move(*completed->candidate))) {
                    state.rejected = false;
                    state.events.Publish(ThemeReloadedEvent{state.source});
                    LOG_INFO("editor.theme", "Reloaded theme '%s'.", state.source.c_str());
                } else if (!state.rejected) {
                    state.rejected = true;
                    LOG_WARN("editor.theme", "Theme reload rejected; keeping the previous theme: '%s'.", state.source.c_str());
                }
            }
        }
        if (state.source.empty())
            return;
        if (std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0)
            state.elapsed = (std::min)(0.5, state.elapsed + elapsedSeconds);
        if (state.elapsed < 0.5)
            return;
        state.elapsed = 0.0;
        const auto completion = state.completion;
        auto submitted = state.jobs.SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                 [source = state.source, generation = state.generation, previous = state.observed,
                                                  completion](const CancellationToken &cancelled) {
            if (cancelled.IsCancellationRequested())
                return JobCancelled();
            auto inspected = Inspect(source, generation, previous);
            if (cancelled.IsCancellationRequested())
                return JobCancelled();
            std::lock_guard lock(completion->mutex);
            completion->result = std::move(inspected);
            return Result<void>::Success();
        });
        if (submitted.HasValue())
            state.job = std::move(submitted).Value();
    }

    /** @copydoc LiveReload::Shutdown */
    void LiveReload::Shutdown() noexcept {
        auto &state = *state_;
        if (state.stopped)
            return;
        state.stopped = true;
        if (state.job) {
            static_cast<void>(state.job->RequestCancel());
            static_cast<void>(state.job->Wait());
            state.job.reset();
        }
    }
}  // namespace Horo::Editor::Theme
