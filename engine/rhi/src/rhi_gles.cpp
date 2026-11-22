// SPDX-License-Identifier: Apache-2.0
// OpenGL ES 3.0 backend of the VICTUS RHI (ADR-0144). See rhi_gles.hpp for the conventions.
#include "rhi_gles.hpp"

#include "rhi_slotmap.hpp"

#include "victus/core/log.hpp"
#include "victus/platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

namespace victus::rhi {

namespace {

using detail::SlotMap;

// ---- Minimal GL ES 3.0 loader ------------------------------------------------------------
// Declared here rather than taken from <GLES3/gl3.h> so the backend builds on every desktop
// toolchain; entry points come from SDL_GL_GetProcAddress.
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::intptr_t;

constexpr GLenum GL_NO_ERROR = 0, GL_FALSE_ = 0, GL_TRUE_ = 1;
constexpr GLenum GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_SAMPLES = 0x8D57;
constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872, GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x8B4C;
constexpr GLenum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_3D = 0x806F, GL_TEXTURE_2D_ARRAY = 0x8C1A, GL_TEXTURE_CUBE_MAP = 0x8513,
                 GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_WRAP_S = 0x2802,
                 GL_TEXTURE_WRAP_T = 0x2803, GL_TEXTURE_WRAP_R = 0x8072, GL_TEXTURE_MAX_LOD = 0x813B,
                 GL_TEXTURE_COMPARE_MODE = 0x884C, GL_TEXTURE_COMPARE_FUNC = 0x884D, GL_COMPARE_REF_TO_TEXTURE = 0x884E,
                 GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;
constexpr GLenum GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
                 GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_MIRRORED_REPEAT = 0x8370;
constexpr GLenum GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203, GL_GREATER = 0x0204,
                 GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_INT = 0x1404, GL_UNSIGNED_INT = 0x1405,
                 GL_FLOAT = 0x1406, GL_HALF_FLOAT = 0x140B, GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368,
                 GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8C3B, GL_UNSIGNED_INT_24_8 = 0x84FA;
constexpr GLenum GL_RED = 0x1903, GL_RG = 0x8227, GL_RGBA = 0x1908, GL_DEPTH_COMPONENT = 0x1902, GL_DEPTH_STENCIL = 0x84F9;
constexpr GLenum GL_RGBA32UI = 0x8D70, GL_RGBA_INTEGER = 0x8D99;
constexpr GLenum GL_R8 = 0x8229, GL_RG8 = 0x822B, GL_RGBA8 = 0x8058, GL_SRGB8_ALPHA8 = 0x8C43, GL_RGBA16F = 0x881A,
                 GL_R16F = 0x822D, GL_RG16F = 0x822F, GL_R32F = 0x822E, GL_RGB10_A2 = 0x8059, GL_R11F_G11F_B10F = 0x8C3A,
                 GL_DEPTH_COMPONENT16 = 0x81A5, GL_DEPTH24_STENCIL8 = 0x88F0, GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1, GL_COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3,
                 GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1 = 0x8C4D, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5 = 0x8C4F,
                 GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8E8C, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM = 0x8E8D,
                 GL_COMPRESSED_RGBA_ASTC_4x4 = 0x93B0, GL_COMPRESSED_RGBA_ASTC_6x6 = 0x93B4,
                 GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 = 0x93D0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6 = 0x93D4;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8A11,
                 GL_COPY_READ_BUFFER = 0x8F36;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8, GL_STREAM_DRAW = 0x88E0;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
                 GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFFu;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
                 GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00,
                 GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                 GL_COLOR = 0x1800, GL_DEPTH = 0x1801;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
constexpr GLenum GL_BLEND = 0x0BE2, GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_SCISSOR_TEST = 0x0C11,
                 GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_FRONT = 0x0404, GL_BACK_FACE = 0x0405, GL_CW = 0x0900, GL_CCW = 0x0901;
constexpr GLenum GL_FUNC_ADD = 0x8006, GL_ZERO = 0, GL_ONE = 1, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
                 GL_DST_COLOR = 0x0306;
constexpr GLenum GL_TRIANGLES = 0x0004, GL_TRIANGLE_STRIP = 0x0005, GL_LINES = 0x0001, GL_LINE_STRIP = 0x0003, GL_POINTS = 0x0000;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5;

#define VICTUS_GL_FUNCS(X)                                                                                                  \
    X(const GLubyte*, GetString, (GLenum))                                                                                  \
    X(const GLubyte*, GetStringi, (GLenum, GLuint))                                                                         \
    X(void, GetIntegerv, (GLenum, GLint*))                                                                                  \
    X(GLenum, GetError, ())                                                                                                 \
    X(void, Enable, (GLenum))                                                                                               \
    X(void, Disable, (GLenum))                                                                                              \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                                     \
    X(void, DepthRangef, (GLfloat, GLfloat))                                                                                \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                                      \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat*))                                                                 \
    X(void, ClearBufferfi, (GLenum, GLint, GLfloat, GLint))                                                                 \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                        \
    X(void, DepthMask, (GLboolean))                                                                                         \
    X(void, DepthFunc, (GLenum))                                                                                            \
    X(void, CullFace, (GLenum))                                                                                             \
    X(void, FrontFace, (GLenum))                                                                                            \
    X(void, PolygonOffset, (GLfloat, GLfloat))                                                                              \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                            \
    X(void, BlendEquationSeparate, (GLenum, GLenum))                                                                        \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                                \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                                       \
    X(void, BindTexture, (GLenum, GLuint))                                                                                  \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                                                      \
    X(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))                                             \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))                    \
    X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*))    \
    X(void, CompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*))         \
    X(void, CompressedTexSubImage3D,                                                                                        \
      (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void*))                        \
    X(void, GenerateMipmap, (GLenum))                                                                                       \
    X(void, ActiveTexture, (GLenum))                                                                                        \
    X(void, GenSamplers, (GLsizei, GLuint*))                                                                                \
    X(void, DeleteSamplers, (GLsizei, const GLuint*))                                                                       \
    X(void, BindSampler, (GLuint, GLuint))                                                                                  \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint))                                                                     \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                                                                   \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                                 \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                                        \
    X(void, BindBuffer, (GLenum, GLuint))                                                                                   \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                                          \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                                     \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))                                                \
    X(void*, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                                                    \
    X(GLboolean, UnmapBuffer, (GLenum))                                                                                     \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                                            \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                                   \
    X(void, BindVertexArray, (GLuint))                                                                                      \
    X(void, EnableVertexAttribArray, (GLuint))                                                                              \
    X(void, DisableVertexAttribArray, (GLuint))                                                                             \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                                  \
    X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*))                                            \
    X(void, VertexAttribDivisor, (GLuint, GLuint))                                                                          \
    X(GLuint, CreateShader, (GLenum))                                                                                       \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                            \
    X(void, CompileShader, (GLuint))                                                                                        \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                                          \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                         \
    X(void, DeleteShader, (GLuint))                                                                                         \
    X(GLuint, CreateProgram, ())                                                                                            \
    X(void, AttachShader, (GLuint, GLuint))                                                                                 \
    X(void, LinkProgram, (GLuint))                                                                                          \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                                         \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                        \
    X(void, DeleteProgram, (GLuint))                                                                                        \
    X(void, UseProgram, (GLuint))                                                                                           \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                                   \
    X(void, Uniform1i, (GLint, GLint))                                                                                      \
    X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar*))                                                                \
    X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))                                                                  \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                                            \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                                   \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                              \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                                  \
    X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint))                                                \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                                      \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                                             \
    X(void, DrawBuffers, (GLsizei, const GLenum*))                                                                          \
    X(void, ReadBuffer, (GLenum))                                                                                           \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))                  \
    X(void, InvalidateFramebuffer, (GLenum, GLsizei, const GLenum*))                                                        \
    X(void, GenRenderbuffers, (GLsizei, GLuint*))                                                                           \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*))                                                                  \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                                                             \
    X(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                                    \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                                            \
    X(void, PixelStorei, (GLenum, GLint))                                                                                   \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                                         \
    X(void, DrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))                                         \
    X(void, Finish, ())                                                                                                     \
    X(void, Flush, ())

// ES 3.2 / OES_draw_buffers_indexed: per-target blend state. Optional.
#define VICTUS_GL_OPTIONAL_FUNCS(X)                                                                                         \
    X(void, Enablei, (GLenum, GLuint))                                                                                      \
    X(void, Disablei, (GLenum, GLuint))                                                                                     \
    X(void, BlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum))

struct Gl {
#define VICTUS_GL_DECLARE(ret, name, args) ret(SDLCALL* name) args = nullptr;
    VICTUS_GL_FUNCS(VICTUS_GL_DECLARE)
    VICTUS_GL_OPTIONAL_FUNCS(VICTUS_GL_DECLARE)
#undef VICTUS_GL_DECLARE

    // Returns the first missing required entry point, or nullptr.
    const char* load() {
#define VICTUS_GL_LOAD(ret, name, args)                                                                                     \
    name = reinterpret_cast<ret(SDLCALL*) args>(SDL_GL_GetProcAddress("gl" #name));                                        \
    if (!name) return "gl" #name;
        VICTUS_GL_FUNCS(VICTUS_GL_LOAD)
#undef VICTUS_GL_LOAD
#define VICTUS_GL_LOAD_OPTIONAL(ret, name, args)                                                                            \
    name = reinterpret_cast<ret(SDLCALL*) args>(SDL_GL_GetProcAddress("gl" #name));                                        \
    if (!name) name = reinterpret_cast<ret(SDLCALL*) args>(SDL_GL_GetProcAddress("gl" #name "OES"));
        VICTUS_GL_OPTIONAL_FUNCS(VICTUS_GL_LOAD_OPTIONAL)
#undef VICTUS_GL_LOAD_OPTIONAL
        return nullptr;
    }
};

// ---- Format tables --------------------------------------------------------------------------

struct GlFormat {
    GLenum internal{0}, format{0}, type{0};
    bool compressed{false};
};

GlFormat gl_format(Format f) {
    switch (f) {
    case Format::RGBA8_UNORM: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::RGBA8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::R8_UNORM: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case Format::RG8_UNORM: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case Format::RGBA16_FLOAT: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    case Format::R16_FLOAT: return {GL_R16F, GL_RED, GL_HALF_FLOAT};
    case Format::RG16_FLOAT: return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
    case Format::R32_FLOAT: return {GL_R32F, GL_RED, GL_FLOAT};
    case Format::RGB10A2_UNORM: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV};
    case Format::R11G11B10_FLOAT: return {GL_R11F_G11F_B10F, 0x1907 /* GL_RGB */, GL_UNSIGNED_INT_10F_11F_11F_REV};
    case Format::D16_UNORM: return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
    case Format::D24_UNORM_S8: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
    case Format::D32_FLOAT: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
    case Format::BC1_RGBA_UNORM: return {GL_COMPRESSED_RGBA_S3TC_DXT1, 0, 0, true};
    case Format::BC1_RGBA_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1, 0, 0, true};
    case Format::BC3_RGBA_UNORM: return {GL_COMPRESSED_RGBA_S3TC_DXT5, 0, 0, true};
    case Format::BC3_RGBA_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5, 0, 0, true};
    case Format::BC7_RGBA_UNORM: return {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0, true};
    case Format::BC7_RGBA_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 0, 0, true};
    case Format::ASTC_4x4_UNORM: return {GL_COMPRESSED_RGBA_ASTC_4x4, 0, 0, true};
    case Format::ASTC_4x4_SRGB: return {GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4, 0, 0, true};
    case Format::ASTC_6x6_UNORM: return {GL_COMPRESSED_RGBA_ASTC_6x6, 0, 0, true};
    case Format::ASTC_6x6_SRGB: return {GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6, 0, 0, true};
    default: return {}; // BGRA8 has no portable ES 3.0 equivalent
    }
}

bool is_float_format(Format f) {
    return f == Format::RGBA16_FLOAT || f == Format::R16_FLOAT || f == Format::RG16_FLOAT || f == Format::R32_FLOAT ||
           f == Format::R11G11B10_FLOAT;
}

GLenum gl_compare(CompareOp c) {
    switch (c) {
    case CompareOp::Never: return GL_NEVER;
    case CompareOp::Less: return GL_LESS;
    case CompareOp::Equal: return GL_EQUAL;
    case CompareOp::LessEqual: return GL_LEQUAL;
    case CompareOp::Greater: return GL_GREATER;
    case CompareOp::NotEqual: return GL_NOTEQUAL;
    case CompareOp::GreaterEqual: return GL_GEQUAL;
    case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_ALWAYS;
}

GLint gl_wrap(AddressMode a) {
    switch (a) {
    case AddressMode::Repeat: return GL_REPEAT;
    case AddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
    case AddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

struct BlendState {
    bool enable{false};
    GLenum src_c{GL_ONE}, dst_c{GL_ZERO}, src_a{GL_ONE}, dst_a{GL_ZERO};
};

BlendState gl_blend(BlendMode m) {
    switch (m) {
    case BlendMode::Opaque: return {};
    case BlendMode::Alpha: return {true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    case BlendMode::PremultipliedAlpha: return {true, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    case BlendMode::Additive: return {true, GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE};
    case BlendMode::Multiply: return {true, GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    }
    return {};
}

struct AttribFormat {
    GLint size;
    GLenum type;
    bool normalized;
    bool integer;
};

AttribFormat gl_attrib(VertexFormat f) {
    switch (f) {
    case VertexFormat::Float: return {1, GL_FLOAT, false, false};
    case VertexFormat::Float2: return {2, GL_FLOAT, false, false};
    case VertexFormat::Float3: return {3, GL_FLOAT, false, false};
    case VertexFormat::Float4: return {4, GL_FLOAT, false, false};
    case VertexFormat::UByte4Norm: return {4, GL_UNSIGNED_BYTE, true, false};
    case VertexFormat::UShort2Norm: return {2, GL_UNSIGNED_SHORT, true, false};
    case VertexFormat::Int4: return {4, GL_INT, false, true};
    case VertexFormat::UInt: return {1, GL_UNSIGNED_INT, false, true};
    case VertexFormat::UShort4: return {4, GL_UNSIGNED_SHORT, false, true};
    }
    return {4, GL_FLOAT, false, false};
}

// IEEE half <-> float for float-target readback (GL ES reads float colour buffers as GL_FLOAT).
std::uint16_t float_to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xFF) - 127 + 15;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const auto shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h; // round half up
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h; // round to nearest
    return static_cast<std::uint16_t>(h);
}

// Packs an unsigned float into an 11- or 10-bit float (5-bit exponent, 6 or 5 mantissa bits).
std::uint32_t float_to_small(float f, int mant_bits) {
    if (!(f > 0.0f)) return 0;
    const std::uint16_t h = float_to_half(f);
    const std::uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    const int drop = 10 - mant_bits;
    std::uint32_t r = (exp << mant_bits) | (mant >> drop);
    if (exp != 31 && ((mant >> (drop - 1)) & 1u)) ++r;
    return std::min<std::uint32_t>(r, (30u << mant_bits) | ((1u << mant_bits) - 1u)); // largest finite value
}

struct TextureEntry {
    GLuint tex{0};
    GLuint renderbuffer{0}; // multisampled colour/depth targets (ES 3.0 has no multisample textures)
    GLenum target{GL_TEXTURE_2D};
    TextureDesc desc;
    std::uint64_t bytes{0};
};
struct BufferEntry {
    GLuint buf{0};
    std::uint32_t size{0};
    std::uint32_t usage{0};
    std::vector<std::uint8_t> shadow; // CPU copy of indirect argument and storage buffers
    // Storage buffers (BufferUsageStorageRead) are mirrored into an RGBA32UI texture of
    // kStorageRowTexels texels per row, 16 bytes per texel: shaders read them with texelFetch.
    GLuint data_tex{0};
    std::uint32_t data_width{0}, data_height{0};
};
struct ShaderEntry {
    GLuint shader{0};
    ShaderStage stage{ShaderStage::Vertex};
    std::vector<std::pair<std::uint32_t, std::string>> samplers, ubos, storage;
    [[nodiscard]] std::uint32_t sampler_units() const {
        std::uint32_t n = 0;
        for (const auto& [slot, name] : samplers) n = std::max(n, slot + 1);
        return n;
    }
};
struct PipelineEntry {
    GLuint program{0};
    GLint base_instance{-1}; // SPIRV_Cross_BaseInstance (gl_InstanceIndex emulation), -1 if unused
    std::array<std::uint32_t, 2> storage_unit{}; // first texture unit of the storage slots (vertex, fragment)
    PipelineDesc desc;
    std::array<BlendState, 8> blend{};
};

std::uint64_t texture_bytes(const TextureDesc& d) {
    std::uint64_t total = 0;
    std::uint32_t w = d.width, h = d.height;
    for (std::uint32_t m = 0; m < std::max(1u, d.mip_levels); ++m) {
        total += format_block(d.format).bytes ? mip_size_bytes(d.format, w, h) : std::uint64_t{w} * h;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    const std::uint64_t layers = d.type == TextureType::Cube ? 6 : d.depth_or_layers;
    return total * layers * d.sample_count;
}

// Texture units: the fragment stage owns units [0, fragment_units) and the vertex stage the next
// vertex_units (GlesBackend::State). Storage buffers are emulated as data textures on the units
// after a stage's samplers, so the PBR variants with surface-shader textures need more than
// OpenGL ES 3.0's minimum of 16 fragment units; drivers that expose more (Mesa, most desktop
// GPUs) get them all.
constexpr std::uint32_t kFragmentUnitBase = 0, kMinUnitsPerStage = 16, kMaxUnitsPerStage = 32;
constexpr std::uint32_t kStorageRowTexels = 1024;
constexpr std::uint32_t kVertexUboBase = 0, kFragmentUboBase = 4, kUniformSlots = 4;
constexpr std::uint32_t kMaxVertexBuffers = 8;
constexpr GLsizeiptr kUniformRing = 4 << 20;

// Parses the "//! sampler <slot> <name>" / "//! ubo <slot> <name>" resource table.
void parse_bindings(std::string_view src, ShaderEntry& e) {
    std::size_t pos = 0;
    while ((pos = src.find("//!", pos)) != std::string_view::npos) {
        const std::size_t end = src.find('\n', pos);
        std::string_view line = src.substr(pos + 3, end == std::string_view::npos ? std::string_view::npos : end - pos - 3);
        pos = end == std::string_view::npos ? src.size() : end;
        auto next = [&line]() {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            std::size_t n = 0;
            while (n < line.size() && line[n] != ' ' && line[n] != '\t' && line[n] != '\r') ++n;
            std::string_view tok = line.substr(0, n);
            line.remove_prefix(n);
            return tok;
        };
        const std::string_view kind = next(), slot = next(), name = next();
        if (name.empty()) continue;
        std::uint32_t s = 0;
        for (char c : slot) {
            if (c < '0' || c > '9') {
                s = ~0u;
                break;
            }
            s = s * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (s == ~0u) continue;
        if (kind == "sampler") e.samplers.emplace_back(s, std::string(name));
        else if (kind == "ubo") e.ubos.emplace_back(s, std::string(name));
        else if (kind == "storage") e.storage.emplace_back(s, std::string(name));
    }
}

} // namespace

struct GlesBackend::State {
    Gl gl;
    SlotMap<TextureEntry> textures;
    SlotMap<BufferEntry> buffers;
    SlotMap<GLuint> samplers;
    SlotMap<ShaderEntry> shaders;
    SlotMap<PipelineEntry> pipelines;
    std::vector<std::string> extensions;
    bool float_targets{false}, half_targets{false}, anisotropy{false}, indexed_blend{false};
    GLint ubo_align{256};
    std::uint32_t fragment_units{kMinUnitsPerStage}, vertex_unit_base{kMinUnitsPerStage}, vertex_units{kMinUnitsPerStage};
    GLuint vao{0}, fbo{0}, read_fbo{0}, ring{0}, storage_sampler{0};
    GLsizeiptr ring_offset{0};
    // Last pushed uniform data per stage (0 vertex, 1 fragment) and slot: re-uploaded when the
    // ring is orphaned so bindings never point at discarded storage.
    std::array<std::array<std::vector<std::uint8_t>, kUniformSlots>, 2> uniforms;
    // Current render pass.
    struct Attachment {
        TextureHandle texture, resolve;
        std::uint32_t mip{0}, layer{0};
        StoreOp store{StoreOp::Store};
    };
    std::vector<Attachment> colors;
    TextureHandle depth;
    StoreOp depth_store{StoreOp::Store};
    std::uint32_t pass_w{0}, pass_h{0};
    bool in_pass{false};
    // Draw state.
    PipelineHandle pipeline;
    struct VertexBinding {
        GLuint buf{0};
        std::uint32_t offset{0};
    };
    std::array<VertexBinding, kMaxVertexBuffers> vbs{};
    GLuint index_buf{0};
    IndexType index_type{IndexType::U16};
    std::uint32_t index_offset{0};
    std::uint32_t enabled_attribs{0};
    // Swapchain emulation: the frame renders into an RGBA8 texture that submit() blits to the window.
    SDL_Window* present_window{nullptr};
    TextureHandle swapchain;
    bool presented{false};
    int swap_interval{1};
};

GlesBackend::GlesBackend(Device& device) : device_(device), s_(std::make_unique<State>()) {}

Result<std::unique_ptr<GlesBackend>> GlesBackend::create(Device& device, const DeviceDesc& desc, DeviceCapabilities& caps) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return make_error(ErrorCode::DeviceError, "GLES device needs the SDL video subsystem");
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (desc.debug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    auto b = std::unique_ptr<GlesBackend>(new GlesBackend(device));
    SDL_Window* w = SDL_CreateWindow("victus-gles", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w) return make_error(ErrorCode::DeviceError, std::format("GLES context window: {}", SDL_GetError()));
    b->window_ = w;
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) return make_error(ErrorCode::DeviceError, std::format("no OpenGL ES 3.0 context: {}", SDL_GetError()));
    b->context_ = ctx;
    auto& s = *b->s_;
    auto& gl = s.gl;
    if (const char* missing = gl.load()) return make_error(ErrorCode::DeviceError, std::format("GLES entry point {} missing", missing));
    const auto* version = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
    const std::string_view ver = version ? version : "";
    if (!ver.starts_with("OpenGL ES ") || ver.size() < 11 || ver[10] < '3')
        return make_error(ErrorCode::Unsupported, std::format("OpenGL ES 3.0 required, got '{}'", ver));
    GLint n = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
        if (const auto* e = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))) s.extensions.emplace_back(reinterpret_cast<const char*>(e));
    const auto has = [&s](std::string_view e) { return std::find(s.extensions.begin(), s.extensions.end(), e) != s.extensions.end(); };
    s.float_targets = has("GL_EXT_color_buffer_float");
    s.half_targets = s.float_targets || has("GL_EXT_color_buffer_half_float");
    s.anisotropy = has("GL_EXT_texture_filter_anisotropic");
    s.indexed_blend = gl.Enablei && gl.Disablei && gl.BlendFuncSeparatei &&
                      (ver[10] > '3' || (ver.size() > 12 && ver[12] >= '2') || has("GL_OES_draw_buffers_indexed") ||
                       has("GL_EXT_draw_buffers_indexed"));
    GLint units = 0, frag_units = 0, vert_units = 0;
    gl.GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &frag_units);
    gl.GetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vert_units);
    s.fragment_units = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::max(frag_units, 0)), kMinUnitsPerStage, kMaxUnitsPerStage);
    s.vertex_unit_base = s.fragment_units;
    const GLint left = std::max(0, units - static_cast<GLint>(s.fragment_units));
    s.vertex_units = static_cast<std::uint32_t>(std::clamp<GLint>(std::min(vert_units, left), 0, kMaxUnitsPerStage));
    if (s.vertex_units < kMinUnitsPerStage)
        VICTUS_LOG_WARN("rhi", "GLES: {} combined texture units; vertex-stage samplers above slot {} are unavailable", units,
                        static_cast<int>(s.vertex_units) - 1);
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s.ubo_align);
    s.ubo_align = std::max<GLint>(s.ubo_align, 16);
    gl.GenVertexArrays(1, &s.vao);
    gl.BindVertexArray(s.vao);
    gl.GenFramebuffers(1, &s.fbo);
    gl.GenFramebuffers(1, &s.read_fbo);
    gl.GenSamplers(1, &s.storage_sampler); // integer data textures must be sampled unfiltered
    gl.SamplerParameteri(s.storage_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.SamplerParameteri(s.storage_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.GenBuffers(1, &s.ring);
    gl.BindBuffer(GL_UNIFORM_BUFFER, s.ring);
    gl.BufferData(GL_UNIFORM_BUFFER, kUniformRing, nullptr, GL_STREAM_DRAW);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    caps.backend = Backend::GLES;
    const auto* renderer = reinterpret_cast<const char*>(gl.GetString(GL_RENDERER));
    caps.driver_name = std::format("gles ({}, {})", renderer ? renderer : "unknown", ver);
    caps.bc_compression = has("GL_EXT_texture_compression_bptc") && has("GL_EXT_texture_compression_s3tc");
    caps.astc_compression = has("GL_KHR_texture_compression_astc_ldr");
    caps.anisotropy = s.anisotropy;
    caps.depth_clamp = false;
    caps.compute = false;
    caps.shader_float16 = false;
    GLint max_tex = 0, max_samples = 1;
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    gl.GetIntegerv(GL_MAX_SAMPLES, &max_samples);
    caps.max_texture_size = static_cast<std::uint32_t>(max_tex);
    caps.max_msaa = max_samples >= 8 ? 8 : max_samples >= 4 ? 4 : max_samples >= 2 ? 2 : 1;
    if (!s.half_targets) caps.max_msaa = std::min<std::uint32_t>(caps.max_msaa, 4);
    VICTUS_LOG_INFO("rhi", "GPU device: {} msaa<={} bc={} astc={} float_targets={} indexed_blend={}", caps.driver_name,
                    caps.max_msaa, caps.bc_compression, caps.astc_compression, s.float_targets, s.indexed_blend);
    return b;
}

GlesBackend::~GlesBackend() {
    if (context_) {
        make_current();
        auto& s = *s_;
        auto& gl = s.gl;
        if (gl.Finish) {
            gl.Finish();
            for (auto& e : s.pipelines.entries)
                if (e.alive) gl.DeleteProgram(e.value.program);
            for (auto& e : s.shaders.entries)
                if (e.alive) gl.DeleteShader(e.value.shader);
            for (auto& e : s.samplers.entries)
                if (e.alive) gl.DeleteSamplers(1, &e.value);
            for (auto& e : s.buffers.entries)
                if (e.alive) {
                    gl.DeleteBuffers(1, &e.value.buf);
                    if (e.value.data_tex) gl.DeleteTextures(1, &e.value.data_tex);
                }
            for (auto& e : s.textures.entries) {
                if (!e.alive) continue;
                if (e.value.tex) gl.DeleteTextures(1, &e.value.tex);
                if (e.value.renderbuffer) gl.DeleteRenderbuffers(1, &e.value.renderbuffer);
            }
            gl.DeleteBuffers(1, &s.ring);
            gl.DeleteSamplers(1, &s.storage_sampler);
            gl.DeleteFramebuffers(1, &s.fbo);
            gl.DeleteFramebuffers(1, &s.read_fbo);
            gl.DeleteVertexArrays(1, &s.vao);
        }
        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_), nullptr);
        SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
    }
    if (window_) SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
}

void GlesBackend::make_current() const {
    if (SDL_GL_GetCurrentContext() == static_cast<SDL_GLContext>(context_)) return;
    SDL_Window* w = s_ && s_->present_window ? s_->present_window : static_cast<SDL_Window*>(window_);
    SDL_GL_MakeCurrent(w, static_cast<SDL_GLContext>(context_));
}

Status GlesBackend::attach_window(platform::Window& window, PresentMode mode) {
    auto* w = static_cast<SDL_Window*>(window.native());
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_OPENGL))
        return make_error(ErrorCode::Unsupported, "GLES presentation needs a window created with OpenGL support");
    if (!SDL_GL_MakeCurrent(w, static_cast<SDL_GLContext>(context_)))
        return make_error(ErrorCode::DeviceError, std::format("GLES make current failed: {}", SDL_GetError()));
    s_->present_window = w;
    s_->swap_interval = mode == PresentMode::Immediate ? 0 : mode == PresentMode::Mailbox ? -1 : 1;
    if (!SDL_GL_SetSwapInterval(s_->swap_interval) && s_->swap_interval < 0) SDL_GL_SetSwapInterval(1);
    return {};
}

void GlesBackend::detach_window(platform::Window& window) {
    make_current();
    if (s_->present_window != window.native()) return;
    s_->gl.Finish();
    s_->present_window = nullptr;
    SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_), static_cast<SDL_GLContext>(context_));
}

bool GlesBackend::supports_format(Format format, std::uint32_t usage) const {
    const GlFormat g = gl_format(format);
    if (!g.internal) return false;
    const auto has = [this](std::string_view e) { return std::find(s_->extensions.begin(), s_->extensions.end(), e) != s_->extensions.end(); };
    if (g.compressed) {
        if (usage & (TextureUsageColorTarget | TextureUsageDepthTarget)) return false;
        if (format == Format::BC7_RGBA_SRGB || format == Format::BC7_RGBA_UNORM) return has("GL_EXT_texture_compression_bptc");
        if (format == Format::BC1_RGBA_SRGB || format == Format::BC3_RGBA_SRGB) return has("GL_EXT_texture_compression_s3tc_srgb");
        if (format == Format::BC1_RGBA_UNORM || format == Format::BC3_RGBA_UNORM) return has("GL_EXT_texture_compression_s3tc");
        return has("GL_KHR_texture_compression_astc_ldr");
    }
    if (usage & (TextureUsageStorageRead | TextureUsageStorageWrite)) return false;
    if (usage & TextureUsageColorTarget) {
        if (is_depth(format)) return false;
        if (format == Format::RGBA16_FLOAT || format == Format::R16_FLOAT || format == Format::RG16_FLOAT) return s_->half_targets;
        if (format == Format::R32_FLOAT || format == Format::R11G11B10_FLOAT) return s_->float_targets;
    }
    if ((usage & TextureUsageDepthTarget) && !is_depth(format)) return false;
    return true;
}

Result<TextureHandle> GlesBackend::create_texture(const TextureDesc& desc) {
    make_current();
    if (desc.width == 0 || desc.height == 0) return make_error(ErrorCode::InvalidArgument, "texture dimensions must be non-zero");
    const GlFormat g = gl_format(desc.format);
    if (!g.internal) return make_error(ErrorCode::Unsupported, std::format("texture '{}': format unsupported on GLES", desc.name));
    if ((desc.usage & TextureUsageColorTarget) && !supports_format(desc.format, TextureUsageColorTarget))
        return make_error(ErrorCode::Unsupported, std::format("texture '{}': format is not colour-renderable on this GLES device", desc.name));
    auto& gl = s_->gl;
    TextureEntry e;
    e.desc = desc;
    e.desc.mip_levels = std::max(1u, desc.mip_levels);
    e.bytes = texture_bytes(e.desc);
    if (desc.sample_count > 1) {
        if (desc.type != TextureType::Tex2D) return make_error(ErrorCode::Unsupported, "multisampled GLES textures must be 2D");
        gl.GenRenderbuffers(1, &e.renderbuffer);
        gl.BindRenderbuffer(GL_RENDERBUFFER, e.renderbuffer);
        gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, static_cast<GLsizei>(desc.sample_count), g.internal,
                                          static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
    } else {
        e.target = desc.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP
                   : desc.type == TextureType::Tex2DArray ? GL_TEXTURE_2D_ARRAY
                   : desc.type == TextureType::Tex3D ? GL_TEXTURE_3D
                                                     : GL_TEXTURE_2D;
        gl.GenTextures(1, &e.tex);
        gl.BindTexture(e.target, e.tex);
        const auto levels = static_cast<GLsizei>(e.desc.mip_levels);
        if (e.target == GL_TEXTURE_2D || e.target == GL_TEXTURE_CUBE_MAP)
            gl.TexStorage2D(e.target, levels, g.internal, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
        else
            gl.TexStorage3D(e.target, levels, g.internal, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
                            static_cast<GLsizei>(std::max(1u, desc.depth_or_layers)));
    }
    if (const GLenum err = gl.GetError(); err != GL_NO_ERROR) {
        if (e.tex) gl.DeleteTextures(1, &e.tex);
        if (e.renderbuffer) gl.DeleteRenderbuffers(1, &e.renderbuffer);
        return make_error(ErrorCode::DeviceError, std::format("create texture '{}' failed (GL error 0x{:x})", desc.name, err));
    }
    auto& stats = device_.stats_;
    stats.texture_bytes += e.bytes;
    auto h = s_->textures.insert<TextureHandle>(std::move(e));
    stats.live_textures = s_->textures.live;
    return h;
}

const TextureDesc* GlesBackend::describe(TextureHandle h) const {
    auto* e = s_->textures.get(h);
    return e ? &e->desc : nullptr;
}

Result<BufferHandle> GlesBackend::create_buffer(const BufferDesc& desc) {
    make_current();
    if (desc.size == 0) return make_error(ErrorCode::InvalidArgument, "buffer size must be non-zero");
    auto& gl = s_->gl;
    BufferEntry e;
    e.size = desc.size;
    e.usage = desc.usage;
    gl.GenBuffers(1, &e.buf);
    // Index buffers may only ever be bound to GL_ELEMENT_ARRAY_BUFFER under WebGL-style
    // validation, so they are created through that target.
    const GLenum target = (desc.usage & BufferUsageIndex) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    gl.BindVertexArray(0);
    gl.BindBuffer(target, e.buf);
    gl.BufferData(target, desc.size, nullptr, GL_DYNAMIC_DRAW);
    gl.BindVertexArray(s_->vao);
    if (desc.usage & BufferUsageIndirect) e.shadow.assign(desc.size, 0);
    if ((desc.usage & BufferUsageStorageRead) && desc.size > 0) {
        const std::uint32_t texels = (desc.size + 15) / 16;
        e.data_width = std::min(texels, kStorageRowTexels);
        e.data_height = (texels + kStorageRowTexels - 1) / kStorageRowTexels;
        e.shadow.assign(std::size_t{e.data_width} * e.data_height * 16, 0);
        gl.GenTextures(1, &e.data_tex);
        gl.BindTexture(GL_TEXTURE_2D, e.data_tex);
        gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, static_cast<GLsizei>(e.data_width), static_cast<GLsizei>(e.data_height));
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }
    if (const GLenum err = gl.GetError(); err != GL_NO_ERROR) {
        gl.DeleteBuffers(1, &e.buf);
        if (e.data_tex) gl.DeleteTextures(1, &e.data_tex);
        return make_error(ErrorCode::DeviceError, std::format("create buffer '{}' failed (GL error 0x{:x})", desc.name, err));
    }
    device_.stats_.buffer_bytes += desc.size;
    auto h = s_->buffers.insert<BufferHandle>(std::move(e));
    device_.stats_.live_buffers = s_->buffers.live;
    return h;
}

Result<SamplerHandle> GlesBackend::create_sampler(const SamplerDesc& desc) {
    make_current();
    auto& gl = s_->gl;
    GLuint smp = 0;
    gl.GenSamplers(1, &smp);
    const bool lin_min = desc.min_filter == Filter::Linear, lin_mip = desc.mip_filter == Filter::Linear;
    const GLint min = lin_min ? (lin_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                              : (lin_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    gl.SamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, min);
    gl.SamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, desc.mag_filter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(smp, GL_TEXTURE_WRAP_S, gl_wrap(desc.address_u));
    gl.SamplerParameteri(smp, GL_TEXTURE_WRAP_T, gl_wrap(desc.address_v));
    gl.SamplerParameteri(smp, GL_TEXTURE_WRAP_R, gl_wrap(desc.address_w));
    gl.SamplerParameterf(smp, GL_TEXTURE_MAX_LOD, 1000.0f);
    if (desc.max_anisotropy > 1.0f && s_->anisotropy) gl.SamplerParameterf(smp, GL_TEXTURE_MAX_ANISOTROPY, desc.max_anisotropy);
    if (desc.compare) {
        gl.SamplerParameteri(smp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl.SamplerParameteri(smp, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(gl_compare(desc.compare_op)));
    }
    auto h = s_->samplers.insert<SamplerHandle>(smp);
    device_.stats_.live_samplers = s_->samplers.live;
    return h;
}

Result<ShaderHandle> GlesBackend::create_shader(const ShaderDesc& desc) {
    make_current();
    if (desc.stage == ShaderStage::Compute) return make_error(ErrorCode::Unsupported, "GLES has no compute shaders");
    if (desc.glsl_es.empty())
        return make_error(ErrorCode::Unsupported, std::format("shader '{}' has no GLSL ES source for the GLES backend", desc.name));
    if (desc.storage_textures)
        return make_error(ErrorCode::Unsupported, std::format("shader '{}' uses storage textures, which GLES 3.0 lacks", desc.name));
    auto& gl = s_->gl;
    ShaderEntry e;
    e.stage = desc.stage;
    parse_bindings(desc.glsl_es, e);
    if (e.storage.size() < desc.storage_buffers)
        return make_error(ErrorCode::Unsupported,
                          std::format("shader '{}': {} storage buffers but only {} \"//! storage\" slots in its GLSL ES", desc.name,
                                      desc.storage_buffers, e.storage.size()));
    e.shader = gl.CreateShader(desc.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    const GLchar* src = desc.glsl_es.data();
    const auto len = static_cast<GLint>(desc.glsl_es.size());
    gl.ShaderSource(e.shader, 1, &src, &len);
    gl.CompileShader(e.shader);
    GLint ok = 0;
    gl.GetShaderiv(e.shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        gl.GetShaderiv(e.shader, GL_INFO_LOG_LENGTH, &n);
        std::string log(static_cast<std::size_t>(std::max(n, 1)), '\0');
        gl.GetShaderInfoLog(e.shader, n, nullptr, log.data());
        gl.DeleteShader(e.shader);
        return make_error(ErrorCode::DeviceError, std::format("compile shader '{}' failed: {}", desc.name, log.c_str()));
    }
    auto h = s_->shaders.insert<ShaderHandle>(std::move(e));
    device_.stats_.live_shaders = s_->shaders.live;
    return h;
}

Result<PipelineHandle> GlesBackend::create_pipeline(const PipelineDesc& desc) {
    make_current();
    auto* vs = s_->shaders.get(desc.vertex);
    auto* fs = s_->shaders.get(desc.fragment);
    if (!vs || !fs) return make_error(ErrorCode::InvalidArgument, std::format("pipeline '{}' references invalid shaders", desc.name));
    if (desc.color_targets.size() > 8) return make_error(ErrorCode::InvalidArgument, "at most 8 colour targets");
    auto& gl = s_->gl;
    PipelineEntry p;
    p.desc = desc;
    p.program = gl.CreateProgram();
    gl.AttachShader(p.program, vs->shader);
    gl.AttachShader(p.program, fs->shader);
    gl.LinkProgram(p.program);
    GLint ok = 0;
    gl.GetProgramiv(p.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        gl.GetProgramiv(p.program, GL_INFO_LOG_LENGTH, &n);
        std::string log(static_cast<std::size_t>(std::max(n, 1)), '\0');
        gl.GetProgramInfoLog(p.program, n, nullptr, log.data());
        gl.DeleteProgram(p.program);
        return make_error(ErrorCode::DeviceError, std::format("link pipeline '{}' failed: {}", desc.name, log.c_str()));
    }
    gl.UseProgram(p.program);
    const auto bind = [&](const ShaderEntry& sh, std::uint32_t unit_base, std::uint32_t ubo_base) {
        for (const auto& [slot, name] : sh.samplers)
            if (const GLint loc = gl.GetUniformLocation(p.program, name.c_str()); loc >= 0)
                gl.Uniform1i(loc, static_cast<GLint>(unit_base + slot));
        for (const auto& [slot, name] : sh.ubos)
            if (const GLuint idx = gl.GetUniformBlockIndex(p.program, name.c_str()); idx != GL_INVALID_INDEX)
                gl.UniformBlockBinding(p.program, idx, ubo_base + slot);
    };
    bind(*vs, s_->vertex_unit_base, kVertexUboBase);
    // Storage slots take the texture units after the stage's samplers.
    const auto bind_storage = [&](const ShaderEntry& sh, std::uint32_t unit_base, std::uint32_t stage) -> bool {
        p.storage_unit[stage] = unit_base + sh.sampler_units();
        const std::uint32_t limit = stage == 0 ? s_->vertex_units : s_->fragment_units;
        for (const auto& [slot, name] : sh.storage) {
            if (sh.sampler_units() + slot >= limit) return false;
            if (const GLint loc = gl.GetUniformLocation(p.program, name.c_str()); loc >= 0)
                gl.Uniform1i(loc, static_cast<GLint>(p.storage_unit[stage] + slot));
        }
        return true;
    };
    if (!bind_storage(*vs, s_->vertex_unit_base, 0) || !bind_storage(*fs, kFragmentUnitBase, 1)) {
        gl.DeleteProgram(p.program);
        return make_error(ErrorCode::Unsupported,
                          std::format("pipeline '{}': more samplers and storage buffers than the device's {} fragment / {} vertex texture units",
                                      desc.name, s_->fragment_units, s_->vertex_units));
    }
    p.base_instance = gl.GetUniformLocation(p.program, "SPIRV_Cross_BaseInstance");
    bind(*fs, kFragmentUnitBase, kFragmentUboBase);
    for (std::size_t i = 0; i < desc.color_targets.size(); ++i) p.blend[i] = gl_blend(desc.color_targets[i].blend);
    if (s_->pipeline.valid()) {
        if (auto* cur = s_->pipelines.get(s_->pipeline)) gl.UseProgram(cur->program);
    } else {
        gl.UseProgram(0);
    }
    if (const GLenum err = gl.GetError(); err != GL_NO_ERROR)
        VICTUS_LOG_WARN("rhi", "pipeline '{}': GL error 0x{:x} while binding resources", desc.name, err);
    auto h = s_->pipelines.insert<PipelineHandle>(std::move(p));
    device_.stats_.live_pipelines = s_->pipelines.live;
    return h;
}

void GlesBackend::destroy(TextureHandle h) {
    make_current();
    TextureEntry e;
    if (!s_->textures.remove(h, e)) return;
    if (e.tex) s_->gl.DeleteTextures(1, &e.tex);
    if (e.renderbuffer) s_->gl.DeleteRenderbuffers(1, &e.renderbuffer);
    device_.stats_.texture_bytes -= e.bytes;
    device_.stats_.live_textures = s_->textures.live;
    if (h == s_->swapchain) s_->swapchain = {};
}
void GlesBackend::destroy(BufferHandle h) {
    make_current();
    BufferEntry e;
    if (!s_->buffers.remove(h, e)) return;
    s_->gl.DeleteBuffers(1, &e.buf);
    if (e.data_tex) s_->gl.DeleteTextures(1, &e.data_tex);
    for (auto& vb : s_->vbs)
        if (vb.buf == e.buf) vb = {};
    if (s_->index_buf == e.buf) s_->index_buf = 0;
    device_.stats_.buffer_bytes -= e.size;
    device_.stats_.live_buffers = s_->buffers.live;
}
void GlesBackend::destroy(SamplerHandle h) {
    make_current();
    GLuint smp = 0;
    if (!s_->samplers.remove(h, smp)) return;
    s_->gl.DeleteSamplers(1, &smp);
    device_.stats_.live_samplers = s_->samplers.live;
}
void GlesBackend::destroy(ShaderHandle h) {
    make_current();
    ShaderEntry e;
    if (!s_->shaders.remove(h, e)) return;
    s_->gl.DeleteShader(e.shader); // flagged; freed once no program references it
    device_.stats_.live_shaders = s_->shaders.live;
}
void GlesBackend::destroy(PipelineHandle h) {
    make_current();
    PipelineEntry e;
    if (!s_->pipelines.remove(h, e)) return;
    if (s_->pipeline == h) {
        s_->pipeline = {};
        s_->gl.UseProgram(0);
    }
    s_->gl.DeleteProgram(e.program);
    device_.stats_.live_pipelines = s_->pipelines.live;
}

void GlesBackend::wait_idle() { s_->gl.Finish(); }

namespace {

// Attaches mip 0 of `layer` of a texture to the read framebuffer's colour attachment 0.
void attach_read(Gl& gl, const TextureEntry& e, std::uint32_t layer, std::uint32_t mip) {
    if (e.renderbuffer) {
        gl.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, e.renderbuffer);
    } else if (e.target == GL_TEXTURE_2D) {
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.tex, static_cast<GLint>(mip));
    } else if (e.target == GL_TEXTURE_CUBE_MAP) {
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, e.tex,
                                static_cast<GLint>(mip));
    } else {
        gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, e.tex, static_cast<GLint>(mip), static_cast<GLint>(layer));
    }
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
}

} // namespace

Result<std::vector<std::uint8_t>> GlesBackend::read_texture(TextureHandle h) {
    make_current();
    auto* e = s_->textures.get(h);
    if (!e) return make_error(ErrorCode::InvalidArgument, "invalid texture handle");
    const Format f = e->desc.format;
    const std::uint32_t bpp = bytes_per_pixel(f);
    if (bpp == 0) return make_error(ErrorCode::Unsupported, "readback of compressed formats is unsupported");
    if (is_depth(f)) return make_error(ErrorCode::Unsupported, "GLES cannot read back depth textures");
    if (e->renderbuffer) return make_error(ErrorCode::Unsupported, "readback of multisampled targets is unsupported");
    auto& gl = s_->gl;
    const std::uint32_t w = e->desc.width, hgt = e->desc.height;
    const std::uint32_t slices = e->desc.type == TextureType::Tex3D ? std::max(1u, e->desc.depth_or_layers) : 1u;
    const std::size_t texels = std::size_t{w} * hgt;
    std::vector<std::uint8_t> out(texels * slices * bpp);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s_->read_fbo);
    std::vector<float> fbuf;
    std::vector<std::uint8_t> bbuf;
    for (std::uint32_t z = 0; z < slices; ++z) {
        attach_read(gl, *e, z, 0);
        if (gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            return make_error(ErrorCode::Unsupported, std::format("texture '{}' cannot be read back on GLES", e->desc.name));
        }
        std::uint8_t* dst = out.data() + texels * bpp * z;
        if (f == Format::RGBA8_UNORM || f == Format::RGBA8_SRGB) {
            gl.ReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(hgt), GL_RGBA, GL_UNSIGNED_BYTE, dst);
        } else if (f == Format::RGB10A2_UNORM) {
            gl.ReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(hgt), GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, dst);
        } else if (f == Format::R8_UNORM || f == Format::RG8_UNORM) {
            // Normalized fixed-point buffers are always readable as RGBA/UNSIGNED_BYTE.
            bbuf.resize(texels * 4);
            gl.ReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(hgt), GL_RGBA, GL_UNSIGNED_BYTE, bbuf.data());
            for (std::size_t i = 0; i < texels; ++i)
                for (std::uint32_t c = 0; c < bpp; ++c) dst[i * bpp + c] = bbuf[i * 4 + c];
        } else if (is_float_format(f)) {
            fbuf.resize(texels * 4);
            gl.ReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(hgt), GL_RGBA, GL_FLOAT, fbuf.data());
            for (std::size_t i = 0; i < texels; ++i) {
                const float* px = &fbuf[i * 4];
                if (f == Format::R32_FLOAT) {
                    std::memcpy(dst + i * 4, px, 4);
                } else if (f == Format::R11G11B10_FLOAT) {
                    const std::uint32_t v = float_to_small(px[0], 6) | (float_to_small(px[1], 6) << 11) | (float_to_small(px[2], 5) << 22);
                    std::memcpy(dst + i * 4, &v, 4);
                } else {
                    const std::uint32_t comps = f == Format::RGBA16_FLOAT ? 4 : f == Format::RG16_FLOAT ? 2 : 1;
                    for (std::uint32_t c = 0; c < comps; ++c) {
                        const std::uint16_t hv = float_to_half(px[c]);
                        std::memcpy(dst + i * bpp + c * 2, &hv, 2);
                    }
                }
            }
        } else {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            return make_error(ErrorCode::Unsupported, "readback format unsupported on GLES");
        }
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (const GLenum err = gl.GetError(); err != GL_NO_ERROR)
        return make_error(ErrorCode::DeviceError, std::format("read texture '{}' failed (GL error 0x{:x})", e->desc.name, err));
    return out;
}

Result<std::vector<std::uint8_t>> GlesBackend::read_buffer(BufferHandle h, std::uint32_t offset, std::uint32_t size) {
    make_current();
    auto* e = s_->buffers.get(h);
    if (!e) return make_error(ErrorCode::InvalidArgument, "invalid buffer handle");
    if (offset > e->size) return make_error(ErrorCode::InvalidArgument, "read_buffer offset out of range");
    if (size == 0) size = e->size - offset;
    if (size > e->size - offset) return make_error(ErrorCode::InvalidArgument, "read_buffer range out of bounds");
    auto& gl = s_->gl;
    gl.BindBuffer(GL_COPY_READ_BUFFER, e->buf);
    const void* mapped = gl.MapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
    if (!mapped) return make_error(ErrorCode::DeviceError, "map buffer for readback failed");
    std::vector<std::uint8_t> out(size);
    std::memcpy(out.data(), mapped, size);
    gl.UnmapBuffer(GL_COPY_READ_BUFFER);
    return out;
}

void GlesBackend::begin_commands() { s_->presented = false; }

void GlesBackend::push_uniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    make_current();
    if (stage == ShaderStage::Compute || slot >= kUniformSlots || size == 0) return;
    auto& s = *s_;
    auto& gl = s.gl;
    const int st = stage == ShaderStage::Vertex ? 0 : 1;
    auto& copy = s.uniforms[static_cast<std::size_t>(st)][slot];
    // Bound ranges are padded to 16 bytes; reads past the pushed size see zeros.
    copy.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    copy.resize((size + 15u) & ~15u, 0);
    const auto write = [&](int stg, std::uint32_t sl) {
        const auto& bytes = s.uniforms[static_cast<std::size_t>(stg)][sl];
        if (bytes.empty()) return;
        const auto n = static_cast<GLsizeiptr>(bytes.size());
        gl.BufferSubData(GL_UNIFORM_BUFFER, s.ring_offset, n, bytes.data());
        gl.BindBufferRange(GL_UNIFORM_BUFFER, (stg == 0 ? kVertexUboBase : kFragmentUboBase) + sl, s.ring, s.ring_offset, n);
        s.ring_offset = (s.ring_offset + n + s.ubo_align - 1) / s.ubo_align * s.ubo_align;
    };
    gl.BindBuffer(GL_UNIFORM_BUFFER, s.ring);
    if (s.ring_offset + static_cast<GLsizeiptr>(copy.size()) > kUniformRing) {
        // Orphan the ring and re-upload every live slot so earlier bindings stay valid.
        gl.BufferData(GL_UNIFORM_BUFFER, kUniformRing, nullptr, GL_STREAM_DRAW);
        s.ring_offset = 0;
        for (int stg = 0; stg < 2; ++stg)
            for (std::uint32_t sl = 0; sl < kUniformSlots; ++sl)
                if (!(stg == st && sl == slot)) write(stg, sl);
    }
    write(st, slot);
}

void GlesBackend::upload_buffer(BufferHandle buffer, std::uint32_t offset, std::span<const std::uint8_t> data, bool discard) {
    make_current();
    auto* e = s_->buffers.get(buffer);
    if (!e || offset + data.size() > e->size) {
        VICTUS_LOG_ERROR("rhi", "upload_buffer out of range or invalid handle");
        return;
    }
    auto& gl = s_->gl;
    const GLenum target = (e->usage & BufferUsageIndex) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    gl.BindVertexArray(0);
    gl.BindBuffer(target, e->buf);
    // Discard: orphan the storage so the driver need not wait for draws still reading it.
    if (discard && offset == 0) gl.BufferData(target, e->size, nullptr, GL_DYNAMIC_DRAW);
    gl.BufferSubData(target, offset, static_cast<GLsizeiptr>(data.size()), data.data());
    gl.BindVertexArray(s_->vao);
    if (!e->shadow.empty()) std::memcpy(e->shadow.data() + offset, data.data(), data.size());
    if (e->data_tex) {
        // Re-upload the whole rows the write touched (the shadow is padded to full rows).
        const std::uint32_t row_bytes = e->data_width * 16;
        const std::uint32_t first = offset / row_bytes;
        const std::uint32_t last = (offset + static_cast<std::uint32_t>(data.size()) - 1) / row_bytes;
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.BindTexture(GL_TEXTURE_2D, e->data_tex);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(first), static_cast<GLsizei>(e->data_width),
                         static_cast<GLsizei>(last - first + 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                         e->shadow.data() + std::size_t{first} * row_bytes);
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }
    device_.stats_.uploads++;
    device_.stats_.upload_bytes += data.size();
}

void GlesBackend::upload_texture(TextureHandle texture, std::span<const std::uint8_t> pixels, std::uint32_t mip, std::uint32_t layer) {
    make_current();
    auto* e = s_->textures.get(texture);
    if (!e || !e->tex) {
        VICTUS_LOG_ERROR("rhi", "upload_texture: invalid handle");
        return;
    }
    const TextureDesc& d = e->desc;
    if (mip >= d.mip_levels) {
        VICTUS_LOG_ERROR("rhi", "upload_texture '{}': mip {} out of range", d.name, mip);
        return;
    }
    const bool volume = d.type == TextureType::Tex3D;
    const std::uint32_t layers = d.type == TextureType::Cube ? 6u : volume ? 1u : std::max(1u, d.depth_or_layers);
    if (layer >= layers) {
        VICTUS_LOG_ERROR("rhi", "upload_texture '{}': layer {} out of range", d.name, layer);
        return;
    }
    const std::uint32_t w = std::max(1u, d.width >> mip), h = std::max(1u, d.height >> mip);
    const std::uint32_t depth = volume ? std::max(1u, std::max(1u, d.depth_or_layers) >> mip) : 1u;
    if (const std::uint32_t bpp = bytes_per_pixel(d.format); bpp > 0 && pixels.size() < std::size_t{w} * h * depth * bpp) {
        VICTUS_LOG_ERROR("rhi", "upload_texture '{}': {} bytes for a {}x{}x{} region", d.name, pixels.size(), w, h, depth);
        return;
    }
    if (pixels.empty()) return;
    auto& gl = s_->gl;
    const GlFormat g = gl_format(d.format);
    gl.BindTexture(e->target, e->tex);
    const auto gm = static_cast<GLint>(mip);
    const auto gw = static_cast<GLsizei>(w), gh = static_cast<GLsizei>(h);
    if (e->target == GL_TEXTURE_2D || e->target == GL_TEXTURE_CUBE_MAP) {
        const GLenum t = e->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer : GL_TEXTURE_2D;
        if (g.compressed) gl.CompressedTexSubImage2D(t, gm, 0, 0, gw, gh, g.internal, static_cast<GLsizei>(pixels.size()), pixels.data());
        else gl.TexSubImage2D(t, gm, 0, 0, gw, gh, g.format, g.type, pixels.data());
    } else {
        const auto z = static_cast<GLint>(volume ? 0 : layer);
        const auto gd = static_cast<GLsizei>(depth);
        if (g.compressed)
            gl.CompressedTexSubImage3D(e->target, gm, 0, 0, z, gw, gh, gd, g.internal, static_cast<GLsizei>(pixels.size()), pixels.data());
        else gl.TexSubImage3D(e->target, gm, 0, 0, z, gw, gh, gd, g.format, g.type, pixels.data());
    }
    if (const GLenum err = gl.GetError(); err != GL_NO_ERROR)
        VICTUS_LOG_ERROR("rhi", "upload_texture '{}': GL error 0x{:x}", d.name, err);
    device_.stats_.uploads++;
    device_.stats_.upload_bytes += pixels.size();
}

void GlesBackend::generate_mipmaps(TextureHandle texture) {
    make_current();
    auto* e = s_->textures.get(texture);
    if (!e || !e->tex || e->desc.mip_levels <= 1) return;
    s_->gl.BindTexture(e->target, e->tex);
    s_->gl.GenerateMipmap(e->target);
}

TextureHandle GlesBackend::acquire_swapchain(platform::Window& window, std::uint32_t& width, std::uint32_t& height) {
    make_current();
    width = height = 0;
    auto* w = static_cast<SDL_Window*>(window.native());
    if (w != s_->present_window) return {};
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) return {};
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    if (pw <= 0 || ph <= 0) return {};
    const auto* cur = s_->textures.get(s_->swapchain);
    if (!cur || cur->desc.width != static_cast<std::uint32_t>(pw) || cur->desc.height != static_cast<std::uint32_t>(ph)) {
        destroy(s_->swapchain);
        TextureDesc d;
        d.name = "swapchain";
        d.width = static_cast<std::uint32_t>(pw);
        d.height = static_cast<std::uint32_t>(ph);
        d.format = Format::RGBA8_UNORM;
        d.usage = TextureUsageColorTarget | TextureUsageSampled;
        auto t = create_texture(d);
        if (!t) return {};
        s_->swapchain = t.value();
    }
    width = static_cast<std::uint32_t>(pw);
    height = static_cast<std::uint32_t>(ph);
    s_->presented = true;
    return s_->swapchain;
}

bool GlesBackend::begin_render_pass(const RenderPassDesc& desc) {
    make_current();
    auto& s = *s_;
    auto& gl = s.gl;
    s.colors.clear();
    s.depth = {};
    s.pass_w = s.pass_h = 0;
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
    std::array<GLenum, 8> draw_buffers{};
    const std::size_t count = std::min<std::size_t>(desc.colors.size(), 8);
    for (std::size_t i = 0; i < 8; ++i) {
        const GLenum att = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        if (i >= count) {
            gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
            continue;
        }
        const auto& c = desc.colors[i];
        auto* e = s.textures.get(c.texture);
        if (!e) {
            VICTUS_LOG_ERROR("rhi", "render pass color attachment {} invalid", i);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            return false;
        }
        if (e->renderbuffer) gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, e->renderbuffer);
        else if (e->target == GL_TEXTURE_2D) gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, e->tex, static_cast<GLint>(c.mip));
        else if (e->target == GL_TEXTURE_CUBE_MAP)
            gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_CUBE_MAP_POSITIVE_X + c.layer, e->tex, static_cast<GLint>(c.mip));
        else gl.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, att, e->tex, static_cast<GLint>(c.mip), static_cast<GLint>(c.layer));
        draw_buffers[i] = att;
        s.colors.push_back({c.texture, c.resolve, c.mip, c.layer, c.store});
        if (s.pass_w == 0) {
            s.pass_w = std::max(1u, e->desc.width >> c.mip);
            s.pass_h = std::max(1u, e->desc.height >> c.mip);
        }
    }
    gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    if (auto* d = s.textures.get(desc.depth.texture)) {
        const GLenum att = d->desc.format == Format::D24_UNORM_S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        if (d->renderbuffer) gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, d->renderbuffer);
        else if (d->target == GL_TEXTURE_2D) gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, d->tex, 0);
        else gl.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, att, d->tex, 0, static_cast<GLint>(desc.depth.layer));
        s.depth = desc.depth.texture;
        s.depth_store = desc.depth.store;
        if (s.pass_w == 0) {
            s.pass_w = d->desc.width;
            s.pass_h = d->desc.height;
        }
    }
    gl.DrawBuffers(static_cast<GLsizei>(count), draw_buffers.data());
    if (gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        VICTUS_LOG_ERROR("rhi", "GLES render pass framebuffer incomplete");
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        return false;
    }
    // Clears ignore the pipeline's masks and scissor; the next bind_pipeline restores them.
    gl.Disable(GL_SCISSOR_TEST);
    std::array<GLenum, 9> invalidate{};
    GLsizei n_invalidate = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& c = desc.colors[i];
        if (c.load == LoadOp::Clear) {
            gl.ColorMask(GL_TRUE_, GL_TRUE_, GL_TRUE_, GL_TRUE_);
            const std::array<GLfloat, 4> col{c.clear.r, c.clear.g, c.clear.b, c.clear.a};
            gl.ClearBufferfv(GL_COLOR, static_cast<GLint>(i), col.data());
        } else if (c.load == LoadOp::DontCare) {
            invalidate[static_cast<std::size_t>(n_invalidate++)] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        }
    }
    if (s.depth.valid()) {
        if (desc.depth.load == LoadOp::Clear) {
            gl.DepthMask(GL_TRUE_);
            if (s.textures.get(s.depth)->desc.format == Format::D24_UNORM_S8) gl.ClearBufferfi(GL_DEPTH_STENCIL, 0, desc.depth.clear_depth, 0);
            else gl.ClearBufferfv(GL_DEPTH, 0, &desc.depth.clear_depth);
        } else if (desc.depth.load == LoadOp::DontCare) {
            invalidate[static_cast<std::size_t>(n_invalidate++)] = GL_DEPTH_ATTACHMENT;
        }
    }
    if (n_invalidate) gl.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, n_invalidate, invalidate.data());
    gl.Viewport(0, 0, static_cast<GLsizei>(s.pass_w), static_cast<GLsizei>(s.pass_h));
    gl.DepthRangef(0.0f, 1.0f);
    s.pipeline = {};
    s.in_pass = true;
    return true;
}

void GlesBackend::end_render_pass() {
    auto& s = *s_;
    if (!s.in_pass) return;
    auto& gl = s.gl;
    std::array<GLenum, 9> invalidate{};
    GLsizei n_invalidate = 0;
    // Resolve multisampled attachments into their resolve textures.
    for (std::size_t i = 0; i < s.colors.size(); ++i) {
        const auto& c = s.colors[i];
        auto* src = s.textures.get(c.texture);
        auto* dst = s.textures.get(c.resolve);
        if (src && dst) {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
            gl.ReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.read_fbo);
            gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->tex, 0);
            const GLenum db = GL_COLOR_ATTACHMENT0;
            gl.DrawBuffers(1, &db);
            gl.Disable(GL_SCISSOR_TEST);
            const auto w = static_cast<GLint>(s.pass_w), h = static_cast<GLint>(s.pass_h);
            gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
        }
        if (c.store == StoreOp::DontCare) invalidate[static_cast<std::size_t>(n_invalidate++)] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
    }
    if (s.depth.valid() && s.depth_store == StoreOp::DontCare) invalidate[static_cast<std::size_t>(n_invalidate++)] = GL_DEPTH_ATTACHMENT;
    if (n_invalidate) gl.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, n_invalidate, invalidate.data());
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    s.in_pass = false;
    s.colors.clear();
    s.depth = {};
}

void GlesBackend::blit(TextureHandle src, TextureHandle dst, Filter filter) {
    make_current();
    auto& s = *s_;
    auto* a = s.textures.get(src);
    auto* b = s.textures.get(dst);
    if (!a || !b || is_depth(a->desc.format) || b->renderbuffer) return;
    auto& gl = s.gl;
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fbo);
    attach_read(gl, *a, 0, 0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
    for (GLenum i = 1; i < 8; ++i) gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, 0);
    gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    if (b->target == GL_TEXTURE_2D) gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->tex, 0);
    else if (b->target == GL_TEXTURE_CUBE_MAP)
        gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, b->tex, 0);
    else gl.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, b->tex, 0, 0);
    const GLenum db = GL_COLOR_ATTACHMENT0;
    gl.DrawBuffers(1, &db);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE_, GL_TRUE_, GL_TRUE_, GL_TRUE_);
    // Multisampled sources resolve; scaled blits of integer-free colour formats may filter.
    const bool scaled = a->desc.width != b->desc.width || a->desc.height != b->desc.height;
    gl.BlitFramebuffer(0, 0, static_cast<GLint>(a->desc.width), static_cast<GLint>(a->desc.height), 0, 0,
                       static_cast<GLint>(b->desc.width), static_cast<GLint>(b->desc.height), GL_COLOR_BUFFER_BIT,
                       scaled && filter == Filter::Linear && !a->renderbuffer ? GL_LINEAR : GL_NEAREST);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    s.pipeline = {};
}

void GlesBackend::submit(bool wait) {
    make_current();
    auto& s = *s_;
    auto& gl = s.gl;
    if (s.presented && s.present_window) {
        if (auto* sc = s.textures.get(s.swapchain)) {
            // The frame is stored top row first; the default framebuffer's row 0 is the bottom.
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fbo);
            attach_read(gl, *sc, 0, 0);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            gl.Disable(GL_SCISSOR_TEST);
            gl.ColorMask(GL_TRUE_, GL_TRUE_, GL_TRUE_, GL_TRUE_);
            const auto w = static_cast<GLint>(sc->desc.width), h = static_cast<GLint>(sc->desc.height);
            gl.BlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            SDL_GL_SwapWindow(s.present_window);
        }
    }
    s.presented = false;
    s.pipeline = {};
    if (wait) gl.Finish();
    else gl.Flush();
}

// ---- Draw state ----------------------------------------------------------------------------

void GlesBackend::bind_pipeline(PipelineHandle pipeline) {
    auto& s = *s_;
    auto* p = s.pipelines.get(pipeline);
    if (!p) return;
    auto& gl = s.gl;
    const PipelineDesc& d = p->desc;
    gl.UseProgram(p->program);
    if (d.cull == CullMode::None) {
        gl.Disable(GL_CULL_FACE);
    } else {
        gl.Enable(GL_CULL_FACE);
        gl.CullFace(d.cull == CullMode::Front ? GL_FRONT : GL_BACK_FACE);
    }
    // The vertex shaders flip y, which mirrors the winding: swap the front face to compensate.
    gl.FrontFace(d.front_face == FrontFace::CounterClockwise ? GL_CW : GL_CCW);
    if (d.depth_bias_constant != 0.0f || d.depth_bias_slope != 0.0f) {
        gl.Enable(GL_POLYGON_OFFSET_FILL);
        gl.PolygonOffset(d.depth_bias_slope, d.depth_bias_constant);
    } else {
        gl.Disable(GL_POLYGON_OFFSET_FILL);
    }
    if (d.depth_test || d.depth_write) {
        // GL skips depth writes when the test is disabled, so "write without test" uses ALWAYS.
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(d.depth_test ? gl_compare(d.depth_compare) : GL_ALWAYS);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }
    if (d.alpha_to_coverage && d.sample_count > 1) gl.Enable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    else gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    gl.DepthMask(d.depth_write ? GL_TRUE_ : GL_FALSE_);
    gl.ColorMask(GL_TRUE_, GL_TRUE_, GL_TRUE_, GL_TRUE_);
    const std::size_t n = d.color_targets.size();
    if (s.indexed_blend && n > 1) {
        for (std::size_t i = 0; i < n; ++i) {
            const BlendState& b = p->blend[i];
            const auto idx = static_cast<GLuint>(i);
            if (b.enable) {
                gl.Enablei(GL_BLEND, idx);
                gl.BlendFuncSeparatei(idx, b.src_c, b.dst_c, b.src_a, b.dst_a);
            } else {
                gl.Disablei(GL_BLEND, idx);
            }
        }
    } else {
        // ES 3.0 shares one blend state between targets: use the first blended target's.
        BlendState b{};
        for (std::size_t i = 0; i < n; ++i)
            if (p->blend[i].enable) {
                b = p->blend[i];
                break;
            }
        if (b.enable) {
            gl.Enable(GL_BLEND);
            gl.BlendFuncSeparate(b.src_c, b.dst_c, b.src_a, b.dst_a);
        } else {
            gl.Disable(GL_BLEND);
        }
    }
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl.Disable(GL_SCISSOR_TEST);
    s.pipeline = pipeline;
}

void GlesBackend::set_viewport(const Viewport& vp) {
    auto& gl = s_->gl;
    // Rendering is y-flipped, so the RHI's top-left origin is GL's row offset as is.
    gl.Viewport(static_cast<GLint>(vp.x), static_cast<GLint>(vp.y), static_cast<GLsizei>(vp.width), static_cast<GLsizei>(vp.height));
    gl.DepthRangef(vp.min_depth, vp.max_depth);
}

void GlesBackend::set_scissor(const Rect& r) {
    auto& gl = s_->gl;
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(r.x, r.y, r.width, r.height);
}

void GlesBackend::bind_vertex_buffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t offset) {
    if (slot >= kMaxVertexBuffers) return;
    if (auto* b = s_->buffers.get(buffer)) s_->vbs[slot] = {b->buf, offset};
}

void GlesBackend::bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset) {
    if (auto* b = s_->buffers.get(buffer)) {
        s_->index_buf = b->buf;
        s_->index_type = type;
        s_->index_offset = offset;
    }
}

void GlesBackend::bind_texture(ShaderStage stage, std::uint32_t slot, TextureHandle texture, SamplerHandle sampler) {
    auto* t = s_->textures.get(texture);
    auto* smp = s_->samplers.get(sampler);
    if (!t || !smp || !t->tex) {
        VICTUS_LOG_ERROR("rhi", "bind_texture: invalid texture or sampler handle (slot {})", slot);
        return;
    }
    auto& gl = s_->gl;
    const std::uint32_t unit = (stage == ShaderStage::Vertex ? s_->vertex_unit_base : kFragmentUnitBase) + slot;
    gl.ActiveTexture(GL_TEXTURE0 + unit);
    gl.BindTexture(t->target, t->tex);
    gl.BindSampler(unit, *smp);
    gl.ActiveTexture(GL_TEXTURE0);
}

void GlesBackend::bind_storage_buffer(ShaderStage stage, std::uint32_t slot, BufferHandle buffer) {
    auto* b = s_->buffers.get(buffer);
    auto* p = s_->pipelines.get(s_->pipeline);
    if (!b || !b->data_tex || !p) {
        VICTUS_LOG_ERROR("rhi", "bind_storage_buffer: needs a bound pipeline and a BufferUsageStorageRead buffer (slot {})", slot);
        return;
    }
    auto& gl = s_->gl;
    const std::uint32_t unit = p->storage_unit[stage == ShaderStage::Vertex ? 0 : 1] + slot;
    gl.ActiveTexture(GL_TEXTURE0 + unit);
    gl.BindTexture(GL_TEXTURE_2D, b->data_tex);
    gl.BindSampler(unit, s_->storage_sampler);
    gl.ActiveTexture(GL_TEXTURE0);
}

namespace {

// Points every attribute of the pipeline at its bound buffer. ES 3.0 has neither base vertex
// nor base instance, so both are applied as byte offsets on the per-vertex / per-instance
// streams.
void apply_vertex_state(Gl& gl, GlesBackend::State& s, const PipelineDesc& d, std::int32_t base_vertex, std::uint32_t base_instance,
                        std::uint32_t& enabled) {
    std::uint32_t want = 0;
    for (const auto& a : d.attributes) {
        if (a.location >= 32) continue;
        const VertexBufferLayout* layout = nullptr;
        for (const auto& vb : d.vertex_buffers)
            if (vb.slot == a.buffer_slot) layout = &vb;
        if (!layout || a.buffer_slot >= s.vbs.size()) continue;
        const auto& bind = s.vbs[a.buffer_slot];
        if (!bind.buf) continue;
        const std::int64_t base = layout->per_instance ? std::int64_t{base_instance} : std::int64_t{base_vertex};
        const std::int64_t off = std::int64_t{bind.offset} + a.offset + base * layout->stride;
        if (off < 0) continue;
        const AttribFormat f = gl_attrib(a.format);
        gl.BindBuffer(GL_ARRAY_BUFFER, bind.buf);
        // NOLINTNEXTLINE(performance-no-int-to-ptr): GL takes buffer offsets as pointers
        const void* ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(off));
        if (f.integer) gl.VertexAttribIPointer(a.location, f.size, f.type, static_cast<GLsizei>(layout->stride), ptr);
        else gl.VertexAttribPointer(a.location, f.size, f.type, f.normalized ? GL_TRUE_ : GL_FALSE_, static_cast<GLsizei>(layout->stride), ptr);
        gl.VertexAttribDivisor(a.location, layout->per_instance ? 1 : 0);
        want |= 1u << a.location;
    }
    for (std::uint32_t i = 0; i < 32; ++i) {
        const std::uint32_t bit = 1u << i;
        if ((want & bit) && !(enabled & bit)) gl.EnableVertexAttribArray(i);
        if (!(want & bit) && (enabled & bit)) gl.DisableVertexAttribArray(i);
    }
    enabled = want;
}

GLenum gl_primitive(Primitive p) {
    switch (p) {
    case Primitive::TriangleList: return GL_TRIANGLES;
    case Primitive::TriangleStrip: return GL_TRIANGLE_STRIP;
    case Primitive::LineList: return GL_LINES;
    case Primitive::LineStrip: return GL_LINE_STRIP;
    case Primitive::PointList: return GL_POINTS;
    }
    return GL_TRIANGLES;
}

} // namespace

void GlesBackend::draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) {
    auto& s = *s_;
    auto* p = s.pipelines.get(s.pipeline);
    if (!p || !s.in_pass || vertex_count == 0 || instance_count == 0) return;
    apply_vertex_state(s.gl, s, p->desc, 0, first_instance, s.enabled_attribs);
    if (p->base_instance >= 0) s.gl.Uniform1i(p->base_instance, static_cast<GLint>(first_instance));
    s.gl.DrawArraysInstanced(gl_primitive(p->desc.primitive), static_cast<GLint>(first_vertex), static_cast<GLsizei>(vertex_count),
                             static_cast<GLsizei>(instance_count));
    device_.stats_.draw_calls++;
}

void GlesBackend::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                               std::int32_t vertex_offset, std::uint32_t first_instance) {
    auto& s = *s_;
    auto* p = s.pipelines.get(s.pipeline);
    if (!p || !s.in_pass || !s.index_buf || index_count == 0 || instance_count == 0) return;
    apply_vertex_state(s.gl, s, p->desc, vertex_offset, first_instance, s.enabled_attribs);
    if (p->base_instance >= 0) s.gl.Uniform1i(p->base_instance, static_cast<GLint>(first_instance));
    s.gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.index_buf);
    const std::uint32_t isize = s.index_type == IndexType::U16 ? 2 : 4;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): GL takes buffer offsets as pointers
    const void* ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(s.index_offset + std::uint64_t{first_index} * isize));
    s.gl.DrawElementsInstanced(gl_primitive(p->desc.primitive), static_cast<GLsizei>(index_count),
                               s.index_type == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, ptr,
                               static_cast<GLsizei>(instance_count));
    device_.stats_.draw_calls++;
}

void GlesBackend::draw_indexed_indirect(BufferHandle buffer, std::uint32_t offset, std::uint32_t draw_count) {
    auto* b = s_->buffers.get(buffer);
    if (!b || b->shadow.empty() || draw_count == 0) return;
    for (std::uint32_t i = 0; i < draw_count; ++i) {
        const std::size_t at = offset + std::size_t{i} * sizeof(IndexedIndirectCommand);
        if (at + sizeof(IndexedIndirectCommand) > b->shadow.size()) break;
        IndexedIndirectCommand c;
        std::memcpy(&c, b->shadow.data() + at, sizeof c);
        draw_indexed(c.index_count, c.instance_count, c.first_index, c.vertex_offset, c.first_instance);
    }
    device_.stats_.indirect_draws += draw_count;
}

} // namespace victus::rhi
