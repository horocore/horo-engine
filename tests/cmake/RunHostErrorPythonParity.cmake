# The build graph supplies the fixture and Python interpreter paths. Python
# consumes the fixture's JSON through stdin and never launches a CLI-supplied executable.
foreach(required IN ITEMS HOST_ERROR_FIXTURE HOST_ERROR_PYTHON HOST_ERROR_TEST)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "Missing host error parity input: ${required}")
    endif()
endforeach()

execute_process(
    COMMAND "${HOST_ERROR_FIXTURE}"
    # Keep parity independent of site packages and effective under optimisation.
    COMMAND "${HOST_ERROR_PYTHON}" -S -O "${HOST_ERROR_TEST}"
    TIMEOUT 30
    COMMAND_ERROR_IS_FATAL ANY
)
