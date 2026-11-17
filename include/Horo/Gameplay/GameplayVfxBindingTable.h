#pragma once

/** @file GameplayVfxBindingTable.h
 * @brief Immutable application-owned semantic presentation bindings above the canonical VFX queue.
 */

#include "Horo/Assets/AssetProvider.h"
#include "Horo/Gameplay/Behavior.h"
#include "Horo/Vfx/VfxSpawnPreparation.h"

#include <memory>
#include <string_view>
#include <vector>

namespace Horo::Gameplay {
    namespace GameplayVfxErrors {
        /** @brief Source shape, field kind or finite numeric encoding is malformed. */
        extern const ErrorCodeDescriptor Malformed;
        /** @brief An object contains duplicate decoded field names. */
        extern const ErrorCodeDescriptor DuplicateField;
        /** @brief Source version requires an explicit unsupported migration. */
        extern const ErrorCodeDescriptor VersionUnsupported;
        /** @brief A compiled byte, node, nesting, channel or fan-out limit is exceeded. */
        extern const ErrorCodeDescriptor LimitExceeded;
        /** @brief Semantic selectors, stable destinations or policies conflict. */
        extern const ErrorCodeDescriptor BindingInvalid;
        /** @brief A referenced effect has no record in the captured registry. */
        extern const ErrorCodeDescriptor MissingAsset;
        /** @brief A referenced record is not an explicitly typed effect asset. */
        extern const ErrorCodeDescriptor AssetTypeMismatch;
        /** @brief A referenced effect has no available payload or genuine prepared recipe. */
        extern const ErrorCodeDescriptor AssetUnavailable;
        /** @brief Source or target scalar channels, schemas or ranges conflict. */
        extern const ErrorCodeDescriptor ParameterMismatch;
        /** @brief Cold application metadata does not match its actual owner/palette. */
        extern const ErrorCodeDescriptor ScopeInvalid;
        /** @brief Bounded cold preparation storage could not be allocated. */
        extern const ErrorCodeDescriptor AllocationFailed;
        /** @brief Cold preparation was cooperatively cancelled before publication. */
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace GameplayVfxErrors

    /** @brief Finite source/cook bounds; runtime storage is prepared before any behavior callback. */
    struct GameplayVfxBindingLimits final {
        static constexpr std::size_t SourceBytes = 1024U * 1024U;
        static constexpr std::size_t JsonDepth = 16;
        static constexpr std::size_t JsonNodes = 1'000'000;
        static constexpr std::size_t Bindings = 128;
        static constexpr std::size_t TextBytes = 160;
    };

    /** @brief Cooked registry-backed effect tag; knowing its number never grants semantic spawn permission. */
    struct GameplayVfxEffectTag final {
        std::uint64_t value{};

        /** @brief Checks representation only. @return True for a nonzero registered-tag candidate; no permission is granted. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        constexpr auto operator<=>(const GameplayVfxEffectTag &) const noexcept = default;
    };

    /** @brief One declared semantic scalar copied to one asset-specific GameplayInput channel. */
    struct GameplayVfxParameterRoute final {
        std::uint16_t source{};
        std::uint16_t target{};
    };

    /** @brief Finite authored layer; constants and routes have disjoint, canonical target channels. */
    struct GameplayVfxLayer final {
        Assets::AssetId effectAsset;
        float scale{1.0F};
        Vfx::VfxParameterBlock constants;
        std::vector<GameplayVfxParameterRoute> routes;
    };

    /** @brief One exact semantic event OR tag binding; application data alone selects effect assets. */
    struct GameplayVfxBinding final {
        std::uint64_t destination{}; /**< Stable authored mapping identity, unique and increasing in the document. */
        GameplayEventTypeId event;   /**< Existing gameplay semantic identity; empty only for tag bindings. */
        GameplayVfxEffectTag tag;    /**< Exactly one selector is populated. */
        std::uint32_t schemaVersion{1};
        std::uint64_t payloadSchema{};
        std::vector<Vfx::VfxSpawnParameter> inputs;
        Vfx::VfxRequirementClass requirement{Vfx::VfxRequirementClass::Cosmetic};
        std::vector<GameplayVfxLayer> layers;
    };

    /** @brief Strict version-one source document; replacement generations increase under the application owner. */
    struct GameplayVfxBindingDocument final {
        std::uint32_t version{1};
        std::uint64_t generation{};
        std::vector<GameplayVfxBinding> bindings;
    };

    /** @brief Copied semantic placement/arguments; it contains no asset selection or owner authorization. */
    struct GameplayVfxEventPayload final {
        Vfx::VfxSpawnTransform transform;
        std::optional<Math::Vec3> impactNormal;
        float scale{1.0F};
        Vfx::VfxParameterBlock parameters;
    };

    /** @brief Mapping-generation metadata, never a capability; stale keys cannot silently name another semantic binding. */
    struct GameplayVfxEventKey final {
        std::uint64_t generation{};
        std::uint64_t destination{};
        constexpr auto operator<=>(const GameplayVfxEventKey &) const noexcept = default;
    };

    /** @brief Allocation-free lookup result, including the declared semantic payload schema. */
    struct GameplayVfxLookup final {
        Vfx::VfxSpawnStatus status{Vfx::VfxSpawnStatus::UnknownEvent};
        GameplayVfxEventKey key;
        std::uint64_t payloadSchema{};
    };

    /** @brief Complete bounded resolution outcome; no World is mutated by resolving a command. */
    struct GameplayVfxResolveOutcome final {
        Vfx::VfxSpawnStatus status{Vfx::VfxSpawnStatus::InvalidRequest};
        std::uint32_t layers{};
    };

    namespace Detail {
        struct GameplayVfxCompiledBindings;
        struct GameplayVfxBindingAccess;
    }  // namespace Detail

    /** @brief Cooked immutable application policy; ordinary behavior clients receive a scoped context rather than this table. */
    class GameplayVfxBindingTable final {
    public:
        /** @brief Releases immutable cold policy storage after all captured semantic commands retire. */
        ~GameplayVfxBindingTable();
        GameplayVfxBindingTable(const GameplayVfxBindingTable &) = delete;
        GameplayVfxBindingTable &operator=(const GameplayVfxBindingTable &) = delete;

        /** @brief Cold validation against one pinned registry and genuinely prepared World asset palette.
         * @param document Complete bounded source data. @param registry Exact immutable asset snapshot retained by the table.
         * @param provider Existing cold asset provider, borrowed only during availability validation.
         * @param prepared Actual World recipe/schema facts, copied before return.
         * @param cancellation Cooperative cold cancellation, never retained by gameplay.
         * @return Immutable table or typed malformed, missing, wrong-type, unavailable, schema or allocation failure.
         * Referenced effect records must have type core.vfx.effect; this does not imply a graph compiler or GPU recipe.
         */
        [[nodiscard]] static Result<std::shared_ptr<const GameplayVfxBindingTable>> Cook(GameplayVfxBindingDocument document,
                                                                                         Assets::AssetRegistrySnapshot registry,
                                                                                         const Assets::IAssetProvider &provider,
                                                                                         std::span<const Vfx::VfxSpawnAssetInfo> prepared,
                                                                                         const CancellationToken &cancellation = {});

        /** @brief Returns the immutable nonzero policy generation. @return Captured generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Looks up an exact registered gameplay identity without constructing strings.
         * @param event Borrowed canonical semantic text, compared exactly. @param schemaVersion Declared source schema revision.
         * @return Typed known/unknown/schema outcome and generation-fenced key; no permission is granted.
         */
        [[nodiscard]] GameplayVfxLookup FindEvent(std::string_view event, std::uint32_t schemaVersion = 1) const noexcept;
        /** @brief Looks up one registry-backed tag without interpreting its meaning.
         * @param tag Authored stable tag. @param schemaVersion Declared semantic schema revision.
         * @return Typed known/unknown/schema outcome and generation-fenced key.
         */
        [[nodiscard]] GameplayVfxLookup FindTag(GameplayVfxEffectTag tag, std::uint32_t schemaVersion = 1) const noexcept;
        /** @brief Resolves all authored layers into caller-owned scratch using the captured generation.
         * @param key Exact looked-up generation/destination. @param payload Copied semantic placement and typed scalar arguments.
         * @param basis Genuine application routing/occurrence evidence; semantic fields are overwritten from the cooked table.
         * @param output Finite caller-owned layer storage; unchanged on rejection.
         * @return Complete layer count or typed stale/schema/placement/capacity rejection, without allocation or World mutation.
         */
        [[nodiscard]] GameplayVfxResolveOutcome Resolve(GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                                        Vfx::VfxSpawnRequest basis, std::span<Vfx::VfxSpawnRequest> output) const noexcept;

    private:
        friend struct Detail::GameplayVfxBindingAccess;
        explicit GameplayVfxBindingTable(std::unique_ptr<Detail::GameplayVfxCompiledBindings> state);
        std::unique_ptr<const Detail::GameplayVfxCompiledBindings> state_;
    };

    /** @brief Decodes strict version-one bounded JSON; duplicate/unknown fields and noncanonical ordering fail.
     * @param bytes Complete UTF-8 source under the compiled byte/node/depth/work limits.
     * @return Owned source document or typed malformed/duplicate/version/limit failure; performs no I/O.
     */
    [[nodiscard]] Result<GameplayVfxBindingDocument> DecodeGameplayVfxBindings(std::string_view bytes);
    /** @brief Encodes the same canonical source schema after complete structural validation.
     * @param document Bounded source data; no active table is changed.
     * @return Canonical bounded JSON or typed structural/limit failure; performs no I/O.
     */
    [[nodiscard]] Result<std::string> EncodeGameplayVfxBindings(const GameplayVfxBindingDocument &document);
}  // namespace Horo::Gameplay
