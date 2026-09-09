#include "attachment.h"
#include <Eigen/SVD>
#include <charconv>
#include <limits>

namespace edm {
namespace {
bool argumentKey(std::string_view key) {
    return key == "argument" || key == "edm_argument" || key == "edm_damage_argument";
}
void inspectArguments(const Json& value, std::set<int>& arguments) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (argumentKey(it.key()) && it.value().is_number_integer()) {
                const auto argument = it.value().get<int64_t>();
                require(argument <= std::numeric_limits<int>::max(), "EDM argument index exceeds int range");
                if (argument >= 0)
                    arguments.insert(int(argument));
            } else
                inspectArguments(it.value(), arguments);
        }
    } else if (value.is_array())
        for (const auto& child : value)
            inspectArguments(child, arguments);
}
std::set<int> arguments(const Scene& scene) {
    std::set<int> result;
    for (auto [argument, range] : scene.limits)
        result.insert(argument);
    for (auto [argument, value] : scene.defaultArgs)
        result.insert(argument);
    for (const auto& track : scene.tracks)
        result.insert(track.arg);
    result.insert(scene.numberArgs.begin(), scene.numberArgs.end());
    for (const auto& node : scene.nodes)
        inspectArguments(node.extras, result);
    for (const auto& mesh : scene.meshes) {
        inspectArguments(mesh.extras, result);
        for (const auto& number : mesh.numbers)
            for (int argument : {number.u, number.v})
                if (argument >= 0)
                    result.insert(argument);
    }
    for (const auto& material : scene.materials) {
        inspectArguments(material.uniforms, result);
        inspectArguments(material.animatedUniforms, result);
        inspectArguments(material.extras, result);
    }
    require(result.empty() || *result.begin() >= 0, "Negative scene argument index");
    return result;
}
void remapMetadata(Json& value, const std::map<int, int>& mapping, int rawNodeOffset, int renderOffset) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (argumentKey(it.key()) && it.value().is_number_integer()) {
                const int argument = it.value().get<int>();
                if (argument >= 0)
                    it.value() = mapping.at(argument);
            } else if ((it.key() == "edm_node" || it.key() == "edm_parent") &&
                       it.value().is_number_integer()) {
                const int index = it.value().get<int>();
                if (index >= 0)
                    it.value() = index + rawNodeOffset;
            } else if (it.key() == "edm_render_index" && it.value().is_number_integer())
                it.value() = it.value().get<int>() + renderOffset;
            else if (it.key() == "number_controls" && it.value().is_array()) {
                for (auto& control : it.value())
                    if (control.is_array() && control.size() == 4)
                        for (int element : {0, 2}) {
                            const int argument = control[element].get<int>();
                            if (argument >= 0)
                                control[element] = mapping.at(argument);
                        }
            } else
                remapMetadata(it.value(), mapping, rawNodeOffset, renderOffset);
        }
    } else if (value.is_array())
        for (auto& child : value)
            remapMetadata(child, mapping, rawNodeOffset, renderOffset);
}
void affine(const Mat& matrix, std::string_view name) {
    require(matrix.allFinite() && matrix.row(3).head<3>().cwiseAbs().maxCoeff() < 1e-9 &&
                std::abs(matrix(3, 3) - 1) < 1e-9,
            "Invalid attachment affine transform: " + std::string(name));
}
void rebuild(Scene& scene) {
    scene.order.clear();
    scene.staticLocal.clear();
    std::vector<std::vector<int>> children(scene.nodes.size());
    std::vector<int> pending;
    for (int index = 0; index < int(scene.nodes.size()); ++index) {
        const auto& node = scene.nodes[index];
        require(node.parent >= -1 && node.parent < int(scene.nodes.size()), "Invalid attachment parent");
        scene.staticLocal.push_back(node.local());
        affine(scene.staticLocal.back(), node.name);
        if (node.parent >= 0)
            children[node.parent].push_back(index);
        else
            pending.push_back(index);
    }
    while (!pending.empty()) {
        const int index = pending.back();
        pending.pop_back();
        scene.order.push_back(index);
        pending.insert(pending.end(), children[index].begin(), children[index].end());
    }
    require(scene.order.size() == scene.nodes.size(), "Attachment scene graph cycle");
    scene.defaultWorld = scene.evaluate({});
}
void validateScene(const Scene& scene) {
    require(!scene.nodes.empty() && scene.nodes.size() == scene.staticLocal.size() &&
                scene.nodes.size() == scene.order.size(),
            "Attachment source is not a built scene");
    // Validate evaluation order before evaluate() follows parent and skin indices.
    std::vector<bool> seen(scene.nodes.size());
    for (const int index : scene.order) {
        require(index >= 0 && index < int(scene.nodes.size()) && !seen[index],
                "Invalid attachment scene order");
        const int parent = scene.nodes[index].parent;
        require(parent >= -1 && parent < int(scene.nodes.size()) && (parent < 0 || seen[parent]),
                "Invalid attachment scene parent/order");
        affine(scene.staticLocal[index], scene.nodes[index].name);
        seen[index] = true;
    }
    for (const auto& track : scene.tracks)
        require(track.node >= 0 && track.node < int(scene.nodes.size()) && track.arg >= 0,
                "Invalid attachment animation track");
    for (const auto& mesh : scene.meshes) {
        require(mesh.node >= 0 && mesh.node < int(scene.nodes.size()) && mesh.material >= 0 &&
                    mesh.material < int(scene.materials.size()) &&
                    mesh.skinNodes.size() == mesh.inverseBind.size(),
                "Invalid attachment mesh references");
        for (const int joint : mesh.skinNodes)
            require(joint >= 0 && joint < int(scene.nodes.size()), "Invalid attachment skin node");
    }
}
} // namespace
bool isConnector(const Node& node) {
    return node.extras.is_object() && node.extras.value("edm_type", "") == "Connector";
}
int findConnector(const Scene& scene, std::string_view name) {
    if (name.starts_with('#')) {
        int index = -1;
        const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), index);
        require(parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && index >= 0 &&
                    index < int(scene.nodes.size()) && isConnector(scene.nodes[index]),
                "Invalid connector node index: " + std::string(name));
        return index;
    }
    int result = -1;
    for (int index = 0; index < int(scene.nodes.size()); ++index)
        if (isConnector(scene.nodes[index]) && scene.nodes[index].name == name) {
            require(result < 0, "Ambiguous connector name: " + std::string(name) +
                                    "; select a scene node explicitly with #index");
            result = index;
        }
    require(result >= 0, "Connector not found: " + std::string(name));
    return result;
}
std::shared_ptr<Scene> attachScene(const Scene& base, const Scene& child, int targetConnector,
                                   const Args& childDefaults,
                                   std::optional<std::map<int, int>> forcedArgumentMap) {
    validateScene(base);
    validateScene(child);
    require(targetConnector >= 0 && targetConnector < int(base.nodes.size()) &&
                isConnector(base.nodes[targetConnector]),
            "Attachment target must be a Connector node");
    Args defaults = child.defaultArgs;
    for (auto [argument, value] : childDefaults) {
        require(argument >= 0 && std::isfinite(value), "Invalid attachment default argument");
        defaults[argument] = value;
    }
    int attachPoint = -1;
    for (int index = 0; index < int(child.nodes.size()); ++index) {
        const auto& node = child.nodes[index];
        // A preassembled child's own mount is distinct from mounts of its existing payloads.
        if (isConnector(node) && node.name == "AttachPoint" && !node.extras.contains("edm_attachment")) {
            require(attachPoint < 0, "Child EDM has multiple AttachPoint connectors; mounting is ambiguous");
            attachPoint = index;
        }
    }
    const auto childWorld = child.evaluate(defaults);
    const Mat mount = attachPoint >= 0 ? childWorld[attachPoint] : Mat::Identity();
    affine(mount, "AttachPoint");
    Eigen::JacobiSVD<Eigen::Matrix3d> mountSvd(mount.block<3, 3>(0, 0));
    const auto singular = mountSvd.singularValues();
    require(singular.minCoeff() > 1e-12 * std::max(1., singular.maxCoeff()),
            "Child AttachPoint is singular at its default pose");
    const Mat offset = mount.inverse();
    affine(offset, "inverse AttachPoint");
    auto result = std::make_shared<Scene>(base);
    const int attachmentIndex = int(result->attachments.size());
    const int nodeBegin = int(result->nodes.size()), nodeOffset = nodeBegin + 2;
    const int materialOffset = int(result->materials.size()), meshOffset = int(result->meshes.size());
    auto reserved = arguments(base), childArguments = arguments(child);
    for (auto [argument, value] : defaults) {
        require(argument >= 0 && std::isfinite(value), "Invalid attachment default argument");
        childArguments.insert(argument);
    }
    std::map<int, int> mapping;
    int64_t next = reserved.empty() ? 0 : int64_t(*reserved.rbegin()) + 1;
    if (forcedArgumentMap) {
        require(forcedArgumentMap->size() == childArguments.size(),
                "Saved attachment argument map is incomplete");
        for (auto [original, assigned] : *forcedArgumentMap) {
            require(childArguments.contains(original) && assigned >= 0 && reserved.insert(assigned).second,
                    "Saved attachment argument map is invalid or conflicts with another object");
            mapping[original] = assigned;
        }
    } else
        for (int original : childArguments) {
            if (next > std::numeric_limits<int>::max())
                next = 0;
            while (next <= std::numeric_limits<int>::max() && reserved.contains(int(next)))
                ++next;
            require(next <= std::numeric_limits<int>::max(), "Attachment argument index space exhausted");
            mapping[original] = int(next);
            reserved.insert(int(next++));
        }
    for (auto [argument, value] : defaults)
        result->defaultArgs[mapping.at(argument)] = value;
    int renderOffset = 0;
    for (const auto& mesh : base.meshes)
        if (mesh.extras.is_object())
            renderOffset = std::max(renderOffset, mesh.extras.value("edm_render_index", -1) + 1);
    auto metadata = [&](Json value) {
        if (value.is_null())
            value = Json::object();
        require(value.is_object(), "Attachment metadata must be an object");
        remapMetadata(value, mapping, base.sourceNodes, renderOffset);
        if (value.contains("edm_attachment")) {
            int original = value.at("edm_attachment").get<int>();
            require(original >= 0 && original < int(child.attachments.size()),
                    "Invalid nested attachment metadata");
            value["edm_attachment"] = attachmentIndex + 1 + original;
        } else
            value["edm_attachment"] = attachmentIndex;
        return value;
    };
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(offset.block<3, 3>(0, 0),
                                          Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d u = svd.matrixU(), vt = svd.matrixV().transpose();
    V3 scale = svd.singularValues();
    if (u.determinant() < 0) {
        u.col(2) *= -1;
        scale[2] *= -1;
    }
    if (vt.determinant() < 0) {
        vt.row(2) *= -1;
        scale[2] *= -1;
    }
    Node root;
    root.name = pathString(child.source.stem()) + " / attachment";
    root.parent = targetConnector;
    root.t = offset.block<3, 1>(0, 3);
    root.q = quatMatrix(u);
    root.s = scale;
    root.extras = {{"edm_attachment", attachmentIndex}, {"edm_attachment_source", pathString(child.source)}};
    result->nodes.push_back(root);
    Node basis;
    basis.name = root.name + " / basis";
    basis.parent = nodeBegin;
    basis.q = quatMatrix(vt);
    basis.extras = {{"edm_attachment", attachmentIndex}};
    result->nodes.push_back(basis);
    for (auto node : child.nodes) {
        node.parent = node.parent < 0 ? nodeBegin + 1 : nodeOffset + node.parent;
        node.extras = metadata(std::move(node.extras));
        result->nodes.push_back(std::move(node));
    }
    for (auto material : child.materials) {
        if (material.source.empty())
            material.source = child.source;
        material.extras = metadata(std::move(material.extras));
        remapMetadata(material.uniforms, mapping, base.sourceNodes, renderOffset);
        remapMetadata(material.animatedUniforms, mapping, base.sourceNodes, renderOffset);
        result->materials.push_back(std::move(material));
    }
    for (auto mesh : child.meshes) {
        mesh.node += nodeOffset;
        mesh.material += materialOffset;
        for (auto& joint : mesh.skinNodes)
            joint += nodeOffset;
        // Vertex coordinates and inverse bind matrices stay in the child's model space.
        // Prefixing every child root transforms all skin joints by the same attachment matrix.
        for (auto& number : mesh.numbers) {
            if (number.u >= 0)
                number.u = mapping.at(number.u);
            if (number.v >= 0)
                number.v = mapping.at(number.v);
        }
        mesh.extras = metadata(std::move(mesh.extras));
        result->meshes.push_back(std::move(mesh));
    }
    for (auto track : child.tracks) {
        track.node += nodeOffset;
        track.arg = mapping.at(track.arg);
        result->tracks.push_back(std::move(track));
    }
    for (auto [argument, range] : child.limits)
        result->limits[mapping.at(argument)] = range;
    for (int argument : child.numberArgs)
        result->numberArgs.push_back(mapping.at(argument));
    std::sort(result->numberArgs.begin(), result->numberArgs.end());
    result->numberArgs.erase(std::unique(result->numberArgs.begin(), result->numberArgs.end()),
                             result->numberArgs.end());
    for (int index : child.heads)
        result->heads.push_back(index + nodeOffset);
    for (int index : child.tails)
        result->tails.push_back(index + nodeOffset);
    SceneAttachment attached;
    attached.source = child.source;
    attached.targetNode = targetConnector;
    attached.root = attached.nodeBegin = nodeBegin;
    attached.attachNode = attachPoint < 0 ? -1 : attachPoint + nodeOffset;
    attached.nodeCount = int(child.nodes.size()) + 2;
    attached.materialBegin = materialOffset;
    attached.materialCount = int(child.materials.size());
    attached.meshBegin = meshOffset;
    attached.meshCount = int(child.meshes.size());
    attached.argumentMap = mapping;
    attached.renderBegin = renderOffset;
    for (const auto& mesh : child.meshes)
        if (mesh.extras.is_object())
            attached.renderCount =
                std::max(attached.renderCount, mesh.extras.value("edm_render_index", -1) + 1);
    attached.sourceCollisions = child.collisionCount;
    attached.sourceRenderTypes = child.renderTypes.is_object() ? child.renderTypes : Json::object();
    for (const auto& nested : child.attachments) {
        attached.sourceCollisions -= nested.sourceCollisions;
        for (auto it = nested.sourceRenderTypes.begin(); it != nested.sourceRenderTypes.end(); ++it) {
            int remaining = attached.sourceRenderTypes.value(it.key(), 0) - it.value().get<int>();
            require(remaining >= 0, "Invalid nested render statistics");
            if (remaining)
                attached.sourceRenderTypes[it.key()] = remaining;
            else
                attached.sourceRenderTypes.erase(it.key());
        }
    }
    require(attached.sourceCollisions >= 0, "Invalid nested collision statistics");
    result->attachments.push_back(std::move(attached));
    for (auto nested : child.attachments) {
        nested.targetNode += nodeOffset;
        nested.root += nodeOffset;
        if (nested.attachNode >= 0)
            nested.attachNode += nodeOffset;
        nested.nodeBegin += nodeOffset;
        nested.materialBegin += materialOffset;
        nested.meshBegin += meshOffset;
        nested.renderBegin += renderOffset;
        for (auto& [original, remapped] : nested.argumentMap)
            remapped = mapping.at(remapped);
        result->attachments.push_back(std::move(nested));
    }
    result->sourceNodes += child.sourceNodes;
    result->connectorCount += child.connectorCount;
    result->collisionCount += child.collisionCount;
    if (!result->renderTypes.is_object())
        result->renderTypes = Json::object();
    if (child.renderTypes.is_object())
        for (auto it = child.renderTypes.begin(); it != child.renderTypes.end(); ++it)
            result->renderTypes[it.key()] = result->renderTypes.value(it.key(), 0) + it.value().get<int>();
    for (const auto& warning : child.warnings)
        result->warn(pathString(child.source.filename()) + ": " + warning);
    if (attachPoint < 0)
        result->warn(pathString(child.source.filename()) + " 没有 AttachPoint，使用模型原点连接。");
    rebuild(*result);
    return result;
}
DetachResult detachScene(const Scene& source, int targetConnector) {
    validateScene(source);
    require(targetConnector >= 0 && targetConnector < int(source.nodes.size()) &&
                isConnector(source.nodes[targetConnector]),
            "Detachment target must be a Connector node");
    DetachResult result;
    std::vector<bool> removedNodes(source.nodes.size()), removedAttachments(source.attachments.size());
    for (const auto& attachment : source.attachments) {
        require(attachment.root >= 0 && attachment.root < int(source.nodes.size()),
                "Invalid attachment root");
        if (attachment.targetNode == targetConnector)
            removedNodes[attachment.root] = true;
    }
    for (int index : source.order)
        if (source.nodes[index].parent >= 0 && removedNodes[source.nodes[index].parent])
            removedNodes[index] = true;
    for (size_t index = 0; index < source.attachments.size(); ++index) {
        removedAttachments[index] = removedNodes[source.attachments[index].root];
        if (removedAttachments[index]) {
            ++result.removedCount;
            for (auto [original, remapped] : source.attachments[index].argumentMap)
                result.removedArguments.insert(remapped);
        }
    }
    auto ownerRemoved = [&](const Json& metadata) {
        if (!metadata.is_object() || !metadata.contains("edm_attachment"))
            return false;
        const int owner = metadata.at("edm_attachment").get<int>();
        require(owner >= 0 && owner < int(removedAttachments.size()), "Invalid attachment owner");
        return bool(removedAttachments[owner]);
    };
    auto indexMap = [](size_t count, auto removed) {
        std::vector<int> mapping(count, -1);
        int next = 0;
        for (size_t index = 0; index < count; ++index)
            if (!removed(index))
                mapping[index] = next++;
        return mapping;
    };
    result.nodeMap = indexMap(source.nodes.size(), [&](size_t index) { return removedNodes[index]; });
    result.attachmentMap =
        indexMap(source.attachments.size(), [&](size_t index) { return removedAttachments[index]; });
    result.materialMap = indexMap(source.materials.size(),
                                  [&](size_t index) { return ownerRemoved(source.materials[index].extras); });
    result.meshMap = indexMap(source.meshes.size(), [&](size_t index) {
        const auto& mesh = source.meshes[index];
        return removedNodes[mesh.node] || ownerRemoved(mesh.extras);
    });
    require(source.heads.size() == source.tails.size(), "Invalid source node lookup");
    const auto rawNodes =
        indexMap(source.heads.size(), [&](size_t index) { return removedNodes.at(source.heads[index]); });
    int renderSize = 0;
    for (const auto& mesh : source.meshes)
        if (mesh.extras.is_object())
            renderSize = std::max(renderSize, mesh.extras.value("edm_render_index", -1) + 1);
    std::vector<bool> removedRenders(renderSize);
    for (size_t index = 0; index < source.attachments.size(); ++index)
        if (removedAttachments[index]) {
            const auto& attachment = source.attachments[index];
            require(attachment.renderBegin >= 0 && attachment.renderCount >= 0 &&
                        int64_t(attachment.renderBegin) + attachment.renderCount <= renderSize,
                    "Invalid attachment source render range");
            for (int render = attachment.renderBegin;
                 render < attachment.renderBegin + attachment.renderCount; ++render)
                removedRenders[render] = true;
        }
    const auto renderMap =
        indexMap(removedRenders.size(), [&](size_t index) { return removedRenders[index]; });
    auto mapped = [](const std::vector<int>& mapping, int oldIndex, bool negative = false) {
        if (negative && oldIndex < 0)
            return oldIndex;
        require(oldIndex >= 0 && oldIndex < int(mapping.size()) && mapping[oldIndex] >= 0,
                "Surviving object references a removed attachment");
        return mapping[oldIndex];
    };
    std::function<void(Json&)> metadata = [&](Json& value) {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key() == "edm_attachment")
                    it.value() = mapped(result.attachmentMap, it.value().get<int>());
                else if ((it.key() == "edm_node" || it.key() == "edm_parent") &&
                         it.value().is_number_integer())
                    it.value() = mapped(rawNodes, it.value().get<int>(), true);
                else if (it.key() == "edm_render_index" && it.value().is_number_integer())
                    it.value() = mapped(renderMap, it.value().get<int>(), true);
                else
                    metadata(it.value());
            }
        } else if (value.is_array())
            for (auto& child : value)
                metadata(child);
    };
    // Do not duplicate large meshes that are about to be removed. Only surviving
    // geometry is copied, while the currently displayed scene remains immutable.
    auto scene = std::make_shared<Scene>();
    scene->source = source.source;
    scene->version = source.version;
    scene->collisionCount = source.collisionCount;
    scene->renderTypes = source.renderTypes;
    scene->limits = source.limits;
    scene->defaultArgs = source.defaultArgs;
    scene->numberArgs = source.numberArgs;
    scene->warnings = source.warnings;
    scene->parseSeconds = source.parseSeconds;
    scene->buildSeconds = source.buildSeconds;
    for (size_t index = 0; index < source.nodes.size(); ++index)
        if (result.nodeMap[index] >= 0) {
            auto node = source.nodes[index];
            node.parent = mapped(result.nodeMap, node.parent, true);
            metadata(node.extras);
            scene->nodes.push_back(std::move(node));
        }
    for (size_t index = 0; index < source.materials.size(); ++index)
        if (result.materialMap[index] >= 0) {
            auto material = source.materials[index];
            metadata(material.extras);
            metadata(material.uniforms);
            metadata(material.animatedUniforms);
            scene->materials.push_back(std::move(material));
        }
    for (size_t index = 0; index < source.meshes.size(); ++index)
        if (result.meshMap[index] >= 0) {
            auto mesh = source.meshes[index];
            mesh.node = mapped(result.nodeMap, mesh.node);
            mesh.material = mapped(result.materialMap, mesh.material);
            for (auto& joint : mesh.skinNodes)
                joint = mapped(result.nodeMap, joint);
            metadata(mesh.extras);
            scene->meshes.push_back(std::move(mesh));
        }
    for (auto track : source.tracks)
        if (result.nodeMap[track.node] >= 0) {
            track.node = result.nodeMap[track.node];
            require(!result.removedArguments.contains(track.arg),
                    "A surviving track uses a removed argument");
            scene->tracks.push_back(std::move(track));
        }
    for (size_t index = 0; index < source.heads.size(); ++index)
        if (rawNodes[index] >= 0) {
            scene->heads.push_back(mapped(result.nodeMap, source.heads[index]));
            scene->tails.push_back(mapped(result.nodeMap, source.tails[index]));
        }
    auto range = [](const std::vector<int>& mapping, int begin, int count) {
        require(begin >= 0 && count >= 0 && int64_t(begin) + count <= int64_t(mapping.size()),
                "Invalid attachment range");
        const int newBegin = int(
            std::count_if(mapping.begin(), mapping.begin() + begin, [](int value) { return value >= 0; }));
        const int newCount = int(std::count_if(mapping.begin() + begin, mapping.begin() + begin + count,
                                               [](int value) { return value >= 0; }));
        return std::pair<int, int>{newBegin, newCount};
    };
    for (size_t index = 0; index < source.attachments.size(); ++index) {
        const auto& original = source.attachments[index];
        if (removedAttachments[index]) {
            scene->collisionCount -= original.sourceCollisions;
            for (auto it = original.sourceRenderTypes.begin(); it != original.sourceRenderTypes.end(); ++it) {
                int remaining = scene->renderTypes.value(it.key(), 0) - it.value().get<int>();
                require(remaining >= 0, "Invalid remaining render statistics");
                if (remaining)
                    scene->renderTypes[it.key()] = remaining;
                else
                    scene->renderTypes.erase(it.key());
            }
            continue;
        }
        auto attachment = original;
        attachment.targetNode = mapped(result.nodeMap, attachment.targetNode);
        attachment.root = mapped(result.nodeMap, attachment.root);
        attachment.attachNode = mapped(result.nodeMap, attachment.attachNode, true);
        std::tie(attachment.nodeBegin, attachment.nodeCount) =
            range(result.nodeMap, attachment.nodeBegin, attachment.nodeCount);
        std::tie(attachment.materialBegin, attachment.materialCount) =
            range(result.materialMap, attachment.materialBegin, attachment.materialCount);
        std::tie(attachment.meshBegin, attachment.meshCount) =
            range(result.meshMap, attachment.meshBegin, attachment.meshCount);
        std::tie(attachment.renderBegin, attachment.renderCount) =
            range(renderMap, attachment.renderBegin, attachment.renderCount);
        std::erase_if(attachment.argumentMap,
                      [&](const auto& value) { return result.removedArguments.contains(value.second); });
        scene->attachments.push_back(std::move(attachment));
    }
    for (int argument : result.removedArguments) {
        scene->limits.erase(argument);
        scene->defaultArgs.erase(argument);
    }
    std::erase_if(scene->numberArgs,
                  [&](int argument) { return result.removedArguments.contains(argument); });
    scene->sourceNodes = int(scene->heads.size());
    scene->connectorCount = int(std::count_if(scene->nodes.begin(), scene->nodes.end(), isConnector));
    require(scene->collisionCount >= 0, "Invalid remaining collision statistics");
    rebuild(*scene);
    result.scene = std::move(scene);
    return result;
}
} // namespace edm
