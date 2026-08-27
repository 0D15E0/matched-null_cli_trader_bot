# C++ helpers for the order-gate experiment (see ../README.md).
#
# THESE MUST NOT LIVE UNDER src/. The root CMakeLists builds cli_trader from
#   file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)
# so any .cpp under src/ is compiled INTO cli_trader. Each tool here has its
# own main(), so putting one there breaks the main binary's link with
# duplicate symbols - and because the glob is CONFIGURE_DEPENDS it happens the
# moment the file appears, with no CMake edit to blame it on.
#
# Each tool links only the src/ translation units it actually uses, and none
# of them link libcurl, OpenSSL or nlohmann_json: nothing here fetches or
# parses anything, it all reads local .ctc files. Keeping the dependency edges
# honest means these keep building even if the fetch stack's dependencies move,
# and it keeps the tools' build time near zero.

# storeinfo / resample / keeplast: pure store manipulation.
foreach(tool storeinfo resample keeplast)
  add_executable(${tool} ${tool}.cpp ${PROJECT_SOURCE_DIR}/src/core/candle_store.cpp)
  target_include_directories(${tool} PRIVATE ${PROJECT_SOURCE_DIR}/src)
endforeach()

# spiral_modes additionally runs the estimator: the spiral contour + truncated
# Laplace transform (math/spiral.cpp) on top of the matrix-pencil line-spectrum
# solver it calls (math/pencil.cpp). Both are dependency-free dense numerics.
add_executable(spiral_modes
  spiral_modes.cpp
  ${PROJECT_SOURCE_DIR}/src/core/candle_store.cpp
  ${PROJECT_SOURCE_DIR}/src/math/spiral.cpp
  ${PROJECT_SOURCE_DIR}/src/math/pencil.cpp
)
target_include_directories(spiral_modes PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Dumps the shipped causal volatility forecast alongside forward realized
# volatility, for the implied-vs-forecast encompassing regression.
add_executable(vol_forecast_dump vol_forecast_dump.cpp
    ${PROJECT_SOURCE_DIR}/src/core/candle_store.cpp
    ${PROJECT_SOURCE_DIR}/src/indicators/vol_forecast.cpp
    ${PROJECT_SOURCE_DIR}/src/indicators/indicators.cpp
    ${PROJECT_SOURCE_DIR}/src/math/pencil.cpp)
target_include_directories(vol_forecast_dump PRIVATE ${PROJECT_SOURCE_DIR}/src)

# causality_check: every registry strategy must give the same signal for a bar
# whether or not the bars after it exist. Links the strategy zoo, which is
# header-only apart from the indicator and math translation units it calls.
add_executable(causality_check causality_check.cpp
    ${PROJECT_SOURCE_DIR}/src/core/candle_store.cpp
    ${PROJECT_SOURCE_DIR}/src/indicators/indicators.cpp
    ${PROJECT_SOURCE_DIR}/src/math/pencil.cpp)
target_include_directories(causality_check PRIVATE ${PROJECT_SOURCE_DIR}/src)
