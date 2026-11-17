#include "AnimationCookedWire.h"

#include <utf8proc.h>

namespace Horo::Animation::CookedInternal {
    namespace {
        bool CopyNormalized(const std::vector<utf8proc_int32_t> &buffer, std::size_t bytes, std::string &output, Policy &policy);

        /** @brief Uses the existing pinned Unicode implementation after exact size/work/allocation preflight. */
        bool Normalize(std::span<const std::uint8_t> input, std::string &output, Policy &policy) {
            if (input.size() > policy.context.limits.stringBytes)
                return policy.Reject(AnimationAssetErrors::Capacity);
            constexpr auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_REJECTNA);
            const auto length = static_cast<utf8proc_ssize_t>(input.size());
            const auto count = utf8proc_decompose(input.data(), length, nullptr, 0, options);
            if (count < 0)
                return policy.Reject(AnimationAssetErrors::Malformed);
            const auto units = static_cast<std::uint64_t>(count) + 1;
            if (units > policy.context.limits.ownedBytes / sizeof(utf8proc_int32_t))
                return policy.Reject(AnimationAssetErrors::Capacity);
            if (units * 2 > policy.context.limits.values - policy.work)
                return policy.Reject(AnimationAssetErrors::Capacity);
            policy.work += units * 2;
            if (!policy.Allocate(units * sizeof(utf8proc_int32_t)))
                return false;
            std::vector<utf8proc_int32_t> buffer(static_cast<std::size_t>(units));
            const auto decoded = utf8proc_decompose(input.data(), length, buffer.data(), count, options);
            const auto bytes = decoded < 0 ? decoded : utf8proc_reencode(buffer.data(), decoded, options);
            if (bytes < 0)
                return policy.Reject(AnimationAssetErrors::Malformed);
            return CopyNormalized(buffer, static_cast<std::size_t>(bytes), output, policy);
        }

        /** @brief Copies only the admitted initialized UTF-8 output prefix from the native code-unit buffer. */
        bool CopyNormalized(const std::vector<utf8proc_int32_t> &buffer, std::size_t bytes, std::string &output, Policy &policy) {
            if (bytes > policy.context.limits.stringBytes || !policy.Allocate(bytes))
                return policy.Reject(AnimationAssetErrors::Capacity);
            const auto raw = std::as_bytes(std::span{buffer});
            output.resize(bytes);
            for (std::size_t index = 0; index < bytes; ++index)
                output[index] = static_cast<char>(std::to_integer<unsigned char>(raw[index]));
            return true;
        }
    }  // namespace

    /** @copydoc Transfer */
    void Transfer(Writer &wire, const std::string &value) {
        if (!wire.policy.Value())
            return;
        if (value.size() > wire.policy.context.limits.stringBytes) {
            wire.policy.Reject(AnimationAssetErrors::Capacity);
            return;
        }
        if (!wire.policy.Allocate(value.size()))
            return;
        const std::vector<std::uint8_t> input(value.begin(), value.end());
        std::string normalized;
        if (!Normalize(input, normalized, wire.policy))
            return;
        wire.Scalar<std::uint32_t>(static_cast<std::uint32_t>(normalized.size()));
        if (!wire.policy.Allocate(normalized.size()))
            return;
        const std::vector<std::uint8_t> encoded(normalized.begin(), normalized.end());
        wire.Raw(encoded);
    }

    /** @copydoc Transfer */
    void Transfer(Reader &wire, std::string &value) {
        const auto count = wire.Scalar<std::uint32_t>();
        const auto input = wire.Raw(count);
        if (!wire.policy.Good())
            return;
        std::string normalized;
        if (!Normalize(input, normalized, wire.policy))
            return;
        if (!std::ranges::equal(input, normalized, [](std::uint8_t left, char right) {
            return left == static_cast<unsigned char>(right);
        })) {
            wire.policy.Reject(AnimationAssetErrors::Malformed);
            return;
        }
        value = std::move(normalized);
    }

    void Transfer(Writer &wire, const Assets::AssetTypeId &value) {
        const auto &text = value.Value();
        if (!wire.policy.Allocate(text.size()))
            return;
        Transfer(wire, std::string{text});
    }

    void Transfer(Reader &wire, Assets::AssetTypeId &value) {
        std::string text;
        Transfer(wire, text);
        if (!wire.policy.Good())
            return;
        if (!wire.policy.Allocate(text.size()))
            return;
        auto type = Assets::AssetTypeId::Parse(text);
        if (type.HasError())
            wire.policy.Reject(AnimationAssetErrors::Malformed);
        else
            value = std::move(type).Value();
    }
}  // namespace Horo::Animation::CookedInternal
