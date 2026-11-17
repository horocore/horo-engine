#include "ScriptModuleRuntimeFixture.h"

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    TEST_CASE("Module port rejects invented AnyThread before job admission or provider entry", "[Extensions][ScriptModule]") {
        Fixture fixture{BackendServiceThreadRule::ProviderOwnerThread};
        auto input = fixture.Input();
        input.service.threadRule = BackendServiceThreadRule::AnyThread;
        auto binding = BindScriptRuntimeBackend<ObservedService>(std::move(input), &ObservedService::Invoke);
        REQUIRE(binding.HasError());
        CHECK(binding.ErrorValue().code.Value() == ExtensionErrors::ScriptInvocationInvalid.code.Value());
        CHECK(fixture.audit->calls == 0);
    }

    TEST_CASE("Module port permits genuine owner-only synchronous profile and checks every actual generation",
              "[Extensions][ScriptModule]") {
        Fixture fixture{BackendServiceThreadRule::ProviderOwnerThread};
        auto input = fixture.Input({"sum"});
        auto binding = Take(BindScriptRuntimeBackend<ObservedService>(std::move(input), &ObservedService::Invoke));
        CHECK(binding->IsUsable());
        fixture.provider->Reset();
        CHECK_FALSE(binding->IsUsable());
        CHECK(fixture.audit->calls == 0);
    }

    TEST_CASE("Actual backend descriptor remains diagnostic-only and consumption clears its borrow", "[Extensions][ScriptModule]") {
        Fixture fixture;
        auto input = fixture.Input();
        const auto &consumer = input.consumerCapability.Activation();
        auto authority =
            Take(fixture.capabilities->ResolveExact(input.consumerCapability, {{1, 0, 0}, {1, 0, 0}}, fixture.descriptor.provider,
                                                    consumer.ExtensionId(), consumer.ModuleId(), consumer.Generation()));
        auto imported = Take(fixture.services->BindImport(std::move(authority), input.import));
        auto call = Take(fixture.services->ResolveImported<ObservedService>(std::move(imported)));
        REQUIRE(call.Descriptor());
        CHECK(call.Descriptor()->threadRule == BackendServiceThreadRule::AnyThread);
        auto moved = std::move(call);
        CHECK(call.Descriptor() == nullptr);
        REQUIRE(moved.Descriptor());
        fixture.consumer.Revoke();
        auto denied = std::move(moved).Invoke(&ObservedService::Invoke, ScriptInvocationRequest{});
        CHECK(denied.HasError());
        CHECK(moved.Descriptor() == nullptr);
        CHECK(fixture.audit->calls == 0);
    }
}  // namespace Horo::Extensions::Tests
