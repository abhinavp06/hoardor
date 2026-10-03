#pragma once

// The player engine (features/player.md): plays files through libmpv, keeps the queue of
// entry ids, and stores what the user did with each file (position, viewed, play count,
// liked). No Qt: the app draws the video frames through VideoRenderer.

#include <hoardor/db/database.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hoardor::player {

using EntryId = std::int64_t;

struct Error {
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

// Every tunable of the engine (features/player.md §7), persisted in player_settings.
struct Settings {
    int read_ahead_mib = 64;            // mpv's demuxer-max-bytes
    int read_ahead_seconds = 20;        // mpv's demuxer-readahead-secs
    bool hardware_decoding = true;      // hwdec=auto-safe, or no
    std::string audio_languages;        // preferred, e.g. "jpn,eng" (mpv alang); "" = the file's default
    std::string subtitle_languages;     // (mpv slang)
    bool subtitles_on = false;          // show subtitles by default
    int previous_restarts_after_seconds = 3;
    int progress_save_seconds = 10;
    int resume_min_seconds = 60;
    int viewed_percent = 90;
    int play_count_percent = 50;
    int play_count_seconds = 240;
    bool resume_audio = false;
    int volume = 100;                   // 0–100, remembered
    bool muted = false;

    static Settings defaults() { return {}; }
};

// Limits a setting must stay within (inclusive); save_settings rejects anything outside.
struct SettingLimits {
    int min, max;
};

// What the user did with one file (player_items). A file never played or liked is all zeros.
struct ItemState {
    EntryId entry = 0;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    bool viewed = false;
    int play_count = 0;
    std::int64_t last_played_ns = 0;  // Unix ns, 0 = never
    std::int64_t liked_ns = 0;        // when it was liked, 0 = not liked

    bool liked() const { return liked_ns > 0; }
};

// The player's tables on one connection (one thread at a time, like every repository).
// The Player writes through its own; the app reads states and sets likes through another.
class Library {
public:
    static Result<Library> open(db::Database& database);

    Result<ItemState> state(EntryId entry);
    // The states of a page of entries, in the same order; entries without a row are zeros.
    Result<std::vector<ItemState>> states(std::span<const EntryId> entries);

    Result<void> set_liked(EntryId entry, bool liked, std::int64_t now_ns);
    // Position and length; `viewed` only ever turns on here (a re-watch keeps the check).
    Result<void> save_position(EntryId entry, std::int64_t position_ms, std::int64_t duration_ms, bool viewed,
                               std::int64_t now_ns);
    Result<void> count_play(EntryId entry, std::int64_t now_ns);

    // Settings::defaults() overlaid with every stored value that parses and is in range.
    Result<Settings> load_settings();
    Result<void> save_settings(const Settings& settings);

private:
    explicit Library(db::Database& database) : db_(&database) {}
    db::Database* db_;
};

// ---------------------------------------------------------------- Playback

enum class State : std::uint8_t { Idle, Loading, Playing, Paused };

// One audio or subtitle track of the loaded file.
struct Track {
    int id = 0;                // mpv's track id
    std::string language;      // as tagged ("eng"), or ""
    std::string title;         // "Commentary", "SDH", …
    std::string codec;         // "truehd", "subrip", …
    int channels = 0;          // audio only
    bool external = false;     // a .srt/.ass next to the file
    bool selected = false;
};

struct Status {
    State state = State::Idle;
    std::optional<EntryId> entry;       // what's loaded, or nothing
    std::size_t index = 0;              // its position in the queue
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    std::int64_t resumed_from_ms = 0;   // > 0: playback resumed there (the UI offers "from the start")
    int volume = 100;
    bool muted = false;
    bool has_video = false;             // a real picture, not a music file's cover
    std::vector<Track> audio, subtitles;
};

// Turns an entry into a path to open. It may touch the drive (a sleeping HDD takes seconds),
// and only ever runs on the player's own thread, one entry at a time.
using Resolver = std::function<Result<std::filesystem::path>(EntryId)>;

// All of them run on the player's thread: return quickly, and don't call back into Player
// from inside one except through its (non-blocking) commands.
struct Callbacks {
    std::function<void(const Status&)> status;               // state, file, tracks, volume changed
    std::function<void(std::int64_t position_ms)> position;   // at most 4 times a second while playing
    std::function<void()> queue;                             // the queue or the current item changed
    std::function<void(EntryId, const Error&)> error;        // a file couldn't play and was skipped
};

// Where the sound and the picture go. Tests use "null" for both.
struct Outputs {
    std::string audio = "auto";
    std::string video = "libmpv";   // frames for VideoRenderer; "null" = decode nothing
    // A diagnosis aid: mpv writes its own detailed log here (the decoder, hardware decoding,
    // scaler, dithering, and output it chose). Empty: no log.
    std::filesystem::path log_file;
};

// mpv's OpenGL render API (features/player.md §4.5). Every call happens on the app's render
// thread with its OpenGL context current, and destroy() must come before the Player goes.
class VideoRenderer {
public:
    using GetProcAddress = void* (*)(void* context, const char* name);

    // false if mpv couldn't set it up (no OpenGL, or video output isn't "libmpv").
    bool create(GetProcAddress get_proc_address, void* context);
    bool ready() const { return context_ != nullptr; }
    // Draws the current frame into the OpenGL framebuffer `fbo` (0 = the default one).
    void render(int fbo, int width, int height, bool flip_y);
    void destroy();
    // Called (from any thread) when a new frame is ready to be drawn. Set it before create().
    void set_update_callback(std::function<void()> callback) { on_update_ = std::move(callback); }

private:
    friend class Player;
    explicit VideoRenderer(void* mpv) : mpv_(mpv) {}
    static void on_update(void* self);
    void* mpv_;                   // mpv_handle*
    void* context_ = nullptr;     // mpv_render_context*
    std::function<void()> on_update_;
};

// One playback session on a thread of its own (features/player.md §4). Every command returns
// at once: it's carried out on the player's thread, in order.
class Player {
public:
    // Opens its own connection to `database_file` (player_items, player_settings) and starts
    // libmpv. libmpv needs numbers in the "C" locale, so this sets LC_NUMERIC for the process.
    static Result<std::unique_ptr<Player>> start(const std::filesystem::path& database_file, Resolver resolver,
                                                 Callbacks callbacks, Outputs outputs = {});
    // Saves the position, stops, and joins the thread. The VideoRenderer must be destroyed first.
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // ---- The queue (entry ids, never "an album")
    void play_now(std::vector<EntryId> entries, std::size_t start = 0);  // clear and play from `start`
    void add(std::vector<EntryId> entries);                              // append; plays if nothing is loaded
    void jump(std::size_t index);
    void remove(std::size_t index);
    void clear();                                                         // stop and empty
    std::vector<EntryId> queue() const;
    std::size_t current() const;

    // ---- Transport
    void toggle();
    void pause();
    void resume();
    void stop();   // unload; the queue stays
    void next();
    void previous();   // restarts the item when it's past Settings::previous_restarts_after_seconds
    void seek(std::int64_t position_ms);
    void seek_by(std::int64_t delta_ms);
    void set_volume(int volume);
    void set_muted(bool muted);
    void select_audio(int track_id);
    void select_subtitle(std::optional<int> track_id);   // nullopt: off

    Status status() const;
    VideoRenderer& video();

    struct Impl;   // the engine's own types (src/player/player.cpp)

private:
    explicit Player(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
