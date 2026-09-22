#include "UiStyleInternal.h"

#include <type_traits>

namespace Horo::Runtime::Ui::StyleInternal {
    namespace {
        template <typename Identity> void HashIdentity(std::uint64_t &hash, const Identity &identity) noexcept {
            for (const std::uint8_t byte : identity.Bytes()) {
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
        }

        void HashBytes(std::uint64_t &hash, const std::uint8_t *bytes, const std::size_t count) noexcept {
            for (std::size_t index = 0; index < count; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ULL;
            }
        }

        void HashValue(std::uint64_t &hash, const UiStyleValue &value) noexcept {
            hash ^= static_cast<std::uint8_t>(UiStyleValueCategoryOf(value));
            hash *= 1099511628211ULL;
            std::visit([&hash](const auto &typed) noexcept {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value, UiStyleColor>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.red), sizeof(float));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.green), sizeof(float));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.blue), sizeof(float));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.alpha), sizeof(float));
                    hash ^= static_cast<std::uint8_t>(typed.role);
                } else if constexpr (std::is_same_v<Value, UiStyleDimension>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                } else if constexpr (std::is_same_v<Value, UiStyleTypography>) {
                    HashIdentity(hash, typed.family);
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.weight), sizeof(typed.weight));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.stretch), sizeof(typed.stretch));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.style), sizeof(typed.style));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.size), sizeof(typed.size));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.lineHeight), sizeof(typed.lineHeight));
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.letterSpacing), sizeof(typed.letterSpacing));
                } else if constexpr (std::is_same_v<Value, UiStyleImage>) {
                    HashIdentity(hash, typed.asset);
                    hash ^= static_cast<std::uint8_t>(typed.fit);
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(typed.nineSlice.data()), sizeof(typed.nineSlice));
                    HashValue(hash, UiStyleValue{typed.tint});
                } else if constexpr (std::is_same_v<Value, UiStyleShape>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed), sizeof(typed));
                } else if constexpr (std::is_same_v<Value, UiStyleScalar>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                } else if constexpr (std::is_same_v<Value, UiStyleEnumValue>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                } else if constexpr (std::is_same_v<Value, UiStyleMotionReference>) {
                    HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed), sizeof(typed));
                }
            }, value);
        }

        void HashSource(std::uint64_t &hash, const UiStyleValueSource &source) noexcept {
            hash ^= source.referencesToken ? 1U : 0U;
            hash *= 1099511628211ULL;
            if (source.referencesToken) {
                HashIdentity(hash, source.token.asset);
                HashIdentity(hash, source.token.id);
            } else {
                HashValue(hash, source.literal);
            }
        }

        void HashAssignment(std::uint64_t &hash, const UiStyleAssignment &assignment) noexcept {
            HashIdentity(hash, assignment.property);
            hash ^= assignment.sealed ? 1U : 0U;
            hash *= 1099511628211ULL;
            HashSource(hash, assignment.value);
        }

        [[nodiscard]] std::uint64_t HashAssignments(const std::span<const UiStyleAssignment> assignments) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const auto &assignment : assignments)
                HashAssignment(hash, assignment);
            return hash;
        }
    }  // namespace

    std::uint64_t HashElementContent(const UiStyleElementInput &input) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        HashIdentity(hash, input.asset);
        HashIdentity(hash, input.typeClass.asset);
        HashIdentity(hash, input.typeClass.id);
        for (const auto &classReference : input.classes) {
            HashIdentity(hash, classReference.asset);
            HashIdentity(hash, classReference.id);
        }
        hash ^= HashAssignments(input.inlineProperties);
        hash *= 1099511628211ULL;
        hash ^= HashAssignments(input.policyProperties);
        hash *= 1099511628211ULL;
        return hash;
    }

    std::uint64_t HashElementState(const UiVisualStateMask state) noexcept {
        return static_cast<std::uint64_t>(state.bits) * 1099511628211ULL;
    }
}  // namespace Horo::Runtime::Ui::StyleInternal
