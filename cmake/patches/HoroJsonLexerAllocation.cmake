# Reviewed against nlohmann/json 3.11.3, 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03.
# Checked STL bookkeeping may allocate even for an empty vector/string. Keep
# those failures inside the caller's exception boundary instead of terminating.
if(NOT DEFINED HORO_JSON_SOURCE_DIR)
    message(FATAL_ERROR "HORO_JSON_SOURCE_DIR is required for the reviewed lexer patch")
endif()

# Verify the entire set first: a drifted parser must not leave a patched lexer.
function(horo_json_allocation_file relative original_digest patched_digest apply)
    if(relative STREQUAL "json.hpp")
        set(path "${HORO_JSON_SOURCE_DIR}/include/nlohmann/json.hpp")
    else()
        set(path "${HORO_JSON_SOURCE_DIR}/include/nlohmann/detail/input/${relative}")
    endif()
    file(READ "${path}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual "${source}")
    if(actual STREQUAL patched_digest)
        return()
    endif()
    if(NOT actual STREQUAL original_digest)
        message(FATAL_ERROR "JSON lexer allocation patch source drift: ${relative} (${actual})")
    endif()
    if(NOT apply)
        return()
    endif()

    set(patched "${source}")
    if(relative STREQUAL "lexer.hpp")
        string(REPLACE
            "explicit lexer(InputAdapterType&& adapter, bool ignore_comments_ = false) noexcept"
            "explicit lexer(InputAdapterType&& adapter, bool ignore_comments_ = false)"
            patched "${patched}")
        string(REPLACE "std::vector<char_type> token_string {};"
            "std::vector<char_type> token_string = std::vector<char_type>(std::size_t{0});"
            patched "${patched}")
        string(REPLACE "string_t token_buffer {};"
            "string_t token_buffer = string_t(std::size_t{0}, typename string_t::value_type{});"
            patched "${patched}")
    elseif(relative STREQUAL "json_sax.hpp")
        string(REPLACE "std::vector<BasicJsonType*> ref_stack {};"
            "std::vector<BasicJsonType*> ref_stack = std::vector<BasicJsonType*>(std::size_t{0});"
            patched "${patched}")
        string(REPLACE "std::vector<bool> keep_stack {};"
            "std::vector<bool> keep_stack = std::vector<bool>(std::size_t{0});"
            patched "${patched}")
        string(REPLACE "std::vector<bool> key_keep_stack {};"
            "std::vector<bool> key_keep_stack = std::vector<bool>(std::size_t{0});"
            patched "${patched}")
    elseif(relative STREQUAL "parser.hpp")
        string(REPLACE "std::vector<bool> states;" "std::vector<bool> states(std::size_t{0});"
            patched "${patched}")
    elseif(relative STREQUAL "json.hpp")
        # Use the count/value overload already required by the upstream data constructor.
        string(REPLACE "create<array_t>()" "create<array_t>(std::size_t{0}, basic_json{})"
            patched "${patched}")
        # Empty containers need no traversal stack; retain the original destruction/deallocation below it.
        string(REPLACE "if (t == value_t::array || t == value_t::object)"
            "if ((t == value_t::array && !array->empty()) || (t == value_t::object && !object->empty()))"
            patched "${patched}")
    endif()
    string(SHA256 calculated "${patched}")
    if(NOT calculated STREQUAL patched_digest)
        message(FATAL_ERROR "JSON lexer allocation patch output differs from reviewed digest: ${relative} (${calculated})")
    endif()
    file(WRITE "${path}" "${patched}")
endfunction()

foreach(apply IN ITEMS FALSE TRUE)
    horo_json_allocation_file("lexer.hpp"
        "550c275f09d55deb1705b97b8a15b02f211ea0ba87e95452e7a023b854bb024c"
        "368bd04919671910654dd0489b0691e84b8afe408051f50ee4cd4e7af45741c4" ${apply})
    horo_json_allocation_file("json_sax.hpp"
        "9147c9d07784a5f3de890d68401ae07d03d0bc0db24ebcbe7beb8a31cfd858a0"
        "af661865ca8c60aed74ef3c1a56c5299dc3fd6369e88288ce0edf13f6ffe9f0b" ${apply})
    horo_json_allocation_file("parser.hpp"
        "094aed22cfdd641ac15ef59bfef8dd1e23fd92373633a92ed928cb580f0cbd7e"
        "107b7ca6308acfa0a58d59fecaa5d0a66d75100bdbd0190e3b493ee6bac2c32c" ${apply})
    horo_json_allocation_file("json.hpp"
        "2432cbf7f4258453538eca193cd937fc777462ae2ce8ee9f46e6ee460ef56bea"
        "b7fea417b98ff85c07c3061bec8553df94cb371ddc45cc73ec09a0a4cae65b23" ${apply})
endforeach()
