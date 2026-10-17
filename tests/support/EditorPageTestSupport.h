#pragma once
#include "Horo/Extensions/EditorPageHost.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Extensions::Tests::PageSupport {
    /** @brief Non-standard foreign callback failure used to exercise containment boundaries. */
    struct ForeignFailure final {};

    enum class StoreThrowPoint {
        None,
        BeforeReplacement,
        AfterReplacement
    };

    /** @brief Actual canonical configuration document authority with controllable durable outcome, not a competing value registry. */
    struct Store final : IEditorPageStore {
        std::optional<std::string> document;
        std::uint64_t epoch{1};
        unsigned writes{};
        bool failWrite{};
        bool replaceThenFail{};
        StoreThrowPoint throwPoint{StoreThrowPoint::None};

        std::uint64_t ContextRevision(SettingScope) const noexcept override {
            return epoch;
        }

        Result<std::optional<std::string>> Read(const EditorSurfaceProviderIdentity &, std::string_view, SettingScope) override {
            return Result<std::optional<std::string>>::Success(document);
        }

        Result<void> Write(const EditorSurfaceProviderIdentity &, std::string_view, SettingScope, std::uint64_t revision,
                           const ConfigurationSnapshot &snapshot) override {
            REQUIRE(revision == epoch);
            ++writes;
            if (throwPoint == StoreThrowPoint::BeforeReplacement)
                throw ForeignFailure{};
            if (!failWrite || replaceThenFail)
                document = snapshot.ToJson();
            if (throwPoint == StoreThrowPoint::AfterReplacement)
                throw ForeignFailure{};
            if (failWrite)
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected, "Original durable-store failure"));
            return Result<void>::Success();
        }
    };

    /** @brief Worker-controlled provider; atomic barrier permits cancellation/revocation tests without concurrent callback overlap. */
    struct Operations final : IEditorPageOperations {
        std::atomic_bool hold{};
        std::atomic_bool entered{};
        std::atomic_uint active{};
        std::atomic_uint maximumActive{};
        std::array<std::atomic_uint, 3> calls{};
        std::vector<EditorPageFinding> findings;
        bool fail{};
        bool throwForeign{};

        Result<std::vector<EditorPageFinding>> Invoke(EditorPageOperation operation, const EditorPageDraft &, const EditorUiActionId &,
                                                      const CancellationToken &cancel) override {
            const auto concurrent = active.fetch_add(1) + 1;
            maximumActive.store(std::max(maximumActive.load(), concurrent));
            ++calls[static_cast<std::size_t>(operation)];
            entered.store(true);
            while (hold.load() && !cancel.IsCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            --active;
            if (throwForeign)
                throw ForeignFailure{};
            if (fail)
                return Result<std::vector<EditorPageFinding>>::Failure(
                    MakeError(ExtensionErrors::ContributionRejected, "Original provider failure"));
            return Result<std::vector<EditorPageFinding>>::Success(findings);
        }
    };

    /** @brief Explicit approved host policy; tests can independently remove approval before any capability/context creation. */
    inline ExtensionAdmissionPolicy Policy() {
        return {1,
                {{"editor.settings"}, {"editor.modal"}, {"configuration.project.write"}},
                {{"editor.settings"}, {"editor.modal"}, {"configuration.project.write"}},
                {EditorSettingsPageCapability, EditorModalPageCapability}};
    }

    /** @brief Builds the same complete canonical field/localization schema for settings and transient modal domains. */
    inline EditorPageDefinition Definition(bool modal, std::string_view id) {
        EditorPageDefinition definition;
        definition.form.id = {std::string{id}};
        definition.form.title = {EditorUiTextKind::LocalizationKey, "fixture.title"};
        EditorPageField field;
        field.binding = {"page.count"};
        field.setting = {SettingKey{"page.count"},
                         SettingValueType::Integer,
                         std::int64_t{20},
                         modal ? SettingScope::Session : SettingScope::Project,
                         ReloadPolicy::NextOperation,
                         SettingSensitivity::Public,
                         {}};
        definition.fields.push_back(field);
        definition.form.nodes.push_back(
            {EditorUiNumberNode{.base = {.id = {"fixture.number"}, .label = {EditorUiTextKind::LocalizationKey, "fixture.count"}},
                                .binding = field.binding,
                                .numberKind = EditorUiNumberKind::Integer,
                                .value = std::int64_t{20}}});
        definition.messages = {{"en-US", "fixture.title", "Settings"},
                               {"tr-TR", "fixture.title", "Ayarlar"},
                               {"en-US", "fixture.count", "Count"},
                               {"en-US", "fixture.invalid", "Invalid"}};
        return definition;
    }

    /** @brief Owns scheduler before host and revocable registration; every worker is released before fixture destruction joins. */
    struct Fixture {
        JobSystem jobs{JobSystemConfig{.workerCount = 1}};
        Store store;
        EditorSurfaceRegistry registry;
        EditorPageHost host{jobs, registry, store, Policy()};
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        std::shared_ptr<ExtensionRetirement> retirement{
            std::make_shared<ExtensionRetirement>("fixture.package", std::vector<std::string>{"fixture.module"})};
        std::shared_ptr<void> codeOwner{std::make_shared<int>(1)};
        std::shared_ptr<Operations> operations{std::make_shared<Operations>()};
        std::optional<EditorPageRegistration> registration;
        EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};
        const std::string id = "fixture.page";

        explicit Fixture(bool modal = false, std::optional<EditorPageDefinition> suppliedDefinition = std::nullopt) {
            const auto capability = modal ? EditorModalPageCapability : EditorSettingsPageCapability;
            const std::vector<ExtensionPermissionId> permissions =
                modal ? std::vector<ExtensionPermissionId>{{"editor.modal"}}
                      : std::vector<ExtensionPermissionId>{{"editor.settings"}, {"configuration.project.write"}};
            auto admitted = ExtensionCapabilityAdmission::Evaluate({provider.extensionId,
                                                                    provider.moduleId,
                                                                    provider.activationGeneration,
                                                                    {{capability, permissions}}},
                                                                   Policy());
            REQUIRE(admitted.HasValue());
            admission.emplace(std::move(admitted).Value());
            auto grant = admission->Grant(capability);
            REQUIRE(grant.HasValue());
            const std::array grants{std::move(grant).Value()};
            EditorSurfaceContextDescriptor
                context{.surface = {.id = id,
                                    .kind = modal ? EditorSurfaceKind::Modal : EditorSurfaceKind::SettingsPage,
                                    .labelLocalizationKey = "fixture.title",
                                    .placement = {modal ? EditorSurfacePlacementKind::Modal : EditorSurfacePlacementKind::Settings, {}, 0},
                                    .persistence = EditorSurfacePersistence::None,
                                    .requiredCapabilities = {capability},
                                    .requiredPermissions = permissions,
                                    .provider = provider},
                        .commands = {{"fixture.action"}},
                        .state = {{"page.count"}},
                        .localization = {{"fixture.title"}, {"fixture.count"}, {"fixture.invalid"}},
                        .diagnostics = {{"fixture.validation"}}};
            auto attached = contexts.Attach(context, admission->ActivationLease(), grants);
            if (attached.HasError())
                UNSCOPED_INFO(attached.ErrorValue().message);
            REQUIRE(attached.HasValue());
            auto definition = suppliedDefinition ? std::move(*suppliedDefinition) : Definition(modal, id);
            REQUIRE(retirement->BindModuleCode("fixture.module", codeOwner));
            auto registered = host.Register(std::move(attached).Value(), std::move(definition), operations, retirement);
            if (registered.HasError())
                UNSCOPED_INFO(registered.ErrorValue().message);
            REQUIRE(registered.HasValue());
            registration.emplace(std::move(registered).Value());
            host.Update();
        }

        ~Fixture() {
            operations->hold.store(false);
            host.BeginShutdown();
        }

        const EditorPageProjection &Page() const {
            REQUIRE(host.Prepared().size() == 1);
            return host.Prepared().front();
        }

        void Begin() {
            REQUIRE(host.Begin(provider, id).HasValue());
            host.Update();
        }

        void Set(std::int64_t value) {
            REQUIRE(host.SetValue(provider, id, Page().revision, {{"page.count"}, value}).HasValue());
            host.Update();
        }

        std::int64_t Value() const {
            return std::get<std::int64_t>(std::get<EditorUiNumberNode>(Page().form.nodes.front().payload).value);
        }

        void Wait() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (std::chrono::steady_clock::now() < deadline) {
                host.Update();
                if (Page().state != EditorPageState::Validating && Page().state != EditorPageState::Submitting &&
                    operations->active.load() == 0)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Bounded page callback did not complete");
        }
    };
}  // namespace Horo::Extensions::Tests::PageSupport
