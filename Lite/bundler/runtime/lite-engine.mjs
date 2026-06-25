// Babylon Lite — JS engine layer (Model B).
//
// This is the part of Lite that the original architecture put "in Polyfills + JS":
// material and mesh creation, including WGSL composition, all run in JavaScript on
// top of the WebGPU N-API polyfill. JS creates the real Dawn resources (shader
// modules, pipelines, bind group layouts, bind groups, vertex/index buffers) and
// registers a drawable with the native loop. After setup, the native loop owns every
// frame (animation, world matrices, camera/lights, draw submission) with ZERO engine
// JS per frame. The only native setup primitives used here are createEngine (Dawn +
// HWND surface), createNode (transform + model uniform buffer), and registerDrawable.
//
// Resolved by the bundler's externalize plugin: createMaterial/createMesh come from
// here (JS), while the render-loop primitives come from the native BabylonNativeLite.

const __bnl =
    (typeof globalThis !== "undefined" && globalThis.BabylonNativeLite)
        ? globalThis.BabylonNativeLite
        : (typeof BabylonNativeLite !== "undefined" ? BabylonNativeLite : undefined);

// Per-engine shared GPU state (bind group layouts, pipeline layout, the group-0 frame
// bind group). Created once per engine and reused across materials/meshes.
const engineState = new WeakMap();

function getState(engine) {
    let s = engineState.get(engine);
    if (s) return s;

    const device = engine._device;
    // Bind group layouts: 0 = frame (camera/lights), 1 = model (mat4), 2 = material.
    const frameBGL = device.createBindGroupLayout({
        entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: "uniform" } }],
    });
    const modelBGL = device.createBindGroupLayout({
        entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: "uniform" } }],
    });
    const materialBGL = device.createBindGroupLayout({
        entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "uniform" } }],
    });
    const pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [frameBGL, modelBGL, materialBGL] });

    // Group 0 references the native-owned frame uniform buffer (camera/lights/viewProj),
    // which the native loop writes each frame. JS only references it here.
    const frameBindGroup = device.createBindGroup({
        layout: frameBGL,
        entries: [{ binding: 0, resource: { buffer: engine.frameBuffer } }],
    });

    s = { device, frameBGL, modelBGL, materialBGL, pipelineLayout, frameBindGroup,
          pipelineCache: new Map() };
    engineState.set(engine, s);
    return s;
}

// WGSL composer — the JS analogue of the native ShaderComposer. Assembles the shader
// from feature flags (lighting / vertexColor / pbr). The Frame uniform layout matches
// the native frame buffer's first 224 bytes (viewProj, cameraPos, lightCount, lights);
// the native buffer is larger (it also holds the shadow matrix), which is fine.
function composeWgsl(features) {
    const lighting = !!features.lighting || !!features.pbr;
    const vertexColor = !!features.vertexColor;
    const pbr = !!features.pbr;

    let s = `
struct Light { posType : vec4<f32>, colorIntensity : vec4<f32> };
struct Frame {
    viewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightCount : vec4<f32>,
    lights : array<Light, 4>,
};
@group(0) @binding(0) var<uniform> frame : Frame;
@group(1) @binding(0) var<uniform> model : mat4x4<f32>;
struct MaterialU { baseColor : vec4<f32>, params : vec4<f32> };
@group(2) @binding(0) var<uniform> material : MaterialU;

struct VOut {
    @builtin(position) position : vec4<f32>,
`;
    if (vertexColor) s += "    @location(0) color : vec3<f32>,\n";
    if (lighting) s += "    @location(1) worldNormal : vec3<f32>,\n    @location(2) worldPos : vec3<f32>,\n";
    s += `};

@vertex
fn vs_main(@location(0) inPos : vec3<f32>, @location(1) inNormal : vec3<f32>,
           @location(2) inUV : vec2<f32>, @location(3) inColor : vec3<f32>) -> VOut {
    var o : VOut;
    let wp : vec4<f32> = model * vec4<f32>(inPos, 1.0);
    o.position = frame.viewProj * wp;
`;
    if (vertexColor) s += "    o.color = inColor;\n";
    if (lighting) s += "    o.worldNormal = normalize((model * vec4<f32>(inNormal, 0.0)).xyz);\n    o.worldPos = wp.xyz;\n";
    s += `    return o;
}

@fragment
fn fs_main(i : VOut) -> @location(0) vec4<f32> {
    var albedo : vec3<f32> = material.baseColor.rgb;
`;
    if (vertexColor) s += "    albedo = albedo * i.color;\n";

    if (pbr) {
        s += `
    let N : vec3<f32> = normalize(i.worldNormal);
    let V : vec3<f32> = normalize(frame.cameraPos.xyz - i.worldPos);
    let NdotV : f32 = max(dot(N, V), 1e-4);
    let metallic : f32 = material.params.x;
    let roughness : f32 = clamp(material.params.y, 0.04, 1.0);
    let a2 : f32 = roughness * roughness * roughness * roughness;
    let F0 : vec3<f32> = mix(vec3<f32>(0.04), albedo, metallic);
    let kPI : f32 = 3.14159265;
    var color : vec3<f32> = albedo * (1.0 - metallic) * 0.08 + F0 * 0.20;
    let count : u32 = u32(frame.lightCount.x);
    for (var k : u32 = 0u; k < count; k = k + 1u) {
        let lp : vec4<f32> = frame.lights[k].posType;
        let lc : vec4<f32> = frame.lights[k].colorIntensity;
        var L : vec3<f32>; var atten : f32 = 1.0;
        if (lp.w < 0.5) { L = normalize(-lp.xyz); }
        else { let d = lp.xyz - i.worldPos; L = normalize(d); atten = 1.0 / (1.0 + 0.15 * dot(d, d)); }
        let H : vec3<f32> = normalize(L + V);
        let NdotL : f32 = max(dot(N, L), 0.0);
        let NdotH : f32 = max(dot(N, H), 0.0);
        let VdotH : f32 = max(dot(V, H), 0.0);
        let denom : f32 = NdotH * NdotH * (a2 - 1.0) + 1.0;
        let D : f32 = a2 / max(kPI * denom * denom, 1e-6);
        let kg : f32 = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        let G : f32 = (NdotV / (NdotV * (1.0 - kg) + kg)) * (NdotL / (NdotL * (1.0 - kg) + kg));
        let F : vec3<f32> = F0 + (vec3<f32>(1.0) - F0) * pow(1.0 - VdotH, 5.0);
        let spec : vec3<f32> = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
        let kd : vec3<f32> = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        color = color + (kd * albedo / kPI + spec) * lc.rgb * lc.w * atten * NdotL;
    }
    albedo = color;
`;
    } else if (lighting) {
        s += `
    let N : vec3<f32> = normalize(i.worldNormal);
    let V : vec3<f32> = normalize(frame.cameraPos.xyz - i.worldPos);
    var color : vec3<f32> = albedo * 0.12;
    let count : u32 = u32(frame.lightCount.x);
    for (var k : u32 = 0u; k < count; k = k + 1u) {
        let lp : vec4<f32> = frame.lights[k].posType;
        let lc : vec4<f32> = frame.lights[k].colorIntensity;
        var L : vec3<f32>; var atten : f32 = 1.0;
        if (lp.w < 0.5) { L = normalize(-lp.xyz); }
        else { let d = lp.xyz - i.worldPos; L = normalize(d); atten = 1.0 / (1.0 + 0.15 * dot(d, d)); }
        let diff : f32 = max(dot(N, L), 0.0);
        let H : vec3<f32> = normalize(L + V);
        let sp : f32 = select(0.0, pow(max(dot(N, H), 0.0), 32.0), diff > 0.0);
        color = color + albedo * lc.rgb * lc.w * atten * diff + lc.rgb * lc.w * sp * 0.4;
    }
    albedo = color;
`;
    }
    s += `    return vec4<f32>(albedo, 1.0);
}
`;
    return s;
}

function featureKey(f) {
    return (f.lighting ? 1 : 0) | (f.vertexColor ? 2 : 0) | (f.pbr ? 4 : 0);
}

// createMaterial — JS factory. Composes WGSL, creates the pipeline (cached per feature
// permutation, mirroring the native pipeline cache), and a material uniform buffer +
// bind group. Returns a material the JS createMesh consumes.
export function createMaterial(engine, options) {
    if (!__bnl) throw new Error("BabylonNativeLite global is not installed.");
    const opts = options || {};
    const st = getState(engine);
    const device = st.device;

    const key = featureKey(opts);
    let pipeline = st.pipelineCache.get(key);
    if (!pipeline) {
        const module = device.createShaderModule({ code: composeWgsl(opts) });
        pipeline = device.createRenderPipeline({
            layout: st.pipelineLayout,
            vertex: {
                module, entryPoint: "vs_main",
                buffers: [{
                    arrayStride: 44,
                    attributes: [
                        { format: "float32x3", offset: 0, shaderLocation: 0 },
                        { format: "float32x3", offset: 12, shaderLocation: 1 },
                        { format: "float32x2", offset: 24, shaderLocation: 2 },
                        { format: "float32x3", offset: 32, shaderLocation: 3 },
                    ],
                }],
            },
            fragment: { module, entryPoint: "fs_main", targets: [{ format: engine.colorFormat }] },
            primitive: { topology: "triangle-list", cullMode: "none" },
            depthStencil: { format: engine.depthFormat, depthWriteEnabled: true, depthCompare: "less" },
        });
        st.pipelineCache.set(key, pipeline);
    }

    const color = opts.color || [1, 1, 1, 1];
    const uni = new Float32Array([color[0], color[1], color[2], color[3] == null ? 1 : color[3],
        opts.metallic == null ? 0 : opts.metallic, opts.roughness == null ? 0.5 : opts.roughness, 0, 0]);
    const matBuffer = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(matBuffer, 0, uni);
    const materialBindGroup = device.createBindGroup({
        layout: st.materialBGL,
        entries: [{ binding: 0, resource: { buffer: matBuffer } }],
    });

    return { __kind: "material", pipeline, materialBindGroup, _engine: engine };
}

// createMesh — JS factory. Builds the interleaved vertex buffer + index buffer, the
// per-mesh model bind group (referencing the node's native model uniform buffer), and
// registers the drawable with the native loop.
export function createMesh(engine, positions, normals, uvs, colors, indices, material, node) {
    if (!__bnl) throw new Error("BabylonNativeLite global is not installed.");
    const st = getState(engine);
    const device = st.device;
    const vertexCount = positions.length / 3;

    // Interleave [pos.xyz, normal.xyz, uv.xy, color.rgb] = 11 floats / vertex.
    const inter = new Float32Array(vertexCount * 11);
    for (let i = 0; i < vertexCount; i++) {
        const o = i * 11;
        inter[o] = positions[i * 3]; inter[o + 1] = positions[i * 3 + 1]; inter[o + 2] = positions[i * 3 + 2];
        inter[o + 3] = normals[i * 3]; inter[o + 4] = normals[i * 3 + 1]; inter[o + 5] = normals[i * 3 + 2];
        inter[o + 6] = uvs[i * 2]; inter[o + 7] = uvs[i * 2 + 1];
        inter[o + 8] = colors[i * 3]; inter[o + 9] = colors[i * 3 + 1]; inter[o + 10] = colors[i * 3 + 2];
    }
    const vertexBuffer = device.createBuffer({ size: inter.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(vertexBuffer, 0, inter);

    // Index buffer (uint16), padded so the byte length is a multiple of 4.
    let idx = indices;
    if ((idx.length & 1) !== 0) {
        const padded = new Uint16Array(idx.length + 1);
        padded.set(idx);
        idx = padded;
    }
    const indexBuffer = device.createBuffer({ size: idx.byteLength, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(indexBuffer, 0, idx);

    // Group 1 references the node's native model uniform buffer (written per frame).
    const modelBindGroup = device.createBindGroup({
        layout: st.modelBGL,
        entries: [{ binding: 0, resource: { buffer: node.modelBuffer } }],
    });

    __bnl.registerDrawable(engine, {
        pipeline: material.pipeline,
        bindGroups: [st.frameBindGroup, modelBindGroup, material.materialBindGroup],
        vertexBuffer,
        indexBuffer,
        indexCount: indices.length,
        indexFormat: "uint16",
        node,
    });

    return { __kind: "mesh", node };
}
