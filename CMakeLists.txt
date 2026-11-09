cmake_minimum_required(VERSION 3.16)
project(minivectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(minivectordb
  src/main.cpp
  src/protocol.cpp
  src/engine.cpp
)

target_compile_options(minivectordb PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
)

# Static runtime: the evaluation image runs this binary directly, so it must not
# depend on a matching libstdc++ being present in the final stage.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(minivectordb PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS minivectordb RUNTIME DESTINATION bin)
