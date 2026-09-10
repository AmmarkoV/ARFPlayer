/** @file player.js
 *  @brief WebGL2 viewer for the containers arf.js reads: orbit camera,
 *         playback clock, timeline.  The browser counterpart of src/player.
 *
 *  No dependencies — raw WebGL2, one shader program, and the page's own form
 *  controls for the timeline rather than a drawn one.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

(function () {
'use strict';

/* The body shaders, matching src/render/default.{vert,frag} at --shiny 0:
 * half-Lambert wrap lighting, which avoids the hard N.L=0 terminator that
 * makes individual triangles pop on a faceted normal field, plus a cheap
 * dither to break up 8-bit banding on the gradient. */
const VERTEX_SHADER = `#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
uniform mat4 uMVP;
out vec3 vNorm;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNorm = aNorm;
}`;

const FRAGMENT_SHADER = `#version 300 es
precision highp float;
in vec3 vNorm;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNorm);
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));

    float d = clamp((dot(N, L) + 0.15) / 1.15, 0.0, 1.0);
    d = d * d * (3.0 - 2.0 * d);
    d = d * 0.65 + 0.35;

    vec3 col = uColor * d;
    col += (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
    fragColor = vec4(col, 1.0);
}`;

/* ===========================================================================
 *  Matrices -- row-major, matching the container's own convention.
 * =========================================================================*/

function multiply(a, b)
{
    const out = new Float32Array(16);
    for (let row = 0; row < 4; row++)
    {
        for (let column = 0; column < 4; column++)
        {
            out[row*4 + column] = a[row*4]*b[column] + a[row*4+1]*b[4+column] +
                                  a[row*4+2]*b[8+column] + a[row*4+3]*b[12+column];
        }
    }
    return out;
}

/** WebGL, unlike desktop GL, refuses a transpose flag on uniformMatrix4fv, so
 *  row-major matrices are flipped here on the way to the GPU. */
function transpose(m)
{
    const out = new Float32Array(16);
    for (let row = 0; row < 4; row++)
    {
        for (let column = 0; column < 4; column++) { out[row*4 + column] = m[column*4 + row]; }
    }
    return out;
}

/* ===========================================================================
 *  Orbit camera
 *
 *  ARF data is centimetres, +Y up, and the avatar both sits at positive Z and
 *  FACES +Z.  So the front view is an eye on the +Z side looking back toward
 *  -Z, which is yaw = 0.  Put it on the -Z side and you film the avatar's back.
 * =========================================================================*/

class Camera
{
    constructor()
    {
        this.target = [0, 0, 0];
        this.distance = 300;
        this.yaw = 0;
        this.pitch = 0;
        this.fieldOfView = 45;
        this.near = 1;
        this.far = 10000;
    }

    frame(centre, radius)
    {
        this.target = centre.slice();
        this.yaw = 0;
        this.pitch = 0;
        this.distance = 1.15 * Math.max(radius, 1e-3) / Math.tan(0.5 * this.fieldOfView * Math.PI / 180);
        this.near = this.distance * 0.01;
        this.far  = this.distance * 20;
    }

    eye()
    {
        const cosPitch = Math.cos(this.pitch);
        return [
            this.target[0] + this.distance * cosPitch * Math.sin(this.yaw),
            this.target[1] + this.distance * Math.sin(this.pitch),
            this.target[2] + this.distance * cosPitch * Math.cos(this.yaw)
        ];
    }

    orbit(dx, dy)
    {
        this.yaw   += dx;
        this.pitch += dy;

        /* Stop short of the poles, where the up vector would become parallel
         * to the forward vector and collapse the basis. */
        const limit = 1.55;
        this.pitch = Math.max(-limit, Math.min(limit, this.pitch));
    }

    zoom(factor) { this.distance = Math.max(1, this.distance * factor); }

    /** Slide the target across the view plane; pixels, scaled by distance so
     *  panning feels the same at any zoom. */
    pan(dx, dy, viewportHeight)
    {
        const scale = 2 * this.distance * Math.tan(0.5 * this.fieldOfView * Math.PI / 180) / Math.max(viewportHeight, 1);
        const right = [Math.cos(this.yaw), 0, -Math.sin(this.yaw)];
        const up = [-Math.sin(this.pitch) * Math.sin(this.yaw), Math.cos(this.pitch), -Math.sin(this.pitch) * Math.cos(this.yaw)];

        for (let c = 0; c < 3; c++) { this.target[c] += -right[c] * dx * scale + up[c] * dy * scale; }
    }

    view()
    {
        const eye = this.eye();
        let forward = [this.target[0] - eye[0], this.target[1] - eye[1], this.target[2] - eye[2]];
        let length = Math.hypot(forward[0], forward[1], forward[2]) || 1e-9;
        forward = forward.map(v => v / length);

        /* forward x (0,1,0), written out. */
        let right = [-forward[2], 0, forward[0]];
        length = Math.hypot(right[0], right[1], right[2]) || 1e-9;
        right = right.map(v => v / length);

        const up = [
            right[1]*forward[2] - right[2]*forward[1],
            right[2]*forward[0] - right[0]*forward[2],
            right[0]*forward[1] - right[1]*forward[0]
        ];

        return new Float32Array([
            right[0],   right[1],   right[2],   -(right[0]*eye[0] + right[1]*eye[1] + right[2]*eye[2]),
            up[0],      up[1],      up[2],      -(up[0]*eye[0] + up[1]*eye[1] + up[2]*eye[2]),
            -forward[0],-forward[1],-forward[2], (forward[0]*eye[0] + forward[1]*eye[1] + forward[2]*eye[2]),
            0, 0, 0, 1
        ]);
    }

    projection(aspect)
    {
        const focal = 1 / Math.tan(0.5 * this.fieldOfView * Math.PI / 180);
        const depth = this.near - this.far;

        return new Float32Array([
            focal / Math.max(aspect, 1e-6), 0, 0, 0,
            0, focal, 0, 0,
            0, 0, (this.far + this.near) / depth, (2 * this.far * this.near) / depth,
            0, 0, -1, 0
        ]);
    }
}

/* ===========================================================================
 *  Renderer
 * =========================================================================*/

function compile(gl, type, source, label)
{
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
    {
        throw new Error('cannot compile ' + label + ': ' + gl.getShaderInfoLog(shader));
    }
    return shader;
}

class Renderer
{
    constructor(canvas)
    {
        const gl = canvas.getContext('webgl2', { antialias: true, alpha: false });
        if (!gl) { throw new Error('this browser has no WebGL2'); }
        this.gl = gl;

        const program = gl.createProgram();
        gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, VERTEX_SHADER, 'the vertex shader'));
        gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER, 'the fragment shader'));
        gl.linkProgram(program);
        if (!gl.getProgramParameter(program, gl.LINK_STATUS))
        {
            throw new Error('cannot link the shaders: ' + gl.getProgramInfoLog(program));
        }

        this.program = program;
        this.uMVP = gl.getUniformLocation(program, 'uMVP');
        this.uColor = gl.getUniformLocation(program, 'uColor');

        gl.enable(gl.DEPTH_TEST);
    }

    /** Allocate the buffers for one avatar.  Positions and normals are
     *  rewritten every frame after CPU skinning; the topology never changes. */
    setAvatar(avatar)
    {
        const gl = this.gl;
        if (this.vao) { gl.deleteVertexArray(this.vao); }

        this.indexCount = avatar.indices.length;
        this.vao = gl.createVertexArray();
        gl.bindVertexArray(this.vao);

        const bytes = avatar.numberOfVertices * 3 * 4;

        this.positions = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.positions);
        gl.bufferData(gl.ARRAY_BUFFER, bytes, gl.DYNAMIC_DRAW);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

        this.normals = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.normals);
        gl.bufferData(gl.ARRAY_BUFFER, bytes, gl.DYNAMIC_DRAW);
        gl.enableVertexAttribArray(1);
        gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);

        const elements = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, elements);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, avatar.indices, gl.STATIC_DRAW);

        gl.bindVertexArray(null);
    }

    upload(pose)
    {
        const gl = this.gl;
        gl.bindBuffer(gl.ARRAY_BUFFER, this.positions);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, pose.positions);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.normals);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, pose.normals);
    }

    draw(width, height, modelViewProjection, color)
    {
        const gl = this.gl;
        gl.viewport(0, 0, width, height);
        gl.clearColor(0.09, 0.10, 0.12, 1);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

        if (!this.vao) { return; }

        gl.useProgram(this.program);
        gl.uniformMatrix4fv(this.uMVP, false, transpose(modelViewProjection));
        gl.uniform3fv(this.uColor, color);
        gl.bindVertexArray(this.vao);
        gl.drawElements(gl.TRIANGLES, this.indexCount, gl.UNSIGNED_INT, 0);
        gl.bindVertexArray(null);
    }
}

/* ===========================================================================
 *  The page
 * =========================================================================*/

const ui = id => document.getElementById(id);

const canvas = ui('view');
const camera = new Camera();
let renderer = null, avatar = null, pose = null;

let playing = true, speed = 1, looping = true;
let playhead = 0, previous = performance.now();
let framesDrawn = 0, fpsWindow = previous, measuredFPS = 0;
const color = new Float32Array([0.80, 0.75, 0.70]);

function status(text, isError)
{
    ui('status').textContent = text;
    ui('status').className = isError ? 'error' : '';
}

function framing()
{
    /* Centre on every sampled pose so a walking avatar stays in frame without
     * the camera following it, but size from the largest single pose so one
     * pose fills the view rather than the whole travel volume. */
    const unionMin = [Infinity, Infinity, Infinity], unionMax = [-Infinity, -Infinity, -Infinity];
    let largest = 0;

    const step = Math.max(1, Math.ceil(avatar.numberOfFrames / 24));
    for (let f = 0; f < avatar.numberOfFrames; f += step)
    {
        pose.evaluate(f);
        const b = pose.bounds();
        let radius = 0;
        for (let c = 0; c < 3; c++)
        {
            unionMin[c] = Math.min(unionMin[c], b.min[c]);
            unionMax[c] = Math.max(unionMax[c], b.max[c]);
            const half = 0.5 * (b.max[c] - b.min[c]);
            radius += half * half;
        }
        largest = Math.max(largest, Math.sqrt(radius));
    }

    pose.frame = -1;   /* the sampling left a stale frame cached */
    camera.frame(unionMin.map((v, c) => 0.5 * (v + unionMax[c])), largest);
}

async function open(arrayBuffer, label)
{
    status('reading ' + label + ' …');
    try
    {
        avatar = await ARF.Avatar.load(arrayBuffer);
        pose = new ARF.Pose(avatar);

        renderer.setAvatar(avatar);
        framing();

        const slider = ui('timeline');
        slider.max = avatar.numberOfFrames - 1;
        slider.value = 0;
        playhead = 0;
        playing = true;
        ui('play').textContent = '❚❚';

        status(avatar.name + ' — ' + avatar.nodes.length + ' joints, ' +
               avatar.numberOfVertices.toLocaleString() + ' vertices, ' +
               avatar.numberOfFrames + ' frames at ' + avatar.timescale.toFixed(0) + ' fps');
    }
    catch (problem)
    {
        avatar = null;
        status(label + ': ' + problem.message, true);
    }
}

function tick(now)
{
    requestAnimationFrame(tick);

    const elapsed = Math.min((now - previous) / 1000, 0.25);
    previous = now;

    const width  = Math.max(1, canvas.clientWidth  * devicePixelRatio | 0);
    const height = Math.max(1, canvas.clientHeight * devicePixelRatio | 0);
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }

    if (avatar)
    {
        if (playing)
        {
            playhead += elapsed * avatar.timescale * speed;
            if (playhead >= avatar.numberOfFrames)
            {
                if (looping) { playhead %= avatar.numberOfFrames; }
                else { playhead = avatar.numberOfFrames - 1; setPlaying(false); }
            }
            ui('timeline').value = Math.floor(playhead);
        }

        const frame = Math.min(Math.floor(playhead), avatar.numberOfFrames - 1);
        pose.evaluate(frame);
        renderer.upload(pose);

        ui('readout').textContent = 'frame ' + frame + '/' + (avatar.numberOfFrames - 1) + '  ·  ' +
            (frame / avatar.timescale).toFixed(2) + '/' + avatar.duration.toFixed(2) + ' s  ·  ' +
            measuredFPS.toFixed(0) + ' fps';
    }

    const modelViewProjection = multiply(camera.projection(width / height), camera.view());
    renderer.draw(width, height, modelViewProjection, color);

    framesDrawn++;
    if (now - fpsWindow >= 500) { measuredFPS = framesDrawn * 1000 / (now - fpsWindow); framesDrawn = 0; fpsWindow = now; }
}

function setPlaying(value)
{
    playing = value;
    ui('play').textContent = value ? '❚❚' : '▶';
}

/* -- input ---------------------------------------------------------------- */

let dragButton = -1, lastX = 0, lastY = 0;

canvas.addEventListener('pointerdown', e =>
{
    dragButton = e.button; lastX = e.clientX; lastY = e.clientY;
    canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener('pointerup', e => { dragButton = -1; canvas.releasePointerCapture(e.pointerId); });
canvas.addEventListener('pointermove', e =>
{
    if (dragButton < 0) { return; }
    const dx = e.clientX - lastX, dy = e.clientY - lastY;
    lastX = e.clientX; lastY = e.clientY;

    if (dragButton === 0) { camera.orbit(dx * 0.007, dy * 0.007); }
    else                  { camera.pan(dx, dy, canvas.clientHeight); }
});
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('wheel', e => { e.preventDefault(); camera.zoom(Math.pow(0.88, -Math.sign(e.deltaY))); }, { passive: false });

ui('play').addEventListener('click', () => setPlaying(!playing));
ui('loop').addEventListener('change', e => { looping = e.target.checked; });
ui('speed').addEventListener('change', e => { speed = parseFloat(e.target.value); });
ui('timeline').addEventListener('input', e => { setPlaying(false); playhead = parseInt(e.target.value, 10); });
ui('reset').addEventListener('click', () => { if (avatar) { framing(); } });

window.addEventListener('keydown', e =>
{
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') { return; }
    if (!avatar) { return; }

    if (e.key === ' ')          { setPlaying(!playing); e.preventDefault(); }
    else if (e.key === 'ArrowLeft')  { setPlaying(false); playhead = Math.max(0, Math.floor(playhead) - 1); }
    else if (e.key === 'ArrowRight') { setPlaying(false); playhead = Math.min(avatar.numberOfFrames - 1, Math.floor(playhead) + 1); }
    else if (e.key === 'Home')  { playhead = 0; }
    else if (e.key === 'End')   { playhead = avatar.numberOfFrames - 1; }
    else if (e.key === 'r')     { framing(); }
    else if (e.key === 'l')     { looping = !looping; ui('loop').checked = looping; }
    ui('timeline').value = Math.floor(playhead);
});

/* -- loading -------------------------------------------------------------- */

/** The containers committed to the repository, offered as one-click loads.
 *  https://github.com/AmmarkoV/ARFPlayer/tree/main/samples */
const SAMPLES = [
    { label: 'summerlove', file: 'summerlove_0.arfz' }
];

const SAMPLES_RAW = 'https://raw.githubusercontent.com/AmmarkoV/ARFPlayer/main/samples/';

/** Fetch the first of several candidate URLs that answers.
 *
 *  A sample is tried next to the page first, so a local checkout serves its
 *  own copy and works offline, then from the repository, which is what makes
 *  the buttons work when this page is hosted anywhere else.  GitHub serves raw
 *  files with an open CORS policy, so the cross-origin fetch is allowed. */
async function fetchFirst(urls)
{
    let lastProblem = null;
    for (const url of urls)
    {
        try
        {
            const response = await fetch(url);
            if (response.ok) { return await response.arrayBuffer(); }
            lastProblem = new Error(response.status + ' ' + response.statusText);
        }
        catch (problem) { lastProblem = problem; }
    }
    throw lastProblem || new Error('not found');
}

function loadSample(sample)
{
    status('fetching ' + sample.file + ' …');
    fetchFirst(['../samples/' + sample.file, SAMPLES_RAW + sample.file])
        .then(buffer => open(buffer, sample.file))
        .catch(problem => status('cannot fetch ' + sample.file + ': ' + problem.message, true));
}

for (const sample of SAMPLES)
{
    const button = document.createElement('button');
    button.textContent = sample.label;
    button.title = 'Load ' + sample.file;
    button.addEventListener('click', () => loadSample(sample));
    ui('samples').appendChild(button);
}

function openFile(file) { file.arrayBuffer().then(b => open(b, file.name)); }

ui('file').addEventListener('change', e => { if (e.target.files[0]) { openFile(e.target.files[0]); } });

document.addEventListener('dragover', e => { e.preventDefault(); document.body.classList.add('dragging'); });
document.addEventListener('dragleave', () => document.body.classList.remove('dragging'));
document.addEventListener('drop', e =>
{
    e.preventDefault();
    document.body.classList.remove('dragging');
    if (e.dataTransfer.files[0]) { openFile(e.dataTransfer.files[0]); }
});

/* -- start ---------------------------------------------------------------- */

try
{
    renderer = new Renderer(canvas);
    requestAnimationFrame(tick);

    /* ?url= overrides; otherwise open the first committed sample, looking
     * beside the page before falling back to the repository.  Straight off the
     * filesystem both fetches are blocked, which is not an error worth
     * shouting about -- drag a container in instead. */
    const requested = new URLSearchParams(location.search).get('url');
    const candidates = requested ? [requested]
                                 : ['../samples/' + SAMPLES[0].file, SAMPLES_RAW + SAMPLES[0].file];

    fetchFirst(candidates)
        .then(buffer => open(buffer, candidates[0].split('/').pop()))
        .catch(() => status('drag a .arfz container onto this page, or use the file button'));
}
catch (problem)
{
    status(problem.message, true);
}

})();
