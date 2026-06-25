// Multi-mesh REAL Babylon Lite scene — three meshes (box, sphere, torus) with distinct
// standard materials + positions, one camera, one directional light. Validates that the
// NATIVE draw loop generalizes beyond a single mesh: it iterates N opaque DrawBindings,
// dedups/sets the right pipeline per binding, and binds each mesh's own group-1 bind
// group — all by reading exposed DrawCommand data (zero JS per frame). Setup is real Lite.

import {
    createEngine,
    createSceneContext,
    createBox,
    createSphere,
    createTorus,
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

    const cam = createArcRotateCamera(1.0, 1.2, 7, { x: 0, y: 0, z: 0 });
    cam.nearPlane = 0.1;
    cam.farPlane = 100;
    scene.camera = cam;

    addToScene(scene, createDirectionalLight([-0.5, -1, -0.6]));

    const box = createBox(engine, 1.2);
    const boxMat = createStandardMaterial();
    boxMat.diffuseColor = [0.9, 0.4, 0.2];
    box.material = boxMat;
    box.position.x = -2.2;
    addToScene(scene, box);

    const sphere = createSphere(engine, { diameter: 1.4 });
    const sphereMat = createStandardMaterial();
    sphereMat.diffuseColor = [0.2, 0.7, 0.9];
    sphere.material = sphereMat;
    addToScene(scene, sphere);

    const torus = createTorus(engine, { diameter: 1.4, thickness: 0.45 });
    const torusMat = createStandardMaterial();
    torusMat.diffuseColor = [0.4, 0.9, 0.3];
    torus.material = torusMat;
    torus.position.x = 2.2;
    addToScene(scene, torus);

    console.log("real Lite: multi-mesh scene built (box + sphere + torus)");
    await registerScene(scene);
    console.log("real Lite: scene registered");

    await startEngine(engine);
    console.log("real Lite: startEngine returned");
}

main().catch((e) => console.log("scene error: " + (e && e.stack ? e.stack : e)));
