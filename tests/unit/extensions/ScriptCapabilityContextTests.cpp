#include "Horo/Extensions/ScriptCapabilityContext.h"
#include "ScriptInvocationTestSupport.h"

#include <algorithm>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace Support;

    namespace {
        struct CapabilityFixture final {
            std::shared_ptr<ScriptInvocationRegistry> registry = std::make_shared<ScriptInvocationRegistry>();
            ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project};
            ScriptCapabilityScope runtime{ScriptCapabilityScopeKind::Runtime};
            ScriptCapabilityScope scene{ScriptCapabilityScopeKind::Scene};
            ScriptCapabilityScope operation{ScriptCapabilityScopeKind::Operation};
            ExtensionCapabilityAdmission admission;
            ScriptInvocationProviderRegistration provider;

            CapabilityFixture() : admission(MakeAdmission()), provider(MakeProvider()) {}

            ExtensionCapabilityAdmission MakeAdmission() {
                auto result = ExtensionCapabilityAdmission::Evaluate({.extensionId = "com.example.package",
                                                                      .moduleId = "com.example.module",
                                                                      .activationGeneration = 1,
                                                                      .capabilities = {{{"com.example.service"}, {{"service.read"}}}}},
                                                                     {.revision = 1,
                                                                      .knownPermissions = {{"service.read"}},
                                                                      .approvedPermissions = {{"service.read"}},
                                                                      .availableCapabilities = {{"com.example.service"}}});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            ScriptInvocationProviderRegistration MakeProvider() {
                auto result = registry->RegisterProvider({.generation = 1});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            ScriptCapabilityContextDescriptor Descriptor() {
                auto grant = admission.Grant({"com.example.service"});
                REQUIRE(grant.HasValue());
                return {.scriptId = "com.example.script",
                        .policyRevision = 7,
                        .profile = ScriptCapabilityProfile::Gameplay,
                        .packageVerified = true,
                        .packageTrusted = true,
                        .scriptTrusted = true,
                        .runtimeCompatible = true,
                        .packageCapabilities = {{"com.example.service"}},
                        .scriptCapabilities = {{"com.example.service"}},
                        .projectCapabilities = {{"com.example.service"}},
                        .contextCapabilities = {{"com.example.service"}},
                        .scopes = {project.Lease(), runtime.Lease(), scene.Lease(), operation.Lease()},
                        .imports = {{"com.example.api",
                                     {{1, 0, 0}, {1, 0, 0}},
                                     DescriptorSnapshot(),
                                     grant.Value(),
                                     provider.Lease(),
                                     ScriptCapabilityProfile::Gameplay,
                                     {"fetch"}}}};
            }
        };
    }  // namespace

    TEST_CASE("Script binding requires every trust and policy layer before publishing imports", "[Extensions][ScriptCapability]") {
        using Descriptor = ScriptCapabilityContextDescriptor;
        for (const auto trust :
             {&Descriptor::packageVerified, &Descriptor::packageTrusted, &Descriptor::scriptTrusted, &Descriptor::runtimeCompatible}) {
            CapabilityFixture fixture;
            auto descriptor = fixture.Descriptor();
            descriptor.*trust = false;
            RequireErrorCode(ScriptCapabilityContext::Create(fixture.registry, std::move(descriptor)), "script_invocation_unavailable");
        }
        for (const auto permissions : {&Descriptor::packageCapabilities, &Descriptor::scriptCapabilities, &Descriptor::projectCapabilities,
                                       &Descriptor::contextCapabilities}) {
            CapabilityFixture fixture;
            auto descriptor = fixture.Descriptor();
            (descriptor.*permissions).clear();
            RequireErrorCode(ScriptCapabilityContext::Create(fixture.registry, std::move(descriptor)), "script_invocation_unavailable");
        }
        CapabilityFixture fixture;
        auto descriptor = fixture.Descriptor();
        fixture.admission.Revoke();
        CHECK(ScriptCapabilityContext::Create(fixture.registry, std::move(descriptor)).HasError());
    }

    TEST_CASE("Script context rejects incompatible or malformed required import evidence", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto descriptor = fixture.Descriptor();
        SECTION("incompatible version") {
            descriptor.imports.front().versions.minimum = {2, 0, 0};
        }
        SECTION("missing API") {
            descriptor.imports.front().apiId = "com.example.missing";
        }
        SECTION("duplicate import") {
            descriptor.imports.push_back(descriptor.imports.front());
        }
        SECTION("private text is not identity") {
            descriptor.scriptId = "secret/path\ncredential";
        }
        SECTION("missing operation") {
            descriptor.imports.front().functions = {"missing"};
        }
        SECTION("duplicate operation") {
            descriptor.imports.front().functions = {"fetch", "fetch"};
        }
        SECTION("missing policy revision") {
            descriptor.policyRevision = 0;
        }
        RequireErrorCode(ScriptCapabilityContext::Create(fixture.registry, std::move(descriptor)), "script_invocation_unavailable");
    }

    TEST_CASE("Script binding limits profiles and requires gameplay lifetime evidence", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto descriptor = fixture.Descriptor();
        descriptor.scopes = {fixture.project.Lease()};
        CHECK(ScriptCapabilityContext::Create(fixture.registry, descriptor).HasError());
        descriptor.profile = ScriptCapabilityProfile::Tooling;
        CHECK(ScriptCapabilityContext::Create(fixture.registry, descriptor).HasError());
        descriptor.imports.front().profile = ScriptCapabilityProfile::Tooling;
        REQUIRE(ScriptCapabilityContext::Create(fixture.registry, descriptor).HasValue());
        descriptor.scopes.clear();
        CHECK(ScriptCapabilityContext::Create(fixture.registry, descriptor).HasError());
    }

    TEST_CASE("Script bindings reject undeclared imports and preserve safe async identity", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
        REQUIRE(context.HasValue());
        RequireErrorCode(context.Value().Bind("com.example.undeclared"), "script_invocation_unavailable");
        auto binding = context.Value().Bind("com.example.api");
        REQUIRE(binding.HasValue());
        auto deniedFunction = Request(DescriptorSnapshot());
        deniedFunction.functionId = "undeclared";
        CHECK(binding.Value().Begin(fixture.provider, std::move(deniedFunction)).HasError());
        auto wrong = fixture.registry->RegisterProvider({.generation = 2});
        REQUIRE(wrong.HasValue());
        CHECK(binding.Value().Begin(wrong.Value(), {}).HasError());
        auto request = Request(DescriptorSnapshot());
        request.apiId = "secret";
        request.diagnostics.scriptId = "secret";
        auto invocation = binding.Value().Begin(fixture.provider, std::move(request), 42);
        REQUIRE(invocation.HasValue());
        const auto snapshot = invocation.Value().Handle().Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->diagnostics.packageId == "com.example.package");
        CHECK(snapshot->diagnostics.moduleId == "com.example.module");
        CHECK(snapshot->diagnostics.scriptId == "com.example.script");
        CHECK(snapshot->diagnostics.apiId == "com.example.api");
        CHECK(snapshot->diagnostics.policyRevision == 7);
        CHECK(snapshot->diagnostics.operationId == 42);
        REQUIRE(invocation.Value().Complete(ScriptCallResult::Success({ScriptValue::String("secret result")})).HasValue());
        auto events = context.Value().Drain();
        REQUIRE(events.HasValue());
        REQUIRE(events.Value().size() == 1);
        CHECK(events.Value().front().diagnostics == snapshot->diagnostics);
    }

    TEST_CASE("Script async worker logs inherit only safe invocation identity", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
        REQUIRE(context.HasValue());
        auto binding = context.Value().Bind("com.example.api");
        REQUIRE(binding.HasValue());
        auto invocation = binding.Value().Begin(fixture.provider, Request(DescriptorSnapshot()), 42);
        REQUIRE(invocation.HasValue());
        const auto operation = invocation.Value().Request()->diagnostics.CaptureOperationContext();
        Telemetry::OperationContext forwarded;
        std::thread worker([&forwarded, operation] {
            const Telemetry::ScopedOperationContext logBinding(operation);
            forwarded = Telemetry::CaptureOperationContext();
        });
        worker.join();
        CHECK(forwarded.operationId == 42);
        const auto fields = forwarded.diagnosticContext.Fields();
        CHECK(fields.size() == 6);
        CHECK(std::ranges::find(fields, Log::MdcField{"package.id", "com.example.package"}) != fields.end());
        CHECK(std::ranges::find(fields, Log::MdcField{"module.id", "com.example.module"}) != fields.end());
        CHECK(std::ranges::find(fields, Log::MdcField{"script.id", "com.example.script"}) != fields.end());
        CHECK(std::ranges::find(fields, Log::MdcField{"script.api_id", "com.example.api"}) != fields.end());
        CHECK(std::ranges::find(fields, Log::MdcField{"operation.id", "42"}) != fields.end());
        CHECK(std::ranges::find(fields, Log::MdcField{"policy.revision", "7"}) != fields.end());
    }

    TEST_CASE("Script operation allowlists deny declared functions without widening installed provider authority",
              "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto descriptor = fixture.Descriptor();
        auto api = MakeDescriptor();
        auto otherFunction = api.functions.front();
        otherFunction.id = "other";
        api.functions.push_back(otherFunction);
        auto exports = BuildScriptExportDescriptorSnapshot(std::array{api});
        REQUIRE(exports.HasValue());
        descriptor.imports.front().descriptors = exports.Value();
        auto context = ScriptCapabilityContext::Create(fixture.registry, descriptor);
        REQUIRE(context.HasValue());
        auto binding = context.Value().Bind(api.id);
        REQUIRE(binding.HasValue());
        auto request = Request(exports.Value());
        request.functionId = "other";
        RequireErrorCode(binding.Value().Begin(fixture.provider, request), "script_invocation_unavailable");
        descriptor.imports.front().functions.clear();
        auto emptyOperations = ScriptCapabilityContext::Create(fixture.registry, descriptor);
        REQUIRE(emptyOperations.HasValue());
        auto emptyBinding = emptyOperations.Value().Bind(api.id);
        REQUIRE(emptyBinding.HasValue());
        CHECK(emptyBinding.Value().Begin(fixture.provider, Request(exports.Value())).HasError());
        descriptor.imports.clear();
        auto emptyImports = ScriptCapabilityContext::Create(fixture.registry, descriptor);
        REQUIRE(emptyImports.HasValue());
        CHECK(emptyImports.Value().Bind(api.id).HasError());
    }

    TEST_CASE("Script binding revocation preserves completed results and suppresses expired scope delivery",
              "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
        REQUIRE(context.HasValue());
        auto binding = context.Value().Bind("com.example.api");
        REQUIRE(binding.HasValue());
        auto invocation = binding.Value().Begin(fixture.provider, Request(DescriptorSnapshot()));
        REQUIRE(invocation.HasValue());
        const auto handle = invocation.Value().Handle();
        REQUIRE(invocation.Value().Complete(ScriptCallResult::Success({ScriptValue::String("committed")})).HasValue());
        fixture.scene.Revoke();
        CHECK(handle.Snapshot()->state == ScriptInvocationStateKind::Completed);
        CHECK(context.Value().Drain().HasError());
        CHECK_FALSE(binding.Value().IsUsable());
    }

    TEST_CASE("Every script binding lifecycle owner revokes new calls and cancels late completion", "[Extensions][ScriptCapability]") {
        for (int owner = 0; owner < 7; ++owner) {
            CapabilityFixture fixture;
            auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
            REQUIRE(context.HasValue());
            auto binding = context.Value().Bind("com.example.api");
            REQUIRE(binding.HasValue());
            auto invocation = binding.Value().Begin(fixture.provider, Request(DescriptorSnapshot()));
            REQUIRE(invocation.HasValue());
            const auto handle = invocation.Value().Handle();
            CHECK(handle.Snapshot()->diagnostics.operationId == handle.Id().value);
            switch (owner) {
                case 0:
                    fixture.project.Revoke();
                    break;
                case 1:
                    fixture.runtime.Revoke();
                    break;
                case 2:
                    fixture.scene.Revoke();
                    break;
                case 3:
                    fixture.operation.Revoke();
                    break;
                case 4:
                    fixture.admission.Revoke();
                    break;
                case 5:
                    context.Value().Reset();
                    break;
                case 6:
                    fixture.registry->BeginShutdown();
                    break;
            }
            CHECK(binding.Value().Begin(fixture.provider, Request(DescriptorSnapshot())).HasError());
            REQUIRE(invocation.Value().Complete(ScriptCallResult::Success({ScriptValue::String("late")})).HasValue());
            CHECK(handle.Snapshot()->state == ScriptInvocationStateKind::Cancelled);
            CHECK(invocation.Value().Cancellation().IsCancellationRequested());
        }
    }

    TEST_CASE("Provider registration replacement cannot revive an expired script binding", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
        REQUIRE(context.HasValue());
        auto binding = context.Value().Bind("com.example.api");
        REQUIRE(binding.HasValue());
        fixture.provider.Reset();
        auto replacement = fixture.registry->RegisterProvider({.generation = 1});
        REQUIRE(replacement.HasValue());
        CHECK_FALSE(binding.Value().IsUsable());
        CHECK(binding.Value().Begin(replacement.Value(), Request(DescriptorSnapshot())).HasError());
        CHECK(context.Value().Bind("com.example.api").HasError());
    }

    TEST_CASE("Script context and scope destruction revoke retained bindings", "[Extensions][ScriptCapability]") {
        CapabilityFixture fixture;
        auto context = ScriptCapabilityContext::Create(fixture.registry, fixture.Descriptor());
        REQUIRE(context.HasValue());
        auto binding = context.Value().Bind("com.example.api");
        REQUIRE(binding.HasValue());
        {
            auto moved = std::move(context).Value();
            CHECK(binding.Value().IsUsable());
        }
        CHECK_FALSE(binding.Value().IsUsable());
        auto scope = std::make_unique<ScriptCapabilityScope>(ScriptCapabilityScopeKind::Project);
        const auto lease = scope->Lease();
        scope.reset();
        CHECK_FALSE(lease.IsUsable());
    }
}  // namespace Horo::Extensions::Tests
