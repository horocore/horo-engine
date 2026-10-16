#include "../runtime/prefab/PrefabValueOverrideTestSupport.h"
#include "ScenePrefabExpansionTestSupport.h"

namespace Horo::SceneSource {
    namespace {
        using namespace Prefab::RuntimeValueTestSupport;

        /** @brief Retained publication rejects substituted source-only candidates and unavailable current schema context. */
        void CheckProjectionFences(const SceneSourceDocument &document, const Prefab::PrefabSourceResolverSnapshot &sources,
                                   const Prefab::PrefabValueSchemaSnapshot &schemas, const ScenePrefabProjection &fresh,
                                   const Prefab::EffectivePrefabCandidate &source) {
            auto wrong = fresh;
            wrong.instances[0].expanded = source;
            CHECK(ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, {19}, {1}, wrong, sources, Limits(),
                                                        &schemas)
                      .HasError());
            CHECK(ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, {19}, {1}, fresh, sources, Limits())
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
