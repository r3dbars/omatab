cmake_minimum_required(VERSION 3.20)

project(tilde-linux VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Fcitx5Core 5.1.0 REQUIRED)

add_fcitx5_addon(tilde src/tilde.cpp)
target_compile_features(tilde PRIVATE cxx_std_17)
target_compile_options(tilde PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tilde PRIVATE Fcitx5::Core)

configure_file(data/tilde.conf.in tilde.conf @ONLY)

install(TARGETS tilde DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/tilde.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
)

if(BUILD_TESTING)
    add_executable(tilde-policy-test tests/policy_test.cpp)
    target_compile_features(tilde-policy-test PRIVATE cxx_std_17)
    target_compile_options(
        tilde-policy-test PRIVATE -Wall -Wextra -Wpedantic -Werror
    )
    target_include_directories(
        tilde-policy-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME tilde-policy COMMAND tilde-policy-test)
endif()
