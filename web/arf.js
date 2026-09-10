/** @file arf.js
 *  @brief Reading MPEG ARF avatar containers (.arfz) in a browser, and posing
 *         them on the CPU.  The JavaScript counterpart of src/libarf.
 *
 *  No dependencies.  The only thing this needs beyond plain JavaScript is the
 *  platform's own DecompressionStream for the DEFLATE inside the ZIP, so there
 *  is no inflate implementation here and no library to load.
 *
 *  Conventions carried over from the C library, all of which will silently
 *  produce a wrong-looking avatar if ignored:
 *
 *   - every 4x4 matrix is ROW-MAJOR with the translation in the last column
 *     (m[3], m[7], m[11]).  WebGL wants column-major, so a matrix is
 *     transposed on upload, not here.
 *   - a frame's local matrix is the COMPLETE local transform; the node's rest
 *     translation and rotation are not applied on top when animating.
 *   - units are centimetres, +Y is up, and the avatar faces +Z.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

(function (global) {
'use strict';

const AAU_CONFIG = 0, AAU_BLENDSHAPE = 1, AAU_JOINT = 2;
const COMPONENT_FLOAT = 5126, COMPONENT_UNSIGNED_INT = 5125;

/* ===========================================================================
 *  ZIP
 * =========================================================================*/

/** Inflate a raw DEFLATE stream using the platform's own decompressor. */
async function inflateRaw(bytes)
{
    if (typeof DecompressionStream === 'undefined')
    {
        throw new Error('this browser has no DecompressionStream — needs Chrome 80+, Firefox 113+ or Safari 16.4+');
    }

    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** Read every entry of a ZIP into a Map of name -> Uint8Array.
 *
 *  A .arfz is a handful of entries totalling a few megabytes, so reading them
 *  all up front is simpler than seeking and costs nothing worth saving. */
async function unzip(arrayBuffer)
{
    const view  = new DataView(arrayBuffer);
    const bytes = new Uint8Array(arrayBuffer);

    /* The end-of-central-directory record is last, but a trailing comment can
     * push it back by up to 64 KB, so scan backwards for its signature. */
    let end = -1;
    const limit = Math.max(0, bytes.length - 66000);
    for (let i = bytes.length - 22; i >= limit; i--)
    {
        if (view.getUint32(i, true) === 0x06054b50) { end = i; break; }
    }
    if (end < 0) { throw new Error('not a readable ZIP container'); }

    const count = view.getUint16(end + 10, true);
    let offset  = view.getUint32(end + 16, true);

    const decoder = new TextDecoder();
    const entries = [];

    for (let i = 0; i < count; i++)
    {
        if (view.getUint32(offset, true) !== 0x02014b50)
        {
            throw new Error('corrupt ZIP central directory at entry ' + i);
        }

        const method       = view.getUint16(offset + 10, true);
        const compressed   = view.getUint32(offset + 20, true);
        const uncompressed = view.getUint32(offset + 24, true);
        const nameLength   = view.getUint16(offset + 28, true);
        const extraLength  = view.getUint16(offset + 30, true);
        const commentLength= view.getUint16(offset + 32, true);
        const localOffset  = view.getUint32(offset + 42, true);

        if (uncompressed === 0xFFFFFFFF || compressed === 0xFFFFFFFF)
        {
            throw new Error('zip64 containers are not supported');
        }

        entries.push({
            name: decoder.decode(bytes.subarray(offset + 46, offset + 46 + nameLength)),
            method, compressed, localOffset
        });

        offset += 46 + nameLength + extraLength + commentLength;
    }

    const files = new Map();
    for (const entry of entries)
    {
        if (view.getUint32(entry.localOffset, true) !== 0x04034b50)
        {
            throw new Error('corrupt local header for "' + entry.name + '"');
        }

        /* The local header's own name and extra fields are allowed to differ
         * in length from the central directory's, so measure them here. */
        const nameLength  = view.getUint16(entry.localOffset + 26, true);
        const extraLength = view.getUint16(entry.localOffset + 28, true);
        const start = entry.localOffset + 30 + nameLength + extraLength;
        const raw   = bytes.subarray(start, start + entry.compressed);

        if      (entry.method === 0) { files.set(entry.name, raw.slice()); }
        else if (entry.method === 8) { files.set(entry.name, await inflateRaw(raw)); }
        else { throw new Error('"' + entry.name + '" uses unsupported compression method ' + entry.method); }
    }

    return files;
}

/* ===========================================================================
 *  Tensors and animation units
 * =========================================================================*/

/** A bounds-checked little-endian cursor, so a truncated payload throws where
 *  it runs out rather than reading whatever follows it in memory. */
class Cursor
{
    constructor(bytes, what)
    {
        this.view   = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        this.bytes  = bytes;
        this.offset = 0;
        this.what   = what || 'payload';
    }

    take(length)
    {
        if (this.offset + length > this.bytes.length)
        {
            throw new Error(this.what + ' is truncated');
        }
        const at = this.offset;
        this.offset += length;
        return at;
    }

    int32()  { return this.view.getInt32(this.take(4), true);   }
    uint32() { return this.view.getUint32(this.take(4), true);  }
    uint8()  { return this.view.getUint8(this.take(1));         }
    float32(){ return this.view.getFloat32(this.take(4), true); }

    /** A run of little-endian float32s.  Copied rather than aliased: the
     *  inflated buffer is not guaranteed to be 4-byte aligned, which a
     *  Float32Array view would require. */
    floats(count)
    {
        const at = this.take(count * 4);
        const out = new Float32Array(count);
        for (let i = 0; i < count; i++) { out[i] = this.view.getFloat32(at + i * 4, true); }
        return out;
    }

    uint32s(count)
    {
        const at = this.take(count * 4);
        const out = new Uint32Array(count);
        for (let i = 0; i < count; i++) { out[i] = this.view.getUint32(at + i * 4, true); }
        return out;
    }

    string()
    {
        const length = this.uint32();
        const at = this.take(length);
        return new TextDecoder().decode(this.bytes.subarray(at, at + length));
    }
}

/** Dense tensor: int32 dimension count, dims, dtype, then row-major data. */
function readDense(bytes, what)
{
    const cursor = new Cursor(bytes, what);
    const rank = cursor.int32();
    if (rank < 1 || rank > 4) { throw new Error(what + ': dense tensor claims ' + rank + ' dimensions'); }

    const dims = [];
    let count = 1;
    for (let i = 0; i < rank; i++) { dims.push(cursor.int32()); count *= dims[i]; }

    const dtype = cursor.int32();
    const data  = (dtype === COMPONENT_FLOAT) ? cursor.floats(count)
                : (dtype === COMPONENT_UNSIGNED_INT) ? cursor.uint32s(count)
                : null;

    if (data === null) { throw new Error(what + ': unsupported component type ' + dtype); }
    return { dims, dtype, data };
}

/** Sparse tensor, as the skin weights use it. */
function readSparse(bytes, what)
{
    const cursor = new Cursor(bytes, what);
    const rank = cursor.int32();
    if (rank !== 2) { throw new Error(what + ': expected a 2 dimensional sparse tensor'); }

    const dims = [cursor.int32(), cursor.int32()];
    const valueCount = cursor.int32();
    const itype = cursor.int32();
    const dtype = cursor.int32();

    if (itype !== COMPONENT_UNSIGNED_INT || dtype !== COMPONENT_FLOAT)
    {
        throw new Error(what + ': component types are ' + itype + '/' + dtype);
    }

    return { dims, indices: cursor.uint32s(valueCount), values: cursor.floats(valueCount) };
}

/** Walk an animation stream, yielding one unit at a time.
 *
 *  Unknown unit types are handed back like any other; skipping them is the
 *  caller's job and is required behaviour, since unitLength exists precisely
 *  so a reader can step over what it does not understand. */
function* animationUnits(bytes)
{
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let offset = 0;

    while (offset + 5 <= bytes.length)
    {
        const type   = view.getUint8(offset);
        const length = view.getUint32(offset + 1, true);
        const start  = offset + 5;

        if (start + length > bytes.length) { throw new Error('animation stream is truncated'); }

        yield { type, payload: bytes.subarray(start, start + length) };
        offset = start + length;
    }
}

/* ===========================================================================
 *  Avatar
 * =========================================================================*/

class Avatar
{
    /** Read a container from its raw bytes. */
    static async load(arrayBuffer)
    {
        const files = await unzip(arrayBuffer);

        const jsonBytes = files.get('arf.json');
        if (!jsonBytes) { throw new Error('arf.json missing from the container'); }

        const document = JSON.parse(new TextDecoder().decode(jsonBytes));
        for (const key of ['preamble', 'metadata', 'structure', 'components', 'data'])
        {
            if (!(key in document)) { throw new Error('arf.json is missing the required top level key "' + key + '"'); }
        }
        if (document.preamble.signature !== 'ARF')
        {
            throw new Error('preamble.signature is "' + document.preamble.signature + '", expected "ARF"');
        }

        return new Avatar(document, files);
    }

    constructor(document, files)
    {
        const components = document.components;

        /* Resolve a component reference (a data[].id) to its ZIP entry. */
        const uriOf = new Map(document.data.map(item => [item.id, item.uri]));
        const entry = (id, what) =>
        {
            const uri = uriOf.get(id);
            if (!uri) { throw new Error(what + ' reference "' + id + '" does not resolve to a data item'); }

            const bytes = files.get(uri);
            if (!bytes) { throw new Error(what + ' points at missing entry "' + uri + '"'); }

            const declared = (document.data.find(d => d.id === id) || {}).byteLength;
            if (declared !== undefined && declared !== bytes.length)
            {
                throw new Error(what + ': byteLength ' + declared + ' does not match the ' + bytes.length + ' bytes in "' + uri + '"');
            }
            return bytes;
        };

        this.name = document.metadata.name || 'avatar';
        this.id   = document.metadata.id   || '0';

        /* -- skeleton -- */
        const nodes = components.nodes || [];
        if (nodes.length === 0) { throw new Error('components.nodes is empty or missing'); }

        const indexOf = new Map(nodes.map((n, i) => [n.id, i]));
        this.nodes = nodes.map((node, i) =>
        {
            let parent = -1;
            if (node.parent !== undefined)
            {
                if (!indexOf.has(node.parent))
                {
                    throw new Error('node "' + node.id + '" names a parent "' + node.parent + '" that is not in components.nodes');
                }
                parent = indexOf.get(node.parent);

                /* Depended on by composeGlobals, which is a single forward
                 * pass, and it rules out cycles for free. */
                if (parent >= i)
                {
                    throw new Error('node "' + node.id + '" at index ' + i + ' has a parent at index ' + parent +
                                    ' — parents must be listed before their children');
                }
            }
            return { id: node.id, parent, translation: node.translation || [0,0,0], rotation: node.rotation || [0,0,0,1] };
        });

        const roots = this.nodes.map((n, i) => n.parent < 0 ? i : -1).filter(i => i >= 0);
        if (roots.length !== 1) { throw new Error('expected exactly one node without a parent, found ' + roots.length); }
        this.rootNode = roots[0];

        /* AAU joint indices address the skeleton's joint list positionally, so
         * a disagreement here would drive every joint with another joint's
         * animation, silently. */
        const skeleton = (components.skeletons || [])[0];
        if (!skeleton) { throw new Error('components.skeletons is empty or missing'); }
        const joints = skeleton.joints || [];
        if (joints.length !== this.nodes.length)
        {
            throw new Error('skeleton lists ' + joints.length + ' joints but components.nodes has ' + this.nodes.length);
        }
        joints.forEach((name, i) =>
        {
            if (name !== this.nodes[i].id)
            {
                throw new Error('skeleton joint ' + i + ' is "' + name + '" but components.nodes[' + i + '] is "' + this.nodes[i].id + '"');
            }
        });

        /* -- mesh -- */
        const mesh = (components.meshes || [])[0];
        if (!mesh) { throw new Error('components.meshes is empty or missing'); }

        const positions = readDense(entry(mesh.positions, 'mesh positions'), 'mesh positions');
        const indices   = readDense(entry(mesh.indices,   'mesh indices'),   'mesh indices');

        this.numberOfVertices  = positions.dims[0];
        this.numberOfTriangles = indices.dims[0];
        this.positions = positions.data;
        this.indices   = indices.data;

        for (let i = 0; i < this.indices.length; i++)
        {
            if (this.indices[i] >= this.numberOfVertices)
            {
                throw new Error('triangle index ' + i + ' addresses vertex ' + this.indices[i] + ' of ' + this.numberOfVertices);
            }
        }

        /* -- skin -- */
        const skin = (components.skins || []).find(s => s.id === mesh.skin) || (components.skins || [])[0];
        if (!skin) { throw new Error('the mesh references a skin that is not in components.skins'); }

        const weights = readSparse(entry(skin.weights, 'skin weights'), 'skin weights');
        if (weights.dims[0] !== this.numberOfVertices || weights.dims[1] !== this.nodes.length)
        {
            throw new Error('skin weights cover [' + weights.dims + '] but the mesh/skeleton are [' +
                            this.numberOfVertices + ',' + this.nodes.length + ']');
        }

        const inverseBind = readDense(entry(skeleton.inverseBindMatrices, 'inverse bind matrices'), 'inverse bind matrices');
        if (inverseBind.dims[0] !== this.nodes.length || inverseBind.dims[1] !== 16)
        {
            throw new Error('inverse bind matrices must be a [' + this.nodes.length + ',16] tensor');
        }
        this.inverseBindMatrices = inverseBind.data;

        this.buildSkinIndex(weights);

        /* -- animation -- */
        const stream = (document.structure.animationStreams || []).find(s => s.frameworks === 'arf-body-v1');
        const streamBytes = files.get(stream ? stream.uri : 'animations/joints.bin');
        if (!streamBytes) { throw new Error('the joint animation stream is missing'); }

        this.readJointStream(streamBytes);
    }

    /** Turn the container's COO weights into per-vertex runs.
     *
     *  Nothing in the format promises the entries arrive vertex-major, and a
     *  player that assumed it would skin a scrambled mesh in silence, so the
     *  runs are counted and built rather than trusted. */
    buildSkinIndex(weights)
    {
        const vertexCount = this.numberOfVertices;
        const jointCount  = this.nodes.length;
        const count = weights.values.length;

        const start = new Uint32Array(vertexCount + 1);
        const vertexOf = new Uint32Array(count);
        for (let e = 0; e < count; e++)
        {
            /* The flat index is vertex * jointCount + joint. */
            const vertex = (weights.indices[e] / jointCount) | 0;
            if (vertex >= vertexCount) { throw new Error('skin weight ' + e + ' addresses vertex ' + vertex); }
            vertexOf[e] = vertex;
            start[vertex + 1]++;
        }
        for (let v = 0; v < vertexCount; v++) { start[v + 1] += start[v]; }

        const jointIndex = new Uint32Array(count);
        const weight     = new Float32Array(count);
        const fill = start.slice(0, vertexCount);

        for (let e = 0; e < count; e++)
        {
            const slot = fill[vertexOf[e]]++;
            jointIndex[slot] = weights.indices[e] % jointCount;
            weight[slot]     = weights.values[e];
        }

        this.skinStart = start;
        this.skinJoint = jointIndex;
        this.skinWeight = weight;
        this.numberOfWeights = count;
    }

    readJointStream(bytes)
    {
        const jointCount = this.nodes.length;
        const frames = [];
        let timescale = 30;

        for (const unit of animationUnits(bytes))
        {
            if (unit.type === AAU_CONFIG)
            {
                const cursor = new Cursor(unit.payload, 'AAU_CONFIG');
                cursor.uint32();                 /* config timestamp, always zero */
                this.profile = cursor.string();
                timescale = cursor.float32();
                continue;
            }

            /* Anything else is stepped over — forward compatibility. */
            if (unit.type !== AAU_JOINT) { continue; }

            const cursor = new Cursor(unit.payload, 'an AAU_JOINT frame');
            const timestamp = cursor.uint32();
            const driven = cursor.uint32();

            const matrices = new Float32Array(jointCount * 16);
            for (let j = 0; j < jointCount; j++) { matrices[j * 16] = matrices[j * 16 + 5] = matrices[j * 16 + 10] = matrices[j * 16 + 15] = 1; }

            for (let j = 0; j < driven; j++)
            {
                const index = cursor.uint32();
                if (index >= jointCount) { throw new Error('a frame drives joint index ' + index + ' of ' + jointCount); }
                matrices.set(cursor.floats(16), index * 16);
            }

            frames.push({ timestamp, matrices });
        }

        if (frames.length === 0) { throw new Error('the joint animation stream has no AAU_JOINT frames'); }

        this.timescale = timescale > 0 ? timescale : 30;
        this.numberOfFrames = frames.length;
        this.frames = frames;
    }

    get duration() { return this.numberOfFrames / this.timescale; }
}

/* ===========================================================================
 *  Posing
 * =========================================================================*/

/** result = a * b, row-major.  Written out rather than looped: this runs
 *  numberOfJoints times per frame and the flat form is measurably quicker. */
function multiply4x4(out, a, ao, b, bo)
{
    for (let row = 0; row < 4; row++)
    {
        const a0 = a[ao + row*4], a1 = a[ao + row*4 + 1], a2 = a[ao + row*4 + 2], a3 = a[ao + row*4 + 3];
        for (let column = 0; column < 4; column++)
        {
            out[row*4 + column] = a0 * b[bo + column]        + a1 * b[bo + 4 + column] +
                                  a2 * b[bo + 8 + column]    + a3 * b[bo + 12 + column];
        }
    }
}

class Pose
{
    constructor(avatar)
    {
        this.avatar = avatar;
        const joints = avatar.nodes.length;
        const vertices = avatar.numberOfVertices;

        this.jointGlobals = new Float32Array(joints * 16);
        this.skinMatrices = new Float32Array(joints * 16);
        this.positions    = new Float32Array(vertices * 3);
        this.normals      = new Float32Array(vertices * 3);
        this.scratch      = new Float32Array(16);
        this.frame        = -1;
    }

    /** Hierarchy compose, skin matrices, linear blend skinning and normals. */
    evaluate(frame)
    {
        if (frame === this.frame) { return; }
        this.frame = frame;

        const avatar = this.avatar;
        const nodes  = avatar.nodes;
        const locals = avatar.frames[frame].matrices;
        const globals = this.jointGlobals;
        const scratch = this.scratch;

        /* One forward pass: every parent precedes its children. */
        for (let j = 0; j < nodes.length; j++)
        {
            const parent = nodes[j].parent;
            if (parent < 0)
            {
                globals.set(locals.subarray(j * 16, j * 16 + 16), j * 16);
            }
            else
            {
                multiply4x4(scratch, globals, parent * 16, locals, j * 16);
                globals.set(scratch, j * 16);
            }
        }

        const skinMatrices = this.skinMatrices;
        for (let j = 0; j < nodes.length; j++)
        {
            multiply4x4(scratch, globals, j * 16, avatar.inverseBindMatrices, j * 16);
            skinMatrices.set(scratch, j * 16);
        }

        this.skinVertices();
        this.recomputeNormals();
    }

    skinVertices()
    {
        const avatar = this.avatar;
        const rest = avatar.positions, out = this.positions;
        const start = avatar.skinStart, joint = avatar.skinJoint, weight = avatar.skinWeight;
        const m = this.skinMatrices;

        for (let v = 0; v < avatar.numberOfVertices; v++)
        {
            const rx = rest[v*3], ry = rest[v*3 + 1], rz = rest[v*3 + 2];
            let x = 0, y = 0, z = 0;

            for (let e = start[v]; e < start[v + 1]; e++)
            {
                const o = joint[e] * 16, w = weight[e];
                x += w * (m[o]     * rx + m[o + 1] * ry + m[o + 2]  * rz + m[o + 3]);
                y += w * (m[o + 4] * rx + m[o + 5] * ry + m[o + 6]  * rz + m[o + 7]);
                z += w * (m[o + 8] * rx + m[o + 9] * ry + m[o + 10] * rz + m[o + 11]);
            }

            out[v*3] = x; out[v*3 + 1] = y; out[v*3 + 2] = z;
        }
    }

    /** Area-weighted smooth vertex normals from the posed positions.
     *
     *  Mandatory every frame: rest-pose normals on a bent limb point somewhere
     *  the surface no longer faces, so the lit/shadowed split falls wherever
     *  the stale normal crosses N.L=0 — sharp shading bands across limbs. */
    recomputeNormals()
    {
        const p = this.positions, n = this.normals, index = this.avatar.indices;
        n.fill(0);

        for (let t = 0; t < index.length; t += 3)
        {
            const i0 = index[t] * 3, i1 = index[t + 1] * 3, i2 = index[t + 2] * 3;

            const e1x = p[i1] - p[i0], e1y = p[i1+1] - p[i0+1], e1z = p[i1+2] - p[i0+2];
            const e2x = p[i2] - p[i0], e2y = p[i2+1] - p[i0+1], e2z = p[i2+2] - p[i0+2];

            /* Left unnormalised: the cross product's length is twice the
             * triangle area, which is the area weighting, for free. */
            const fx = e1y*e2z - e1z*e2y;
            const fy = e1z*e2x - e1x*e2z;
            const fz = e1x*e2y - e1y*e2x;

            n[i0] += fx; n[i0+1] += fy; n[i0+2] += fz;
            n[i1] += fx; n[i1+1] += fy; n[i1+2] += fz;
            n[i2] += fx; n[i2+1] += fy; n[i2+2] += fz;
        }

        for (let i = 0; i < n.length; i += 3)
        {
            const x = n[i], y = n[i+1], z = n[i+2];
            const length = Math.sqrt(x*x + y*y + z*z);
            if (length > 1e-12) { n[i] = x/length; n[i+1] = y/length; n[i+2] = z/length; }
        }
    }

    /** Axis-aligned bounds of the current posed mesh. */
    bounds()
    {
        const p = this.positions;
        const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];

        for (let i = 0; i < p.length; i += 3)
        {
            for (let c = 0; c < 3; c++)
            {
                if (p[i+c] < min[c]) { min[c] = p[i+c]; }
                if (p[i+c] > max[c]) { max[c] = p[i+c]; }
            }
        }
        return { min, max };
    }
}

global.ARF = { Avatar, Pose, unzip };

})(typeof window !== 'undefined' ? window : globalThis);
