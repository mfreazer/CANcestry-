# CANcestry BMS governor conformance (Phase 12, issue #30).
#
# The test exercises the pure governor directly, including normal operation,
# thermal intervention, BMS fault fail-closed behavior, invalid snapshots and
# arithmetic boundary cases. Requirement ids are cited in the source.

add_executable(cancestry_conformance_bms_governor
    test_bms_governor.c)

target_include_directories(cancestry_conformance_bms_governor
    PRIVATE
        "${CMAKE_SOURCE_DIR}/core/governor/include"
        "${CMAKE_SOURCE_DIR}/tests/unit/support")

target_link_libraries(cancestry_conformance_bms_governor
    PRIVATE cancestry_governor)

set_target_properties(cancestry_conformance_bms_governor PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

add_test(NAME cancestry_conformance_bms_governor
    COMMAND cancestry_conformance_bms_governor)
