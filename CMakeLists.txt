cmake_minimum_required(VERSION 3.13)

project(git-starter-cpp)

set(CMAKE_CXX_STANDARD 23) # Enable the C++23 standard

file(GLOB_RECURSE SOURCE_FILES src/*.cpp src/*.hpp)

add_executable(git ${SOURCE_FILES})

# 查找 OpenSSL
find_package(OpenSSL REQUIRED)

# 采用现代导入目标，通常会自动包含 OpenSSL 的头文件路径
target_link_libraries(git PRIVATE OpenSSL::Crypto -lz)

# 如果发现头文件没有自动包含，也可以手动添加
target_include_directories(git PRIVATE ${OPENSSL_INCLUDE_DIR})
