# CMake generated Testfile for 
# Source directory: /home/z/my-project/FastAlloc
# Build directory: /home/z/my-project/FastAlloc/build2
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(fast_alloc_validate_tests "/home/z/my-project/FastAlloc/build2/fast_alloc_tests")
set_tests_properties(fast_alloc_validate_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/z/my-project/FastAlloc/CMakeLists.txt;166;add_test;/home/z/my-project/FastAlloc/CMakeLists.txt;0;")
subdirs("_deps/googletest-build")
subdirs("tests/audit")
