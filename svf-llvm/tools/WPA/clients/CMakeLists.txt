target_sources(wpa PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Extract.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Snapshot.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Analyze.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Query.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Driver.cpp)

if(SVF_CLIENT_CUDA)
  # LLVM's C++ warning, RTTI and exception flags do not apply to nvcc.
  add_library(SvfClientCuda STATIC Cuda.cu)
  set_target_properties(SvfClientCuda PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
    COMPILE_OPTIONS "" COMPILE_DEFINITIONS "")
  target_compile_definitions(wpa PRIVATE SVF_CLIENT_CUDA)
  target_link_libraries(wpa PRIVATE SvfClientCuda)
  add_executable(client-gpu-tiles EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/tests/alias-clients/gpu-tiles.cpp)
  target_include_directories(client-gpu-tiles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(client-gpu-tiles PRIVATE SvfClientCuda)
  set_target_properties(client-gpu-tiles PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(SVF_CLIENT_BASELINES)
  add_subdirectory(pestrie)
  target_compile_definitions(wpa PRIVATE SVF_CLIENT_BASELINES)
  target_link_libraries(wpa PRIVATE SvfClientBaselines)
  add_dependencies(wpa svf-bit-index svf-pes-index)
endif()
