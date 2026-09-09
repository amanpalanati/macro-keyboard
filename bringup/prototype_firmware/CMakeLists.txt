# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(prototype_firmware C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(prototype_firmware prototype_firmware.c )

pico_set_program_name(prototype_firmware "prototype_firmware")
pico_set_program_version(prototype_firmware "0.1")

# TinyUSB owns the USB device (HID keyboard + consumer).
# pico_stdio_usb cannot share the bus with a custom TinyUSB stack.
pico_enable_stdio_uart(prototype_firmware 0)
pico_enable_stdio_usb(prototype_firmware 0)

target_link_libraries(prototype_firmware
        pico_stdlib
        pico_unique_id
        hardware_i2c
        tinyusb_device
        tinyusb_board)

# Add the standard include files to the build
target_include_directories(prototype_firmware PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(prototype_firmware)

