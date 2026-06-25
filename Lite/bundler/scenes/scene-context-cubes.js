// A scene authored in the upstream Babylon Lite "scene context" style (mirroring
// lab/lite/src/lite/scene1.ts): it builds a scene through createSceneContext /
// addToScene / createDefaultCamera / createHemisphericLight / registerScene, then
// hands off to startEngine().
//
// The point of this sample: ALL of those orchestration functions run on the JS side
// at setup — they are NOT in the render loop — so they're provided by the pure-JS
// Lite runtime (bundled in), layered over the native render-loop primitives
// (createEngine/createMaterial/createMesh/createNode/createAnimation/startEngine).
// Once startEngine() runs, the native loop owns every frame with zero engine JS.

import {
    createEngine,
    createSceneContext,
    addToScene,
    createDefaultCamera,
    createHemisphericLight,
    registerScene,
    startEngine,
    createMaterial,
    createMesh,
    createNode,
    createAnimation,
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

async function main() {
    // Engine + scene context: createEngine is native (brings up Dawn); the scene
    // context is JS bookkeeping.
    const engine = createEngine();
    const scene = createSceneContext(engine);

    const cube = cubeData();
    const plane = planeData(8);
    const TAU = Math.PI * 2;

    // Native materials (composed-WGSL permutations + cached pipelines).
    const mats = [
        createMaterial(engine, { lighting: true, vertexColor: true }),
        createMaterial(engine, { pbr: true, metallic: 1.0, roughness: 0.25, color: [1.0, 0.78, 0.34, 1] }),
        createMaterial(engine, { pbr: true, metallic: 0.0, roughness: 0.6, color: [0.85, 0.1, 0.12, 1] }),
        createMaterial(engine, { lighting: true, color: [0.2, 0.7, 0.9, 1] }),
    ];

    // Native scene-graph nodes + meshes + animations. addToScene tracks them in the
    // JS scene context (the meshes are already drawn by the native engine).
    const root = createNode(engine);
    root.setTransform([0, 0.4, 0], [0, 0, 0], [1, 1, 1]);
    const span = mats.length - 1;
    for (let i = 0; i < mats.length; i++) {
        const child = createNode(engine, root);
        child.setTransform([(i - span / 2) * 1.8, 0, 0], [0, 0, 0], [0.7, 0.7, 0.7]);
        const mesh = createMesh(engine, cube.positions, cube.normals, cube.uvs, cube.colors, cube.indices, mats[i], child);
        addToScene(scene, mesh);
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
    addToScene(scene, createMesh(engine, plane.positions, plane.normals, plane.uvs, plane.colors, plane.indices, groundMat, groundNode));

    // JS-side camera + light construction (scene1 style). createDefaultCamera builds
    // an ArcRotate-style camera over the native camera; setting alpha recomputes the
    // view in JS. createHemisphericLight returns a JS descriptor that addToScene
    // forwards to the native addLight.
    const cam = createDefaultCamera(scene, { radius: 8.5, target: [0, 0.3, 0], alpha: 1.2, beta: 1.15 });
    addToScene(scene, createHemisphericLight([0, 1, 0], 0.9));

    // Finalize the scene (setup-time), then hand off to the native render loop.
    await registerScene(scene);
    console.log("scene built via JS orchestration (scene context / addToScene / default camera / hemispheric light)");
    startEngine(engine);
}

main();
