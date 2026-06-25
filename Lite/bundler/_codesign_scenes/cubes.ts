// Draw-call-heavy benchmark scene — a 20x20x20 grid of cube nodes (~8000 separate
// meshes/draws) loaded from cubes.glb. Unlike the single-mesh BoomBox scene (whose
// opaque draws collapse into one cached render bundle, hiding per-draw cost), this
// scene calls setForceNoOpaqueBundle(true) so the JS render loop re-records every
// opaque draw each frame — the realistic dynamic-scene case and the apples-to-apples
// match for the native C++ loop (which always re-records). This exposes the per-draw
// encoder/N-API cost that the native render loop eliminates.
//
// Setup (createEngine, glTF/env load, scene build, setForceNoOpaqueBundle) is real Lite
// JS. Only the per-frame render loop is the variable under test (native vs JS).

import {
    createEngine,
    createSceneContext,
    loadGltf,
    loadEnvironment,
    createDefaultCamera,
    addToScene,
    registerScene,
    setForceNoOpaqueBundle,
    startEngine,
} from "babylon-lite";

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);

    console.log("cubes: fetching cubes.glb (8000 nodes) ...");
    addToScene(scene, await loadGltf(engine, "file:///D:/Repos/BabylonNative2/Lite/assets/cubes.glb"));
    console.log("cubes: cubes.glb added");

    console.log("cubes: loading environment (.env) ...");
    await loadEnvironment(scene, "https://assets.babylonjs.com/core/environments/environmentSpecular.env", {
        skipSkybox: true,
        skipGround: true,
        brdfUrl: "file:///D:/Repos/Babylon-Lite-3/packages/babylon-lite/assets/brdf-lut.png",
    });
    console.log("cubes: environment loaded");

    createDefaultCamera(scene);

    // Benchmark instrument: force per-frame opaque re-recording (no cached bundle).
    setForceNoOpaqueBundle(true);

    await registerScene(scene);
    console.log("cubes: scene registered");

    await startEngine(engine);
    console.log("cubes: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
