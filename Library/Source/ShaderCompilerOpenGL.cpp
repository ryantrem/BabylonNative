#include "ShaderCompiler.h"
#include "ResourceLimits.h"
#include <arcana/experimental/array.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_parser.hpp>
#include <spirv_glsl.hpp>
#include "Console.h"

namespace babylon
{
    extern const TBuiltInResource DefaultTBuiltInResource;

    namespace
    {
        void AddShader(glslang::TProgram& program, glslang::TShader& shader, std::string_view source)
        {
            const std::array<const char*, 1> sources{ source.data() };
            shader.setStrings(sources.data(), gsl::narrow_cast<int>(sources.size()));
            shader.setEnvInput(glslang::EShSourceGlsl, shader.getStage(), glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

            if (!shader.parse(&DefaultTBuiltInResource, 450, false, EShMsgDefault))
            {
                throw std::exception();
            }

            program.addShader(&shader);
        }

        std::unique_ptr<spirv_cross::Compiler> CompileShader(glslang::TProgram& program, EShLanguage stage, std::string& glsl)
        {
            std::vector<uint32_t> spirv;
            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);

            spirv_cross::Parser parser{ std::move(spirv) };
            parser.parse();

            auto compiler = std::make_unique<spirv_cross::CompilerGLSL>(parser.get_parsed_ir());
            
            compiler->build_combined_image_samplers();
            
            spirv_cross::CompilerGLSL::Options options = compiler->get_common_options();
#ifdef ANDROID
            options.version = 300;
            options.es = true;
#else
            options.version = 430;
            options.es = false;
#endif
            compiler->set_common_options(options);
            
#if 0
            spirv_cross::ShaderResources resources = compiler->get_shader_resources();
            for (auto& resource : resources.uniform_buffers)
            {
                auto str = resource.name;
                auto fbn = compiler->get_fallback_name(22);
                auto activeBuffer = compiler->get_active_buffer_ranges(resource.id);

                //auto& type = compiler->get<spirv_cross::SPIRType>(resource.type_id);
                // Modify the decoration to prepare it for GLSL.
                compiler->unset_decoration(resource.id, spv::DecorationDescriptorSet);

                for (auto& active : activeBuffer)
                {
                    auto memberName = compiler->get_member_name(resource.id, active.index);
                    //auto memberType = compiler->get_type(active.)
                    int a = 1;
                }
                
                //compiler->set_name(22, "truc");
            }
/*
            compiler->set_variable_type_remap_callback([](const spirv_cross::SPIRType& type, const std::string& var_name, std::string& name_of_type){
                
                });
                */
            program.buildReflection();
            //int numblk = program.getNumUniformVariables();


            //auto& block = program.getUniformBlock(0);
#endif

            program.buildReflection();
            int numUniforms = program.getNumUniformVariables();

            for (int i = 0; i < numUniforms; i++)
            {
                const auto& uniform = program.getUniform(i);
                std::string uniformString = "uniform highp ";
                if (uniform.glDefineType == 0x8B5C) //GL_FLOAT_MAT4
                {
                    uniformString += "mat4 ";
                }
                else
                {
                    uniformString += "vec4 ";
                }
                uniformString += uniform.name;
                uniformString += ";";
                compiler->add_header_line(uniformString);
            }

            std::string compiled = compiler->compile();

            // rename "uniform Frame { .... }" . Keep the uniforms
            const std::string frameNewName = ((stage == EShLangVertex)?"FrameVS":"FrameFS");
            const std::string frame = "Frame";
            size_t pos = compiled.find(frame);
            if (pos != std::string::npos)
            {
                compiled.replace(pos, frame.size(), frameNewName);
            }
            
            spirv_cross::ShaderResources resources = compiler->get_shader_resources();
            for (auto& resource : resources.uniform_buffers)
            {
                auto str = resource.name;
                auto fbn = compiler->get_fallback_name(resource.id);
                std::string rep = fbn + ".";
                size_t pos = compiled.find(rep);
                while (pos != std::string::npos)
                {
                    compiled.replace(pos, rep.size(), "");
                    pos = compiled.find(rep);
                }
            }

#ifdef ANDROID
            glsl = compiled.substr(strlen("#version 300 es\n"));

            // frag def
            static const std::string fragDef = "layout(location = 0) out highp vec4 glFragColor;";
            pos = glsl.find(fragDef);
            if (pos != std::string::npos)
            {
                glsl.replace(pos, fragDef.size(), "");
            }

            // frag
            static const std::string fragColor = "glFragColor";
            pos = glsl.find(fragColor);
            if (pos != std::string::npos)
            {
                glsl.replace(pos, fragColor.size(), "gl_FragColor");
            }
#if 0
            static const std::string cmt = "layout(std140) uniform";
            pos = glsl.find(cmt);
            if (pos != std::string::npos)
            {
                glsl.replace(pos, cmt.size(), "/*");
            }
            
            static const std::string cmt2 = "_22;";
            pos = glsl.find(cmt2);
            if (pos != std::string::npos)
            {
                glsl.replace(pos, cmt2.size(), "*/");
            }

            static const std::string cmt3 = "_69;";
            pos = glsl.find(cmt3);
            if (pos != std::string::npos)
            {
                glsl.replace(pos, cmt3.size(), "*/");
            }
#endif
#else 
            glsl = compiled;
#endif
            return std::move(compiler);
        }
    }

    ShaderCompiler::ShaderCompiler()
    {
        glslang::InitializeProcess();
    }

    ShaderCompiler::~ShaderCompiler()
    {
        glslang::FinalizeProcess();
    }

    void ShaderCompiler::Compile(std::string_view vertexSource, std::string_view fragmentSource, std::function<void(ShaderInfo, ShaderInfo)> onCompiled)
    {
        glslang::TProgram program;

        glslang::TShader vertexShader{ EShLangVertex };
        AddShader(program, vertexShader, vertexSource);

        glslang::TShader fragmentShader{ EShLangFragment };
        AddShader(program, fragmentShader, fragmentSource);

        if (!program.link(EShMsgDefault))
        {
            throw std::exception();
        }

        std::string vertexGLSL(vertexSource.data(), vertexSource.size());
        auto vertexCompiler = CompileShader(program, EShLangVertex, vertexGLSL);

        std::string fragmentGLSL(fragmentSource.data(), fragmentSource.size());
        auto fragmentCompiler = CompileShader(program, EShLangFragment, fragmentGLSL);

        uint8_t* strVertex = (uint8_t*)vertexGLSL.data();
        uint8_t* strFragment = (uint8_t*)fragmentGLSL.data();
        onCompiled
        (
            { std::move(vertexCompiler), gsl::make_span(strVertex, vertexGLSL.size()) },
            { std::move(fragmentCompiler), gsl::make_span(strFragment, fragmentGLSL.size()) }
        );
    }
}
