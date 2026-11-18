cmake_minimum_required(VERSION 3.16)
project(mano VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MANO_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MANO_STATIC "Link a fully static executable (release builds on Linux/Windows)" OFF)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(NCURSESW QUIET IMPORTED_TARGET ncursesw)
endif()

add_executable(mano
    src/main.c
    src/asm.c
    src/backend.c
    src/cpu.c
    src/isa.c
    src/program.c
    src/tui.c
    src/util.c
    src/x86_backend.c
    src/arm64_backend.c
    src/arm32_backend.c
)

target_include_directories(mano PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(mano PRIVATE _DEFAULT_SOURCE MANO_VERSION="${PROJECT_VERSION}")

target_compile_options(mano PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wpointer-arith
    -Wwrite-strings
    -Wformat-security
)

if(MANO_ASAN)
    target_compile_options(mano PRIVATE
        -O1
        -g
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(mano PRIVATE
        -fsanitize=address,undefined
    )
else()
    target_compile_options(mano PRIVATE -O3)
endif()

if(MANO_STATIC)
    target_link_options(mano PRIVATE -static)
    if(WIN32)
        target_compile_definitions(mano PRIVATE NCURSES_STATIC)
    endif()
endif()

if(MANO_STATIC AND NCURSESW_FOUND)
    target_include_directories(mano PRIVATE ${NCURSESW_STATIC_INCLUDE_DIRS})
    target_link_libraries(mano PRIVATE ${NCURSESW_STATIC_LDFLAGS})
elseif(TARGET PkgConfig::NCURSESW)
    target_link_libraries(mano PRIVATE PkgConfig::NCURSESW)
else()
    # macOS ships a wide-capable libncurses without a pkg-config file.
    find_library(NCURSESW_LIB NAMES ncursesw ncurses REQUIRED)
    target_link_libraries(mano PRIVATE "${NCURSESW_LIB}")
endif()

set_target_properties(mano PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

install(TARGETS mano RUNTIME DESTINATION bin)
install(FILES README.md LICENSE DESTINATION share/doc/mano)
install(DIRECTORY examples/ DESTINATION share/doc/mano/examples)

set(CPACK_PACKAGE_NAME mano)
set(CPACK_PACKAGE_VENDOR "Yazdan Samari")
set(CPACK_PACKAGE_CONTACT "Yazdan Samari <yazdan.phdgamer0@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Assembler, simulator, TUI debugger and native compiler for the Mano basic computer")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/phdgamer0/mano")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_DEBIAN_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
include(CPack)

if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COPY_ON_ERROR SYMBOLIC
    )
endif()

