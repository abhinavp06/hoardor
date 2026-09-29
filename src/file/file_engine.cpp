#include <hoardor/file/file_engine.hpp>

std::vector<hoardor::file::FileEntry> hoardor::file::discover(const std::filesystem::path& root, MediaType media_type) {
	std::vector<FileEntry> result;

	if (std::filesystem::exists(root) && std::filesystem::is_directory(root)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {

			FileEntry file_entry;

			file_entry.relative_path = std::filesystem::relative(entry.path(), root);
			file_entry.size = entry.file_size();
			file_entry.last_modified = entry.last_write_time();

			result.push_back(file_entry);
		}

		return result;
	}
	else {
		return result; // TODO: throw error here.. returning result for now
	}
}