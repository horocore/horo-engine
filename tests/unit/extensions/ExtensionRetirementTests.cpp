#include "ExternalAssetImporter.h"
#include "Horo/Extensions/ExtensionRetirement.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <memory>
#include <type_traits>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        class Publication final : public IExtensionRetirementContribution {
        public:
            Publication(std::vector<int> &order, const int id, ExtensionRetirement &retirement)
                : order_(order), id_(id), retirement_(retirement) {}

            void Revoke() noexcept override {
                order_.push_back(id_);
                retirement_.CloseAdmission();
            }

        private:
            std::vector<int> &order_;
            int id_;
            ExtensionRetirement &retirement_;
        };

        class ReentrantPublication final : public IExtensionRetirementContribution {
        public:
            ReentrantPublication(ExtensionRetirement &retirement, bool &destroyed) : retirement_(retirement), destroyed_(destroyed) {}

            ~ReentrantPublication() override {
                (void)retirement_.Inspect();
                destroyed_ = true;
            }

            void Revoke() noexcept override {}

        private:
            ExtensionRetirement &retirement_;
            bool &destroyed_;
        };

        struct ReentrantWork final {
            ExtensionRetirement &retirement;
            bool &destroyed;

            ~ReentrantWork() {
                (void)retirement.Inspect();
                destroyed = true;
            }
        };

        struct ModuleAudit final {
            std::vector<int> *order;
            int id;
        };

        std::shared_ptr<ExtensionModuleLifetime> Module(ModuleAudit &audit) {
            auto module = std::make_shared<ExtensionModuleLifetime>();
            module->loaded = true;
            module->moduleApi.moduleContext = &audit;
            module->unload = [](HoroExtensionModuleApi *api) {
                auto &audit = *static_cast<ModuleAudit *>(api->moduleContext);
                audit.order->push_back(audit.id);
            };
            return module;
        }
    }  // namespace

    static_assert(!std::is_move_constructible_v<ExtensionModuleLifetime>);
    static_assert(!std::is_move_assignable_v<ExtensionModuleLifetime>);

    TEST_CASE("Retirement closes executable admission and reports every attributed work category", "[Extensions][Retirement]") {
        ExtensionRetirement retirement{"extension", {"provider"}};
        auto code = std::make_shared<int>(0);
        REQUIRE(retirement.BindModuleCode("provider", code));
        CHECK_FALSE(retirement.BindModuleCode("provider", code));
        std::vector<std::shared_ptr<ExtensionExecutableLease>> leases;
        for (const auto kind : {ExtensionLeaseKind::Callback, ExtensionLeaseKind::Job, ExtensionLeaseKind::Resource,
                                ExtensionLeaseKind::UiSurface, ExtensionLeaseKind::HostService}) {
            auto lease = retirement.Acquire("provider", kind, "owned-work", std::make_shared<int>(1));
            REQUIRE(lease);
            leases.push_back(std::move(lease));
        }
        auto report = retirement.BeginRetirement();
        REQUIRE(report.outstanding.size() == 5);
        CHECK(report.extensionId == "extension");
        CHECK(report.disposition == ExtensionRetirementDisposition::Draining);
        CHECK_FALSE(retirement.Acquire("provider", ExtensionLeaseKind::Job, "late-work", code));
        CHECK_FALSE(retirement.IsDrained());
        leases.clear();
        CHECK(retirement.IsDrained());
        CHECK(retirement.BeginRetirement().disposition == ExtensionRetirementDisposition::Complete);
    }

    TEST_CASE("Retirement revokes dependent publications before providers and tolerates reentrant closure", "[Extensions][Retirement]") {
        ExtensionRetirement retirement{"extension", {"provider", "dependent"}};
        auto code = std::make_shared<int>(0);
        REQUIRE(retirement.BindModuleCode("provider", code));
        REQUIRE(retirement.BindModuleCode("dependent", code));
        std::vector<int> order;
        order.reserve(3);
        REQUIRE(retirement.RegisterContribution("dependent", std::make_shared<Publication>(order, 2, retirement)));
        REQUIRE(retirement.RegisterContribution("provider", std::make_shared<Publication>(order, 1, retirement)));
        REQUIRE(retirement.RegisterContribution("dependent", std::make_shared<Publication>(order, 3, retirement)));
        retirement.CloseAdmission();
        retirement.CloseAdmission();
        CHECK(order == std::vector<int>{3, 2, 1});
        CHECK(retirement.IsDrained());
        CHECK_FALSE(retirement.RegisterContribution("provider", std::make_shared<Publication>(order, 4, retirement)));
    }

    TEST_CASE("Executable work pins provider dependencies even when its object does not own native code", "[Extensions][Retirement]") {
        std::vector<int> order;
        order.reserve(2);
        ModuleAudit providerAudit{&order, 1};
        ModuleAudit dependentAudit{&order, 2};
        auto provider = Module(providerAudit);
        auto dependent = Module(dependentAudit);
        dependent->code->dependencies.push_back(provider);
        ExtensionRetirement retirement{"extension", {"provider", "dependent"}};
        REQUIRE(retirement.BindModuleCode("provider", provider));
        REQUIRE(retirement.BindModuleCode("dependent", dependent));
        auto lease = retirement.Acquire("dependent", ExtensionLeaseKind::Job, "queued-job", std::make_shared<int>(0));
        REQUIRE(lease);
        retirement.CloseAdmission();
        provider.reset();
        dependent.reset();
        CHECK(order.empty());
        lease.reset();
        CHECK(order == std::vector<int>{2, 1});
        CHECK(retirement.IsDrained());
    }

    TEST_CASE("Worker release keeps owner-lane native pins intact until host finalization", "[Extensions][Retirement]") {
        std::vector<int> order;
        order.reserve(1);
        ModuleAudit audit{&order, 1};
        auto hostPin = Module(audit);
        ExtensionRetirement retirement{"extension", {"provider"}};
        REQUIRE(retirement.BindModuleCode("provider", hostPin));
        auto work = retirement.Acquire("provider", ExtensionLeaseKind::Callback, "callback", std::make_shared<int>(0));
        REQUIRE(work);
        retirement.CloseAdmission();
        auto released = std::async(std::launch::async, [work = std::move(work)]() mutable {
            work.reset();
        });
        released.get();
        CHECK(retirement.IsDrained());
        CHECK(order.empty());
        hostPin.reset();
        CHECK(order == std::vector<int>{1});
    }

    TEST_CASE("Retirement capacity rejection destroys reentrant native owners outside the state mutex", "[Extensions][Retirement]") {
        ExtensionRetirement retirement{"extension", {"provider"}};
        auto code = std::make_shared<int>(0);
        REQUIRE(retirement.BindModuleCode("provider", code));
        std::vector<std::shared_ptr<ExtensionExecutableLease>> work;
        work.reserve(4096);
        for (std::size_t index = 0; index < 4096; ++index) {
            auto lease = retirement.Acquire("provider", ExtensionLeaseKind::Job, "queued-work", code);
            REQUIRE(lease);
            work.push_back(std::move(lease));
        }
        bool workDestroyed{};
        auto rejectedWork = std::shared_ptr<ReentrantWork>{new ReentrantWork{retirement, workDestroyed}};
        CHECK_FALSE(retirement.Acquire("provider", ExtensionLeaseKind::Job, "over-capacity", std::move(rejectedWork)));
        CHECK(workDestroyed);

        bool publicationDestroyed{};
        auto publication = std::make_shared<ReentrantPublication>(retirement, publicationDestroyed);
        for (std::size_t index = 0; index < 1024; ++index)
            REQUIRE(retirement.RegisterContribution("provider", publication));
        bool rejectedDestroyed{};
        CHECK_FALSE(retirement.RegisterContribution("provider", std::make_shared<ReentrantPublication>(retirement, rejectedDestroyed)));
        CHECK(rejectedDestroyed);
        publication.reset();
        retirement.CloseAdmission();
        CHECK(publicationDestroyed);
        CHECK(retirement.Inspect().outstanding.size() == 4096);
        work.clear();
        CHECK(retirement.IsDrained());
    }
}  // namespace Horo::Extensions::Tests
