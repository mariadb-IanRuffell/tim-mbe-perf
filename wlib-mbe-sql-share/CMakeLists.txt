#
# wlib-mbe-sql (MySQL/MariaDB backend only) and mbeperf, extracted from the wlib project.
#
#   mkdir build; cd build
#   cmake .. -DWLIB_LIBRARY=/path/to/libwlib.so -DWLIB_GRPC_LIBRARY=/path/to/libwlib-grpc.so
#   make
#
# Libraries needed
# ----------------
#   libwlib      the wlib main library (WLIB_LIBRARY): configuration, logging, I/O and file
#                system layer, and the INS API (FILE_OPEN_, READX, WRITEX ...) used by mbeperf.
#                Its own headers needed for this build are in include/.
#   libmariadb   MySQL/MariaDB client library (found with pkg-config, headers <mysql/mysql.h>
#                and <mysql/errmsg.h>)
#   libwlib-grpc the wlib gRPC library (WLIB_GRPC_LIBRARY)
#   glib-2.0     GLib (found with pkg-config)
#   hiredis      Redis client library (found with pkg-config)
#   rdkafka      Kafka client library (found with pkg-config)
#   libm         math (mbeperf)
#   libc
#
# If libwlib is a static library, its own dependencies must be linked as well: pass them in
# WLIB_EXTRA_LIBS (expat, prom, promhttp, microhttpd ... as in the
# build of libwlib). With a shared libwlib they are pulled in by libwlib itself.
#
#   cmake .. -DWLIB_LIBRARY=/path/libwlib.a -DWLIB_GRPC_LIBRARY=/path/libwlib-grpc.so -DWLIB_EXTRA_LIBS="expat;prom"
#
# The PostgreSQL backend (pq.c) is not part of this project, so PSQL_ENABLED must not be defined.
#
# The compiler flags have to match the ones libwlib was built with (for instance the
# _FILE_OFFSET_BITS and _TIME_BITS definitions), as the headers share types with it.
#

cmake_minimum_required(VERSION 3.13)
project(wlib-mbe-sql-share C)

set(LOG_DOMAIN "wlib-mbe-sql")

add_compile_options(-m32)
add_link_options(-m32)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED libmariadb)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(REDIS REQUIRED hiredis)
pkg_check_modules(RDKAFKA REQUIRED rdkafka)

set(WLIB_LIBRARY "" CACHE FILEPATH "Path of libwlib (static or shared)")
if(NOT WLIB_LIBRARY)
    find_library(WLIB_LIBRARY wlib)
endif()
if(NOT WLIB_LIBRARY)
    message(FATAL_ERROR "libwlib not found: use -DWLIB_LIBRARY=/path/to/libwlib.{so,a}")
endif()

set(WLIB_GRPC_LIBRARY "" CACHE FILEPATH "Path of libwlib-grpc")
if(NOT WLIB_GRPC_LIBRARY)
    find_library(WLIB_GRPC_LIBRARY wlib-grpc)
endif()
if(NOT WLIB_GRPC_LIBRARY)
    message(FATAL_ERROR "libwlib-grpc not found: use -DWLIB_GRPC_LIBRARY=/path/to/libwlib-grpc.so")
endif()

set(WLIB_EXTRA_LIBS "" CACHE STRING "Libraries (names, ';' separated) libwlib depends on when it is static")

option(BUILD_STATIC_LIB "Build wlib-mbe-sql as a static library" OFF)
if(BUILD_STATIC_LIB)
    set(LTYPE STATIC)
else()
    set(LTYPE SHARED)
endif()

#
# wlib-mbe-sql
#

add_library(wlib-mbe-sql ${LTYPE}
    src/mbe.c
    src/be.c
    src/cfg.c
    src/dt.c
    src/sql.c
    src/fdes.c
    src/cursor.c
    src/my.c)
target_compile_definitions(wlib-mbe-sql PRIVATE G_LOG_DOMAIN="${LOG_DOMAIN}" MYSQL_ENABLED)
target_compile_options(wlib-mbe-sql PRIVATE -Wall -fPIC)
target_include_directories(wlib-mbe-sql PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${MYSQL_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS} ${REDIS_INCLUDE_DIRS} ${RDKAFKA_INCLUDE_DIRS})
target_link_directories(wlib-mbe-sql PUBLIC ${MYSQL_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS} ${REDIS_LIBRARY_DIRS} ${RDKAFKA_LIBRARY_DIRS})
target_link_libraries(wlib-mbe-sql PRIVATE ${WLIB_LIBRARY} ${WLIB_GRPC_LIBRARY} ${MYSQL_LIBRARIES} ${GLIB_LIBRARIES} ${REDIS_LIBRARIES} ${RDKAFKA_LIBRARIES} ${WLIB_EXTRA_LIBS})

#
# mbeperf: performance tool for the MBE emulator
#

add_executable(mbeperf
    perf/mbeperf.c
    perf/patterns.c
    perf/stats.c
    perf/pace.c
    perf/setup.c)
target_compile_definitions(mbeperf PRIVATE MBEPERF_SQL MYSQL_ENABLED)
target_include_directories(mbeperf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${GLIB_INCLUDE_DIRS} ${REDIS_INCLUDE_DIRS} ${RDKAFKA_INCLUDE_DIRS})
target_link_directories(mbeperf PRIVATE ${GLIB_LIBRARY_DIRS} ${REDIS_LIBRARY_DIRS} ${RDKAFKA_LIBRARY_DIRS})
target_link_libraries(mbeperf PRIVATE wlib-mbe-sql ${WLIB_LIBRARY} ${WLIB_GRPC_LIBRARY} ${GLIB_LIBRARIES} ${REDIS_LIBRARIES} ${RDKAFKA_LIBRARIES} m ${WLIB_EXTRA_LIBS})

# Pull in the wlib constructor
if(APPLE)
    target_link_options(mbeperf PRIVATE -u _wlib_ctor)
else()
    target_link_options(mbeperf PRIVATE -u wlib_ctor)
endif()

# The fixture goes next to the executable, which is where it looks for it by default
configure_file(perf/mbeperf-my.mbe.ini ${CMAKE_CURRENT_BINARY_DIR}/mbeperf-my.mbe.ini COPYONLY)
