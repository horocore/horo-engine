#pragma once

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveMigration.h"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::SaveMigrationRegistryDetail {
    struct SnapshotStorage final {
        std::vector<SaveMigrationDefinition> definitions;
        Sha256Digest identity;
    };
}  // namespace Horo::Runtime::SaveMigrationRegistryDetail

namespace Horo::Runtime::SaveMigrationDetail {
    enum class VersionAdmission : std::uint8_t {
        Direct,
        Migration,
    };

    struct StepView final {
        SaveMigrationId id;
        SaveMigrationAxis axis{SaveMigrationAxis::ArchiveFormat};
        SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
        std::uint32_t from{};
        std::uint32_t to{};
        std::optional<SaveParticipantId> participant;
        const SaveMigrationFn *migrate{};
        const std::vector<SaveMigrationId> *equivalentSequentialSteps{};
        std::uint64_t estimatedWork{};
    };

    [[nodiscard]] inline Error MigrationError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return MakeError(descriptor, std::move(message));
    }

    [[nodiscard]] inline bool IsCanonicalMigrationIdentity(const std::string_view value) noexcept {
        if (value.empty() || value.size() > MaximumSaveMigrationIdentityBytes)
            return false;
        if (!((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= '0' && value.front() <= '9')))
            return false;
        return std::ranges::all_of(value, [](const char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                   character == '_' || character == '-';
        });
    }

    [[nodiscard]] inline std::string AxisName(const SaveMigrationAxis axis) {
        switch (axis) {
            case SaveMigrationAxis::ArchiveFormat:
                return "archive-format";
            case SaveMigrationAxis::SaveSchema:
                return "save-schema";
            case SaveMigrationAxis::ParticipantSchema:
                return "participant-schema";
            case SaveMigrationAxis::ProductCompatibility:
                return "product-compatibility";
            case SaveMigrationAxis::Count:
                break;
        }
        return "unknown-axis";
    }

    [[nodiscard]] inline std::string StepScope(const StepView &step) {
        if (step.participant)
            return std::format("{} participant={}", AxisName(step.axis), step.participant->Value());
        return AxisName(step.axis);
    }

    [[nodiscard]] inline StepView View(const SaveMigrationDefinition &definition) {
        return std::visit([](const auto &step) -> StepView {
            using Step = std::decay_t<decltype(step)>;
            if constexpr (std::is_same_v<Step, ArchiveMigrationStep>) {
                return {.id = step.id,
                        .axis = SaveMigrationAxis::ArchiveFormat,
                        .kind = step.kind,
                        .from = step.from.Value(),
                        .to = step.to.Value(),
                        .participant = std::nullopt,
                        .migrate = &step.migrate,
                        .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                        .estimatedWork = step.estimatedWork};
            } else if constexpr (std::is_same_v<Step, SaveSchemaMigrationStep>) {
                return {.id = step.id,
                        .axis = SaveMigrationAxis::SaveSchema,
                        .kind = step.kind,
                        .from = step.from.Value(),
                        .to = step.to.Value(),
                        .participant = std::nullopt,
                        .migrate = &step.migrate,
                        .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                        .estimatedWork = step.estimatedWork};
            } else {
                return {.id = step.id,
                        .axis = SaveMigrationAxis::ParticipantSchema,
                        .kind = step.kind,
                        .from = step.from.Value(),
                        .to = step.to.Value(),
                        .participant = step.participant,
                        .migrate = &step.migrate,
                        .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                        .estimatedWork = step.estimatedWork};
            }
        }, definition);
    }

    [[nodiscard]] inline bool SameParticipant(const std::optional<SaveParticipantId> &left,
                                              const std::optional<SaveParticipantId> &right) noexcept {
        return left == right;
    }

    [[nodiscard]] inline bool MatchesScope(const StepView &step, const SaveMigrationAxis axis,
                                           const std::optional<SaveParticipantId> &participant) noexcept {
        return step.axis == axis && SameParticipant(step.participant, participant);
    }

    [[nodiscard]] inline std::string EdgeKey(const StepView &step) {
        return std::format("{}|{}|{}|{}|{}", static_cast<unsigned>(step.axis),
                           step.participant ? step.participant->Value() : std::string_view{}, static_cast<unsigned>(step.kind), step.from,
                           step.to);
    }

    [[nodiscard]] inline bool StepOrder(const SaveMigrationDefinition &left, const SaveMigrationDefinition &right) {
        const StepView lhs = View(left);
        const StepView rhs = View(right);
        if (lhs.axis != rhs.axis)
            return lhs.axis < rhs.axis;
        if (lhs.participant != rhs.participant)
            return lhs.participant < rhs.participant;
        if (lhs.from != rhs.from)
            return lhs.from < rhs.from;
        if (lhs.to != rhs.to)
            return lhs.to < rhs.to;
        if (lhs.kind != rhs.kind)
            return lhs.kind < rhs.kind;
        return lhs.id.value < rhs.id.value;
    }

    [[nodiscard]] inline bool ValidLimits(const SaveMigrationLimits &limits) noexcept {
        return limits.maximumDefinitions != 0 && limits.maximumPlanSteps != 0 && limits.maximumParticipants != 0 &&
               limits.maximumArchiveBytes != 0 && limits.maximumParticipantPayloadBytes != 0 && limits.maximumTotalPayloadBytes != 0 &&
               limits.maximumParticipantPayloadBytes <= limits.maximumTotalPayloadBytes;
    }

    template <typename Tag> [[nodiscard]] inline bool IsValidSupport(const SaveVersionSupport<Tag> &support) noexcept {
        if (!support.direct.minimum.IsValid() || !support.direct.maximum.IsValid() || support.direct.minimum > support.direct.maximum)
            return false;
        if (!support.migrationSource)
            return true;
        const auto &migration = *support.migrationSource;
        if (!migration.minimum.IsValid() || !migration.maximum.IsValid() || migration.minimum > migration.maximum)
            return false;
        return migration.maximum < support.direct.minimum || support.direct.maximum < migration.minimum;
    }

    [[nodiscard]] inline bool ValidParticipantPolicy(const SaveCompatibilityPolicy &policy) noexcept {
        if (!std::ranges::is_sorted(policy.participants, {}, &SaveParticipantCompatibility::participant))
            return false;
        for (std::size_t index = 0; index < policy.participants.size(); ++index) {
            const auto &entry = policy.participants[index];
            if (!entry.participant.IsValid() || !IsValidSupport(entry.versions) ||
                (index != 0 && policy.participants[index - 1].participant == entry.participant))
                return false;
        }
        return true;
    }

    template <typename Tag>
    [[nodiscard]] inline Result<VersionAdmission> ClassifyVersion(const SaveVersion<Tag> version, const SaveVersionSupport<Tag> &support,
                                                                  const SaveMigrationAxis axis) {
        if (version > support.direct.maximum)
            return Result<VersionAdmission>::Failure(
                MigrationError(SaveErrors::MigrationUnsupportedNewer,
                               std::format("{} source version {} is newer than supported writer version {}.", AxisName(axis),
                                           version.Value(), support.direct.maximum.Value())));
        if (support.direct.Contains(version))
            return Result<VersionAdmission>::Success(VersionAdmission::Direct);
        if (support.migrationSource && support.migrationSource->Contains(version))
            return Result<VersionAdmission>::Success(VersionAdmission::Migration);
        return Result<VersionAdmission>::Failure(
            MigrationError(SaveErrors::MigrationSourceUnsupported,
                           std::format("{} source version {} is outside the declared direct and migration-source ranges.", AxisName(axis),
                                       version.Value())));
    }

    [[nodiscard]] inline const SaveParticipantCompatibility *FindPolicyParticipant(const SaveCompatibilityPolicy &policy,
                                                                                   const SaveParticipantId &participant) noexcept {
        const auto found = std::ranges::lower_bound(policy.participants, participant, {}, &SaveParticipantCompatibility::participant);
        return found != policy.participants.end() && found->participant == participant ? &*found : nullptr;
    }

    [[nodiscard]] inline const SaveMigrationParticipantState *FindStateParticipant(const SaveMigrationState &state,
                                                                                   const SaveParticipantId &participant) noexcept {
        const auto found = std::ranges::lower_bound(state.participants, participant, {}, &SaveMigrationParticipantState::participant);
        return found != state.participants.end() && found->participant == participant ? &*found : nullptr;
    }

    inline void AppendVersion(std::string &output, const std::uint32_t value) {
        output.push_back(static_cast<char>(value));
        output.push_back(static_cast<char>(value >> 8U));
        output.push_back(static_cast<char>(value >> 16U));
        output.push_back(static_cast<char>(value >> 24U));
    }

    inline void AppendCount(std::string &output, const std::uint64_t value) {
        for (std::size_t index = 0; index < sizeof(value); ++index)
            output.push_back(static_cast<char>(value >> (index * 8U)));
    }

    inline void AppendText(std::string &output, const std::string_view value) {
        AppendCount(output, value.size());
        output.append(value);
    }

    inline void AppendStep(std::string &output, const StepView &step) {
        output.push_back(static_cast<char>(step.axis));
        output.push_back(static_cast<char>(step.kind));
        AppendVersion(output, step.from);
        AppendVersion(output, step.to);
        AppendCount(output, step.estimatedWork);
        AppendText(output, step.id.value);
        AppendText(output, step.participant ? step.participant->Value() : std::string_view{});
        AppendCount(output, step.equivalentSequentialSteps->size());
        for (const SaveMigrationId &identity : *step.equivalentSequentialSteps)
            AppendText(output, identity.value);
    }

    [[nodiscard]] inline std::span<const std::byte> Bytes(const std::string &value) noexcept {
        return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
    }

    inline void AppendDigest(std::string &output, const Sha256Digest &digest) {
        for (const std::uint8_t byte : digest.bytes)
            output.push_back(static_cast<char>(byte));
    }

    [[nodiscard]] Result<void> ValidateDefinition(const SaveMigrationDefinition &definition, const SaveMigrationLimits &limits);
    [[nodiscard]] Result<void> ValidateCatalog(std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationLimits &limits);
    [[nodiscard]] Result<void> ValidateSupport(const SaveMigrationSupportDescriptor &support, const SaveMigrationLimits &limits);
    [[nodiscard]] Result<void> ValidateState(const SaveMigrationState &state, const SaveMigrationLimits &limits);
    [[nodiscard]] Result<void> ValidateTargetParticipants(const SaveMigrationCandidate &candidate, const SaveMigrationPlan &plan);

    [[nodiscard]] Result<std::vector<const SaveMigrationDefinition *>> BuildCheckpointRoute(
        const std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationSupportDescriptor &support, SaveMigrationAxis axis,
        const std::optional<SaveParticipantId> &participant, std::uint32_t source, std::uint32_t target, const SaveMigrationLimits &limits);
    [[nodiscard]] Result<void> AppendRoute(SaveMigrationPlan &plan, const std::vector<const SaveMigrationDefinition *> &route,
                                           const SaveMigrationLimits &limits);
    [[nodiscard]] Sha256Digest CatalogIdentity(const std::vector<SaveMigrationDefinition> &definitions);
    [[nodiscard]] Sha256Digest RouteIdentity(const SaveMigrationPlan &plan);
    [[nodiscard]] Result<SaveMigrationPlan> PlanSnapshot(const SaveMigrationRegistrySnapshot &snapshot, const SaveMigrationSource &source,
                                                         const SaveMigrationSupportDescriptor &support, const SaveMigrationLimits &limits,
                                                         const std::vector<SaveMigrationDefinition> &definitions,
                                                         const Sha256Digest &catalogIdentity);
}  // namespace Horo::Runtime::SaveMigrationDetail
