// Babylon Lite — JS orchestration runtime (host-agnostic, setup-time only).
//
// These are the parts of the Lite API that DON'T run in the render loop: scene
// bookkeeping, camera/light construction, scene registration. They run once during
// scene setup, so they stay in JavaScript — exactly the project's boundary (zero
// engine JS per frame; only the render loop is native). Each function is a thin
// layer over the native render-loop primitives exposed on `BabylonNativeLite`.
//
// The bundler resolves the orchestration names (createSceneContext, addToScene,
// createDefaultCamera, createHemisphericLight, registerScene, attachControl, ...) to
// THIS module, and resolves the render-loop primitives (createEngine, createMesh,
// createMaterial, createNode, createCamera, addLight, createAnimation, startEngine)
// to the native global. Both bundle together; only this orchestration runs in JS, at
// setup — the native loop owns every frame.

const __bnl =
    (typeof globalThis !== "undefined" && globalThis.BabylonNativeLite)
        ? globalThis.BabylonNativeLite
        : (typeof BabylonNativeLite !== "undefined" ? BabylonNativeLite : undefined);

function requireHost() {
    if (!__bnl) {
        throw new Error("BabylonNativeLite global is not installed — run inside Babylon Native Lite.");
    }
    return __bnl;
}

// A scene context is pure JS bookkeeping: it tracks the engine plus the meshes and
// lights added to it. The native engine already owns the GPU resources and draws
// them; the scene context just gives Lite-style code a place to register and a
// lifetime to manage. Nothing here touches the render loop.
export function createSceneContext(engine, _options) {
    requireHost();
    return {
        engine,
        meshes: [],
        lights: [],
        _beforeRender: [],
        _disposed: false,
    };
}

// Add a previously-created object to the scene. Meshes are already drawn by the
// native engine (createMesh registers them), so this just tracks them. Lights are
// JS descriptors produced by createHemisphericLight et al.; adding one forwards to
// the native addLight. Returns the object for chaining, matching Lite's shape.
export function addToScene(scene, object) {
    if (object == null) return object;
    if (object.__kind === "light") {
        const host = requireHost();
        host.addLight(scene.engine, object.descriptor);
        scene.lights.push(object);
    } else if (Array.isArray(object)) {
        for (const o of object) addToScene(scene, o);
    } else {
        scene.meshes.push(object);
    }
    return object;
}

export function removeFromScene(scene, object) {
    const arr = object && object.__kind === "light" ? scene.lights : scene.meshes;
    const i = arr.indexOf(object);
    if (i >= 0) arr.splice(i, 1);
}

// A per-frame JS hook is an ALLOWED exception to "zero JS per frame": the native
// loop calls it before each frame if the host wires onBeforeRender. Most scenes
// don't need it (animation runs natively).
export function onBeforeRender(scene, fn) {
    scene._beforeRender.push(fn);
    const host = requireHost();
    if (typeof host.setBeforeRender === "function") {
        host.setBeforeRender(scene.engine, () => {
            for (const cb of scene._beforeRender) cb();
        });
    }
    return fn;
}

// registerScene finalizes the scene before the first frame. With the native engine
// this is a no-op today (resources are created eagerly), but it's the natural place
// for any one-time validation/upload — still setup-time, never in the loop.
export async function registerScene(_scene) {
    requireHost();
    return _scene;
}

export function disposeScene(scene) {
    scene._disposed = true;
}

// A hemispheric-style light: a JS descriptor that addToScene forwards to the native
// addLight. Modeled as a soft directional light (sky direction), which the native
// shading already supports; the upward hemisphere term can be refined natively later.
export function createHemisphericLight(direction, intensity) {
    const dir = direction || [0, 1, 0];
    // Hemispheric "direction" points toward the sky; the directional light's travel
    // direction is the negation.
    return {
        __kind: "light",
        descriptor: {
            type: "directional",
            direction: [-dir[0], -dir[1], -dir[2]],
            color: [1, 1, 1],
            intensity: intensity == null ? 1.0 : intensity,
        },
    };
}

// A default ArcRotate-style camera built on the native camera primitive. alpha/beta/
// radius are JS state; setting any recomputes the eye position and pushes the view
// to the native camera. All of this is setup-time math (and, via attachControl,
// optional input handling) — not render-loop work.
export function createDefaultCamera(scene, options) {
    requireHost();
    const native = __bnl.createCamera(scene.engine);
    const opts = options || {};
    const state = {
        alpha: opts.alpha != null ? opts.alpha : -Math.PI / 2,
        beta: opts.beta != null ? opts.beta : Math.PI / 2.5,
        radius: opts.radius != null ? opts.radius : 8,
        target: opts.target || [0, 0, 0],
        fov: opts.fov != null ? opts.fov : 0.8,
        aspect: opts.aspect != null ? opts.aspect : 1280 / 720,
        near: 0.1,
        far: 100,
    };

    function applyView() {
        // Spherical (alpha around Y, beta from +Y) to Cartesian, offset from target.
        const sb = Math.sin(state.beta);
        const x = state.target[0] + state.radius * sb * Math.cos(state.alpha);
        const y = state.target[1] + state.radius * Math.cos(state.beta);
        const z = state.target[2] + state.radius * sb * Math.sin(state.alpha);
        native.setView([x, y, z], state.target, [0, 1, 0]);
    }

    native.setProjection(state.fov, state.aspect, state.near, state.far);
    applyView();

    const cam = {
        __kind: "camera",
        _native: native,
        get alpha() { return state.alpha; },
        set alpha(v) { state.alpha = v; applyView(); },
        get beta() { return state.beta; },
        set beta(v) { state.beta = v; applyView(); },
        get radius() { return state.radius; },
        set radius(v) { state.radius = v; applyView(); },
        setTarget(t) { state.target = t; applyView(); },
    };
    return cam;
}

// attachControl wires pointer/keyboard input to the camera. Input handling is a
// setup-time registration; any per-frame inertia it applies would run through the
// allowed onBeforeRender hook, never as engine JS in the loop. Native input plumbing
// isn't exposed yet, so this is a no-op stub that keeps scene code running.
export function attachControl(_camera, _canvas, _scene) {
    requireHost();
    /* no-op until native input events are exposed */
}

// Async asset loaders stay in JS by design (the loaders-in-JS boundary) — they don't
// run in the render loop. Not implemented yet; throwing keeps failures precise.
export async function loadGltf(_engine, _url) {
    throw new Error("loadGltf is not implemented yet (glTF parsing stays in JS; native mesh upload pending).");
}
export async function loadEnvironment(_scene, _url, _options) {
    throw new Error("loadEnvironment is not implemented yet (IBL/skybox pending).");
}
