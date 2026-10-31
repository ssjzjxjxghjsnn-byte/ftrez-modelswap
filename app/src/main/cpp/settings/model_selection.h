#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ft::settings {
struct SelectionChoice { int key; std::string id, label; };

// An empty identifier means the original model. Catalog keys are session-local;
// only the identifier is serialized, including while its directory is loading.
class ModelSelection {
public:
    static constexpr std::string_view header = "FTREZ_MODEL_SELECTION_V1\n";
    std::string id;

    static bool validId(std::string_view value) {
        return value.size() <= 4096 && value.find_first_of("\r\n") == value.npos &&
            value.find('\0') == value.npos;
    }
    bool decode(std::string_view bytes) {
        if (bytes.size() < header.size() || bytes.substr(0, header.size()) != header) return false;
        bytes.remove_prefix(header.size());
        if (bytes.empty() || bytes.back() != '\n') return false;
        bytes.remove_suffix(1);
        if (!validId(bytes)) return false;
        id.assign(bytes);
        return true;
    }
    std::string encode() const { return std::string(header) + id + '\n'; }
    std::optional<int> resolve(const std::vector<SelectionChoice>& choices) const {
        if (id.empty()) return -1;
        for (const auto& choice : choices) if (choice.id == id) return choice.key;
        return {}; // Do not discard a saved choice before the async scan finishes.
    }
    void cycle(const std::vector<SelectionChoice>& choices, int direction) {
        size_t position = 0;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i].id == id) { position = i + 1; break; }
        const auto count = choices.size() + 1;
        position = direction < 0 ? (position + count - 1) % count : (position + 1) % count;
        id = position ? choices[position - 1].id : std::string{};
    }
    std::string label(const std::vector<SelectionChoice>& choices) const {
        if (id.empty()) return "原始模型";
        for (const auto& choice : choices) if (choice.id == id) return choice.label;
        return "等待模型";
    }
};
}
