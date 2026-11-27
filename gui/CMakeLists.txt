cmake_minimum_required(VERSION 3.21)
project(nocry-gui VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

# Qt (up to at least 6.8) links Apple's AGL framework, which current macOS SDKs no longer ship
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  get_target_property(_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
  if(_gl_libs)
    list(FILTER _gl_libs EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_gl_libs}")
  endif()
endif()

# nocry-core through its C interface (host/ffi), built by cargo
set(NOCRY_HOST ${CMAKE_SOURCE_DIR}/../host)
set(NOCRY_FFI_LIB ${NOCRY_HOST}/target/release/${CMAKE_STATIC_LIBRARY_PREFIX}nocry_ffi${CMAKE_STATIC_LIBRARY_SUFFIX})
# always ask cargo (it is a no-op when nothing changed), so library edits reach the GUI
add_custom_target(nocry_ffi_build
  COMMAND cargo build --release -p nocry-ffi
  WORKING_DIRECTORY ${NOCRY_HOST}
  BYPRODUCTS ${NOCRY_FFI_LIB}
  COMMENT "cargo build nocry-ffi" VERBATIM)

set(SOURCES main.cpp resources.qrc)
if(WIN32)
  list(APPEND SOURCES packaging/nocry.rc)
endif()
if(APPLE)
  # packaging/nocry.icns is generated from nocry-1024.png by the release workflow
  if(EXISTS ${CMAKE_SOURCE_DIR}/packaging/nocry.icns)
    set(ICNS ${CMAKE_SOURCE_DIR}/packaging/nocry.icns)
    set_source_files_properties(${ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND SOURCES ${ICNS})
  endif()
endif()

qt_add_executable(nocry-gui WIN32 MACOSX_BUNDLE ${SOURCES})
add_dependencies(nocry-gui nocry_ffi_build)
target_include_directories(nocry-gui PRIVATE ${NOCRY_HOST}/ffi/include)
target_link_libraries(nocry-gui PRIVATE Qt6::Widgets Qt6::Concurrent ${NOCRY_FFI_LIB})
if(APPLE)
  set_target_properties(nocry-gui PROPERTIES
    OUTPUT_NAME nocry
    MACOSX_BUNDLE_BUNDLE_NAME nocry
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.destinal.nocry
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE nocry.icns)
  target_link_libraries(nocry-gui PRIVATE "-framework IOKit" "-framework CoreFoundation" "-framework Security" objc iconv)
elseif(WIN32)
  target_link_libraries(nocry-gui PRIVATE ws2_32 userenv ntdll bcrypt advapi32 synchronization dbghelp setupapi ole32)
else()
  target_link_libraries(nocry-gui PRIVATE usb-1.0 pthread dl m)
endif()
