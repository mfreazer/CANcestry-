# CANcestry gateway integration harness (issue #13, Phase 5).
#
# The acceptance criteria of issue #13 require this program to compile with
# -Wall -Wextra -Werror -Wpedantic -Wconversion and to run under ASan/UBSan
# with zero errors and zero dynamic allocations in the processing loop. The
# project-wide warning set already covers the first four flags; -Wconversion
# is added here on top for this target only.

add_executable(cancestry_gateway main.c)

target_link_libraries(cancestry_gateway PRIVATE
    cancestry_event
    cancestry_codec
    cancestry_codec_loader
    cancestry_recipe
    cancestry_recipe_loader
    cancestry_fsm
    cancestry_fsm_loader)

set_target_properties(cancestry_gateway PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

if(NOT MSVC)
    target_compile_options(cancestry_gateway PRIVATE -Wconversion)
endif()

# The processing loop must run with zero heap allocation. The executable links
# the loaders (which allocate by contract at load time), so the scan runs on
# this example's own object file instead: it may not reference any allocator
# entry point (SYS-NF-002, issue #13 constraint 1). Single-config generators
# only (Makefiles/Ninja layout); the runtime tripwire in main.c covers the
# remaining configurations on glibc hosts.
find_program(CANCESTRY_PYTHON NAMES python3 python)
if(CANCESTRY_PYTHON AND NOT CMAKE_CROSSCOMPILING AND NOT CMAKE_CONFIGURATION_TYPES)
    add_test(NAME cancestry_gateway_no_alloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../../ci/check_no_alloc.py"
                "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/cancestry_gateway.dir/main.c.o")
endif()

add_test(NAME cancestry_gateway_loop COMMAND cancestry_gateway)
