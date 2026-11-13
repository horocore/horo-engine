#pragma once
#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationAssetCook.h"
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Foundation/JobSystem.h"
#include "NativeCacheFiles.h"
#include "assets/AssetCookPublicationFixture.h"
#include "assets/AssetCookTestValues.h"

#include <catch2/catch_message.hpp>
#include <fstream>

namespace Horo::Animation::Test::Cook {
    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Write(const std::filesystem::path &path, std::string_view text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(file.good());
    }

    struct Fixture final {
        Assets::CookTestValues::OwnedCookTestDirectory source{"horo-animation-source", true};
        Assets::CookTestValues::OwnedCookTestDirectory cache{"horo-animation-cache", true};
        Assets::CookTestValues::OwnedCookTestDirectory cooked{"horo-animation-cooked", true};
        Instance::Prepared native;
        Assets::AssetRegistry registry;
        JobSystem jobs{{1, 8}};
        Assets::AssetCookRequest request;
        std::shared_ptr<const PreparedAnimationCook> prepared;
        std::shared_ptr<const Assets::CookerCatalogSnapshot> catalog;

        Fixture() {
            std::filesystem::create_directory(source.path / "assets");
            std::vector<Assets::AssetRecord> records;
            Add(records, "skeleton", native.skeleton->Data().skeleton.Asset(), Take(SerializeSkeletonAnimationSource(*native.skeleton)));
            Add(records, "clip", native.compileClips.front().Data().descriptor.id.Asset(),
                Take(SerializeClipAnimationSource(native.compileClips.front())));
            Add(records, "graph", native.program->Id().Asset(), Take(SerializeAnimationGraphSource(Graph::Simple())));
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            request.sourceRoot = source.path;
            request.cacheFiles = TestSupport::NativeCacheFiles(cache.path);
            request.cookedRoot = cooked.path;
            request.registry = registry.Snapshot();
            request.target = Take(AssetCookTargetId::Parse("headless-null"));
            Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
            Capture();
        }

        ~Fixture() {
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        void Add(std::vector<Assets::AssetRecord> &records, std::string_view kind, Assets::AssetId id, std::string bytes,
                 std::string_view name = {}) {
            const auto relative = "assets/" + std::string(name.empty() ? kind : name) + ".json";
            const auto type = "core.animation." + std::string(kind);
            Write(source.path / relative, bytes);
            Write(source.path / (relative + ".horo"),
                  "{\"schemaVersion\":1,\"assetId\":\"" + id.ToString() + "\",\"assetType\":\"" + type + "\"}");
            records.push_back({id, Take(Assets::AssetTypeId::Parse(type)), Take(ProjectPath::Parse(relative)),
                               Take(ProjectPath::Parse(relative + ".horo"))});
        }

        void Replace(const SkeletonAsset &skeleton, std::span<const AnimationClipAsset> clips, const AnimationGraphData &graph) {
            std::vector<Assets::AssetRecord> records;
            Add(records, "skeleton", skeleton.Data().skeleton.Asset(), Take(SerializeSkeletonAnimationSource(skeleton)));
            for (std::size_t index = 0; index < clips.size(); ++index)
                Add(records, "clip", clips[index].Data().descriptor.id.Asset(), Take(SerializeClipAnimationSource(clips[index])),
                    index == 0 ? "clip" : "clip-" + std::to_string(index));
            Add(records, "graph", graph.id.Asset(), Take(SerializeAnimationGraphSource(graph)));
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            request.registry = registry.Snapshot();
            Capture();
        }

        void Capture() {
            auto capture = Take(Assets::AssetCookInputSnapshot::Capture(source.path, registry.Snapshot(), {}, 16U * 1024U * 1024U));
            request.pinnedInputs = std::make_shared<const Assets::AssetCookInputSnapshot>(std::move(capture));
            prepared =
                std::make_shared<const PreparedAnimationCook>(Take(PreparedAnimationCook::Create(*request.pinnedInputs, request.target)));
            Assets::CookerCatalog builder;
            for (auto contribution : prepared->Contributions())
                REQUIRE(builder.Register(std::move(contribution)).HasValue());
            catalog = Take(builder.Publish());
            std::vector<Assets::AssetCookKeySelection> selections;
            for (const auto &input : request.pinnedInputs->Sources()) {
                const auto *strategy = catalog->Find(input.record.type, request.target);
                REQUIRE(strategy);
                Assets::CookSourceView view{input.record.id,    input.record.type, request.target,
                                            input.sourceDigest, input.bytes,       input.record.sourcePath.String()};
                const auto declared = Take(strategy->InputDependencies(view, {}));
                REQUIRE(declared);
                auto ids = *declared;
                ids.push_back(input.record.id);
                std::ranges::sort(ids);
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                selections.push_back({input.record.id, std::move(ids), strategy->CacheIdentity().settingsDigest});
            }
            request.keyProjection = Take(Assets::AssetCookKeyProjection::Create(*request.pinnedInputs, std::move(selections), 16));
            const auto pin = request.pinnedInputs;
            request.validateHostInputs = [pin] {
                return pin->VerifyUnchanged();
            };
        }

        Result<Assets::AssetCookReport> Cook(const CancellationToken &cancellation = {}) {
            Assets::AssetCookService service(jobs, catalog);
            return service.Cook(request, cancellation);
        }

        Assets::AssetCookGenerationContents Contents(const Assets::AssetCookReport &report) {
            return Take(Assets::ReadCookGenerationContents(*request.publicationFiles, report.generation, 16U * 1024U * 1024U));
        }

        static std::vector<Assets::AssetCookCandidateArtifactView> Views(const Assets::AssetCookGenerationContents &contents) {
            std::vector<Assets::AssetCookCandidateArtifactView> views;
            for (std::size_t i = 0; i < contents.entries.size(); ++i)
                views.push_back({{contents.entries[i].assetId, contents.entries[i].assetType, contents.entries[i].artifactHash},
                                 contents.artifacts[i]});
            return views;
        }
    };
}  // namespace Horo::Animation::Test::Cook
