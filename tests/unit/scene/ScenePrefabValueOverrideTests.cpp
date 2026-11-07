#include "../runtime/prefab/PrefabValueOverrideTestSupport.h"
#include "AllocationProbe.h"
#include "ScenePrefabExpansionTestSupport.h"

#include <cstdio>
#include <new>

namespace Horo::SceneSource {
    namespace {
        using namespace Prefab::RuntimeValueTestSupport;

        TEST_CASE("Authored Scene overrides remain canonical when optional snapshot allocation fails",
                  "[prefab][value-snapshot][scene][allocation]") {
            const auto sources = Sources();
            const auto schemas = Schemas();
            const auto instance = Prefab::PrefabInstanceId::Create(9).Value();
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            const auto intent =
                Prefab::PreparePrefabValueEdit(source, {}, {Prefab::PrefabValueEditKind::Assign, Property(), false}, &schemas, Limits())
                    .Value()
                    .rebased;
            SceneSourceDocument document;
            document.prefabInstances.push_back({instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}, intent});
            const SceneSourceView view{document.objects, document.prefabInstances};

            struct Trace final {
                std::array<std::size_t, 4096> sizes{};
                std::size_t count{};
            } trace;

            const auto limits = Limits();
            std::optional<Result<ScenePrefabProjection>> baseline;
            {
                Tests::AllocationProbe::ScopedObserver observer{+[](const std::size_t size, void *context) noexcept {
                    auto &capture = *static_cast<Trace *>(context);
                    if (capture.count < capture.sizes.size())
                        capture.sizes[capture.count] = size;
                    ++capture.count;
                }, &trace};
                baseline.emplace(BuildScenePrefabProjection(view, sources, limits, &schemas));
            }
            REQUIRE(baseline->HasValue());
            REQUIRE(baseline->Value().instances[0].expanded);
            REQUIRE(trace.count <= trace.sizes.size());
            bool observedUnavailable{};
            for (std::size_t index = 0; index < trace.count && !observedUnavailable; ++index) {
                if (trace.sizes[index] != sizeof(Prefab::PrefabValueSnapshot))
                    continue;
                std::fprintf(stderr, "value-snapshot carrier allocation index=%zu bytes=%zu\n", index, trace.sizes[index]);
                std::fflush(stderr);
                std::optional<Result<ScenePrefabProjection>> attempt;
                try {
                    Tests::AllocationProbe::ScopedFailure failure{index};
                    attempt.emplace(BuildScenePrefabProjection(view, sources, limits, &schemas));
                } catch (const std::bad_alloc &) {
                    continue;  // Injections before the optional observation phase are outside this regression's selected seam.
                }
                if (!attempt || attempt->HasError())
                    continue;
                const auto &entry = attempt->Value().instances[0];
                if (!entry.valueInspectionFailure || entry.valueSnapshot || !entry.expanded)
                    continue;
                observedUnavailable = true;
                CHECK_FALSE(attempt->Value().HasBrokenInstances());
                CHECK(entry.rebasedValues == intent);
                CHECK(entry.expanded->ValueOverridesDigest() == baseline->Value().instances[0].expanded->ValueOverridesDigest());
                auto converted = ConvertScenePrefabProjectionToRuntime(view, {19}, {1}, attempt->Value(), {sources, Limits(), &schemas});
                REQUIRE(converted.HasValue());
                CHECK_FALSE(converted.Value().Entities()[0].components.navigationAgent);
            }
            CHECK(observedUnavailable);
        }

        TEST_CASE("Source-only Scene projection captures immutable values and preserves activation when read-only schemas fail",
                  "[prefab][value-snapshot][scene]") {
            const auto sources = Sources();
            SceneSourceDocument document;
            document.prefabInstances.push_back(
                {Prefab::PrefabInstanceId::Create(9).Value(), Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}});
            const SceneSourceView view{document.objects, document.prefabInstances};
            auto withoutSchema = BuildScenePrefabProjection(view, sources, Limits());
            REQUIRE(withoutSchema.HasValue());
            REQUIRE(withoutSchema.Value().instances[0].valueSnapshot);
            CHECK(withoutSchema.Value().instances[0].valueSnapshot->Values().size() == 1);
            CHECK(withoutSchema.Value().instances[0].valueSnapshot->Opaque().size() == 1);
            CHECK_FALSE(withoutSchema.Value().HasBrokenInstances());
            REQUIRE(withoutSchema.Value().instances[0].debugSnapshot);
            CHECK(withoutSchema.Value().instances[0].debugSnapshot->Values().empty());
            CHECK(withoutSchema.Value().instances[0].debugSnapshot->ValueOverridesDigest() == Sha256Digest{});
            auto presentDocument = document;
            const auto source = sources.Resolve(Asset(), document.prefabInstances[0].instanceId, Limits()).Value();
            presentDocument.prefabInstances[0].valueOverrides =
                Prefab::PrefabValueOverrideSet{Asset(), source.Revision().rootSource, {}, {}, {}, {}};
            const auto presentEmpty =
                BuildScenePrefabProjection({presentDocument.objects, presentDocument.prefabInstances}, sources, Limits());
            REQUIRE(presentEmpty.HasValue());
            REQUIRE(presentEmpty.Value().instances[0].debugSnapshot);
            REQUIRE(presentEmpty.Value().instances[0].valueSnapshot);
            CHECK(presentEmpty.Value().instances[0].valueSnapshot->Revision().delta != Sha256Digest{});
            CHECK(presentEmpty.Value().instances[0].debugSnapshot->Values().size() == 1);
            CHECK(presentEmpty.Value().instances[0].debugSnapshot->ValueOverridesDigest() != Sha256Digest{});
            const auto invalidInspectionSchema =
                Prefab::PrefabValueSchemaSnapshot::Create({{Type(), Prefab::PrefabPropertyId::Create(7).Value(),
                                                            Gameplay::ComponentPropertyId::Parse("enabled").Value(), 1,
                                                            Gameplay::ComponentPropertyKind::Number, 0.0}},
                                                          Limits())
                    .Value();
            auto failedInspection = BuildScenePrefabProjection(view, sources, Limits(), &invalidInspectionSchema);
            REQUIRE(failedInspection.HasValue());
            CHECK_FALSE(failedInspection.Value().HasBrokenInstances());
            CHECK(failedInspection.Value().instances[0].valueInspectionFailure);
            CHECK_FALSE(failedInspection.Value().instances[0].valueSnapshot);
            CHECK(ConvertScenePrefabProjectionToRuntime(view, {19}, {1}, failedInspection.Value(),
                                                        {sources, Limits(), &invalidInspectionSchema})
                      .HasValue());
        }

        /** @brief Retained publication rejects substituted source-only candidates and unavailable current schema context. */
        void CheckProjectionFences(const SceneSourceDocument &document, const Prefab::PrefabSourceResolverSnapshot &sources,
                                   const Prefab::PrefabValueSchemaSnapshot &schemas, const ScenePrefabProjection &fresh,
                                   const Prefab::EffectivePrefabCandidate &source) {
            auto wrong = fresh;
            wrong.instances[0].expanded = source;
            CHECK(ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, {19}, {1}, wrong,
                                                        {sources, Limits(), &schemas})
                      .HasError());
            CHECK(ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, {19}, {1}, fresh, {sources, Limits()})
                      .HasError());
            auto unavailable = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits());
            REQUIRE(unavailable.HasValue());
            CHECK(unavailable.Value().HasBrokenInstances());
            REQUIRE(unavailable.Value().instances[0].rebasedValues);
            CHECK(unavailable.Value().instances[0].rebasedValues->orphans.size() == 1);
        }

        /** @brief Checks actual typed enablement and keeps unsupported raw prefab components rejected. */
        void CheckSupportedProjection(const Prefab::PrefabSourceResolverSnapshot &sources, const Prefab::PrefabInstanceId instance) {
            SceneSourceDocument original;
            original.prefabInstances.push_back({instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}});
            const auto baseline = ConvertSceneSourceToRuntime({original.objects, original.prefabInstances}, {19}, {1}, sources, Limits());
            REQUIRE(baseline.HasValue());
            REQUIRE(baseline.Value().Entities()[0].components.navigationAgent);
            CHECK(baseline.Value().Entities()[0].components.navigationAgent->enabled);
            const auto unsupported = ConvertSceneSourceToRuntime({original.objects, original.prefabInstances}, {19}, {1},
                                                                 Prefab::ValueTestSupport::Sources(), Limits());
            REQUIRE(unsupported.HasError());
            CHECK(unsupported.ErrorValue().code.Value() == "scene_conversion.prefab_component_projection_unsupported");
        }

        /** @brief The cached public conversion uses the same explicit schema and cancellation policy as the canonical projection. */
        void CheckCachedConversion(const SceneSourceDocument &document, const Prefab::PrefabSourceResolverSnapshot &sources,
                                   const Prefab::PrefabValueSchemaSnapshot &schemas, Prefab::PrefabExpansionCache &cache) {
            const auto converted = ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, {19}, {1}, sources, Limits(),
                                                               cache, {.schemas = &schemas});
            REQUIRE(converted.HasValue());
            REQUIRE(converted.Value().Entities().size() == 1);
            CHECK_FALSE(converted.Value().Entities()[0].components.navigationAgent);
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            CHECK(ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, {19}, {1}, sources, Limits(), cache,
                                              {cancelled.Token(), &schemas})
                      .HasError());
        }

        TEST_CASE("Fresh cached and actual worker Scene conversion apply identical typed instance overrides",
                  "[unit][scene][prefab][override]") {
            const auto sources = Sources();
            const auto schemas = Schemas();
            const auto instance = Prefab::PrefabInstanceId::Create(9).Value();
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            CheckSupportedProjection(sources, instance);
            auto values = Prefab::PreparePrefabValueEdit(source, std::nullopt, {Prefab::PrefabValueEditKind::Assign, Property(), false},
                                                         &schemas, Limits())
                              .Value()
                              .rebased;
            SceneSourceDocument document;
            document.prefabInstances.push_back({instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}, values});
            auto fresh = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits(), &schemas);
            REQUIRE(fresh.HasValue());
            REQUIRE_FALSE(fresh.Value().HasBrokenInstances());
            Prefab::PrefabExpansionCache cache;
            auto cached = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits(), cache, {}, &schemas);
            REQUIRE(cached.HasValue());
            REQUIRE(cached.Value().instances[0].expanded);
            CheckCachedConversion(document, sources, schemas, cache);
            CHECK(cached.Value().instances[0].expanded->Objects()[0] == fresh.Value().instances[0].expanded->Objects()[0]);
            auto runtime =
                ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, {19}, {1}, sources, Limits(), &schemas);
            REQUIRE(runtime.HasValue());
            REQUIRE(runtime.Value().Entities().size() == 1);
            const auto &agent = runtime.Value().Entities()[0].components.navigationAgent;
            CHECK_FALSE(agent);  // Canonical runtime conversion omits explicitly disabled Navigation components.
            CheckProjectionFences(document, sources, schemas, fresh.Value(), source);
            JobSystem jobs;
            ScenePrefabExpansionOwner owner{jobs};
            ScenePrefabExpansionRequest request{1, {19}, {1}, document, sources, Limits(), schemas};
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(ExpansionTestSupport::TestJoin).HasValue());
            auto completed = owner.TakeCompleted(request);
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            CHECK(completed.Value()->Entities()[0].components.navigationAgent == agent);
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(ExpansionTestSupport::TestJoin).HasValue());
            request.valueSchemas.reset();
            CHECK(owner.TakeCompleted(request).HasError());
            REQUIRE(owner.Shutdown(ExpansionTestSupport::TestJoin).HasValue());
        }
    }  // namespace
}  // namespace Horo::SceneSource
