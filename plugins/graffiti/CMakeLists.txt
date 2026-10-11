# graffiti — Plugify C++ port of the Metamod:Source plugin cs2-graffiti (CS2 graffiti for
# everyone: the server sprays a player_spray_decal with a design and a tint from the game files).
#
#   graffiti_core       engine-free core (config, catalog parsing, decal geometry; also used by
#                       test/graffiti_core_tests.cpp)
#   plugify-graffiti    the plugin shared library (entry "bin/graffiti")

set(GRAFFITI_VERSION "1.0.0")

# --- core ---------------------------------------------------------------------
add_library(graffiti_core STATIC src/graffiti_core.cpp)
target_include_directories(graffiti_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(graffiti_core PUBLIC pisexcompat)
target_compile_options(graffiti_core PRIVATE -Wall -Wextra -Werror)
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(graffiti_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(graffiti_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-graffiti SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-graffiti PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(plugify-graffiti SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-graffiti PRIVATE
        graffiti_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-graffiti PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-graffiti PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="graffiti"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-graffiti PROPERTIES OUTPUT_NAME graffiti)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-graffiti PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/graffiti.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/graffiti.pplugin"
)

# --- tests ---------------------------------------------------------------------
# The catalog parser is driven with a format-faithful sample of items_game.txt and
# resource/csgo_<lang>.txt (test/data/): the real files live in the game install, which a test
# container does not have.
add_executable(graffiti_tests test/graffiti_core_tests.cpp)
target_link_libraries(graffiti_tests PRIVATE graffiti_core -static-libstdc++ -static-libgcc)
target_compile_options(graffiti_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(graffiti_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        GR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data"
        GR_PROD_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
set_target_properties(graffiti_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
