cmake_minimum_required(VERSION 3.22)
set(SC_MODULE cbor)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-cbor" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_CBOR_BUILD_TESTS "Build simply-cpp-cbor tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# 1.26.0: sc::base64::encode_url (JSON's bytes); utf8::is_valid (text).
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.26.0)
find_or_install_package(nlohmann_json nlohmann-json3-dev nlohmann_json)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(sc_cbor
        LINK_LIBRARIES sc::sc-core nlohmann_json::nlohmann_json
        PUBLIC_LINK_LIBRARIES sc::sc-core nlohmann_json::nlohmann_json)

add_sc_libraries(DESCRIPTION "simply-cpp CBOR (RFC 8949): decoding with limits and validity checks, preferred and deterministic encoding, diagnostic notation, JSON")
target_compile_features(sc-cbor PUBLIC cxx_std_20)
target_compile_features(sc-cbor-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-cbor
        COMPONENT development)

if (SC_CBOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-cbor-test)
endif ()

add_sc_example(sc-cbor-demo)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.26.0~)" nlohmann-json3-dev)
