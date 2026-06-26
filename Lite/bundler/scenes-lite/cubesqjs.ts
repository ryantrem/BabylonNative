// QuickJS cubes benchmark scene — the 20x20x20 cube grid (~8000 draws) on the PRODUCTION
// render-bundle path, but WITHOUT loadEnvironment/IBL (a directional light instead). This
// isolates the per-frame render-loop cost of 8000 cubes (cached opaque GPURenderBundle ->
// one executeBundles/frame) for the QuickJS-vs-V8 comparison, and avoids the heavier .env
// prefilter path. Setup (createEngine, glTF parse of 8000 nodes, scene build) is real Lite JS.

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

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);

    addToScene(scene, createDirectionalLight([-0.5, -1, -0.6]));

    console.log("cubesqjs: fetching cubes.glb (8000 nodes) ...");
    addToScene(scene, await loadGltf(engine, "file:///D:/Repos/BabylonNative2/Lite/assets/cubes.glb"));
    console.log("cubesqjs: cubes.glb added");

    createDefaultCamera(scene);

    // Orbit the camera around the model every frame so frame-rate smoothness is
    // visually observable. ~0.4 rad/s, deltaMs-scaled (framerate-independent).
    const camera = scene.camera;
    const orbitSpeedRadPerMs = 0.0004;
    scene._beforeRender.push((deltaMs) => {
        camera.alpha += orbitSpeedRadPerMs * deltaMs;
    });

    await registerScene(scene);
    console.log("cubesqjs: scene registered");

    await startEngine(engine);
    console.log("cubesqjs: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
