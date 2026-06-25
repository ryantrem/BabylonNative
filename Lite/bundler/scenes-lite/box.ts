// Drawable REAL Babylon Lite scene — a box with a standard material, an arc-rotate
// camera, and a directional light. All SETUP runs as real unmodified Lite (createBox →
// createMeshFromData uploads geometry to GPU via our WebGPU; createStandardMaterial;
// createArcRotateCamera; createDirectionalLight; addToScene; registerScene). The render
// loop (startEngine) is externalized to the NATIVE loop, which reads these structures.

import {
    createEngine,
    createSceneContext,
    createBox,
    createStandardMaterial,
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

    const light = createDirectionalLight([-0.5, -1, -0.6]);
    addToScene(scene, light);

    const box = createBox(engine, 1);
    const mat = createStandardMaterial();
    mat.diffuseColor = [0.9, 0.5, 0.2];
    box.material = mat;
    addToScene(scene, box);

    console.log("real Lite: drawable scene built (box + material + camera + light)");
    await registerScene(scene);
    console.log("real Lite: scene registered");

    await startEngine(engine);
    console.log("real Lite: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
