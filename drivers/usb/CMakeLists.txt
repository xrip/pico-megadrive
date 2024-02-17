set(FAMILY rp2040)
set(BOARD pico_sdk)
family_configure_device_example(usbdisk noos)

add_library(usbdisk INTERFACE ${CMAKE_CURRENT_LIST_DIR})

# Make our header file(s) accessible both within and external to the library.
target_include_directories(usbdisk INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_sources(usbdisk INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
        ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
        ${CMAKE_CURRENT_LIST_DIR}/usb.c
)

# Specify the Pico C/C++ SDK libraries we need
target_link_libraries(usbdisk INTERFACE
  pico_stdlib  tinyusb_device tinyusb_boards tinyusb_host
)

