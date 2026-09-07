cmake_minimum_required(VERSION 3.20)
# Single source of truth for the version: the resource block, the About box and the
# release tag all read from here.
project(HDRScope VERSION 1.0.2 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "HDRScope for Windows builds with MSVC. Use the Visual Studio generator.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Windows SDK ships generated C++/WinRT headers, but the MSVC toolchain does not put
# that directory on the include path by default. Locate it so no NuGet package is needed.
set(WINDOWS_KITS_CANDIDATES
  "$ENV{WindowsSdkDir}"
  "$ENV{ProgramFiles\(x86\)}/Windows Kits/10"
  "$ENV{ProgramFiles}/Windows Kits/10"
  "C:/Program Files (x86)/Windows Kits/10")
set(CPPWINRT_INCLUDE_DIR "")
foreach(kit ${WINDOWS_KITS_CANDIDATES})
  if(kit AND EXISTS "${kit}/Include")
    file(GLOB found "${kit}/Include/*/cppwinrt")
    if(found)
      list(SORT found)
      list(GET found -1 CPPWINRT_INCLUDE_DIR)
      break()
    endif()
  endif()
endforeach()
if(NOT CPPWINRT_INCLUDE_DIR)
  message(FATAL_ERROR "C++/WinRT headers (Include/<version>/cppwinrt) were not found. Install a Windows 10/11 SDK.")
endif()
message(STATUS "C++/WinRT headers: ${CPPWINRT_INCLUDE_DIR}")

configure_file(Resources/HDRScope.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HDRScope.rc" @ONLY)

add_executable(HDRScope WIN32
  src/main.cpp
  src/Common.cpp
  src/PixelImage.cpp
  src/IccProfile.cpp
  src/FloatTiff.cpp
  src/ImageFile.cpp
  src/HdrExport.cpp
  src/Display.cpp
  src/Capture.cpp
  src/Diagnostics.cpp
  src/Canvas.cpp
  src/MainWindow.cpp
  src/SelfTests.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/HDRScope.rc"
)

target_include_directories(HDRScope PRIVATE "${CPPWINRT_INCLUDE_DIR}" src)
target_compile_options(HDRScope PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_compile_definitions(HDRScope PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  HDRSCOPE_VERSION="${PROJECT_VERSION}")
target_link_libraries(HDRScope PRIVATE
  d3d11 dxgi d3dcompiler windowscodecs comctl32 comdlg32 ole32 oleaut32 shell32 shlwapi
  user32 gdi32 dwmapi windowsapp)

# Static C runtime, so a downloaded binary runs on a machine that has never had a
# Visual C++ redistributable installed. Nothing here crosses a DLL boundary, which is
# what would otherwise make a static CRT the wrong choice.
set_property(TARGET HDRScope PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(HDRScope PROPERTIES
  VS_DPI_AWARE "PerMonitor"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist")
