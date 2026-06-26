// Bundle-path benchmark scene — same 20x20x20 cube grid (~8000 draws) as cubes.ts, but
// WITHOUT setForceNoOpaqueBundle. Lite's record() therefore builds a cached opaque
// GPURenderBundle (task._opaqueBundles[0]) at registerScene, and both the JS render loop
// (executePassBody) and the native render loop replay it with a single executeBundles call
// — rebuilt only when the renderable set / visibility changes (_renderableVersion / _vis).
// This is the PRODUCTION render path (static-structure scene), the realistic counterpart to
// cubes.ts's worst-case per-draw re-recording. Comparing the two isolates the win from
// retained-mode bundle caching, and comparing native-vs-js on THIS scene shows the residual
// binding-layer overhead once per-draw cost is removed on both sides.
//
// Setup (createEngine, glTF/env load, scene build) is real Lite JS. Only the per-frame
// render loop is the variable under test (native vs JS).

import {
    createEngine,
    createSceneContext,
    loadGltf,
    loadEnvironment,
    createDefaultCamera,
    addToScene,
    registerScene,
    startEngine,
} from "babylon-lite";

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);

    console.log("cubesbundle: fetching cubes.glb (8000 nodes) ...");
    addToScene(scene, await loadGltf(engine, "file:///D:/Repos/BabylonNative2/Lite/assets/cubes.glb"));
    console.log("cubesbundle: cubes.glb added");

    console.log("cubesbundle: loading environment (.env) ...");
    await loadEnvironment(scene, "https://assets.babylonjs.com/core/environments/environmentSpecular.env", {
        skipSkybox: true,
        skipGround: true,
        brdfUrl: "file:///D:/Repos/Babylon-Lite-3/packages/babylon-lite/assets/brdf-lut.png",
    });
    console.log("cubesbundle: environment loaded");

    createDefaultCamera(scene);

    // Orbit the camera around the model every frame so frame-rate smoothness is
    // visually observable. `scene._beforeRender` callbacks run once per frame in
    // Lite's JS render loop and receive the frame delta in ms; `alpha` is the
    // ArcRotateCamera's rotation around the Y axis. ~0.4 rad/s (a full revolution
    // every ~16 s), framerate-independent via deltaMs.
    const camera = scene.camera;
    const orbitSpeedRadPerMs = 0.0004;
    scene._beforeRender.push((deltaMs) => {
        camera.alpha += orbitSpeedRadPerMs * deltaMs;
    });

    // No setForceNoOpaqueBundle — Lite builds + caches the opaque render bundle at setup.
    await registerScene(scene);
    console.log("cubesbundle: scene registered");

    await startEngine(engine);
    console.log("cubesbundle: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
