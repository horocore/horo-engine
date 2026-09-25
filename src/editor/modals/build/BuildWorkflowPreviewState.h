#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    enum class BuildPreviewKind {
        Build,
        Tests,
        Release,
        Publish
    };
    enum class BuildPreviewStatus {
        Running,
        Completed,
        Cancelled
    };

    /** @brief UI-only completion signal for the workspace notification surface. */
    struct BuildPreviewCompletedEvent {
        static constexpr auto HoroEventTypeName = "Horo.Editor.BuildPreviewCompletedEvent";
        BuildPreviewKind kind{BuildPreviewKind::Build};
    };

    struct BuildPreviewRequest {
        BuildPreviewKind kind{BuildPreviewKind::Build};
        std::string name;
        std::string version;
        std::string target;
        std::string output;
        bool testAfterBuild{false};
        bool runAfterBuild{false};
        bool compileOnly{false};
    };

    /** @brief Localized presentation keys for one deterministic mock pipeline step. */
    struct BuildPreviewStage {
        const char *labelKey;
        const char *activityKey;
        const char *logKey;
        const char *pipelineLabelKey{nullptr};
    };

    struct BuildPreviewJob {
        BuildPreviewRequest request;
        BuildPreviewStatus status{BuildPreviewStatus::Running};
        std::vector<BuildPreviewStage> stages;
        std::vector<float> stageProgress; /**< Independent 0–1 progress for each pipeline stage. */
        std::size_t stageIndex{0};
        float elapsed{0.0F};
    };

    /** @brief Deterministic UI-only workflow; it never calls build, test, signing, or publishing services. */
    class BuildWorkflowPreviewState {
    public:
        [[nodiscard]] bool Start(BuildPreviewRequest request);
        void Update(float dt);
        void Cancel();

        [[nodiscard]] const std::optional<BuildPreviewJob> &Job() const noexcept {
            return job_;
        }

        [[nodiscard]] bool HasVerifiedCandidate() const noexcept {
            return verifiedCandidate_ && !published_;
        }

        [[nodiscard]] bool IsPublished() const noexcept {
            return published_;
        }

        [[nodiscard]] float Progress() const noexcept;

        [[nodiscard]] const std::string &CandidateName() const noexcept {
            return candidateName_;
        }

        [[nodiscard]] const std::string &CandidatePath() const noexcept {
            return candidatePath_;
        }

        [[nodiscard]] const std::string &CandidateTarget() const noexcept {
            return candidateTarget_;
        }

    private:
        std::optional<BuildPreviewJob> job_;
        std::string candidateName_;
        std::string candidatePath_;
        std::string candidateTarget_;
        bool verifiedCandidate_{false};
        bool published_{false};
    };
}  // namespace Horo::Editor
