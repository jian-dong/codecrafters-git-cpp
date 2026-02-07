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

add_library(gitcpp_core
  src/git_cli_application.cpp
  src/git_fs.cpp
  src/git_object_store.cpp
  src/git_object_format.cpp
  src/git_pack.cpp
  src/git_remote_client.cpp
  src/git_repository.cpp
  src/utils.cpp
)

target_include_directories(gitcpp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/expected/include>
  $<INSTALL_INTERFACE:include>
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(gitcpp_core PUBLIC
  OpenSSL::Crypto
  -lz
  -lpthread
  -lcurl
)

add_executable(${PROJECT_NAME}
  src/main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE gitcpp_core)

include(CTest)
if (BUILD_TESTING)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  include(GoogleTest)

  add_executable(git_object_format_test
    tests/git_object_format_test.cpp
  )
  target_link_libraries(git_object_format_test PRIVATE gitcpp_core GTest::gtest_main)
  gtest_discover_tests(git_object_format_test)

  add_executable(git_pack_test
    tests/git_pack_test.cpp
  )
  target_link_libraries(git_pack_test PRIVATE gitcpp_core GTest::gtest_main)
  gtest_discover_tests(git_pack_test)
endif ()
