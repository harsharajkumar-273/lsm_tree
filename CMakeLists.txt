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
    )
    target_include_directories(lsm_core PUBLIC src ${URING_INCLUDE_DIRS})
    target_link_libraries(lsm_core PUBLIC ${URING_LIBRARIES} Threads::Threads)

    # Enable CTest integration
    enable_testing()

    # Unit Tests targets
    add_executable(test_skip_list tests/test_skip_list.cpp)
    target_link_libraries(test_skip_list PRIVATE lsm_core)
    add_test(NAME test_skip_list COMMAND test_skip_list)

    # Structural invariants of the lock-free SkipList under contention (issue #19).
    add_executable(test_skip_list_invariants tests/test_skip_list_invariants.cpp)
    target_link_libraries(test_skip_list_invariants PRIVATE lsm_core)
    add_test(NAME test_skip_list_invariants COMMAND test_skip_list_invariants)

    add_executable(test_bloom_filter tests/test_bloom_filter.cpp)
    target_link_libraries(test_bloom_filter PRIVATE lsm_core)
    add_test(NAME test_bloom_filter COMMAND test_bloom_filter)

    add_executable(test_wal_recovery tests/test_wal_recovery.cpp)
    target_link_libraries(test_wal_recovery PRIVATE lsm_core)
    add_test(NAME test_wal_recovery COMMAND test_wal_recovery)

    add_executable(test_sstable tests/test_sstable.cpp)
    target_link_libraries(test_sstable PRIVATE lsm_core)

    add_executable(test_compaction_streaming tests/test_compaction_streaming.cpp)
    target_link_libraries(test_compaction_streaming PRIVATE lsm_core)

    add_executable(test_block_cache tests/test_block_cache.cpp)
    target_link_libraries(test_block_cache PRIVATE lsm_core)

    # Benchmark targets
    add_executable(bench_write benchmarks/bench_write.cpp)
    target_link_libraries(bench_write PRIVATE lsm_core)

    add_executable(bench_read benchmarks/bench_read.cpp)
    target_link_libraries(bench_read PRIVATE lsm_core)

    add_executable(bench_block_bottleneck benchmarks/bench_block_bottleneck.cpp)
    target_link_libraries(bench_block_bottleneck PRIVATE lsm_core)

    # Interactive CLI Tool
    add_executable(lsm_cli tools/lsm_cli.cpp)
    target_link_libraries(lsm_cli PRIVATE lsm_core)

else()
    message(FATAL_ERROR "This project requires Linux for native io_uring and O_DIRECT support.")
endif()
