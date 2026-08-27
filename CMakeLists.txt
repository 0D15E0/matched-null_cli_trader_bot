cmake_minimum_required(VERSION 3.16)
project(cli_trader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(cli_trader ${CLI_SOURCES})
target_include_directories(cli_trader PRIVATE src)
target_link_libraries(cli_trader PRIVATE CURL::libcurl Threads::Threads nlohmann_json::nlohmann_json OpenSSL::Crypto)

add_subdirectory(experiments/tools)

enable_testing()
