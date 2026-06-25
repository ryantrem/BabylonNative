#include <Lite/Renderer.h>

#include <string>

// The WGSL-composition generator. This is the native analogue of Babylon Lite's
// runtime shader-string composition: instead of one fixed shader, the source is
// assembled from fragments selected by the material's feature permutation. The
// composed string is handed straight to Dawn/Tint (no translation), and the
// resulting pipeline is cached by the same permutation key (see GetOrCreatePipeline).
//
// The vertex buffer layout is uniform across all permutations (pos, normal, uv,
// color); the composer simply decides which inputs/uniforms each variant consumes.
// New material features are added by extending the fragment set + feature bits.
namespace lite
{
    namespace
    {
        // Bind group 0/1 are always present. Group 2 (material) varies: an untextured
        // material has just the color uniform; a textured one adds a texture + sampler.
        const char* kFrameModelUniforms = R"(
struct Light {
    posType : vec4<f32>,        // xyz = position (point) or direction (dir); w = type (0 dir, 1 point)
    colorIntensity : vec4<f32>, // rgb = color, a = intensity
};
struct Frame {
    viewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightCount : vec4<f32>,     // x = active light count
    lights : array<Light, 4>,
    lightViewProj : mat4x4<f32>, // directional light's shadow view-projection
};
@group(0) @binding(0) var<uniform> frame : Frame;
@group(0) @binding(1) var shadowMap : texture_depth_2d;
@group(0) @binding(2) var shadowSampler : sampler_comparison;
@group(1) @binding(0) var<uniform> model : mat4x4<f32>;
struct MaterialU {
    baseColor : vec4<f32>,
    params : vec4<f32>, // x = metallic, y = roughness, zw unused
};
@group(2) @binding(0) var<uniform> material : MaterialU;

// Sample the shadow map for a world position: project into light clip space, convert
// to UV + depth, and PCF-filter a comparison lookup. Returns 1.0 = fully lit, 0.0 =
// fully shadowed. Off-map / behind-light fragments are treated as lit. The comparison
// sample is issued unconditionally (uniform control flow) and masked afterward.
fn shadowFactor(worldPos : vec3<f32>) -> f32 {
    let ls : vec4<f32> = frame.lightViewProj * vec4<f32>(worldPos, 1.0);
    let proj : vec3<f32> = ls.xyz / ls.w;
    let uv : vec2<f32> = vec2<f32>(proj.x * 0.5 + 0.5, proj.y * -0.5 + 0.5);
    let bias : f32 = 0.0025;
    let refDepth : f32 = proj.z - bias;
    let texel : f32 = 1.0 / 2048.0;
    var sum : f32 = 0.0;
    for (var dy : i32 = -1; dy <= 1; dy = dy + 1) {
        for (var dx : i32 = -1; dx <= 1; dx = dx + 1) {
            let off : vec2<f32> = vec2<f32>(f32(dx), f32(dy)) * texel;
            sum = sum + textureSampleCompare(shadowMap, shadowSampler, uv + off, refDepth);
        }
    }
    let lit : f32 = sum / 9.0;
    let outside : bool = ls.w <= 0.0 || uv.x < 0.0 || uv.x > 1.0 ||
        uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0;
    return select(lit, 1.0, outside);
}
)";
    }

    std::string ComposeMeshWgsl(uint32_t features)
    {
        const bool useVertexColor = (features & MaterialFeature_VertexColor) != 0;
        const bool useLighting = (features & MaterialFeature_Lighting) != 0;
        const bool useTexture = (features & MaterialFeature_Texture) != 0;
        const bool usePBR = (features & MaterialFeature_PBR) != 0 && useLighting;

        std::string src;
        src.reserve(2560);
        src += kFrameModelUniforms;

        if (useTexture)
        {
            src += "@group(2) @binding(1) var baseTex : texture_2d<f32>;\n";
            src += "@group(2) @binding(2) var baseSampler : sampler;\n";
        }

        // ---- Vertex output: conditionally carries color, uv, world normal+pos. ----
        src += "\nstruct VertexOutput {\n";
        src += "    @builtin(position) position : vec4<f32>,\n";
        if (useVertexColor) src += "    @location(0) color : vec3<f32>,\n";
        if (useTexture)     src += "    @location(1) uv : vec2<f32>,\n";
        if (useLighting)
        {
            src += "    @location(2) worldNormal : vec3<f32>,\n";
            src += "    @location(3) worldPos : vec3<f32>,\n";
        }
        src += "};\n";

        // ---- Vertex stage. Inputs are always pos(0)/normal(1)/uv(2)/color(3). ----
        src += "\n@vertex\n";
        src += "fn vs_main(\n";
        src += "    @location(0) inPos : vec3<f32>,\n";
        src += "    @location(1) inNormal : vec3<f32>,\n";
        src += "    @location(2) inUV : vec2<f32>,\n";
        src += "    @location(3) inColor : vec3<f32>) -> VertexOutput {\n";
        src += "    var output : VertexOutput;\n";
        src += "    let worldPos : vec4<f32> = model * vec4<f32>(inPos, 1.0);\n";
        src += "    output.position = frame.viewProj * worldPos;\n";
        if (useVertexColor) src += "    output.color = inColor;\n";
        if (useTexture)     src += "    output.uv = inUV;\n";
        if (useLighting)
        {
            // Uniform scale assumed, so the model's upper 3x3 suffices for normals.
            src += "    output.worldNormal = normalize((model * vec4<f32>(inNormal, 0.0)).xyz);\n";
            src += "    output.worldPos = worldPos.xyz;\n";
        }
        src += "    return output;\n";
        src += "}\n";

        // ---- Fragment stage. Albedo source(s) + optional lighting composed in. ----
        src += "\n@fragment\n";
        src += "fn fs_main(input : VertexOutput) -> @location(0) vec4<f32> {\n";
        src += "    var albedo : vec3<f32> = material.baseColor.rgb;\n";
        if (useTexture)
        {
            src += "    albedo = albedo * textureSample(baseTex, baseSampler, input.uv).rgb;\n";
        }
        if (useVertexColor)
        {
            src += "    albedo = albedo * input.color;\n";
        }
        if (usePBR)
        {
            // Cook-Torrance metallic-roughness BRDF: GGX (Trowbridge-Reitz) normal
            // distribution, Smith-Schlick geometry, Schlick Fresnel. Mirrors the core
            // of Lite's PBR material, accumulated over the active scene lights.
            src += "    let N : vec3<f32> = normalize(input.worldNormal);\n";
            src += "    let V : vec3<f32> = normalize(frame.cameraPos.xyz - input.worldPos);\n";
            src += "    let NdotV : f32 = max(dot(N, V), 1e-4);\n";
            src += "    let metallic : f32 = material.params.x;\n";
            src += "    let roughness : f32 = clamp(material.params.y, 0.04, 1.0);\n";
            src += "    let a : f32 = roughness * roughness;\n";
            src += "    let a2 : f32 = a * a;\n";
            src += "    let F0 : vec3<f32> = mix(vec3<f32>(0.04), albedo, metallic);\n";
            src += "    let kPI : f32 = 3.14159265;\n";
            // Ambient: dielectric diffuse + a flat environment reflectance approximation
            // (F0-tinted) so metals show their color without a full IBL probe.
            src += "    var color : vec3<f32> = albedo * (1.0 - metallic) * 0.08 + F0 * 0.20;\n";
            src += "    let shadow : f32 = shadowFactor(input.worldPos);\n";
            src += "    let count : u32 = u32(frame.lightCount.x);\n";
            src += "    for (var i : u32 = 0u; i < count; i = i + 1u) {\n";
            src += "        let lp : vec4<f32> = frame.lights[i].posType;\n";
            src += "        let lc : vec4<f32> = frame.lights[i].colorIntensity;\n";
            src += "        var L : vec3<f32>;\n";
            src += "        var atten : f32 = 1.0;\n";
            src += "        if (lp.w < 0.5) {\n";
            src += "            L = normalize(-lp.xyz);\n";
            src += "            atten = shadow;\n"; // directional caster is shadow-mapped
            src += "        } else {\n";
            src += "            let d : vec3<f32> = lp.xyz - input.worldPos;\n";
            src += "            L = normalize(d);\n";
            src += "            let dist : f32 = length(d);\n";
            src += "            atten = 1.0 / (1.0 + 0.15 * dist * dist);\n";
            src += "        }\n";
            src += "        let H : vec3<f32> = normalize(L + V);\n";
            src += "        let NdotL : f32 = max(dot(N, L), 0.0);\n";
            src += "        let NdotH : f32 = max(dot(N, H), 0.0);\n";
            src += "        let VdotH : f32 = max(dot(V, H), 0.0);\n";
            src += "        let denom : f32 = NdotH * NdotH * (a2 - 1.0) + 1.0;\n";
            src += "        let D : f32 = a2 / max(kPI * denom * denom, 1e-6);\n";
            src += "        let k : f32 = (roughness + 1.0) * (roughness + 1.0) / 8.0;\n";
            src += "        let gv : f32 = NdotV / (NdotV * (1.0 - k) + k);\n";
            src += "        let gl : f32 = NdotL / (NdotL * (1.0 - k) + k);\n";
            src += "        let G : f32 = gv * gl;\n";
            src += "        let F : vec3<f32> = F0 + (vec3<f32>(1.0) - F0) * pow(1.0 - VdotH, 5.0);\n";
            src += "        let specular : vec3<f32> = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);\n";
            src += "        let kd : vec3<f32> = (vec3<f32>(1.0) - F) * (1.0 - metallic);\n";
            src += "        let radiance : vec3<f32> = lc.rgb * lc.w * atten;\n";
            src += "        color = color + (kd * albedo / kPI + specular) * radiance * NdotL;\n";
            src += "    }\n";
            src += "    albedo = color;\n";
        }
        else if (useLighting)
        {
            // Ambient term + a loop over the active lights (directional + point),
            // each contributing Lambert diffuse and a Blinn-Phong specular highlight.
            src += "    let N : vec3<f32> = normalize(input.worldNormal);\n";
            src += "    let V : vec3<f32> = normalize(frame.cameraPos.xyz - input.worldPos);\n";
            src += "    var color : vec3<f32> = albedo * 0.12;\n"; // ambient
            src += "    let shadow : f32 = shadowFactor(input.worldPos);\n";
            src += "    let count : u32 = u32(frame.lightCount.x);\n";
            src += "    for (var i : u32 = 0u; i < count; i = i + 1u) {\n";
            src += "        let lp : vec4<f32> = frame.lights[i].posType;\n";
            src += "        let lc : vec4<f32> = frame.lights[i].colorIntensity;\n";
            src += "        var L : vec3<f32>;\n";
            src += "        var atten : f32 = 1.0;\n";
            src += "        if (lp.w < 0.5) {\n";
            src += "            L = normalize(-lp.xyz);\n"; // directional: xyz = direction of travel
            src += "            atten = shadow;\n"; // directional caster is shadow-mapped
            src += "        } else {\n";
            src += "            let d : vec3<f32> = lp.xyz - input.worldPos;\n"; // point
            src += "            L = normalize(d);\n";
            src += "            let dist : f32 = length(d);\n";
            src += "            atten = 1.0 / (1.0 + 0.15 * dist * dist);\n";
            src += "        }\n";
            src += "        let diff : f32 = max(dot(N, L), 0.0);\n";
            src += "        let H : vec3<f32> = normalize(L + V);\n";
            src += "        let spec : f32 = select(0.0, pow(max(dot(N, H), 0.0), 32.0), diff > 0.0);\n";
            src += "        let radiance : vec3<f32> = lc.rgb * lc.w * atten;\n";
            src += "        color = color + albedo * radiance * diff + radiance * spec * 0.4;\n";
            src += "    }\n";
            src += "    albedo = color;\n";
        }
        src += "    return vec4<f32>(albedo, 1.0);\n";
        src += "}\n";

        return src;
    }
}
