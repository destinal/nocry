cmake_minimum_required(VERSION 3.21)
project(nocry-gui LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

# nocry-core through its C interface (host/ffi), built by cargo
set(NOCRY_HOST ${CMAKE_SOURCE_DIR}/../host)
set(NOCRY_FFI_LIB ${NOCRY_HOST}/target/release/${CMAKE_STATIC_LIBRARY_PREFIX}nocry_ffi${CMAKE_STATIC_LIBRARY_SUFFIX})
# always ask cargo (it is a no-op when nothing changed), so library edits reach the GUI
add_custom_target(nocry_ffi_build
  COMMAND cargo build --release -p nocry-ffi
  WORKING_DIRECTORY ${NOCRY_HOST}
  BYPRODUCTS ${NOCRY_FFI_LIB}
  COMMENT "cargo build nocry-ffi" VERBATIM)

qt_add_executable(nocry-gui main.cpp)
add_dependencies(nocry-gui nocry_ffi_build)
target_include_directories(nocry-gui PRIVATE ${NOCRY_HOST}/ffi/include)
target_link_libraries(nocry-gui PRIVATE Qt6::Widgets Qt6::Concurrent ${NOCRY_FFI_LIB})
if(UNIX AND NOT APPLE)
  target_link_libraries(nocry-gui PRIVATE usb-1.0 pthread dl m)
endif()
