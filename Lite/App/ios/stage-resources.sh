#!/usr/bin/env bash
# Stage the iOS demo's bundle resources into App/ios/Resources/ so CMake copies them into the
# .app. Run this on the Mac AFTER building the JS bundle (node build-lite.mjs) and BEFORE
# configuring the Xcode project (the CMakeLists globs Resources/ at configure time).
#
# Produces in App/ios/Resources/:
#   scene.lite.js              <- the bundled cubesios scene (renamed)
#   cubes.glb                  <- the 8000-node model
#   brdf-lut.png               <- IBL BRDF LUT
#   environmentSpecular.env    <- prefiltered environment (downloaded if absent)
#
# Override paths via env vars (see defaults below).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
res="${here}/Resources"
mkdir -p "${res}"

# --- Inputs (override as needed) ------------------------------------------------------------
BUNDLE_JS="${LITE_IOS_BUNDLE_JS:-${here}/../../bundler/dist/cubesios.lite.js}"
CUBES_GLB="${LITE_IOS_CUBES_GLB:-${here}/../../assets/cubes.glb}"
BRDF_PNG="${LITE_IOS_BRDF_PNG:-}"   # e.g. .../babylon-lite/assets/brdf-lut.png
ENV_URL="${LITE_IOS_ENV_URL:-https://assets.babylonjs.com/core/environments/environmentSpecular.env}"

# --- Scene bundle ---------------------------------------------------------------------------
if [[ ! -f "${BUNDLE_JS}" ]]; then
    echo "ERROR: scene bundle not found: ${BUNDLE_JS}" >&2
    echo "       Build it first: (cd ../../bundler && node build-lite.mjs)" >&2
    exit 1
fi
cp -f "${BUNDLE_JS}" "${res}/scene.lite.js"
echo "staged scene.lite.js  <- ${BUNDLE_JS}"

# --- cubes.glb ------------------------------------------------------------------------------
if [[ ! -f "${CUBES_GLB}" ]]; then
    echo "ERROR: cubes.glb not found: ${CUBES_GLB}" >&2
    exit 1
fi
cp -f "${CUBES_GLB}" "${res}/cubes.glb"
echo "staged cubes.glb      <- ${CUBES_GLB}"

# --- brdf-lut.png ---------------------------------------------------------------------------
if [[ -n "${BRDF_PNG}" && -f "${BRDF_PNG}" ]]; then
    cp -f "${BRDF_PNG}" "${res}/brdf-lut.png"
    echo "staged brdf-lut.png   <- ${BRDF_PNG}"
elif [[ ! -f "${res}/brdf-lut.png" ]]; then
    echo "WARN: brdf-lut.png not provided (set LITE_IOS_BRDF_PNG). Copy it into Resources/ manually." >&2
fi

# --- environmentSpecular.env ----------------------------------------------------------------
if [[ ! -f "${res}/environmentSpecular.env" ]]; then
    echo "downloading environmentSpecular.env ..."
    curl -fsSL "${ENV_URL}" -o "${res}/environmentSpecular.env"
    echo "staged environmentSpecular.env <- ${ENV_URL}"
fi

echo "Resources staged in ${res}"
ls -la "${res}"
