#pragma once

/**
 * @file PhysicsReferenceSceneCorpus.h
 * @brief Versioned, backend-neutral expectations for the Physics reference scenes.
 */

#include "Horo/Physics/PhysicsCapabilities.h"
#include "Horo/Physics/PhysicsEvents.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace Horo::Physics::Test {
    inline constexpr std::uint32_t PhysicsReferenceSceneCorpusVersion = 1;
    inline constexpr std::size_t MaximumPhysicsReferenceEvents = 3;
    inline constexpr std::size_t MaximumPhysicsReferenceQueryHits = 2;

    /** @brief Stable identity for one canonical Physics reference scene. */
    enum class PhysicsReferenceSceneId : std::uint8_t {
        ContactLifecycle,
        TriggerLifecycle,
        CcdTunnelling,
        StackingAndSleep,
        FixedJoint,
        QueryOrdering,
        Count,
    };

    inline constexpr std::size_t PhysicsReferenceSceneCount = static_cast<std::size_t>(PhysicsReferenceSceneId::Count);

    /** @brief Whether a reference scene is executable by the current Physics contract. */
    enum class PhysicsReferenceSceneStatus : std::uint8_t {
        Supported,
        Unsupported,
    };

    /** @brief Why an intentionally unsupported reference scene cannot run. */
    enum class PhysicsReferenceUnsupportedReason : std::uint8_t {
        None,
        RequiredCapabilityUnsupported,
    };

    /** @brief Stable, backend-neutral observations captured from one reference-scene run. */
    struct PhysicsReferenceObservation final {
        std::uint32_t corpusVersion{};
        PhysicsReferenceSceneId scene{};
        PhysicsReferenceSceneStatus status{};
        PhysicsReferenceUnsupportedReason unsupportedReason{};
        PhysicsCapability requiredCapability{PhysicsCapability::WorldCreation};
        std::uint32_t eventCount{};
        std::array<PhysicsEventKind, MaximumPhysicsReferenceEvents> eventKinds{};
        std::array<std::uint64_t, MaximumPhysicsReferenceEvents> eventTicks{};
        std::uint32_t queryHitCount{};
        std::array<std::uint32_t, MaximumPhysicsReferenceQueryHits> queryBodySlots{};
        std::array<std::uint32_t, MaximumPhysicsReferenceQueryHits> queryBodyGenerations{};
        std::array<std::uint32_t, MaximumPhysicsReferenceQueryHits> queryDistanceBits{};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsReferenceObservation &) const noexcept = default;
    };

    namespace Detail {
        /** @brief Validates the bounded lifecycle fields and their canonical padding. */
        [[nodiscard]] constexpr bool IsValidPhysicsReferenceEventFields(const PhysicsReferenceObservation &observation) noexcept {
            for (std::size_t index = 0; index < observation.eventKinds.size(); ++index) {
                if (index >= observation.eventCount) {
                    if (observation.eventKinds[index] != PhysicsEventKind{} || observation.eventTicks[index] != 0)
                        return false;
                    continue;
                }
                if (static_cast<std::uint8_t>(observation.eventKinds[index]) > static_cast<std::uint8_t>(PhysicsEventKind::TriggerExit) ||
                    observation.eventTicks[index] == 0)
                    return false;
                if (index > 0 && observation.eventTicks[index - 1] > observation.eventTicks[index])
                    return false;
            }
            return true;
        }

        /** @brief Validates query identities and zeroed entries outside the active hit count. */
        [[nodiscard]] constexpr bool IsValidPhysicsReferenceQueryFields(const PhysicsReferenceObservation &observation) noexcept {
            for (std::size_t index = 0; index < observation.queryBodyGenerations.size(); ++index) {
                if (index >= observation.queryHitCount) {
                    if (observation.queryBodySlots[index] != 0 || observation.queryBodyGenerations[index] != 0 ||
                        observation.queryDistanceBits[index] != 0)
                        return false;
                    continue;
                }
                if (observation.queryBodyGenerations[index] == 0)
                    return false;
            }
            return true;
        }

        /** @brief Validates support status, reason and empty fields for unsupported scenes. */
        [[nodiscard]] constexpr bool IsValidPhysicsReferenceStatus(const PhysicsReferenceObservation &observation) noexcept {
            switch (observation.status) {
                case PhysicsReferenceSceneStatus::Supported:
                    return observation.unsupportedReason == PhysicsReferenceUnsupportedReason::None;
                case PhysicsReferenceSceneStatus::Unsupported:
                    return observation.unsupportedReason == PhysicsReferenceUnsupportedReason::RequiredCapabilityUnsupported &&
                           observation.eventCount == 0 && observation.queryHitCount == 0;
                default:
                    return false;
            }
        }
    }  // namespace Detail

    /**
     * @brief Checks the version-one shape and status invariants of one reference observation.
     * @param observation Captured or expected backend-neutral reference evidence.
     * @return True when the bounded fields and support status are coherent.
     */
    [[nodiscard]] constexpr bool IsValidPhysicsReferenceObservation(const PhysicsReferenceObservation &observation) noexcept {
        if (observation.corpusVersion != PhysicsReferenceSceneCorpusVersion || observation.scene >= PhysicsReferenceSceneId::Count ||
            observation.requiredCapability >= PhysicsCapability::Count || observation.eventCount > MaximumPhysicsReferenceEvents ||
            observation.queryHitCount > MaximumPhysicsReferenceQueryHits)
            return false;
        return Detail::IsValidPhysicsReferenceEventFields(observation) && Detail::IsValidPhysicsReferenceQueryFields(observation) &&
               Detail::IsValidPhysicsReferenceStatus(observation);
    }

    namespace Detail {
        /** @brief Small constexpr FNV-1a accumulator with explicit little-endian field encoding. */
        struct PhysicsReferenceHash final {
            std::uint64_t value{14695981039346656037ULL};

            constexpr void AddByte(const std::uint8_t byte) noexcept {
                value ^= byte;
                value *= 1099511628211ULL;
            }

            template <typename Integer> constexpr void AddInteger(const Integer input) noexcept {
                using Unsigned = std::make_unsigned_t<Integer>;
                const auto valueToEncode = static_cast<Unsigned>(input);
                for (std::size_t index = 0; index < sizeof(Integer); ++index)
                    AddByte(static_cast<std::uint8_t>(valueToEncode >> (index * 8U)));
            }
        };
    }  // namespace Detail

    /**
     * @brief Hashes only Horo-owned observation fields in their versioned canonical order.
     * @param observation Captured or expected reference-scene observation.
     * @return Stable 64-bit fingerprint; no object padding or native identity participates.
     */
    [[nodiscard]] constexpr std::uint64_t PhysicsReferenceObservationHash(const PhysicsReferenceObservation &observation) noexcept {
        Detail::PhysicsReferenceHash hash;
        hash.AddInteger(observation.corpusVersion);
        hash.AddInteger(static_cast<std::uint8_t>(observation.scene));
        hash.AddInteger(static_cast<std::uint8_t>(observation.status));
        hash.AddInteger(static_cast<std::uint8_t>(observation.unsupportedReason));
        hash.AddInteger(static_cast<std::uint8_t>(observation.requiredCapability));
        hash.AddInteger(observation.eventCount);
        for (const auto kind : observation.eventKinds)
            hash.AddInteger(static_cast<std::uint8_t>(kind));
        for (const auto tick : observation.eventTicks)
            hash.AddInteger(tick);
        hash.AddInteger(observation.queryHitCount);
        for (const auto slot : observation.queryBodySlots)
            hash.AddInteger(slot);
        for (const auto generation : observation.queryBodyGenerations)
            hash.AddInteger(generation);
        for (const auto distanceBits : observation.queryDistanceBits)
            hash.AddInteger(distanceBits);
        return hash.value;
    }

    /** @brief One named expected observation and its canonical fingerprint. */
    struct PhysicsReferenceSceneExpectation final {
        std::string_view name;
        PhysicsReferenceObservation observation;
        std::uint64_t expectedHash{};
    };

    [[nodiscard]] constexpr PhysicsReferenceSceneExpectation MakePhysicsReferenceSceneExpectation(
        const std::string_view name, const PhysicsReferenceObservation observation, const std::uint64_t expectedHash) noexcept {
        return {.name = name, .observation = observation, .expectedHash = expectedHash};
    }

    inline constexpr auto PhysicsReferenceSceneCorpus = std::array{
        MakePhysicsReferenceSceneExpectation("contact-lifecycle",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::ContactLifecycle,
                                                                         .status = PhysicsReferenceSceneStatus::Supported,
                                                                         .requiredCapability = PhysicsCapability::WorldCreation,
                                                                         .eventCount = 3,
                                                                         .eventKinds = {PhysicsEventKind::ContactBegin,
                                                                                        PhysicsEventKind::ContactPersist,
                                                                                        PhysicsEventKind::ContactEnd},
                                                                         .eventTicks = {1, 2, 3}},
                                             0x13DCA78746A2E83AULL),
        MakePhysicsReferenceSceneExpectation("trigger-lifecycle",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::TriggerLifecycle,
                                                                         .status = PhysicsReferenceSceneStatus::Supported,
                                                                         .requiredCapability = PhysicsCapability::WorldCreation,
                                                                         .eventCount = 2,
                                                                         .eventKinds = {PhysicsEventKind::TriggerEnter,
                                                                                        PhysicsEventKind::TriggerExit},
                                                                         .eventTicks = {1, 3}},
                                             0x368EDA1C9CF2E36AULL),
        MakePhysicsReferenceSceneExpectation("ccd-tunnelling",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::CcdTunnelling,
                                                                         .status = PhysicsReferenceSceneStatus::Unsupported,
                                                                         .unsupportedReason = PhysicsReferenceUnsupportedReason::
                                                                             RequiredCapabilityUnsupported,
                                                                         .requiredCapability = PhysicsCapability::RigidBodies},
                                             0xEAA255EB47668689ULL),
        MakePhysicsReferenceSceneExpectation("stacking-and-sleep",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::StackingAndSleep,
                                                                         .status = PhysicsReferenceSceneStatus::Unsupported,
                                                                         .unsupportedReason = PhysicsReferenceUnsupportedReason::
                                                                             RequiredCapabilityUnsupported,
                                                                         .requiredCapability = PhysicsCapability::RigidBodies},
                                             0x5469C06595F985F6ULL),
        MakePhysicsReferenceSceneExpectation("fixed-joint",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::FixedJoint,
                                                                         .status = PhysicsReferenceSceneStatus::Unsupported,
                                                                         .unsupportedReason = PhysicsReferenceUnsupportedReason::
                                                                             RequiredCapabilityUnsupported,
                                                                         .requiredCapability = PhysicsCapability::Constraints},
                                             0xA1A94C52D29738C5ULL),
        MakePhysicsReferenceSceneExpectation("query-ordering",
                                             PhysicsReferenceObservation{.corpusVersion = PhysicsReferenceSceneCorpusVersion,
                                                                         .scene = PhysicsReferenceSceneId::QueryOrdering,
                                                                         .status = PhysicsReferenceSceneStatus::Supported,
                                                                         .requiredCapability = PhysicsCapability::ImmediateQueries,
                                                                         .queryHitCount = 2,
                                                                         .queryBodySlots = {0, 1},
                                                                         .queryBodyGenerations = {1, 2},
                                                                         .queryDistanceBits = {0x40900000U, 0x41180000U}},
                                             0xD4A2E843C2919D84ULL),
    };

    static_assert(PhysicsReferenceSceneCorpus.size() == PhysicsReferenceSceneCount);

    /** @brief Returns the complete immutable corpus manifest. */
    [[nodiscard]] constexpr std::span<const PhysicsReferenceSceneExpectation> PhysicsReferenceSceneCorpusView() noexcept {
        return PhysicsReferenceSceneCorpus;
    }
}  // namespace Horo::Physics::Test
