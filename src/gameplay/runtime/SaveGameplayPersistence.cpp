#include "Horo/Gameplay/SaveGameplayPersistence.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<std::byte, 4> EnvelopeMagic{std::byte{'H'}, std::byte{'G'}, std::byte{'S'}, std::byte{'P'}};
        constexpr std::size_t EnvelopePrefixBytes = EnvelopeMagic.size() + 1U + sizeof(std::uint32_t) + 1U;

        /** @brief Appends the module compatibility version in explicit little-endian order. */
        void AppendVersion(std::vector<std::byte> &bytes, const std::uint32_t version) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::byte>((version >> shift) & 0xffU));
        }

        /** @brief Reads a prevalidated four-byte little-endian compatibility version. */
        [[nodiscard]] std::uint32_t ReadVersion(const std::span<const std::byte> bytes) noexcept {
            std::uint32_t value = 0;
            for (unsigned index = 0; index < 4; ++index)
                value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * 8U);
            return value;
        }

        /** @brief Intersects host admission and descriptor limits before invoking project code. */
        [[nodiscard]] Result<std::uint64_t> CapturePayloadBudget(const GameplayPersistenceDescriptor &descriptor,
                                                                 const CanonicalCaptureContext &context) {
            if (context.participant != descriptor.participant.participant ||
                context.schemaVersion != descriptor.participant.schemaVersion || context.scope != descriptor.participant.scope)
                return Result<std::uint64_t>::Failure(MakeError(SaveErrors::CaptureContextInvalid));
            const auto &moduleId = descriptor.moduleId.Value();
            const std::uint64_t prefix = EnvelopePrefixBytes + moduleId.size();
            const std::uint64_t maximum =
                std::min({descriptor.participant.limits.maximumPayloadBytes, context.admission.participantPayloadBytes,
                          context.admission.operationPayloadBytes, context.admission.maximumCopiedRecordBytes});
            if (moduleId.size() > std::numeric_limits<std::uint8_t>::max() || maximum < prefix ||
                context.admission.participantRecords == 0 || context.admission.operationRecords == 0)
                return Result<std::uint64_t>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            return Result<std::uint64_t>::Success(maximum - prefix);
        }

        /** @brief Encodes owned payload bytes with exact module identity, version and owner authority. */
        [[nodiscard]] std::vector<std::byte> EncodeEnvelope(const GameplayPersistenceDescriptor &descriptor,
                                                            const std::span<const std::byte> payload) {
            const auto &moduleId = descriptor.moduleId.Value();
            std::vector<std::byte> encoded;
            encoded.reserve(EnvelopePrefixBytes + moduleId.size() + payload.size());
            encoded.insert(encoded.end(), EnvelopeMagic.begin(), EnvelopeMagic.end());
            encoded.push_back(static_cast<std::byte>(moduleId.size()));
            AppendVersion(encoded, descriptor.moduleVersion);
            encoded.push_back(static_cast<std::byte>(descriptor.owner));
            for (const char byte : moduleId)
                encoded.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
            encoded.insert(encoded.end(), payload.begin(), payload.end());
            return encoded;
        }

        /** @brief Compares encoded module bytes without aliasing byte-oriented storage as characters. */
        [[nodiscard]] bool HasExactModuleIdentity(const std::span<const std::byte> saved, const std::string &moduleId) noexcept {
            return std::ranges::equal(saved, moduleId, [](const std::byte byte, const char character) {
                return byte == static_cast<std::byte>(static_cast<unsigned char>(character));
            });
        }

        /** @brief Opaque prepared-state projection without gameplay payload access. */
        class PreparedProjection final : public ICanonicalRestorePreparedState {};

        /** @brief Detached receipt destroying source and candidate before releasing its module pin. */
        class GameplayStagedRestore final : public IStagedRestoreParticipant {
        public:
            GameplayStagedRestore(GameplayPersistenceDescriptor descriptor, std::shared_ptr<IGameplayPersistenceSource> source,
                                  std::shared_ptr<void> moduleLease, std::vector<std::byte> encoded) noexcept
                : descriptor_(std::move(descriptor)), moduleLease_(std::move(moduleLease)), source_(std::move(source)),
                  encoded_(std::move(encoded)),
                  requirement_{descriptor_.participant.participant, descriptor_.participant.schemaVersion, descriptor_.participant.scope} {}

            [[nodiscard]] const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
                return requirement_;
            }

            [[nodiscard]] Result<void> Decode(const StagedRestoreContext &) override {
                if (encoded_.size() < EnvelopePrefixBytes || !std::equal(EnvelopeMagic.begin(), EnvelopeMagic.end(), encoded_.begin()) ||
                    encoded_.size() > descriptor_.participant.limits.maximumPayloadBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                if (const std::size_t moduleLength = std::to_integer<unsigned>(encoded_[EnvelopeMagic.size()]);
                    moduleLength == 0 || moduleLength > MaximumSaveParticipantIdBytes ||
                    moduleLength > encoded_.size() - EnvelopePrefixBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                decoded_ = true;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> Validate(const StagedRestoreContext &) override {
                if (!decoded_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                const std::size_t moduleLength = std::to_integer<unsigned>(encoded_[EnvelopeMagic.size()]);
                const auto savedModule = std::span<const std::byte>{encoded_}.subspan(EnvelopePrefixBytes, moduleLength);
                const std::uint32_t savedVersion = ReadVersion(std::span<const std::byte>{encoded_}.subspan(EnvelopeMagic.size() + 1U, 4));
                if (const auto savedOwner =
                        static_cast<GameplayPersistenceOwner>(std::to_integer<unsigned>(encoded_[EnvelopeMagic.size() + 1U + 4U]));
                    !HasExactModuleIdentity(savedModule, descriptor_.moduleId.Value()) || savedVersion != descriptor_.moduleVersion ||
                    savedOwner != descriptor_.owner)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid,
                                                           "Gameplay module identity or durable schema version is incompatible."));
                validated_ = true;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> Instantiate(const StagedRestoreContext &) override {
                if (!validated_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                instantiated_ = true;
                return Result<void>::Success();
            }

            [[nodiscard]] const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
                return instantiated_ ? &projection_ : nullptr;
            }

            [[nodiscard]] Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &) override {
                if (!instantiated_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                const std::size_t moduleLength = std::to_integer<unsigned>(encoded_[EnvelopeMagic.size()]);
                const auto payload = std::span<const std::byte>{encoded_}.subspan(EnvelopePrefixBytes + moduleLength);
                static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
                // Prepare owned failures before project code runs; unwinding never allocates a replacement error.
                return [this, payload, allocationFailure = Result<void>::Failure(MakeError(SaveErrors::RestoreAllocationFailed)),
                        callbackFailure = Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed))]() mutable noexcept {
                    try {
                        auto prepared = source_->PrepareRuntimeState(payload);
                        if (prepared.HasError())
                            return Result<void>::Failure(prepared.ErrorValue());
                        candidate_ = std::move(prepared).Value();
                        if (!candidate_)
                            return Result<void>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
                        return Result<void>::Success();
                    } catch (const std::bad_alloc &) {
                        return std::move(allocationFailure);
                    } catch (...) {
                        // Project modules may throw non-standard exceptions; none may cross the restore boundary.
                        return std::move(callbackFailure);
                    }
                }();
            }

            [[nodiscard]] Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &dependencies) override {
                return FixupReferences(dependencies, {});
            }

            [[nodiscard]] Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &,
                                                       const SaveRestoreReferenceView &references) override {
                if (!candidate_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
                static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
                // Owned fallback construction may throw before project code; fault translation itself never allocates.
                return [this, &references, allocationFailure = Result<void>::Failure(MakeError(SaveErrors::RestoreAllocationFailed)),
                        callbackFailure = Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed))]() mutable noexcept {
                    try {
                        if (auto fixed = candidate_->FixupRuntimeReferences(references); fixed.HasError())
                            return fixed;
                        ready_ = true;
                        return Result<void>::Success();
                    } catch (const std::bad_alloc &) {
                        return std::move(allocationFailure);
                    } catch (...) {
                        return std::move(callbackFailure);
                    }
                }();
            }

            void PublishPrepared() noexcept override {
                if (candidate_ && ready_ && !published_) {
                    candidate_->Publish();
                    published_ = true;
                }
            }

            void RollbackPrepared() noexcept override {
                candidate_.reset();
                encoded_.clear();
                instantiated_ = false;
                ready_ = false;
            }

        private:
            GameplayPersistenceDescriptor descriptor_;
            std::shared_ptr<void> moduleLease_;
            std::shared_ptr<IGameplayPersistenceSource> source_;
            std::vector<std::byte> encoded_;
            StagedRestoreParticipantRequirement requirement_;
            PreparedProjection projection_;
            std::unique_ptr<IPreparedGameplayPersistenceState> candidate_;
            bool decoded_{};
            bool validated_{};
            bool instantiated_{};
            bool ready_{};
            bool published_{};
        };
    }  // namespace

    /** @copydoc GameplayPersistenceAdapter::GameplayPersistenceAdapter */
    GameplayPersistenceAdapter::GameplayPersistenceAdapter(ValidatedConstruction, GameplayPersistenceDescriptor descriptor,
                                                           std::shared_ptr<IGameplayPersistenceSource> source,
                                                           std::shared_ptr<void> moduleLease) noexcept
        : descriptor_(std::move(descriptor)), moduleLease_(std::move(moduleLease)), source_(std::move(source)) {}

    /** @copydoc GameplayPersistenceAdapter::Create */
    Result<std::shared_ptr<GameplayPersistenceAdapter>> GameplayPersistenceAdapter::Create(
        GameplayPersistenceDescriptor descriptor, std::shared_ptr<IGameplayPersistenceSource> source, std::shared_ptr<void> moduleLease) {
        // Failure paths must destroy project code before dropping the incoming unload barrier too.
        auto retainedLease = std::move(moduleLease);
        auto retainedSource = std::move(source);
        if (!IsValidGameplayPersistenceDescriptor(descriptor))
            return Result<std::shared_ptr<GameplayPersistenceAdapter>>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
        if (auto valid = ValidateCanonicalStateParticipantDescriptor(descriptor.participant); valid.HasError())
            return Result<std::shared_ptr<GameplayPersistenceAdapter>>::Failure(valid.ErrorValue());
        if (!retainedSource || !retainedLease)
            return Result<std::shared_ptr<GameplayPersistenceAdapter>>::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
        try {
            return Result<std::shared_ptr<GameplayPersistenceAdapter>>::Success(
                std::make_shared<GameplayPersistenceAdapter>(ValidatedConstruction{}, std::move(descriptor), std::move(retainedSource),
                                                             std::move(retainedLease)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayPersistenceAdapter>>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }

    /** @copydoc GameplayPersistenceAdapter::Capture */
    Result<CanonicalCaptureDisposition> GameplayPersistenceAdapter::Capture(const CanonicalCaptureContext &context,
                                                                            ICanonicalCaptureSink &sink) const {
        const auto budget = CapturePayloadBudget(descriptor_, context);
        if (budget.HasError())
            return Result<CanonicalCaptureDisposition>::Failure(budget.ErrorValue());
        using Return = Result<CanonicalCaptureDisposition>;
        static_assert(std::is_nothrow_move_constructible_v<Return>);
        // Reserve both typed failures before invoking project code, including the allocation-failure result.
        return [this, &sink, &budget, allocationFailure = Return::Failure(MakeError(SaveErrors::CaptureAllocationFailed)),
                callbackFailure = Return::Failure(MakeError(SaveErrors::LifecycleCallbackFailed))]() mutable noexcept {
            try {
                auto captured = source_->CaptureRuntimeState(budget.Value());
                if (captured.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(captured.ErrorValue());
                auto payload = std::move(captured).Value();
                if (payload.size() > budget.Value())
                    return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                const auto encoded = EncodeEnvelope(descriptor_, payload);
                if (auto written = sink.WriteCopied(descriptor_.record, encoded); written.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            } catch (const std::bad_alloc &) {
                return std::move(allocationFailure);
            } catch (...) {
                // Unknown project exceptions must become typed failures, not unwind through archive work.
                return std::move(callbackFailure);
            }
        }();
    }

    /** @copydoc GameplayPersistenceAdapter::StageRestore */
    Result<std::unique_ptr<IStagedRestoreParticipant>> GameplayPersistenceAdapter::StageRestore(
        const ParticipantSchemaVersion savedSchema, const SaveRecordId savedRecord, const std::span<const std::byte> bytes) const {
        if (savedSchema != descriptor_.participant.schemaVersion || savedRecord != descriptor_.record ||
            bytes.size() > descriptor_.participant.limits.maximumPayloadBytes)
            return Result<std::unique_ptr<IStagedRestoreParticipant>>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
        try {
            std::vector<std::byte> owned{bytes.begin(), bytes.end()};
            return Result<std::unique_ptr<IStagedRestoreParticipant>>::Success(
                std::make_unique<GameplayStagedRestore>(descriptor_, source_, moduleLease_, std::move(owned)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<IStagedRestoreParticipant>>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    /** @copydoc GameplayPersistenceAdapter::Descriptor */
    const GameplayPersistenceDescriptor &GameplayPersistenceAdapter::Descriptor() const noexcept {
        return descriptor_;
    }
}  // namespace Horo::Runtime
