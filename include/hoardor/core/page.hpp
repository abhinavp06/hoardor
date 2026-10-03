#pragma once

// Keyset paging, shared by every engine that lists things (ARCHITECTURE §4: never load
// a whole library; never OFFSET). The first core component, triggered by the audio and
// video engines (features/media_listing.md §5).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hoardor::core {

// A position in a list. Opaque to callers: pass a page's `next` back to get the
// following page. It only fits the query (filter, order) that produced it.
struct Cursor {
    std::string key;
    std::int64_t id = 0;
};

template <class T>
struct Page {
    std::vector<T> items;
    std::optional<Cursor> next;  // empty: this was the last page
};

}
