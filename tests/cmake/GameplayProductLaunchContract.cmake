# The executable and fixture paths come only from CTest's generated target configuration.
foreach(required ENGINE MODULE REVISION_FILE SOURCE_ROOT PHYSICS_NATIVE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing CTest launch fixture input: ${required}")
    endif()
endforeach()
if(NOT IS_ABSOLUTE "${ENGINE}" OR NOT EXISTS "${ENGINE}")
    message(FATAL_ERROR "CTest must supply its absolute built engine target")
endif()
file(READ "${REVISION_FILE}" revision LIMIT 128)
string(STRIP "${revision}" revision)
set(script "${SOURCE_ROOT}/tests/fixtures/gameplay_physics/PhysicsContext.horo_script")
if(NOT DEFINED PROCESS_TIMEOUT)
    set(PROCESS_TIMEOUT 30)
endif()
if(NOT PROCESS_TIMEOUT MATCHES "^[1-9][0-9]*$" OR PROCESS_TIMEOUT GREATER 120)
    message(FATAL_ERROR "Launch fixture timeout must be between 1 and 120 seconds")
endif()

function(expect_launch expected count kind first second third permission)
    # Quote each fixed argument independently: empty identities must reach the real parser.
    if(count EQUAL 4)
        execute_process(COMMAND "${ENGINE}" --run-network-product standalone 2
            "${kind}" "${first}" "${second}" "${third}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT "${PROCESS_TIMEOUT}")
    elseif(count EQUAL 6)
        execute_process(COMMAND "${ENGINE}" --run-network-product standalone 2
            "${kind}" "${first}" "${second}" "${third}" "${permission}" extra
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT "${PROCESS_TIMEOUT}")
    else()
        execute_process(COMMAND "${ENGINE}" --run-network-product standalone 2
            "${kind}" "${first}" "${second}" "${third}" "${permission}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT "${PROCESS_TIMEOUT}")
    endif()
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR "Launch ${kind} returned ${result}, expected ${expected}: ${output}${error}")
    endif()
    if(expected EQUAL 0)
        if(NOT output MATCHES "network product completed 2 frames")
            message(FATAL_ERROR "Missing success receipt: ${output}${error}")
        endif()
    elseif(NOT error MATCHES "horo-engine:")
        message(FATAL_ERROR "Missing failure diagnostic: ${output}${error}")
    endif()
endfunction()

expect_launch(2 5 --unknown "${MODULE}" game.tests "${revision}" grant)
expect_launch(2 5 --game-module "${MODULE}" game.tests "${revision}" implicit)
expect_launch(2 5 --game-module relative.so game.tests "${revision}" grant)
foreach(invalid_revision 0 bad 1tail 18446744073709551616)
    expect_launch(2 5 --game-module "${MODULE}" game.tests "${invalid_revision}" grant)
endforeach()
expect_launch(2 5 --game-script relative.horo_script "${script}.meta" game.tests grant)
expect_launch(2 5 --game-script "${script}" relative.meta game.tests grant)
expect_launch(2 4 --game-module "${MODULE}" game.tests "${revision}" grant)
expect_launch(2 6 --game-module "${MODULE}" game.tests "${revision}" grant)
# OFF must reject startup rather than silently select another solver.
if(PHYSICS_NATIVE)
    set(expected 0)
else()
    set(expected 3)
endif()
foreach(permission grant deny)
    expect_launch(${expected} 5 --game-module "${MODULE}" game.tests "${revision}" "${permission}")
    expect_launch(${expected} 5 --game-script "${script}" "${script}.meta" game.tests "${permission}")
endforeach()
expect_launch(3 5 --game-script "${script}.missing" "${script}.meta" game.tests grant)
expect_launch(3 5 --game-module "${MODULE}" "" "${revision}" grant)
expect_launch(3 5 --game-script "${script}" "${script}.meta" "" grant)
# Independent prefab capability policy must not widen the existing Physics permission suffix.
function(expect_prefab_launch expected)
    execute_process(COMMAND "${ENGINE}" --run-network-product standalone 2 ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT "${PROCESS_TIMEOUT}")
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR "Prefab permission returned ${result}, expected ${expected}: ${output}${error}")
    endif()
    if(NOT expected EQUAL 0 AND NOT error MATCHES "horo-engine:")
        message(FATAL_ERROR "Missing typed prefab policy failure diagnostic: ${output}${error}")
    endif()
endfunction()
expect_prefab_launch(2 --prefab)
expect_prefab_launch(2 --prefab implicit)
expect_prefab_launch(2 --prefab deny)
expect_prefab_launch(2 --game-module "${MODULE}" game.tests "${revision}" deny --prefab deny --prefab grant)
expect_prefab_launch(2 --game-module "${MODULE}" game.tests "${revision}" deny --prefab grant)
expect_prefab_launch(${expected} --game-module "${MODULE}" game.tests "${revision}" deny --prefab deny)
expect_prefab_launch(3 --project "${SOURCE_ROOT}" --scene source:Assets/absent.horo --physics null
    --game-module "${MODULE}" game.tests "${revision}" deny --prefab grant)
message(STATUS "18 gameplay and 7 independent prefab policy launch cases passed")

# A recognized selection must reach actual project admission. Incomplete or ambiguous inputs fail at the CLI boundary.
function(expect_ai_launch expected diagnostic)
    execute_process(COMMAND "${ENGINE}" --run-network-product standalone 2 ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT "${PROCESS_TIMEOUT}")
    if(NOT "${result}" STREQUAL "${expected}" OR NOT error MATCHES "${diagnostic}")
        message(FATAL_ERROR "AI launch returned ${result}, expected ${expected} with ${diagnostic}: ${output}${error}")
    endif()
endfunction()
set(scene_id 137a4cd0-c9fe-43c4-b45e-9c8185472561)
set(decision_id ac2f250c-1ce7-4e87-bdc2-abd86bd607ee)
expect_ai_launch(2 "Use --ai-decision" --ai-decision)
expect_ai_launch(2 "Invalid AI controller identity" --ai-decision 0 "${decision_id}")
expect_ai_launch(2 "horo-engine:" --ai-decision 1 malformed)
expect_ai_launch(2 "AI decisions require standalone mode" --ai-decision 1 "${decision_id}")
expect_ai_launch(2 "AI decisions require standalone mode"
    --project "${SOURCE_ROOT}" --scene "cooked:${scene_id}" --cooked-root "${SOURCE_ROOT}" --cook-target headless-null
    --game-script "${script}" "${script}.meta" game.tests deny --ai-decision 1 "${decision_id}")
expect_ai_launch(3 "horo-engine:"
    --project "${SOURCE_ROOT}" --scene "cooked:${scene_id}" --cooked-root "${SOURCE_ROOT}" --cook-target headless-null
    --game-module "${MODULE}" game.tests "${revision}" deny --ai-decision 1 "${decision_id}")
message(STATUS "6 explicit cooked AI launch cases passed")

set(prefab_id f0d42785-5a26-44e1-a37f-e5ecc5045523)
expect_ai_launch(2 "Use --ai-prefab-decision" --ai-prefab-decision 1 "${decision_id}")
expect_ai_launch(2 "horo-engine:" --ai-prefab-decision 1 "${decision_id}" malformed)
expect_ai_launch(2 "AI prefab selection requires explicit --prefab grant"
    --project "${SOURCE_ROOT}" --scene "cooked:${scene_id}" --cooked-root "${SOURCE_ROOT}" --cook-target headless-null
    --game-module "${MODULE}" game.tests "${revision}" deny --ai-prefab-decision 1 "${decision_id}" "${prefab_id}")
message(STATUS "3 prefab-only AI option refusals passed")
