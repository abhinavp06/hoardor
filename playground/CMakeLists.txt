# Manual experiments against real drives. Anything goes here.

add_executable(hoardor_scan file/scan_playground.cpp)
target_link_libraries(hoardor_scan PRIVATE hoardor)
