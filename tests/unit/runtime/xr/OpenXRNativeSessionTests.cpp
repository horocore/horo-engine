#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <array>
#include <string>

namespace Horo::Tests::OpenXR {
    TEST_CASE("OpenXR native transaction publishes only complete ownership and retires in reverse order", "[unit][xr][native]") {
        Fixture fixture;
        const auto request = fixture.Request();
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.owner.Session() == request.candidate);
        CHECK(fixture.owner.HasExtension("XR_KHR_vulkan_enable2"));
        REQUIRE(fixture.owner.Validate(request.candidate).HasValue());
        const auto borrowed = fixture.owner.Borrow(request.candidate);
        REQUIRE(borrowed.HasValue());
        CHECK(borrowed.Value().instance != XR_NULL_HANDLE);
        CHECK(borrowed.Value().session != XR_NULL_HANDLE);
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.calls.size() >= 3);
        const auto end = fixture.script.calls.end();
        CHECK(*(end - 3) == "destroy-session");
        CHECK(*(end - 2) == "release-graphics");
        CHECK(*(end - 1) == "destroy-instance");
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.owner.Borrow(request.candidate).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native failures at every acquisition stage roll back without publication", "[unit][xr][native]") {
        for (const auto phase : {"layers", "extensions", "instance", "system", "graphics", "session"}) {
            Fixture fixture;
            fixture.script.fail = phase;
            INFO(phase);
            CHECK(fixture.owner.Create(fixture.Request()).HasError());
            CHECK_FALSE(fixture.owner.Session().IsValid());
            CHECK(fixture.script.liveSessions == 0);
            CHECK(fixture.script.liveInstances == 0);
        }
    }

    TEST_CASE("OpenXR missing bootstrap or instance dispatch never strands native ownership", "[unit][xr][native]") {
        for (const auto symbol :
             {"xrGetInstanceProcAddr", "xrDestroyInstance", "xrCreateInstance", "xrDestroySession", "xrGetSystem", "xrCreateSession"}) {
            Fixture fixture;
            fixture.script.absent = symbol;
            INFO(symbol);
            CHECK(fixture.owner.Create(fixture.Request()).HasError());
            CHECK(fixture.script.liveInstances == 0);
            CHECK(fixture.script.liveSessions == 0);
        }
    }

    TEST_CASE("OpenXR stale device evidence is rejected before creation and after session acquisition", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("before any native work") {
            fixture.script.stale = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapabilityStale);
            CHECK(fixture.script.calls.empty());
        }
        SECTION("before publication") {
            fixture.script.staleAfterSession = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapabilityStale);
            CHECK(fixture.script.liveSessions == 0);
            CHECK(fixture.script.liveInstances == 0);
        }
        SECTION("retained owner") {
            REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
            fixture.script.stale = true;
            RequireFailureIdentity(fixture.owner.Validate(fixture.Request().candidate), XRErrors::CapabilityStale);
        }
    }

    TEST_CASE("OpenXR bounded negotiation rejects overflow malformed publication and unsupported requirements", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("native publication overflow") {
            fixture.script.extensionCount = MaximumNativeExtensions + 1;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapacityExceeded);
        }
        SECTION("corrupt native metadata") {
            fixture.script.malformedExtension = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::OperationInvalid);
        }
        SECTION("required missing extension") {
            const std::array extensions{NativeExtensionRequest{"XR_TEST_missing", true}};
            auto request = fixture.Request();
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::OperationUnsupported);
        }
        SECTION("optional missing extension is observable disablement") {
            const std::array extensions{NativeExtensionRequest{"XR_TEST_missing", false}};
            auto request = fixture.Request();
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            CHECK_FALSE(fixture.owner.HasExtension("XR_TEST_missing"));
            REQUIRE(fixture.owner.Close().HasValue());
        }
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR extension evidence separates discovery version compatibility and missing dependencies", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        auto request = fixture.Request();
        SECTION("discovered but unrequested") {
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::Discovered);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(evidence.Value().minimumVersion == 0);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        SECTION("optional request with a newer minimum version") {
            const std::array extensions{NativeExtensionRequest{.name = depth, .minimumVersion = 2}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::VersionIncompatible);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(evidence.Value().minimumVersion == 2);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        SECTION("required request with a newer minimum version") {
            const std::array extensions{NativeExtensionRequest{.name = depth, .required = true, .minimumVersion = 2}};
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::OperationIncompatible);
            CHECK(fixture.script.liveInstances == 0);
        }
        SECTION("the declared core route suppresses a duplicate advertised extension") {
            const std::array extensions{
                NativeExtensionRequest{.name = depth, .required = true, .promotedToCore = request.preflight.LoaderApiVersion()}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::PromotedToCore);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(fixture.script.enabledExtensions.size() == 1);
        }
        SECTION("advertised feature depends on a missing request") {
            constexpr std::array dependencies{std::string_view{"XR_TEST_missing"}};
            const std::array extensions{NativeExtensionRequest{.name = depth, .dependencies = dependencies}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth).Value().state ==
                  NativeExtensionState::DependencyMissing);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth).HasError());
    }

    TEST_CASE("OpenXR core promotion admits a declared dependency without enabling its extension name", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        constexpr std::string_view promoted = "XR_TEST_promoted";
        constexpr std::array needsCore{promoted};
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = depth, .required = true, .dependencies = needsCore},
                                    NativeExtensionRequest{.name = promoted,
                                                           .required = true,
                                                           .promotedToCore = request.preflight.LoaderApiVersion()}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const auto core = fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted);
        REQUIRE(core.HasValue());
        CHECK(core.Value().state == NativeExtensionState::PromotedToCore);
        CHECK(core.Value().discoveredVersion == 0);
        CHECK_FALSE(fixture.owner.HasExtension(promoted));
        CHECK(fixture.owner.HasExtension(depth));
        CHECK(fixture.owner.ReadExtensionAdmission(request.candidate, depth).Value().state == NativeExtensionState::Enabled);
        CHECK(std::ranges::find(fixture.script.enabledExtensions, promoted) == fixture.script.enabledExtensions.end());
        CHECK(std::ranges::find(fixture.script.enabledExtensions, depth) != fixture.script.enabledExtensions.end());
        REQUIRE(fixture.owner.RetireSession().HasValue());
        CHECK_FALSE(fixture.owner.HasExtension(depth));
        CHECK(fixture.owner.ReadExtensionAdmission(request.candidate, depth).HasError());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted).Value().state == NativeExtensionState::PromotedToCore);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted).HasError());
    }

    TEST_CASE("OpenXR cannot admit optional dependency cycles", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view first = "XR_TEST_first";
        constexpr std::string_view second = "XR_TEST_second";
        constexpr std::array firstNeeds{second};
        constexpr std::array secondNeeds{first};
        auto request = fixture.Request();
        const auto core = request.preflight.LoaderApiVersion();
        const std::array extensions{NativeExtensionRequest{.name = first, .dependencies = firstNeeds, .promotedToCore = core},
                                    NativeExtensionRequest{.name = second, .dependencies = secondNeeds, .promotedToCore = core}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, first).Value().state == NativeExtensionState::DependencyMissing);
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, second).Value().state == NativeExtensionState::DependencyMissing);
        CHECK(fixture.script.enabledExtensions.size() == 1);
    }

    TEST_CASE("OpenXR invalid graphics binding and host exception roll back the instance", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("native type mismatch") {
            fixture.graphics.binding.type = XR_TYPE_UNKNOWN;
        }
        SECTION("unrelated chain") {
            fixture.graphics.binding.next = &fixture.graphics.binding;
        }
        SECTION("host exception") {
            fixture.script.throwAfterInstance = true;
        }
        SECTION("non-standard host exception") {
            fixture.script.throwUnknownAfterInstance = true;
        }
        CHECK(fixture.owner.Create(fixture.Request()).HasError());
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR failed retirement remains explicit and retains the loader until a successful retry", "[unit][xr][native]") {
        bool unloaded{};
        Fixture fixture;
        fixture.library->destroyed = &unloaded;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.library.reset();
        fixture.script.failRetirement = true;
        REQUIRE(fixture.owner.Close().HasError());
        CHECK_FALSE(unloaded);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.owner.Create(fixture.Request(2)).HasError());
        fixture.script.failRetirement = false;
        REQUIRE(fixture.owner.Close().HasValue());
        // The owner intentionally retains the verified library for safe future activation.
        CHECK_FALSE(unloaded);
    }

    TEST_CASE("OpenXR owner reuse rejects retired generations and admits only an explicit new candidate", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        CHECK(fixture.owner.Create(fixture.Request(2)).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
        RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::IdentityStale);
        REQUIRE(fixture.owner.Create(fixture.Request(2)).HasValue());
        RequireFailureIdentity(fixture.owner.Validate(fixture.Request().candidate), XRErrors::IdentityStale);
    }

    TEST_CASE("OpenXR replacement is separately owned and candidate failure cannot retire the active session", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        Graphics replacementGraphics;
        OpenXRNativeSession replacement{fixture.library, replacementGraphics, fixture.fence};
        fixture.script.fail = "session";
        CHECK(replacement.Create(fixture.Request(2)).HasError());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        REQUIRE(fixture.owner.Validate(fixture.Request().candidate).HasValue());
        fixture.script.fail = {};
        REQUIRE(replacement.Create(fixture.Request(3)).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveSessions == 1);
        REQUIRE(replacement.Validate(fixture.Request(3).candidate).HasValue());
    }

    TEST_CASE("OpenXR malformed policy and shipping API layers fail before object creation", "[unit][xr][native]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("invalid owner") {
            request.candidate = {};
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::IdentityInvalid);
        }
        SECTION("overcapacity requests") {
            const std::array<NativeExtensionRequest, MaximumEnabledExtensions> extensions{};
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::CapacityExceeded);
        }
        SECTION("embedded nul") {
            const std::array extensions{NativeExtensionRequest{std::string_view{"XR_BAD\0NAME", 11}, true}};
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::OperationInvalid);
        }
        SECTION("shipping layers cannot be enabled by approval alone") {
            const std::array<std::string_view, 1> layers{"XR_APILAYER_TEST"};
            request.layers = layers;
            request.developmentLayersApproved = true;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::RuntimeOverrideRejected);
        }
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR corrupted instance retirement dispatch rolls back through the secured loader export", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.instanceDispatchAbsent = "xrDestroyInstance";
        REQUIRE(fixture.owner.Create(fixture.Request()).HasError());
        CHECK(std::ranges::find(fixture.script.calls, "instance") != fixture.script.calls.end());
        CHECK(std::ranges::find(fixture.script.calls, "destroy-instance") != fixture.script.calls.end());
        CHECK(fixture.script.liveInstances == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
    }

    TEST_CASE("OpenXR failed instance retirement retains only the remaining native owner for retry", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.script.fail = "destroy-instance";
        REQUIRE(fixture.owner.Close().HasError());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
        CHECK(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "release-graphics") == 1);
    }

    TEST_CASE("OpenXR explicit development layers contribute required extensions before instance activation", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.advertiseLayer = true;
        const std::array<std::string_view, 1> layers{"XR_APILAYER_TEST"};
        const std::array extensions{NativeExtensionRequest{"XR_TEST_layer_extension", true}};
        auto request = fixture.Request();
        request.layers = layers;
        request.extensions = extensions;
        request.developmentLayersApproved = true;
        request.productMode = XRPreflightProductMode::Development;
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.script.enabledLayer);
        CHECK(fixture.owner.HasExtension("XR_TEST_layer_extension"));
    }

    TEST_CASE("OpenXR absent loader fails explicitly without native acquisition or fallback", "[unit][xr][native]") {
        Fixture fixture;
        OpenXRNativeSession absent{nullptr, fixture.graphics, fixture.fence};
        RequireFailureIdentity(absent.Create(fixture.Request()), XRErrors::LoaderAbsent);
        CHECK(fixture.script.calls.empty());
        CHECK_FALSE(absent.Session().IsValid());
    }

    TEST_CASE("OpenXR native system failure preserves category phase and numeric API context", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.fail = "system";
        fixture.script.failureResult = XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        const auto result = fixture.owner.Create(fixture.Request());
        RequireFailureIdentity(result, XRErrors::SystemUnsupported);
        CHECK(result.ErrorValue().message.find("xrGetSystem") != std::string::npos);
        CHECK(result.ErrorValue().message.find(std::to_string(XR_ERROR_FORM_FACTOR_UNSUPPORTED)) != std::string::npos);
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR native result translation preserves every mapped category", "[unit][xr][native]") {
        struct Mapping {
            XrResult result;
            const ErrorCodeDescriptor *error;
        };

        const std::array mappings{Mapping{XR_ERROR_RUNTIME_UNAVAILABLE, &XRErrors::RuntimeUnavailable},
                                  Mapping{XR_ERROR_FORM_FACTOR_UNAVAILABLE, &XRErrors::SystemTemporarilyUnavailable},
                                  Mapping{XR_ERROR_FORM_FACTOR_UNSUPPORTED, &XRErrors::SystemUnsupported},
                                  Mapping{XR_ERROR_PERMISSION_INSUFFICIENT, &XRErrors::PermissionDenied},
                                  Mapping{XR_ERROR_EXTENSION_NOT_PRESENT, &XRErrors::OperationUnsupported},
                                  Mapping{XR_ERROR_API_LAYER_NOT_PRESENT, &XRErrors::OperationUnsupported},
                                  Mapping{XR_ERROR_RUNTIME_FAILURE, &XRErrors::OperationUnavailable}};
        for (const auto &mapping : mappings) {
            Fixture fixture;
            fixture.script.fail = "system";
            fixture.script.failureResult = mapping.result;
            const auto result = fixture.owner.Create(fixture.Request());
            RequireFailureIdentity(result, *mapping.error);
            CHECK(result.ErrorValue().message.find("xrGetSystem failed; native result=") == 0);
            CHECK(result.ErrorValue().message.find(std::to_string(mapping.result)) != std::string::npos);
            CHECK(fixture.script.liveInstances == 0);
            CHECK_FALSE(fixture.owner.Session().IsValid());
        }
    }

    TEST_CASE("OpenXR host exception preserves failed rollback ownership and its cause", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("standard exception") {
            fixture.script.throwAfterInstance = true;
        }
        SECTION("non-standard exception") {
            fixture.script.throwUnknownAfterInstance = true;
        }
        fixture.script.fail = "destroy-instance";
        const auto result = fixture.owner.Create(fixture.Request());
        RequireFailureIdentity(result, XRErrors::OperationUnavailable);
        REQUIRE(result.ErrorValue().cause.Get() != nullptr);
        CHECK(result.ErrorValue().cause.Get()->message.find("xrDestroyInstance failed; native result=") == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.script.liveSessions == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR plan admission preserves stale system and capacity errors before native work", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("replaced system") {
            auto request = fixture.Request();
            ++request.candidate.system.slot.generation;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::IdentityStale);
        }
        SECTION("capacity no longer fits retained plan") {
            fixture.capabilities = Capabilities(4);
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapacityExceeded);
        }
        CHECK(fixture.script.calls.empty());
        CHECK(fixture.script.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR
