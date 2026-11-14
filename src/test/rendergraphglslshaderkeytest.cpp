#include <gtest/gtest.h>
#include <rhi/qshader.h>

#include <QFile>

#include "rendergraph/glslshaderkey.h"

namespace {

QByteArray shaderSource(const QString& resource, bool openGLES) {
    QFile file(resource);
    if (!file.open(QIODeviceBase::ReadOnly)) {
        return QByteArray();
    }
    const QShader bundle = QShader::fromSerialized(file.readAll());
    return bundle.shader(rendergraph::glslShaderKey(openGLES)).shader();
}

// NVIDIA's EGL on Wayland gives Qt an OpenGL ES context. The waveform
// shaders must come out of their .qsb bundles as GLSL ES there: the desktop
// GLSL 1.20 variant does not compile on ES, and nothing is drawn.
TEST(RenderGraphGlslShaderKeyTest, PicksGlslEsForAnOpenGLESContext) {
    const QByteArray source = shaderSource(
            QStringLiteral(":/shaders/rendergraph/rgba.vert.qsb"), true);
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.startsWith("#version 100")) << source.left(40).constData();
}

TEST(RenderGraphGlslShaderKeyTest, KeepsGlsl120ForDesktopOpenGL) {
    const QByteArray source = shaderSource(
            QStringLiteral(":/shaders/rendergraph/rgba.vert.qsb"), false);
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.startsWith("#version 120")) << source.left(40).constData();
}

} // namespace
