"""
Low-level binary reader for EDM files.

EDM is a little-endian, sequential, length-prefixed binary format.
This module provides a thin wrapper around `struct` that reads the
primitive types used throughout the EDM specification:

  * uchar / ushort / uint / int    (1 / 2 / 4 / 4 bytes)
  * float / double                 (4 / 8 bytes)
  * Vec2f / Vec3d / Matrixf / Matrixd / Quaternion
  * uint_string                    (length-prefixed Windows-1251 string)
  * v10 string lookup table        (uint index into pre-loaded list)
  * list<T>                        (uint count + count repeated reads)

The reader is **stateful** with respect to:
  * file version (8 or 10)
  * the v10 string lookup table

References
----------
EDM Specification, "Basic types and Structures":
    https://ndevenish.github.io/Blender_ioEDM/EDM_Specification.html
"""

from __future__ import annotations

import struct
from typing import Callable, List, Optional, Sequence, Tuple


# Windows-1251 is the character encoding documented for EDM string blobs.
EDM_STRING_ENCODING = "windows-1251"


class BinaryReader:
    """Wraps an open binary file with EDM-aware read primitives."""

    __slots__ = ("f", "version", "_strings", "_size")

    def __init__(self, fileobj):
        self.f = fileobj
        self.version: Optional[int] = None
        self._strings: Optional[List[str]] = None
        # Cache size for safer EOF checks.
        self.f.seek(0, 2)
        self._size = self.f.tell()
        self.f.seek(0)

    # ----- positional helpers ------------------------------------------------
    def tell(self) -> int:
        return self.f.tell()

    def remaining(self) -> int:
        return self._size - self.f.tell()

    def read_raw(self, n: int) -> bytes:
        if n < 0 or n > self.remaining():
            raise ValueError(f"Truncated EDM at byte {self.tell()}: requested {n}, remaining {self.remaining()}")
        return self.f.read(n)

    def skip(self, n: int) -> None:
        self.read_raw(n)

    # ----- scalars -----------------------------------------------------------
    def uchar(self) -> int:
        return self.read_raw(1)[0]

    def uchars(self, count: int) -> Tuple[int, ...]:
        return struct.unpack(f"<{count}B", self.read_raw(count))

    def ushort(self) -> int:
        return struct.unpack("<H", self.read_raw(2))[0]

    def uint(self) -> int:
        return struct.unpack("<I", self.read_raw(4))[0]

    def uints(self, count: int) -> Tuple[int, ...]:
        return struct.unpack(f"<{count}I", self.read_raw(4 * count))

    def int32(self) -> int:
        return struct.unpack("<i", self.read_raw(4))[0]

    def ints(self, count: int) -> Tuple[int, ...]:
        return struct.unpack(f"<{count}i", self.read_raw(4 * count))

    def float32(self) -> float:
        return struct.unpack("<f", self.read_raw(4))[0]

    def floats(self, count: int) -> Tuple[float, ...]:
        return struct.unpack(f"<{count}f", self.read_raw(4 * count))

    def double(self) -> float:
        return struct.unpack("<d", self.read_raw(8))[0]

    def doubles(self, count: int) -> Tuple[float, ...]:
        return struct.unpack(f"<{count}d", self.read_raw(8 * count))

    # ----- math composites ---------------------------------------------------
    def vec3d(self) -> Tuple[float, float, float]:
        return self.doubles(3)  # type: ignore[return-value]

    def matrixd(self) -> Tuple[float, ...]:
        # 16 doubles, column-major
        return self.doubles(16)

    def matrixf(self) -> Tuple[float, ...]:
        return self.floats(16)

    def quaternion_xyzw(self) -> Tuple[float, float, float, float]:
        """Read an osg::Quaternion stored as xyzw doubles.

        Returned in (w, x, y, z) order so it can be passed straight to
        :class:`mathutils.Quaternion`.
        """
        x, y, z, w = self.doubles(4)
        return (w, x, y, z)

    # ----- strings -----------------------------------------------------------
    def install_string_table(self, table: Sequence[str]) -> None:
        """Set the v10 string lookup table used by :meth:`string`."""
        self._strings = list(table)

    @property
    def string_table(self) -> Optional[List[str]]:
        return self._strings

    def uint_string(self) -> str:
        """Length-prefixed Windows-1251 string (used regardless of version)."""
        length = self.uint()
        if length == 0:
            return ""
        data = self.read_raw(length)
        return data.decode(EDM_STRING_ENCODING, errors="replace")

    def string(self) -> str:
        """Read a string. In v10 this resolves through the lookup table."""
        if self.version == 10 and self._strings is not None:
            idx = self.uint()
            if 0 <= idx < len(self._strings):
                return self._strings[idx]
            raise ValueError(f"Invalid string index {idx} at byte {self.tell()-4}")
        return self.uint_string()

    # ----- generic list ------------------------------------------------------
    def list_of(self, read_fn: Callable[[], object]) -> List[object]:
        count = self.uint()
        return [read_fn() for _ in range(count)]
