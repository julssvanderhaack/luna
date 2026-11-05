find_package(Qt6 REQUIRED COMPONENTS Test)

# Every test runs with its own HOME and XDG folders under the build tree,
# emptied before each run, and without a display or desktop theme: nothing
# touches the real settings, cache, trash or keys.
function(luna_test name)
  add_executable(${name} ${name}.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE lunacore Qt6::Test)
  set(home ${CMAKE_CURRENT_BINARY_DIR}/home-${name})
  add_test(NAME ${name}_home COMMAND ${CMAKE_COMMAND} -DDIR=${home} -P ${CMAKE_CURRENT_SOURCE_DIR}/fresh_home.cmake)
  set_tests_properties(${name}_home PROPERTIES FIXTURES_SETUP ${name}_home)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ${name}_home TIMEOUT 120 ENVIRONMENT
    "HOME=${home};XDG_CONFIG_HOME=${home}/.config;XDG_CACHE_HOME=${home}/.cache;XDG_DATA_HOME=${home}/.local/share;GNUPGHOME=${home}/.gnupg;QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;LANG=C.UTF-8;LC_ALL=C.UTF-8")
endfunction()

luna_test(tst_fileops)
luna_test(tst_archives)
luna_test(tst_remote)
luna_test(tst_admin)
luna_test(tst_pane)
luna_test(tst_fsmodel)
luna_test(tst_crypto)
luna_test(tst_i18n ${PROJECT_SOURCE_DIR}/data/luna.qrc)
target_compile_definitions(tst_i18n PRIVATE LUNA_TS="${PROJECT_SOURCE_DIR}/i18n/luna_en_US.ts")
if(LUNA_WITH_TERMINAL AND VTERM_FOUND)
  luna_test(tst_terminal)
endif()
