// iOS variant of cubesbundle.ts — identical scene (20x20x20 cube grid, cached opaque
// GPURenderBundle, orbiting camera), but assets load from the app bundle via the `app://`
// URL scheme. JsRuntimeHost's UrlLib (UrlRequest_Apple.mm) resolves `app:///foo` to
// `[[NSBundle mainBundle] pathForResource:@"foo"]`, so cubes.glb / the .env / brdf-lut.png
// load from files bundled into the .app (a fully offline demo). The render path and engine
// code are unchanged from the desktop bundle scene.

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

    console.log("cubesios: loading cubes.glb (8000 nodes) ...");
    addToScene(scene, await loadGltf(engine, "app:///cubes.glb"));
    console.log("cubesios: cubes.glb added");

    console.log("cubesios: loading environment (.env) ...");
    await loadEnvironment(scene, "app:///environmentSpecular.env", {
        skipSkybox: true,
        skipGround: true,
        brdfUrl: "app:///brdf-lut.png",
    });
    console.log("cubesios: environment loaded");

    createDefaultCamera(scene);

    // Orbit the camera around the model every frame (framerate-independent via deltaMs).
    const camera = scene.camera;
    const orbitSpeedRadPerMs = 0.0004;
    scene._beforeRender.push((deltaMs) => {
        camera.alpha += orbitSpeedRadPerMs * deltaMs;
    });

    await registerScene(scene);
    console.log("cubesios: scene registered");

    await startEngine(engine);
    console.log("cubesios: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
