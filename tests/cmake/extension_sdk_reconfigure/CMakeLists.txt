cmake_minimum_required(VERSION 3.25)

project(HoroExtensionSdkReconfigure NONE)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HORO_EXTENSION_SDK_VERSION "0.1.0")
set(HORO_EXTENSION_ABI_MAJOR_VERSION 1)
set(HORO_EXTENSION_ABI_PUBLISHED_MINOR 8)
set(HORO_EXTENSION_ABI_MIN_HOST_MINOR 0)
set(HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE 2)

include("${HORO_EXTENSION_SDK_MODULE}")
horo_configure_extension_sdk()
