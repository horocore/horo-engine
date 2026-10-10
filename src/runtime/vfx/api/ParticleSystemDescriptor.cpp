#include "Horo/Vfx/ParticleSystemDescriptor.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    namespace {
        using Json = nlohmann::json;
        using namespace std::string_view_literals;

        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &code, std::string message = {}) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] bool ExactObject(const Json &value, const std::initializer_list<std::string_view> names,
                                       const std::initializer_list<std::string_view> optional = {}) {
            if (!value.is_object())
                return false;
            const auto present = [&value](const std::string_view name) {
                return value.contains(std::string{name});
            };
            const auto optionalCount = static_cast<std::size_t>(std::ranges::count_if(optional, present));
            return value.size() == names.size() + optionalCount && std::ranges::all_of(names, present);
        }

        class StrictJsonObserver final {
        public:
            explicit StrictJsonObserver(const std::size_t maximumDepth) : maximumDepth_(maximumDepth) {}

            bool Observe(const int depth, const Json::parse_event_t event, const Json &parsed) {
                using enum Json::parse_event_t;
                if (depth < 0 || static_cast<std::size_t>(depth) >= maximumDepth_) {
                    depthExceeded_ = true;
                    return false;
                }
                if (event == object_start) {
                    objectKeys_.emplace_back();
                } else if (event == key) {
                    if (objectKeys_.empty() || !objectKeys_.back().insert(parsed.get<std::string>()).second)
                        duplicateField_ = true;
                } else if (event == object_end && !objectKeys_.empty()) {
                    objectKeys_.pop_back();
                }
                return !duplicateField_ && !depthExceeded_;
            }

            [[nodiscard]] bool DuplicateField() const noexcept {
                return duplicateField_;
            }

            [[nodiscard]] bool DepthExceeded() const noexcept {
                return depthExceeded_;
            }

        private:
            std::size_t maximumDepth_{};
            std::vector<std::set<std::string, std::less<>>> objectKeys_;
            bool duplicateField_{};
            bool depthExceeded_{};
        };

        [[nodiscard]] bool ValidLimits(const ParticleDescriptorLimits &limits) noexcept {
            return limits.maximumSourceBytes > 0 && limits.maximumSourceBytes <= ParticleDescriptorHardLimits::SourceBytes &&
                   limits.maximumJsonDepth > 0 && limits.maximumJsonDepth <= ParticleDescriptorHardLimits::JsonDepth &&
                   limits.maximumParticles > 0 && limits.maximumParticles <= ParticleDescriptorHardLimits::Particles &&
                   std::isfinite(limits.maximumSpawnRate) && limits.maximumSpawnRate > 0.0 &&
                   limits.maximumSpawnRate <= ParticleDescriptorHardLimits::SpawnRate;
        }

        template <typename Integer> [[nodiscard]] std::optional<Integer> Unsigned(const Json &value) {
            if (!value.is_number_unsigned())
                return std::nullopt;
            const std::uint64_t decoded = value.get<std::uint64_t>();
            if (decoded > std::numeric_limits<Integer>::max())
                return std::nullopt;
            return static_cast<Integer>(decoded);
        }

        [[nodiscard]] std::optional<double> Number(const Json &value) {
            if (!value.is_number())
                return std::nullopt;
            return value.get<double>();
        }

        template <typename Enum, std::size_t Extent>
        [[nodiscard]] Enum EnumValue(const Json &value, const std::array<std::pair<std::string_view, Enum>, Extent> &mapping,
                                     const Enum invalid) {
            if (!value.is_string())
                return invalid;
            const std::string &text = value.get_ref<const std::string &>();
            const auto match = std::ranges::find(mapping, std::string_view{text}, &std::pair<std::string_view, Enum>::first);
            return match == mapping.end() ? invalid : match->second;
        }

        constexpr std::array SimulationPreferences{std::pair{"automatic"sv, SimulationPreference::Automatic},
                                                   std::pair{"requireCpu"sv, SimulationPreference::RequireCPU},
                                                   std::pair{"preferCpu"sv, SimulationPreference::PreferCPU},
                                                   std::pair{"preferGpu"sv, SimulationPreference::PreferGPU},
                                                   std::pair{"requireGpu"sv, SimulationPreference::RequireGPU}};
        constexpr std::array EmitterShapes{std::pair{"point"sv, ParticleEmitterShape::Point},
                                           std::pair{"sphere"sv, ParticleEmitterShape::Sphere},
                                           std::pair{"box"sv, ParticleEmitterShape::Box}, std::pair{"cone"sv, ParticleEmitterShape::Cone}};
        constexpr std::array LifetimeKinds{std::pair{"finite"sv, ParticleLifetimeKind::Finite},
                                           std::pair{"infinite"sv, ParticleLifetimeKind::Infinite}};
        constexpr std::array KillConditions{std::pair{"none"sv, ParticleKillCondition::None},
                                            std::pair{"lifetime"sv, ParticleKillCondition::Lifetime},
                                            std::pair{"collision"sv, ParticleKillCondition::Collision},
                                            std::pair{"explicitSignal"sv, ParticleKillCondition::ExplicitSignal}};
        constexpr std::array RenderModes{std::pair{"billboard"sv, ParticleRenderMode::Billboard},
                                         std::pair{"mesh"sv, ParticleRenderMode::Mesh}, std::pair{"ribbon"sv, ParticleRenderMode::Ribbon}};
        constexpr std::array SortModes{std::pair{"none"sv, ParticleSortMode::None}, std::pair{"byDistance"sv, ParticleSortMode::ByDistance},
                                       std::pair{"oldestFirst"sv, ParticleSortMode::OldestFirst}};
        constexpr std::array CollisionModes{std::pair{"none"sv, ParticleCollisionMode::None},
                                            std::pair{"planes"sv, ParticleCollisionMode::Planes},
                                            std::pair{"sceneDepth"sv, ParticleCollisionMode::SceneDepth},
                                            std::pair{"physicsWorld"sv, ParticleCollisionMode::PhysicsWorld}};

        [[nodiscard]] Result<ParticleDescriptorSchemaVersion> DecodeVersion(const Json &value) {
            if (!ExactObject(value, {"major", "minor"}))
                return Reject<ParticleDescriptorSchemaVersion>(VfxErrors::ParticleDescriptorMalformed,
                                                               "schemaVersion must contain exactly major and minor.");
            const auto major = Unsigned<std::uint16_t>(value.at("major"));
            const auto minor = Unsigned<std::uint16_t>(value.at("minor"));
            if (!major.has_value() || !minor.has_value())
                return Reject<ParticleDescriptorSchemaVersion>(VfxErrors::ParticleDescriptorMalformed,
                                                               "schemaVersion values must be unsigned 16-bit integers.");
            const ParticleDescriptorSchemaVersion version{*major, *minor};
            if (ClassifyParticleDescriptorCompatibility(version) != ParticleDescriptorCompatibility::Exact)
                return Reject<
                    ParticleDescriptorSchemaVersion>(VfxErrors::ParticleDescriptorVersionUnsupported,
                                                     std::format("Particle descriptor schema {}.{} requires migration or a newer reader.",
                                                                 *major, *minor));
            return Result<ParticleDescriptorSchemaVersion>::Success(version);
        }

        constexpr std::array CollisionResponses{std::pair{"bounce"sv, ParticleCollisionResponse::Bounce},
                                                std::pair{"die"sv, ParticleCollisionResponse::Die}};

        [[nodiscard]] Result<EmitterId> DecodeEmitter(const Json &value) {
            if (!ExactObject(value, {"scope", "slot", "generation"}))
                return Reject<EmitterId>(VfxErrors::ParticleDescriptorMalformed, "emitterId has an invalid shape.");
            const auto scopeValue = Unsigned<std::uint64_t>(value.at("scope"));
            const auto slot = Unsigned<std::uint32_t>(value.at("slot"));
            const auto generation = Unsigned<std::uint32_t>(value.at("generation"));
            if (!scopeValue.has_value() || !slot.has_value() || !generation.has_value())
                return Reject<EmitterId>(VfxErrors::ParticleDescriptorMalformed, "emitterId values are outside their encoded ranges.");
            auto scope = VfxIdentityScope::Create(*scopeValue);
            if (scope.HasError())
                return Reject<EmitterId>(VfxErrors::ParticleDescriptorMalformed, "emitterId scope is reserved.");
            auto emitter = MakeVfxIdentity<EmitterIdentityTag>(scope.Value(), *slot, *generation);
            if (emitter.HasError())
                return Reject<EmitterId>(VfxErrors::ParticleDescriptorMalformed, "emitterId contains a reserved value.");
            return emitter;
        }

        [[nodiscard]] Result<ParticleScalarRange> DecodeRange(const Json &value, const std::string_view field) {
            if (!ExactObject(value, {"minimum", "maximum"}))
                return Reject<ParticleScalarRange>(VfxErrors::ParticleDescriptorMalformed,
                                                   std::format("{} must contain exactly minimum and maximum.", field));
            const auto minimum = Number(value.at("minimum"));
            const auto maximum = Number(value.at("maximum"));
            if (!minimum.has_value() || !maximum.has_value())
                return Reject<ParticleScalarRange>(VfxErrors::ParticleDescriptorMalformed,
                                                   std::format("{} bounds must be numbers.", field));
            return Result<ParticleScalarRange>::Success({*minimum, *maximum});
        }

        template <typename... Results> [[nodiscard]] std::optional<Error> FirstDecodeFailure(const Results &...results) {
            std::optional<Error> failure;
            const auto capture = [&failure](const auto &result) {
                if (!failure.has_value() && result.HasError())
                    failure = result.ErrorValue();
            };
            (capture(results), ...);
            return failure;
        }

        [[nodiscard]] bool ValidRoot(const Json &root) {
            return ExactObject(root,
                               {"schemaVersion", "emitterId", "simulationPreference", "maximumParticles", "shape", "spawnRate", "lifetime",
                                "initialSpeed", "initialSize", "initialOpacity", "materialId", "renderMode", "sortMode", "collisionMode"},
                               {"collisionResponse"}) &&
                   ExactObject(root.at("lifetime"), {"kind", "seconds", "killCondition"});
        }

        [[nodiscard]] Result<ParticleSystemDescriptorData> DecodeRoot(const Json &root) {
            if (!ValidRoot(root))
                return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorMalformed,
                                                            "Particle-system root or lifetime object has an invalid shape.");

            auto version = DecodeVersion(root.at("schemaVersion"));
            auto emitter = DecodeEmitter(root.at("emitterId"));
            auto spawnRate = DecodeRange(root.at("spawnRate"), "spawnRate");
            auto lifetime = DecodeRange(root.at("lifetime").at("seconds"), "lifetime.seconds");
            auto speed = DecodeRange(root.at("initialSpeed"), "initialSpeed");
            auto size = DecodeRange(root.at("initialSize"), "initialSize");
            auto opacity = DecodeRange(root.at("initialOpacity"), "initialOpacity");
            const auto maximumParticles = Unsigned<std::uint32_t>(root.at("maximumParticles"));
            if (auto failure = FirstDecodeFailure(version, emitter, spawnRate, lifetime, speed, size, opacity); failure.has_value())
                return Result<ParticleSystemDescriptorData>::Failure(std::move(*failure));
            if (!maximumParticles.has_value() || !root.at("materialId").is_string())
                return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorMalformed,
                                                            "Particle-system source contains a structurally invalid field.");

            auto material = Assets::AssetId::Parse(root.at("materialId").get_ref<const std::string &>());
            if (material.HasError())
                return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorMalformed,
                                                            "materialId must be one canonical non-zero lowercase UUID.");

            return Result<ParticleSystemDescriptorData>::Success(
                {.version = version.Value(),
                 .emitter = emitter.Value(),
                 .simulationPreference = EnumValue(root.at("simulationPreference"), SimulationPreferences, SimulationPreference::Count),
                 .maximumParticles = *maximumParticles,
                 .shape = EnumValue(root.at("shape"), EmitterShapes, ParticleEmitterShape::Count),
                 .spawnRate = spawnRate.Value(),
                 .lifetimeKind = EnumValue(root.at("lifetime").at("kind"), LifetimeKinds, ParticleLifetimeKind::Count),
                 .lifetimeSeconds = lifetime.Value(),
                 .killCondition = EnumValue(root.at("lifetime").at("killCondition"), KillConditions, ParticleKillCondition::Count),
                 .initialSpeed = speed.Value(),
                 .initialSize = size.Value(),
                 .initialOpacity = opacity.Value(),
                 .material = std::move(material).Value(),
                 .renderMode = EnumValue(root.at("renderMode"), RenderModes, ParticleRenderMode::Count),
                 .sortMode = EnumValue(root.at("sortMode"), SortModes, ParticleSortMode::Count),
                 .collisionMode = EnumValue(root.at("collisionMode"), CollisionModes, ParticleCollisionMode::Count),
                 .collisionResponse = root.contains("collisionResponse")
                                          ? EnumValue(root.at("collisionResponse"), CollisionResponses, ParticleCollisionResponse::Count)
                                          : ParticleCollisionResponse::Bounce});
        }

        [[nodiscard]] bool FiniteOrdered(const ParticleScalarRange &range) noexcept {
            return std::isfinite(range.minimum) && std::isfinite(range.maximum) && range.minimum <= range.maximum;
        }

        [[nodiscard]] Result<void> AddFinding(ValidationResultBuilder &builder, const ErrorCodeDescriptor &code, std::string message,
                                              const std::string &sourceName) {
            return builder.Add(code, std::move(message), {.source = sourceName});
        }

        /** @brief Accumulates semantic findings while preserving the first operational registry failure. */
        class FindingCollector final {
        public:
            FindingCollector(ValidationResultBuilder &builder, const std::string &sourceName) noexcept
                : builder_(builder), sourceName_(sourceName) {}

            void AddIf(const bool condition, const ErrorCodeDescriptor &code, std::string message) {
                if (!condition || failure_.has_value())
                    return;
                auto added = AddFinding(builder_, code, std::move(message), sourceName_);
                if (added.HasError())
                    failure_ = added.ErrorValue();
            }

            [[nodiscard]] std::optional<Error> Finish() && noexcept {
                return std::move(failure_);
            }

        private:
            ValidationResultBuilder &builder_;
            const std::string &sourceName_;
            std::optional<Error> failure_;
        };

        [[nodiscard]] std::optional<Error> AddImportFindings(ValidationResultBuilder &builder, const ParticleSystemDescriptorData &data,
                                                             const ParticleDescriptorLimits &limits, const std::string &sourceName) {
            FindingCollector findings{builder, sourceName};
            findings.AddIf(ClassifyParticleDescriptorCompatibility(data.version) != ParticleDescriptorCompatibility::Exact,
                           VfxErrors::ParticleDescriptorVersionUnsupported, "schemaVersion is not directly compatible.");
            findings.AddIf(!data.emitter.IsValid(), VfxErrors::ParticleDescriptorMalformed, "emitterId contains a reserved value.");
            findings.AddIf(data.simulationPreference >= SimulationPreference::Count || data.shape >= ParticleEmitterShape::Count ||
                               data.lifetimeKind >= ParticleLifetimeKind::Count || data.killCondition >= ParticleKillCondition::Count ||
                               data.renderMode >= ParticleRenderMode::Count || data.sortMode >= ParticleSortMode::Count ||
                               data.collisionMode >= ParticleCollisionMode::Count ||
                               data.collisionResponse >= ParticleCollisionResponse::Count,
                           VfxErrors::ParticleDescriptorMalformed, "One or more closed policy values are unknown.");
            findings.AddIf(data.maximumParticles == 0 || data.maximumParticles > limits.maximumParticles,
                           VfxErrors::ParticleDescriptorLimitExceeded, "maximumParticles exceeds the active semantic limit.");
            findings.AddIf(!FiniteOrdered(data.spawnRate) || data.spawnRate.minimum < 0.0 ||
                               data.spawnRate.maximum > limits.maximumSpawnRate,
                           VfxErrors::ParticleRangeInvalid, "spawnRate must be finite, ordered, non-negative, and bounded.");
            findings.AddIf(!FiniteOrdered(data.initialSpeed), VfxErrors::ParticleRangeInvalid,
                           "initialSpeed must contain finite ordered values.");
            findings.AddIf(!FiniteOrdered(data.initialSize) || data.initialSize.minimum <= 0.0, VfxErrors::ParticleRangeInvalid,
                           "initialSize must contain finite ordered positive values.");
            findings.AddIf(!FiniteOrdered(data.initialOpacity) || data.initialOpacity.minimum < 0.0 || data.initialOpacity.maximum > 1.0,
                           VfxErrors::ParticleRangeInvalid, "initialOpacity must remain inside the inclusive range [0, 1].");
            if (data.lifetimeKind == ParticleLifetimeKind::Finite) {
                findings.AddIf(!FiniteOrdered(data.lifetimeSeconds) || data.lifetimeSeconds.minimum <= 0.0, VfxErrors::ParticleRangeInvalid,
                               "Finite lifetime seconds must contain finite ordered positive values.");
                findings.AddIf(data.killCondition != ParticleKillCondition::Lifetime, VfxErrors::ParticleModeIncompatible,
                               "Finite lifetime descriptors use the canonical lifetime kill condition.");
            } else if (data.lifetimeKind == ParticleLifetimeKind::Infinite) {
                findings.AddIf(data.lifetimeSeconds != ParticleScalarRange{}, VfxErrors::ParticleModeIncompatible,
                               "Infinite lifetime descriptors must encode seconds as the canonical zero range.");
                findings.AddIf(data.killCondition == ParticleKillCondition::None || data.killCondition == ParticleKillCondition::Lifetime,
                               VfxErrors::ParticleLifetimeUnbounded,
                               "Infinite particles require collision or explicit-signal termination.");
            }

            findings.AddIf(data.killCondition == ParticleKillCondition::Collision && data.collisionMode == ParticleCollisionMode::None,
                           VfxErrors::ParticleModeIncompatible, "Collision termination requires a non-None collision mode.");
            findings.AddIf(data.collisionMode == ParticleCollisionMode::PhysicsWorld &&
                               data.simulationPreference == SimulationPreference::RequireGPU,
                           VfxErrors::ParticleModeIncompatible, "PhysicsWorld collision is CPU-mandatory and conflicts with RequireGPU.");
            findings.AddIf(data.renderMode == ParticleRenderMode::Ribbon && data.sortMode == ParticleSortMode::OldestFirst,
                           VfxErrors::ParticleModeIncompatible, "Ribbon output cannot use per-particle OldestFirst sorting.");
            findings.AddIf(!data.material.IsValid(), VfxErrors::ParticleDescriptorMalformed, "materialId is a reserved zero identity.");
            return std::move(findings).Finish();
        }

        [[nodiscard]] std::optional<Error> AddCookFindings(ValidationResultBuilder &builder, const ParticleSystemDescriptorData &data,
                                                           const ParticleCookProfile &profile, const ParticleMaterialEvidence &material,
                                                           const std::string &sourceName) {
            FindingCollector findings{builder, sourceName};
            findings.AddIf(profile.tier >= ParticleCookTier::Count || profile.maximumParticles == 0 ||
                               !std::isfinite(profile.maximumSpawnRate) || profile.maximumSpawnRate <= 0.0,
                           VfxErrors::ParticleDescriptorMalformed, "Particle cook profile is malformed.");
            findings.AddIf(data.maximumParticles > profile.maximumParticles || data.spawnRate.maximum > profile.maximumSpawnRate,
                           VfxErrors::ParticleCookTierExceeded, "Particle count or spawn rate exceeds the selected cook tier.");
            findings.AddIf(material.material != data.material, VfxErrors::ParticleMaterialMissing,
                           "Material evidence does not describe the descriptor's exact stable identity.");

            const ErrorCodeDescriptor *materialFailure{};
            using enum ParticleMaterialAvailability;
            switch (material.availability) {
                case Available:
                    break;
                case Missing:
                    materialFailure = &VfxErrors::ParticleMaterialMissing;
                    break;
                case TypeMismatch:
                    materialFailure = &VfxErrors::ParticleMaterialTypeMismatch;
                    break;
                case Unloadable:
                    materialFailure = &VfxErrors::ParticleMaterialUnloadable;
                    break;
                case Count:
                    materialFailure = &VfxErrors::ParticleDescriptorMalformed;
                    break;
            }
            if (materialFailure != nullptr)
                findings.AddIf(true, *materialFailure, std::string{materialFailure->summary});
            return std::move(findings).Finish();
        }
    }  // namespace

    ParticleSystemDescriptor::ParticleSystemDescriptor(ParticleSystemDescriptorData data) noexcept : data_(std::move(data)) {}

    /** @copydoc ParticleSystemDescriptor::Data */
    const ParticleSystemDescriptorData &ParticleSystemDescriptor::Data() const noexcept {
        return data_;
    }

    /** @copydoc ParticleDescriptorValidation::Accepted */
    bool ParticleDescriptorValidation::Accepted() const noexcept {
        return descriptor.has_value() && !diagnostics.HasErrors();
    }

    /** @copydoc ParseParticleSystemDescriptor */
    Result<ParticleSystemDescriptorData> ParseParticleSystemDescriptor(const std::string_view source,
                                                                       const ParticleDescriptorLimits &limits) {
        if (!ValidLimits(limits) || source.size() > limits.maximumSourceBytes)
            return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorLimitExceeded,
                                                        "Particle-system parser limits or source byte count are invalid.");

        StrictJsonObserver observer{limits.maximumJsonDepth};
        Json root =
            Json::parse(source.begin(), source.end(), [&observer](const int depth, const Json::parse_event_t event, const Json &value) {
            return observer.Observe(depth, event, value);
        }, false, true);
        if (observer.DuplicateField())
            return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorDuplicate);
        if (observer.DepthExceeded())
            return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorLimitExceeded,
                                                        "Particle-system JSON nesting exceeds the parser limit.");
        if (root.is_discarded())
            return Reject<ParticleSystemDescriptorData>(VfxErrors::ParticleDescriptorMalformed);
        return DecodeRoot(root);
    }

    /** @copydoc ValidateParticleSystemDescriptor */
    Result<ParticleDescriptorValidation> ValidateParticleSystemDescriptor(ParticleSystemDescriptorData data, ErrorCodeRegistry registry,
                                                                          const std::string &sourceName,
                                                                          const ParticleDescriptorLimits &limits) {
        if (!ValidLimits(limits))
            return Reject<ParticleDescriptorValidation>(VfxErrors::ParticleDescriptorLimitExceeded,
                                                        "Active particle descriptor limits are invalid.");
        auto builder = ValidationResultBuilder::Create(std::move(registry));
        if (builder.HasError())
            return Result<ParticleDescriptorValidation>::Failure(builder.ErrorValue());
        auto validationBuilder = std::move(builder).Value();
        if (auto failure = AddImportFindings(validationBuilder, data, limits, sourceName); failure.has_value())
            return Result<ParticleDescriptorValidation>::Failure(std::move(*failure));
        auto completed = validationBuilder.Complete();
        if (completed.HasError())
            return Result<ParticleDescriptorValidation>::Failure(completed.ErrorValue());

        ValidationResult diagnostics = std::move(completed).Value();
        std::optional<ParticleSystemDescriptor> descriptor;
        if (!diagnostics.HasErrors())
            descriptor.emplace(ParticleSystemDescriptor{std::move(data)});
        return Result<ParticleDescriptorValidation>::Success({std::move(diagnostics), std::move(descriptor)});
    }

    /** @copydoc ParticleCookValidation::Accepted */
    bool ParticleCookValidation::Accepted() const noexcept {
        return plan.has_value() && !diagnostics.HasErrors();
    }

    /** @copydoc GetParticleCookProfile */
    Result<ParticleCookProfile> GetParticleCookProfile(const ParticleCookTier tier) {
        using enum ParticleCookTier;
        switch (tier) {
            case Compact:
                return Result<ParticleCookProfile>::Success({tier, 4'096, 10'000.0});
            case Standard:
                return Result<ParticleCookProfile>::Success({tier, 65'536, 100'000.0});
            case Large:
                return Result<ParticleCookProfile>::Success(
                    {tier, ParticleDescriptorHardLimits::Particles, ParticleDescriptorHardLimits::SpawnRate});
            case Count:
                return Reject<ParticleCookProfile>(VfxErrors::ParticleDescriptorMalformed, "Unknown particle cook tier.");
        }
        return Reject<ParticleCookProfile>(VfxErrors::ParticleDescriptorMalformed, "Unknown particle cook tier.");
    }

    /** @copydoc BuildParticleSystemCookPlan */
    Result<ParticleCookValidation> BuildParticleSystemCookPlan(const ParticleSystemDescriptor &descriptor,
                                                               const ParticleCookProfile &profile, const ParticleMaterialEvidence &material,
                                                               ErrorCodeRegistry registry, const std::string &sourceName) {
        auto builder = ValidationResultBuilder::Create(std::move(registry));
        if (builder.HasError())
            return Result<ParticleCookValidation>::Failure(builder.ErrorValue());
        auto validationBuilder = std::move(builder).Value();
        if (auto failure = AddCookFindings(validationBuilder, descriptor.Data(), profile, material, sourceName); failure.has_value())
            return Result<ParticleCookValidation>::Failure(std::move(*failure));
        auto completed = validationBuilder.Complete();
        if (completed.HasError())
            return Result<ParticleCookValidation>::Failure(completed.ErrorValue());

        ValidationResult diagnostics = std::move(completed).Value();
        std::optional<ParticleCookPlan> plan;
        if (!diagnostics.HasErrors()) {
            plan.emplace(ParticleCookPlan{.profile = profile,
                                          .emitter = descriptor.Data().emitter,
                                          .material = descriptor.Data().material,
                                          .maximumParticles = descriptor.Data().maximumParticles,
                                          .maximumSpawnRate = descriptor.Data().spawnRate.maximum});
        }
        return Result<ParticleCookValidation>::Success({std::move(diagnostics), std::move(plan)});
    }
}  // namespace Horo::Vfx
