#pragma once

#include <string>

namespace engine {

// Task 033: prompt -> animation graph JSON (deterministic template engine).
// Keywords: idle/stand, walk, run/sprint, jump/leap.
// Output matches assets format: {"nodes":[...],"links":[...],"output":N}
// with Clip nodes referencing clip indices into the component's animations.
// No keywords recognized -> default locomotion (idle+walk+run).
std::string generateGraphJSON(const std::string& prompt);

} // namespace engine
