#include "Horo/Runtime/Render/LightFrameLayout.h"
#include "RegisterRasterReflectionInternal.h"

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        constexpr std::array LightFields{StorageElementField{ShaderValueType::Float32, 4, offsetof(PackedRenderLight, positionRange)},
                                         StorageElementField{ShaderValueType::Float32, 3, offsetof(PackedRenderLight, direction)},
                                         StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedRenderLight, kind)},
                                         StorageElementField{ShaderValueType::Float32, 4, offsetof(PackedRenderLight, colorIntensity)},
                                         StorageElementField{ShaderValueType::Float32, 2, offsetof(PackedRenderLight, cones)},
                                         StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedRenderLight, identityLow)},
                                         StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedRenderLight, identityHigh)}};
        constexpr std::array MembershipFields{StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedLightMembership, offset)},
                                              StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedLightMembership, count)},
                                              StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedLightMembership, omitted)},
                                              StorageElementField{ShaderValueType::Uint32, 1, offsetof(PackedLightMembership, reserved)}};
        constexpr std::array ReferenceFields{StorageElementField{ShaderValueType::Uint32, 1, 0}};

        bool SameShape(const ShaderParameter &parameter, const StorageElementField &field) noexcept {
            return parameter.type == field.type && parameter.rows == 1 && parameter.columns == field.columns && parameter.arrayCount == 1;
        }

        bool Matches(const ShaderManifest &manifest, ShaderBindingId binding, StorageElementKind kind) noexcept {
            const auto fields = StorageFields(kind);
            std::size_t index{};
            for (const auto &parameter : manifest.parameters) {
                if (parameter.binding != binding)
                    continue;
                if (index == fields.size() || !SameShape(parameter, fields[index]))
                    return false;
                ++index;
            }
            return index == fields.size();
        }
    }  // namespace

    std::span<const StorageElementField> StorageFields(StorageElementKind kind) noexcept {
        switch (kind) {
            using enum Horo::Render::RegisterRasterCookDetail::StorageElementKind;
            case PackedLight:
                return LightFields;
            case Membership:
                return MembershipFields;
            case Reference:
                return ReferenceFields;
        }
        return {};
    }

    std::uint32_t StorageStride(StorageElementKind kind) noexcept {
        switch (kind) {
            using enum Horo::Render::RegisterRasterCookDetail::StorageElementKind;
            case PackedLight:
                return sizeof(PackedRenderLight);
            case Membership:
                return sizeof(PackedLightMembership);
            case Reference:
                return sizeof(std::uint32_t);
        }
        return 0;
    }

    std::optional<StorageElementKind> StorageKind(const ShaderManifest &manifest, ShaderBindingId binding) noexcept {
        using enum Horo::Render::RegisterRasterCookDetail::StorageElementKind;
        for (const auto kind : {PackedLight, Membership, Reference})
            if (Matches(manifest, binding, kind))
                return kind;
        return std::nullopt;
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
