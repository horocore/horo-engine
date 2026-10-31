#include "../runtime/prefab/PrefabValueOverrideTestSupport.h"
#include "AllocationProbe.h"
#include "ScenePrefabExpansionTestSupport.h"
#include "ScenePrefabProjectionContext.h"

namespace Horo::SceneSource {
    namespace {
        using namespace Prefab::RuntimeValueTestSupport;

        TEST_CASE("Scene prefab inspection shares bounded admission across fresh cached and worker preparation",
                  "[unit][scene][prefab][debug][boundary]") {
            const auto sources = Sources();
            SceneSourceDocument document;
            for (std::size_t index = 0; index < 65; ++index)
                document.prefabInstances.push_back(
                    {Prefab::PrefabInstanceId::Create(index + 1).Value(), Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}});
            auto fresh = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits());
            REQUIRE(fresh.HasValue());
            REQUIRE(fresh.Value().instances.size() == 65);
            CHECK_FALSE(fresh.Value().HasBrokenInstances());
            for (std::size_t index = 0; index < 64; ++index) {
                REQUIRE(fresh.Value().instances[index].debugSnapshot);
                CHECK_FALSE(fresh.Value().instances[index].debugCaptureFailure);
            }
            const auto &last = fresh.Value().instances.back();
            CHECK(last.debugCaptureOmitted);
            CHECK_FALSE(last.debugSnapshot);
            CHECK(last.IsSynchronized());
            CHECK_FALSE(last.debugCaptureFailure);
            Prefab::PrefabExpansionCache cache;
            const auto cached = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits(), cache);
            REQUIRE(cached.HasValue());
            CHECK(cached.Value().instances.back().debugCaptureOmitted);
            CHECK_FALSE(cached.Value().HasBrokenInstances());
            // Inspection status is outside canonical publication validation and cannot fail an otherwise valid conversion.
            auto retained = fresh.Value();
            retained.instances[0].debugSnapshot.reset();
            retained.instances[0].debugCaptureFailure = MakeError(Prefab::PrefabErrors::DiagnosticBudgetExceeded);
            CHECK(ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, {19}, {1}, retained,
                                                        {sources, Limits()})
                      .HasValue());
            JobSystem jobs;
            ScenePrefabExpansionOwner owner{jobs};
            ScenePrefabExpansionRequest request{1, {19}, {1}, document, sources, Limits()};
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(ExpansionTestSupport::TestJoin).HasValue());
            const auto completed = owner.TakeCompleted(request);
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            CHECK(completed.Value()->Entities().size() == 65);
            REQUIRE(owner.Shutdown(ExpansionTestSupport::TestJoin).HasValue());
        }

        TEST_CASE("Scene prefab capture allocation failure and cancellation preserve canonical expansion",
                  "[unit][scene][prefab][debug][lifecycle]") {
            const auto sources = Sources();
            const auto instance = Prefab::PrefabInstanceId::Create(9).Value();
            ScenePrefabInstanceProjection entry{.authored = {instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}},
                                                .expanded = sources.Resolve(Asset(), instance, Limits()).Value()};
            Detail::ScenePrefabDebugCaptureBudget budget;
            const auto limits = Limits();
            {
                Tests::AllocationProbe::ScopedFailure failure;
                Detail::ApplyInstanceValues(entry, nullptr, limits, budget);
            }
            REQUIRE(entry.debugCaptureFailure);
            CHECK(entry.debugCaptureFailure->code.Value() == Prefab::PrefabErrors::DiagnosticBudgetExceeded.code.Value());
            CHECK(entry.IsSynchronized());
            CHECK_FALSE(entry.failure);
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            Detail::ApplyInstanceValues(entry, nullptr, Limits(), budget, cancelled.Token());
            REQUIRE(entry.debugCaptureFailure);
            CHECK(entry.debugCaptureFailure->code.Value() == Prefab::PrefabErrors::Cancelled.code.Value());
            CHECK(entry.IsSynchronized());
            CHECK_FALSE(entry.failure);
            Detail::ApplyInstanceValues(entry, nullptr, Limits(), budget);
            CHECK(entry.debugSnapshot.has_value());
            CHECK_FALSE(entry.debugCaptureFailure);
        }

        TEST_CASE("Scene prefab debug capture retains canonical override conflict evidence", "[unit][scene][prefab][debug]") {
            const auto sources = Sources();
            const auto schemas = Schemas();
            const auto instance = Prefab::PrefabInstanceId::Create(9).Value();
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            auto values = Prefab::PreparePrefabValueEdit(source, std::nullopt, {Prefab::PrefabValueEditKind::Assign, Property(), false},
                                                         &schemas, Limits())
                              .Value()
                              .rebased;
            SceneSourceDocument document;
            document.prefabInstances.push_back({instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}, values});
            const auto resolved = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits(), &schemas);
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().instances[0].debugSnapshot);
            CHECK(resolved.Value().instances[0].debugSnapshot->Overrides().assignments == 1);
            CHECK_FALSE(resolved.Value().instances[0].failure);
            const auto orphan = BuildScenePrefabProjection({document.objects, document.prefabInstances}, sources, Limits());
            REQUIRE(orphan.HasValue());
            REQUIRE(orphan.Value().instances[0].debugSnapshot);
            CHECK(orphan.Value().instances[0].debugSnapshot->Overrides().orphans == 1);
            CHECK(orphan.Value().instances[0].IsBroken());
            CHECK_FALSE(orphan.Value().instances[0].debugCaptureFailure);
        }
    }  // namespace
}  // namespace Horo::SceneSource
