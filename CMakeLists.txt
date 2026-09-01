cmake_minimum_required(VERSION 3.20)

project(omatab VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Fcitx5Core 5.1.0 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_library(
    omatab-model STATIC
    src/ocr_context.cpp
    src/ollama_client.cpp
    src/telemetry.cpp
)
set_target_properties(omatab-model PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(omatab-model PUBLIC cxx_std_17)
target_compile_options(omatab-model PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
    omatab-model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    omatab-model PUBLIC PkgConfig::LIBCURL PkgConfig::JSONCPP
)

add_fcitx5_addon(omatab src/omatab.cpp)
target_compile_features(omatab PRIVATE cxx_std_17)
target_compile_options(omatab PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(omatab PRIVATE Fcitx5::Core Threads::Threads omatab-model)

configure_file(data/omatab.conf.in omatab.conf @ONLY)

install(TARGETS omatab DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/omatab.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
)
install(
    PROGRAMS scripts/telemetry-report.sh
    DESTINATION bin
    RENAME omatab-telemetry-report
)
install(
    PROGRAMS scripts/omatab.sh
    DESTINATION bin
    RENAME omatab
)
install(
    PROGRAMS scripts/warm-model.sh
    DESTINATION libexec
    RENAME omatab-warm-model
)
install(
    FILES
        packaging/systemd/omatab-model-warm.service
        packaging/systemd/omatab-model-warm.timer
    DESTINATION share/systemd/user
)

if(BUILD_TESTING)
    add_executable(omatab-policy-test tests/policy_test.cpp)
    target_compile_features(omatab-policy-test PRIVATE cxx_std_17)
    target_compile_options(
        omatab-policy-test PRIVATE -Wall -Wextra -Wpedantic -Werror
    )
    target_include_directories(
        omatab-policy-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME omatab-policy COMMAND omatab-policy-test)

    add_executable(omatab-model-test tests/model_test.cpp)
    target_compile_features(omatab-model-test PRIVATE cxx_std_17)
    target_compile_options(
        omatab-model-test PRIVATE -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(omatab-model-test PRIVATE omatab-model)
    add_test(NAME omatab-model COMMAND omatab-model-test)
endif()
