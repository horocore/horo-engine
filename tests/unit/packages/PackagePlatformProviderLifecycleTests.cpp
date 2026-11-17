#include "PackagePlatformProviderTestSupport.h"

namespace Horo::Packages::Tests {
    using namespace ProviderSupport;

    TEST_CASE("Verified platform packages publish only after explicit trusted activation", "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.Install();
            CHECK(fixture.images.empty());
            CHECK(fixture.generations.empty());
            REQUIRE(fixture.Activate().HasValue());
            REQUIRE(fixture.generations.size() == 1);
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            REQUIRE(host->Close().HasValue());
            const auto stopped = fixture.images.front()->Snapshot();
            CHECK(stopped.servicesStopped == 1);
            CHECK(stopped.sessionStopped == 1);
            CHECK(stopped.sinkRetained == 0);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CHECK(fixture.Start(0).HasError());
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("Provider package trust stale approvals mixed claims and absent host reject before native loading",
              "[packages][platform-services][provider]") {
        for (const unsigned failure : {0U, 1U, 2U, 3U}) {
            ProviderFixture fixture{false, failure != 3};
            fixture.Install(failure == 2);
            fixture.base.trust.deny = failure == 0;
            fixture.base.trust.stale = failure == 1;
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.images.empty());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
        }
    }

    TEST_CASE("Malformed and unapproved package provider claims cannot publish a capability", "[packages][platform-services][provider]") {
        for (const std::uint32_t flags : {HORO_PACKAGE_PROVIDER_MALFORMED, HORO_PACKAGE_PROVIDER_UNAPPROVED,
                                          HORO_PACKAGE_PROVIDER_WRONG_PROFILE, HORO_PACKAGE_PROVIDER_INCOMPATIBLE_ABI}) {
            ProviderFixture fixture;
            fixture.flags = flags;
            fixture.Install();
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
            REQUIRE(fixture.images.size() == 1);
            CHECK(fixture.images.front()->released);
            CHECK(fixture.images.front()->Snapshot().created == 0);
        }
    }

    TEST_CASE("Package approval bounds provider permissions even when the process policy is broader",
              "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.flags = HORO_PACKAGE_PROVIDER_UNAPPROVED;
            fixture.rootPolicy.knownPermissions = {{"platform.services.private"}};
            fixture.rootPolicy.approvedPermissions = fixture.rootPolicy.knownPermissions;
            fixture.Install();
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
            REQUIRE(fixture.images.size() == 1);
            CHECK(fixture.images.front()->released);
            CHECK(fixture.images.front()->Snapshot().created == 0);
        }
    }

    TEST_CASE("Unavailable SDK and partial candidate creation release package resources without fallback",
              "[packages][platform-services][provider]") {
        for (const std::uint32_t flags : {HORO_PACKAGE_PROVIDER_SDK_UNAVAILABLE, HORO_PACKAGE_PROVIDER_PARTIAL_CREATE}) {
            ProviderFixture fixture;
            fixture.flags = flags;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            CHECK(fixture.Start(0).HasError());
            CHECK(fixture.images.front()->Snapshot().created == 1);
            CHECK(fixture.images.front()->Snapshot().destroyed == 1);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("Exact package and process approvals jointly admit a provider permission", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_UNAPPROVED;
        fixture.rootPolicy.knownPermissions = {{"platform.services.private"}};
        fixture.rootPolicy.approvedPermissions = fixture.rootPolicy.knownPermissions;
        fixture.base.trust.permissions = fixture.rootPolicy.knownPermissions;
        fixture.base.trust.approvals = fixture.base.trust.permissions;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        auto started = fixture.Start(0);
        REQUIRE(started.HasValue());
        REQUIRE(started.Value()->Close().HasValue());
        fixture.base.service->Shutdown();
        fixture.Finalize();
        CheckReleased(*fixture.images.front());
    }

    TEST_CASE("Package replacement retains revoked native work and verified files until owner drain",
              "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.flags = HORO_PACKAGE_PROVIDER_DELAY | HORO_PACKAGE_PROVIDER_BUSY;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            auto oldStarted = fixture.Start(0);
            REQUIRE(oldStarted.HasValue());
            auto oldHost = std::move(oldStarted).Value();
            auto oldRequest = oldHost->UnlockAchievement({1});
            REQUIRE(oldRequest.HasValue());
            CHECK(oldHost->DispatchCompletions(0) == 0);
            CHECK(fixture.images.front()->Snapshot().submitted == 1);
            std::weak_ptr<const VerifiedPackageInstallRecord> previousInstall = fixture.base.install.InstalledRecord();
            fixture.flags = 0;
            fixture.Install(false, 2);
            REQUIRE(fixture.Activate().HasValue());
            REQUIRE(fixture.generations.size() == 2);
            CHECK(fixture.Start(0).HasError());
            CHECK_FALSE(fixture.images.front()->released);
            CHECK_FALSE(previousInstall.expired());
            CHECK(std::filesystem::exists(fixture.images.front()->root));
            auto replacement = fixture.Start(1);
            REQUIRE(replacement.HasValue());
            auto host = std::move(replacement).Value();
            auto request = host->UnlockAchievement({2});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            REQUIRE(oldHost->Close().HasError());
            CHECK(fixture.images.front()->Snapshot().cancelled >= 1);
            REQUIRE_FALSE(fixture.images.front()->released);
            fixture.images.front()->configure(0);
            REQUIRE(oldHost->Close().HasValue());
            const auto retiredRequest = oldHost->Query(oldRequest.Value());
            REQUIRE(retiredRequest.HasValue());
            CHECK(retiredRequest.Value().terminal);
            fixture.Finalize();
            CheckReleased(*fixture.images.front());
            CHECK(previousInstall.expired());
            REQUIRE(host->Close().HasValue());
            REQUIRE(fixture.Activate(false).HasValue());
            fixture.Finalize();
            CheckReleased(*fixture.images.back());
        }
    }

    TEST_CASE("A failed partially created provider keeps its package image until native retirement succeeds",
              "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_PARTIAL_CREATE | HORO_PACKAGE_PROVIDER_BUSY;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        REQUIRE(fixture.Start(0).HasError());
        fixture.base.service->Shutdown();
        REQUIRE(fixture.images.size() == 1);
        auto &image = *fixture.images.front();
        REQUIRE_FALSE(image.released);
        CHECK(image.Snapshot().created == 1);
        CHECK(image.Snapshot().destroyed == 0);
        CHECK(std::filesystem::exists(image.root));
        CHECK(fixture.realm->admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        image.configure(0);
        fixture.Finalize();
        CheckReleased(image);
    }

    TEST_CASE("Cancelled and stale package provider updates revoke staged registrations and preserve the old realm",
              "[packages][platform-services][provider]") {
        for (const bool stale : {false, true}) {
            ProviderFixture fixture;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            const auto previous = fixture.base.service->Active();
            fixture.Install(false, 2);
            fixture.cancelAfterCommit = !stale;
            fixture.staleAfterCommit = stale;
            REQUIRE(fixture.Activate().HasError());
            CHECK(fixture.base.service->Active() == previous);
            REQUIRE(fixture.generations.size() == 2);
            CHECK(fixture.Start(1).HasError());
            auto prior = fixture.Start(0);
            REQUIRE(prior.HasValue());
            REQUIRE(prior.Value()->Close().HasValue());
            fixture.Finalize();
            CheckReleased(*fixture.images.back());
        }
    }

    TEST_CASE("Provider package rollback and uninstall retire each exact installed generation", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        for (const unsigned version : {1U, 2U, 1U}) {
            fixture.Install(false, version);
            REQUIRE(fixture.Activate().HasValue());
            const auto index = fixture.generations.size() - 1;
            auto started = fixture.Start(index);
            REQUIRE(started.HasValue());
            REQUIRE(started.Value()->Close().HasValue());
            fixture.Finalize();
            if (index != 0) {
                CHECK(fixture.Start(index - 1).HasError());
                CheckReleased(*fixture.images[index - 1]);
            }
        }
        auto removed = std::make_shared<PackageRestoreGraph>(*fixture.base.install.ActiveGraph());
        removed->packages.clear();
        fixture.base.Install(std::move(removed));
        REQUIRE(fixture.Activate(false).HasValue());
        fixture.Finalize();
        CHECK(fixture.base.service->Active()->install->Graph()->packages.empty());
        for (std::size_t index = 0; index < fixture.images.size(); ++index) {
            CHECK(fixture.Start(index).HasError());
            CheckReleased(*fixture.images[index]);
        }
    }

    TEST_CASE("Same-contract provider updates require explicit disable before registry reuse", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        const auto previous = fixture.base.service->Active();
        fixture.Install();
        REQUIRE(fixture.Activate().HasError());
        CHECK(fixture.base.service->Active() == previous);
        REQUIRE(fixture.generations.size() == 1);
        auto prior = fixture.Start(0);
        REQUIRE(prior.HasValue());
        REQUIRE(prior.Value()->Close().HasValue());
        REQUIRE(fixture.Activate(false).HasValue());
        fixture.Finalize();
        CHECK(fixture.Start(0).HasError());
        REQUIRE(fixture.Activate().HasValue());
        REQUIRE(fixture.generations.size() == 2);
        auto replacement = fixture.Start(1);
        REQUIRE(replacement.HasValue());
        CHECK(fixture.generations[1] > fixture.generations[0]);
        REQUIRE(replacement.Value()->Close().HasValue());
    }

    TEST_CASE("Provider and headless tooling roles activate once through their exact native owners",
              "[packages][platform-services][provider][hybrid]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.tooling = true;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            CHECK(fixture.toolingLoads == 1);
            REQUIRE(fixture.generations.size() == 1);
            if (roleAware) {
                const auto snapshot = fixture.base.service->Active();
                REQUIRE(snapshot->modules.size() == 2);
                for (const auto &module : snapshot->modules)
                    CHECK(module.outcome == PackageModuleOutcome::Active);
            }
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            REQUIRE(started.Value()->Close().HasValue());
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CHECK(fixture.Start(0).HasError());
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("BUSY provider close retries native cancellation only once even when cancellation is rejected",
              "[packages][platform-services][provider][shutdown]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_DELAY | HORO_PACKAGE_PROVIDER_BUSY | HORO_PACKAGE_PROVIDER_CANCEL_REJECTED;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        auto started = fixture.Start(0);
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        auto &image = *fixture.images.front();
        REQUIRE(image.Snapshot().submitted == 1);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto closed = host->Close();
            REQUIRE(closed.HasError());
            CHECK(closed.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::DrainBusy.code.Value());
            CHECK(image.Snapshot().cancelled == 1);
            CHECK(image.Snapshot().destroyed == 0);
            CHECK(host->Query(request.Value()).Value().terminal);
            CHECK(host->UnlockAchievement({2}).HasError());
        }
        REQUIRE_FALSE(image.released);
        image.configure(0);
        REQUIRE(host->Close().HasValue());
        CHECK(image.Snapshot().cancelled == 1);
        CHECK(image.Snapshot().destroyed == 1);
        fixture.base.service->Shutdown();
        fixture.Finalize();
        CheckReleased(image);
    }

    TEST_CASE("Failed provider close withdraws deferred user delivery before native retirement",
              "[packages][platform-services][provider][shutdown]") {
        for (const std::uint32_t failure :
             {HORO_PACKAGE_PROVIDER_BUSY, HORO_PACKAGE_PROVIDER_CLOSE_ADMISSION_FAILED, HORO_PACKAGE_PROVIDER_CLOSE_INGRESS_FAILED}) {
            ProviderFixture fixture;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            REQUIRE(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            unsigned deliveries{};
            auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
                ++deliveries;
            });
            REQUIRE(subscription.HasValue());
            CHECK(deliveries == 0);
            auto &image = *fixture.images.front();
            image.configure(failure);
            REQUIRE(host->Close().HasError());
            REQUIRE_FALSE(image.released);
            CHECK(image.Snapshot().destroyed == 0);
            CHECK(image.Snapshot().sinkRetained == 1);
            CHECK(host->DispatchCompletions(16) == 0);
            CHECK(deliveries == 0);
            CHECK(host->OnComplete(request.Value(), [&](const auto &) {
                ++deliveries;
            }).HasError());
            CHECK(host->UnlockAchievement({2}).HasError());
            image.configure(0);
            REQUIRE(host->Close().HasValue());
            CHECK(host->DispatchCompletions(16) == 0);
            CHECK(deliveries == 0);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CheckReleased(image);
        }
    }
}  // namespace Horo::Packages::Tests
