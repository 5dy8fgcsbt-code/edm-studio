#pragma once
#include "model.h"

namespace edm {
bool isConnector(const Node& node);
// A name must identify one connector. "#123" explicitly selects a scene node index.
int findConnector(const Scene& scene, std::string_view name);
// Existing base indices remain stable. Every independent child argument receives a new index.
std::shared_ptr<Scene> attachScene(const Scene& base, const Scene& child, int targetConnector,
                                   const Args& childDefaults = {},
                                   std::optional<std::map<int, int>> forcedArgumentMap = std::nullopt);
struct DetachResult {
    std::shared_ptr<Scene> scene;
    std::vector<int> nodeMap, materialMap, meshMap, attachmentMap;
    std::set<int> removedArguments;
    size_t removedCount = 0;
};
// Remove every direct payload of this connector, including each payload's descendants.
// Maps translate old indices to new ones; a removed element maps to -1.
DetachResult detachScene(const Scene& source, int targetConnector);
} // namespace edm
