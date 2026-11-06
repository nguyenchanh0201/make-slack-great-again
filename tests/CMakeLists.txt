# The test suites (MSGA_BUILD_TESTS=ON, added from the root CMakeLists.txt).
# One directory per module under ../src, each defining its own ctest tests;
# support/ holds what several of them share. Everything a test reads from
# disk is in assets/ (MSGA_TEST_ASSETS): the fake workspace's fixture.json,
# its pictures, GIFs and files, and the SVG fixtures.
#
# plat's own tests (plat/) belong to its standalone build (cmake -S src/plat),
# which adds that directory itself; the app build leaves them out.
get_filename_component(MSGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter QUIET)

# ── Shared ──────────────────────────────────────────────────────────────────
# The harness: TEST/CHECK/REQUIRE and the portable helpers (support/test.h).
add_library(msga_test STATIC support/test.cpp)
target_include_directories(msga_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msga_test PUBLIC msga_base msga_flags)

# msga_add_test(<name> COMMAND <exe> [suite] [group] [TZ] [HOME <dir>]
#               [ENV <var=value>...] [LABELS <label>...])
# One ctest test, on plat's headless backend. TZ pins a zone, so "local
# time" is the same anywhere; HOME gives the test a throwaway home <dir>
# with the XDG folders under it, so it never touches the user's settings or
# caches. Without one, runAll makes a temporary home of its own (deleted at
# exit); a <dir> must have "test-home" in its name for runAll to keep it
# (support/test.cpp).
function(msga_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "TZ" "HOME" "COMMAND;ENV;LABELS")
    set(env "PLAT_BACKEND=headless")
    if(arg_TZ)
        list(APPEND env "TZ=Europe/Stockholm")
    endif()
    if(arg_HOME)
        if(NOT arg_HOME MATCHES "test-home")
            message(FATAL_ERROR "msga_add_test(${name}): HOME ${arg_HOME} has no \"test-home\"")
        endif()
        list(APPEND env "HOME=${arg_HOME}" "XDG_CONFIG_HOME=${arg_HOME}/.config"
            "XDG_CACHE_HOME=${arg_HOME}/.cache" "XDG_DATA_HOME=${arg_HOME}/.local/share"
            "XDG_STATE_HOME=${arg_HOME}/.local/state")
    endif()
    list(APPEND env ${arg_ENV})
    add_test(NAME ${name} COMMAND ${arg_COMMAND})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${env}")
    if(arg_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${arg_LABELS}")
    endif()
endfunction()

# A fake AI server (support/fake_llm.py) for any suite that needs answers:
# the llm tests, and the messages/settings/shell screens'. POSIX only.
if(TARGET msga_app_llm)
    add_library(msga_llm_testkit STATIC support/fake_llm_server.cpp)
    target_link_libraries(msga_llm_testkit PUBLIC msga_app_llm msga_test)
    target_compile_definitions(msga_llm_testkit PRIVATE
        FAKE_LLM_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/support/fake_llm.py")
endif()

if(APPLE)
    enable_language(OBJCXX)
    add_executable(cocoa_notification_sound_tests plat/test_cocoa_notification_sound.mm)
    set_target_properties(cocoa_notification_sound_tests PROPERTIES OBJCXX_STANDARD 20)
    target_compile_options(cocoa_notification_sound_tests PRIVATE -fobjc-arc -UNDEBUG)
    target_include_directories(cocoa_notification_sound_tests PRIVATE
        "${MSGA_ROOT}/src/plat/src" "${MSGA_ROOT}/src/plat/include" "${MSGA_ROOT}/src/prim/include")
    target_link_libraries(cocoa_notification_sound_tests PRIVATE
        "-framework AppKit" "-framework UserNotifications"
        "-framework UniformTypeIdentifiers" "-framework ImageIO")
    # The private resolver is included directly; unused CocoaApp entry points
    # need no app runtime. Only the resolver runs, with explicit temporary paths.
    target_link_options(cocoa_notification_sound_tests PRIVATE
        -Wl,-dead_strip -Wl,-undefined,dynamic_lookup)
    add_test(NAME cocoa_notification_sound_tests COMMAND cocoa_notification_sound_tests)
    set_tests_properties(cocoa_notification_sound_tests PROPERTIES LABELS core)
endif()

# ── Suites, bottom-up ───────────────────────────────────────────────────────
foreach(dir base net gfx text ui app)
    if(TARGET msga_${dir} OR (dir STREQUAL "app" AND TARGET msga_app_core))
        add_subdirectory(${dir})
    endif()
endforeach()
