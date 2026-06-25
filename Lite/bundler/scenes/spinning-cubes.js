// A Babylon Native Lite scene authored exactly like an upstream Lite scene: it
// imports from "babylon-lite" and never touches the native global directly. The
// bundler externalizes those imports onto Babylon Native Lite's `BabylonNativeLite`
// global, so this same source runs natively with zero engine JS in the render loop.
//
// It uses the subset of the API the native host implements today (engine, materials,
// meshes, scene-graph nodes, camera, lights, native keyframe animation). The render
// loop, scene-graph world-matrix updates, lighting, PBR, shadows, and animation all
// run in C++ — this file only describes the scene and then hands control to the
// native loop via startEngine().

import {
    createEngine,
    createMaterial,
    createMesh,
    createNode,
    createCamera,
    addLight,
    createAnimation,
    startEngine,
} from "babylon-lite";

function cubeData() {
    const faces = [
        { n: [0, 0, 1], v: [[-0.5, -0.5, 0.5], [0.5, -0.5, 0.5], [0.5, 0.5, 0.5], [-0.5, 0.5, 0.5]] },
        { n: [0, 0, -1], v: [[0.5, -0.5, -0.5], [-0.5, -0.5, -0.5], [-0.5, 0.5, -0.5], [0.5, 0.5, -0.5]] },
        { n: [1, 0, 0], v: [[0.5, -0.5, 0.5], [0.5, -0.5, -0.5], [0.5, 0.5, -0.5], [0.5, 0.5, 0.5]] },
        { n: [-1, 0, 0], v: [[-0.5, -0.5, -0.5], [-0.5, -0.5, 0.5], [-0.5, 0.5, 0.5], [-0.5, 0.5, -0.5]] },
        { n: [0, 1, 0], v: [[-0.5, 0.5, 0.5], [0.5, 0.5, 0.5], [0.5, 0.5, -0.5], [-0.5, 0.5, -0.5]] },
        { n: [0, -1, 0], v: [[-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0.5, -0.5, 0.5], [-0.5, -0.5, 0.5]] },
    ];
    const uvFace = [[0, 1], [1, 1], [1, 0], [0, 0]];
    const palette = [[0.9, 0.3, 0.3], [0.3, 0.9, 0.3], [0.3, 0.3, 0.9], [0.9, 0.9, 0.3], [0.9, 0.3, 0.9], [0.3, 0.9, 0.9]];
    const pos = [], nrm = [], uv = [], col = [], idx = [];
    for (let f = 0; f < 6; f++) {
        const base = f * 4;
        for (let i = 0; i < 4; i++) {
            pos.push(...faces[f].v[i]);
            nrm.push(...faces[f].n);
            uv.push(...uvFace[i]);
            col.push(...palette[f]);
        }
        idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
    }
    return {
        positions: new Float32Array(pos),
        normals: new Float32Array(nrm),
        uvs: new Float32Array(uv),
        colors: new Float32Array(col),
        indices: new Uint16Array(idx),
    };
}

function planeData(s) {
    return {
        positions: new Float32Array([-s, 0, -s, s, 0, -s, s, 0, s, -s, 0, s]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        uvs: new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]),
        colors: new Float32Array([1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]),
        indices: new Uint16Array([0, 1, 2, 0, 2, 3]),
    };
}

const engine = createEngine();
const cube = cubeData();
const plane = planeData(8);

const TAU = Math.PI * 2;
const mats = [
    createMaterial(engine, { lighting: true, vertexColor: true }),
    createMaterial(engine, { pbr: true, metallic: 1.0, roughness: 0.25, color: [1.0, 0.78, 0.34, 1] }),
    createMaterial(engine, { pbr: true, metallic: 0.0, roughness: 0.6, color: [0.85, 0.1, 0.12, 1] }),
    createMaterial(engine, { lighting: true, color: [0.2, 0.7, 0.9, 1] }),
];

const root = createNode(engine);
root.setTransform([0, 0.4, 0], [0, 0, 0], [1, 1, 1]);
const span = mats.length - 1;
for (let i = 0; i < mats.length; i++) {
    const child = createNode(engine, root);
    child.setTransform([(i - span / 2) * 1.8, 0, 0], [0, 0, 0], [0.7, 0.7, 0.7]);
    createMesh(engine, cube.positions, cube.normals, cube.uvs, cube.colors, cube.indices, mats[i], child);
    createAnimation(engine, child, {
        property: "rotation",
        keys: [{ time: 0, value: [0, 0, 0] }, { time: 1, value: [TAU * 2, TAU * 2, 0] }],
        duration: 3 + i * 0.6,
        loop: true,
    });
}
createAnimation(engine, root, {
    property: "rotation",
    keys: [{ time: 0, value: [0, 0, 0] }, { time: 6, value: [0, TAU, 0] }],
    duration: 6,
    loop: true,
});

const groundMat = createMaterial(engine, { lighting: true, color: [0.6, 0.6, 0.62, 1] });
const groundNode = createNode(engine);
groundNode.setTransform([0, -1.5, 0], [0, 0, 0], [1, 1, 1]);
createMesh(engine, plane.positions, plane.normals, plane.uvs, plane.colors, plane.indices, groundMat, groundNode);

const camera = createCamera(engine);
camera.setProjection(0.8, 1280.0 / 720.0, 0.1, 100.0);
camera.setView([0, 3.2, 8.0], [0, 0.3, 0], [0, 1, 0]);

addLight(engine, { type: "directional", direction: [-0.4, -1.0, -0.6], color: [1.0, 0.97, 0.9], intensity: 0.9 });
addLight(engine, { type: "point", position: [2.5, 1.5, 2.5], color: [0.3, 0.5, 1.0], intensity: 6.0 });

console.log("scene authored via 'babylon-lite' imports; handing off to the native loop");
startEngine(engine);
