add_executable(HojichaCacheTest
  HojichaCacheTest.cpp
  ${REPO_ROOT}/lib/HojichaCacheRefresh/HojichaCacheRefresh.cpp
)
target_include_directories(HojichaCacheTest PRIVATE
  stubs
  ${REPO_ROOT}/lib/HojichaCacheRefresh
  ${REPO_ROOT}/lib/Memory
)
target_compile_options(HojichaCacheTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME HojichaCacheTest COMMAND HojichaCacheTest)
