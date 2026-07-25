cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32p4")

# The network updater ("OTA-Firmware", docs/PLAN_NETWORK.md). One binary serves
# two roles, deliberately:
#   - flashed into `factory`, it is the recovery updater that no OTA can destroy;
#   - pushed into ota_0/ota_1, it is a known-good application that validates
#     itself, so it can be used to prove the update path end to end.
# It contains no camera, no GCP and none of the statistics tables — that is why
# it fits the ~130 KB RAM budget (measured: 68 KB static).
#
# The update endpoint and boot-safety logic come from the SHARED
# components/elotto_ota, the same code the master and slave run: three copies of
# the logic that decides whether a node is still reachable would be three
# chances to get a recovery path subtly wrong on one node only.
#
# Points at the single component, NOT at ../components: that directory also holds
# elotto_camera, and IDF compiles every component it discovers. Pulling the
# camera in here would drag PSRAM and the sensor driver into the recovery image —
# the opposite of the point. A path containing CMakeLists.txt is treated as the
# component itself (tools/cmake/project.cmake, __project_component_dir).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/elotto_ota")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elotto_ota)
