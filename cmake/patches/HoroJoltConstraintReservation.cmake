# Reviewed against Jolt 5.6.0 / e77f175595e64cb44218cc9d9d56fc365ad0e36a.
# Only supported PhysicsSystem capacity methods are exposed; no registered state changes.
if(NOT DEFINED JOLT_SOURCE_DIR)
    message(FATAL_ERROR "JOLT_SOURCE_DIR is required for the reviewed reservation patch")
endif()

# Verify the entire source set before modifying any member: drift cannot leave a partially applied patch.
function(horo_verify_jolt_file relative original_digest patched_digest)
    file(READ "${JOLT_SOURCE_DIR}/Jolt/Physics/${relative}" source)
    # CMake writes text with native line endings on Windows. Verify canonical content
    # for both fresh archives and previously patched trees without weakening drift checks.
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual "${source}")
    if(NOT actual STREQUAL original_digest AND NOT actual STREQUAL patched_digest)
        message(FATAL_ERROR "Jolt reservation patch source drift: ${relative} (${actual})")
    endif()
endfunction()
horo_verify_jolt_file("Constraints/ConstraintManager.h"
    "61647d0e6be1bbf9a230c6a40b2b9a8d7f64960f06865538155f747f71824ea7"
    "bf6d83c9e39541550222d4b88b0c5193931707f2eb950f03410fb8a391320336")
horo_verify_jolt_file("Constraints/ConstraintManager.cpp"
    "6b2d208bf071441190cfcbb739db5a885b41d7e0c720fe2b481238c2975afe18"
    "c8be632f8cae8754e3128106a16e9e24f1c736992015f6d56f9b1399455d4d40")
horo_verify_jolt_file("PhysicsSystem.h"
    "fcb6f9500c59535c86d8823c4b9f31863e272df5d4646e33e4d745d171986bd4"
    "252c910f85c21af2e9f75eaec8b5445d0b20bf85bf9d7bc541f076dd6e09729d")

function(horo_patch_jolt_file relative original_digest patched_digest needle addition)
    set(path "${JOLT_SOURCE_DIR}/Jolt/Physics/${relative}")
    file(READ "${path}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual "${source}")
    if(actual STREQUAL patched_digest)
        return()
    endif()
    if(NOT actual STREQUAL original_digest)
        message(FATAL_ERROR "Jolt reservation patch source drift: ${relative} (${actual})")
    endif()
    string(FIND "${source}" "${needle}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "Jolt reservation patch context missing: ${relative}")
    endif()
    string(REPLACE "${needle}" "${addition}${needle}" patched "${source}")
    string(SHA256 calculated "${patched}")
    if(NOT calculated STREQUAL patched_digest)
        message(FATAL_ERROR "Jolt reservation patch output differs from reviewed digest: ${relative}")
    endif()
    file(WRITE "${path}" "${patched}")
endfunction()

horo_patch_jolt_file("Constraints/ConstraintManager.h"
    "61647d0e6be1bbf9a230c6a40b2b9a8d7f64960f06865538155f747f71824ea7"
    "bf6d83c9e39541550222d4b88b0c5193931707f2eb950f03410fb8a391320336"
    "\t/// Add a new constraint. This is thread safe."
    "\t/// Horo patch: reserve detached-publication capacity without registering constraints.\n\tuint32 Reserve(uint32 inCapacity);\n\tuint32 GetCapacity() const;\n\n")

horo_patch_jolt_file("Constraints/ConstraintManager.cpp"
    "6b2d208bf071441190cfcbb739db5a885b41d7e0c720fe2b481238c2975afe18"
    "c8be632f8cae8754e3128106a16e9e24f1c736992015f6d56f9b1399455d4d40"
    "void ConstraintManager::Add(Constraint **inConstraints, int inNumber)"
    "// Horo patch: capacity-only operations preserve all constraint indices and registrations.\nuint32 ConstraintManager::Reserve(uint32 inCapacity)\n{\n\tUniqueLock lock(mConstraintsMutex JPH_IF_ENABLE_ASSERTS(, mLockContext, EPhysicsLockTypes::ConstraintsList));\n\tmConstraints.reserve(inCapacity);\n\treturn uint32(mConstraints.capacity());\n}\n\nuint32 ConstraintManager::GetCapacity() const\n{\n\tUniqueLock lock(mConstraintsMutex JPH_IF_ENABLE_ASSERTS(, mLockContext, EPhysicsLockTypes::ConstraintsList));\n\treturn uint32(mConstraints.capacity());\n}\n\n")

horo_patch_jolt_file("PhysicsSystem.h"
    "fcb6f9500c59535c86d8823c4b9f31863e272df5d4646e33e4d745d171986bd4"
    "252c910f85c21af2e9f75eaec8b5445d0b20bf85bf9d7bc541f076dd6e09729d"
    "\t/// Add constraint to the world"
    "\t/// Horo patch: owner-safe capacity reservation; no constraint is registered or activated.\n\tuint32 ReserveConstraints(uint32 inCapacity) { return mConstraintManager.Reserve(inCapacity); }\n\tuint32 GetConstraintCapacity() const { return mConstraintManager.GetCapacity(); }\n\n")
