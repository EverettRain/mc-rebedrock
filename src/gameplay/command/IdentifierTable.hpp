#pragma once

#include "gameplay/command/ArgumentType.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace mc::gameplay::command {

// One candidate a table-backed argument validates and completes to. `identifier`
// is the namespaced form the command completes to (e.g. "rebedrock:stone"); the
// SuggestionSink also matches its bare path, so typing `stone` finds it. `hint`
// is the tooltip text (display name, category, ...).
struct TableEntry final {
    std::string_view identifier;
    std::string_view hint;
};

// A table-backed argument, the mechanism that gives every registered table
// automatic completion and validation. The `Table` type implements the visitor
// contract chosen at planning time — C++ template callbacks, not a virtual
// visitor:
//
//     template <typename F> void forEach(F&& visitor) const; // F: void(const TableEntry&)
//     bool contains(std::string_view identifier) const;      // validation
//     std::string_view kind() const;                         // "item", "game mode", ...
//
// Adapters are thin wrappers over the existing registries (constexpr arrays,
// runtime slots), so adding an entry to a table — one constexpr line — flows
// into completion and validation with no command-side changes.
template <typename Table>
class TableArgument final : public ArgumentType {
  public:
    // The table adapters are stateless value types, so default-constructing the
    // argument (`make_unique<TableArgument<ItemTable>>()`) is the normal case.
    explicit TableArgument(Table table = {}) : table_(std::move(table)) {}

    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a " + std::string{table_.kind()}, reader);
        }
        if (!table_.contains(*token)) {
            return parseFail("Unknown " + std::string{table_.kind()} + ": " + *token, reader);
        }
        return parseOk(std::string{*token});
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        table_.forEach([&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }

  private:
    Table table_;
};

} // namespace mc::gameplay::command
