#pragma once

/**
 * @file PhysicsDiagnostics.h
 * @brief Backend-neutral bounded Physics diagnostic evidence.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace Horo::Physics {
    /** @brief Maximum owned context entries retained by one Physics diagnostic record. */
    inline constexpr std::size_t MaximumPhysicsDiagnosticContextEntries = 8;
    /** @brief Maximum UTF-8 byte count admitted for one Physics diagnostic message. */
    inline constexpr std::size_t MaximumPhysicsDiagnosticMessageBytes = 512;

    /** @brief Stable Physics failure area used for presentation and filtering. */
    enum class PhysicsDiagnosticCategory : std::uint8_t {
        Configuration,
        Cook,
        Runtime,
        Query,
        Event,
        Lifecycle,
    };

    /** @brief Closed typed vocabulary for safe Physics diagnostic context. */
    enum class PhysicsDiagnosticContextKey : std::uint8_t {
        World,
        Body,
        Shape,
        Constraint,
        Asset,
        SceneGeneration,
        SimulationTick,
        QuerySnapshotGeneration,
        OperationSequence,
        RequestedCount,
        Capacity,
        SceneEntity,
    };

    /** @brief Owned scalar or stable Horo identity allowed in diagnostic context. */
    using PhysicsDiagnosticContextValue =
        std::variant<std::uint64_t, PhysicsWorldId, BodyHandle, ShapeHandle, ConstraintHandle, Assets::AssetId>;

    /** @brief One typed context field; records require fields in strictly increasing key order. */
    struct PhysicsDiagnosticContextEntry final {
        PhysicsDiagnosticContextKey key{PhysicsDiagnosticContextKey::World};
        PhysicsDiagnosticContextValue value;
    };

    /**
     * @brief Owned inert Physics failure evidence for result, observability and support projections.
     *
     * This value owns its message and bounded context. It carries no native identifiers, resource
     * leases, store references, logging authority or runtime mutation capability.
     */
    struct PhysicsDiagnosticRecord final {
        std::uint32_t schemaVersion{1};
        PhysicsDiagnosticCategory category{PhysicsDiagnosticCategory::Runtime};
        DiagnosticCode code;
        DiagnosticSeverity severity{DiagnosticSeverity::Error};
        std::string message;
        std::array<PhysicsDiagnosticContextEntry, MaximumPhysicsDiagnosticContextEntries> context;
        std::uint8_t contextCount{};
    };

    /**
     * @brief Returns the stable dotted presentation name derived from a known category.
     * @param category Closed category value to render.
     * @return Stable process-lifetime name, or an empty view for an unknown enum representation.
     */
    [[nodiscard]] std::string_view PhysicsDiagnosticCategoryName(PhysicsDiagnosticCategory category) noexcept;

    /**
     * @brief Creates bounded owned diagnostic evidence from one canonical Physics error.
     * @param category Closed Physics failure area; its stable name is derived, never parsed as input.
     * @param error Canonical `horo.physics` operation error whose code is mapped explicitly.
     * @param context Borrowed fields in strictly increasing key order; copied on success only.
     * @return Owned record, or PhysicsErrors::DescriptorInvalid/OperationUnsupported for malformed,
     * foreign, unknown, oversized, unordered, duplicate or type-incompatible input.
     * @pre Control, validation or adapter boundary; record/error construction may allocate.
     * @post Inputs are unchanged. Failure publishes no partial record, log, event or native state.
     */
    [[nodiscard]] Result<PhysicsDiagnosticRecord> MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory category, const Error &error,
                                                                              std::span<const PhysicsDiagnosticContextEntry> context = {});
}  // namespace Horo::Physics
