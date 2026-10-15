# Host unit tests (no IDF):
#   cmake -S test/host -B build_host_test && cmake --build build_host_test && ctest --test-dir build_host_test
cmake_minimum_required(VERSION 3.16)
project(winject_radio_esp32c6_host_test C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)
enable_testing()
set(COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../../components)
add_executable(test_sdio_msg test_sdio_msg.c ${COMPONENTS}/sdio_msg/sdio_msg.c)
target_include_directories(test_sdio_msg PRIVATE ${COMPONENTS}/sdio_msg/include)
add_test(NAME sdio_msg COMMAND test_sdio_msg)
