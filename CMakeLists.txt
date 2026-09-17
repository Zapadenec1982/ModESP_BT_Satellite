cmake_minimum_required(VERSION 3.16)

# components/bt is built by tools/setup.ps1 from the pinned IDF plus patches/.
# When it is present it shadows the IDF's own bt component, which is how the codec
# patches reach the build (docs/BASE.md). When it is absent the stock one is used
# and the device speaks SBC only.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(modesp_bt_satellite)
