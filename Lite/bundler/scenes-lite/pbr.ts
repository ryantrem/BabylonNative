// PBR validation scene — a single sphere with a metallic-roughness PBR material, drawn by
// the NATIVE loop via the newly-exposed pbr-renderable DrawCommand (_draw). Confirms native
// can issue PBR draws (the material family glTF/BoomBox uses) by reading data, before the
// full glTF+env scene1. Setup is real Lite (createPbrMaterial + createSphere). No env/IBL,
// so lighting is direct-only — the goal here is the native PBR DRAW path, not full shading.

import {
    createEngine,
    createSceneContext,
    createSphere,
    createPbrMaterial,
    createArcRotateCamera,
    createDirectionalLight,
    addToScene,
    registerScene,
    startEngine,
} from "babylon-lite";

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);

    const cam = createArcRotateCamera(1.0, 1.2, 4, { x: 0, y: 0, z: 0 });
    cam.nearPlane = 0.1;
    cam.farPlane = 100;
    scene.camera = cam;

    addToScene(scene, createDirectionalLight([-0.5, -1, -0.6]));

    const sphere = createSphere(engine, { diameter: 1.6, segments: 32 });
    const mat = createPbrMaterial({
        baseColorFactor: [0.9, 0.5, 0.2, 1.0],
        metallicFactor: 0.1,
        roughnessFactor: 0.5,
    });
    sphere.material = mat;
    addToScene(scene, sphere);

    console.log("real Lite: PBR sphere scene built");
    await registerScene(scene);
    console.log("real Lite: scene registered");

    await startEngine(engine);
    console.log("real Lite: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
