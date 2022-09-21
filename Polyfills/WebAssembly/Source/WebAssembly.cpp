#include <Babylon/JsRuntime.h>

#include <arcana/threading/task.h>
#include <arcana/threading/task_schedulers.h>
#include <wasm3_cpp.h>
#include <m3_env.h>

namespace
{

}

namespace Babylon::Polyfills::Internal
{
    class Module : public Napi::ObjectWrap<Module>
    {
    public:
        static constexpr auto JS_CLASS_NAME = "Module";
    
        static Napi::Function Initialize(Napi::Env env)
        {
            Napi::HandleScope scope{env};

            //Napi::Function func = Module::DefineClass(
            return Module::DefineClass(
                env,
                JS_CLASS_NAME,
                {
                    // TODO
                });

            //env.Global().Set(JS_CLASS_NAME, func);
        }

        Module(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<Module>{info}
            , m_module{Napi::Persistent(info[0].As<Napi::External<wasm3::module>>())}
            , m_runtime{Napi::Persistent(info[1].As<Napi::External<wasm3::runtime>>())}
        {
            // TODO: Either a bufferSource passed from JS, or a wasm3::module and wasm3::runtime passed from native
            auto thisObject{info.This().As<Napi::Object>()};
            auto exports{Napi::Object::New(info.Env())};
            thisObject.Set("exports", exports);

            auto module{m_module.Value().Data()->m_module};
            for (u32 functionIndex = 0; functionIndex < module->numFunctions; functionIndex++)
            {
                auto function{&module->functions[functionIndex]};
                if (function->export_name != nullptr)
                {
                    exports.Set(function->export_name, Napi::Function::New(info.Env(), [function](const Napi::CallbackInfo& info) {
                        // todo: allocate vectors large enough to make sure they don't resize (garbage)
                        std::vector<i32> i32Args{};
                        i32Args.reserve(function->funcType->numArgs);
                        std::vector<i64> i64Args(function->funcType->numArgs);
                        std::vector<f32> f32Args(function->funcType->numArgs);
                        std::vector<f64> f64Args(function->funcType->numArgs);
                        std::vector<const void*> args{};
                        for (u32 argIndex = 0; argIndex < function->funcType->numArgs; argIndex++)
                        {
                            auto argType{function->funcType->types[function->funcType->numRets + argIndex]};
                            if (argType == M3ValueType::c_m3Type_i32)
                            {
                                auto& value{i32Args.emplace_back(info[argIndex].ToNumber().Int32Value())};
                                args.push_back(&value);
                                //i32Args[]
                                //args.push_back(&i32Args.back());
                            }
                            else if (argType == M3ValueType::c_m3Type_i64)
                            {
                                auto& value{i64Args.emplace_back(info[argIndex].ToNumber().Int64Value())};
                                args.push_back(&value);
                            }
                            else if (argType == M3ValueType::c_m3Type_f32)
                            {
                                auto& value{f32Args.emplace_back(info[argIndex].ToNumber().FloatValue())};
                                args.push_back(&value);
                            }
                            else if (argType == M3ValueType::c_m3Type_f64)
                            {
                                auto& value{f64Args.emplace_back(info[argIndex].ToNumber().DoubleValue())};
                                args.push_back(&value);
                            }
                        }

                        if (!function->compiled)
                        {
                            CompileFunction(function);
                        }
                        M3Result res = m3_Call(function, args.size(), args.data());
                        // todo: check result

                        if (function->funcType->numRets == 0)
                        {
                            return info.Env().Undefined();
                        }
                        else
                        {
                            //assert(&function == function.module->runtime->lastCalled);
                            u8* stack = (u8*)function->module->runtime->stack;
                            if (function->funcType->types[0] == M3ValueType::c_m3Type_i32)
                            {
                                return Napi::Value::From(info.Env(), *(i32*)stack);
                            }
                            else if (function->funcType->types[0] == M3ValueType::c_m3Type_i64)
                            {
                                return Napi::Value::From(info.Env(), *(i64*)stack);
                            }
                            else if (function->funcType->types[0] == M3ValueType::c_m3Type_f32)
                            {
                                return Napi::Value::From(info.Env(), *(f32*)stack);
                            }
                            else if (function->funcType->types[0] == M3ValueType::c_m3Type_f64)
                            {
                                return Napi::Value::From(info.Env(), *(f64*)stack);
                            }
                        }
                    }));
                }
            }
        }

    private:
        Napi::Reference<Napi::External<wasm3::module>> m_module;
        Napi::Reference<Napi::External<wasm3::runtime>> m_runtime;
    };

    class WebAssembly : public Napi::ObjectWrap<WebAssembly>
    {
    public:
        static constexpr auto JS_CLASS_NAME = "WebAssembly";
        static constexpr auto JS_WASM_ENV_NAME = "WasmEnv";

        static void Initialize(Napi::Env env)
        {
            Napi::HandleScope scope{env};

            Napi::Function func = WebAssembly::DefineClass(
                env,
                JS_CLASS_NAME,
                {
                    StaticMethod("compile", &WebAssembly::Compile),
                    StaticMethod("instantiate", &WebAssembly::Instantiate),
                });

            func.Set(Module::JS_CLASS_NAME, Module::Initialize(env));

            env.Global().Set(JS_CLASS_NAME, func);

            Napi::Value wasmEnv = Napi::External<wasm3::environment>::New(env, new wasm3::environment(), [](Napi::Env, wasm3::environment* wasmEnv) { delete wasmEnv; });
            func.Set(JS_WASM_ENV_NAME, wasmEnv);
        }

        WebAssembly(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<WebAssembly>{info}
            , m_runtime{JsRuntime::GetFromJavaScript(info.Env())}
        {
           
        }

        ~WebAssembly()
        {
            
        }

    private:
        static wasm3::environment& WasmEnv(Napi::Env env)
        {
            return *env.Global().Get(JS_CLASS_NAME).As<Napi::Function>().Get(JS_WASM_ENV_NAME).As<Napi::External<wasm3::environment>>().Data();
        }

        static Napi::Value Compile(const Napi::CallbackInfo& info)
        {
            auto env{info.Env()};

            if (info.Length() != 1)
            {
                throw Napi::Error::New(env, "Exactly one argument expected.");
            }

            Napi::ArrayBuffer bufferSource{};
            if (info[0].IsArrayBuffer())
            {
                bufferSource = info[0].As<Napi::ArrayBuffer>();
            }
            else if (info[0].IsTypedArray())
            {
                bufferSource = info[0].As<Napi::TypedArray>().ArrayBuffer();
            }
            else
            {
                throw Napi::Error::New(env, "Argument must be an ArrayBuffer or typed array."); 
            }

            auto& wasmEnv{WasmEnv(env)};

            const auto deferred{Napi::Promise::Deferred::New(env)};
            //arcana::make_task(arcana::threadpool_scheduler, arcana::cancellation::none(),
            // wasm3 has a thread affinity, not sure how to parse on a background thread...
            arcana::make_task(arcana::inline_scheduler, arcana::cancellation::none(),
                [wasmEnv, env, bufferSourceRef{Napi::Persistent(bufferSource)}, deferred]() mutable {
                    wasm3::module module = wasmEnv.parse_module(static_cast<uint8_t*>(bufferSourceRef.Value().Data()), bufferSourceRef.Value().ByteLength());
                    wasm3::runtime runtime = wasmEnv.new_runtime(1024);
                    runtime.load(module);

                    deferred.Resolve(env.Global().Get(WebAssembly::JS_CLASS_NAME).As<Napi::Function>().Get(Module::JS_CLASS_NAME).As<Napi::Function>().New({
                        Napi::External<wasm3::module>::New(env, new wasm3::module(std::move(module)), [](Napi::Env, wasm3::module* module) { delete module; }),
                        Napi::External<wasm3::runtime>::New(env, new wasm3::runtime(std::move(runtime)), [](Napi::Env, wasm3::runtime* runtime) { delete runtime; }),
                    }));
                }
            );

            return deferred.Promise();
        }

        static Napi::Value Instantiate(const Napi::CallbackInfo& info)
        {
            // For now just return the passed in module, and expose the exports from module
            return info[0];
            //return info.Env().Undefined();
        }

    private:
        JsRuntime& m_runtime;
    };
}

namespace Babylon::Polyfills::WebAssembly
{
    void Initialize(Napi::Env env)
    {
        Internal::WebAssembly::Initialize(env);
        //Internal::Module::Initialize(env);
    }
}
