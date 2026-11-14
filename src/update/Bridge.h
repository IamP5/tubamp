#pragma once

// The update half of the editor bridge, as JSON text: the shapes in ui/src/bridge/types.ts
// (UpdateView, UpdateCommand). WebEditor converts the text to and from juce::var, so this
// file needs no JUCE and tubamp_update_tool checks it against the golden file
// ui/src/bridge/update-contract.json, which tsc checks from the other side.

#include "UpdateTypes.h"

#include <optional>
#include <string>
#include <string_view>

namespace tubamp::update
{
/** The `UiState.update` field and the `updateChanged` payload. `hostName` ("Logic Pro")
    completes the restart sentence. */
std::string viewJson (const View&, std::string_view hostName);

/** The argument of the `updateCommand` native function. nullopt unless it is exactly one
    of the documented shapes: a known `type`, every field present with the right type, a
    valid version, and no other key. */
std::optional<Command> parseCommand (std::string_view json);
} // namespace tubamp::update
