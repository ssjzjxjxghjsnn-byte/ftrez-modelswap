#include "settings/model_selection.h"
#include <cstdlib>
#include <iostream>

using ft::settings::ModelSelection;
using ft::settings::SelectionChoice;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    const std::vector<SelectionChoice> first{{3, "knight", "模型骑士"}, {8, "models/法师", "模型法师"}};
    ModelSelection selection;
    require(selection.resolve(first) == -1, "Default must preserve original model");
    selection.cycle(first, 1);
    require(selection.id == "knight" && selection.resolve(first) == 3, "Next must select first catalog choice");
    selection.cycle(first, -1);
    require(selection.id.empty(), "Previous must restore original model");
    selection.cycle(first, -1);
    require(selection.id == "models/法师", "Previous from original must wrap to last model");
    require(selection.label(first) == "模型法师", "Display label must come from catalog");

    const auto saved = selection.encode();
    ModelSelection restored;
    require(restored.decode(saved) && restored.id == "models/法师", "UTF-8 stable ID must survive serialization");
    require(!restored.resolve({}), "Saved ID must wait while async catalog is empty");
    require(restored.label({}) == "等待模型", "Pending model must be visible to the user");
    require(restored.encode() == saved, "An unfinished scan must not discard the persisted selection");
    const std::vector<SelectionChoice> later{{0, "models/法师", "模型法师"}, {1, "knight", "模型骑士"}};
    require(restored.resolve(later) == 0, "Restore must use stable ID despite catalog reorder and changed keys");
    restored.cycle(later, 1);
    require(restored.id == "knight", "Cycling after restore must follow the current catalog");
    restored.cycle(later, 1);
    require(restored.id.empty() && restored.resolve(later) == -1, "Forward wrap must include original model");
    ModelSelection original;
    require(original.decode(restored.encode()) && original.id.empty(), "Original model must be persistable");

    ModelSelection missing;
    missing.id = "temporarily-unavailable";
    require(!missing.resolve(first), "Missing saved model must not select a different numeric key");
    require(missing.id == "temporarily-unavailable", "Catalog queries must not clear unresolved ID");
    missing.cycle(first, -1);
    require(missing.id == "models/法师", "Explicit user input must leave pending state");
    missing.cycle({}, 1);
    require(missing.id.empty(), "Empty catalog must still offer original model");

    const std::string header(ModelSelection::header);
    selection.id = "keep-me";
    for (const auto& bad : {std::string{}, std::string("knight\n"), header, header + "knight",
            header + "knight\nextra\n", header + "bad\r\n", header + std::string("bad\0id", 6) + '\n',
            header + std::string(4097, 'a') + '\n'}) {
        require(!selection.decode(bad), "Malformed selection must be rejected");
        require(selection.id == "keep-me", "Malformed data must not replace a valid selection");
    }
    require(selection.decode(header + std::string(4096, 'a') + '\n'), "Maximum permitted ID must be accepted");
    std::cout << "model_selection_test: passed\n";
}
