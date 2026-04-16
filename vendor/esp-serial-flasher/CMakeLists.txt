cmake_minimum_required(VERSION 3.22)
project(esp-serial-flasher-build C)

add_subdirectory(src)

install(TARGETS flasher ARCHIVE DESTINATION lib)
install(DIRECTORY src/include/ DESTINATION include FILES_MATCHING PATTERN "*.h")
