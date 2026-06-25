// Stage-0 microbenchmark: N-API vs direct-V8 vs V8 fast-API call overhead.
//
// The native render-loop benchmarks showed our native WebGPU stack is ~4x slower
// than Chrome on the bundle path even though the per-draw cost is gone for both.
// The leading hypothesis is that the residual gap is per-call N-API marshaling
// (handle scopes, type checks, indirection) on the hot per-frame WebGPU calls
// (queue.writeBuffer, beginRenderPass, setBindGroup, executeBundles, ...).
//
// This probe isolates JUST the JS->native call-boundary cost with trivial work,
// across three binding mechanisms, for two argument shapes:
//   * scalar      (number -> number)            — pure call overhead
//   * typedarray  (Float32Array -> number)      — closest to writeBuffer marshaling
// Mechanisms:
//   * napi    — Napi::Function::New ObjectWrap-style callback (what the polyfill uses)
//   * v8      — raw v8::FunctionTemplate slow callback (no napi layer)
//   * v8fast  — v8 fast API call (v8::CFunction; engaged once TurboFan tiers up)
//
// A JS driver (microbench scene) loops each function N times, timed via the native
// high-res clock _mbNowNs(), and reports ns/call. If the build wasn't compiled with
// fast-API calls enabled, the v8fast path simply falls back to the slow callback,
// so v8fast ~= v8 (which we note in the analysis).

#include <Lite/NativeLite.h>

#if defined(LITE_ENGINE_V8)

#include <napi/env.h> // brings v8.h + Napi::GetContext(Napi::Env) -> v8::Local<v8::Context>
#include <v8-fast-api-calls.h>

#include <chrono>
#include <cstring>

namespace lite::nativelite
{
    namespace
    {
        // ---- N-API probes (the mechanism the WebGPU polyfill uses) ----
        Napi::Value NapiScalar(const Napi::CallbackInfo& info)
        {
            double x = info.Length() > 0 && info[0].IsNumber() ? info[0].As<Napi::Number>().DoubleValue() : 0.0;
            return Napi::Number::New(info.Env(), x + 1.0);
        }

        Napi::Value NapiTypedArray(const Napi::CallbackInfo& info)
        {
            double r = 0.0;
            if (info.Length() > 0 && info[0].IsTypedArray())
            {
                Napi::Float32Array ta = info[0].As<Napi::Float32Array>();
                if (ta.ElementLength() > 0) r = ta[0];
            }
            return Napi::Number::New(info.Env(), r);
        }

        // ---- Direct-V8 slow callbacks ----
        void V8Scalar(const v8::FunctionCallbackInfo<v8::Value>& info)
        {
            v8::Isolate* iso = info.GetIsolate();
            double x = 0.0;
            if (info.Length() > 0)
                x = info[0]->NumberValue(iso->GetCurrentContext()).FromMaybe(0.0);
            info.GetReturnValue().Set(x + 1.0);
        }

        void V8TypedArray(const v8::FunctionCallbackInfo<v8::Value>& info)
        {
            double r = 0.0;
            if (info.Length() > 0 && info[0]->IsFloat32Array())
            {
                v8::Local<v8::Float32Array> ta = info[0].As<v8::Float32Array>();
                std::shared_ptr<v8::BackingStore> bs = ta->Buffer()->GetBackingStore();
                if (ta->Length() > 0)
                {
                    const auto* base = reinterpret_cast<const char*>(bs->Data()) + ta->ByteOffset();
                    float v = 0.0f;
                    std::memcpy(&v, base, sizeof(float));
                    r = v;
                }
            }
            info.GetReturnValue().Set(r);
        }

        // ---- V8 fast-API callbacks (engaged after TurboFan tier-up) ----
        double FastScalar(v8::Local<v8::Object> /*receiver*/, double x)
        {
            return x + 1.0;
        }

        double FastTypedArray(v8::Local<v8::Object> /*receiver*/, const v8::FastApiTypedArray<float>& arr)
        {
            return arr.length() > 0 ? static_cast<double>(arr.get(0)) : 0.0;
        }

        void SetGlobalFn(v8::Local<v8::Context> ctx, const char* name, v8::Local<v8::Function> fn)
        {
            v8::Isolate* iso = ctx->GetIsolate();
            v8::Local<v8::String> key = v8::String::NewFromUtf8(iso, name).ToLocalChecked();
            ctx->Global()->Set(ctx, key, fn).Check();
        }

        void InstallV8Probe(v8::Local<v8::Context> ctx, const char* name,
            v8::FunctionCallback slow, const v8::CFunction* fast)
        {
            v8::Isolate* iso = ctx->GetIsolate();
            v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(
                iso, slow, v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 1,
                v8::ConstructorBehavior::kThrow, v8::SideEffectType::kHasSideEffect, fast);
            SetGlobalFn(ctx, name, tmpl->GetFunction(ctx).ToLocalChecked());
        }
    }

    void InstallMicrobench(Napi::Env env)
    {
        // High-res clock for the JS driver (called twice per measurement → negligible).
        env.Global().Set("_mbNowNs", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                return Napi::Number::New(info.Env(), static_cast<double>(ns));
            },
            "_mbNowNs"));

        // N-API probes.
        env.Global().Set("_mbNapiScalar", Napi::Function::New(env, NapiScalar, "_mbNapiScalar"));
        env.Global().Set("_mbNapiTA", Napi::Function::New(env, NapiTypedArray, "_mbNapiTA"));

        // Direct-V8 + fast-API probes (reach the live isolate/context via the napi env).
        v8::Local<v8::Context> ctx = Napi::GetContext(env);
        v8::Isolate* iso = ctx->GetIsolate();
        v8::HandleScope scope(iso);

        static const v8::CFunction s_fastScalar = v8::CFunction::Make(FastScalar);
        static const v8::CFunction s_fastTA = v8::CFunction::Make(FastTypedArray);

        InstallV8Probe(ctx, "_mbV8Scalar", V8Scalar, nullptr);
        InstallV8Probe(ctx, "_mbV8TA", V8TypedArray, nullptr);
        InstallV8Probe(ctx, "_mbV8FastScalar", V8Scalar, &s_fastScalar);
        InstallV8Probe(ctx, "_mbV8FastTA", V8TypedArray, &s_fastTA);

        std::fprintf(stderr, "[microbench] probes installed (_mbNapi*/_mbV8*/_mbV8Fast*)\n");
    }
}

#else // !LITE_ENGINE_V8 — the V8-direct/fast-API probes don't apply; install nothing.

namespace lite::nativelite
{
    void InstallMicrobench(Napi::Env /*env*/) {}
}

#endif // LITE_ENGINE_V8
