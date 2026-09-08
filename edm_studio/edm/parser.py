"""
High-level parser for the EDM container format.

This module reads a sequential EDM stream. Unknown layouts, invalid lengths,
invalid references and trailing bytes are errors. No byte-pattern resync is used.

The dispatch table maps EDM type strings (``"model::RenderNode"``) to
private factory methods. Adding support for a new node type is simply a
matter of writing the reader and registering it in
:meth:`EDMFileParser._build_dispatch`.

References
----------
  * EDM Specification: https://ndevenish.github.io/Blender_ioEDM/EDM_Specification.html
  * Reference implementation: https://github.com/ndevenish/Blender_ioEDM
"""

from __future__ import annotations

import math
import os
import struct
import numpy as np
from collections import OrderedDict
from typing import Any, Callable, Dict, List, Optional, Tuple

from .reader import BinaryReader, EDM_STRING_ENCODING
from . import types as t


# ---------------------------------------------------------------------------
#  Public entry point
# ---------------------------------------------------------------------------


def parse_edm(filepath: str) -> t.ParsedEDM:
    """Convenience wrapper: open ``filepath`` and return a :class:`ParsedEDM`."""
    with open(filepath, "rb") as fh:
        return EDMFileParser(fh).parse()


# ---------------------------------------------------------------------------
#  Parser
# ---------------------------------------------------------------------------


class EDMParseError(Exception):
    """Raised when the file is not a valid / supported EDM file."""


class EDMFileParser:
    """Stateful parser for one EDM file.

    Use :meth:`parse` once per parser instance. The parser does NOT close
    the underlying file object — that is the caller's responsibility.
    """

    SUPPORTED_VERSIONS = (8, 10)

    def __init__(self, fileobj):
        self.r = BinaryReader(fileobj)
        self._dispatch: Dict[str, Callable[[], Any]] = self._build_dispatch()

    # ------------------------------------------------------------------ main
    def parse(self) -> t.ParsedEDM:
        magic = self.r.read_raw(3)
        if magic != b"EDM":
            raise EDMParseError(f"Not an EDM file (magic={magic!r})")

        version = self.r.ushort()
        if version not in self.SUPPORTED_VERSIONS:
            raise EDMParseError(
                f"Unsupported EDM version {version} "
                f"(supported: {self.SUPPORTED_VERSIONS})"
            )
        self.r.version = version

        if version == 10:
            self._read_string_lookup_table()

        # Two index maps, used by DCS as cross-checks. We don't need them
        # for import, but we must consume them to advance the file cursor.
        self.index_a = self._read_index_map()
        self.index_b = self._read_index_map()

        # The first named type in the file is always the RootNode.
        root = self._read_named_type()
        if not isinstance(root, t.RootNode):
            raise EDMParseError(
                f"Expected RootNode as first named type, got {type(root).__name__}"
            )

        # All transform / animation nodes in a flat list.
        node_count = self.r.uint()
        nodes: List[t._NodeBase] = []
        for _ in range(node_count):
            try:
                nodes.append(self._read_named_type())
            except Exception as exc:  # pragma: no cover - defensive
                raise EDMParseError(
                    f"Failed to read scene node #{len(nodes)}: {exc}"
                ) from exc

        # The "rather opaque block" of parent indices, one int per node.
        if nodes:
            parents = self.r.ints(len(nodes))
            for node, p_idx in zip(nodes, parents):
                node.parent_idx = p_idx

        # World-placed render items, organised into named buckets.
        render_items = self._read_render_items()

        if self.r.remaining():
            raise EDMParseError(f"Unconsumed data at byte {self.r.tell()}: {self.r.remaining()} bytes")
        for i, n in enumerate(nodes):
            if n.parent_idx < -1 or n.parent_idx >= len(nodes) or n.parent_idx == i:
                raise EDMParseError(f"Invalid parent {n.parent_idx} on scene node {i}")
        for i in range(len(nodes)):
            seen = set()
            j = i
            while j >= 0:
                if j in seen:
                    raise EDMParseError(f"Parent cycle at scene node {j}")
                seen.add(j)
                j = nodes[j].parent_idx

        return t.ParsedEDM(
            version=version,
            root=root,
            nodes=nodes,
            render_nodes=render_items.get("RENDER_NODES", []),
            shell_nodes=render_items.get("SHELL_NODES", []),
            light_nodes=render_items.get("LIGHT_NODES", []),
            connectors=render_items.get("CONNECTORS", []),
            extra_render_items={
                k: v for k, v in render_items.items()
                if k not in ("RENDER_NODES", "SHELL_NODES", "LIGHT_NODES", "CONNECTORS")
            },
        )

    # --------------------------------------------------------------- helpers
    def _read_string_lookup_table(self) -> None:
        """v10 only: read the global null-separated string table."""
        size = self.r.uint()
        blob = self.r.read_raw(size)
        # The blob is one big block of null-terminated strings.
        # split() will produce one trailing empty string if the blob ends
        # in a null byte; that's fine because indices match.
        strings = [
            chunk.decode(EDM_STRING_ENCODING, errors="replace")
            for chunk in blob.split(b"\x00")
        ]
        self.r.install_string_table(strings)

    def _read_index_map(self) -> "OrderedDict[str, int]":
        count = self.r.uint()
        out: "OrderedDict[str, int]" = OrderedDict()
        for _ in range(count):
            key = self.r.string()
            value = self.r.uint()
            out[key] = value
        return out

    def _read_render_items(self) -> Dict[str, List[Any]]:
        cat_count = self.r.uint()
        items: Dict[str, List[Any]] = {}
        for _ in range(cat_count):
            cat_name = self.r.string()
            bucket: List[Any] = []
            items[cat_name] = bucket
            # Sequential typed parsing only. A resync match in raw vertex bytes
            # is not evidence of a valid node, and must never produce a success.
            self._read_named_list_into(bucket)
            if any(not isinstance(n, t._NodeBase) for n in bucket):
                raise EDMParseError(f"Non-node record in {cat_name}")
        return items

    def _read_named_list(self) -> List[Any]:
        out: List[Any] = []
        self._read_named_list_into(out)
        return out

    def _read_named_list_into(self, out: List[Any]) -> None:
        count = self.r.uint()
        for _ in range(count):
            out.append(self._read_named_type())

    def _read_named_type(self) -> Any:
        type_name = self.r.string()
        handler = self._dispatch.get(type_name)
        if handler is None:
            raise EDMParseError(
                f"Unknown named type {type_name!r} at byte {self.r.tell()}"
            )
        return handler()

    # --------------------------------------------------- dispatch table -----
    def _build_dispatch(self) -> Dict[str, Callable[[], Any]]:
        d = self._dispatch_table_factory
        return {
            # Properties (used in PropertiesSet / animated uniforms)
            "model::Property<float>":           d(self._prop_float),
            "model::Property<unsigned int>":    d(self._prop_uint),
            "model::Property<osg::Vec2f>":      d(self._prop_vec2f),
            "model::Property<osg::Vec3f>":      d(self._prop_vec3f),
            "model::Property<osg::Vec4f>":      d(self._prop_vec4f),
            "model::Property<const char*>":     d(self._prop_string),
            "model::AnimatedProperty<float>":   d(self._anim_prop_float),
            "model::AnimatedProperty<osg::Vec2f>": d(self._anim_prop_vec2f),
            "model::AnimatedProperty<osg::Vec3f>": d(self._anim_prop_vec3f),
            "model::ArgumentProperty":          d(self._argument_property),
            # Keys (sometimes appear as named types in render_items maps)
            "model::Key<key::FLOAT>":           d(self._key_float),
            "model::Key<key::VEC2F>":           d(self._key_vec2f),
            "model::Key<key::VEC3F>":           d(self._key_vec3f),
            "model::Key<key::ROTATION>":        d(self._key_rotation),
            "model::Key<key::POSITION>":        d(self._key_position),
            "model::Key<key::SCALE>":           d(self._key_scale),
            # Scene nodes
            "model::RootNode":                  d(self._root_node),
            "model::Node":                      d(self._plain_node),
            "model::TransformNode":             d(self._transform_node),
            "model::Bone":                      d(self._bone),
            "model::LodNode":                   d(self._lod_node),
            "model::BillboardNode":             d(self._billboard_node),
            "model::NumberNode":                d(self._number_node),
            # Animated nodes — same payload, distinguished by type tag
            "model::ArgAnimationNode": lambda: self._arg_anim_node(t.NodeType.ARG_ANIMATION),
            "model::ArgRotationNode":  lambda: self._arg_anim_node(t.NodeType.ARG_ROTATION),
            "model::ArgPositionNode":  lambda: self._arg_anim_node(t.NodeType.ARG_POSITION),
            "model::ArgScaleNode":     lambda: self._arg_anim_node(t.NodeType.ARG_SCALE),
            "model::ArgAnimatedBone":  d(self._arg_animated_bone),
            "model::ArgVisibilityNode": d(self._arg_visibility_node),
            # Geometry / render items
            "model::RenderNode":        d(self._render_node),
            "model::SkinNode":          d(self._skin_node),
            "model::ShellNode":         d(self._shell_node),
            "model::SegmentsNode":      d(self._segments_node),
            # Lights / FX
            "model::LightNode":         d(self._light_node),
            "model::FakeOmniLightsNode": lambda: self._fake_lights(t.NodeType.FAKE_OMNI),
            "model::FakeSpotLightsNode": lambda: self._fake_spot_lights(),
            "model::FakeALSNode":       lambda: self._fake_als_node(),
            # Attachments
            "model::Connector":         d(self._connector),
        }

    @staticmethod
    def _dispatch_table_factory(fn):
        """Identity wrapper kept so the table reads neatly aligned."""
        return fn

    # ============================================================== nodes ==
    def _read_base_node(self):
        """Return (name, version, props) shared by every model::Node-derived type."""
        name = self.r.uint_string()
        version = self.r.uint()
        props = self._read_properties_set()
        return name, version, props

    def _read_properties_set(self) -> Dict[str, Any]:
        count = self.r.uint()
        out: "OrderedDict[str, Any]" = OrderedDict()
        for _ in range(count):
            prop = self._read_named_type()
            if isinstance(prop, t.AnimatedProperty):
                out[prop.name] = prop  # keep full object for animated props
            elif isinstance(prop, t.ArgumentProperty):
                out[prop.name] = prop
            elif isinstance(prop, t.Property):
                out[prop.name] = prop.value
            else:
                # Defensive fallback
                out[getattr(prop, "name", "?")] = prop
        return out

    # ------------------------------------------------------- properties ----
    def _prop_float(self) -> t.Property:
        return t.Property(self.r.string(), self.r.float32())

    def _prop_uint(self) -> t.Property:
        return t.Property(self.r.string(), self.r.uint())

    def _prop_vec2f(self) -> t.Property:
        return t.Property(self.r.string(), self.r.floats(2))

    def _prop_vec3f(self) -> t.Property:
        return t.Property(self.r.string(), self.r.floats(3))

    def _prop_vec4f(self) -> t.Property:
        return t.Property(self.r.string(), self.r.floats(4))

    def _prop_string(self) -> t.Property:
        return t.Property(self.r.string(), self.r.string())

    def _read_keyframe_list(self, value_reader) -> List[t.AnimatedKey]:
        return [
            t.AnimatedKey(self.r.double(), value_reader())
            for _ in range(self.r.uint())
        ]

    def _anim_prop_float(self) -> t.AnimatedProperty:
        name = self.r.string()
        arg = self.r.uint()
        keys = self._read_keyframe_list(self.r.float32)
        return t.AnimatedProperty(name=name, argument=arg, keys=keys)

    def _anim_prop_vec2f(self) -> t.AnimatedProperty:
        name = self.r.string()
        arg = self.r.uint()
        keys = self._read_keyframe_list(lambda: self.r.floats(2))
        return t.AnimatedProperty(name=name, argument=arg, keys=keys)

    def _anim_prop_vec3f(self) -> t.AnimatedProperty:
        name = self.r.string()
        arg = self.r.uint()
        keys = self._read_keyframe_list(lambda: self.r.floats(3))
        return t.AnimatedProperty(name=name, argument=arg, keys=keys)

    def _argument_property(self) -> t.ArgumentProperty:
        return t.ArgumentProperty(name=self.r.string(), argument=self.r.uint())

    # ------------------------------------------------------- key types -----
    def _key_float(self):
        return t.AnimatedKey(self.r.double(), self.r.float32())

    def _key_vec2f(self):
        return t.AnimatedKey(self.r.double(), self.r.floats(2))

    def _key_vec3f(self):
        return t.AnimatedKey(self.r.double(), self.r.floats(3))

    def _key_rotation(self):
        return t.AnimatedKey(self.r.double(), self.r.quaternion_xyzw())

    def _key_position(self):
        return t.AnimatedKey(self.r.double(), self.r.vec3d())

    def _key_scale(self):
        return t.AnimatedKey(self.r.double(), self.r.doubles(4))

    # ------------------------------------------------------- scene nodes ---
    def _root_node(self) -> t.RootNode:
        name, version, props = self._read_base_node()
        # In v8 there is one extra opaque uchar; v10 omits it.
        if self.r.version == 8:
            self.r.uchar()
        bbox_min = self.r.vec3d()
        bbox_max = self.r.vec3d()
        # Four extra Vec3d's of unknown meaning
        for _ in range(4):
            self.r.vec3d()
        materials = [self._read_material() for _ in range(self.r.uint())]
        # Trailing pair of unknown uints
        self.r.uints(2)
        return t.RootNode(
            type=t.NodeType.ROOT.value,
            name=name,
            version=version,
            props=props,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            materials=materials,
        )

    def _plain_node(self) -> t._NodeBase:
        name, version, props = self._read_base_node()
        return t._NodeBase(
            type=t.NodeType.NODE.value,
            name=name,
            version=version,
            props=props,
        )

    def _transform_node(self) -> t.TransformNode:
        name, version, props = self._read_base_node()
        matrix = self.r.matrixd()
        return t.TransformNode(
            type=t.NodeType.TRANSFORM.value,
            name=name,
            version=version,
            props=props,
            matrix=matrix,
        )

    def _bone(self) -> t.BoneNode:
        name, version, props = self._read_base_node()
        m1 = self.r.matrixd()
        m2 = self.r.matrixd()
        return t.BoneNode(
            type=t.NodeType.BONE.value,
            name=name,
            version=version,
            props=props,
            matrix1=m1,
            matrix2=m2,
        )

    def _lod_node(self) -> t.LodNode:
        name, version, props = self._read_base_node()
        count = self.r.uint()
        levels: List[t.LodLevel] = []
        for _ in range(count):
            start_sq, end_sq = self.r.doubles(2)
            levels.append(
                t.LodLevel(
                    distance_min=math.sqrt(max(0.0, start_sq)),
                    distance_max=math.sqrt(max(0.0, end_sq)),
                )
            )
        return t.LodNode(
            type=t.NodeType.LOD.value,
            name=name,
            version=version,
            props=props,
            levels=levels,
        )

    def _billboard_node(self) -> t.BillboardNode:
        name, version, props = self._read_base_node()
        # The 154 opaque bytes documented in the spec.
        raw = self.r.read_raw(154)
        return t.BillboardNode(
            type=t.NodeType.BILLBOARD.value,
            name=name,
            version=version,
            props=props,
            raw=raw,
        )

    def _number_node(self) -> t.NumberNode:
        # Verified on F-16C and FA-18C: a complete RenderNode followed by
        # count * {int32, float32, uint32, float32}. Preserve the unknown
        # atlas/number controls verbatim; the geometry must not be skipped.
        node = self._render_node()
        node.type = t.NodeType.NUMBER.value
        count = self.r.uint()
        if count > self.r.remaining() // 16:
            raise EDMParseError("Invalid NumberNode control count")
        node.props['number_controls'] = [
            (self.r.int32(), self.r.float32(), self.r.uint(), self.r.float32())
            for _ in range(count)
        ]
        return node

    # ------------------------------------------- arg-animation nodes -------
    def _read_arg_anim_base(self) -> t.ArgAnimationBase:
        return t.ArgAnimationBase(
            matrix=self.r.matrixd(),
            position=self.r.vec3d(),
            quat1=self.r.quaternion_xyzw(),
            quat2=self.r.quaternion_xyzw(),
            scale=self.r.vec3d(),
        )

    def _read_pos_arg_entry(self):
        arg = self.r.uint()
        count = self.r.uint()
        keys = [t.AnimatedKey(self.r.double(), self.r.doubles(3)) for _ in range(count)]
        return arg, keys

    def _read_rot_arg_entry(self):
        arg = self.r.uint()
        count = self.r.uint()
        keys = [t.AnimatedKey(self.r.double(), self.r.quaternion_xyzw()) for _ in range(count)]
        return arg, keys

    def _read_scale_arg_entry(self):
        arg = self.r.uint()
        count_a = self.r.uint()
        keys_a = [t.AnimatedKey(self.r.double(), self.r.doubles(4)) for _ in range(count_a)]
        count_b = self.r.uint()
        keys_b = [t.AnimatedKey(self.r.double(), self.r.doubles(3)) for _ in range(count_b)]
        return arg, (keys_a, keys_b)

    def _arg_anim_node(self, node_type: t.NodeType) -> t.ArgAnimationNode:
        name, version, props = self._read_base_node()
        base = self._read_arg_anim_base()
        pos_count = self.r.uint()
        pos_data = [self._read_pos_arg_entry() for _ in range(pos_count)]
        rot_count = self.r.uint()
        rot_data = [self._read_rot_arg_entry() for _ in range(rot_count)]
        scale_count = self.r.uint()
        scale_data = [self._read_scale_arg_entry() for _ in range(scale_count)]
        return t.ArgAnimationNode(
            type=node_type.value,
            name=name,
            version=version,
            props=props,
            base=base,
            pos_data=pos_data,
            rot_data=rot_data,
            scale_data=scale_data,
        )

    def _arg_animated_bone(self) -> t.ArgAnimationNode:
        node = self._arg_anim_node(t.NodeType.ARG_ANIMATED_BONE)
        node.bone_transform = self.r.matrixd()
        return node

    def _arg_visibility_node(self) -> t.ArgVisibilityNode:
        name, version, props = self._read_base_node()
        layout = props.get('__VERSION__', 0)
        if layout not in (0, 1):
            raise EDMParseError(f"Unsupported ArgVisibilityNode layout {layout!r}")

        def _vis_arg():
            arg = self.r.uint()
            count = self.r.uint()
            ranges = [tuple(self.r.doubles(2)) for _ in range(count)]
            return arg, ranges  # type: ignore[return-value]

        count = self.r.uint()
        vis_data = [_vis_arg() for _ in range(count)]
        # C-130 uses layout 1: a complete 16-float matrix follows the ranges.
        # Select by the explicit property, never by searching for a next node.
        trailing_matrix = self.r.matrixf() if layout == 1 else ()
        return t.ArgVisibilityNode(
            type=t.NodeType.ARG_VISIBILITY.value,
            name=name,
            version=version,
            props=props,
            vis_data=vis_data,
            trailing_matrix=trailing_matrix,
        )

    # ------------------------------------------------ render-item helpers --
    def _read_parent_data(self) -> List[t.ParentEntry]:
        count = self.r.uint()
        if count == 1:
            parent = self.r.uint()
            damage_arg = self.r.int32()
            return [t.ParentEntry(node=parent, index_start=0, damage_arg=damage_arg)]
        entries: List[t.ParentEntry] = []
        for _ in range(count):
            node = self.r.uint()
            idx_start, damage_arg = self.r.ints(2)
            entries.append(t.ParentEntry(node=node, index_start=idx_start, damage_arg=damage_arg))
        return entries

    def _read_vertex_data(self) -> List[tuple]:
        count = self.r.uint()
        stride = self.r.uint()
        if stride > 128 or (count and stride < 3):
            raise EDMParseError(f"Invalid vertex stride {stride}")
        # Preserve packed JOINTS bytes in position.w, including NaN bit
        # patterns. Converting through Python float corrupts those patterns.
        raw = self.r.read_raw(count * stride * 4)
        return np.frombuffer(raw, dtype='<f4').reshape(count, stride)

    def _read_index_data(self) -> List[int]:
        data_type = self.r.uchar()
        entries = self.r.uint()
        # Spec says this is "0, 1 or 5" — most commonly 5; we don't use it.
        self.r.uint()
        if entries == 0:
            return []
        if data_type == 0:
            return list(self.r.uchars(entries))
        if data_type == 1:
            return list(struct.unpack(f"<{entries}H", self.r.read_raw(2 * entries)))
        if data_type == 2:
            return list(self.r.uints(entries))
        raise EDMParseError(f"Unknown index data_type {data_type} at byte {self.r.tell()}")

    def _read_vertex_format(self) -> t.VertexFormat:
        count = self.r.uint()
        return t.VertexFormat(channels=tuple(self.r.uchars(count)))

    # --------------------------------------------------------- geometry ----
    def _render_node(self) -> t.RenderNode:
        name, version, props = self._read_base_node()
        self.r.uint()  # unknown — always 0 in known files
        material_id = self.r.uint()
        parents = self._read_parent_data()
        vertex_data = self._read_vertex_data()
        index_data = self._read_index_data()
        return t.RenderNode(
            type=t.NodeType.RENDER.value,
            name=name,
            version=version,
            props=props,
            material_id=material_id,
            parents=parents,
            vertex_data=vertex_data,
            index_data=index_data,
        )

    def _skin_node(self) -> t.SkinNode:
        name, version, props = self._read_base_node()
        self.r.uint()  # unknown
        material_id = self.r.uint()
        bone_count = self.r.uint()
        bones = list(self.r.uints(bone_count))
        self.r.uint()  # post-bone unknown
        vertex_data = self._read_vertex_data()
        index_data = self._read_index_data()
        return t.SkinNode(
            type=t.NodeType.SKIN.value,
            name=name,
            version=version,
            props=props,
            material_id=material_id,
            bones=bones,
            vertex_data=vertex_data,
            index_data=index_data,
        )

    def _shell_node(self) -> t.ShellNode:
        name, version, props = self._read_base_node()
        parent = self.r.uint()
        vertex_format = self._read_vertex_format()
        vertex_data = self._read_vertex_data()
        index_data = self._read_index_data()
        return t.ShellNode(
            type=t.NodeType.SHELL.value,
            name=name,
            version=version,
            props=props,
            parent=parent,
            vertex_format=vertex_format,
            vertex_data=vertex_data,
            index_data=index_data,
        )

    def _segments_node(self) -> t.SegmentsNode:
        name, version, props = self._read_base_node()
        self.r.uint()  # unknown
        count = self.r.uint()
        segments = [tuple(self.r.floats(6)) for _ in range(count)]
        return t.SegmentsNode(
            type=t.NodeType.SEGMENTS.value,
            name=name,
            version=version,
            props=props,
            segments=segments,
        )

    # ---------------------------------------------------------- lights -----
    def _light_node(self) -> t.LightNode:
        name, version, props = self._read_base_node()
        parent = self.r.uint()
        self.r.uchar()  # unknownB
        light_props = self._read_properties_set()
        self.r.uchar()  # unknownC
        return t.LightNode(
            type=t.NodeType.LIGHT.value,
            name=name,
            version=version,
            props=props,
            parent=parent,
            light_props=light_props,
        )

    def _fake_lights(self, node_type: t.NodeType) -> t.FakeLightsNode:
        name, version, props = self._read_base_node()
        # Skip opaque omni light data per spec.
        self.r.uints(5)
        count = self.r.uint()
        for _ in range(count):
            self.r.doubles(6)
        return t.FakeLightsNode(
            type=node_type.value,
            name=name,
            version=version,
            props=props,
        )

    def _fake_spot_lights(self) -> t.FakeLightsNode:
        name, version, props = self._read_base_node()
        layout = props.get('__VERSION__', 0)
        # FA-18 uses layout 1 (65 bytes). F-100D layout 2 appends a
        # three-float direction to each light, including multi-light nodes.
        record_size = {0: 65, 1: 65, 2: 77}.get(layout)
        if record_size is None:
            raise EDMParseError(f"Unsupported FakeSpotLightsNode layout {layout!r}")
        self.r.uint()  # unknown_start
        self.r.uint()  # material_ish
        ctrl_count = self.r.uint()
        for _ in range(ctrl_count):
            self.r.uint()       # nodeId
            self.r.uint()       # unknownA
            self.r.floats(3)    # unknownB
        data_count = self.r.uint()
        self.r.read_raw(data_count * record_size)
        return t.FakeLightsNode(
            type=t.NodeType.FAKE_SPOT.value,
            name=name,
            version=version,
            props=props,
        )

    def _fake_als_node(self) -> t.FakeLightsNode:
        name, version, props = self._read_base_node()
        self.r.uints(3)
        count = self.r.uint()
        for _ in range(count):
            self.r.read_raw(80)
        return t.FakeLightsNode(
            type=t.NodeType.FAKE_ALS.value,
            name=name,
            version=version,
            props=props,
        )

    # ------------------------------------------------------- attachments ---
    def _connector(self) -> t.Connector:
        name, version, props = self._read_base_node()
        parent = self.r.uint()
        self.r.uint()  # unknown
        return t.Connector(
            type=t.NodeType.CONNECTOR.value,
            name=name,
            version=version,
            props=props,
            parent=parent,
        )

    # --------------------------------------------------------- material ----
    def _read_material(self) -> t.Material:
        mat = t.Material()
        count = self.r.uint()
        for _ in range(count):
            key = self.r.string()
            self._apply_material_key(mat, key)
        return mat

    def _apply_material_key(self, mat: t.Material, key: str) -> None:
        if key == "BLENDING":
            mat.blending = self.r.uchar()
        elif key == "CULLING":
            mat.culling = self.r.uchar()
        elif key == "DEPTH_BIAS":
            mat.depth_bias = self.r.uint()
        elif key == "VERTEX_FORMAT":
            mat.vertex_format = self._read_vertex_format()
        elif key == "TEXTURE_COORDINATES_CHANNELS":
            count = self.r.uint()
            mat.texture_coordinates_channels = tuple(self.r.uints(count))
        elif key == "MATERIAL_NAME":
            mat.material_name = self.r.string()
        elif key == "NAME":
            mat.name = self.r.string()
        elif key == "SHADOWS":
            mat.shadows = self.r.uchar()
        elif key == "TEXTURES":
            count = self.r.uint()
            mat.textures = [self._read_texture() for _ in range(count)]
        elif key == "UNIFORMS":
            mat.uniforms = self._read_properties_set()
        elif key == "ANIMATED_UNIFORMS":
            mat.animated_uniforms = self._read_properties_set()
        elif key in ("DECAL", "FLAT_COLOR_RENDERING", "HAS_ALPHA_CHANNEL",
                     "DAMAGE_REQUIRED", "NIGHT_LIGHTING_ALPHA", "LIGHT_MAP"):
            setattr(mat, key.lower(), self.r.uchar())
        elif key in ("DAMAGE_TEXTURE_OFFSET", "Z_OFFSET"):
            setattr(mat, key.lower(), self.r.float32())
        else:
            raise EDMParseError(
                f"Unknown material key {key!r} at byte {self.r.tell()}"
            )

    def _read_texture(self) -> t.Texture:
        index = self.r.uint()
        sentinel = self.r.int32()
        if sentinel != -1:
            print(
                f"[EDM] Warning: texture sentinel={sentinel} (expected -1) "
                f"at byte {self.r.tell()}"
            )
        name = self.r.string()
        # Always [2, 2, 10, 6] per spec
        self.r.uints(4)
        matrix = self.r.matrixf()
        return t.Texture(index=index, name=name, matrix=matrix)
