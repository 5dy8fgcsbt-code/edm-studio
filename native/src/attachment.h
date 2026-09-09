#pragma once
#include "model.h"

namespace edm {
bool isConnector(const Node& node);
// A name must identify one connector. "#123" explicitly selects a scene node index.
int findConnector(const Scene& scene, std::string_view name);
// Existing base indices remain stable. Every independent child argument receives a new index.
std::shared_ptr<Scene> attachScene(const Scene& base, const Scene& child, int targetConnector,
                                   const Args& childDefaults = {});
} // namespace edm
