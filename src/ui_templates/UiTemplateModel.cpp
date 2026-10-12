#include "Horo/UiTemplates/UiTemplateModel.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateIdentityEncoder.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace Horo::UiTemplates {
    namespace {
        /** @brief Checks one positive finite policy ceiling without admitting a zero/unbounded budget. */
        bool WithinMaximum(const std::size_t value, const std::size_t maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        /** @brief Admits the complete stable instance lineage before hashing any persistent identity bytes. */
        bool ValidLineage(const Runtime::Ui::UiDocumentId document, const std::span<const UiTemplateInstanceId> lineage) {
            return document.IsValid() && !lineage.empty() && lineage.size() <= 32 &&
                   std::ranges::all_of(lineage, [](const UiTemplateInstanceId id) {
                return id.IsValid();
            });
        }

        /** @brief Hashes admitted lineage and suffix fields in their version-one persistent wire order. */
        bool HashIdentityFields(Sha256Builder &hash, const std::span<const UiTemplateInstanceId> lineage,
                                const std::span<const Runtime::Ui::SerializedUiId> suffix) {
            for (const UiTemplateInstanceId instance : lineage)
                if (!hash.Update(std::as_bytes(std::span{instance.Bytes()})))
                    return false;
            for (const auto &field : suffix)
                if (!hash.Update(std::as_bytes(std::span{field})))
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc TemplateValueKind */
    UiTemplateValueKind TemplateValueKind(const UiTemplateValue &value) noexcept {
        return static_cast<UiTemplateValueKind>(value.index());
    }

    /** @copydoc Detail::EncodeTemplateIdentity */
    Result<Runtime::Ui::UiElementId> Detail::EncodeTemplateIdentity(const TemplateIdentityDomain domain,
                                                                    const Runtime::Ui::UiDocumentId document,
                                                                    const std::span<const UiTemplateInstanceId> lineage,
                                                                    const std::span<const Runtime::Ui::SerializedUiId> suffix) {
        if (!ValidLineage(document, lineage))
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));

        Sha256Builder hash;
        const std::string_view wireDomain =
            domain == TemplateIdentityDomain::Element ? "horo.ui-template.element.v1" : "horo.ui-template.slot-default.v1";
        if (const std::size_t fieldCount = domain == TemplateIdentityDomain::Element ? 1 : 2; suffix.size() != fieldCount)
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        // Fixed-size identity fields and the explicit lineage count make this encoding unambiguous.
        if (const std::array count{static_cast<std::uint8_t>(lineage.size())};
            !hash.Update(std::as_bytes(std::span{wireDomain.data(), wireDomain.size()})) ||
            !hash.Update(std::as_bytes(std::span{document.Bytes()})) || !hash.Update(std::as_bytes(std::span{count})))
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        if (!HashIdentityFields(hash, lineage, suffix))
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        Runtime::Ui::SerializedUiId bytes{};
        const auto digest = hash.Finalize();
        std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
        return Runtime::Ui::UiElementId::Create(bytes);
    }

    /** @copydoc Detail::DeriveTemplateSlotDefaultElementId */
    Result<Runtime::Ui::UiElementId> Detail::DeriveTemplateSlotDefaultElementId(const Runtime::Ui::UiDocumentId document,
                                                                                const std::span<const UiTemplateInstanceId> lineage,
                                                                                const UiTemplateSlotId slot,
                                                                                const Runtime::Ui::UiElementId authored) {
        if (!slot.IsValid() || !authored.IsValid())
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        const std::array suffix{slot.Bytes(), authored.Bytes()};
        return EncodeTemplateIdentity(TemplateIdentityDomain::SlotDefault, document, lineage, suffix);
    }

    /** @copydoc DeriveTemplateElementId */
    Result<Runtime::Ui::UiElementId> DeriveTemplateElementId(const Runtime::Ui::UiDocumentId document,
                                                             const std::span<const UiTemplateInstanceId> lineage,
                                                             const UiTemplateLocalElementId local) {
        if (!local.IsValid())
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        const std::array<Runtime::Ui::SerializedUiId, 1> suffix{local.Bytes()};
        return Detail::EncodeTemplateIdentity(Detail::TemplateIdentityDomain::Element, document, lineage, suffix);
    }

    /** @copydoc UiTemplateLimits::IsValid */
    bool UiTemplateLimits::IsValid() const noexcept {
        return WithinMaximum(maximumElements, 4096) && WithinMaximum(maximumDepth, 32) && WithinMaximum(maximumInstances, 256) &&
               WithinMaximum(maximumParameters, 256) && WithinMaximum(maximumSlots, 256) && WithinMaximum(maximumProperties, 16384) &&
               WithinMaximum(maximumDependencies, 1024) && WithinMaximum(maximumPayloadBytes, 4 * 1024 * 1024) &&
               WithinMaximum(maximumUndoEntries, 16);
    }
}  // namespace Horo::UiTemplates
