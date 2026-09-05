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

add_executable(portfolio_benchmark_test tests/portfolio_benchmark_test.cpp)
target_include_directories(portfolio_benchmark_test PRIVATE src)
add_test(NAME portfolio_benchmark COMMAND portfolio_benchmark_test)

# factor_trend reads a second instrument through strategy/zoo/market_context.h;
# its test needs the store reader and the indicator translation unit.
add_executable(factor_trend_test tests/factor_trend_test.cpp
    src/core/candle_store.cpp src/indicators/indicators.cpp)
target_include_directories(factor_trend_test PRIVATE src)
add_test(NAME factor_trend COMMAND factor_trend_test)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME research_protocol
           COMMAND ${Python3_EXECUTABLE} -m unittest tests/research_protocol_test.py)
  set_tests_properties(research_protocol PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
