"""
Typed data classes for the parsed EDM scene.

The parser populates instances of these dataclasses; downstream Blender
code accesses them by attribute. Using explicit dataclasses (instead of
`SimpleNamespace`) gives us:

  * IDE autocompletion / type checking
  * Default values (so optional fields don't raise AttributeError)
  * A documented field list per node type

All numeric tuples are kept as Python tuples / lists; conversion to
``mathutils`` objects happens in the `blender` package so that this
sub-package stays free of Blender dependencies.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple


# Convenient aliases for documentation purposes
Vec3d = Tuple[float, float, float]
Vec4f = Tuple[float, float, float, float]
QuatWXYZ = Tuple[float, float, float, float]
Matrix4x4 = Tuple[float, ...]   # 16 entries, column-major


class NodeType(str, Enum):
    """Canonical scene-node type tags used internally by the importer.

    These mirror the EDM ``model::*`` class names but in a Pythonic form.
    """
    NODE = "Node"
    ROOT = "RootNode"
    TRANSFORM = "TransformNode"
    BONE = "Bone"
    LOD = "LodNode"
    BILLBOARD = "BillboardNode"
    NUMBER = "NumberNode"
    ARG_ANIMATION = "ArgAnimationNode"
    ARG_POSITION = "ArgPositionNode"
    ARG_ROTATION = "ArgRotationNode"
    ARG_SCALE = "ArgScaleNode"
    ARG_ANIMATED_BONE = "ArgAnimatedBone"
    ARG_VISIBILITY = "ArgVisibilityNode"
    RENDER = "RenderNode"
    SKIN = "SkinNode"
    SHELL = "ShellNode"
    SEGMENTS = "SegmentsNode"
    LIGHT = "LightNode"
    FAKE_OMNI = "FakeOmniLightsNode"
    FAKE_SPOT = "FakeSpotLightsNode"
    FAKE_ALS = "FakeALSNode"
    CONNECTOR = "Connector"


# Node types that animate via a DCS argument (driven by aircraft state).
ANIMATING_NODE_TYPES = frozenset(
    {
        NodeType.ARG_ANIMATION.value,
        NodeType.ARG_POSITION.value,
        NodeType.ARG_ROTATION.value,
        NodeType.ARG_SCALE.value,
        NodeType.ARG_ANIMATED_BONE.value,
        NodeType.ARG_VISIBILITY.value,
    }
)

# Node types that have a static pose only.
STATIC_TRANSFORM_NODE_TYPES = frozenset(
    {
        NodeType.TRANSFORM.value,
        NodeType.BONE.value,
        NodeType.LOD.value,
        NodeType.NODE.value,
        NodeType.BILLBOARD.value,
        NodeType.NUMBER.value,
    }
)


# ---------------------------------------------------------------------------
#  Properties
# ---------------------------------------------------------------------------


@dataclass
class Property:
    """A non-animated material/scene property."""
    name: str
    value: Any


@dataclass
class AnimatedKey:
    """Single keyframe inside an AnimatedProperty."""
    frame: float
    value: Any


@dataclass
class AnimatedProperty:
    """Material/scene property that varies with a DCS animation argument."""
    name: str
    argument: int
    keys: List[AnimatedKey] = field(default_factory=list)


@dataclass
class ArgumentProperty:
    """Property whose value is implied by the DCS argument index itself."""
    name: str
    argument: int


# ---------------------------------------------------------------------------
#  Vertex format
# ---------------------------------------------------------------------------


@dataclass
class VertexFormat:
    """Layout of one vertex inside a render node's float blob.

    The ``channels`` array is a list of channel sizes (in floats). Each
    channel index has a fixed semantic meaning per the EDM spec:

      * 0 -> position (typically 4 floats; the 4th is the parent index)
      * 1 -> normal   (3 floats)
      * 4 -> UV0      (2 floats)
      * 21 -> bone weights (4 floats, paired with bone indices)
      * etc.
    """
    channels: Tuple[int, ...]

    # ---------------- helpers -------------------------------------------------
    @property
    def stride(self) -> int:
        return sum(self.channels)

    def offset_of(self, channel: int) -> int:
        """Return the float offset of a channel within a vertex, or -1."""
        if channel < 0 or channel >= len(self.channels):
            return -1
        if self.channels[channel] == 0:
            return -1
        return sum(self.channels[:channel])

    def size_of(self, channel: int) -> int:
        if channel < 0 or channel >= len(self.channels):
            return 0
        return self.channels[channel]

    # Channel constants (per EDM spec)
    POSITION = 0
    NORMAL = 1
    UV0 = 4
    UV1 = 5
    BONE_WEIGHTS = 21


# ---------------------------------------------------------------------------
#  Material / texture
# ---------------------------------------------------------------------------


@dataclass
class Texture:
    """A texture reference inside a Material."""
    index: int                    # role (0=diffuse, 1=normal, 2=spec, ...)
    name: str                     # filename without extension
    matrix: Tuple[float, ...] = ()  # rarely non-identity texture matrix


@dataclass
class Material:
    """Resolved EDM material record."""
    name: str = ""
    material_name: str = ""        # base shader id (e.g. 'def_material')
    blending: int = 0
    culling: int = 0
    depth_bias: int = 0
    shadows: int = 0
    decal: int = 0
    flat_color_rendering: int = 0
    has_alpha_channel: int = 0
    damage_required: int = 0
    night_lighting_alpha: int = 0
    light_map: int = 0
    damage_texture_offset: float = 0.0
    z_offset: float = 0.0
    vertex_format: Optional[VertexFormat] = None
    texture_coordinates_channels: Tuple[int, ...] = ()
    textures: List[Texture] = field(default_factory=list)
    uniforms: Dict[str, Any] = field(default_factory=dict)
    animated_uniforms: Dict[str, Any] = field(default_factory=dict)

    def texture_by_index(self, index: int) -> Optional[Texture]:
        for t in self.textures:
            if t.index == index:
                return t
        return None


# ---------------------------------------------------------------------------
#  Scene nodes
# ---------------------------------------------------------------------------


@dataclass
class _NodeBase:
    """Shared header on every scene-graph node."""
    type: str
    name: str
    version: int
    props: Dict[str, Any] = field(default_factory=dict)
    parent_idx: int = -1


@dataclass
class RootNode(_NodeBase):
    bbox_min: Vec3d = (0.0, 0.0, 0.0)
    bbox_max: Vec3d = (0.0, 0.0, 0.0)
    materials: List[Material] = field(default_factory=list)


@dataclass
class TransformNode(_NodeBase):
    matrix: Matrix4x4 = ()


@dataclass
class BoneNode(_NodeBase):
    matrix1: Matrix4x4 = ()
    matrix2: Matrix4x4 = ()


@dataclass
class LodLevel:
    distance_min: float
    distance_max: float


@dataclass
class LodNode(_NodeBase):
    levels: List[LodLevel] = field(default_factory=list)


@dataclass
class BillboardNode(_NodeBase):
    raw: bytes = b""


@dataclass
class NumberNode(_NodeBase):
    value: int = 0


@dataclass
class ArgAnimationBase:
    """The 'tf_*' transform header common to every ArgAnimationNode."""
    matrix: Matrix4x4 = ()
    position: Vec3d = (0.0, 0.0, 0.0)
    quat1: QuatWXYZ = (1.0, 0.0, 0.0, 0.0)
    quat2: QuatWXYZ = (1.0, 0.0, 0.0, 0.0)
    scale: Vec3d = (1.0, 1.0, 1.0)


@dataclass
class ArgAnimationNode(_NodeBase):
    base: ArgAnimationBase = field(default_factory=ArgAnimationBase)
    pos_data: List[Tuple[int, List[AnimatedKey]]] = field(default_factory=list)
    rot_data: List[Tuple[int, List[AnimatedKey]]] = field(default_factory=list)
    scale_data: List[Tuple[int, Tuple[List[AnimatedKey], List[AnimatedKey]]]] = field(default_factory=list)
    bone_transform: Optional[Matrix4x4] = None  # only on ArgAnimatedBone


@dataclass
class ArgVisibilityNode(_NodeBase):
    # List of (argument, [(frame_start, frame_end), ...])
    vis_data: List[Tuple[int, List[Tuple[float, float]]]] = field(default_factory=list)
    # __VERSION__=1 adds a Matrixf after the ranges (identity in C-130).
    trailing_matrix: Matrix4x4 = ()


# ---------------------------------------------------------------------------
#  Render-item nodes
# ---------------------------------------------------------------------------


@dataclass
class ParentEntry:
    """One entry in the multi-parent table of a RenderNode."""
    node: int
    index_start: int = 0
    damage_arg: int = -1


@dataclass
class RenderNode(_NodeBase):
    material_id: int = 0
    parents: List[ParentEntry] = field(default_factory=list)
    vertex_data: List[Tuple[float, ...]] = field(default_factory=list)
    index_data: List[int] = field(default_factory=list)


@dataclass
class SkinNode(_NodeBase):
    material_id: int = 0
    bones: List[int] = field(default_factory=list)
    vertex_data: List[Tuple[float, ...]] = field(default_factory=list)
    index_data: List[int] = field(default_factory=list)


@dataclass
class ShellNode(_NodeBase):
    parent: int = -1
    vertex_format: Optional[VertexFormat] = None
    vertex_data: List[Tuple[float, ...]] = field(default_factory=list)
    index_data: List[int] = field(default_factory=list)


@dataclass
class SegmentsNode(_NodeBase):
    segments: List[Tuple[float, ...]] = field(default_factory=list)


@dataclass
class LightNode(_NodeBase):
    parent: int = -1
    light_props: Dict[str, Any] = field(default_factory=dict)


@dataclass
class FakeLightsNode(_NodeBase):
    """Generic placeholder for FakeOmni / FakeSpot / FakeALS nodes."""
    pass


@dataclass
class Connector(_NodeBase):
    parent: int = -1


# ---------------------------------------------------------------------------
#  Top-level container
# ---------------------------------------------------------------------------


@dataclass
class ParsedEDM:
    """Result of parsing an .edm file."""
    version: int
    root: RootNode
    nodes: List[_NodeBase] = field(default_factory=list)
    render_nodes: List[_NodeBase] = field(default_factory=list)
    shell_nodes: List[_NodeBase] = field(default_factory=list)
    light_nodes: List[_NodeBase] = field(default_factory=list)
    connectors: List[_NodeBase] = field(default_factory=list)
    extra_render_items: Dict[str, List[Any]] = field(default_factory=dict)

    # --- convenience -----------------------------------------------------
    @property
    def materials(self) -> List[Material]:
        return self.root.materials

    def all_render_objects(self):
        """Iterate everything that should produce a Blender object."""
        for n in self.render_nodes:
            yield n
        for n in self.shell_nodes:
            yield n
        for n in self.light_nodes:
            yield n
        for n in self.connectors:
            yield n
