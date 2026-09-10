"""ctypes binding for libarf, the MPEG ARF avatar container reader/writer.

The C library keeps everything in flat contiguous arrays, so this binding is a
thin one: the structures below mirror ``src/libarf/arf.h`` field for field, and
the array accessors hand back zero-copy views of the library's own memory
rather than copies.  With numpy installed those views are ``ndarray``s of the
right shape; without it they are ``memoryview``s, and nothing here requires
numpy.

    from arf import Avatar

    with Avatar.load("person_0.arfz") as avatar:
        print(avatar.name, avatar.frame_count, avatar.duration)

        pose = avatar.pose()
        for frame in range(avatar.frame_count):
            pose.evaluate(frame)
            draw(pose.positions, pose.normals, avatar.mesh_indices)

Views borrow memory owned by the avatar.  Keep the avatar alive for as long as
you hold one, and do not hold a pose's view across a call to ``evaluate`` if
you needed the old values -- the buffer is reused.

This reads SAM3DBody-flavoured ARF and is not a certified conformant
ISO/IEC 23090-39 implementation.  See README.md.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys

__all__ = ["Avatar", "Pose", "Node", "ArfError", "MAX_NAME", "DESPIKE_DEFAULT_DEGREES", "library"]

MAX_NAME = 64

#: Joint angular velocity above which a frame is treated as a tracking glitch.
DESPIKE_DEFAULT_DEGREES = 40.0

ARF_OK = 0
_RESULT_NAMES = {
    0: "ok",
    1: "bad argument",
    2: "out of memory",
    3: "I/O error",
    4: "ZIP error",
    5: "JSON error",
    6: "format error",
}


class ArfError(Exception):
    """Raised when a libarf call fails.  Carries the library's own message."""

    def __init__(self, message: str, code: int = 0):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------------
# Structures -- these mirror src/libarf/arf.h exactly, in declaration order, so
# that ctypes reproduces the C compiler's padding.
# ---------------------------------------------------------------------------

class _Node(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * MAX_NAME),
        ("parent", ctypes.c_int),
        ("translation", ctypes.c_float * 3),
        ("rotation", ctypes.c_float * 4),
        ("scale", ctypes.c_float * 3),
    ]


class _Mesh(ctypes.Structure):
    _fields_ = [
        ("numberOfVertices", ctypes.c_uint),
        ("numberOfTriangles", ctypes.c_uint),
        ("positions", ctypes.POINTER(ctypes.c_float)),
        ("indices", ctypes.POINTER(ctypes.c_uint)),
    ]


class _Skin(ctypes.Structure):
    _fields_ = [
        ("numberOfVertices", ctypes.c_uint),
        ("numberOfJoints", ctypes.c_uint),
        ("numberOfWeights", ctypes.c_uint),
        ("vertexIndex", ctypes.POINTER(ctypes.c_uint)),
        ("jointIndex", ctypes.POINTER(ctypes.c_uint)),
        ("weight", ctypes.POINTER(ctypes.c_float)),
        ("vertexStart", ctypes.POINTER(ctypes.c_uint)),
        ("inverseBindMatrices", ctypes.POINTER(ctypes.c_float)),
    ]


class _Blendshapes(ctypes.Structure):
    _fields_ = [
        ("numberOfShapes", ctypes.c_uint),
        ("numberOfVertices", ctypes.c_uint),
        ("deltas", ctypes.POINTER(ctypes.c_float)),
    ]


class _Landmarks(ctypes.Structure):
    _fields_ = [
        ("numberOfLandmarks", ctypes.c_uint),
        ("vertexIndex", ctypes.POINTER(ctypes.c_uint)),
    ]


class _TextureTarget(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * MAX_NAME),
        ("mimeType", ctypes.c_char * MAX_NAME),
        ("bytes", ctypes.c_void_p),
        ("length", ctypes.c_size_t),
    ]


class _TextureSet(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * MAX_NAME),
        ("materialMimeType", ctypes.c_char * MAX_NAME),
        ("materialBytes", ctypes.c_void_p),
        ("materialLength", ctypes.c_size_t),
        ("numberOfTargets", ctypes.c_uint),
        ("targets", ctypes.POINTER(_TextureTarget)),
    ]


class _Avatar(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * MAX_NAME),
        ("id", ctypes.c_char * MAX_NAME),
        ("numberOfNodes", ctypes.c_uint),
        ("nodes", ctypes.POINTER(_Node)),
        ("rootNode", ctypes.c_uint),
        ("mesh", _Mesh),
        ("skin", _Skin),
        ("timescale", ctypes.c_float),
        ("numberOfFrames", ctypes.c_uint),
        ("frameTimestamp", ctypes.POINTER(ctypes.c_uint)),
        ("localMatrices", ctypes.POINTER(ctypes.c_float)),
        ("hasFace", ctypes.c_uint),
        ("blendshapes", _Blendshapes),
        ("faceTimescale", ctypes.c_float),
        ("numberOfFaceFrames", ctypes.c_uint),
        ("faceTimestamp", ctypes.POINTER(ctypes.c_uint)),
        ("blendshapeWeights", ctypes.POINTER(ctypes.c_float)),
        ("hasLandmarks", ctypes.c_uint),
        ("landmarks", _Landmarks),
        ("landmarkTimescale", ctypes.c_float),
        ("numberOfLandmarkFrames", ctypes.c_uint),
        ("landmarkTimestamp", ctypes.POINTER(ctypes.c_uint)),
        ("landmarkPositions", ctypes.POINTER(ctypes.c_float)),
        ("hasTextureSet", ctypes.c_uint),
        ("textureSet", _TextureSet),
        ("frameCapacity", ctypes.c_uint),
        ("faceFrameCapacity", ctypes.c_uint),
        ("landmarkFrameCapacity", ctypes.c_uint),
    ]


class _Pose(ctypes.Structure):
    _fields_ = [
        ("numberOfJoints", ctypes.c_uint),
        ("numberOfVertices", ctypes.c_uint),
        ("jointGlobals", ctypes.POINTER(ctypes.c_float)),
        ("skinMatrices", ctypes.POINTER(ctypes.c_float)),
        ("restPositions", ctypes.POINTER(ctypes.c_float)),
        ("positions", ctypes.POINTER(ctypes.c_float)),
        ("normals", ctypes.POINTER(ctypes.c_float)),
    ]


# ---------------------------------------------------------------------------
# Loading the shared object
# ---------------------------------------------------------------------------

def _candidate_paths() -> list:
    """Where to look for libarf, most specific first."""
    name = {"darwin": "libarf.dylib", "win32": "arf.dll"}.get(sys.platform, "libarf.so")

    override = os.environ.get("ARF_LIBRARY")
    if override:
        return [override]

    here = os.path.dirname(os.path.abspath(__file__))
    repository = os.path.abspath(os.path.join(here, "..", ".."))

    paths = [
        os.path.join(here, name),
        os.path.join(repository, "build", name),
        os.path.join(repository, "build", "lib", name),
    ]

    found = ctypes.util.find_library("arf")
    if found:
        paths.append(found)
    paths.append(name)
    return paths


def _bind(library: ctypes.CDLL) -> ctypes.CDLL:
    """Declare argument and return types.  Without this, ctypes assumes every
    function returns int, which truncates the returned pointers on 64 bit."""
    avatar_p = ctypes.POINTER(_Avatar)
    pose_p = ctypes.POINTER(_Pose)
    float_p = ctypes.POINTER(ctypes.c_float)

    signatures = {
        "arfLastError": ([], ctypes.c_char_p),
        "arfLoad": ([ctypes.c_char_p], avatar_p),
        "arfLoadFromMemory": ([ctypes.c_void_p, ctypes.c_size_t], avatar_p),
        "arfFree": ([avatar_p], None),
        "arfDuration": ([avatar_p], ctypes.c_float),
        "arfFrameAtTime": ([avatar_p, ctypes.c_float], ctypes.c_uint),
        "arfPrintInfo": ([avatar_p], None),
        "arfPoseAllocate": ([avatar_p], pose_p),
        "arfPoseFree": ([pose_p], None),
        "arfPoseEvaluate": ([avatar_p, pose_p, ctypes.c_uint], ctypes.c_int),
        "arfComposeGlobals": ([avatar_p, float_p, float_p], None),
        "arfComputeSkinMatrices": ([avatar_p, float_p, float_p], None),
        "arfApplyBlendshapes": ([avatar_p, float_p, float_p], None),
        "arfSkinVertices": ([avatar_p, float_p, float_p, float_p], None),
        "arfRecomputeNormals": ([avatar_p, float_p, float_p], None),
        "arfRebuildSkinIndex": ([avatar_p], ctypes.c_int),
        "arfDespikeFrames": ([avatar_p, ctypes.c_float, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int),
        "arfMultiply4x4": ([float_p, float_p, float_p], None),
        "arfTranspose4x4": ([float_p, float_p], None),
        "arfIdentity4x4": ([float_p], None),
        "arfCreate": ([ctypes.c_uint] * 4, avatar_p),
        "arfSetNode": ([avatar_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_int, float_p, float_p], ctypes.c_int),
        "arfAppendFrame": ([avatar_p, ctypes.c_uint, float_p], ctypes.c_int),
        "arfEnableFace": ([avatar_p, ctypes.c_uint, float_p], ctypes.c_int),
        "arfAppendFaceFrame": ([avatar_p, ctypes.c_uint, float_p], ctypes.c_int),
        "arfEnableLandmarks": ([avatar_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int),
        "arfAppendLandmarkFrame": ([avatar_p, ctypes.c_uint, float_p], ctypes.c_int),
        "arfEnableTextureSet": ([avatar_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p], ctypes.c_int),
        "arfAddTextureTarget": ([avatar_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p], ctypes.c_int),
        "arfSave": ([avatar_p, ctypes.c_char_p], ctypes.c_int),
        "arfExportOBJ": ([avatar_p, float_p, ctypes.c_char_p], ctypes.c_int),
    }

    for symbol, (argtypes, restype) in signatures.items():
        function = getattr(library, symbol)
        function.argtypes = argtypes
        function.restype = restype

    return library


def _load_library() -> ctypes.CDLL:
    attempts = []
    for path in _candidate_paths():
        try:
            return _bind(ctypes.CDLL(path))
        except OSError as problem:
            attempts.append(f"  {path}: {problem}")

    raise ImportError(
        "cannot find libarf. Build it with `cmake -S . -B build && cmake --build build`, "
        "or point ARF_LIBRARY at the shared object.\nTried:\n" + "\n".join(attempts)
    )


library = _load_library()


def _check(code: int, what: str) -> None:
    if code != ARF_OK:
        raise ArfError(
            f"{what}: {library.arfLastError().decode('utf-8', 'replace')} "
            f"({_RESULT_NAMES.get(code, code)})",
            code,
        )


# ---------------------------------------------------------------------------
# Array views
# ---------------------------------------------------------------------------

try:
    import numpy as _numpy
except ImportError:  # numpy is a convenience here, never a requirement
    _numpy = None


_MEMORYVIEW_FORMATS = {
    ctypes.c_float: "f",
    ctypes.c_uint: "I",
    ctypes.c_int: "i",
}


def _view(pointer, count: int, shape):
    """A zero-copy view of `count` elements of the library's memory.

    With numpy present this is an ndarray; otherwise a memoryview, which has to
    be reshaped through a byte view because memoryview.cast only accepts native
    single character formats."""
    if not pointer:
        return None

    if _numpy is not None:
        array = _numpy.ctypeslib.as_array(pointer, shape=(count,))
        return array.reshape(shape) if shape else array

    flat = (pointer._type_ * count).from_address(ctypes.addressof(pointer.contents))
    view = memoryview(flat).cast("B")
    return view.cast(_MEMORYVIEW_FORMATS[pointer._type_], shape if shape else (count,))


class Node:
    """One rest-skeleton joint."""

    __slots__ = ("index", "name", "parent", "translation", "rotation", "scale")

    def __init__(self, index: int, raw: _Node):
        self.index = index
        self.name = raw.name.decode("utf-8", "replace")
        self.parent = raw.parent if raw.parent >= 0 else None
        self.translation = tuple(raw.translation)
        self.rotation = tuple(raw.rotation)  # XYZW, not WXYZ
        self.scale = tuple(raw.scale)

    def __repr__(self) -> str:
        return f"Node({self.index}, {self.name!r}, parent={self.parent})"


class Pose:
    """Scratch buffers for one evaluated frame.  Allocate once, reuse."""

    def __init__(self, avatar: "Avatar"):
        self._avatar = avatar
        self._pose = library.arfPoseAllocate(avatar._handle)
        if not self._pose:
            raise ArfError(library.arfLastError().decode("utf-8", "replace"))

    @property
    def _raw(self) -> _Pose:
        return self._pose.contents

    def evaluate(self, frame: int) -> "Pose":
        """Blendshapes, hierarchy compose, skinning and normals for one frame."""
        _check(library.arfPoseEvaluate(self._avatar._handle, self._pose, frame),
               f"evaluating frame {frame}")
        return self

    @property
    def positions(self):
        """Skinned vertex positions, shaped (vertices, 3), in centimetres."""
        raw = self._raw
        return _view(raw.positions, raw.numberOfVertices * 3, (raw.numberOfVertices, 3))

    @property
    def normals(self):
        """Recomputed smooth vertex normals, shaped (vertices, 3)."""
        raw = self._raw
        return _view(raw.normals, raw.numberOfVertices * 3, (raw.numberOfVertices, 3))

    @property
    def joint_globals(self):
        """Model-space joint transforms, shaped (joints, 4, 4), row-major."""
        raw = self._raw
        return _view(raw.jointGlobals, raw.numberOfJoints * 16, (raw.numberOfJoints, 4, 4))

    @property
    def skin_matrices(self):
        """Joint global times inverse bind, shaped (joints, 4, 4), row-major."""
        raw = self._raw
        return _view(raw.skinMatrices, raw.numberOfJoints * 16, (raw.numberOfJoints, 4, 4))

    def close(self) -> None:
        if getattr(self, "_pose", None):
            library.arfPoseFree(self._pose)
            self._pose = None

    __del__ = close

    def __enter__(self) -> "Pose":
        return self

    def __exit__(self, *exception) -> None:
        self.close()


class Avatar:
    """An ARF container held in memory."""

    def __init__(self, handle):
        if not handle:
            raise ArfError(library.arfLastError().decode("utf-8", "replace"))
        self._handle = handle

    # -- construction -------------------------------------------------------

    @classmethod
    def load(cls, path) -> "Avatar":
        """Read a .arfz container from disk."""
        return cls(library.arfLoad(os.fsencode(path)))

    @classmethod
    def from_bytes(cls, data: bytes) -> "Avatar":
        """Read a .arfz container already in memory."""
        buffer = ctypes.create_string_buffer(data, len(data))
        return cls(library.arfLoadFromMemory(ctypes.cast(buffer, ctypes.c_void_p), len(data)))

    @classmethod
    def create(cls, nodes: int, vertices: int, triangles: int, weights: int) -> "Avatar":
        """Allocate an empty avatar to fill in and save."""
        return cls(library.arfCreate(nodes, vertices, triangles, weights))

    # -- metadata -----------------------------------------------------------

    @property
    def _raw(self) -> _Avatar:
        return self._handle.contents

    @property
    def name(self) -> str:
        return self._raw.name.decode("utf-8", "replace")

    @property
    def id(self) -> str:
        return self._raw.id.decode("utf-8", "replace")

    @property
    def node_count(self) -> int:
        return self._raw.numberOfNodes

    @property
    def vertex_count(self) -> int:
        return self._raw.mesh.numberOfVertices

    @property
    def triangle_count(self) -> int:
        return self._raw.mesh.numberOfTriangles

    @property
    def frame_count(self) -> int:
        return self._raw.numberOfFrames

    @property
    def timescale(self) -> float:
        """Ticks per second, which for these containers is frames per second."""
        return self._raw.timescale

    @property
    def duration(self) -> float:
        """Clip length in seconds."""
        return library.arfDuration(self._handle)

    @property
    def has_face(self) -> bool:
        return bool(self._raw.hasFace)

    @property
    def root(self) -> Node:
        return self.nodes[self._raw.rootNode]

    @property
    def nodes(self) -> list:
        raw = self._raw
        return [Node(i, raw.nodes[i]) for i in range(raw.numberOfNodes)]

    def frame_at_time(self, seconds: float) -> int:
        """Frame index nearest a wall-clock time, clamped to the clip."""
        return library.arfFrameAtTime(self._handle, seconds)

    def print_info(self) -> None:
        """Print the library's own summary, matching tools/validate_arf.py."""
        sys.stdout.flush()
        library.arfPrintInfo(self._handle)

    # -- geometry -----------------------------------------------------------

    @property
    def mesh_positions(self):
        """Rest mesh vertices, shaped (vertices, 3), in centimetres."""
        raw = self._raw
        return _view(raw.mesh.positions, raw.mesh.numberOfVertices * 3,
                     (raw.mesh.numberOfVertices, 3))

    @property
    def mesh_indices(self):
        """Triangle vertex indices, shaped (triangles, 3)."""
        raw = self._raw
        return _view(raw.mesh.indices, raw.mesh.numberOfTriangles * 3,
                     (raw.mesh.numberOfTriangles, 3))

    @property
    def inverse_bind_matrices(self):
        """Shaped (joints, 4, 4), row-major."""
        raw = self._raw
        return _view(raw.skin.inverseBindMatrices, raw.numberOfNodes * 16,
                     (raw.numberOfNodes, 4, 4))

    @property
    def skin_weights(self):
        """The sparse skin as three parallel arrays: vertex, joint, weight."""
        raw = self._raw
        count = raw.skin.numberOfWeights
        return (_view(raw.skin.vertexIndex, count, None),
                _view(raw.skin.jointIndex, count, None),
                _view(raw.skin.weight, count, None))

    # -- animation ----------------------------------------------------------

    @property
    def local_matrices(self):
        """Every frame's local joint transforms, shaped (frames, joints, 4, 4),
        row-major.  A frame's matrix is the joint's complete local transform --
        do not also apply the node's rest translation and rotation."""
        raw = self._raw
        count = raw.numberOfFrames * raw.numberOfNodes * 16
        return _view(raw.localMatrices, count, (raw.numberOfFrames, raw.numberOfNodes, 4, 4))

    @property
    def frame_timestamps(self):
        """Per-frame timestamps in ticks; seconds are ticks / timescale."""
        raw = self._raw
        return _view(raw.frameTimestamp, raw.numberOfFrames, None)

    @property
    def blendshape_weights(self):
        """Face track weights shaped (face frames, shapes), or None."""
        raw = self._raw
        if not raw.hasFace:
            return None
        count = raw.numberOfFaceFrames * raw.blendshapes.numberOfShapes
        return _view(raw.blendshapeWeights, count,
                     (raw.numberOfFaceFrames, raw.blendshapes.numberOfShapes))

    def pose(self) -> Pose:
        """Allocate reusable per-frame buffers for this avatar."""
        return Pose(self)

    def despike(self, max_degrees_per_frame: float = DESPIKE_DEFAULT_DEGREES) -> int:
        """Replace tracking-glitch frames by interpolating across them.

        The pose estimator behind these containers regresses joint rotations as
        Euler angles; a joint near that representation's singularity -- the
        pelvis, for whole clips -- can jump branches for two or three frames and
        fold the body up. The matrices stay valid, so only their velocity gives
        them away. This rewrites the animation in place, which is why it is
        never applied for you.

        Returns the number of frames repaired."""
        repaired = ctypes.c_uint(0)
        _check(library.arfDespikeFrames(self._handle, max_degrees_per_frame,
                                        ctypes.byref(repaired)), "despiking")
        return repaired.value

    # -- writing ------------------------------------------------------------

    def set_node(self, index: int, name: str, parent, translation=None, rotation=None) -> None:
        """Fill in one rest-skeleton node.  `parent` is an index, or None for
        the root, and must be less than `index`."""
        as_float3 = (ctypes.c_float * 3)
        as_float4 = (ctypes.c_float * 4)
        _check(library.arfSetNode(
            self._handle, index, name.encode("utf-8"),
            -1 if parent is None else parent,
            ctypes.cast(as_float3(*translation), ctypes.POINTER(ctypes.c_float)) if translation else None,
            ctypes.cast(as_float4(*rotation), ctypes.POINTER(ctypes.c_float)) if rotation else None,
        ), f"setting node {index}")

    def append_frame(self, timestamp: int, local_matrices) -> None:
        """Append one body frame: nodes x 16 row-major floats."""
        _check(library.arfAppendFrame(self._handle, timestamp, _as_float_pointer(local_matrices)),
               "appending a frame")

    def enable_face(self, shape_count: int, deltas) -> None:
        """Attach a blendshape set: shapes x vertices x 3 floats."""
        _check(library.arfEnableFace(self._handle, shape_count, _as_float_pointer(deltas)),
               "enabling the face track")

    def append_face_frame(self, timestamp: int, weights) -> None:
        """Append one face frame: one weight per blendshape."""
        _check(library.arfAppendFaceFrame(self._handle, timestamp, _as_float_pointer(weights)),
               "appending a face frame")

    def enable_landmarks(self, vertex_index) -> None:
        """Attach a landmark set: one mesh-vertex index per landmark."""
        indices = list(vertex_index)
        _check(library.arfEnableLandmarks(self._handle, len(indices), _as_uint_pointer(indices)),
               "enabling the landmark track")

    def append_landmark_frame(self, timestamp: int, positions) -> None:
        """Append one landmark frame: landmarks x 3 floats (x,y,z; z=0 for 2D)."""
        _check(library.arfAppendLandmarkFrame(self._handle, timestamp, _as_float_pointer(positions)),
               "appending a landmark frame")

    def enable_texture_set(self, name: str, material_bytes: bytes, material_mime_type: str) -> None:
        """Attach a texture set: an opaque base material image, carried but
        never decoded (there is no per-frame track -- TextureSet has no AAU
        counterpart)."""
        _check(library.arfEnableTextureSet(self._handle, name.encode("utf-8"),
                                            material_bytes, len(material_bytes),
                                            material_mime_type.encode("utf-8")),
               "enabling the texture set")

    def add_texture_target(self, name: str, image_bytes: bytes, mime_type: str) -> None:
        """Append one texture target: another opaque image blend target."""
        _check(library.arfAddTextureTarget(self._handle, name.encode("utf-8"),
                                            image_bytes, len(image_bytes),
                                            mime_type.encode("utf-8")),
               "adding a texture target")

    def save(self, path) -> None:
        """Write this avatar out as a .arfz container."""
        _check(library.arfSave(self._handle, os.fsencode(path)), f"writing {path}")

    def export_obj(self, path, positions=None) -> None:
        """Write the mesh as a Wavefront OBJ, posed or at rest."""
        _check(library.arfExportOBJ(self._handle, _as_float_pointer(positions) if positions is not None else None,
                                    os.fsencode(path)), f"writing {path}")

    # -- lifetime -----------------------------------------------------------

    def close(self) -> None:
        if getattr(self, "_handle", None):
            library.arfFree(self._handle)
            self._handle = None

    __del__ = close

    def __enter__(self) -> "Avatar":
        return self

    def __exit__(self, *exception) -> None:
        self.close()

    def __repr__(self) -> str:
        return (f"<Avatar {self.id!r} {self.node_count} joints, {self.vertex_count} vertices, "
                f"{self.frame_count} frames, {self.duration:.2f} s>")


def _as_float_pointer(values):
    """Accept a numpy array, a ctypes array or any float sequence."""
    if values is None:
        return None

    if _numpy is not None and isinstance(values, _numpy.ndarray):
        contiguous = _numpy.ascontiguousarray(values, dtype=_numpy.float32)
        return contiguous.ctypes.data_as(ctypes.POINTER(ctypes.c_float))

    if isinstance(values, ctypes.Array):
        return ctypes.cast(values, ctypes.POINTER(ctypes.c_float))

    flat = list(values)
    return (ctypes.c_float * len(flat))(*flat)


def _as_uint_pointer(values):
    """Accept a numpy array, a ctypes array or any integer sequence."""
    if values is None:
        return None

    if _numpy is not None and isinstance(values, _numpy.ndarray):
        contiguous = _numpy.ascontiguousarray(values, dtype=_numpy.uint32)
        return contiguous.ctypes.data_as(ctypes.POINTER(ctypes.c_uint))

    if isinstance(values, ctypes.Array):
        return ctypes.cast(values, ctypes.POINTER(ctypes.c_uint))

    flat = list(values)
    return (ctypes.c_uint * len(flat))(*flat)


def _main(argv) -> int:
    """`python3 arf.py file.arfz` prints a summary, the same as arfplay --info."""
    if len(argv) != 2:
        print(__doc__)
        print("usage: python3 arf.py <container.arfz>")
        return 1

    with Avatar.load(argv[1]) as avatar:
        avatar.print_info()
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))
