#include "core/Json.hpp"

#include <cassert>
#include <cmath>

using mc::core::Json;

int main() {
    const auto document = Json::parse(R"({
        "name": "root",
        "loop": true,
        "length": 1.5,
        "list": [1, 2, 3.5, -4e2],
        "nested": {"a": null, "b": "he\"llo\n"},
        // line comment tolerated in geometry files
        "unicode": "Aé"
    })");

    assert(document["name"].asString() == "root");
    assert(document["loop"].asBool());
    assert(std::abs(document["length"].asNumber() - 1.5) < 1e-9);
    assert(document["list"].isArray());
    assert(document["list"].size() == 4U);
    assert(std::abs(document["list"][3].asNumber() + 400.0) < 1e-6);
    assert(document["nested"]["a"].isNull());
    assert(document["nested"]["b"].asString() == "he\"llo\n");
    assert(document["unicode"].asString() == "A\xc3\xa9");
    assert(!document.contains("missing"));
    assert(document["missing"].isNull());
    assert(document["list"][99].isNull());

    // Object member order is preserved.
    assert(document.asObject().front().first == "name");

    bool threw = false;
    try {
        Json parsed = Json::parse("{ this is not json }");
        (void)parsed;
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    return 0;
}
