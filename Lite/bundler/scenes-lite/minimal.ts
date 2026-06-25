// Minimal REAL Babylon Lite scene — stage 1: just bring up the engine + an empty
// scene context and start the loop. No meshes yet. This exercises real Lite's
// createEngine (navigator.gpu + canvas), createSceneContext, registerScene, and
// startEngine against our native WebGPU host. Grows mesh/material/camera/light next.

import {
    createEngine,
    createSceneContext,
    registerScene,
    startEngine,
} from "babylon-lite";

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    console.log("real Lite: engine created");

    const scene = createSceneContext(engine);
    console.log("real Lite: scene context created");

    await registerScene(scene);
    console.log("real Lite: scene registered");

    await startEngine(engine);
    console.log("real Lite: startEngine returned (first frame rendered)");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
