# msga_screens_shell: whole shells on the test fixture (../../../assets).
# Suites of msga_app_tests (../../CMakeLists.txt), one ctest test each.
target_link_libraries(msga_app_tests PRIVATE msga_screens_shell msga_app_fake msga_llm_testkit)

# Headless plat, a pinned zone, and a throwaway HOME/XDG so the tests never
# touch the user's settings or font index cache.
set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home")
foreach(s shell shortcuts pages shell_ui agents agent_ui links_ui unified_search theme_chrome)
    target_sources(msga_app_tests PRIVATE test_${s}.cpp)
endforeach()
msga_add_test(shell_tests COMMAND msga_app_tests test_shell TZ HOME ${_home} LABELS shell)
msga_add_test(shortcut_tests COMMAND msga_app_tests test_shortcuts TZ HOME ${_home} LABELS shell)
msga_add_test(pages_tests COMMAND msga_app_tests test_pages TZ HOME ${_home} LABELS shell)
msga_add_test(shell_ui_tests COMMAND msga_app_tests test_shell_ui TZ HOME ${_home} LABELS shell)
msga_add_test(agent_tests COMMAND msga_app_tests test_agents TZ HOME ${_home} LABELS shell)
msga_add_test(agent_ui_tests COMMAND msga_app_tests test_agent_ui TZ HOME ${_home} LABELS shell)
msga_add_test(links_ui_tests COMMAND msga_app_tests test_links_ui TZ HOME ${_home} LABELS shell)
msga_add_test(unified_search_tests COMMAND msga_app_tests test_unified_search TZ HOME ${_home} LABELS shell)
msga_add_test(theme_chrome_tests COMMAND msga_app_tests test_theme_chrome TZ HOME ${_home} LABELS shell)

# Its own throwaway HOME: it writes a launcher entry and a socket there.
target_sources(msga_app_tests PRIVATE test_identity.cpp)
msga_add_test(identity_tests COMMAND msga_app_tests test_identity
    HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home-identity LABELS shell)
