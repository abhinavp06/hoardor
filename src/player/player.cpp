// player::Player and player::VideoRenderer on libmpv (features/player.md §4).

#include <hoardor/player/player.hpp>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <clocale>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace hoardor::player {

namespace fs = std::filesystem;

namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Properties the player observes; the number is mpv's reply_userdata.
enum Property : std::uint64_t { TimePos = 1, Duration, Pause, Volume, Mute, TrackList };

const mpv_node* member(const mpv_node& map, const char* key) {
    if (map.format != MPV_FORMAT_NODE_MAP) return nullptr;
    for (int i = 0; i < map.u.list->num; ++i) {
        if (std::string_view(map.u.list->keys[i]) == key) return &map.u.list->values[i];
    }
    return nullptr;
}
std::string text_of(const mpv_node* n) { return n && n->format == MPV_FORMAT_STRING ? n->u.string : ""; }
std::int64_t int_of(const mpv_node* n) { return n && n->format == MPV_FORMAT_INT64 ? n->u.int64 : 0; }
bool flag_of(const mpv_node* n) { return n && n->format == MPV_FORMAT_FLAG && n->u.flag; }

std::string utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::string seconds_text(std::int64_t ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", static_cast<double>(ms) / 1000.0);  // LC_NUMERIC is "C"
    return buffer;
}

}  // namespace

// ---------------------------------------------------------------- VideoRenderer

bool VideoRenderer::create(GetProcAddress get_proc_address, void* context) {
    if (context_) return true;
    mpv_opengl_init_params gl{get_proc_address, context};
    // Advanced control stays off: it hung in the check of 2026-10-03 (features/player.md §4.5).
    int advanced = 0;
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* created = nullptr;
    if (mpv_render_context_create(&created, static_cast<mpv_handle*>(mpv_), params) < 0) return false;
    context_ = created;
    mpv_render_context_set_update_callback(created, &VideoRenderer::on_update, this);
    return true;
}

void VideoRenderer::on_update(void* self) {
    auto* renderer = static_cast<VideoRenderer*>(self);
    if (renderer->on_update_) renderer->on_update_();
}

void VideoRenderer::render(int fbo, int width, int height, bool flip_y) {
    if (!context_) return;
    mpv_opengl_fbo target{fbo, width, height, 0};
    int flip = flip_y ? 1 : 0;
    mpv_render_param params[]{{MPV_RENDER_PARAM_OPENGL_FBO, &target},
                              {MPV_RENDER_PARAM_FLIP_Y, &flip},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(static_cast<mpv_render_context*>(context_), params);
}

void VideoRenderer::destroy() {
    if (!context_) return;
    mpv_render_context_free(static_cast<mpv_render_context*>(context_));
    context_ = nullptr;
}

// ---------------------------------------------------------------- Player

struct Player::Impl {
    // ---- Fixed after start()
    mpv_handle* mpv = nullptr;
    std::optional<db::Database> db;   // used on the player thread only
    std::optional<Library> library;
    Resolver resolve;
    Callbacks callbacks;
    Settings settings;
    std::optional<VideoRenderer> video;
    std::thread thread;

    // ---- Commands from any thread, run on the player thread in order
    std::mutex tasks_mutex;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;

    // ---- Read from any thread (under state_mutex), written on the player thread
    mutable std::mutex state_mutex;
    std::vector<EntryId> queue;
    std::size_t current = 0;
    Status status;

    // ---- The player thread only
    bool loaded = false;                    // a file is loaded or loading
    std::optional<std::size_t> appended;    // the queue index mpv holds as its next file
    ItemState item;                         // the stored state of the loaded entry
    bool counted = false;                   // its play was counted
    bool finished = false;                  // it reached its end
    std::int64_t position_ms = 0, duration_ms = 0;
    std::int64_t last_save_ms = 0, last_position_ms = 0, last_flush_ms = 0, last_poll_ms = 0;
    bool settings_dirty = false;

    void post(std::function<void()> task) {
        {
            std::lock_guard lock(tasks_mutex);
            tasks.push_back(std::move(task));
        }
        mpv_wakeup(mpv);
    }

    // mpv's commands are synchronous here: they only queue work inside mpv, and keep their order.
    void command(std::initializer_list<std::string> args) {
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        mpv_command(mpv, argv.data());
    }

    void publish() {
        Status copy;
        {
            std::lock_guard lock(state_mutex);
            copy = status;
        }
        if (callbacks.status) callbacks.status(copy);
    }
    void publish_queue() {
        if (callbacks.queue) callbacks.queue();
    }
    template <class F>
    void update(F&& change) {
        {
            std::lock_guard lock(state_mutex);
            change(status);
        }
        publish();
    }

    std::optional<EntryId> entry() const {
        std::lock_guard lock(state_mutex);
        return status.entry;
    }

    bool viewed_at(std::int64_t position) const {
        return duration_ms > 0 && position * 100 >= duration_ms * settings.viewed_percent;
    }

    void save_current() {
        const auto e = entry();
        if (!loaded || !e || (duration_ms <= 0 && position_ms <= 0)) return;
        library->save_position(*e, finished ? duration_ms : position_ms, duration_ms, finished || viewed_at(position_ms),
                               now_ns());
        last_save_ms = steady_ms();
    }

    void go_idle() {
        save_current();
        loaded = false;
        appended.reset();
        position_ms = duration_ms = 0;
        update([](Status& s) {
            s.state = State::Idle;
            s.entry.reset();
            s.position_ms = s.duration_ms = s.resumed_from_ms = 0;
            s.has_video = false;
            s.audio.clear();
            s.subtitles.clear();
        });
    }

    // The queue's item at `index` becomes the loaded one (its stored state, fresh counters).
    void begin_item(std::size_t index) {
        EntryId e = 0;
        {
            std::lock_guard lock(state_mutex);
            current = index;
            e = queue[index];
            status.entry = e;
            status.index = index;
            status.state = State::Loading;
            status.position_ms = status.duration_ms = status.resumed_from_ms = 0;
            status.has_video = false;
        }
        loaded = true;
        counted = finished = false;
        position_ms = duration_ms = 0;
        auto stored = library->state(e);
        item = stored ? *stored : ItemState{.entry = e};
        last_save_ms = steady_ms();
    }

    // Loads the first item from `index` on that resolves; reports and skips the others.
    void start_item(std::size_t index) {
        save_current();
        appended.reset();
        const std::vector<EntryId> snapshot = queue_copy();
        for (std::size_t i = index; i < snapshot.size(); ++i) {
            auto path = resolve(snapshot[i]);
            if (!path) {
                if (callbacks.error) callbacks.error(snapshot[i], path.error());
                continue;
            }
            begin_item(i);
            command({"playlist-clear"});
            command({"loadfile", utf8(*path), "replace"});
            const int unpause = 0;
            mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&unpause));
            publish();
            publish_queue();
            prepare_next();
            return;
        }
        // Nothing from `index` on can play: stop, once, rather than loop over the queue.
        command({"stop"});
        {
            std::lock_guard lock(state_mutex);
            current = std::min(index, snapshot.empty() ? std::size_t{0} : snapshot.size() - 1);
        }
        go_idle();
        publish_queue();
    }

    // Hands mpv the next item ahead of time, so it can join the two without a gap.
    void prepare_next() {
        if (appended || !loaded) return;
        const std::vector<EntryId> snapshot = queue_copy();
        std::size_t from = 0;
        {
            std::lock_guard lock(state_mutex);
            from = current + 1;
        }
        for (std::size_t j = from; j < snapshot.size(); ++j) {
            auto path = resolve(snapshot[j]);
            if (!path) {
                if (callbacks.error) callbacks.error(snapshot[j], path.error());
                continue;
            }
            command({"loadfile", utf8(*path), "append"});
            appended = j;
            return;
        }
    }

    std::vector<EntryId> queue_copy() const {
        std::lock_guard lock(state_mutex);
        return queue;
    }
    std::size_t current_index() const {
        std::lock_guard lock(state_mutex);
        return current;
    }
    std::size_t queue_size() const {
        std::lock_guard lock(state_mutex);
        return queue.size();
    }

    std::vector<Track> tracks_of(const mpv_node& list, bool& has_video, std::vector<Track>& subtitles) {
        std::vector<Track> audio;
        has_video = false;
        if (list.format != MPV_FORMAT_NODE_ARRAY) return audio;
        for (int i = 0; i < list.u.list->num; ++i) {
            const mpv_node& t = list.u.list->values[i];
            const std::string type = text_of(member(t, "type"));
            if (type == "video") {
                if (flag_of(member(t, "selected")) && !flag_of(member(t, "albumart"))) has_video = true;
                continue;
            }
            Track track;
            track.id = static_cast<int>(int_of(member(t, "id")));
            track.language = text_of(member(t, "lang"));
            track.title = text_of(member(t, "title"));
            track.codec = text_of(member(t, "codec"));
            track.channels = static_cast<int>(int_of(member(t, "demux-channel-count")));
            track.external = flag_of(member(t, "external"));
            track.forced = flag_of(member(t, "forced"));
            track.selected = flag_of(member(t, "selected"));
            if (type == "audio") audio.push_back(std::move(track));
            else if (type == "sub") subtitles.push_back(std::move(track));
        }
        return audio;
    }

    void read_tracks(const mpv_node& list) {
        bool has_video = false;
        std::vector<Track> subtitles;
        std::vector<Track> audio = tracks_of(list, has_video, subtitles);
        update([&](Status& s) {
            s.audio = std::move(audio);
            s.subtitles = std::move(subtitles);
            s.has_video = has_video;
        });
    }

    void on_position(std::int64_t ms) {
        position_ms = ms;
        {
            std::lock_guard lock(state_mutex);
            status.position_ms = ms;
        }
        if (loaded && duration_ms > 0) {
            const std::int64_t threshold = std::min<std::int64_t>(duration_ms * settings.play_count_percent / 100,
                                                                  std::int64_t{settings.play_count_seconds} * 1000);
            const auto e = entry();
            if (!counted && e && ms >= threshold) {
                library->count_play(*e, now_ns());
                counted = true;
            }
            if (steady_ms() - last_save_ms >= std::int64_t{settings.progress_save_seconds} * 1000) save_current();
        }
        const std::int64_t now = steady_ms();
        if (now - last_position_ms >= 250 && callbacks.position) {
            last_position_ms = now;
            callbacks.position(ms);
        }
    }

    // Subtitles on, and mpv picked none (no track in the preferred languages, none flagged
    // default): the first full track, else a forced one. Only when a file loads, so turning them
    // off on the video page stays off.
    void pick_subtitle() {
        std::optional<int> pick;
        {
            std::lock_guard lock(state_mutex);
            for (const Track& t : status.subtitles) {
                if (t.selected) return;
            }
            for (const Track& t : status.subtitles) {
                if (!t.forced) {
                    pick = t.id;
                    break;
                }
            }
            if (!pick && !status.subtitles.empty()) pick = status.subtitles.front().id;
        }
        if (!pick) return;
        std::int64_t id = *pick;
        mpv_set_property(mpv, "sid", MPV_FORMAT_INT64, &id);
    }

    void on_file_loaded() {
        // The track list now, to know whether there's a picture (resume is for video by default).
        mpv_node list{};
        if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &list) >= 0) {
            read_tracks(list);
            mpv_free_node_contents(&list);
        }
        if (settings.subtitles_on) pick_subtitle();
        double duration = 0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0) duration_ms = std::llround(duration * 1000);
        int paused = 0;
        mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &paused);
        bool has_video = false;
        {
            std::lock_guard lock(state_mutex);
            has_video = status.has_video;
        }
        std::int64_t resumed = 0;
        const bool resumable = has_video || settings.resume_audio;
        // The end of a file is covered by "viewed" (viewed_percent): a viewed item starts over.
        if (resumable && !item.viewed && item.position_ms >= std::int64_t{settings.resume_min_seconds} * 1000) {
            command({"seek", seconds_text(item.position_ms), "absolute"});
            resumed = item.position_ms;
            position_ms = item.position_ms;
        }
        update([&](Status& s) {
            s.state = paused ? State::Paused : State::Playing;
            s.duration_ms = duration_ms;
            s.resumed_from_ms = resumed;
            s.position_ms = position_ms;
        });
    }

    void on_end(const mpv_event_end_file& end) {
        if (end.reason == MPV_END_FILE_REASON_STOP || end.reason == MPV_END_FILE_REASON_QUIT ||
            end.reason == MPV_END_FILE_REASON_REDIRECT) {
            return;   // we replaced or stopped it ourselves
        }
        if (!loaded) return;
        if (end.reason == MPV_END_FILE_REASON_ERROR) {
            const auto e = entry();
            if (e && callbacks.error) callbacks.error(*e, Error{std::string("can't play: ") + mpv_error_string(end.error)});
        } else {
            finished = true;
            const auto e = entry();
            if (!counted && e) {   // a short file can end between two position updates
                library->count_play(*e, now_ns());
                counted = true;
            }
            save_current();
        }
        if (appended) return;   // mpv moves on to it by itself (MPV_EVENT_START_FILE)
        const std::size_t next = current_index() + 1;
        if (next < queue_size()) start_item(next);
        else go_idle();
    }

    void on_start_file() {
        std::int64_t pos = 0;
        mpv_get_property(mpv, "playlist-pos", MPV_FORMAT_INT64, &pos);
        if (!appended || pos != 1) return;   // the file start_item loaded
        // mpv went on to the file it held next: the queue follows it.
        save_current();
        const std::size_t index = *appended;
        appended.reset();
        command({"playlist-remove", "0"});
        begin_item(index);
        publish();
        publish_queue();
        prepare_next();
    }

    void on_property(const mpv_event_property& p, std::uint64_t id) {
        switch (id) {
        case TimePos:
            if (p.format == MPV_FORMAT_DOUBLE) on_position(std::llround(*static_cast<double*>(p.data) * 1000));
            break;
        case Duration:
            if (p.format == MPV_FORMAT_DOUBLE) {
                duration_ms = std::llround(*static_cast<double*>(p.data) * 1000);
                update([&](Status& s) { s.duration_ms = duration_ms; });
            }
            break;
        case Pause:
            if (p.format == MPV_FORMAT_FLAG && loaded) {
                const bool paused = *static_cast<int*>(p.data) != 0;
                if (paused) save_current();
                update([&](Status& s) {
                    if (s.state != State::Loading) s.state = paused ? State::Paused : State::Playing;
                });
            }
            break;
        case Volume:
            if (p.format == MPV_FORMAT_DOUBLE) {
                const int volume = static_cast<int>(std::lround(*static_cast<double*>(p.data)));
                if (volume != settings.volume) {
                    settings.volume = volume;
                    settings_dirty = true;
                }
                update([&](Status& s) { s.volume = volume; });
            }
            break;
        case Mute:
            if (p.format == MPV_FORMAT_FLAG) {
                const bool muted = *static_cast<int*>(p.data) != 0;
                if (muted != settings.muted) {
                    settings.muted = muted;
                    settings_dirty = true;
                }
                update([&](Status& s) { s.muted = muted; });
            }
            break;
        case TrackList:
            if (p.format == MPV_FORMAT_NODE) read_tracks(*static_cast<mpv_node*>(p.data));
            break;
        default:
            break;
        }
    }

    void run() {
        while (true) {
            const mpv_event* event = mpv_wait_event(mpv, 0.5);
            switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                on_property(*static_cast<mpv_event_property*>(event->data), event->reply_userdata);
                break;
            case MPV_EVENT_START_FILE:
                on_start_file();
                break;
            case MPV_EVENT_FILE_LOADED:
                on_file_loaded();
                break;
            case MPV_EVENT_END_FILE:
                on_end(*static_cast<mpv_event_end_file*>(event->data));
                break;
            default:
                break;
            }
            std::deque<std::function<void()>> todo;
            bool quit = false;
            {
                std::lock_guard lock(tasks_mutex);
                todo.swap(tasks);
                quit = stopping;
            }
            for (auto& task : todo) task();
            // mpv notifies time-pos changes as it likes (rarely for audio-only files): poll it too,
            // so progress, play counts, and viewed don't depend on that.
            if (loaded && steady_ms() - last_poll_ms >= 500) {
                last_poll_ms = steady_ms();
                double pos = 0;
                if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) on_position(std::llround(pos * 1000));
            }
            if (settings_dirty && steady_ms() - last_flush_ms >= 1000) flush_settings();
            if (quit) break;
        }
        save_current();
        if (settings_dirty) flush_settings();
    }

    void flush_settings() {
        library->save_settings(settings);
        settings_dirty = false;
        last_flush_ms = steady_ms();
    }
};

Result<std::unique_ptr<Player>> Player::start(const fs::path& database_file, Resolver resolver, Callbacks callbacks,
                                              Outputs outputs) {
    std::setlocale(LC_NUMERIC, "C");   // libmpv refuses to start otherwise
    auto impl = std::make_unique<Impl>();
    auto database = db::Database::open(database_file);
    if (!database) return std::unexpected(Error{database.error().message});
    impl->db.emplace(std::move(*database));
    auto library = Library::open(*impl->db);
    if (!library) return std::unexpected(library.error());
    impl->library.emplace(std::move(*library));
    auto settings = impl->library->load_settings();
    if (!settings) return std::unexpected(settings.error());
    impl->settings = *settings;
    impl->resolve = std::move(resolver);
    impl->callbacks = std::move(callbacks);

    impl->mpv = mpv_create();
    if (!impl->mpv) return std::unexpected(Error{"libmpv couldn't start"});
    const Settings& s = impl->settings;
    const auto option = [&](const char* name, const std::string& value) { mpv_set_option_string(impl->mpv, name, value.c_str()); };
    // An embedded player: none of mpv's own configuration, keys, or on-screen controls.
    option("config", "no");
    option("terminal", "no");
    option("input-default-bindings", "no");
    option("input-vo-keyboard", "no");
    option("osc", "no");
    option("ytdl", "no");
    option("idle", "yes");
    option("keep-open", "no");
    option("audio-display", "no");   // a music file's cover is not a picture to show
    option("vo", outputs.video);
    if (!outputs.log_file.empty()) option("log-file", utf8(outputs.log_file));
    if (outputs.audio != "auto") option("ao", outputs.audio);
    option("hwdec", s.hardware_decoding ? "auto-safe" : "no");
    // Read ahead (features/player.md §4.6): the cache also for local files, capped.
    option("cache", "yes");
    option("demuxer-max-bytes", std::to_string(s.read_ahead_mib) + "MiB");
    option("demuxer-readahead-secs", std::to_string(s.read_ahead_seconds));
    option("gapless-audio", "weak");
    option("prefetch-playlist", "yes");
    option("sub-auto", "fuzzy");
    if (!s.audio_languages.empty()) option("alang", s.audio_languages);
    if (!s.subtitle_languages.empty()) option("slang", s.subtitle_languages);
    option("sid", s.subtitles_on ? "auto" : "no");
    option("volume-max", "100");
    option("volume", std::to_string(s.volume));
    option("mute", s.muted ? "yes" : "no");
    if (mpv_initialize(impl->mpv) < 0) {
        mpv_terminate_destroy(impl->mpv);
        return std::unexpected(Error{"libmpv couldn't initialize"});
    }
    mpv_observe_property(impl->mpv, TimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(impl->mpv, Duration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(impl->mpv, Pause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(impl->mpv, Volume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(impl->mpv, Mute, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(impl->mpv, TrackList, "track-list", MPV_FORMAT_NODE);
    impl->status.volume = s.volume;
    impl->status.muted = s.muted;
    impl->video.emplace(VideoRenderer(impl->mpv));

    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<Player>(new Player(std::move(impl)));
}

Player::Player(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Player::~Player() {
    {
        std::lock_guard lock(impl_->tasks_mutex);
        impl_->stopping = true;
    }
    mpv_wakeup(impl_->mpv);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->video->destroy();   // a no-op when the app destroyed it first, as it must
    mpv_terminate_destroy(impl_->mpv);
}

void Player::play_now(std::vector<EntryId> entries, std::size_t start) {
    impl_->post([impl = impl_.get(), entries = std::move(entries), start]() mutable {
        {
            std::lock_guard lock(impl->state_mutex);
            impl->queue = std::move(entries);
        }
        if (impl->queue_size() == 0) {
            impl->command({"stop"});
            impl->go_idle();
            impl->publish_queue();
            return;
        }
        impl->start_item(std::min(start, impl->queue_size() - 1));
    });
}

void Player::add(std::vector<EntryId> entries) {
    impl_->post([impl = impl_.get(), entries = std::move(entries)] {
        if (entries.empty()) return;
        std::size_t first = 0;
        {
            std::lock_guard lock(impl->state_mutex);
            first = impl->queue.size();
            impl->queue.insert(impl->queue.end(), entries.begin(), entries.end());
        }
        if (!impl->loaded) impl->start_item(first);
        else {
            impl->publish_queue();
            impl->prepare_next();
        }
    });
}

void Player::jump(std::size_t index) {
    impl_->post([impl = impl_.get(), index] {
        if (index < impl->queue_size()) impl->start_item(index);
    });
}

void Player::remove(std::size_t index) {
    impl_->post([impl = impl_.get(), index] {
        std::size_t current = 0, size = 0;
        {
            std::lock_guard lock(impl->state_mutex);
            if (index >= impl->queue.size()) return;
            impl->queue.erase(impl->queue.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < impl->current) --impl->current;
            current = impl->current;
            size = impl->queue.size();
        }
        if (impl->loaded && index == current && !(impl->appended && *impl->appended == index)) {
            // The loaded item itself: the one after it (now at the same index) plays.
            if (current < size) impl->start_item(current);
            else {
                impl->command({"stop"});
                impl->go_idle();
                impl->publish_queue();
            }
            return;
        }
        if (impl->appended) {
            if (*impl->appended == index) {
                impl->command({"playlist-remove", "1"});
                impl->appended.reset();
                impl->prepare_next();
            } else if (*impl->appended > index) {
                --*impl->appended;
            }
        }
        impl->publish_queue();
    });
}

void Player::clear() {
    impl_->post([impl = impl_.get()] {
        impl->command({"stop"});
        impl->go_idle();
        {
            std::lock_guard lock(impl->state_mutex);
            impl->queue.clear();
            impl->current = 0;
        }
        impl->publish_queue();
    });
}

std::vector<EntryId> Player::queue() const { return impl_->queue_copy(); }
std::size_t Player::current() const { return impl_->current_index(); }

void Player::toggle() {
    impl_->post([impl = impl_.get()] {
        if (!impl->loaded) {
            if (impl->queue_size() > 0) impl->start_item(impl->current_index());
            return;
        }
        impl->command({"cycle", "pause"});
    });
}

void Player::pause() {
    impl_->post([impl = impl_.get()] {
        const int on = 1;
        if (impl->loaded) mpv_set_property(impl->mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&on));
    });
}

void Player::resume() {
    impl_->post([impl = impl_.get()] {
        if (!impl->loaded) {
            if (impl->queue_size() > 0) impl->start_item(impl->current_index());
            return;
        }
        const int off = 0;
        mpv_set_property(impl->mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&off));
    });
}

void Player::stop() {
    impl_->post([impl = impl_.get()] {
        impl->command({"stop"});
        impl->go_idle();
    });
}

void Player::next() {
    impl_->post([impl = impl_.get()] {
        const std::size_t next = impl->current_index() + 1;
        if (next < impl->queue_size()) impl->start_item(next);
        else {
            impl->command({"stop"});
            impl->go_idle();
        }
    });
}

void Player::previous() {
    impl_->post([impl = impl_.get()] {
        const std::size_t current = impl->current_index();
        if (impl->loaded && impl->position_ms > std::int64_t{impl->settings.previous_restarts_after_seconds} * 1000) {
            impl->command({"seek", "0", "absolute"});
        } else if (current > 0) {
            impl->start_item(current - 1);
        } else if (impl->loaded) {
            impl->command({"seek", "0", "absolute"});
        }
    });
}

void Player::seek(std::int64_t position_ms) {
    impl_->post([impl = impl_.get(), position_ms] {
        if (!impl->loaded) return;
        std::int64_t to = std::max<std::int64_t>(0, position_ms);
        if (impl->duration_ms > 0) to = std::min(to, impl->duration_ms);
        impl->command({"seek", seconds_text(to), "absolute"});
        impl->position_ms = to;
        impl->save_current();
    });
}

void Player::seek_by(std::int64_t delta_ms) {
    impl_->post([impl = impl_.get(), delta_ms] {
        if (!impl->loaded) return;
        impl->command({"seek", seconds_text(delta_ms), "relative"});
    });
}

void Player::set_volume(int volume) {
    impl_->post([impl = impl_.get(), volume] {
        double v = std::clamp(volume, 0, 100);
        mpv_set_property(impl->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
    });
}

void Player::set_muted(bool muted) {
    impl_->post([impl = impl_.get(), muted] {
        int flag = muted ? 1 : 0;
        mpv_set_property(impl->mpv, "mute", MPV_FORMAT_FLAG, &flag);
    });
}

void Player::select_audio(int track_id) {
    impl_->post([impl = impl_.get(), track_id] {
        std::int64_t id = track_id;
        mpv_set_property(impl->mpv, "aid", MPV_FORMAT_INT64, &id);
    });
}

void Player::select_subtitle(std::optional<int> track_id) {
    impl_->post([impl = impl_.get(), track_id] {
        if (!track_id) {
            mpv_set_property_string(impl->mpv, "sid", "no");
            return;
        }
        std::int64_t id = *track_id;
        mpv_set_property(impl->mpv, "sid", MPV_FORMAT_INT64, &id);
    });
}

Status Player::status() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->status;
}

VideoRenderer& Player::video() { return *impl_->video; }

}
