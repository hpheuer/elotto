cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32p4")

# The network updater ("OTA-Firmware", docs/PLAN_NETWORK.md). One binary serves
# two roles, deliberately:
#   - flashed into `factory`, it is the recovery updater that no OTA can destroy;
#   - pushed into ota_0/ota_1, it is a known-good application that validates
#     itself, so it can be used to prove the update path end to end.
# It contains no camera, no GCP and none of the statistics tables — that is why
# it fits the ~130 KB RAM budget.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elotto_ota)
