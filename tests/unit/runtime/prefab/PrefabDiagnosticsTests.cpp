#include "Horo/Prefab/PrefabDiagnostics.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace {
        using Test::Asset;

        PrefabSourceRevision Revision(const std::uint8_t suffix = 1) {
            const auto version = Application::ParseHoroVersion("1.2.3");
            REQUIRE(version.HasValue());
            Sha256Digest digest{};
            digest.bytes.back() = suffix;
            return {.projectVersion = version.Value(), .contentDigest = digest};
        }

        PrefabDiagnosticContext Context() {
            const auto instance = PrefabInstanceId::Create(17);
            REQUIRE(instance.HasValue());
            const auto localMember = PrefabObjectAddress::Create({}, LocalObjectId{42});
            REQUIRE(localMember.HasValue());
            const auto componentType = Gameplay::ComponentTypeId::Parse("game.test.transform");
            REQUIRE(componentType.HasValue());
            const auto componentInstance = PrefabComponentInstanceId::Create(23);
            REQUIRE(componentInstance.HasValue());
            const auto property = PrefabPropertyId::Create(29);
            REQUIRE(property.HasValue());
            const auto propertyAddress =
                PrefabPropertyAddress::Create(localMember.Value(), componentType.Value(), componentInstance.Value(), property.Value());
            REQUIRE(propertyAddress.HasValue());
            return {
                .prefabAsset = Asset(1),
                .revision = Revision(),
                .registryRevision = Assets::AssetRegistryRevision{7},
                .instance = instance.Value(),
                .localMember = localMember.Value(),
                .property = propertyAddress.Value(),
                .operationId = OperationId{31},
                .source = DiagnosticSourceLocation{.absolutePath = "/project/assets/prefabs/player.prefab", .line = 12, .column = 4},
            };
        }

        Error ForeignError(const std::string_view domain, const std::string_view code,
                           const ErrorSeverity severity = ErrorSeverity::Error) {
            return Error{
                .code = ErrorCode{std::string{code}},
                .domain = ErrorDomainId{std::string{domain}},
                .severity = severity,
                .message = "foreign provider text must not define identity",
            };
        }

        void RequireFailure(const Result<PrefabDiagnosticRecord> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Prefab diagnostic categories and operations expose stable presentation identities", "[prefab][diagnostics]") {
            const std::array categories{
                std::pair{PrefabDiagnosticCategory::Source, std::string_view{"prefab.source"}},
                std::pair{PrefabDiagnosticCategory::Graph, std::string_view{"prefab.graph"}},
                std::pair{PrefabDiagnosticCategory::Expansion, std::string_view{"prefab.expansion"}},
                std::pair{PrefabDiagnosticCategory::Override, std::string_view{"prefab.override"}},
                std::pair{PrefabDiagnosticCategory::Cook, std::string_view{"prefab.cook"}},
                std::pair{PrefabDiagnosticCategory::Migration, std::string_view{"prefab.migration"}},
                std::pair{PrefabDiagnosticCategory::RuntimeSpawn, std::string_view{"prefab.runtime_spawn"}},
                std::pair{PrefabDiagnosticCategory::Lifecycle, std::string_view{"prefab.lifecycle"}},
            };
            for (const auto &[category, name] : categories)
                REQUIRE(PrefabDiagnosticCategoryName(category) == name);
            REQUIRE(PrefabDiagnosticCategoryName(static_cast<PrefabDiagnosticCategory>(255)).empty());

            const std::array operations{
                std::pair{PrefabDiagnosticOperation::Parse, std::string_view{"parse"}},
                std::pair{PrefabDiagnosticOperation::ValidateSource, std::string_view{"validate_source"}},
                std::pair{PrefabDiagnosticOperation::BuildGraph, std::string_view{"build_graph"}},
                std::pair{PrefabDiagnosticOperation::Expand, std::string_view{"expand"}},
                std::pair{PrefabDiagnosticOperation::ApplyOverride, std::string_view{"apply_override"}},
                std::pair{PrefabDiagnosticOperation::Cook, std::string_view{"cook"}},
                std::pair{PrefabDiagnosticOperation::Migrate, std::string_view{"migrate"}},
                std::pair{PrefabDiagnosticOperation::Load, std::string_view{"load"}},
                std::pair{PrefabDiagnosticOperation::Spawn, std::string_view{"spawn"}},
                std::pair{PrefabDiagnosticOperation::Commit, std::string_view{"commit"}},
                std::pair{PrefabDiagnosticOperation::Publish, std::string_view{"publish"}},
            };
            for (const auto &[operation, name] : operations)
                REQUIRE(PrefabDiagnosticOperationName(operation) == name);
            REQUIRE(PrefabDiagnosticOperationName(static_cast<PrefabDiagnosticOperation>(255)).empty());
        }

        TEST_CASE("Prefab diagnostic records own complete identity, operation, source navigation and dependency context",
                  "[prefab][diagnostics]") {
            const std::array dependencies{
                PrefabDiagnosticDependency{Asset(1), PrefabDependencyKind::NestedPrefab, Revision(1)},
                PrefabDiagnosticDependency{Asset(2), PrefabDependencyKind::VariantParent, Revision(2)},
                PrefabDiagnosticDependency{Asset(3), PrefabDependencyKind::Resource, std::nullopt},
            };
            const auto record =
                MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Expansion, PrefabDiagnosticOperation::Expand,
                                           MakeError(PrefabErrors::IdentityCollision, "identity collision context is presentation only"),
                                           Context(), dependencies);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().schemaVersion == 1);
            REQUIRE(record.Value().category == PrefabDiagnosticCategory::Expansion);
            REQUIRE(record.Value().operation == PrefabDiagnosticOperation::Expand);
            REQUIRE(record.Value().origin == PrefabDiagnosticOrigin::Prefab);
            REQUIRE(record.Value().prefabAsset == Asset(1));
            REQUIRE(record.Value().revision == Revision());
            REQUIRE(record.Value().registryRevision.value == 7);
            REQUIRE(record.Value().instance->Value() == 17);
            REQUIRE(record.Value().localMember->SourceObject().value == 42);
            REQUIRE(record.Value().property->Property().Value() == 29);
            REQUIRE(record.Value().operationId == OperationId{31});
            REQUIRE(record.Value().source->absolutePath == "/project/assets/prefabs/player.prefab");
            REQUIRE(record.Value().source->line == 12);
            REQUIRE(record.Value().DependencyChain().size() == dependencies.size());
            REQUIRE(record.Value().DependencyChain()[1].asset == Asset(2));
            REQUIRE(record.Value().DependencyChain()[1].kind == PrefabDependencyKind::VariantParent);
            REQUIRE(record.Value().DependencyChain()[2].revision == std::nullopt);
        }

        TEST_CASE("Prefab diagnostic translation preserves typed cross-domain causes without parsing messages", "[prefab][diagnostics]") {
            Error causes = ForeignError("horo.application.project", "project.migration.required");
            causes = WithCause(ForeignError("horo.gameplay", "gameplay.component_type_id_invalid"), std::move(causes));
            causes = WithCause(ForeignError("horo.scene", "scene.asset.registry_stale"), std::move(causes));
            causes = WithCause(ForeignError("horo.asset", "asset.cook.source_read_failed"), std::move(causes));
            const Error error = WithCause(MakeError(PrefabErrors::CookInputInvalid, "outer prefab context"), std::move(causes));

            const auto record =
                MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Migration, PrefabDiagnosticOperation::Migrate, error, Context());
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().domain.Value() == "horo.prefab");
            REQUIRE(record.Value().code.Value() == PrefabErrors::CookInputInvalid.code.Value());
            REQUIRE(record.Value().Causes().size() == 4);
            REQUIRE(record.Value().Causes()[0].origin == PrefabDiagnosticOrigin::Asset);
            REQUIRE(record.Value().Causes()[0].code.Value() == "asset.cook.source_read_failed");
            REQUIRE(record.Value().Causes()[1].origin == PrefabDiagnosticOrigin::Scene);
            REQUIRE(record.Value().Causes()[2].origin == PrefabDiagnosticOrigin::Gameplay);
            REQUIRE(record.Value().Causes()[3].origin == PrefabDiagnosticOrigin::Migration);
            REQUIRE(record.Value().Causes()[3].domain.Value() == "horo.application.project");
            REQUIRE(record.Value().message == "outer prefab context");
        }

        TEST_CASE("Prefab diagnostic records accept a foreign primary error and retain its exact identity", "[prefab][diagnostics]") {
            const auto source = ForeignError("horo.asset", "asset.cook.malformed_artifact", ErrorSeverity::Warning);
            const auto record =
                MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Cook, source, Context(), PrefabDiagnosticOperation::Cook);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().origin == PrefabDiagnosticOrigin::Asset);
            REQUIRE(record.Value().domain.Value() == "horo.asset");
            REQUIRE(record.Value().code.Value() == "asset.cook.malformed_artifact");
            REQUIRE(record.Value().severity == DiagnosticSeverity::Warning);
        }

        TEST_CASE("Prefab diagnostics enforce exact message, cause and dependency budgets", "[prefab][diagnostics]") {
            auto message = MakeError(PrefabErrors::DocumentInvalid);
            message.message.assign(MaximumPrefabDiagnosticMessageBytes, 'm');
            REQUIRE(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, message, Context()).HasValue());
            message.message.push_back('x');
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, message, Context()),
                           PrefabErrors::DiagnosticBudgetExceeded);

            Error exactCauses = ForeignError("horo.asset", "asset.cook.malformed_artifact");
            for (std::size_t index = 0; index < MaximumPrefabDiagnosticCauseDepth; ++index)
                exactCauses = WrapError(PrefabErrors::CookInputInvalid, std::move(exactCauses));
            const auto exact = MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Cook, exactCauses, Context());
            REQUIRE(exact.HasValue());
            REQUIRE(exact.Value().Causes().size() == MaximumPrefabDiagnosticCauseDepth);
            exactCauses = WrapError(PrefabErrors::CookInputInvalid, std::move(exactCauses));
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Cook, exactCauses, Context()),
                           PrefabErrors::DiagnosticBudgetExceeded);

            std::array<PrefabDiagnosticDependency, MaximumPrefabDiagnosticDependencyDepth> dependencies{};
            for (std::size_t index = 0; index < dependencies.size(); ++index)
                dependencies[index] = {Asset(static_cast<std::uint16_t>(index + 1)), PrefabDependencyKind::Resource, std::nullopt};
            REQUIRE(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Graph, MakeError(PrefabErrors::DependencyGraphInvalid), Context(),
                                               PrefabDiagnosticOperation::BuildGraph, dependencies)
                        .HasValue());
            dependencies[0].asset = Assets::AssetId{};
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Graph, MakeError(PrefabErrors::DependencyGraphInvalid),
                                                      Context(), PrefabDiagnosticOperation::BuildGraph, dependencies),
                           PrefabErrors::DiagnosticInvalid);
            dependencies[0].asset = Asset(1);
            std::vector<PrefabDiagnosticDependency> over(dependencies.begin(), dependencies.end());
            over.push_back({Asset(99), PrefabDependencyKind::Resource, std::nullopt});
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Graph, MakeError(PrefabErrors::DependencyGraphInvalid),
                                                      Context(), PrefabDiagnosticOperation::BuildGraph, over),
                           PrefabErrors::DiagnosticBudgetExceeded);
        }

        TEST_CASE("Prefab diagnostic construction rejects malformed identity, target and source context transactionally",
                  "[prefab][diagnostics]") {
            const auto error = MakeError(PrefabErrors::DocumentInvalid);
            auto invalidAsset = Context();
            invalidAsset.prefabAsset = {};
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, error, invalidAsset),
                           PrefabErrors::DiagnosticInvalid);

            auto invalidRevision = Context();
            invalidRevision.revision = {};
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, error, invalidRevision),
                           PrefabErrors::DiagnosticInvalid);

            auto invalidOperation = Context();
            invalidOperation.operationId = OperationId{0};
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, error, invalidOperation),
                           PrefabErrors::DiagnosticInvalid);

            auto invalidSource = Context();
            invalidSource.source->line = 0;
            invalidSource.source->column = 2;
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, error, invalidSource),
                           PrefabErrors::DiagnosticInvalid);

            auto invalidProperty = Context();
            const auto otherObject = PrefabObjectAddress::Create({}, LocalObjectId{43});
            REQUIRE(otherObject.HasValue());
            invalidProperty.property->operator=(
                PrefabPropertyAddress::Create(otherObject.Value(), invalidProperty.property->ComponentType(),
                                              invalidProperty.property->ComponentInstance(), invalidProperty.property->Property())
                    .Value());
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Override, error, invalidProperty,
                                                      PrefabDiagnosticOperation::ApplyOverride),
                           PrefabErrors::DiagnosticInvalid);

            auto unknownCategory = MakePrefabDiagnosticRecord(static_cast<PrefabDiagnosticCategory>(255), error, Context());
            RequireFailure(unknownCategory, PrefabErrors::DiagnosticUnsupported);
            auto unknownOperation =
                MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, error, Context(), static_cast<PrefabDiagnosticOperation>(255));
            RequireFailure(unknownOperation, PrefabErrors::DiagnosticUnsupported);

            auto unknownPrefabCode = MakeError(PrefabErrors::DocumentInvalid);
            unknownPrefabCode.code = ErrorCode{"prefab.future.unknown"};
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, unknownPrefabCode, Context()),
                           PrefabErrors::DiagnosticUnsupported);
            RequireFailure(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source,
                                                      ForeignError("native.filesystem", "filesystem.read_failed"), Context()),
                           PrefabErrors::DiagnosticUnsupported);
        }

        TEST_CASE("Prefab diagnostic records project cleanly into existing Build Output and survive source retirement",
                  "[prefab][diagnostics]") {
            std::vector<PrefabDiagnosticRecord> retained;
            {
                auto source = MakeError(PrefabErrors::MigrationFailed, "migration source text");
                const auto record =
                    MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Migration, PrefabDiagnosticOperation::Migrate, source, Context());
                REQUIRE(record.HasValue());
                const auto output = MakePrefabBuildOutputRecord(record.Value());
                REQUIRE(output.operationId == OperationId{31});
                REQUIRE(output.severity == DiagnosticSeverity::Error);
                REQUIRE(output.result == BuildOutputResult::Failed);
                REQUIRE(output.stage == "migrate");
                REQUIRE(output.code.Value() == PrefabErrors::MigrationFailed.code.Value());
                REQUIRE(output.source->absolutePath == "/project/assets/prefabs/player.prefab");
                retained.push_back(std::move(record).Value());
            }
            REQUIRE(retained.front().prefabAsset == Asset(1));
            REQUIRE(retained.front().message == "migration source text");
            REQUIRE(retained.front().DependencyChain().empty());
        }

        TEST_CASE("Prefab diagnostic descriptor table covers every stable prefab error", "[prefab][diagnostics]") {
            REQUIRE(PrefabDiagnosticErrorDescriptors().size() >= 50);
            for (const ErrorCodeDescriptor *descriptor : PrefabDiagnosticErrorDescriptors()) {
                CAPTURE(descriptor->code.Value());
                REQUIRE(MakePrefabDiagnosticRecord(PrefabDiagnosticCategory::Source, MakeError(*descriptor), Context()).HasValue());
            }
        }
    }  // namespace
}  // namespace Horo::Prefab
