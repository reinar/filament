/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "filamat/MaterialBuilder.h"

#include <atomic>
#include <utility>
#include <vector>

#include <utils/JobSystem.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Panic.h>

#include <filamat/Enums.h>

#include <private/filament/UniformInterfaceBlock.h>
#include <private/filament/SamplerInterfaceBlock.h>
#include <private/filament/UibStructs.h>

#include "SibGenerator.h"

#include "MaterialVariants.h"

#include "shaders/MaterialInfo.h"
#include "shaders/ShaderGenerator.h"

#include "eiff/BlobDictionary.h"
#include "eiff/LineDictionary.h"
#include "eiff/MaterialInterfaceBlockChunk.h"
#include "eiff/MaterialTextChunk.h"
#include "eiff/MaterialSpirvChunk.h"
#include "eiff/ChunkContainer.h"
#include "eiff/SimpleFieldChunk.h"
#include "eiff/DictionaryTextChunk.h"
#include "eiff/DictionarySpirvChunk.h"

#include "Includes.h"

#ifndef FILAMAT_LITE
#include "GLSLPostProcessor.h"
#include "sca/GLSLTools.h"
#else
#include "sca/GLSLToolsLite.h"
#endif


namespace filamat {

using namespace utils;
using namespace filament;

std::atomic<int> MaterialBuilderBase::materialBuilderClients(0);

inline void assertSingleTargetApi(MaterialBuilderBase::TargetApi api) {
    // Assert that a single bit is set.
    UTILS_UNUSED uint8_t bits = (uint8_t) api;
    assert(bits && !(bits & bits - 1u));
}

void MaterialBuilderBase::prepare(bool vulkanSemantics) {
    mCodeGenPermutations.clear();
    mShaderModels.reset();

    if (mPlatform == Platform::MOBILE) {
        mShaderModels.set(static_cast<size_t>(ShaderModel::MOBILE));
    } else if (mPlatform == Platform::DESKTOP) {
        mShaderModels.set(static_cast<size_t>(ShaderModel::DESKTOP));
    } else if (mPlatform == Platform::ALL) {
        mShaderModels.set(static_cast<size_t>(ShaderModel::MOBILE));
        mShaderModels.set(static_cast<size_t>(ShaderModel::DESKTOP));
    }

    // OpenGL is a special case. If we're doing any optimization, then we need to go to Spir-V.
    TargetLanguage glTargetLanguage = mOptimization > MaterialBuilder::Optimization::PREPROCESSOR ?
            TargetLanguage::SPIRV : TargetLanguage::GLSL;
    if (vulkanSemantics) {
        // Currently GLSLPostProcessor.cpp is incapable of compiling SPIRV to GLSL without
        // running the optimizer. For now we just activate the optimizer in that case.
        mOptimization = MaterialBuilder::Optimization::PERFORMANCE;
        glTargetLanguage = TargetLanguage::SPIRV;
    }

    // Select OpenGL as the default TargetApi if none was specified.
    if (none(mTargetApi)) {
        mTargetApi = TargetApi::OPENGL;
    }

    // Build a list of codegen permutations, which is useful across all types of material builders.
    static_assert(backend::SHADER_MODEL_COUNT == 2);
    for (const auto shaderModel: { ShaderModel::MOBILE, ShaderModel::DESKTOP }) {
        const auto i = static_cast<uint8_t>(shaderModel);
        if (!mShaderModels.test(i)) {
            continue; // skip this shader model since it was not requested.
        }
        if (any(mTargetApi & TargetApi::OPENGL)) {
            mCodeGenPermutations.push_back({shaderModel, TargetApi::OPENGL, glTargetLanguage});
        }
        if (any(mTargetApi & TargetApi::VULKAN)) {
            mCodeGenPermutations.push_back({shaderModel, TargetApi::VULKAN, TargetLanguage::SPIRV});
        }
        if (any(mTargetApi & TargetApi::METAL)) {
            mCodeGenPermutations.push_back({shaderModel, TargetApi::METAL, TargetLanguage::SPIRV});
        }
    }
}

MaterialBuilder::MaterialBuilder() : mMaterialName("Unnamed") {
    std::fill_n(mProperties, MATERIAL_PROPERTIES_COUNT, false);
    mShaderModels.reset();
}

void MaterialBuilderBase::init() {
    materialBuilderClients++;
#ifndef FILAMAT_LITE
    GLSLTools::init();
#endif
}

void MaterialBuilderBase::shutdown() {
    materialBuilderClients--;
#ifndef FILAMAT_LITE
    GLSLTools::shutdown();
#endif
}

MaterialBuilder& MaterialBuilder::name(const char* name) noexcept {
    mMaterialName = CString(name);
    return *this;
}

MaterialBuilder& MaterialBuilder::fileName(const char* fileName) noexcept {
    mFileName = CString(fileName);
    return *this;
}

MaterialBuilder& MaterialBuilder::material(const char* code, size_t line) noexcept {
    mMaterialFragmentCode.setUnresolved(CString(code));
    mMaterialFragmentCode.setLineOffset(line);
    return *this;
}

MaterialBuilder& MaterialBuilder::includeCallback(IncludeCallback callback) noexcept {
    mIncludeCallback = std::move(callback);
    return *this;
}

MaterialBuilder& MaterialBuilder::materialVertex(const char* code, size_t line) noexcept {
    mMaterialVertexCode.setUnresolved(CString(code));
    mMaterialVertexCode.setLineOffset(line);
    return *this;
}

MaterialBuilder& MaterialBuilder::shading(Shading shading) noexcept {
    mShading = shading;
    return *this;
}

MaterialBuilder& MaterialBuilder::interpolation(Interpolation interpolation) noexcept {
    mInterpolation = interpolation;
    return *this;
}

MaterialBuilder& MaterialBuilder::variable(Variable v, const char* name) noexcept {
    switch (v) {
        case Variable::CUSTOM0:
        case Variable::CUSTOM1:
        case Variable::CUSTOM2:
        case Variable::CUSTOM3:
            assert(size_t(v) < MATERIAL_VARIABLES_COUNT);
            mVariables[size_t(v)] = CString(name);
            break;
    }
    return *this;
}

MaterialBuilder& MaterialBuilder::parameter(
        UniformType type, ParameterPrecision precision, const char* name) noexcept {
    ASSERT_POSTCONDITION(mParameterCount < MAX_PARAMETERS_COUNT, "Too many parameters");
    mParameters[mParameterCount++] = { name, type, 1, precision };
    return *this;
}

MaterialBuilder& MaterialBuilder::parameter(
        UniformType type, size_t size, ParameterPrecision precision, const char* name) noexcept {
    ASSERT_POSTCONDITION(mParameterCount < MAX_PARAMETERS_COUNT, "Too many parameters");
    mParameters[mParameterCount++] = { name, type, size, precision };
    return *this;
}

MaterialBuilder& MaterialBuilder::parameter(
        SamplerType samplerType, SamplerFormat format, ParameterPrecision precision, const char* name) noexcept {
    ASSERT_POSTCONDITION(mParameterCount < MAX_PARAMETERS_COUNT, "Too many parameters");
    mParameters[mParameterCount++] = { name, samplerType, format, precision };
    return *this;
}

MaterialBuilder& MaterialBuilder::parameter(SubpassType subpassType, SamplerFormat format,
        ParameterPrecision precision, const char* name) noexcept {
    ASSERT_PRECONDITION(format == SamplerFormat::FLOAT,
            "Subpass parameters must have FLOAT format.");

    auto subpassCount = std::count_if(std::begin(mParameters), std::end(mParameters),
            [](const auto& p) { return p.isSubpass(); });

    ASSERT_POSTCONDITION(subpassCount < MAX_SUBPASS_COUNT, "Too many subpasses");
    ASSERT_POSTCONDITION(mParameterCount < MAX_PARAMETERS_COUNT, "Too many parameters");
    mParameters[mParameterCount++] = { name, subpassType, format, precision };
    return *this;
}

MaterialBuilder& MaterialBuilder::parameter(
        SamplerType samplerType, SamplerFormat format, const char* name) noexcept {
    return parameter(samplerType, format, ParameterPrecision::DEFAULT, name);
}

MaterialBuilder& MaterialBuilder::parameter(
        SamplerType samplerType, ParameterPrecision precision, const char* name) noexcept {
    return parameter(samplerType, SamplerFormat::FLOAT, precision, name);
}

MaterialBuilder& MaterialBuilder::parameter(
        SamplerType samplerType, const char* name) noexcept {
    return parameter(samplerType, SamplerFormat::FLOAT, ParameterPrecision::DEFAULT, name);
}

MaterialBuilder& MaterialBuilder::parameter(SubpassType subpassType, SamplerFormat format,
        const char* name) noexcept {
    return parameter(subpassType, format, ParameterPrecision::DEFAULT, name);
}

MaterialBuilder& MaterialBuilder::parameter(SubpassType subpassType, ParameterPrecision precision,
        const char* name) noexcept {
    return parameter(subpassType, SamplerFormat::FLOAT, precision, name);
}

MaterialBuilder& MaterialBuilder::parameter(SubpassType subpassType, const char* name) noexcept {
    return parameter(subpassType, SamplerFormat::FLOAT, ParameterPrecision::DEFAULT, name);
}

MaterialBuilder& MaterialBuilder::require(VertexAttribute attribute) noexcept {
    mRequiredAttributes.set(attribute);
    return *this;
}

MaterialBuilder& MaterialBuilder::materialDomain(MaterialDomain materialDomain) noexcept {
    mMaterialDomain = materialDomain;
    return *this;
}

MaterialBuilder& MaterialBuilder::refractionMode(RefractionMode refraction) noexcept {
    mRefractionMode = refraction;
    return *this;
}

MaterialBuilder& MaterialBuilder::refractionType(RefractionType refractionType) noexcept {
    mRefractionType = refractionType;
    return *this;
}

MaterialBuilder& MaterialBuilder::quality(ShaderQuality quality) noexcept {
    mShaderQuality = quality;
    return *this;
}

MaterialBuilder& MaterialBuilder::featureLevel(FeatureLevel featureLevel) noexcept {
    mFeatureLevel = featureLevel;
    return *this;
}

MaterialBuilder& MaterialBuilder::blending(BlendingMode blending) noexcept {
    mBlendingMode = blending;
    return *this;
}

MaterialBuilder& MaterialBuilder::postLightingBlending(BlendingMode blending) noexcept {
    mPostLightingBlendingMode = blending;
    return *this;
}

MaterialBuilder& MaterialBuilder::vertexDomain(VertexDomain domain) noexcept {
    mVertexDomain = domain;
    return *this;
}

MaterialBuilder& MaterialBuilder::culling(CullingMode culling) noexcept {
    mCullingMode = culling;
    return *this;
}

MaterialBuilder& MaterialBuilder::colorWrite(bool enable) noexcept {
    mColorWrite = enable;
    return *this;
}

MaterialBuilder& MaterialBuilder::depthWrite(bool enable) noexcept {
    mDepthWrite = enable;
    mDepthWriteSet = true;
    return *this;
}

MaterialBuilder& MaterialBuilder::depthCulling(bool enable) noexcept {
    mDepthTest = enable;
    return *this;
}

MaterialBuilder& MaterialBuilder::instanced(bool enable) noexcept {
    mInstanced = enable;
    return *this;
}

MaterialBuilder& MaterialBuilder::doubleSided(bool doubleSided) noexcept {
    mDoubleSided = doubleSided;
    mDoubleSidedCapability = true;
    return *this;
}

MaterialBuilder& MaterialBuilder::maskThreshold(float threshold) noexcept {
    mMaskThreshold = threshold;
    return *this;
}

MaterialBuilder& MaterialBuilder::shadowMultiplier(bool shadowMultiplier) noexcept {
    mShadowMultiplier = shadowMultiplier;
    return *this;
}

MaterialBuilder& MaterialBuilder::transparentShadow(bool transparentShadow) noexcept {
    mTransparentShadow = transparentShadow;
    return *this;
}

MaterialBuilder& MaterialBuilder::specularAntiAliasing(bool specularAntiAliasing) noexcept {
    mSpecularAntiAliasing = specularAntiAliasing;
    return *this;
}

MaterialBuilder& MaterialBuilder::specularAntiAliasingVariance(float screenSpaceVariance) noexcept {
    mSpecularAntiAliasingVariance = screenSpaceVariance;
    return *this;
}

MaterialBuilder& MaterialBuilder::specularAntiAliasingThreshold(float threshold) noexcept {
    mSpecularAntiAliasingThreshold = threshold;
    return *this;
}

MaterialBuilder& MaterialBuilder::clearCoatIorChange(bool clearCoatIorChange) noexcept {
    mClearCoatIorChange = clearCoatIorChange;
    return *this;
}

MaterialBuilder& MaterialBuilder::flipUV(bool flipUV) noexcept {
    mFlipUV = flipUV;
    return *this;
}

MaterialBuilder& MaterialBuilder::customSurfaceShading(bool customSurfaceShading) noexcept {
    mCustomSurfaceShading = customSurfaceShading;
    return *this;
}

MaterialBuilder& MaterialBuilder::multiBounceAmbientOcclusion(bool multiBounceAO) noexcept {
    mMultiBounceAO = multiBounceAO;
    mMultiBounceAOSet = true;
    return *this;
}

MaterialBuilder& MaterialBuilder::specularAmbientOcclusion(SpecularAmbientOcclusion specularAO) noexcept {
    mSpecularAO = specularAO;
    mSpecularAOSet = true;
    return *this;
}

MaterialBuilder& MaterialBuilder::transparencyMode(TransparencyMode mode) noexcept {
    mTransparencyMode = mode;
    return *this;
}

MaterialBuilder& MaterialBuilder::reflectionMode(ReflectionMode mode) noexcept {
    mReflectionMode = mode;
    return *this;
}

MaterialBuilder& MaterialBuilder::platform(Platform platform) noexcept {
    mPlatform = platform;
    return *this;
}

MaterialBuilder& MaterialBuilder::targetApi(TargetApi targetApi) noexcept {
    mTargetApi |= targetApi;
    return *this;
}

MaterialBuilder& MaterialBuilder::optimization(Optimization optimization) noexcept {
    mOptimization = optimization;
    return *this;
}

MaterialBuilder& MaterialBuilder::printShaders(bool printShaders) noexcept {
    mPrintShaders = printShaders;
    return *this;
}

MaterialBuilder& MaterialBuilder::generateDebugInfo(bool generateDebugInfo) noexcept {
    mGenerateDebugInfo = generateDebugInfo;
    return *this;
}

MaterialBuilder& MaterialBuilder::variantFilter(UserVariantFilterMask variantFilter) noexcept {
    mVariantFilter = variantFilter;
    return *this;
}

MaterialBuilder& MaterialBuilder::shaderDefine(const char* name, const char* value) noexcept {
    mDefines.emplace_back(name, value);
    return *this;
}

bool MaterialBuilder::hasExternalSampler() const noexcept {
    for (size_t i = 0, c = mParameterCount; i < c; i++) {
        auto const& param = mParameters[i];
        if (param.isSampler() && param.samplerType == SamplerType::SAMPLER_EXTERNAL) {
            return  true;
        }
    }
    return false;
}

void MaterialBuilder::prepareToBuild(MaterialInfo& info) noexcept {
    MaterialBuilderBase::prepare(mEnableFramebufferFetch);

    // Build the per-material sampler block and uniform block.
    SamplerInterfaceBlock::Builder sbb;
    UniformInterfaceBlock::Builder ibb;
    for (size_t i = 0, c = mParameterCount; i < c; i++) {
        auto const& param = mParameters[i];
        if (param.isSampler()) {
            sbb.add({ param.name.data(), param.name.size() },
                    param.samplerType, param.format, param.precision);
        } else if (param.isUniform()) {
            ibb.add({{{ param.name.data(), param.name.size() },
                      uint32_t(param.size == 1u ? 0u : param.size), param.uniformType, param.precision }});
        } else if (param.isSubpass()) {
            // For now, we only support a single subpass for attachment 0.
            // Subpasses belong to the "MaterialParams" block.
            const uint8_t attachmentIndex = 0;
            const uint8_t binding = 0;
            info.subpass = { CString("MaterialParams"), param.name, param.subpassType,
                param.format, param.precision, attachmentIndex, binding };
        }
    }

    if (mSpecularAntiAliasing) {
        ibb.add({
                { "_specularAntiAliasingVariance",  0, UniformType::FLOAT },
                { "_specularAntiAliasingThreshold", 0, UniformType::FLOAT },
        });
    }

    if (mBlendingMode == BlendingMode::MASKED) {
        ibb.add({{ "_maskThreshold", 0, UniformType::FLOAT }});
    }

    if (mDoubleSidedCapability) {
        ibb.add({{ "_doubleSided", 0, UniformType::BOOL }});
    }

    mRequiredAttributes.set(VertexAttribute::POSITION);
    if (mShading != Shading::UNLIT || mShadowMultiplier) {
        mRequiredAttributes.set(VertexAttribute::TANGENTS);
    }

    info.sib = sbb.name("MaterialParams").build();
    info.uib = ibb.name("MaterialParams").build();

    info.isLit = isLit();
    info.hasDoubleSidedCapability = mDoubleSidedCapability;
    info.hasExternalSamplers = hasExternalSampler();
    info.specularAntiAliasing = mSpecularAntiAliasing;
    info.clearCoatIorChange = mClearCoatIorChange;
    info.flipUV = mFlipUV;
    info.requiredAttributes = mRequiredAttributes;
    info.blendingMode = mBlendingMode;
    info.postLightingBlendingMode = mPostLightingBlendingMode;
    info.shading = mShading;
    info.hasShadowMultiplier = mShadowMultiplier;
    info.hasTransparentShadow = mTransparentShadow;
    info.multiBounceAO = mMultiBounceAO;
    info.multiBounceAOSet = mMultiBounceAOSet;
    info.specularAO = mSpecularAO;
    info.specularAOSet = mSpecularAOSet;
    info.refractionMode = mRefractionMode;
    info.refractionType = mRefractionType;
    info.reflectionMode = mReflectionMode;
    info.quality = mShaderQuality;
    info.hasCustomSurfaceShading = mCustomSurfaceShading;
    info.useLegacyMorphing = mUseLegacyMorphing;
    info.instanced = mInstanced;
    info.vertexDomainDeviceJittered = mVertexDomainDeviceJittered;
    info.featureLevel = mFeatureLevel;
}

bool MaterialBuilder::findProperties(backend::ShaderType type,
        MaterialBuilder::PropertyList& allProperties) noexcept {
#ifndef FILAMAT_LITE
    GLSLTools glslTools;
    std::string shaderCodeAllProperties = peek(type, mSemanticCodeGenParams, allProperties);
    // Populate mProperties with the properties set in the shader.
    if (!glslTools.findProperties(type, shaderCodeAllProperties, mProperties,
            mSemanticCodeGenParams.targetApi,
            mSemanticCodeGenParams.targetLanguage,
            mSemanticCodeGenParams.shaderModel)) {
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool MaterialBuilder::findAllProperties() noexcept {
    if (mMaterialDomain != MaterialDomain::SURFACE) {
        return true;
    }

    using namespace backend;

#ifndef FILAMAT_LITE
    // Some fields in MaterialInputs only exist if the property is set (e.g: normal, subsurface
    // for cloth shading model). Give our shader all properties. This will enable us to parse and
    // static code analyse the AST.
    MaterialBuilder::PropertyList allProperties;
    std::fill_n(allProperties, MATERIAL_PROPERTIES_COUNT, true);
    if (!findProperties(ShaderType::FRAGMENT, allProperties)) {
        return false;
    }
    if (!findProperties(ShaderType::VERTEX, allProperties)) {
        return false;
    }
    return true;
#else
    GLSLToolsLite glslTools;
    if (glslTools.findProperties(ShaderType::FRAGMENT, mMaterialFragmentCode.getResolved(), mProperties)) {
        return glslTools.findProperties(
                ShaderType::VERTEX, mMaterialVertexCode.getResolved(), mProperties);
    }
    return false;
#endif
}

bool MaterialBuilder::runSemanticAnalysis(MaterialInfo const& info) noexcept {
#ifndef FILAMAT_LITE
    using namespace backend;
    GLSLTools glslTools;

    TargetApi targetApi = mSemanticCodeGenParams.targetApi;
    TargetLanguage targetLanguage = mSemanticCodeGenParams.targetLanguage;
    assertSingleTargetApi(targetApi);

    if (mEnableFramebufferFetch) {
        // framebuffer fetch is only available with vulkan semantics
        targetApi = TargetApi::VULKAN;
    }

    ShaderModel model = static_cast<ShaderModel>(mSemanticCodeGenParams.shaderModel);
    std::string shaderCode = peek(ShaderType::VERTEX, mSemanticCodeGenParams, mProperties);
    bool result = GLSLTools::analyzeVertexShader(shaderCode, model, mMaterialDomain,
            targetApi, targetLanguage, info);
    if (!result) return false;

    shaderCode = peek(ShaderType::FRAGMENT, mSemanticCodeGenParams, mProperties);
    result = GLSLTools::analyzeFragmentShader(shaderCode, model, mMaterialDomain,
            targetApi, targetLanguage, mCustomSurfaceShading, info);
    return result;
#else
    return true;
#endif
}

bool MaterialBuilder::checkLiteRequirements() noexcept {
#ifdef FILAMAT_LITE
    if (mTargetApi != TargetApi::OPENGL) {
        slog.e
                << "Filamat lite only supports building materials for the OpenGL backend."
                << io::endl;
        return false;
    }

    if (mOptimization != Optimization::NONE) {
        slog.e
                << "Filamat lite does not support material optimization." << io::endl
                << "Ensure optimization is set to NONE." << io::endl;
        return false;
    }
#endif
    return true;
}

bool MaterialBuilder::ShaderCode::resolveIncludes(IncludeCallback callback,
        const CString& fileName) noexcept {
    if (!mCode.empty()) {
        ResolveOptions options {
            .insertLineDirectives = true,
            .insertLineDirectiveCheck = true
        };
        IncludeResult source {
            .includeName = fileName,
            .text = mCode,
            .lineNumberOffset = getLineOffset(),
            .name = CString("")
        };
        if (!::filamat::resolveIncludes(source, std::move(callback), options)) {
            return false;
        }
        mCode = source.text;
    }

    mIncludesResolved = true;
    return true;
}

static void showErrorMessage(const char* materialName, filament::Variant variant,
        MaterialBuilder::TargetApi targetApi, backend::ShaderType shaderType,
        const std::string& shaderCode) {
    using ShaderType = backend::ShaderType;
    using TargetApi = MaterialBuilder::TargetApi;
    slog.e
            << "Error in \"" << materialName << "\""
            << ", Variant 0x" << io::hex << +variant.key
            << (targetApi == TargetApi::VULKAN ? ", Vulkan.\n" : ", OpenGL.\n")
            << "=========================\n"
            << "Generated "
            << (shaderType == ShaderType::VERTEX ? "Vertex Shader\n" : "Fragment Shader\n")
            << "=========================\n"
            << shaderCode;
}

bool MaterialBuilder::generateShaders(JobSystem& jobSystem, const std::vector<Variant>& variants,
        ChunkContainer& container, const MaterialInfo& info) const noexcept {
    // Create a postprocessor to optimize / compile to Spir-V if necessary.
#ifndef FILAMAT_LITE
    uint32_t flags = 0;
    flags |= mPrintShaders ? GLSLPostProcessor::PRINT_SHADERS : 0;
    flags |= mGenerateDebugInfo ? GLSLPostProcessor::GENERATE_DEBUG_INFO : 0;
    GLSLPostProcessor postProcessor(mOptimization, flags);
#endif

    // Start: must be protected by lock
    Mutex entriesLock;
    std::vector<TextEntry> glslEntries;
    std::vector<SpirvEntry> spirvEntries;
    std::vector<TextEntry> metalEntries;
    LineDictionary textDictionary;
#ifndef FILAMAT_LITE
    BlobDictionary spirvDictionary;
#endif
    // End: must be protected by lock

    ShaderGenerator sg(
            mProperties, mVariables, mOutputs, mDefines, mMaterialFragmentCode.getResolved(),
            mMaterialFragmentCode.getLineOffset(), mMaterialVertexCode.getResolved(),
            mMaterialVertexCode.getLineOffset(), mMaterialDomain);

    container.addSimpleChild<bool>(ChunkType::MaterialHasCustomDepthShader, needsStandardDepthProgram());

    std::atomic_bool cancelJobs(false);
    bool firstJob = true;

    for (const auto& params : mCodeGenPermutations) {
        if (cancelJobs.load()) {
            return false;
        }

        const ShaderModel shaderModel = ShaderModel(params.shaderModel);
        const TargetApi targetApi = params.targetApi;
        const TargetLanguage targetLanguage = params.targetLanguage;

        assertSingleTargetApi(targetApi);

        // Metal Shading Language is cross-compiled from Vulkan.
        const bool targetApiNeedsSpirv =
                (targetApi == TargetApi::VULKAN || targetApi == TargetApi::METAL);
        const bool targetApiNeedsMsl = targetApi == TargetApi::METAL;
        const bool targetApiNeedsGlsl = targetApi == TargetApi::OPENGL;

        // Set when a job fails
        JobSystem::Job* parent = jobSystem.createJob();

        for (const auto& v : variants) {
            JobSystem::Job* job = jobs::createJob(jobSystem, parent, [&]() {
                if (cancelJobs.load()) {
                    return;
                }

                // TODO: avoid allocations when not required
                std::vector<uint32_t> spirv;
                std::string msl;

                std::vector<uint32_t>* pSpirv = targetApiNeedsSpirv ? &spirv : nullptr;
                std::string* pMsl = targetApiNeedsMsl ? &msl : nullptr;

                TextEntry glslEntry{0};
                SpirvEntry spirvEntry{0};
                TextEntry metalEntry{0};

                glslEntry.shaderModel = static_cast<uint8_t>(params.shaderModel);
                spirvEntry.shaderModel = static_cast<uint8_t>(params.shaderModel);
                metalEntry.shaderModel = static_cast<uint8_t>(params.shaderModel);

                glslEntry.variantKey  = v.variant.key;
                spirvEntry.variantKey = v.variant.key;
                metalEntry.variantKey = v.variant.key;

                // Generate raw shader code.
                // The quotes in Google-style line directives cause problems with certain drivers. These
                // directives are optimized away when using the full filamat, so down below we
                // explicitly remove them when using filamat lite.
                std::string shader;
                if (v.stage == backend::ShaderType::VERTEX) {
                    shader = sg.createVertexProgram(
                            shaderModel, targetApi, targetLanguage, info, v.variant,
                            mInterpolation, mVertexDomain);
#ifdef FILAMAT_LITE
                    GLSLToolsLite glslTools;
                    glslTools.removeGoogleLineDirectives(shader);
#endif
                } else if (v.stage == backend::ShaderType::FRAGMENT) {
                    shader = sg.createFragmentProgram(
                            shaderModel, targetApi, targetLanguage, info, v.variant, mInterpolation);
#ifdef FILAMAT_LITE
                    GLSLToolsLite glslTools;
                    glslTools.removeGoogleLineDirectives(shader);
#endif
                }

                std::string* pGlsl = nullptr;
                if (targetApiNeedsGlsl) {
                    pGlsl = &shader;
                }

#ifndef FILAMAT_LITE
                GLSLPostProcessor::Config config{
                        .variant = v.variant,
                        .targetApi = targetApi,
                        .targetLanguage = targetLanguage,
                        .shaderType = v.stage,
                        .shaderModel = shaderModel,
                        .domain = mMaterialDomain,
                        .materialInfo = &info,
                        .hasFramebufferFetch = mEnableFramebufferFetch,
                        .glsl = {},
                };

                if (mEnableFramebufferFetch) {
                    config.glsl.subpassInputToColorLocation.emplace_back(0, 0);
                }

                bool ok = postProcessor.process(shader, config, pGlsl, pSpirv, pMsl);
#else
                bool ok = true;
#endif
                if (!ok) {
                    showErrorMessage(mMaterialName.c_str_safe(), v.variant, targetApi, v.stage, shader);
                    cancelJobs = true;
                    return;
                }

                if (targetApi == TargetApi::OPENGL) {
                    if (targetLanguage == TargetLanguage::SPIRV) {
                        ShaderGenerator::fixupExternalSamplers(shaderModel, shader, info);
                    }
                }

                // NOTE: Everything below touches shared structures protected by a lock
                // NOTE: do not execute expensive work from here on!
                std::unique_lock<Mutex> lock(entriesLock);

                // below we rely on casting ShaderType to uint8_t
                static_assert(sizeof(filament::backend::ShaderType) == 1);


                switch (targetApi) {
                    case TargetApi::ALL:
                        // should never happen
                        break;
                    case TargetApi::OPENGL:
                        glslEntry.stage = uint8_t(v.stage);
                        glslEntry.shader = shader;
                        glslEntries.push_back(glslEntry);
                        break;
                    case TargetApi::VULKAN:
#ifndef FILAMAT_LITE
                        assert(!spirv.empty());
                        spirvEntry.stage = uint8_t(v.stage);
                        spirvEntry.spirv = std::move(spirv);
                        spirvEntries.push_back(spirvEntry);
#endif
                        break;
                    case TargetApi::METAL:
#ifndef FILAMAT_LITE
                        assert(!spirv.empty());
                        assert(msl.length() > 0);
                        metalEntry.stage = uint8_t(v.stage);
                        metalEntry.shader = msl;
                        metalEntries.push_back(metalEntry);
#endif
                        break;
                }
            });

            // NOTE: We run the first job separately to work the lack of thread safety
            //       guarantees in glslang. This library performs unguarded global
            //       operations on first use.
            if (firstJob) {
                jobSystem.runAndWait(job);
                firstJob = false;
            } else {
                jobSystem.run(job);
            }
        }

        jobSystem.runAndWait(parent);
    }

    if (cancelJobs.load()) {
        return false;
    }

    // Sort the variants.
    auto compare = [](const auto& a, const auto& b) {
        static_assert(sizeof(decltype(a.variantKey)) == 1);
        static_assert(sizeof(decltype(b.variantKey)) == 1);
        const uint32_t akey = (a.shaderModel << 16) | (a.variantKey << 8) | a.stage;
        const uint32_t bkey = (b.shaderModel << 16) | (b.variantKey << 8) | b.stage;
        return akey < bkey;
    };
    std::sort(glslEntries.begin(), glslEntries.end(), compare);
    std::sort(spirvEntries.begin(), spirvEntries.end(), compare);
    std::sort(metalEntries.begin(), metalEntries.end(), compare);

    // Generate the dictionaries.
    for (const auto& s : glslEntries) {
        textDictionary.addText(s.shader);
    }
#ifndef FILAMAT_LITE
    for (auto& s : spirvEntries) {
        std::vector<uint32_t> spirv = std::move(s.spirv);
        s.dictionaryIndex = spirvDictionary.addBlob(spirv);
    }
#endif
    for (const auto& s : metalEntries) {
        textDictionary.addText(s.shader);
    }

    // Emit dictionary chunk (TextDictionaryReader and DictionaryTextChunk)
    const auto& dictionaryChunk = container.addChild<filamat::DictionaryTextChunk>(
            std::move(textDictionary), ChunkType::DictionaryText);

    // Emit GLSL chunk (MaterialTextChunk).
    if (!glslEntries.empty()) {
        container.addChild<MaterialTextChunk>(std::move(glslEntries),
                dictionaryChunk.getDictionary(), ChunkType::MaterialGlsl);
    }

    // Emit SPIRV chunks (SpirvDictionaryReader and MaterialSpirvChunk).
#ifndef FILAMAT_LITE
    if (!spirvEntries.empty()) {
        const bool stripInfo = !mGenerateDebugInfo;
        container.addChild<filamat::DictionarySpirvChunk>(std::move(spirvDictionary), stripInfo);
        container.addChild<MaterialSpirvChunk>(std::move(spirvEntries));
    }

    // Emit Metal chunk (MaterialTextChunk).
    if (!metalEntries.empty()) {
        container.addChild<MaterialTextChunk>(std::move(metalEntries),
                dictionaryChunk.getDictionary(), ChunkType::MaterialMetal);
    }
#endif

    return true;
}

MaterialBuilder& MaterialBuilder::output(VariableQualifier qualifier, OutputTarget target,
        OutputType type, const char* name, int location) noexcept {

    ASSERT_PRECONDITION(target != OutputTarget::DEPTH || type == OutputType::FLOAT,
            "Depth outputs must be of type FLOAT.");
    ASSERT_PRECONDITION(target != OutputTarget::DEPTH || qualifier == VariableQualifier::OUT,
            "Depth outputs must use OUT qualifier.");

    ASSERT_PRECONDITION(location >= -1,
            "Output location must be >= 0 (or use -1 for default location).");

    // A location value of -1 signals using the default location. We'll simply take the previous
    // output's location and add 1.
    if (location == -1) {
        location = mOutputs.empty() ? 0 : mOutputs.back().location + 1;
    }

    // Unconditionally add this output, then we'll check if we've maxed on on any particular target.
    mOutputs.emplace_back(name, qualifier, target, type, location);

    uint8_t colorOutputCount = 0;
    uint8_t depthOutputCount = 0;
    for (const auto& output : mOutputs) {
        if (output.target == OutputTarget::COLOR) {
            colorOutputCount++;
        }
        if (output.target == OutputTarget::DEPTH) {
            depthOutputCount++;
        }
    }

    ASSERT_PRECONDITION(colorOutputCount <= MAX_COLOR_OUTPUT,
            "A maximum of %d COLOR outputs is allowed.", MAX_COLOR_OUTPUT);
    ASSERT_PRECONDITION(depthOutputCount <= MAX_DEPTH_OUTPUT,
            "A maximum of %d DEPTH output is allowed.", MAX_DEPTH_OUTPUT);

    assert(mOutputs.size() <= MAX_COLOR_OUTPUT + MAX_DEPTH_OUTPUT);

    return *this;
}

MaterialBuilder& MaterialBuilder::enableFramebufferFetch() noexcept {
    // This API is temporary, it is used to enable EXT_framebuffer_fetch for GLSL shaders,
    // this is used sparingly by filament's post-processing stage.
    mEnableFramebufferFetch = true;
    return *this;
}

MaterialBuilder& MaterialBuilder::vertexDomainDeviceJittered(bool enabled) noexcept {
    mVertexDomainDeviceJittered = enabled;
    return *this;
}

MaterialBuilder& MaterialBuilder::useLegacyMorphing() noexcept {
    mUseLegacyMorphing = true;
    return *this;
}

Package MaterialBuilder::build(JobSystem& jobSystem) noexcept {
    bool success;
    if (materialBuilderClients == 0) {
        slog.e << "Error: MaterialBuilder::init() must be called before build()." << io::endl;
        // Return an empty package to signal a failure to build the material.
error:
        return Package::invalidPackage();
    }

    // Add a default color output.
    if (mMaterialDomain == MaterialDomain::POST_PROCESS && mOutputs.empty()) {
        output(VariableQualifier::OUT, OutputTarget::COLOR, OutputType::FLOAT4, "color");
    }

    // Resolve all the #include directives within user code.
    if (!mMaterialFragmentCode.resolveIncludes(mIncludeCallback, mFileName) ||
        !mMaterialVertexCode.resolveIncludes(mIncludeCallback, mFileName)) {
        goto error;
    }

    if (mCustomSurfaceShading && mShading != Shading::LIT) {
        slog.e << "Error: customSurfaceShading can only be used with lit materials." << io::endl;
        goto error;
    }

    // prepareToBuild must be called first, to populate mCodeGenPermutations.
    MaterialInfo info{};
    prepareToBuild(info);

    // check level features
    if (!checkMaterialLevelFeatures(info)) {
        goto error;
    }

    // Run checks, in order.
    // The call to findProperties populates mProperties and must come before runSemanticAnalysis.
    // Return an empty package to signal a failure to build the material.

#ifdef FILAMAT_LITE
    if (!checkLiteRequirements()) {
        goto error;
    }
#endif

    if (!findAllProperties()) {
        goto error;
    }

    if (!runSemanticAnalysis(info)) {
        goto error;
    }

    info.samplerBindings.init(mMaterialDomain, info.sib);

    // Create chunk tree.
    ChunkContainer container;
    writeCommonChunks(container, info);
    if (mMaterialDomain == MaterialDomain::SURFACE) {
        writeSurfaceChunks(container);
    }

    info.useLegacyMorphing = mUseLegacyMorphing;

    // Generate all shaders and write the shader chunks.
    const auto variants = mMaterialDomain == MaterialDomain::SURFACE ?
        determineSurfaceVariants(mVariantFilter, isLit(), mShadowMultiplier) :
        determinePostProcessVariants();
    success = generateShaders(jobSystem, variants, container, info);
    if (!success) {
        // Return an empty package to signal a failure to build the material.
        goto error;
    }

    // Flatten all chunks in the container into a Package.
    Package package(container.getSize());
    Flattener f(package);
    container.flatten(f);

    return package;
}

using namespace backend;
static const char* to_string(ShaderStageFlags stageFlags) noexcept {
    switch (stageFlags) {
        case ShaderStageFlags::NONE:                    return "{ }";
        case ShaderStageFlags::VERTEX:                  return "{ vertex }";
        case ShaderStageFlags::FRAGMENT:                return "{ fragment }";
        case ShaderStageFlags::ALL_SHADER_STAGE_FLAGS:  return "{ vertex | fragment }";
    }
    return nullptr;
}

bool MaterialBuilder::checkMaterialLevelFeatures(MaterialInfo const& info) const noexcept {

    auto logSamplerOverflow = [](SamplerInterfaceBlock const& sib) {
        auto const& samplers = sib.getSamplerInfoList();
        auto const* stage = to_string(sib.getStageFlags());
        for (auto const& sampler: samplers) {
            slog.e << "\"" << sampler.name.c_str() << "\" "
                    << Enums::toString(sampler.type).c_str() << " " << stage << '\n';
        }
        flush(slog.e);
    };

    const auto userSamplerCount = info.sib.getSize();
    switch (info.featureLevel) {
        case FeatureLevel::FEATURE_LEVEL_1: {
            // TODO: we need constants somewhere for these values
            if (userSamplerCount > 9) {
                slog.e << "Error: material \"" << mMaterialName.c_str()
                       << "\" has feature level " << (int)info.featureLevel
                       << " and is using more than 9 samplers." << io::endl;
                logSamplerOverflow(info.sib);
                return false;
            }
            auto const& samplerList = info.sib.getSamplerInfoList();
            using SamplerInfo = SamplerInterfaceBlock::SamplerInfo;
            if (std::any_of(samplerList.begin(), samplerList.end(),
                    [](const SamplerInfo& sampler) {
                        return sampler.type == SamplerType::SAMPLER_CUBEMAP_ARRAY;
                    })) {
                slog.e << "Error: material \"" << mMaterialName.c_str()
                       << "\" has feature level " << (int)info.featureLevel
                       << " and uses a samplerCubemapArray." << io::endl;
                logSamplerOverflow(info.sib);
                return false;
            }
            break;
        }
        case FeatureLevel::FEATURE_LEVEL_2: {
            // TODO: we need constants somewhere for these values
            if (userSamplerCount > 12) {
                slog.e << "Error: material \"" << mMaterialName.c_str()
                       << "\" has feature level " << (int)info.featureLevel
                       << " and is using more than 12 samplers" << io::endl;
                logSamplerOverflow(info.sib);
                return false;
            }
            break;
        }
    }
    return true;
}

bool MaterialBuilder::hasCustomVaryings() const noexcept {
    for (const auto& variable : mVariables) {
        if (!variable.empty()) {
            return true;
        }
    }
    return false;
}

bool MaterialBuilder::needsStandardDepthProgram() const noexcept {
    const bool hasEmptyVertexCode = mMaterialVertexCode.getResolved().empty();
    return !hasEmptyVertexCode ||
           hasCustomVaryings() ||
           mBlendingMode == BlendingMode::MASKED ||
           (mTransparentShadow &&
            (mBlendingMode == BlendingMode::TRANSPARENT ||
             mBlendingMode == BlendingMode::FADE));
}

std::string MaterialBuilder::peek(backend::ShaderType type,
        const CodeGenParams& params, const PropertyList& properties) noexcept {

    ShaderGenerator sg(properties, mVariables, mOutputs, mDefines, mMaterialFragmentCode.getResolved(),
            mMaterialFragmentCode.getLineOffset(), mMaterialVertexCode.getResolved(),
            mMaterialVertexCode.getLineOffset(), mMaterialDomain);

    MaterialInfo info;
    prepareToBuild(info);
    info.samplerBindings.init(mMaterialDomain, info.sib);

    if (type == backend::ShaderType::VERTEX) {
        return sg.createVertexProgram(ShaderModel(params.shaderModel),
                params.targetApi, params.targetLanguage, info, {}, mInterpolation, mVertexDomain);
    } else {
        return sg.createFragmentProgram(ShaderModel(params.shaderModel), params.targetApi,
                params.targetLanguage, info, {}, mInterpolation);
    }
}

void MaterialBuilder::writeCommonChunks(ChunkContainer& container, MaterialInfo& info) const noexcept {
    container.addSimpleChild<uint32_t>(ChunkType::MaterialVersion, MATERIAL_VERSION);
    container.addSimpleChild<uint8_t>(ChunkType::MaterialFeatureLevel, (uint8_t)mFeatureLevel);
    container.addSimpleChild<const char*>(ChunkType::MaterialName, mMaterialName.c_str_safe());
    container.addSimpleChild<uint32_t>(ChunkType::MaterialShaderModels, mShaderModels.getValue());
    container.addSimpleChild<uint8_t>(ChunkType::MaterialDomain, static_cast<uint8_t>(mMaterialDomain));

    // note: this chunk is only needed for OpenGL backends, which don't all support layout(binding=)
    using namespace filament;
    FixedCapacityVector<std::pair<std::string_view, UniformBindingPoints>> list = {
            { PerViewUib::_name,               UniformBindingPoints::PER_VIEW },
            { PerRenderableUib::_name,         UniformBindingPoints::PER_RENDERABLE },
            { LightsUib::_name,                UniformBindingPoints::LIGHTS },
            { ShadowUib::_name,                UniformBindingPoints::SHADOW },
            { FroxelRecordUib::_name,          UniformBindingPoints::FROXEL_RECORDS },
            { PerRenderableBoneUib::_name,     UniformBindingPoints::PER_RENDERABLE_BONES },
            { PerRenderableMorphingUib::_name, UniformBindingPoints::PER_RENDERABLE_MORPHING },
            { info.uib.getName(),              UniformBindingPoints::PER_MATERIAL_INSTANCE }
    };
    container.addChild<MaterialUniformBlockBindingsChunk>(std::move(list));

    // note: this chunk is needed for Vulkan and GL backends. Metal shouldn't need it (but
    // still does as of now).
    container.addChild<MaterialSamplerBlockBindingChunk>(info.samplerBindings);

    // User Material UIB
    container.addChild<MaterialUniformInterfaceBlockChunk>(info.uib);

    // User Material SIB
    container.addChild<MaterialSamplerInterfaceBlockChunk>(info.sib);

    // User Subpass
    container.addChild<MaterialSubpassInterfaceBlockChunk>(info.subpass);


    container.addSimpleChild<bool>(ChunkType::MaterialDoubleSidedSet, mDoubleSidedCapability);
    container.addSimpleChild<bool>(ChunkType::MaterialDoubleSided, mDoubleSided);
    container.addSimpleChild<uint8_t>(ChunkType::MaterialBlendingMode, static_cast<uint8_t>(mBlendingMode));
    container.addSimpleChild<uint8_t>(ChunkType::MaterialTransparencyMode, static_cast<uint8_t>(mTransparencyMode));
    container.addSimpleChild<uint8_t>(ChunkType::MaterialReflectionMode, static_cast<uint8_t>(mReflectionMode));
    container.addSimpleChild<bool>(ChunkType::MaterialDepthWriteSet, mDepthWriteSet);
    container.addSimpleChild<bool>(ChunkType::MaterialColorWrite, mColorWrite);
    container.addSimpleChild<bool>(ChunkType::MaterialDepthWrite, mDepthWrite);
    container.addSimpleChild<bool>(ChunkType::MaterialDepthTest, mDepthTest);
    container.addSimpleChild<bool>(ChunkType::MaterialInstanced, mInstanced);
    container.addSimpleChild<uint8_t>(ChunkType::MaterialCullingMode, static_cast<uint8_t>(mCullingMode));

    uint64_t properties = 0;
    UTILS_NOUNROLL
    for (size_t i = 0; i < MATERIAL_PROPERTIES_COUNT; i++) {
        if (mProperties[i]) {
            properties |= uint64_t(1u) << i;
        }
    }
    container.addSimpleChild<uint64_t>(ChunkType::MaterialProperties, properties);
}

void MaterialBuilder::writeSurfaceChunks(ChunkContainer& container) const noexcept {
    if (mBlendingMode == BlendingMode::MASKED) {
        container.addSimpleChild<float>(ChunkType::MaterialMaskThreshold, mMaskThreshold);
    }

    container.addSimpleChild<uint8_t>(ChunkType::MaterialShading, static_cast<uint8_t>(mShading));

    if (mShading == Shading::UNLIT) {
        container.addSimpleChild<bool>(ChunkType::MaterialShadowMultiplier, mShadowMultiplier);
    }

    container.addSimpleChild<uint8_t>(ChunkType::MaterialRefraction, static_cast<uint8_t>(mRefractionMode));
    container.addSimpleChild<uint8_t>(ChunkType::MaterialRefractionType, static_cast<uint8_t>(mRefractionType));
    container.addSimpleChild<bool>(ChunkType::MaterialClearCoatIorChange, mClearCoatIorChange);
    container.addSimpleChild<uint32_t>(ChunkType::MaterialRequiredAttributes, mRequiredAttributes.getValue());
    container.addSimpleChild<bool>(ChunkType::MaterialSpecularAntiAliasing, mSpecularAntiAliasing);
    container.addSimpleChild<float>(ChunkType::MaterialSpecularAntiAliasingVariance, mSpecularAntiAliasingVariance);
    container.addSimpleChild<float>(ChunkType::MaterialSpecularAntiAliasingThreshold, mSpecularAntiAliasingThreshold);
    container.addSimpleChild<uint8_t>(ChunkType::MaterialVertexDomain, static_cast<uint8_t>(mVertexDomain));
    container.addSimpleChild<uint8_t>(ChunkType::MaterialInterpolation, static_cast<uint8_t>(mInterpolation));
}

} // namespace filamat
