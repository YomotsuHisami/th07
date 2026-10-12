cmake_minimum_required(VERSION 3.20)
project(th07-core-tests LANGUAGES CXX)
include(CTest)
enable_testing()

get_filename_component(GAME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(COMMON "${GAME_ROOT}/third_party/eagler-common")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(core_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE
        "${GAME_ROOT}/src" "${COMMON}/include")
    # assert 包含测试判断；Release 构建也必须执行。
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME th07.${name} COMMAND ${name})
endfunction()

core_test(netplay-core-test
    "${COMMON}/src/netplay/NetplayCore.cpp"
    "${COMMON}/src/netplay/NetplayProtocol.cpp"
    "${COMMON}/src/netplay/NetplaySession.cpp")
core_test(rollback-journal-test "${COMMON}/src/netplay/RollbackJournal.cpp")
core_test(netplay-input-test "${COMMON}/src/netplay/NetplayInput.cpp")
target_compile_definitions(netplay-input-test PRIVATE TH_ENABLE_MULTIPLAYER_GAMEPLAY)
target_include_directories(netplay-input-test PRIVATE "${GAME_ROOT}/vendored/SDL/include")
core_test(netplay-side-effects-test "${GAME_ROOT}/src/netplay/NetplaySideEffects.cpp")
