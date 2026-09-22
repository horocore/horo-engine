#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
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

        struct PropertyOptions final {
            UiBindingAccess access{UiBindingAccess::Read};
            UiBindingPrivacyClass privacy{UiBindingPrivacyClass::Public};
            UiBindingValueLimits limits{.maximumBytes = 32, .maximumElements = 1, .minimumScalar = 0.0, .maximumScalar = 1.0};
            UiBindingPropertyFlags flags{UiBindingPropertyFlags::AffectsPaint};
        };

        UiBindingPropertyDescriptor HealthProperty(const UiBindingValueType type = UiBindingValueType::FixedScalar,
                                                   const UiBindingUpdateKind update = UiBindingUpdateKind::OnChange,
                                                   const PropertyOptions &options = {}) {
            return {.id = Property(),
                    .type = type,
                    .access = options.access,
                    .update = update,
                    .privacy = options.privacy,
                    .limits = options.limits,
                    .flags = options.flags};
        }

        UiBindingProviderDescriptor Provider(
            const std::span<const UiBindingPropertyDescriptor> properties, const UiBindingProviderTypeId type = ProviderType(),
            const ModuleId owner = ModuleId{"game.hud"},
            const UiBindingProviderScopeMask scopes = UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance),
            const UiBindingProviderFlags flags = UiBindingProviderFlags::None, const std::uint32_t major = 1,
            const std::uint32_t minor = 0) {
            UiBindingSchemaVersion version{major, minor, 0};
            version.fingerprint = ComputeUiBindingSchemaFingerprint(type, version, properties);
            return {type, owner, version, properties, scopes, flags};
        }

        UiBindingProviderSchema MakeSchema(const UiBindingValueType type = UiBindingValueType::FixedScalar, const std::uint32_t minor = 0,
                                           const UiBindingUpdateKind update = UiBindingUpdateKind::OnChange) {
            const std::array properties{HealthProperty(type, update)};
            const UiBindingProviderTypeId typeId = ProviderType();
            return RequireValue(UiBindingProviderSchema::Create(
                Provider(properties, typeId, ModuleId{"game.hud"}, UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance),
                         UiBindingProviderFlags::None, 1, minor)));
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

        TEST_CASE("Binding value and target types cover the closed semantic sets", "[runtime_ui][binding][types]") {
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingNullValue{}}) == UiBindingValueType::Optional);
            CHECK(UiBindingValueTypeOf(UiBindingValue{true}) == UiBindingValueType::Boolean);
            CHECK(UiBindingValueTypeOf(UiBindingValue{std::int64_t{-1}}) == UiBindingValueType::SignedInteger);
            CHECK(UiBindingValueTypeOf(UiBindingValue{std::uint64_t{1}}) == UiBindingValueType::UnsignedInteger);
            CHECK(UiBindingValueTypeOf(UiBindingValue{1.0}) == UiBindingValueType::FixedScalar);
            CHECK(UiBindingValueTypeOf(UiBindingValue{std::string{"text"}}) == UiBindingValueType::BoundedText);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingLocalizedMessage{"ui.text"}}) == UiBindingValueType::LocalizedMessage);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingEnumValue{2}}) == UiBindingValueType::Enum);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingFlagsValue{2}}) == UiBindingValueType::Flags);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingVector2{1.0F, 2.0F}}) == UiBindingValueType::Vector2);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingVector3{1.0F, 2.0F, 3.0F}}) == UiBindingValueType::Vector3);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingColor{1.0F, 0.5F, 0.25F, 1.0F}}) == UiBindingValueType::Color);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingReference{UiBindingReferenceKind::Asset, "asset"}}) ==
                  UiBindingValueType::AssetId);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingReference{UiBindingReferenceKind::Entity, "entity"}}) ==
                  UiBindingValueType::EntityId);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingReference{UiBindingReferenceKind::Domain, "domain"}}) ==
                  UiBindingValueType::DomainId);
            CHECK(UiBindingValueTypeOf(UiBindingValue{UiBindingReference{static_cast<UiBindingReferenceKind>(UiBindingReferenceKind::Count),
                                                                         "invalid"}}) == UiBindingValueType::Count);

            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::Text).value() == UiBindingValueType::BoundedText);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::LocalizedText).value() == UiBindingValueType::LocalizedMessage);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::Visible).value() == UiBindingValueType::Boolean);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::Enabled).value() == UiBindingValueType::Boolean);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::BooleanValue).value() == UiBindingValueType::Boolean);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::Selected).value() == UiBindingValueType::Boolean);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::ScalarValue).value() == UiBindingValueType::FixedScalar);
            CHECK(UiBindingTargetValueType(UiBindingTargetProperty::Progress).value() == UiBindingValueType::FixedScalar);
            CHECK_FALSE(UiBindingTargetValueType(UiBindingTargetProperty::Count).has_value());
            CHECK_FALSE(UiBindingTargetValueType(static_cast<UiBindingTargetProperty>(255)).has_value());
        }

        TEST_CASE("Binding identities reject malformed separators and preserve canonical punctuation", "[runtime_ui][binding][ids]") {
            REQUIRE(UiBindingProviderTypeId::Parse("game-hud.player_1").HasValue());
            REQUIRE(UiBindingConverterId::Parse("game.hud-converter").HasValue());
            REQUIRE(UiBindingProviderTypeId::Parse("game").HasError());
            REQUIRE(UiBindingProviderTypeId::Parse("game.-player").HasError());
            REQUIRE(UiBindingProviderTypeId::Parse("game.player-").HasError());
            REQUIRE(UiBindingProviderTypeId::Parse("game.player!").HasError());
            REQUIRE(UiBindingPropertyId::Parse("health_value").HasValue());
            REQUIRE(UiBindingPropertyId::Parse("health-value").HasError());
            REQUIRE(UiBindingPropertyId::Parse("Health").HasError());
            REQUIRE_FALSE(UiBindingProviderTypeId{}.IsValid());
            REQUIRE_FALSE(UiBindingPropertyId{}.IsValid());
            REQUIRE_FALSE(UiBindingConverterId{}.IsValid());
            REQUIRE(UiBindingSchemaVersion::Create(2, 3, 4).HasValue());
        }

        TEST_CASE("Binding fingerprints encode optional limits and property order", "[runtime_ui][binding][fingerprint]") {
            const UiBindingValueLimits limits{.maximumBytes = 64,
                                              .maximumElements = 2,
                                              .minimumSigned = -4,
                                              .maximumSigned = 4,
                                              .minimumUnsigned = 2,
                                              .maximumUnsigned = 8,
                                              .minimumScalar = -1.0,
                                              .maximumScalar = 1.0};
            const auto property =
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = limits});
            const auto changed =
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::Manual, PropertyOptions{.limits = limits});
            CHECK(ComputeUiBindingPropertyFingerprint(property) != ComputeUiBindingPropertyFingerprint(changed));
            const std::array properties{property, changed};
            const auto type = ProviderType();
            const auto first = ComputeUiBindingSchemaFingerprint(type, UiBindingSchemaVersion{1, 0, 0}, properties);
            const std::array reversed{changed, property};
            const auto second = ComputeUiBindingSchemaFingerprint(type, UiBindingSchemaVersion{1, 0, 0}, reversed);
            CHECK(first != second);
        }

        TEST_CASE("Provider validation rejects malformed bounds and identities", "[runtime_ui][binding][schema_validation]") {
            const std::array validProperties{HealthProperty()};
            const auto valid = Provider(validProperties);
            REQUIRE(ValidateUiBindingProviderDescriptor(valid).HasValue());

            auto limits = UiBindingDescriptorLimits{};
            limits.maximumPropertiesPerProvider = 0;
            ExpectError(ValidateUiBindingProviderDescriptor(valid, limits), UiErrors::BindingSchemaInvalid);
            limits = {};
            limits.maximumBindingDescriptors = MaximumUiBindingDescriptors + 1;
            ExpectError(ValidateUiBindingProviderDescriptor(valid, limits), UiErrors::BindingSchemaInvalid);
            limits = {};
            limits.maximumIdentifierBytes = 0;
            ExpectError(ValidateUiBindingProviderDescriptor(valid, limits), UiErrors::BindingSchemaInvalid);
            limits = {};
            limits.maximumValueBytes = MaximumUiBindingValueBytes + 1;
            ExpectError(ValidateUiBindingProviderDescriptor(valid, limits), UiErrors::BindingSchemaInvalid);

            auto invalid = valid;
            invalid.type = UiBindingProviderTypeId{};
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.ownerModule = ModuleId{"other.module"};
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.ownerModule = ModuleId{"Game"};
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.schema.fingerprint = 0;
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.properties = std::span<const UiBindingPropertyDescriptor>{};
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.allowedScopes = 0;
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.allowedScopes = 0x80;
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.flags = static_cast<UiBindingProviderFlags>(0x8000);
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
            invalid = valid;
            invalid.schema.fingerprint++;
            ExpectError(ValidateUiBindingProviderDescriptor(invalid), UiErrors::BindingSchemaInvalid);
        }

        TEST_CASE("Provider validation enforces property capacity and ordering", "[runtime_ui][binding][schema_validation]") {
            const std::array validProperties{HealthProperty()};
            const auto second = HealthProperty();
            auto distinct = second;
            distinct.id = Property("armor");
            const std::array tooMany{validProperties.front(), distinct};
            const auto tooManyDescriptor = Provider(tooMany);
            auto capacityLimits = UiBindingDescriptorLimits{};
            capacityLimits.maximumPropertiesPerProvider = 1;
            ExpectError(ValidateUiBindingProviderDescriptor(tooManyDescriptor, capacityLimits), UiErrors::BindingCapacityExceeded);

            const std::array unordered{validProperties.front(), distinct};
            ExpectError(ValidateUiBindingProviderDescriptor(Provider(unordered)), UiErrors::BindingDescriptorConflict);
        }

        TEST_CASE("Provider property validation rejects unknown identities, enums, and flags",
                  "[runtime_ui][binding][property_validation]") {
            const auto expectInvalid = [](UiBindingPropertyDescriptor property, const UiBindingDescriptorLimits limits = {}) {
                const std::array properties{property};
                ExpectError(ValidateUiBindingProviderDescriptor(Provider(properties), limits), UiErrors::BindingSchemaInvalid);
            };

            auto invalid = HealthProperty();
            invalid.id = UiBindingPropertyId{};
            expectInvalid(invalid);
            invalid = HealthProperty();
            auto shortIdentifierLimits = UiBindingDescriptorLimits{};
            shortIdentifierLimits.maximumIdentifierBytes = 1;
            expectInvalid(invalid, shortIdentifierLimits);
            invalid = HealthProperty(static_cast<UiBindingValueType>(UiBindingValueType::Count));
            expectInvalid(invalid);
            invalid = HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange,
                                     PropertyOptions{.access = static_cast<UiBindingAccess>(UiBindingAccess::Count)});
            expectInvalid(invalid);
            invalid = HealthProperty(UiBindingValueType::FixedScalar, static_cast<UiBindingUpdateKind>(UiBindingUpdateKind::Count));
            expectInvalid(invalid);
            invalid = HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange,
                                     PropertyOptions{.privacy = UiBindingPrivacyClass::Secret});
            expectInvalid(invalid);
            invalid = HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange,
                                     PropertyOptions{.flags = static_cast<UiBindingPropertyFlags>(0x8000)});
            expectInvalid(invalid);
        }

        TEST_CASE("Provider property validation rejects malformed limits and fingerprints", "[runtime_ui][binding][property_validation]") {
            const auto expectInvalid = [](UiBindingPropertyDescriptor property, const UiBindingDescriptorLimits limits = {}) {
                const std::array properties{property};
                ExpectError(ValidateUiBindingProviderDescriptor(Provider(properties), limits), UiErrors::BindingSchemaInvalid);
            };
            auto invalid = HealthProperty();
            auto invalidLimits = UiBindingValueLimits{};
            invalidLimits.maximumBytes = 0;
            expectInvalid(
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {};
            invalidLimits.maximumElements = 0;
            expectInvalid(
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {.maximumBytes = 32, .maximumElements = 1, .minimumSigned = 4, .maximumSigned = 3};
            expectInvalid(
                HealthProperty(UiBindingValueType::SignedInteger, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {.maximumBytes = 32, .maximumElements = 1, .minimumUnsigned = 4, .maximumUnsigned = 3};
            expectInvalid(HealthProperty(UiBindingValueType::UnsignedInteger, UiBindingUpdateKind::OnChange,
                                         PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {.maximumBytes = 32, .maximumElements = 1, .minimumScalar = std::numeric_limits<double>::quiet_NaN()};
            expectInvalid(
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {.maximumBytes = 32, .maximumElements = 1, .minimumScalar = 2.0, .maximumScalar = 1.0};
            expectInvalid(
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));
            invalidLimits = {.maximumBytes = 32, .maximumElements = 1, .maximumScalar = std::numeric_limits<double>::quiet_NaN()};
            expectInvalid(
                HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange, PropertyOptions{.limits = invalidLimits}));

            invalid = HealthProperty();
            invalid.signatureFingerprint = 1;
            expectInvalid(invalid);
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

        TEST_CASE("Bindings fail closed for malformed endpoint shape", "[runtime_ui][binding][validation_edges]") {
            const auto schema = MakeSchema();
            const auto valid = Binding(schema);

            auto invalid = valid;
            invalid.id = UiBindingId{};
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.source.property = UiBindingPropertyId{};
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.target.element = UiElementId{};
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.target.property = UiBindingTargetProperty::Count;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.direction = UiBindingDirection::Count;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.updatePolicy = UiBindingUpdatePolicy::Count;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.requirement = UiBindingRequirement::Count;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.target.limits.maximumBytes = 0;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingDescriptorInvalid);
            invalid = valid;
            invalid.source.schema.major++;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingSchemaIncompatible);
            invalid = valid;
            invalid.source.property = Property("missing");
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingPropertyUnknown);
            invalid = valid;
            invalid.source.propertySignatureFingerprint++;
            ExpectError(ValidateUiBindingDescriptor(invalid, schema), UiErrors::BindingPropertySignatureMismatch);
        }

        TEST_CASE("Bindings enforce access and converter metadata", "[runtime_ui][binding][validation_edges]") {
            const std::array writeProperties{HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange,
                                                            PropertyOptions{.access = UiBindingAccess::WriteCommand})};
            const auto writeSchema = RequireValue(UiBindingProviderSchema::Create(Provider(writeProperties)));
            ExpectError(ValidateUiBindingDescriptor(Binding(writeSchema), writeSchema), UiErrors::BindingAccessInvalid);

            const std::array bidirectionalProperties{HealthProperty(UiBindingValueType::FixedScalar, UiBindingUpdateKind::OnChange,
                                                                    PropertyOptions{.access = UiBindingAccess::ReadWriteCommand})};
            const auto bidirectionalSchema = RequireValue(UiBindingProviderSchema::Create(Provider(bidirectionalProperties)));
            auto twoWay = Binding(bidirectionalSchema);
            twoWay.direction = UiBindingDirection::TwoWay;
            twoWay.converter = UiBindingConverterDescriptor{Converter(), UiBindingValueType::FixedScalar, UiBindingValueType::FixedScalar,
                                                            UiBindingConverterMode::Forward};
            ExpectError(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema), UiErrors::BindingConverterInvalid);
            twoWay.converter->mode = UiBindingConverterMode::Bidirectional;
            REQUIRE(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema).HasValue());
            twoWay.converter->id = UiBindingConverterId{};
            ExpectError(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema), UiErrors::BindingConverterInvalid);
            twoWay.converter = UiBindingConverterDescriptor{Converter(), static_cast<UiBindingValueType>(UiBindingValueType::Count),
                                                            UiBindingValueType::FixedScalar, UiBindingConverterMode::Bidirectional};
            ExpectError(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema), UiErrors::BindingConverterInvalid);
            twoWay.converter = UiBindingConverterDescriptor{Converter(), UiBindingValueType::FixedScalar,
                                                            static_cast<UiBindingValueType>(UiBindingValueType::Count),
                                                            UiBindingConverterMode::Bidirectional};
            ExpectError(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema), UiErrors::BindingConverterInvalid);
            twoWay.converter = UiBindingConverterDescriptor{Converter(), UiBindingValueType::FixedScalar, UiBindingValueType::FixedScalar,
                                                            static_cast<UiBindingConverterMode>(UiBindingConverterMode::Count)};
            ExpectError(ValidateUiBindingDescriptor(twoWay, bidirectionalSchema), UiErrors::BindingConverterInvalid);

            auto targetToSource = Binding(writeSchema);
            targetToSource.direction = UiBindingDirection::TargetToSource;
            targetToSource.converter = UiBindingConverterDescriptor{Converter(), UiBindingValueType::FixedScalar,
                                                                    UiBindingValueType::FixedScalar, UiBindingConverterMode::Forward};
            REQUIRE(ValidateUiBindingDescriptor(targetToSource, writeSchema).HasValue());
        }

        TEST_CASE("Binding fallbacks validate scalar, text, localized, and boolean payloads", "[runtime_ui][binding][fallback]") {
            const auto scalarSchema = MakeSchema();
            auto scalar = Binding(scalarSchema);
            scalar.requirement = UiBindingRequirement::Optional;
            scalar.fallback = UiBindingValue{0.5};
            REQUIRE(ValidateUiBindingDescriptor(scalar, scalarSchema).HasValue());
            scalar.fallback = UiBindingValue{std::numeric_limits<double>::quiet_NaN()};
            ExpectError(ValidateUiBindingDescriptor(scalar, scalarSchema), UiErrors::BindingFallbackInvalid);
            scalar.target.limits.minimumScalar = 0.0;
            scalar.target.limits.maximumScalar = 1.0;
            scalar.fallback = UiBindingValue{2.0};
            ExpectError(ValidateUiBindingDescriptor(scalar, scalarSchema), UiErrors::BindingFallbackInvalid);
            scalar.fallback = UiBindingValue{std::string{"wrong"}};
            ExpectError(ValidateUiBindingDescriptor(scalar, scalarSchema), UiErrors::BindingFallbackInvalid);

            const auto textSchema = MakeSchema(UiBindingValueType::BoundedText);
            auto text = Binding(textSchema);
            text.target.property = UiBindingTargetProperty::Text;
            text.requirement = UiBindingRequirement::Optional;
            text.fallback = UiBindingValue{std::string{"ok"}};
            REQUIRE(ValidateUiBindingDescriptor(text, textSchema).HasValue());
            text.target.limits.maximumBytes = 1;
            ExpectError(ValidateUiBindingDescriptor(text, textSchema), UiErrors::BindingFallbackInvalid);

            const auto localizedSchema = MakeSchema(UiBindingValueType::LocalizedMessage);
            auto localized = Binding(localizedSchema);
            localized.target.property = UiBindingTargetProperty::LocalizedText;
            localized.requirement = UiBindingRequirement::Optional;
            localized.fallback = UiBindingValue{UiBindingLocalizedMessage{"ui.health"}};
            REQUIRE(ValidateUiBindingDescriptor(localized, localizedSchema).HasValue());
            localized.fallback = UiBindingValue{UiBindingLocalizedMessage{""}};
            ExpectError(ValidateUiBindingDescriptor(localized, localizedSchema), UiErrors::BindingFallbackInvalid);

            const auto booleanSchema = MakeSchema(UiBindingValueType::Boolean);
            auto boolean = Binding(booleanSchema);
            boolean.target.property = UiBindingTargetProperty::BooleanValue;
            boolean.requirement = UiBindingRequirement::Optional;
            boolean.fallback = UiBindingValue{true};
            REQUIRE(ValidateUiBindingDescriptor(boolean, booleanSchema).HasValue());
            boolean.fallback = UiBindingValue{std::uint64_t{1}};
            ExpectError(ValidateUiBindingDescriptor(boolean, booleanSchema), UiErrors::BindingFallbackInvalid);
        }

        TEST_CASE("Binding batches reject duplicate targets and preserve exact schema requirements on reload",
                  "[runtime_ui][binding][reload]") {
            const auto schema = MakeSchema();
            auto first = Binding(schema);
            auto second = Binding(schema);
            const std::array properties{HealthProperty()};
            const auto provider = Provider(properties);
            REQUIRE(ValidateUiBindingDescriptor(first, provider).HasValue());
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

            auto invalidLimits = UiBindingDescriptorLimits{};
            invalidLimits.maximumBindingDescriptors = 0;
            ExpectError(ValidateUiBindingDescriptors(bindings, schema, invalidLimits), UiErrors::BindingSchemaInvalid);
            invalidLimits = {};
            invalidLimits.maximumBindingDescriptors = MaximumUiBindingDescriptors + 1;
            ExpectError(ValidateUiBindingDescriptors(bindings, schema, invalidLimits), UiErrors::BindingSchemaInvalid);
            invalidLimits = {};
            invalidLimits.maximumBindingDescriptors = 1;
            ExpectError(ValidateUiBindingDescriptors(bindings, schema, invalidLimits), UiErrors::BindingCapacityExceeded);

            REQUIRE(UiBindingProviderSchema::Create(provider).HasValue());
            auto malformedProvider = provider;
            malformedProvider.schema.fingerprint++;
            REQUIRE(UiBindingProviderSchema::Create(malformedProvider).HasError());
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
