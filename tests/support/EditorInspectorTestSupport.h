#pragma once
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Extensions::Tests::InspectorSupport {
    /** @brief Explicit test host policy exercises the actual capability admission and revocation implementation. */
    inline ExtensionAdmissionPolicy Policy() {
        return {.revision = 1,
                .knownPermissions = {{"editor.inspector"}, {"editor.property-drawer"}},
                .approvedPermissions = {{"editor.inspector"}, {"editor.property-drawer"}},
                .availableCapabilities = {EditorInspectorCapability, EditorPropertyDrawerCapability}};
    }

    /** @brief Complete copied declaration has exact constraints and localization, but no authored value authority. */
    inline EditorInspectorDefinition Definition() {
        EditorInspectorDefinition value;
        value.behaviorType = "project.counter";
        value.behaviorSchemaVersion = 3;
        value.form.id = {"fixture.inspector"};
        value.form.title = {EditorUiTextKind::LocalizationKey, "fixture.title"};
        value.form.nodes.push_back(
            {EditorUiNumberNode{.base = {.id = {"fixture.count"}, .label = {EditorUiTextKind::LocalizationKey, "fixture.count"}},
                                .binding = {"count"},
                                .numberKind = EditorUiNumberKind::Integer,
                                .value = std::int64_t{0}}});
        value.fields.push_back({.field = "count",
                                .kind = EditorInspectorFieldKind::Integer,
                                .node = {"fixture.count"},
                                .minimumInteger = INT64_MIN,
                                .maximumInteger = INT64_MAX});
        value.messages = {{"en-US", "fixture.title", "Counter inspector"},
                          {"en-US", "fixture.count", "Exact count"},
                          {"tr-TR", "fixture.title", "Sayaç inceleyicisi"}};
        return value;
    }

    /** @brief Owns real registry/admission/retirement before its publication; no manually forged approval is supplied. */
    struct Fixture final {
        EditorSurfaceRegistry registry;
        EditorInspectorHost host{registry, Policy()};
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        std::shared_ptr<ExtensionRetirement> retirement{
            std::make_shared<ExtensionRetirement>("fixture.package", std::vector<std::string>{"fixture.module"})};
        std::shared_ptr<void> code{std::make_shared<int>(1)};
        std::optional<EditorInspectorRegistration> registration;
        EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};

        /** @brief Registers through the production sealed admission/context/catalog path, retaining exact retirement ownership. */
        explicit Fixture(EditorInspectorDefinition definition = Definition(), EditorSurfaceKind kind = EditorSurfaceKind::Inspector) {
            const bool drawer = kind == EditorSurfaceKind::PropertyDrawer;
            const auto capability = drawer ? EditorPropertyDrawerCapability : EditorInspectorCapability;
            const std::vector<ExtensionPermissionId> permissions{{drawer ? "editor.property-drawer" : "editor.inspector"}};
            auto admitted = ExtensionCapabilityAdmission::Evaluate({provider.extensionId,
                                                                    provider.moduleId,
                                                                    provider.activationGeneration,
                                                                    {{capability, permissions}}},
                                                                   Policy());
            if (admitted.HasError())
                UNSCOPED_INFO(admitted.ErrorValue().message);
            REQUIRE(admitted.HasValue());
            admission.emplace(std::move(admitted).Value());
            auto grant = admission->Grant(capability);
            REQUIRE(grant.HasValue());
            const std::array grants{std::move(grant).Value()};
            EditorSurfaceContextDescriptor descriptor{.surface = {.id = "fixture.inspector",
                                                                  .kind = kind,
                                                                  .labelLocalizationKey = "fixture.title",
                                                                  .placement = {EditorSurfacePlacementKind::Inspector, {}, 0},
                                                                  .persistence = EditorSurfacePersistence::None,
                                                                  .requiredCapabilities = {capability},
                                                                  .requiredPermissions = permissions,
                                                                  .provider = provider},
                                                      .localization = {{"fixture.title"}, {"fixture.count"}}};
            auto attached = contexts.Attach(descriptor, admission->ActivationLease(), grants);
            REQUIRE(attached.HasValue());
            REQUIRE(retirement->BindModuleCode("fixture.module", code));
            auto published = host.Register(std::move(attached).Value(), std::move(definition), retirement);
            if (published.HasError())
                UNSCOPED_INFO(published.ErrorValue().message);
            REQUIRE(published.HasValue());
            registration.emplace(std::move(published).Value());
        }

        /** @brief Retires publication before dependent lifetime owners. */
        ~Fixture() {
            host.BeginShutdown();
        }

        /** @brief Returns one actual immutable discovery copy for source/controller/GUI regression composition. */
        EditorInspectorContribution Contribution() const {
            auto found = host.Contributions("project.counter", 3);
            REQUIRE(found.HasValue());
            REQUIRE(found.Value().size() == 1);
            return found.Value().front();
        }
    };
}  // namespace Horo::Extensions::Tests::InspectorSupport
