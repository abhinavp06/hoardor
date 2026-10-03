#include <hoardor/master/playback.hpp>

#include <hoardor/file/library.hpp>

#include <memory>
#include <optional>

namespace hoardor::master {

player::Resolver file_resolver(const std::filesystem::path& database_file) {
    struct Connection {
        std::optional<db::Database> database;
        std::optional<file::Library> files;
    };
    auto connection = std::make_shared<Connection>();
    return [database_file, connection](player::EntryId entry) -> player::Result<std::filesystem::path> {
        if (!connection->files) {
            auto database = db::Database::open(database_file);
            if (!database) return std::unexpected(player::Error{database.error().message});
            connection->database.emplace(std::move(*database));
            auto files = file::Library::open(*connection->database);
            if (!files) {
                connection->database.reset();
                return std::unexpected(player::Error{files.error().message});
            }
            connection->files.emplace(std::move(*files));
        }
        auto path = connection->files->resolve(entry);
        if (path) return *path;
        switch (path.error().code) {
        case file::ErrorCode::RootOffline: return std::unexpected(player::Error{"its drive is offline"});
        case file::ErrorCode::FileMissing: return std::unexpected(player::Error{"the file is missing"});
        case file::ErrorCode::NotFound: return std::unexpected(player::Error{"it's no longer in the library"});
        default: return std::unexpected(player::Error{path.error().message});
        }
    };
}

}
