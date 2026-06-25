// Introspection: build a DRAWABLE box scene (box + material + camera + light), then
// dump the POPULATED per-frame structures native renderFrame must read. Does not call
// startEngine. Grounds the Lite co-design + native loop in real, populated data.

import {
    createEngine,
    createSceneContext,
    createBox,
    createStandardMaterial,
    createArcRotateCamera,
    createDirectionalLight,
    addToScene,
    registerScene,
    getFrameGraph,
} from "babylon-lite";

function typeOf(v) {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    if (Array.isArray(v)) return "array[" + v.length + "]";
    const t = typeof v;
    if (t === "object") return "obj:" + (v.constructor && v.constructor.name ? v.constructor.name : "?");
    return t;
}

function dump(label, obj) {
    if (obj == null || typeof obj !== "object") {
        console.log(label + " = " + typeOf(obj));
        return;
    }
    console.log(label + " {ctor=" + (obj.constructor && obj.constructor.name) + "}");
    for (const k of Object.keys(obj)) {
        let v;
        try { v = obj[k]; } catch (e) { v = "<throw>"; }
        console.log("  ." + k + " : " + typeOf(v));
    }
}

async function main() {
    const canvas = document.getElementById("renderCanvas");
    const engine = await createEngine(canvas);
    const scene = createSceneContext(engine);

    const cam = createArcRotateCamera(1.0, 1.2, 4, { x: 0, y: 0, z: 0 });
    cam.nearPlane = 0.1; cam.farPlane = 100;
    scene.camera = cam;
    addToScene(scene, createDirectionalLight([-0.5, -1, -0.6]));

    const box = createBox(engine, 1);
    const mat = createStandardMaterial();
    mat.diffuseColor = [0.9, 0.5, 0.2];
    box.material = mat;
    addToScene(scene, box);

    await registerScene(scene);
    console.log("=== BOX scene populated structures ===");

    dump("box", box);
    dump("box._gpu", box._gpu);

    const fg = getFrameGraph(scene);
    console.log("frameGraph._tasks length = " + fg._tasks.length);
    const t = fg._tasks[0];
    dump("task", t);
    console.log("task._opaqueBindings.length = " + t._opaqueBindings.length);
    console.log("task._directBindings.length = " + t._directBindings.length);
    console.log("task._transparentBindings.length = " + t._transparentBindings.length);
    console.log("task._opaqueBundles.length = " + t._opaqueBundles.length);
    console.log("task._passes.length = " + t._passes.length);

    if (t._opaqueBindings.length > 0) dump("opaqueBindings[0]", t._opaqueBindings[0]);
    if (t._passes.length > 0) {
        dump("passes[0]", t._passes[0]);
        dump("passes[0]._renderTarget", t._passes[0]._renderTarget);
    }
    dump("task._config", t._config);
    dump("task._config.rt", t._config.rt);
    if (t._config.rt) dump("task._config.rt._descriptor", t._config.rt._descriptor);
    dump("task._config.rst", t._config.rst);
    dump("task._config.depth", t._config.depth);
    dump("task._depthSrc", t._depthSrc);
    dump("task._renderPassDescriptor", t._renderPassDescriptor);
    const rpd = t._renderPassDescriptor;
    if (rpd && rpd.colorAttachments && rpd.colorAttachments[0]) dump("rpd.colorAttachments[0]", rpd.colorAttachments[0]);
    if (rpd && rpd.depthStencilAttachment) dump("rpd.depthStencilAttachment", rpd.depthStencilAttachment);
    dump("engine.scRT", engine.scRT);
    dump("engine.scRT._descriptor", engine.scRT._descriptor);
    // surfaces / contexts / tasks counts
    console.log("engine.surfaces.length = " + engine.surfaces.length);
    console.log("surface[0]._renderingContexts.length = " + engine.surfaces[0]._renderingContexts.length);
    console.log("scene === surface[0]._renderingContexts[0] : " + (scene === engine.surfaces[0]._renderingContexts[0]));
    console.log("engine === surfaces[0] : " + (engine === engine.surfaces[0]));
    console.log("=== end ===");
}

main().catch((e) => console.log("introspect error: " + (e && e.stack ? e.stack : e)));
