#include "Horo/Physics/PhysicsDiagnostics.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <optional>
#include <utility>

namespace Horo::Physics {
    namespace {
        /** @brief Maps only canonical Physics error descriptors to stable diagnostic identities. */
        std::optional<DiagnosticCode> CanonicalDiagnosticCode(const Error &error) {
            if (error.domain.Value() != "horo.physics")
                return std::nullopt;

            return DiagnosticCodeForDeclaredError(error, "horo.physics", PhysicsErrors::Descriptors());
        }

        /** @brief Converts the complete Foundation severity vocabulary without fallback. */
        std::optional<DiagnosticSeverity> DiagnosticSeverityFor(const ErrorSeverity severity) noexcept {
            switch (severity) {
                case ErrorSeverity::Info:
                    return DiagnosticSeverity::Note;
                case ErrorSeverity::Warning:
                    return DiagnosticSeverity::Warning;
                case ErrorSeverity::Error:
                    return DiagnosticSeverity::Error;
                case ErrorSeverity::Critical:
                    return DiagnosticSeverity::Fatal;
            }
            return std::nullopt;
        }

        /** @brief Returns whether a scalar key requires non-zero generation/order evidence. */
        bool RequiresNonZeroScalar(const PhysicsDiagnosticContextKey key) noexcept {
            using enum PhysicsDiagnosticContextKey;
            return key == SceneGeneration || key == SimulationTick || key == QuerySnapshotGeneration || key == OperationSequence;
        }

        /** @brief Captures one valid world while preserving a single-world context invariant. */
        bool AcceptWorld(const PhysicsWorldId world, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            if (!world.IsValid() || (contextWorld.has_value() && *contextWorld != world))
                return false;
            contextWorld = world;
            return true;
        }

        /** @brief Validates a typed handle and captures its valid owning world. */
        template <typename Handle>
        bool AcceptHandle(const PhysicsDiagnosticContextValue &value, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            if (const auto *handle = std::get_if<Handle>(&value))
                return handle->IsValid() && AcceptWorld(handle->world, contextWorld);
            return false;
        }

        /** @brief Validates the world-valued context entry. */
        bool ValidateWorldEntry(const PhysicsDiagnosticContextValue &value, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            if (const auto *world = std::get_if<PhysicsWorldId>(&value))
                return AcceptWorld(*world, contextWorld);
            return false;
        }

        /** @brief Validates the asset-valued context entry. */
        bool ValidateAssetEntry(const PhysicsDiagnosticContextValue &value) noexcept {
            if (const auto *asset = std::get_if<Assets::AssetId>(&value))
                return asset->IsValid();
            return false;
        }

        /** @brief Dispatches the closed handle-valued context keys. */
        bool ValidateHandleEntry(const PhysicsDiagnosticContextEntry &entry, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            using enum PhysicsDiagnosticContextKey;
            if (entry.key == Body)
                return AcceptHandle<BodyHandle>(entry.value, contextWorld);
            if (entry.key == Shape)
                return AcceptHandle<ShapeHandle>(entry.value, contextWorld);
            if (entry.key == Constraint)
                return AcceptHandle<ConstraintHandle>(entry.value, contextWorld);
            return false;
        }

        /** @brief Validates one identity-valued context entry. */
        bool ValidateIdentityEntry(const PhysicsDiagnosticContextEntry &entry, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            using enum PhysicsDiagnosticContextKey;
            if (entry.key == World)
                return ValidateWorldEntry(entry.value, contextWorld);
            if (entry.key == Asset)
                return ValidateAssetEntry(entry.value);
            return ValidateHandleEntry(entry, contextWorld);
        }

        /** @brief Validates one scalar-valued context entry. */
        bool ValidateScalarEntry(const PhysicsDiagnosticContextEntry &entry) noexcept {
            if (const auto *value = std::get_if<std::uint64_t>(&entry.value))
                return !RequiresNonZeroScalar(entry.key) || *value != 0;
            return false;
        }

        /** @brief Validates one key/value pair and captures its world identity when applicable. */
        bool ValidateContextEntry(const PhysicsDiagnosticContextEntry &entry, std::optional<PhysicsWorldId> &contextWorld) noexcept {
            using enum PhysicsDiagnosticContextKey;
            if (entry.key <= Asset)
                return ValidateIdentityEntry(entry, contextWorld);
            if (entry.key <= Capacity)
                return ValidateScalarEntry(entry);
            return false;
        }

        /** @brief Validates bounded record-wide inputs before copying evidence. */
        bool ValidateRecordInput(const Error &error, const std::span<const PhysicsDiagnosticContextEntry> context,
                                 const std::optional<DiagnosticCode> &code, const std::optional<DiagnosticSeverity> &severity) noexcept {
            if (!code.has_value() || !severity.has_value())
                return false;
            return !error.message.empty() && error.message.size() <= MaximumPhysicsDiagnosticMessageBytes &&
                   context.size() <= MaximumPhysicsDiagnosticContextEntries;
        }

        /** @brief Validates ordered typed context while preserving a single-world invariant. */
        bool ValidateContext(const std::span<const PhysicsDiagnosticContextEntry> context) noexcept {
            std::optional<PhysicsDiagnosticContextKey> previousKey;
            std::optional<PhysicsWorldId> contextWorld;
            for (const PhysicsDiagnosticContextEntry &entry : context) {
                if ((previousKey.has_value() && entry.key <= *previousKey) || !ValidateContextEntry(entry, contextWorld))
                    return false;
                previousKey = entry.key;
            }
            return true;
        }
    }  // namespace

    /** @copydoc PhysicsDiagnosticCategoryName */
    std::string_view PhysicsDiagnosticCategoryName(const PhysicsDiagnosticCategory category) noexcept {
        switch (category) {
            using enum PhysicsDiagnosticCategory;
            case Configuration:
                return "physics.configuration";
            case Cook:
                return "physics.cook";
            case Runtime:
                return "physics.runtime";
            case Query:
                return "physics.query";
            case Event:
                return "physics.event";
            case Lifecycle:
                return "physics.lifecycle";
        }
        return {};
    }

    /** @copydoc MakePhysicsDiagnosticRecord */
    Result<PhysicsDiagnosticRecord> MakePhysicsDiagnosticRecord(const PhysicsDiagnosticCategory category, const Error &error,
                                                                const std::span<const PhysicsDiagnosticContextEntry> context) {
        if (PhysicsDiagnosticCategoryName(category).empty())
            return Result<PhysicsDiagnosticRecord>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Unknown Physics diagnostic category."));
        const auto code = CanonicalDiagnosticCode(error);
        const auto severity = DiagnosticSeverityFor(error.severity);
        if (!ValidateRecordInput(error, context, code, severity))
            return Result<PhysicsDiagnosticRecord>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics diagnostic evidence is malformed or exceeds its bounded record."));

        if (!ValidateContext(context))
            return Result<PhysicsDiagnosticRecord>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics diagnostic context is unordered, duplicate or malformed."));

        PhysicsDiagnosticRecord record;
        record.category = category;
        record.code = *code;
        record.severity = *severity;
        record.message = error.message;
        record.contextCount = static_cast<std::uint8_t>(context.size());
        for (std::size_t index = 0; index < context.size(); ++index)
            record.context[index] = context[index];
        return Result<PhysicsDiagnosticRecord>::Success(std::move(record));
    }
}  // namespace Horo::Physics
