cmake_minimum_required(VERSION 3.13)

project(git)

#-------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)

# Set the default build type to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif ()

# Common compiler options for GCC and Clang
if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Werror -Wno-pedantic -Wno-array-bounds)
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS} -g -O0")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -g -O0")
else ()
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -O3")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O3")
endif ()


find_package(OpenSSL REQUIRED)
add_executable(${PROJECT_NAME}
  src/main.cpp
  src/git_server.cpp
  src/utils.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OpenSSL::Crypto
  -lz
  -lpthread
  -lcurl
)
