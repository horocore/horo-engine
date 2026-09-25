#include "editor/modals/build/BuildWorkflowPreviewState.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace {
        std::vector<BuildPreviewStage> Stages(const BuildPreviewRequest &request) {
            switch (request.kind) {
                case BuildPreviewKind::Build: {
                    std::vector<BuildPreviewStage> result{{"build.preview.step.validate", "build.preview.stage.validate",
                                                           "build.preview.log.validate", "build.preview.pipeline.validate"},
                                                          {"build.preview.step.compile", "build.preview.stage.compile",
                                                           "build.preview.log.compile", "build.preview.pipeline.compile"}};
                    if (!request.compileOnly) {
                        result.push_back({"build.preview.step.cook", "build.preview.stage.cook", "build.preview.log.cook",
                                          "build.preview.pipeline.cook"});
                        result.push_back({"build.preview.step.package", "build.preview.stage.package", "build.preview.log.package",
                                          "build.preview.pipeline.package"});
                    }
                    if (request.testAfterBuild)
                        result.push_back({"build.preview.step.test", "build.preview.stage.test", "build.preview.log.test",
                                          "build.preview.pipeline.test"});
                    if (request.runAfterBuild && !request.compileOnly)
                        result.push_back({"build.preview.step.run", "build.preview.stage.run", "build.preview.log.run",
                                          "build.preview.pipeline.run"});
                    return result;
                }
                case BuildPreviewKind::Tests:
                    return {{"build.preview.step.discover", "build.preview.stage.discover", "build.preview.log.discover"},
                            {"build.preview.step.test", "build.preview.stage.test", "build.preview.log.test"},
                            {"build.preview.step.report", "build.preview.stage.report", "build.preview.log.report"}};
                case BuildPreviewKind::Release:
                    return {{"build.preview.step.validate", "build.preview.stage.validate", "build.preview.log.validate"},
                            {"build.preview.step.configure", "build.preview.stage.configure", "build.preview.log.configure"},
                            {"build.preview.step.compile", "build.preview.stage.compile", "build.preview.log.compile"},
                            {"build.preview.step.package", "build.preview.stage.package", "build.preview.log.package"},
                            {"build.preview.step.test", "build.preview.stage.test", "build.preview.log.test"},
                            {"build.preview.step.sign", "build.preview.stage.sign", "build.preview.log.sign"},
                            {"build.preview.step.verify", "build.preview.stage.verify", "build.preview.log.verify"}};
                case BuildPreviewKind::Publish:
                    return {{"build.preview.step.validate", "build.preview.stage.validate", "build.preview.log.validate"},
                            {"build.preview.step.upload", "build.preview.stage.upload", "build.preview.log.upload"},
                            {"build.preview.step.confirm", "build.preview.stage.confirm", "build.preview.log.confirm"}};
            }
            return {};
        }
    }  // namespace

    bool BuildWorkflowPreviewState::Start(BuildPreviewRequest request) {
        if (request.kind == BuildPreviewKind::Publish && !HasVerifiedCandidate())
            return false;
        if (job_ && job_->status == BuildPreviewStatus::Running)
            return false;
        job_ = BuildPreviewJob{.request = std::move(request)};
        job_->stages = Stages(job_->request);
        job_->stageProgress.resize(job_->stages.size(), 0.0F);
        return true;
    }

    void BuildWorkflowPreviewState::Update(const float dt) {
        if (!job_ || job_->status != BuildPreviewStatus::Running || dt <= 0.0F)
            return;
        job_->elapsed += dt;
        constexpr float mockStageDuration = 1.8F;
        float remainingProgress = dt / mockStageDuration;
        while (remainingProgress > 0.0F && job_->stageIndex < job_->stages.size()) {
            float &progress = job_->stageProgress[job_->stageIndex];
            const float advance = std::min(remainingProgress, 1.0F - progress);
            progress += advance;
            remainingProgress -= advance;
            if (progress < 1.0F)
                return;
            progress = 1.0F;
            ++job_->stageIndex;
        }
        if (job_->stageIndex < job_->stages.size())
            return;
        job_->status = BuildPreviewStatus::Completed;
        if (job_->request.kind == BuildPreviewKind::Release) {
            verifiedCandidate_ = true;
            published_ = false;
            candidateName_ = job_->request.name + " " + job_->request.version;
            candidatePath_ = job_->request.output;
            candidateTarget_ = job_->request.target;
        } else if (job_->request.kind == BuildPreviewKind::Publish) {
            published_ = true;
        }
    }

    void BuildWorkflowPreviewState::Cancel() {
        if (job_ && job_->status == BuildPreviewStatus::Running)
            job_->status = BuildPreviewStatus::Cancelled;
    }

    float BuildWorkflowPreviewState::Progress() const noexcept {
        if (!job_ || job_->stages.empty())
            return 0.0F;
        if (job_->status == BuildPreviewStatus::Completed)
            return 1.0F;
        float total = 0.0F;
        for (const float progress : job_->stageProgress)
            total += progress;
        return std::clamp(total / static_cast<float>(job_->stages.size()), 0.0F, 1.0F);
    }
}  // namespace Horo::Editor
