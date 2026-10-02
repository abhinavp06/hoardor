# Manual experiments against real drives. Anything goes here.

add_executable(hoardor_scan file/scan_playground.cpp)
target_link_libraries(hoardor_scan PRIVATE hoardor)

add_executable(hoardor_sync file/sync_playground.cpp)
target_link_libraries(hoardor_sync PRIVATE hoardor)

# hoardor_read <folder>: metadata reading speed (files/s, bytes read) on a real drive.
add_executable(hoardor_read media/read_playground.cpp)
target_link_libraries(hoardor_read PRIVATE hoardor)
