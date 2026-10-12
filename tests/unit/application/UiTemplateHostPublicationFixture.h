#pragma once

#include "UiTemplateHostTestFixture.h"

namespace Horo::Application::TemplateHostTests {
    /** @brief Loads exact host cook bytes through Assets and composes real allocator-issued runtime trees. */
    struct PublicationFixture final {
        Fixture source;
        UiTemplateResourceBindings resources;
        Runtime::Ui::UiOwnershipGeneration ownership{Runtime::Ui::UiOwnershipGeneration::Create(811).Value()};
        Runtime::Ui::RuntimeUiInstanceId instance{ownership, 1, 1};
        Runtime::Ui::UiElementSlotAllocator allocator{std::move(Runtime::Ui::UiElementSlotAllocator::Create(ownership)).Value()};

        /** @brief Verifies cook inputs against the fixture's actual captured source.
         * @param snapshot Exact proposed document. @return Verified immutable cook lease.
         */
        [[nodiscard]] UiTemplateHostCook Cook(const UiTemplates::UiTemplateDocumentSnapshot &snapshot) const;
        /** @brief Loads ordinary cooked bytes and constructs complete real runtime trees.
         * @param cook Verified output. @return Privately prepared generation with fresh allocator reservations.
         */
        [[nodiscard]] Runtime::Ui::UiReloadGeneration Generation(const UiTemplateHostCook &cook);
        /** @brief Loads the ordinary root and actual captured resource files through the existing Assets service. */
        [[nodiscard]] Runtime::Ui::UiRuntimeAssetLoadResult Load(const Runtime::Ui::CookedUiDocument &document) const;
        /** @brief Allocates fresh actual tree namespaces for every cooked canvas. */
        [[nodiscard]] std::vector<Runtime::Ui::UiReloadCanvas> Trees(const Runtime::Ui::CookedUiDocument &document);
        /** @brief Transfers the fixture's sole authoring owner into the host.
         * @param limits Runtime retention/candidate bounds. @return Active paired owner.
         */
        [[nodiscard]] std::unique_ptr<UiTemplateHost> Host(Runtime::Ui::UiHotReloadLimits limits = {});
        /** @brief Prepares complete detachment with one fresh persisted element identity.
         * @param host Exact document owner. @param cancellation Captured cancellation ancestry.
         * @return Unpublished document transaction.
         */
        [[nodiscard]] UiTemplates::UiTemplatePreparedTransaction Detach(const UiTemplateHost &host,
                                                                        const CancellationToken &cancellation = {}) const;
        /** @brief Prepares a complete verified document/runtime replacement.
         * @param host Exact publisher. @param document Prepared authoring candidate.
         * @param cancellation Runtime cancellation ancestry. @return Unpublished paired candidate.
         */
        [[nodiscard]] UiTemplateHost::Prepared Pair(UiTemplateHost &host, UiTemplates::UiTemplatePreparedTransaction document,
                                                    const CancellationToken &cancellation = {});
    };
}  // namespace Horo::Application::TemplateHostTests
