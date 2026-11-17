# msga_screens_messages on the test fixture (../../../assets), headless. A
# suite of msga_app_tests (../../CMakeLists.txt), one ctest test per case
# group ("image: ...").
target_sources(msga_app_tests PRIVATE test_messages.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_screens_messages msga_app_fake msga_llm_testkit)
# A throwaway HOME: the headless plat keeps its dirs (Downloads, caches,
# state) under it, never in the user's home, and no run inherits another's.
set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home")
foreach(t image grouping dates anchoring actions thread loading selection opening blocks threads audio users rows rich)
    msga_add_test(messages_${t} COMMAND msga_app_tests test_messages ${t} TZ HOME ${_home} LABELS messages)
endforeach()
