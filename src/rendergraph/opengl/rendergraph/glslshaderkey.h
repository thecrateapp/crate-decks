#pragma once

#include <rhi/qshader.h>

namespace rendergraph {

/// The GLSL variant to take from a .qsb shader bundle for the current OpenGL
/// context. qt_add_shaders() writes GLSL ES 1.00, GLSL 1.20 and GLSL 1.50 into
/// each bundle. An OpenGL ES context (NVIDIA's EGL on Wayland gives Qt one)
/// rejects desktop GLSL 1.20, and the waveforms would draw nothing.
inline QShaderKey glslShaderKey(bool openGLES) {
    return openGLES
            ? QShaderKey(QShader::GlslShader, QShaderVersion(100, QShaderVersion::GlslEs))
            : QShaderKey(QShader::GlslShader, QShaderVersion(120));
}

} // namespace rendergraph
