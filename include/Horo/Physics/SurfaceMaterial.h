#pragma once
/** @file SurfaceMaterial.h
 * @brief Project-owned semantic surfaces and immutable Physics projection
 * evidence.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Physics {
    namespace Detail {
        struct PhysicsStateSchemaAccounting;
    }

    namespace SurfaceErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Limit;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Stale;
    }  // namespace SurfaceErrors

    /** @brief Persistent project semantic identity, distinct from every physical or
     * media asset identity. */
    class SurfaceMaterialId final {
    public:
        SurfaceMaterialId() = default;
        /** @brief Parse exact lowercase nonzero UUID wire text. @return Identity or
         * SurfaceErrors::Invalid. */
        [[nodiscard]] static Result<SurfaceMaterialId> Parse(std::string_view text);

        /** @brief Capture identity bytes without resolving assets. Zero remains an
         * invalid value. */
        [[nodiscard]] static constexpr SurfaceMaterialId FromBytes(const std::array<std::uint8_t, 16> &bytes) noexcept {
            return SurfaceMaterialId{Assets::AssetId::FromBytes(bytes)};
        }

        [[nodiscard]] bool IsValid() const noexcept {
            return value_.IsValid();
        }

        [[nodiscard]] const std::array<std::uint8_t, 16> &Bytes() const noexcept {
            return value_.Bytes();
        }

        [[nodiscard]] std::string ToString() const {
            return value_.ToString();
        }

        [[nodiscard]] constexpr auto operator<=>(const SurfaceMaterialId &) const noexcept = default;

    private:
        explicit constexpr SurfaceMaterialId(Assets::AssetId value) noexcept : value_(value) {}

        Assets::AssetId value_;
    };

    inline constexpr std::size_t MaximumSurfaceCatalogEntries = 256;
    inline constexpr std::size_t MaximumSurfaceDisplayNameBytes = 128;

    /** @brief Display metadata only; coefficients, media and gameplay effects
     * remain separate owners. */
    struct SurfaceCatalogEntry final {
        SurfaceMaterialId id;
        std::string displayName;
        bool operator==(const SurfaceCatalogEntry &) const = default;
    };

    /** @brief Candidate portable project catalogue; an empty legacy catalogue
     * supplies no implicit surface. */
    struct SurfaceCatalogDescriptor final {
        std::string projectId;
        std::uint64_t revision{};
        std::vector<SurfaceCatalogEntry> surfaces;
    };

    /** @brief Immutable value snapshot created only after complete bounded
     * validation and canonical sorting. */
    class SurfaceCatalogSnapshot final {
    public:
        SurfaceCatalogSnapshot() = default;
        /** @brief Validate and own canonical sorted entries. @param descriptor
         * Candidate copied/moved by caller.
         * @param cancellation Cooperative control-boundary cancellation. @return
         * Complete snapshot or stable validation error.
         * @details No filesystem, solver or application service is consulted. Work
         * and retained bytes have fixed ceilings.
         */
        [[nodiscard]] static Result<SurfaceCatalogSnapshot> Create(SurfaceCatalogDescriptor descriptor,
                                                                   const CancellationToken &cancellation = {});

        [[nodiscard]] bool IsValid() const noexcept {
            return value_.revision != 0;
        }

        [[nodiscard]] const std::string &ProjectId() const noexcept {
            return value_.projectId;
        }

        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return value_.revision;
        }

        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept {
            return fingerprint_;
        }

        [[nodiscard]] std::span<const SurfaceCatalogEntry> Entries() const noexcept {
            return value_.surfaces;
        }

        [[nodiscard]] const SurfaceCatalogEntry *Find(SurfaceMaterialId id) const noexcept;

        [[nodiscard]] bool Contains(SurfaceMaterialId id) const noexcept {
            return Find(id) != nullptr;
        }

    private:
        friend struct Detail::PhysicsStateSchemaAccounting;
        SurfaceCatalogDescriptor value_;
        Sha256Digest fingerprint_{};
    };

    /** @brief Explicit collider-local semantic binding; never substitutes for a
     * physical material binding. */
    struct PhysicsSurfaceBinding final {
        PhysicsMaterialSlotId slot;
        SurfaceMaterialId surface;
        constexpr auto operator<=>(const PhysicsSurfaceBinding &) const noexcept = default;
    };

    /** @brief Exact semantic provenance copied beside world/body/shape/subshape and
     * physical slot evidence. */
    struct PhysicsSurfaceEvidence final {
        SurfaceMaterialId surface;
        std::uint64_t catalogRevision{};
        Sha256Digest catalogFingerprint{};
        std::uint64_t mappingGeneration{};
        std::uint64_t collider{}; /**< Exact authored collider slot within the
                                     enclosing body; zero for source-free fixtures. */
        constexpr auto operator<=>(const PhysicsSurfaceEvidence &) const noexcept = default;
    };

    /** @brief Immutable project capture retained for one world; absent catalogue
     * means semantic capability unavailable. */
    struct PhysicsSurfaceWorldBinding final {
        std::shared_ptr<const SurfaceCatalogSnapshot> catalog;
        std::uint64_t mappingGeneration{};
        std::optional<SurfaceMaterialId> defaultSurface; /**< Explicit Project default, never inferred from
                                                            physical coefficients. */
    };

    /** @brief Validate exact presence/generation association before world
     * admission. */
    [[nodiscard]] Result<void> ValidatePhysicsSurfaceWorldBinding(const PhysicsSurfaceWorldBinding &binding);
    /** @brief Copy immutable catalogue association at admission; no lookup occurs
     * in solver callbacks.
     * @pre binding.catalog is valid and mappingGeneration is nonzero.
     * @details This does not prove world residency or authorize a query. Live use
     * additionally requires the exact enclosing body/shape identities and a
     * currently admitted query-capability publication revision. Copied association
     * remains historical evidence after structural replacement or world retirement.
     */
    [[nodiscard]] PhysicsSurfaceEvidence MakePhysicsSurfaceEvidence(const PhysicsSurfaceWorldBinding &binding,
                                                                    SurfaceMaterialId id) noexcept;
    /** @brief Fixed-field identity of the admitted catalogue, generation and
     * explicit default policy.
     * @param binding Captured world policy. @return Deterministic identity; no I/O,
     * allocation or mutable lookup.
     */
    [[nodiscard]] Sha256Digest PhysicsSurfaceWorldFingerprint(const PhysicsSurfaceWorldBinding &binding) noexcept;
    /** @brief Allocation-free historical catalogue/mapping association check for
     * copied semantic evidence.
     * @details Success is never current-world admission. Use the outer exact
     * body/shape identities and query-capability publication revision for live use;
     * those fences reject retired or structurally replaced mappings.
     */
    [[nodiscard]] bool MatchesPhysicsSurfaceEvidence(const PhysicsSurfaceWorldBinding &binding,
                                                     const PhysicsSurfaceEvidence &evidence) noexcept;
}  // namespace Horo::Physics
