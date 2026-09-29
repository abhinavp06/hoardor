#include <file/file_engine.hpp>

std::vector<FileEntry> hoardor::file::discover(const std::filesystem::path& root, MediaType media_type) {
	std::vector<FileEntry> result;

	if (std::filesystem::exists(root) && std::filesystem::is_directory(root)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file(ec)) continue;

			FileEntry file_entry;

			file_entry.relative_path = std::filesystem::relative(entry.path(), root);
			file_entry.size = entry.file_size();
			file_entry.last_modified = entry.last_write_time();

			result.push_back(file_entry);
		}

		return result;
	}
	else {
		// TODO: throw error here
	}
}