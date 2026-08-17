# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The Windows CreateProcess command-line limit is easily exceeded by this
# project's component include paths. Have Ninja pass compiler arguments via
# response files so the project also builds from long directory paths.
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files for Ninja commands" FORCE)

set(PROJECT_VER "2.1.0")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# Downgrade printf-format mismatches (e.g. in third-party managed components) from
# error back to warning. This is appended AFTER IDF's default "-Werror=all" so that
# GCC 14.x does not abort the build on harmless format-specifier warnings.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format" APPEND)
