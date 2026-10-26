#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <thread>

namespace Horo::Tests::OpenXR {
    namespace {
        NativeDiscoveryRequest Discovery(const Fixture &fixture) {
            const auto &preflight = fixture.preflight;
            return {.selection = {.contractVersion = preflight.ContractVersion(),
                                  .attempt = preflight.Attempt(),
                                  .backend = preflight.Backend(),
                                  .installRecord = preflight.InstallRecord(),
                                  .productProfile = preflight.ProductProfile(),
                                  .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}},
                    .requestedApi = preflight.LoaderApiVersion()};
        }

        NativeSessionPreparation Preparation(const Fixture &fixture) {
            return {fixture.preflight, fixture.preflight.Attempt()};
        }

        /** @brief Mutates the original legacy request at a foreign graphics boundary, after its initial complete admission. */
        class MutatingGraphics final : public IOpenXRGraphicsBinding {
        public:
            MutatingGraphics(Graphics &graphics, NativeSessionRequest &request) : graphics_(graphics), request_(request) {}

            const char *RequiredExtension() const noexcept override {
                return graphics_.RequiredExtension();
            }

            Result<void> ValidatePreparation() const override {
                return graphics_.ValidatePreparation();
            }

            Result<void> Validate(const XRSessionId &candidate) const override {
                return graphics_.Validate(candidate);
            }

            Result<const XrBaseInStructure *> Prepare(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr dispatch) override {
                auto prepared = graphics_.Prepare(instance, system, dispatch);
                request_.candidate = {};
                return prepared;
            }

            void Release() noexcept override {
                graphics_.Release();
            }

        private:
            Graphics &graphics_;
            NativeSessionRequest &request_;
        };

        /** @brief Uses the actual canonical generation issuer; native preparation cannot choose its candidate. */
        class PreparedResources final : public IXRSessionResources {
        public:
            explicit PreparedResources(Fixture &fixture) : fixture_(fixture) {}

            Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &) override {
                if (stage != XRSessionPreparation::NativeSessionAndSwapchains)
                    return Result<void>::Success();
                CHECK_FALSE(fixture_.owner.Session().IsValid());
                auto request = fixture_.Request();
                request.candidate = candidate;
                return fixture_.owner.Activate(request);
            }

            void Release(XRSessionPreparation stage, const XRSessionId &) noexcept override {
                if (stage == XRSessionPreparation::NativeSessionAndSwapchains && fixture_.owner.Close().HasError())
                    std::terminate();
            }

        private:
            Fixture &fixture_;
        };
    }  // namespace

    TEST_CASE("OpenXR staged preparation activates the same instance system and session only after complete admission",
              "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        const auto discovered = fixture.owner.BorrowDiscovery(fixture.preflight.Attempt());
        REQUIRE(discovered.HasValue());
        CHECK(discovered.Value().acceptedApi == fixture.preflight.LoaderApiVersion());
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.script.liveSessions == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.owner.Borrow(fixture.Request().candidate).HasError());
        const auto prepared = fixture.owner.BorrowPreparedSession(Preparation(fixture));
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().instance == discovered.Value().instance);
        CHECK(prepared.Value().system == discovered.Value().system);
        CHECK(prepared.Value().maximumActions == 0);
        REQUIRE(fixture.owner.Activate(fixture.Request()).HasValue());
        const auto active = fixture.owner.Borrow(fixture.Request().candidate);
        REQUIRE(active.HasValue());
        CHECK(active.Value().session == prepared.Value().session);
        CHECK(active.Value().maximumActions == fixture.plan.Limits().maximumActions);
        CHECK(std::ranges::count(fixture.script.calls, "instance") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "session") == 1);
        CHECK(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
        CHECK(fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR observed system identity survives session retirement but not native rediscovery", "[unit][xr][native]") {
        Fixture fixture;
        CHECK(fixture.owner.PreparedSystem(Preparation(fixture)).HasError());
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        const auto first = fixture.owner.PreparedSystem(Preparation(fixture));
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().IsValid());
        CHECK(first.Value().runtime == fixture.preflight.RuntimeGeneration());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        REQUIRE(fixture.owner.RetireSession().HasValue());
        const auto retained = fixture.owner.PreparedSystem(Preparation(fixture));
        REQUIRE(retained.HasValue());
        CHECK(retained.Value() == first.Value());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.PreparedSystem(Preparation(fixture)).HasError());
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        const auto replacement = fixture.owner.PreparedSystem(Preparation(fixture));
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().slot.generation > first.Value().slot.generation);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR discovery rejects invalid bounded selection before opening native instance", "[unit][xr][native]") {
        Fixture fixture;
        auto discovery = Discovery(fixture);
        SECTION("cancelled") {
            discovery.selection.cancellationRequested = true;
        }
        SECTION("invalid attempt") {
            discovery.selection.attempt = {};
        }
        SECTION("unsupported compiled API") {
            discovery.requestedApi = {2, 0, 0};
        }
        SECTION("unapproved developer selection") {
            discovery.selection.runtimeSelection = XRRuntimeSelectionPolicy::ApprovedDeveloperOverride;
        }
        CHECK(fixture.owner.PrepareDiscovery(discovery).HasError());
        CHECK(fixture.script.calls.empty());
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR staged admission rejects stale attempt or changed installation before graphics creation", "[unit][xr][native]") {
        Fixture fixture;
        auto discovery = Discovery(fixture);
        auto preparation = Preparation(fixture);
        SECTION("changed installation") {
            discovery.selection.installRecord = IdentityValue<XRInstallRecordId>(44);
        }
        SECTION("stale attempt") {
            preparation.activeAttempt = IdentityValue<XRLoaderPreflightAttempt>(45);
        }
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        CHECK(fixture.owner.PrepareSession(preparation).HasError());
        CHECK(std::ranges::count(fixture.script.calls, "graphics") == 0);
        CHECK(fixture.script.liveSessions == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native API rejection never publishes accepted discovery facts", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.fail = "instance";
        fixture.script.failureResult = XR_ERROR_API_VERSION_UNSUPPORTED;
        const auto discovered = fixture.owner.PrepareDiscovery(Discovery(fixture));
        REQUIRE(discovered.HasError());
        CHECK(discovered.ErrorValue().message.find("xrCreateInstance") != std::string::npos);
        CHECK(discovered.ErrorValue().message.find(std::to_string(static_cast<std::int32_t>(XR_ERROR_API_VERSION_UNSUPPORTED))) !=
              std::string::npos);
        CHECK(fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasError());
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR failed discovery retains native instance debt until successful explicit close", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.fail = "system";
        fixture.script.failInstanceRetirement = true;
        CHECK(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasError());
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasError());
        const auto calls = fixture.script.calls.size();
        CHECK(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasError());
        CHECK(fixture.script.calls.size() == calls);
        fixture.script.failInstanceRetirement = false;
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR failed staged session creation retires graphics before the sole instance", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        fixture.script.fail = "session";
        CHECK(fixture.owner.PrepareSession(Preparation(fixture)).HasError());
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.script.calls.size() >= 2);
        CHECK(fixture.script.calls[fixture.script.calls.size() - 2] == "release-graphics");
        CHECK(fixture.script.calls.back() == "destroy-instance");
    }

    TEST_CASE("OpenXR failed final admission retains session graphics and instance debt for close retry", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        fixture.script.stale = true;
        fixture.script.failRetirement = true;
        CHECK(fixture.owner.Activate(fixture.Request()).HasError());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(std::ranges::count(fixture.script.calls, "release-graphics") == 0);
        CHECK(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
        fixture.script.failRetirement = false;
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR staged admission failure preserves prepared native parents without an implicit destroy", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        const auto prepared = fixture.owner.BorrowPreparedSession(Preparation(fixture));
        REQUIRE(prepared.HasValue());
        fixture.script.stale = true;
        REQUIRE(fixture.owner.Activate(fixture.Request()).HasError());
        REQUIRE_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.script.liveSessions == 1);
        REQUIRE(fixture.script.liveInstances == 1);
        REQUIRE(std::ranges::count(fixture.script.calls, "destroy-session") == 0);
        REQUIRE(std::ranges::count(fixture.script.calls, "destroy-instance") == 0);
        fixture.script.stale = false;
        SECTION("retry the same prepared candidate") {
            REQUIRE(fixture.owner.Activate(fixture.Request()).HasValue());
            const auto active = fixture.owner.Borrow(fixture.Request().candidate);
            REQUIRE(active.HasValue());
            REQUIRE(active.Value().instance == prepared.Value().instance);
            REQUIRE(active.Value().session == prepared.Value().session);
        }
        SECTION("abandon the session explicitly while retaining discovery") {
            REQUIRE(fixture.owner.RetireSession().HasValue());
            REQUIRE(fixture.script.liveSessions == 0);
            REQUIRE(fixture.script.liveInstances == 1);
            REQUIRE(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
            REQUIRE(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
        }
        SECTION("failed explicit teardown retains debt until an explicit retry") {
            fixture.script.failRetirement = true;
            REQUIRE(fixture.owner.RetireSession().HasError());
            REQUIRE(fixture.script.liveSessions == 1);
            REQUIRE(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
            REQUIRE(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
            fixture.script.failRetirement = false;
            REQUIRE(fixture.owner.RetireSession().HasValue());
            REQUIRE(fixture.script.liveSessions == 0);
            REQUIRE(std::ranges::count(fixture.script.calls, "destroy-session") == 2);
        }
        const auto retiredSessions = std::ranges::count(fixture.script.calls, "destroy-session");
        const bool sessionStillOwned = fixture.script.liveSessions != 0;
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(std::ranges::count(fixture.script.calls, "destroy-session") == retiredSessions + (sessionStillOwned ? 1 : 0));
        REQUIRE(std::ranges::count(fixture.script.calls, "destroy-instance") == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(std::ranges::count(fixture.script.calls, "destroy-instance") == 1);
    }

    TEST_CASE("OpenXR prepared native borrows reject foreign control lanes without native work", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        const auto calls = fixture.script.calls.size();
        bool rejected{};
        std::thread foreign{[&] {
            rejected = fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasError();
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(fixture.script.calls.size() == calls);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR prepared names retain bounded owner storage after request mutation", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        std::string name{"XR_KHR_composition_layer_depth"};
        std::array extensions{NativeExtensionRequest{name, true}};
        auto request = Discovery(fixture);
        request.extensions = extensions;
        REQUIRE(fixture.owner.PrepareDiscovery(request).HasValue());
        name.assign(name.size(), 'x');
        const auto borrowed = fixture.owner.BorrowDiscovery(fixture.preflight.Attempt());
        REQUIRE(borrowed.HasValue());
        REQUIRE(borrowed.Value().enabledExtensions.size() == 2);
        CHECK(std::string_view{borrowed.Value().enabledExtensions[1]} == "XR_KHR_composition_layer_depth");
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR attempt-owned native preparation binds only the canonical lifecycle issued session", "[unit][xr][native]") {
        Fixture fixture;
        PreparedResources resources{fixture};
        XRSessionLifecycle lifecycle{resources};
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        const auto prepared = fixture.owner.BorrowPreparedSession(Preparation(fixture));
        REQUIRE(prepared.HasValue());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(lifecycle.Snapshot().state == XRSessionState::Inactive);
        const auto activated =
            lifecycle.Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan);
        REQUIRE(activated.HasValue());
        CHECK(fixture.owner.Session() == activated.Value());
        CHECK(lifecycle.Snapshot().state == XRSessionState::Ready);
        const auto active = fixture.owner.Borrow(activated.Value());
        REQUIRE(active.HasValue());
        CHECK(active.Value().session == prepared.Value().session);
        CHECK(std::ranges::count(fixture.script.calls, "instance") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "session") == 1);
        CHECK(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
        auto reused = fixture.Request();
        reused.candidate = activated.Value();
        CHECK(fixture.owner.Activate(reused).HasError());
        lifecycle.Shutdown();
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR prepared session census borrow rejects stale attempt without losing owned native debt", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(Discovery(fixture)).HasValue());
        REQUIRE(fixture.owner.PrepareSession(Preparation(fixture)).HasValue());
        auto stale = Preparation(fixture);
        stale.activeAttempt = IdentityValue<XRLoaderPreflightAttempt>(99);
        const auto calls = fixture.script.calls.size();
        CHECK(fixture.owner.BorrowPreparedSession(stale).HasError());
        CHECK(fixture.script.calls.size() == calls);
        CHECK(fixture.script.liveSessions == 1);
        REQUIRE(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.BorrowPreparedSession(Preparation(fixture)).HasError());
    }

    TEST_CASE("OpenXR legacy Create rolls back early activation denial once and retains failed destruction debt", "[unit][xr][native]") {
        Fixture fixture;
        auto request = fixture.Request();
        MutatingGraphics graphics{fixture.graphics, request};
        OpenXRNativeSession legacy{fixture.library, graphics, fixture.fence};
        bool sessionDebt{}, instanceDebt{};
        SECTION("complete reverse cleanup") {}
        SECTION("session destroy debt") {
            sessionDebt = true;
            fixture.script.failRetirement = true;
        }
        SECTION("instance destroy debt") {
            instanceDebt = true;
            fixture.script.failInstanceRetirement = true;
        }
        RequireFailureIdentity(legacy.Create(request), XRErrors::IdentityStale);
        CHECK_FALSE(legacy.Session().IsValid());
        CHECK(std::ranges::count(fixture.script.calls, "session") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "destroy-instance") == (sessionDebt ? 0 : 1));
        CHECK(fixture.script.liveSessions == (sessionDebt ? 1 : 0));
        CHECK(fixture.script.liveInstances == (sessionDebt || instanceDebt ? 1 : 0));
        fixture.script.failRetirement = false;
        fixture.script.failInstanceRetirement = false;
        REQUIRE(legacy.Close().HasValue());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR
