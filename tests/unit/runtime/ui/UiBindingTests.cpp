#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> T RequireValue(Result<T> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename Id> Id Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return RequireValue(Id::Create(bytes));
        }

        UiBindingProviderTypeId ProviderType(const std::string_view value = "game.hud.player") {
            return RequireValue(UiBindingProviderTypeId::Parse(value));
        }

        UiBindingPropertyId Property(const std::string_view value = "health") {
            return RequireValue(UiBindingPropertyId::Parse(value));
        }

        UiBindingConverterId Converter(const std::string_view value = "game.hud.scalar_to_text") {
            return RequireValue(UiBindingConverterId::Parse(value));
        }

        UiBindingPropertyDescriptor HealthProperty(const UiBindingValueType type = UiBindingValueType::FixedScalar,
                                                   const UiBindingUpdateKind update = UiBindingUpdateKind::OnChange) {
            return {.id = Property(),
                    .type = type,
                    .access = UiBindingAccess::Read,
                    .update = update,
                    .privacy = UiBindingPrivacyClass::Public,
                    .limits = {.maximumBytes = 32, .maximumElements = 1, .minimumScalar = 0.0, .maximumScalar = 1.0},
                    .flags = UiBindingPropertyFlags::AffectsPaint};
        }

        UiBindingProviderSchema MakeSchema(const UiBindingValueType type = UiBindingValueType::FixedScalar, const std::uint32_t minor = 0,
                                           const UiBindingUpdateKind update = UiBindingUpdateKind::OnChange) {
            const std::array properties{HealthProperty(type, update)};
            const UiBindingProviderTypeId typeId = ProviderType();
            UiBindingSchemaVersion version{1, minor, 0};
            version.fingerprint = ComputeUiBindingSchemaFingerprint(typeId, version, properties);
            const UiBindingProviderDescriptor descriptor{typeId,
                                                         ModuleId{"game.hud"},
                                                         version,
                                                         properties,
                                                         UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance),
                                                         UiBindingProviderFlags::None};
            return RequireValue(UiBindingProviderSchema::Create(descriptor));
        }

        UiBindingDescriptor Binding(const UiBindingProviderSchema &schema) {
            return {.id = Stable<UiBindingId>(9),
                    .source = {.providerType = schema.Type(),
                               .property = Property(),
                               .schema = {schema.Version().major, 0},
                               .propertySignatureFingerprint = ComputeUiBindingPropertyFingerprint(schema.Properties().front())},
                    .target = {.element = Stable<UiElementId>(3), .property = UiBindingTargetProperty::Progress},
                    .direction = UiBindingDirection::SourceToTarget,
                    .converter = std::nullopt,
                    .fallback = std::nullopt,
                    .updatePolicy = UiBindingUpdatePolicy::OnChange,
                    .requirement = UiBindingRequirement::Required};
        }

        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Runtime UI binding identities and semantic fingerprints are typed and inert", "[runtime_ui][binding][schema]") {
            REQUIRE(UiBindingProviderTypeId::Parse("Player.Health").HasError());
            REQUIRE(UiBindingProviderTypeId::Parse("game..player").HasError());
            REQUIRE(UiBindingPropertyId::Parse("health.value").HasError());
            REQUIRE(UiBindingConverterId::Parse("converter").HasError());
            REQUIRE(UiBindingSchemaVersion::Create(0, 0, 1).HasError());
            REQUIRE(UiBindingSchemaVersion::Create(1, 0, 0).HasError());

            static_assert(!std::is_same_v<UiBindingProviderTypeId, UiBindingPropertyId>);
            static_assert(!std::is_same_v<UiBindingPropertyId, UiBindingConverterId>);
            static_assert(!std::is_same_v<UiBindingId, UiElementId>);
            static_assert(!std::is_pointer_v<decltype(UiBindingDescriptor::source)>);

            const auto schema = MakeSchema();
            REQUIRE(schema.Type().Value() == "game.hud.player");
            REQUIRE(schema.Properties().size() == 1);
            REQUIRE(schema.Find(Property()) != nullptr);
            REQUIRE(schema.Version().fingerprint != 0);
        }

        TEST_CASE("Provider schema validation copies borrowed contributions before their lifetime ends",
                  "[runtime_ui][binding][ownership]") {
            const auto schema = MakeSchema();
            const UiBindingDescriptor descriptor = Binding(schema);
            REQUIRE(ValidateUiBindingDescriptor(descriptor, schema).HasValue());
            REQUIRE(schema.Find(Property())->signatureFingerprint != 0);

            const auto second = MakeSchema(UiBindingValueType::FixedScalar, 1);
            REQUIRE(second.Version().fingerprint != schema.Version().fingerprint);
            REQUIRE(ValidateUiBindingDescriptor(descriptor, schema).HasValue());
            REQUIRE(ValidateUiBindingDescriptor(descriptor, second).HasValue());
        }

        TEST_CASE("Bindings validate direction, access, type, converter, fallback, and update policy before activation",
                  "[runtime_ui][binding][validation]") {
            const auto scalarSchema = MakeSchema();
            const auto valid = Binding(scalarSchema);
            REQUIRE(ValidateUiBindingDescriptor(valid, scalarSchema).HasValue());

            auto write = valid;
            write.direction = UiBindingDirection::TargetToSource;
            ExpectError(ValidateUiBindingDescriptor(write, scalarSchema), UiErrors::BindingAccessInvalid);

            auto mismatch = valid;
            mismatch.target.property = UiBindingTargetProperty::Text;
            ExpectError(ValidateUiBindingDescriptor(mismatch, scalarSchema), UiErrors::BindingTypeMismatch);

            mismatch.converter = UiBindingConverterDescriptor{Converter(), UiBindingValueType::FixedScalar, UiBindingValueType::BoundedText,
                                                              UiBindingConverterMode::Forward};
            REQUIRE(ValidateUiBindingDescriptor(mismatch, scalarSchema).HasValue());

            auto invalidConverter = mismatch;
            invalidConverter.converter->targetType = UiBindingValueType::Boolean;
            ExpectError(ValidateUiBindingDescriptor(invalidConverter, scalarSchema), UiErrors::BindingTypeMismatch);

            const auto textSchema = MakeSchema(UiBindingValueType::BoundedText);
            auto optional = Binding(textSchema);
            optional.target.property = UiBindingTargetProperty::Text;
            optional.requirement = UiBindingRequirement::Optional;
            ExpectError(ValidateUiBindingDescriptor(optional, textSchema), UiErrors::BindingFallbackInvalid);
            optional.fallback = UiBindingValue{std::string{"offline"}};
            REQUIRE(ValidateUiBindingDescriptor(optional, textSchema).HasValue());

            optional.fallback = std::uint64_t{4};
            ExpectError(ValidateUiBindingDescriptor(optional, textSchema), UiErrors::BindingFallbackInvalid);

            auto manual = valid;
            manual.updatePolicy = UiBindingUpdatePolicy::Manual;
            REQUIRE(ValidateUiBindingDescriptor(manual, scalarSchema).HasValue());
            const auto manualSchema = MakeSchema(UiBindingValueType::FixedScalar, 0, UiBindingUpdateKind::Manual);
            const auto manualBinding = Binding(manualSchema);
            ExpectError(ValidateUiBindingDescriptor(manualBinding, manualSchema), UiErrors::BindingUpdatePolicyInvalid);
            auto unavailableCadence = manualBinding;
            unavailableCadence.updatePolicy = UiBindingUpdatePolicy::Manual;
            REQUIRE(ValidateUiBindingDescriptor(unavailableCadence, manualSchema).HasValue());
        }

        TEST_CASE("Binding batches reject duplicate targets and preserve exact schema requirements on reload",
                  "[runtime_ui][binding][reload]") {
            const auto schema = MakeSchema();
            auto first = Binding(schema);
            auto second = Binding(schema);
            second.id = Stable<UiBindingId>(10);
            std::array bindings{first, second};
            ExpectError(ValidateUiBindingDescriptors(bindings, schema), UiErrors::BindingDescriptorConflict);

            second.target.element = Stable<UiElementId>(4);
            bindings = {first, second};
            REQUIRE(ValidateUiBindingDescriptors(bindings, schema).HasValue());

            auto stale = first;
            stale.source.schema.minimumMinor = schema.Version().minor + 1;
            ExpectError(ValidateUiBindingDescriptor(stale, schema), UiErrors::BindingSchemaIncompatible);

            const auto replacement = MakeSchema(UiBindingValueType::BoundedText, 1);
            REQUIRE(ValidateUiBindingDescriptor(first, schema).HasValue());
            ExpectError(ValidateUiBindingDescriptor(first, replacement), UiErrors::BindingPropertySignatureMismatch);
        }

        TEST_CASE("Binding validation fails closed for malformed provider metadata and shutdown-independent copies",
                  "[runtime_ui][binding][shutdown]") {
            const auto valid = MakeSchema();
            auto descriptor = Binding(valid);
            descriptor.source.providerType = ProviderType("game.other.provider");
            ExpectError(ValidateUiBindingDescriptor(descriptor, valid), UiErrors::BindingProviderUnknown);

            std::array properties{HealthProperty(), HealthProperty()};
            const UiBindingProviderTypeId type = ProviderType();
            UiBindingSchemaVersion version{1, 0, 1};
            version.fingerprint = ComputeUiBindingSchemaFingerprint(type, version, properties);
            const UiBindingProviderDescriptor duplicate{type,
                                                        ModuleId{"game.hud"},
                                                        version,
                                                        properties,
                                                        UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance),
                                                        UiBindingProviderFlags::None};
            ExpectError(ValidateUiBindingProviderDescriptor(duplicate), UiErrors::BindingDescriptorConflict);

            UiBindingProviderSchema retained = MakeSchema();
            REQUIRE(retained.Find(Property()) != nullptr);
            REQUIRE(ValidateUiBindingDescriptor(Binding(retained), retained).HasValue());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
