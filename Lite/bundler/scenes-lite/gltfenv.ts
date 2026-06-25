// glTF + IBL scene — BoomBox with image-based lighting from a .env specular cubemap. This
// is where PBR finally shades correctly (metallic surfaces reflect the environment).
// loadEnvironment is real Lite SETUP (asset load + GPU prefilter), staying JS; the render
// loop is native. Skybox/ground intentionally skipped here to isolate the .env IBL path
// (specular cube + irradiance SH + BRDF LUT) from the background renderables.

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

    console.log("glTF+IBL: fetching BoomBox.glb ...");
    addToScene(scene, await loadGltf(engine, "https://playground.babylonjs.com/scenes/BoomBox.glb"));
    console.log("glTF+IBL: BoomBox added");

    console.log("glTF+IBL: loading environment (.env) ...");
    // brdfUrl is required by loadEnvironment. The BRDF LUT ships as a local asset in the
    // Lite package; point at it via a file:// URL (UrlLib/WinHTTP serves local files).
    await loadEnvironment(scene, "https://assets.babylonjs.com/core/environments/environmentSpecular.env", {
        skipSkybox: true,
        skipGround: true,
        brdfUrl: "file:///D:/Repos/Babylon-Lite-3/packages/babylon-lite/assets/brdf-lut.png",
    });
    console.log("glTF+IBL: environment loaded");

    const cam = createDefaultCamera(scene);
    cam.alpha = 1.77538207638442;

    await registerScene(scene);
    console.log("glTF+IBL: scene registered");

    await startEngine(engine);
    console.log("glTF+IBL: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
