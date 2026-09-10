#!/usr/bin/env python3
"""Example use of the Python binding's write path.

Builds a two-joint, one-triangle avatar from nothing, animates it, saves it as
a .arfz, then reads it back and prints the skinned result.  Small enough that
the expected numbers can be checked by hand, which makes it a usable smoke test
of the whole round trip.

    python3 examples/write_avatar.py /tmp/tiny.arfz
    python3 tools/validate_arf.py /tmp/tiny.arfz
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bindings", "python"))

from arf import Avatar

IDENTITY = [1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1]


def main(path: str) -> int:
    avatar = Avatar.create(nodes=2, vertices=3, triangles=1, weights=3)

    # A root and one child ten centimetres above it.
    avatar.set_node(0, "root", None, (0, 0, 0), (0, 0, 0, 1))
    avatar.set_node(1, "child", 0, (0, 10, 0), (0, 0, 0, 1))

    # Geometry is written straight into the library's own arrays.
    positions = avatar.mesh_positions
    for index, vertex in enumerate([(0, 0, 0), (10, 0, 0), (0, 10, 0)]):
        for axis in range(3):
            positions[index, axis] = vertex[axis]

    indices = avatar.mesh_indices
    for axis in range(3):
        indices[0, axis] = axis

    # Two vertices ride the root, the third rides the child.
    vertex_of, joint_of, weight_of = avatar.skin_weights
    for slot, (vertex, joint) in enumerate([(0, 0), (1, 0), (2, 1)]):
        vertex_of[slot] = vertex
        joint_of[slot] = joint
        weight_of[slot] = 1.0

    inverse_bind = avatar.inverse_bind_matrices
    for joint in range(2):
        for row in range(4):
            for column in range(4):
                inverse_bind[joint, row, column] = 1.0 if row == column else 0.0

    # Four frames sliding the root five centimetres up each time.  Matrices are
    # row-major, so the Y translation is element 7.
    for frame in range(4):
        matrices = list(IDENTITY) + list(IDENTITY)
        matrices[7] = frame * 5.0
        avatar.append_frame(frame, matrices)

    avatar.save(path)
    print(f"wrote {path}")

    with Avatar.load(path) as reloaded:
        print(repr(reloaded))
        pose = reloaded.pose()
        for frame in range(reloaded.frame_count):
            pose.evaluate(frame)
            rows = [[round(float(pose.positions[v, c]), 1) for c in range(3)] for v in range(3)]
            print(f"  frame {frame}: {rows}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tiny.arfz"))
