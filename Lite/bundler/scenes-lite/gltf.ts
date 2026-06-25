// glTF load scene — loads BoomBox.glb (real Lite loadGltf, using the now-enabled fetch
// polyfill) and renders it via the NATIVE draw loop using the PBR _draw data exposed in
// pbr-renderable. No env/IBL/skybox yet (those are the next layers) — a directional light
// gives direct shading so the model is visible. Setup (fetch + glTF parse + PBR material
// build + camera framing) is 100% real Lite; the render loop is native, zero JS/frame.

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

    console.log("glTF: fetching + parsing BoomBox.glb ...");
    const container = await loadGltf(engine, "https://playground.babylonjs.com/scenes/BoomBox.glb");
    addToScene(scene, container);
    console.log("glTF: BoomBox added to scene");

    const cam = createDefaultCamera(scene);
    cam.alpha = 1.77538207638442;

    await registerScene(scene);
    console.log("glTF: scene registered");

    await startEngine(engine);
    console.log("glTF: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
