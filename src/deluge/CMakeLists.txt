configure_file(util/firmware_version.h.in util/firmware_version.h @ONLY)
add_subdirectory(dsp)
add_subdirectory(gui/l10n)
add_subdirectory(gui/menu_item/generate)
add_subdirectory(version)

# Add the sources to the target
file(GLOB_RECURSE deluge_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/*.S
  ${CMAKE_CURRENT_LIST_DIR}/*.c
  ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

add_dependencies(deluge deluge_strings deluge_version)

# Filter out DSP sources as those are built as part of the deluge_dsp library
list(FILTER deluge_SOURCES EXCLUDE REGEX .*/dsp/.*)

# NAM spike sources are built as part of the nam_core library (ARM mode, no LTO, with the
# NeuralAmpModelerCore/Eigen include paths) - see the bottom of this file
list(FILTER deluge_SOURCES EXCLUDE REGEX .*/processing/nam/.*)

# Ensure the menu sources are built prior to building the main target
add_dependencies(deluge deluge_menus)

target_sources(deluge PUBLIC
  ${deluge_SOURCES}
  # Add strings-sources explicitly: since they're build artefacts clean nukes them,
  # and if they're not there in the first place then the source glob won't find them,
  # and they only get generated not built.
  ${deluge_strings_SOURCES}
)

target_include_directories(deluge PUBLIC
    ${deluge_version_INCLUDES}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${SHARED_INCLUDE}
)

target_link_libraries(deluge PUBLIC
    fatfs
    NE10
    RZA1
    eyalroz_printf
    deluge_dsp
)

target_compile_definitions(deluge PUBLIC
    $<$<CONFIG:DEBUG,RELWITHDEBINFO>:IN_HARDWARE_DEBUG=1>
    $<$<CONFIG:DEBUG,RELWITHDEBINFO>:ENABLE_TEXT_OUTPUT=1>
)

if(ENABLE_RTT)
    message(STATUS "RTT enabled for deluge")
    target_compile_definitions(deluge PUBLIC
        $<$<CONFIG:DEBUG,RELWITHDEBINFO>:HAVE_RTT=1>
    )
    target_link_libraries(deluge PUBLIC
        $<$<CONFIG:DEBUG,RELWITHDEBINFO>:RTT>
    )
endif(ENABLE_RTT)

if(ENABLE_SYSEX_LOAD)
    message(STATUS "Sysex firmware loading enabled for deluge")
    target_compile_definitions(deluge PUBLIC ENABLE_SYSEX_LOAD=1)
endif(ENABLE_SYSEX_LOAD)

if(ENABLE_MATRIX_DEBUG)
    message(STATUS "Extra pad matrix debug enabled for deluge")
    target_compile_definitions(deluge PUBLIC ENABLE_MATRIX_DEBUG=1)
endif(ENABLE_MATRIX_DEBUG)

# ============================ NAM spike (Phase 0) =============================
# NeuralAmpModelerCore + Eigen, fetched pinned; built as a separate static library so the
# inference code gets ARM mode and no LTO (like deluge_dsp) plus its own include paths.
# The SOURCE_SUBDIR pointing at a nonexistent directory stops MakeAvailable from running
# either project's own CMakeLists (NAM core's aborts on bare-metal platforms).
include(FetchContent)
# The commit NeuralAmpModelerCore pins as its submodule (3.4.0 proper miscompiles on
# arm-none-eabi, where int32_t is long int)
FetchContent_Declare(eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/bc3b39870ecb690a623a3f49149a358b95c5781d/eigen-bc3b39870ecb690a623a3f49149a358b95c5781d.tar.gz
    URL_HASH SHA256=5f6557d1aca6e959a4b955c55f2935261fd30de59c0c79e35bc6d5cbe1de22d1
    SOURCE_SUBDIR cmake-not-used
)
FetchContent_Declare(nam_core_src
    GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore
    GIT_TAG daae2d0b482a5fe876258caf3fdfe177044b6295
    GIT_SUBMODULES ""
    SOURCE_SUBDIR cmake-not-used
)
FetchContent_MakeAvailable(eigen nam_core_src)

add_library(nam_core STATIC
    ${nam_core_src_SOURCE_DIR}/NAM/activations.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/conv1d.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/dsp.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/ring_buffer.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/util.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/a2_fast.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/model.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/slimmable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/processing/nam/nam_baked_models.cpp
    ${CMAKE_CURRENT_LIST_DIR}/processing/nam/nam_core_stubs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/processing/nam/nam_newlib_hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/processing/nam/nam_spike.cpp
    # The libstdc++ iostream machinery NAM drags in needs the (normally unlinked) newlib
    # file-descriptor stubs
    ${CMAKE_CURRENT_LIST_DIR}/../sys_stubs.c
)

target_include_directories(nam_core SYSTEM PRIVATE
    ${nam_core_src_SOURCE_DIR}
    ${nam_core_src_SOURCE_DIR}/Dependencies/nlohmann
    ${eigen_SOURCE_DIR}
)
target_include_directories(nam_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${SHARED_INCLUDE}
    ${CMAKE_CURRENT_BINARY_DIR}
)

set_target_properties(nam_core
    PROPERTIES
        CXX_STANDARD ${DELUGE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
)

target_compile_definitions(nam_core PRIVATE
    NAM_SAMPLE_FLOAT
    NAM_ENABLE_A2_FAST
    EIGEN_DONT_PARALLELIZE
    $<$<CONFIG:DEBUG,RELWITHDEBINFO>:IN_HARDWARE_DEBUG=1>
    $<$<CONFIG:DEBUG,RELWITHDEBINFO>:ENABLE_TEXT_OUTPUT=1>
)

target_compile_options(nam_core PRIVATE
    "-fno-lto"
    "-finline-functions"
    "-marm"
)

target_link_libraries(nam_core PRIVATE
    fatfs
    RZA1
    eyalroz_printf
)

target_link_libraries(deluge PUBLIC nam_core)

