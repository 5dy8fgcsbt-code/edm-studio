"""
dcs_edm_importer.edm
====================

Pure-Python parser for the DCS World EDM binary model format.

This sub-package has zero Blender dependencies and can be used standalone
for testing, debugging, or building external tooling around .edm files.

Public API:
    EDMFile.parse(path) -> ParsedEDM
"""

from .parser import EDMFileParser, parse_edm
from .types import (
    ParsedEDM,
    Material,
    Texture,
    VertexFormat,
    NodeType,
)

__all__ = [
    "EDMFileParser",
    "parse_edm",
    "ParsedEDM",
    "Material",
    "Texture",
    "VertexFormat",
    "NodeType",
]
