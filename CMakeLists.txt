cmake_minimum_required(VERSION 3.16)
project(lsm_tree CXX)

if(NOT APPLE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    endif()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")

    # Find system libraries
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED liburing)
    find_package(Threads REQUIRED)

    # Core library target containing all modular database components
    add_library(lsm_core STATIC
        src/wal/wal_writer.cpp
        src/wal/wal_reader.cpp
        src/sstable/sstable_builder.cpp
        src/sstable/sstable_reader.cpp
        src/compaction/leveled_compactor.cpp
        src/db/lsm_engine.cpp
        src/util/fault.cpp
    )
    target_include_directories(lsm_core PUBLIC src ${URING_INCLUDE_DIRS})
    target_link_libraries(lsm_core PUBLIC ${URING_LIBRARIES} Threads::Threads)

    # Enable CTest integration
    enable_testing()

    # Tests are compiled with assertions forced on. CMAKE_CXX_FLAGS_RELEASE is
    # overridden above, but a user (or a packager) passing -DNDEBUG must not be
    # able to turn <cassert> checks into no-ops. test_util.h's CHECK is
    # unconditional anyway; this covers the older tests that use assert().
    function(lsm_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE lsm_core)
        target_compile_options(${name} PRIVATE -UNDEBUG)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    # Unit Tests targets
    lsm_add_test(test_skip_list)

    # Structural invariants of the lock-free SkipList under contention (issue #19).
    lsm_add_test(test_skip_list_invariants)

    lsm_add_test(test_bloom_filter)

    lsm_add_test(test_wal_recovery)

    # WAL submission-order regression tests (issue #14).
    lsm_add_test(test_wal_ordering)

    lsm_add_test(test_sstable)

    lsm_add_test(test_compaction_streaming)

    lsm_add_test(test_block_cache)

    # Randomized model checking, crash/fault injection and the assertion canary.
    lsm_add_test(test_randomized_model)
    lsm_add_test(test_assertions_active)
    lsm_add_test(test_durability_faults)
    lsm_add_test(test_crash_recovery)
    lsm_add_test(test_concurrent_durable)
    lsm_add_test(test_wal_cancellation)
    set_tests_properties(test_crash_recovery PROPERTIES TIMEOUT 300)

    # Benchmark targets
    add_executable(bench_write benchmarks/bench_write.cpp)
    target_link_libraries(bench_write PRIVATE lsm_core)

    add_executable(bench_read benchmarks/bench_read.cpp)
    target_link_libraries(bench_read PRIVATE lsm_core)

    add_executable(bench_durability benchmarks/bench_durability.cpp)
    target_link_libraries(bench_durability PRIVATE lsm_core)

    add_executable(bench_block_bottleneck benchmarks/bench_block_bottleneck.cpp)
    target_link_libraries(bench_block_bottleneck PRIVATE lsm_core)

    # Interactive CLI Tool
    add_executable(lsm_cli tools/lsm_cli.cpp)
    target_link_libraries(lsm_cli PRIVATE lsm_core)

else()
    message(FATAL_ERROR "This project requires Linux for native io_uring and O_DIRECT support.")
endif()
