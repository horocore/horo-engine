#include "EditorPageTestSupport.h"

namespace Horo::Extensions::Tests {
    using PageSupport::Fixture;

    TEST_CASE("Real extension settings draft validates then durably publishes exact64bit configuration",
              "[Extensions][EditorPage][Persistence]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(9007199254740993LL);
        CHECK(fixture.store.writes == 0);
        CHECK(fixture.Page().dirty);
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        CHECK(fixture.store.writes == 1);
        CHECK_FALSE(fixture.Page().dirty);
        CHECK(fixture.Value() == 9007199254740993LL);
        REQUIRE(fixture.store.document);
        CHECK(fixture.store.document->find("9007199254740993") != std::string::npos);
        CHECK(fixture.host.LocalizedText(fixture.provider, "fixture.title", "tr-TR") == "Ayarlar");
        CHECK(fixture.host.LocalizedText(fixture.provider, "fixture.count", "tr-TR") == "Count");
    }

    TEST_CASE("Extension page reset and cancel touch transient drafts only and never enter workspace persistence",
              "[Extensions][EditorPage][Lifecycle]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(77);
        const auto stale = fixture.Page().revision;
        REQUIRE(fixture.host.Reset(fixture.provider, fixture.id, stale).HasValue());
        fixture.host.Update();
        CHECK(fixture.Value() == 20);
        CHECK(fixture.host.SetValue(fixture.provider, fixture.id, stale, {{"page.count"}, std::int64_t{99}}).HasError());
        REQUIRE(fixture.host.Cancel(fixture.provider, fixture.id).HasValue());
        fixture.host.Update();
        CHECK(fixture.registry.Save().surfaces.empty());
        CHECK(fixture.store.writes == 0);
        fixture.registration->Reset();
        CHECK(fixture.registry.Save().surfaces.empty());
    }

    TEST_CASE("Durability uncertainty retains dirty recovery state and original error even after cancellation",
              "[Extensions][EditorPage][Recovery]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        fixture.store.failWrite = true;
        SECTION("failure before replacement") {}
        SECTION("replacement committed before durability failure") {
            fixture.store.replaceThenFail = true;
        }
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        REQUIRE(fixture.Page().state == EditorPageState::RecoveryRequired);
        CHECK(fixture.Page().dirty);
        REQUIRE(fixture.Page().failure);
        CHECK(fixture.Page().failure->message == "Original durable-store failure");
        REQUIRE(fixture.host.Cancel(fixture.provider, fixture.id).HasValue());
        fixture.host.Update();
        CHECK(fixture.Page().state == EditorPageState::RecoveryRequired);
        CHECK(fixture.Page().dirty);
        CHECK(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasError());
        fixture.store.failWrite = false;
        REQUIRE(fixture.host.Begin(fixture.provider, fixture.id).HasValue());
        fixture.host.Update();
        CHECK(fixture.Value() == (fixture.store.replaceThenFail ? 99 : 20));
        CHECK_FALSE(fixture.Page().dirty);
    }

    TEST_CASE("Foreign provider exceptions complete the handoff without publishing settings", "[Extensions][EditorPage][Recovery]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        fixture.operations->throwForeign = true;
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        CHECK(fixture.Page().state == EditorPageState::Failed);
        CHECK(fixture.Page().dirty);
        CHECK(fixture.store.writes == 0);
        CHECK(fixture.operations->active.load() == 0);
        REQUIRE(fixture.Page().failure);
        CHECK(fixture.Page().failure->code.Value() == ExtensionErrors::ContributionRejected.code.Value());
    }

    TEST_CASE("Foreign store exceptions preserve uncertainty before and after replacement", "[Extensions][EditorPage][Recovery]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        bool replaced = false;
        SECTION("before replacement") {
            fixture.store.throwPoint = PageSupport::StoreThrowPoint::BeforeReplacement;
        }
        SECTION("after replacement") {
            fixture.store.throwPoint = PageSupport::StoreThrowPoint::AfterReplacement;
            replaced = true;
        }
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        CHECK(fixture.Page().state == EditorPageState::RecoveryRequired);
        CHECK(fixture.Page().dirty);
        CHECK(fixture.Value() == 99);
        CHECK(fixture.store.writes == 1);
        REQUIRE(fixture.Page().failure);
        CHECK(fixture.Page().failure->code.Value() == ExtensionErrors::ContributionRejected.code.Value());
        REQUIRE(fixture.host.Cancel(fixture.provider, fixture.id).HasValue());
        fixture.host.Update();
        CHECK(fixture.Page().state == EditorPageState::RecoveryRequired);
        CHECK(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasError());
        fixture.store.throwPoint = PageSupport::StoreThrowPoint::None;
        REQUIRE(fixture.host.Begin(fixture.provider, fixture.id).HasValue());
        fixture.host.Update();
        CHECK(fixture.Value() == (replaced ? 99 : 20));
        CHECK_FALSE(fixture.Page().dirty);
    }

    TEST_CASE("Whole candidate validation or changed persistence context prevents settings publication",
              "[Extensions][EditorPage][Validation]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        SECTION("provider validation error") {
            fixture.operations->findings = {{{"page.count"},
                                             {"fixture.validation"},
                                             {EditorUiTextKind::LocalizationKey, "fixture.invalid"},
                                             EditorUiValidationSeverity::Error}};
        }
        SECTION("foreign provider diagnostic") {
            fixture.operations->findings = {
                {{}, {"foreign.code"}, {EditorUiTextKind::LocalizationKey, "fixture.invalid"}, EditorUiValidationSeverity::Error}};
        }
        SECTION("provider failure preserves detail") {
            fixture.operations->fail = true;
        }
        SECTION("actual project changed") {
            ++fixture.store.epoch;
        }
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        CHECK(fixture.Page().state == EditorPageState::Failed);
        CHECK(fixture.Page().dirty);
        CHECK(fixture.store.writes == 0);
    }

    TEST_CASE("Actual transient modal validates then submits without persistent settings writes", "[Extensions][EditorPage][Modal]") {
        Fixture fixture{true};
        fixture.Begin();
        fixture.Set(std::numeric_limits<std::int64_t>::min());
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        fixture.Wait();
        CHECK(fixture.Page().state == EditorPageState::Completed);
        CHECK(fixture.operations->calls[0].load() == 1);
        CHECK(fixture.operations->calls[1].load() == 1);
        CHECK(fixture.store.writes == 0);
        CHECK(fixture.registry.Save().surfaces.empty());
    }

    TEST_CASE("Provider removal cancels admitted work and no late result can publish", "[Extensions][EditorPage][Retirement]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        fixture.operations->hold.store(true);
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!fixture.operations->entered.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(fixture.operations->entered.load());
        static_cast<void>(fixture.retirement->BeginRetirement());
        fixture.host.Update();
        CHECK(fixture.host.Prepared().empty());
        CHECK(fixture.store.writes == 0);
        CHECK_FALSE(fixture.registration->IsRegistered());
        CHECK(fixture.host.Begin(fixture.provider, fixture.id).HasError());
    }

    TEST_CASE("Page focus is generation checked and transient opaque state is never admitted", "[Extensions][EditorPage][Focus]") {
        Fixture fixture;
        fixture.Begin();
        const auto revision = fixture.Page().revision;
        REQUIRE(fixture.host.Focus(fixture.provider, fixture.id, revision, {"fixture.number"}).HasValue());
        fixture.host.Update();
        CHECK(fixture.Page().focused == EditorUiId{"fixture.number"});
        CHECK(fixture.host.Focus(fixture.provider, fixture.id, revision - 1, {"fixture.number"}).HasError());
        CHECK(fixture.host.Focus(fixture.provider, fixture.id, fixture.Page().revision, {"foreign.node"}).HasError());
        const std::array<std::uint8_t, 1> draft{1};
        CHECK(fixture.registry.SetOpaqueState(fixture.id, draft).HasError());
        CHECK(fixture.registry.Save().surfaces.empty());
    }

    TEST_CASE("Cancelling a running page serializes cancellation and fences the old result", "[Extensions][EditorPage][Cancellation]") {
        Fixture fixture;
        fixture.Begin();
        fixture.Set(99);
        fixture.operations->hold.store(true);
        REQUIRE(fixture.host.Apply(fixture.provider, fixture.id, fixture.Page().revision).HasValue());
        REQUIRE(fixture.host.Cancel(fixture.provider, fixture.id).HasValue());
        fixture.operations->hold.store(false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (fixture.operations->calls[2].load() == 0 && std::chrono::steady_clock::now() < deadline) {
            fixture.host.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        CHECK(fixture.operations->calls[2].load() == 1);
        CHECK(fixture.operations->maximumActive.load() <= 1);
        CHECK(fixture.store.writes == 0);
        CHECK(fixture.Page().state == EditorPageState::Cancelled);
        fixture.host.BeginShutdown();
        CHECK(fixture.host.Prepared().empty());
        CHECK(fixture.host.Begin(fixture.provider, fixture.id).HasError());
    }

    TEST_CASE("Malformed persisted settings fail before any draft or provider operation publishes",
              "[Extensions][EditorPage][Persistence]") {
        Fixture fixture;
        fixture.store.document = "{malformed";
        CHECK(fixture.host.Begin(fixture.provider, fixture.id).HasError());
        fixture.host.Update();
        CHECK(fixture.Page().state == EditorPageState::RecoveryRequired);
        CHECK(fixture.store.writes == 0);
        CHECK(fixture.operations->calls[0].load() == 0);
    }
}  // namespace Horo::Extensions::Tests
