#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    TEST_CASE("Actual materialization recipes reuse canonical fingerprints without retaining authority",
              "[Extensions][ScriptModule][Cache]") {
        Fixture fixture;
        ScriptModuleBindingCache cache;
        auto binding = fixture.Binding();
        const auto runtime = LuaScriptModuleRuntimeIdentity("headless");
        auto first = Take(cache.Acquire(runtime, *binding, fixture.Policy()));
        auto repeated = Take(cache.Acquire(runtime, *binding, fixture.Policy()));
        CHECK(first == repeated);
        CHECK(cache.Stats().hits == 1);
        CHECK(cache.Stats().misses == 1);
        CHECK(first->functionIndices.size() == 4);
        fixture.provider->Reset();
        CHECK_FALSE(binding->IsUsable());
        cache.Invalidate(fixture.schema->Fingerprint());
        CHECK(cache.Stats().entries == 0);
        CHECK_FALSE(binding->IsUsable());
        CHECK(first->descriptorFingerprint == fixture.schema->Fingerprint());
    }

    TEST_CASE("Cache keys distinguish real VM identities and exact admitted function projections", "[Extensions][ScriptModule][Cache]") {
        Fixture fixture;
        ScriptModuleBindingCache cache;
        auto binding = fixture.Binding();
        auto lua = Take(cache.Acquire(LuaScriptModuleRuntimeIdentity("headless"), *binding, fixture.Policy()));
        auto js = Take(cache.Acquire(JavaScriptModuleRuntimeIdentity("headless"), *binding, fixture.Policy()));
        CHECK(lua->key != js->key);
        auto restricted = Take(BindScriptRuntimeBackend<ObservedService>(fixture.Input({"sum"}), &ObservedService::Invoke));
        auto minimal = Take(cache.Acquire(LuaScriptModuleRuntimeIdentity("headless"), *restricted, fixture.Policy()));
        CHECK(minimal->key != lua->key);
        CHECK(minimal->functionIndices.size() == 1);
        CHECK(cache.Stats().entries == 3);
    }

    TEST_CASE("Pinned cache capacity returns an equivalent caller-owned plan without changing residency",
              "[Extensions][ScriptModule][Cache]") {
        Fixture fixture;
        ScriptModuleBindingCache cache{1, 1024U * 1024U};
        auto binding = fixture.Binding();
        const auto lua = LuaScriptModuleRuntimeIdentity("headless"), js = JavaScriptModuleRuntimeIdentity("headless");
        auto pinned = Take(cache.Acquire(lua, *binding, fixture.Policy()));
        const auto before = cache.Stats();
        auto candidate = Take(cache.Acquire(js, *binding, fixture.Policy()));
        CHECK(candidate->runtime == js);
        CHECK(candidate->descriptorFingerprint == pinned->descriptorFingerprint);
        CHECK(candidate->functionIndices == pinned->functionIndices);
        CHECK(candidate->namespaceSegments == pinned->namespaceSegments);
        CHECK(cache.Stats().entries == 1);
        CHECK(cache.Stats().retainedBytes == before.retainedBytes);
        CHECK(pinned->runtime == lua);
        CHECK(cache.Stats().retainedBytes <= 1024U * 1024U);
        std::weak_ptr<const ScriptModuleBindingPlan> uncached = candidate;
        candidate.reset();
        CHECK(uncached.expired());
        auto samePinned = Take(cache.Acquire(lua, *binding, fixture.Policy()));
        CHECK(samePinned == pinned);
        pinned.reset();
        samePinned.reset();
        auto admitted = Take(cache.Acquire(js, *binding, fixture.Policy()));
        CHECK(cache.Stats().entries == 1);
        CHECK(cache.Stats().retainedBytes == admitted->retainedBytes);
        CHECK(cache.Stats().retainedBytes <= 1024U * 1024U);
        auto reused = Take(cache.Acquire(js, *binding, fixture.Policy()));
        CHECK(reused == admitted);
        CHECK(cache.Stats().hits == 2);
    }
}  // namespace Horo::Extensions::Tests
