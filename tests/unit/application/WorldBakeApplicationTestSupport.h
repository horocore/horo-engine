#pragma once

#include "../editor/PartitionedWorldResidencyTestSupport.h"
#include "Horo/Application/WorldBakeApplication.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>

namespace Horo::Application::WorldBakeTestSupport {
    namespace T = Editor::WorldResidencyTests;

    /** @brief Captures two actual page documents with equal local IDs and distinct immutable page identities. */
    inline WorldBakeRequest Request(Editor::PartitionedWorldWorkspace &workspace, const std::optional<Assets::AssetId> mesh = {},
                                    const std::optional<Assets::AssetId> navigation = {}) {
        REQUIRE(workspace.Open(T::Document()).HasValue());
        auto source = T::Source();
        if (mesh)
            source.insert(source.find("\"primitiveMesh\""), "\"meshAsset\":\"" + mesh->ToString() + "\",");
        for (const auto &page : T::Document().pages) {
            auto pageSource = source;
            if (navigation) {
                auto decoded = SceneSource::DecodeSceneSource(source);
                REQUIRE(decoded.HasValue());
                const auto surface = Navigation::SurfaceId::Create(page.head.sourceAsset.Bytes().back());
                const auto profile = Navigation::NavigationAgentProfileId::Create(1);
                REQUIRE(surface.HasValue());
                REQUIRE(profile.HasValue());
                auto document = std::move(decoded).Value();
                document.objects.front().components.navigationSurface =
                    Runtime::NavigationSurfaceComponent{.id = surface.Value(), .definition = *navigation, .profiles = {profile.Value()}};
                pageSource = SceneSource::EncodeSceneSource({document.objects, document.prefabInstances});
            }
            REQUIRE(workspace.LoadPage(page.head, pageSource, T::Current(workspace)->fence).HasValue());
        }
        WorldBakeRequest request{.document = T::Current(workspace), .title = "Bake streaming cells"};
        request.project.fence = {T::Asset(12), 1, {}};
        request.project.fence.schemas.bytes.front() = 1;
        request.project.settings.bytes.front() = 2;
        for (const auto &page : request.document->document.pages)
            request.sources.push_back(
                {page.head, {(std::filesystem::current_path() / (page.head.sourceAsset.ToString() + ".scene")).string(), 1, 1}});
        return request;
    }

    /** @brief Single-worker FIFO marker joins all earlier queued callbacks under an explicit finite policy. */
    inline void Drain(JobSystem &jobs, const JobResource lane = JobResource::Cpu) {
        auto marker = jobs.SubmitResult({.resource = lane}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(marker.HasValue());
        REQUIRE(marker.Value()
                    .Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(5000)})
                    .HasValue());
    }

    /** @brief Prevents worker execution until cancellation/replacement/source edits have been deterministically installed. */
    struct Gate final {
        std::mutex mutex;
        std::condition_variable changed;
        bool open{};
        JobHandle job;

        explicit Gate(JobSystem &jobs, const JobResource lane = JobResource::Cpu) {
            auto accepted = jobs.SubmitResult({.resource = lane}, [this](const CancellationToken &) {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return open;
                });
                return Result<void>::Success();
            });
            REQUIRE(accepted.HasValue());
            job = std::move(accepted).Value();
        }

        ~Gate() {
            Release();
            (void)job.Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(5000)});
        }

        void Release() {
            {
                std::lock_guard lock(mutex);
                open = true;
            }
            changed.notify_all();
        }
    };

}  // namespace Horo::Application::WorldBakeTestSupport
