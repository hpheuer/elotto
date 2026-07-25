cmake_minimum_required(VERSION 3.16)

# Without this the target is only known from a pre-existing sdkconfig/build dir,
# so a clean checkout (or a regenerate after deleting sdkconfig, which the
# project's own config rule requires) fails with "CMAKE_C_COMPILER not set".
set(IDF_TARGET "esp32p4")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elotto)
