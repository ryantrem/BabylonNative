// Demo — NativeLiteBenchmark (shared cross-implementation benchmark scene)
//
// Source of truth: CedricGuillemet/Babylon-Lite @ NativeBenchmark,
// lab/lite/src/demos/native-lite-benchmark.ts. Kept byte-identical in behavior so the
// numbers are comparable across implementations — only the brdf asset URL is adapted to
// our bundler (Cedric resolves it via demoAssetUrl(import.meta.url); we point at the
// Lite package asset directly, same as the other scenes-lite bundles).
//
// The Khronos BoomBox glTF (PBR) is loaded ONCE and deep-cloned into a 20x20 grid (400
// INDIVIDUAL meshes — one draw call each, sharing the source GPU buffers). A directional
// "sun" with a cascaded shadow map (CSM) casts shadows from every boombox onto a ground
// plane, and an ArcRotate camera is auto-framed to fit the entire grid.

import {
    addToScene,
    attachControl,
    cloneTransformNode,
    createArcRotateCamera,
    createCsmDirectionalShadowGenerator,
    createDirectionalLight,
    createEngine,
    createGround,
    createHemisphericLight,
    createSceneContext,
    createStandardMaterial,
    getContainerMeshes,
    loadEnvironment,
    loadGltf,
    registerSceneWithShadowSupport,
    setShadowTaskCasterMeshes,
    startEngine,
} from "babylon-lite";
import type { Mesh, SceneNode } from "babylon-lite";

// 20x20 = 400 instances.
const GRID = 20;
// Uniform world-space scale applied to each BoomBox copy (BoomBox is authored at
// a tiny ~decimetre scale). The grid spacing and camera framing are derived from
// the model's measured footprint, so this is just an absolute-size knob.
const MODEL_SCALE = 10;

const BOOMBOX_URL = "https://playground.babylonjs.com/scenes/BoomBox.glb";
const ENV_URL = "https://assets.babylonjs.com/core/environments/environmentSpecular.env";
const SKYBOX_URL = "https://assets.babylonjs.com/core/environments/backgroundSkybox.dds";
const BRDF_URL = "file:///D:/Repos/Babylon-Lite-3/packages/babylon-lite/assets/brdf-lut.png";

/** World-space AABB of the loaded template (before grid scale), from its meshes' bounds. */
function templateWorldAabb(meshes: readonly Mesh[]): {
    min: [number, number, number];
    max: [number, number, number];
} {
    let minX = Infinity,
        minY = Infinity,
        minZ = Infinity,
        maxX = -Infinity,
        maxY = -Infinity,
        maxZ = -Infinity;
    for (const mesh of meshes) {
        const w = mesh.worldMatrix;
        const bmin = mesh.boundMin ?? [-0.5, -0.5, -0.5];
        const bmax = mesh.boundMax ?? [0.5, 0.5, 0.5];
        for (let c = 0; c < 8; c++) {
            const lx = c & 1 ? bmax[0]! : bmin[0]!;
            const ly = c & 2 ? bmax[1]! : bmin[1]!;
            const lz = c & 4 ? bmax[2]! : bmin[2]!;
            const wx = w[0]! * lx + w[4]! * ly + w[8]! * lz + w[12]!;
            const wy = w[1]! * lx + w[5]! * ly + w[9]! * lz + w[13]!;
            const wz = w[2]! * lx + w[6]! * ly + w[10]! * lz + w[14]!;
            minX = Math.min(minX, wx);
            maxX = Math.max(maxX, wx);
            minY = Math.min(minY, wy);
            maxY = Math.max(maxY, wy);
            minZ = Math.min(minZ, wz);
            maxZ = Math.max(maxZ, wz);
        }
    }
    if (!Number.isFinite(minX)) {
        return { min: [-0.5, -0.5, -0.5], max: [0.5, 0.5, 0.5] };
    }
    return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

async function main(): Promise<void> {
    const __initStart = performance.now();
    const canvas = document.getElementById("renderCanvas") as HTMLCanvasElement;
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);
    scene.clearColor = { r: 0.6, g: 0.7, b: 0.85, a: 1 };

    // Load the BoomBox once + image-based lighting.
    const container = await loadGltf(engine, BOOMBOX_URL);
    await loadEnvironment(scene, ENV_URL, {
        skyboxUrl: SKYBOX_URL,
        skyboxSize: 4000,
        skipGround: true, // we add our own shadow-receiving ground below
        brdfUrl: BRDF_URL,
    });

    // Measure the template (one un-scaled copy) so spacing / ground / camera all derive
    // from the model — then clone it into the grid.
    const templateRoot = container.entities[0] as SceneNode;
    const aabb = templateWorldAabb(getContainerMeshes(container));
    const sizeX = (aabb.max[0] - aabb.min[0]) * MODEL_SCALE;
    const sizeZ = (aabb.max[2] - aabb.min[2]) * MODEL_SCALE;
    const spacing = Math.max(sizeX, sizeZ) * 1.4;
    const bottomY = aabb.min[1] * MODEL_SCALE; // grid copies rest on this Y plane
    const centerY = ((aabb.min[1] + aabb.max[1]) * 0.5) * MODEL_SCALE;

    // Clone the loaded BoomBox into a 20x20 grid of INDIVIDUAL meshes. Each clone is a full
    // hierarchy whose meshes shallow-share the source GPU buffers (no re-upload),
    // positioned/scaled via its own root transform — so every copy is its own draw call
    // (no instancing). The original template is never added, so only the 400 clones render.
    const half = (GRID - 1) / 2;
    const clones: SceneNode[] = [];
    for (let i = 0; i < GRID; i++) {
        for (let j = 0; j < GRID; j++) {
            const clone = cloneTransformNode(templateRoot);
            clone.position.set((i - half) * spacing, 0, (j - half) * spacing);
            // Apply MODEL_SCALE on top of the template root's own scale (the glTF RH->LH -Y
            // flip lives there), keeping each clone correctly oriented.
            clone.scaling.set(templateRoot.scaling.x * MODEL_SCALE, templateRoot.scaling.y * MODEL_SCALE, templateRoot.scaling.z * MODEL_SCALE);
            addToScene(scene, clone);
            clones.push(clone);
        }
    }

    // Every clone's meshes cast shadows.
    const casterMeshes = getContainerMeshes({ entities: clones });

    // Ground (shadow receiver).
    const groundSize = GRID * spacing + Math.max(sizeX, sizeZ) * 2;
    const ground = createGround(engine, { width: groundSize, height: groundSize, subdivisions: 1 });
    const groundMat = createStandardMaterial();
    groundMat.diffuseColor = [0.55, 0.55, 0.6];
    groundMat.specularColor = [0, 0, 0];
    ground.material = groundMat;
    ground.position.set(0, bottomY - 0.05, 0);
    ground.receiveShadows = true;
    addToScene(scene, ground);

    // Lights.
    addToScene(scene, createHemisphericLight([0, 1, 0], 0.5));
    const sun = createDirectionalLight([-0.4, -1, -0.55], 1.0);
    sun.diffuse = [1.0, 0.97, 0.9];
    addToScene(scene, sun);

    // Cascaded shadow map — handles the wide spread of the cloned casters.
    sun.shadowGenerator = createCsmDirectionalShadowGenerator(engine, sun, {
        mapSize: 2048,
        numCascades: 4,
        lambda: 0.7,
        bias: 0.0008,
    });
    setShadowTaskCasterMeshes(sun.shadowGenerator, casterMeshes);

    // Camera framed to fit the whole grid.
    const gridHalfExtent = half * spacing + Math.max(sizeX, sizeZ) * 0.5;
    const radius = gridHalfExtent * 2.6;
    const cam = createArcRotateCamera(Math.PI / 4, Math.PI / 3.2, radius, { x: 0, y: centerY, z: 0 });
    cam.nearPlane = Math.max(0.5, radius * 0.01);
    cam.farPlane = radius * 6;
    scene.camera = cam;
    attachControl(cam, canvas, scene);

    await registerSceneWithShadowSupport(scene);
    await startEngine(engine);

    canvas.dataset.drawCalls = String(engine.drawCallCount);
    canvas.dataset.meshes = String(GRID * GRID);
    canvas.dataset.initMs = String(performance.now() - __initStart);
    canvas.dataset.ready = "true";
    console.log("native-lite-benchmark: registered (" + (GRID * GRID) + " boomboxes, " + engine.drawCallCount + " draw calls)");
}

main().catch((err) => {
    console.log("scene error: " + (err && err.stack ? err.stack : err));
});
