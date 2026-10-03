// hoardor_read <folder>: reads the metadata of every audio and video file under a folder,
// the way the metadata pass does, and reports the speed. For measuring on a real drive
// (cold: right after plugging it in, or after dropping the OS cache).

#include <hoardor/audio/audio.hpp>
#include <hoardor/file/settings.hpp>
#include <hoardor/video/video.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace hoardor;

namespace {

// Bytes this process has read so far (Linux only; 0 elsewhere).
long long bytes_read() {
    std::ifstream io("/proc/self/io");
    std::string key;
    long long value = 0;
    while (io >> key >> value) {
        if (key == "rchar:") return value;
    }
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: hoardor_read <folder>\n");
        return 2;
    }
    const auto settings = file::Settings::defaults();
    long long audio_files = 0, video_files = 0, failed = 0, bytes_on_disk = 0;
    const long long read_before = bytes_read();
    const auto start = std::chrono::steady_clock::now();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(argv[1], ec), end; it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        const auto kind = file::kind_of(it->path(), settings);
        if (kind == file::FileKind::Audio) {
            ++audio_files;
            if (!audio::read(it->path())) ++failed;
        } else if (kind == file::FileKind::Video) {
            ++video_files;
            if (!video::read(it->path())) ++failed;
        } else {
            continue;
        }
        bytes_on_disk += static_cast<long long>(it->file_size(ec));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const long long read = bytes_read() - read_before;
    const long long files = audio_files + video_files;
    std::printf("%lld files (%lld audio, %lld video, %lld unreadable) in %.2f s: %.0f files/s\n", files, audio_files, video_files,
                failed, seconds, files / (seconds > 0 ? seconds : 1));
    if (read > 0) {
        std::printf("read %.1f MB of %.1f MB on disk (%.1f KB per file)\n", read / 1e6, bytes_on_disk / 1e6,
                    files ? read / 1e3 / files : 0.0);
    }
    return 0;
}
