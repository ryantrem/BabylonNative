// Phase-timed cubes load — isolates where the 8000-mesh load time goes (createEngine vs
// loadGltf [fetch+parse+GPU upload] vs registerScene [render-bundle build] vs startEngine),
// so we can compare V8 vs QuickJS per-phase and find why QuickJS load is disproportionately
// slow. Prints "PHASE <name> <ms>" lines an external harness greps.

import {
    createEngine,
    createSceneContext,
    loadGltf,
    createDefaultCamera,
    createDirectionalLight,
    addToScene,
    registerScene,
    startEngine,
} from "babylon-lite";

function now() { return (typeof performance !== "undefined" && performance.now) ? performance.now() : Date.now(); }

async function main() {
    const t0 = now();
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);
    const tEngine = now();
    console.log("PHASE createEngine " + (tEngine - t0).toFixed(1));

    addToScene(scene, createDirectionalLight([-0.5, -1, -0.6]));

    const tGltf0 = now();
    const container = await loadGltf(engine, "file:///D:/Repos/BabylonNative2/Lite/assets/cubes.glb");
    const tGltf1 = now();
    console.log("PHASE loadGltf " + (tGltf1 - tGltf0).toFixed(1));

    addToScene(scene, container);
    createDefaultCamera(scene);
    const tAdd = now();
    console.log("PHASE addToScene+camera " + (tAdd - tGltf1).toFixed(1));

    await registerScene(scene);
    const tReg = now();
    console.log("PHASE registerScene " + (tReg - tAdd).toFixed(1));

    await startEngine(engine);
    const tStart = now();
    console.log("PHASE startEngine " + (tStart - tReg).toFixed(1));
    console.log("PHASE TOTAL " + (tStart - t0).toFixed(1));
    console.log("cubestimed: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
