cmake_minimum_required(VERSION 3.5.1)

project(border-router)

option(PERF_TEST "Performance test" ON)
option(NO_RATE_RECORDING "No rate recording" OFF)
option(PROFILER_ENABLE "Profiler enable" OFF)
option(NO_PID_CALCULATION "No PID calculation" OFF)
option(NO_GET_CKSUM "Don't compute checksum for GET packets" OFF)
option(NO_HMAC_COMPUTATION "Don't re-compute HMAC for DATA packets" OFF)
option(NO_HMAC_VERIFICATION "Don't verify HMAC for DATA packets" OFF)
option(NIDT_TEST "Pipeline0 NID table test" OFF)
option(ODCT_TEST "Pipeline0 ODC table test" OFF)
option(PREFETCH_NONE "Prefetch zero cache line for each packet" OFF)
option(PREFETCH_ONE "Prefetch one cache line for each packet" OFF)
option(PREFETCH_TWO "Prefetch two cache lines for each packet" OFF)
option(PREFETCH_THREE "Prefetch three cache lines for each packet" ON)

option(CROSS_COMPILE "Compile br for ARM" OFF)
if (CROSS_COMPILE)
    set(tools /home/zeusnet/dpdk-arm/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu)
    set(ENV{PKG_CONFIG_PATH} "${tools}/aarch64-linux-gnu/lib/pkgconfig")
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR ARM)
    set(CMAKE_C_COMPILER ${tools}/bin/aarch64-linux-gnu-gcc)
    set(CMAKE_CXX_COMPILER ${tools}/bin/aarch64-linux-gnu-g++)
    set(CMAKE_FIND_ROOT_PATH "${tools}/aarch64-linux-gnu")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(PERF_TEST OFF)
endif (CROSS_COMPILE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk=21.08.0)
pkg_check_modules(SQLITE3 REQUIRED sqlite3>=3.38.0)
pkg_check_modules(OPENSSL REQUIRED openssl=1.1.1m)

set(CMAKE_C_FLAGS_RELEASE -O2)
set(CMAKE_BUILD_TYPE Release) # override command line 
message("-- Optimization level: ${CMAKE_C_FLAGS_RELEASE}")

set(TARGET-SHARED br)
set(TARGET-STATIC br-static)
set(TARGET-BR-PIPELINE0 br-pipeline0)
set(TABLE_SRC tables/arp_table.c tables/ip_lpm_table.c tables/nid_table.c 
    tables/odc_table.c tables/peer_table.c tables/port_table.c tables/px_table.c 
    tables/stats_table.c)
set(CLI_SRC commandline/cli.c commandline/cmd_parser.c)
set(OTHER_SRC load_conf.c br_tables.c br_conf.c utils/utils.c 
    profiler/profiler.c arp/arp.c packet/rte_CoLoR.c init.c)
set(BR_RTC_SRC main.c br-rtc.c)
set(BR_PIPELINE0_SRC br-pipeline0-main.c br-pipeline0.c)

add_compile_options(${DPDK_CFLAGS})
add_compile_options(${SQLITE3_CFLAGS})
add_compile_options(${OPENSSL_CFLAGS})

if (PERF_TEST)
    add_definitions(-DPERFORMANCE_TEST)
endif(PERF_TEST)

if (NO_RATE_RECORDING)
    add_definitions(-DNO_RATE_RECORDING)
endif(NO_RATE_RECORDING)

if (PROFILER_ENABLE)
    add_definitions(-DPROFILER_ENABLE)
endif(PROFILER_ENABLE)

if (NO_PID_CALCULATION)
    add_definitions(-DNO_PID_CALCULATION)
endif(NO_PID_CALCULATION)

if (NO_GET_CKSUM)
    add_definitions(-DNO_GET_CKSUM)
endif (NO_GET_CKSUM)

if (NO_HMAC_COMPUTATION)
    add_definitions(-DNO_HMAC_COMPUTATION)
endif (NO_HMAC_COMPUTATION)

if (NO_HMAC_VERIFICATION)
    add_definitions(-DNO_HMAC_VERIFICATION)
endif (NO_HMAC_VERIFICATION)

if (NIDT_TEST)
    add_definitions(-DNIDT_TEST)
endif (NIDT_TEST)

if (ODCT_TEST)
    add_definitions(-DODCT_TEST)
endif (ODCT_TEST)

if (PREFETCH_NONE)
    add_definitions(-DPREFETCH_NONE)
endif (PREFETCH_NONE)

if (PREFETCH_ONE)
    add_definitions(-DPREFETCH_ONE)
endif (PREFETCH_ONE)

if (PREFETCH_TWO)
    add_definitions(-DPREFETCH_TWO)
endif (PREFETCH_TWO)

if (PREFETCH_THREE)
    add_definitions(-DPREFETCH_THREE)
endif (PREFETCH_THREE)

message("-- Performance test is ${PERF_TEST}")
message("-- No rate recording is ${NO_RATE_RECORDING}")
message("-- Profiler enable is ${PROFILER_ENABLE}")
message("-- No PID calculation is ${NO_PID_CALCULATION}")
message("-- Don't compute checksum for GET packets is ${NO_GET_CKSUM}")
message("-- Don't re-compute HMAC for DATA packets is ${NO_HMAC_COMPUTATION}")
message("-- Don't verify HMAC for DATA packets is ${NO_HMAC_VERIFICATION}")
message("-- NID table test is ${NIDT_TEST}")
message("-- ODC table test is ${ODCT_TEST}")
if (PREFETCH_NONE)
    message("-- Prefetch zero cache line for each packet")
elseif (PREFETCH_ONE)
    message("-- Prefetch one cache line for each packet")
elseif (PREFETCH_TWO)
    message("-- Prefetch two cache lines for each packet")
else ()
    message("-- Prefetch three cache lines for each packet")
endif ()

# add_definitions(-DDEBUG_BR_VERBOSE)

add_definitions(-DALLOW_EXPERIMENTAL_API)
add_library(br-obj OBJECT ${OTHER_SRC} ${TABLE_SRC} ${CLI_SRC})

add_executable(${TARGET-SHARED} $<TARGET_OBJECTS:br-obj> ${BR_RTC_SRC}) 
target_link_libraries(${TARGET-SHARED} ${DPDK_LDFLAGS})
target_link_libraries(${TARGET-SHARED} ${SQLITE3_LIBRARIES})
target_link_libraries(${TARGET-SHARED} ${OPENSSL_LIBRARIES})
target_link_libraries(${TARGET-SHARED} m)

add_executable(${TARGET-STATIC} $<TARGET_OBJECTS:br-obj> ${BR_RTC_SRC}) 
target_link_libraries(${TARGET-STATIC} ${DPDK_STATIC_LDFLAGS})
target_link_libraries(${TARGET-STATIC} -l:libsqlite3.a)
target_link_libraries(${TARGET-STATIC} -l:libcrypto.a)
target_link_libraries(${TARGET-STATIC} m)

# pipeline version
add_executable(${TARGET-BR-PIPELINE0} $<TARGET_OBJECTS:br-obj> ${BR_PIPELINE0_SRC}) 
target_link_libraries(${TARGET-BR-PIPELINE0} ${DPDK_LDFLAGS})
target_link_libraries(${TARGET-BR-PIPELINE0} ${SQLITE3_LIBRARIES})
target_link_libraries(${TARGET-BR-PIPELINE0} ${OPENSSL_LIBRARIES})
target_link_libraries(${TARGET-BR-PIPELINE0} m)


# available macros
# NO_GET_CKSUM : Do not calculate and verify checksum of GET packet
# NO_DATA_CKSUM : Do not calculate and verify checksum of DATA packet
# NO_ANN_CKSUM : Do not calculate and verify checksum of ANN packet
# PERFORMANCE_TEST : Do not generate random MAC address
# PROFILER_ENABLE : compile with profiler enabled
# DEBUG_BR_VERBOSE : output more information about every received packet
# NO_RATE_RECORDING : Do not record # of rx pkts, # of tx pkts, forwarding rate
# NO_PID_CALCULATION : Do not verify PID, or calculate PID
