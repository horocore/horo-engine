#pragma once
/** @file CliOutputContractFixture.h @brief Versioned native CLI schema assertions shared by actual adapter regressions. */

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

namespace Horo::Tests::CliContract {
    /** @brief Loads the checked-in fixture selected by the test target.
     * @return Process-owned immutable versioned schema fixture.
     */
    inline const nlohmann::json &Fixture() {
        static const auto fixture = [] {
            std::ifstream input(HORO_CLI_OUTPUT_FIXTURE);
            if (!input)
                throw std::runtime_error("CLI contract fixture is unavailable");
            return nlohmann::json::parse(input);
        }();
        return fixture;
    }

    /** @brief Preserves integer contracts while permitting either JSON number representation for general numeric fields.
     * @param value Actual result field.
     * @param type Versioned fixture type name.
     * @return Whether the field retains its declared JSON type.
     */
    inline bool MatchesType(const nlohmann::json &value, const std::string_view type) {
        if (type == "integer")
            return value.is_number_integer();
        if (type == "number")
            return value.is_number();
        return value.type_name() == type;
    }

    /** @brief Checks required fields while allowing compatible additive fields.
     * @param value Actual result object.
     * @param fields Required field/type pairs from the fixture.
     */
    inline void ValidateFields(const nlohmann::json &value, const nlohmann::json &fields) {
        for (const auto &[name, type] : fields.items()) {
            if (!value.contains(name) || !MatchesType(value.at(name), type.get<std::string>()))
                throw std::runtime_error("CLI contract field changed: " + name);
        }
    }

    /** @brief Checks an adapter's declared schema identity and version before interpreting its result.
     * @param command Canonical dot-separated command path.
     * @param schema Exact output schema identity.
     * @param version Command output version.
     */
    inline void ValidateSchema(const std::string_view command, const std::string_view schema, const unsigned version) {
        const auto &contract = Fixture().at("commandSchemas").at(schema);
        if (contract.at("command") != command || contract.at("version") != version)
            throw std::runtime_error("CLI command schema identity or version changed");
    }

    /** @brief Checks the successful payload emitted by the real shared producer.
     * @param schema Exact command output schema identity.
     * @param result Actual shared producer result object.
     */
    inline void ValidateResult(const std::string_view schema, const nlohmann::json &result) {
        const auto &contract = Fixture().at("commandSchemas").at(schema);
        ValidateFields(result, contract.at("fields"));
        if (contract.contains("publicationFields") && result.at("published") == true)
            ValidateFields(result, contract.at("publicationFields"));
    }

    /** @brief Checks an actual protocol envelope against the complete public schema inventory.
     * @param value Actual decoded CLI terminal envelope.
     */
    inline void ValidateEnvelope(const nlohmann::json &value) {
        for (const auto &field : Fixture().at("requiredEnvelopeFields")) {
            if (!value.contains(field.get<std::string>()))
                throw std::runtime_error("CLI envelope field is missing");
        }
        if (value.at("schemaVersion") != Fixture().at("schemaVersion"))
            throw std::runtime_error("CLI envelope version changed");
        const auto schema = value.at("outputSchema").at("id").get<std::string>();
        ValidateSchema(value.at("command").get<std::string>(), schema, value.at("outputSchema").at("version").get<unsigned>());
        if (value.at("status") == "succeeded")
            ValidateResult(schema, value.at("result"));
    }
}  // namespace Horo::Tests::CliContract
