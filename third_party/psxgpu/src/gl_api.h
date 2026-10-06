#pragma once

#include "psxgpu/psx_gpu_gl.h"

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * OpenGL no Windows usa APIENTRY.
 *
 * Em Win32/i686:
 *     APIENTRY = __stdcall
 *
 * Sem isso, os ponteiros carregados por glfwGetProcAddress()
 * podem ser chamados usando a calling convention errada,
 * corrompendo ESP/stack.
 */
#if defined(_WIN32)

	#if defined(__GNUC__)
		#define PSXGPU_GL_APIENTRY __attribute__((stdcall))
	#else
		#define PSXGPU_GL_APIENTRY __stdcall
	#endif

#else

	#define PSXGPU_GL_APIENTRY

#endif


namespace psxgpu {
namespace glapi {


using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLchar = char;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLsizeiptr = std::ptrdiff_t;


/* ============================================================
 * OpenGL constants
 * ============================================================ */

const GLenum GL_VERTEX_SHADER = 0x8B31;
const GLenum GL_FRAGMENT_SHADER = 0x8B30;

const GLenum GL_COMPILE_STATUS = 0x8B81;
const GLenum GL_LINK_STATUS = 0x8B82;
const GLenum GL_INFO_LOG_LENGTH = 0x8B84;

const GLenum GL_TEXTURE_2D = 0x0DE1;
const GLenum GL_TEXTURE0 = 0x84C0;

const GLenum GL_RGBA = 0x1908;
const GLenum GL_RGBA8 = 0x8058;
const GLenum GL_UNSIGNED_BYTE = 0x1401;

const GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
const GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
const GLenum GL_TEXTURE_WRAP_S = 0x2802;
const GLenum GL_TEXTURE_WRAP_T = 0x2803;

const GLenum GL_NEAREST = 0x2600;
const GLenum GL_CLAMP_TO_EDGE = 0x812F;

const GLenum GL_TRIANGLES = 0x0004;

const GLenum GL_COLOR_BUFFER_BIT = 0x00004000;

const GLenum GL_BLEND = 0x0BE2;
const GLenum GL_DEPTH_TEST = 0x0B71;
const GLenum GL_CULL_FACE = 0x0B44;

const GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;


/* ============================================================
 * OpenGL function pointer types
 *
 * IMPORTANTE:
 * todos usam PSXGPU_GL_APIENTRY.
 * ============================================================ */

typedef GLuint
(PSXGPU_GL_APIENTRY *PFN_CreateShader)(
	GLenum type
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_ShaderSource)(
	GLuint shader,
	GLsizei count,
	const GLchar* const* string,
	const GLint* length
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_CompileShader)(
	GLuint shader
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GetShaderiv)(
	GLuint shader,
	GLenum pname,
	GLint* params
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GetShaderInfoLog)(
	GLuint shader,
	GLsizei maxLength,
	GLsizei* length,
	GLchar* infoLog
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_DeleteShader)(
	GLuint shader
);


typedef GLuint
(PSXGPU_GL_APIENTRY *PFN_CreateProgram)(
	void
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_AttachShader)(
	GLuint program,
	GLuint shader
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_LinkProgram)(
	GLuint program
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GetProgramiv)(
	GLuint program,
	GLenum pname,
	GLint* params
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GetProgramInfoLog)(
	GLuint program,
	GLsizei maxLength,
	GLsizei* length,
	GLchar* infoLog
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_DeleteProgram)(
	GLuint program
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_UseProgram)(
	GLuint program
);


typedef GLint
(PSXGPU_GL_APIENTRY *PFN_GetUniformLocation)(
	GLuint program,
	const GLchar* name
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_Uniform1i)(
	GLint location,
	GLint v0
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GenVertexArrays)(
	GLsizei n,
	GLuint* arrays
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_BindVertexArray)(
	GLuint array
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_DeleteVertexArrays)(
	GLsizei n,
	const GLuint* arrays
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_GenTextures)(
	GLsizei n,
	GLuint* textures
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_BindTexture)(
	GLenum target,
	GLuint texture
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_DeleteTextures)(
	GLsizei n,
	const GLuint* textures
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_TexParameteri)(
	GLenum target,
	GLenum pname,
	GLint param
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_TexImage2D)(
	GLenum target,
	GLint level,
	GLint internalFormat,
	GLsizei width,
	GLsizei height,
	GLint border,
	GLenum format,
	GLenum type,
	const void* pixels
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_TexSubImage2D)(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLsizei width,
	GLsizei height,
	GLenum format,
	GLenum type,
	const void* pixels
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_ActiveTexture)(
	GLenum texture
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_PixelStorei)(
	GLenum pname,
	GLint param
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_Viewport)(
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_ClearColor)(
	GLfloat red,
	GLfloat green,
	GLfloat blue,
	GLfloat alpha
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_Clear)(
	GLbitfield mask
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_Disable)(
	GLenum cap
);


typedef void
(PSXGPU_GL_APIENTRY *PFN_DrawArrays)(
	GLenum mode,
	GLint first,
	GLsizei count
);


/* ============================================================
 * OpenGL API table
 * ============================================================ */

struct Api
{
	PFN_CreateShader CreateShader = nullptr;
	PFN_ShaderSource ShaderSource = nullptr;
	PFN_CompileShader CompileShader = nullptr;

	PFN_GetShaderiv GetShaderiv = nullptr;
	PFN_GetShaderInfoLog GetShaderInfoLog = nullptr;

	PFN_DeleteShader DeleteShader = nullptr;


	PFN_CreateProgram CreateProgram = nullptr;
	PFN_AttachShader AttachShader = nullptr;
	PFN_LinkProgram LinkProgram = nullptr;

	PFN_GetProgramiv GetProgramiv = nullptr;
	PFN_GetProgramInfoLog GetProgramInfoLog = nullptr;

	PFN_DeleteProgram DeleteProgram = nullptr;
	PFN_UseProgram UseProgram = nullptr;


	PFN_GetUniformLocation GetUniformLocation = nullptr;
	PFN_Uniform1i Uniform1i = nullptr;


	PFN_GenVertexArrays GenVertexArrays = nullptr;
	PFN_BindVertexArray BindVertexArray = nullptr;
	PFN_DeleteVertexArrays DeleteVertexArrays = nullptr;


	PFN_GenTextures GenTextures = nullptr;
	PFN_BindTexture BindTexture = nullptr;
	PFN_DeleteTextures DeleteTextures = nullptr;


	PFN_TexParameteri TexParameteri = nullptr;

	PFN_TexImage2D TexImage2D = nullptr;
	PFN_TexSubImage2D TexSubImage2D = nullptr;


	PFN_ActiveTexture ActiveTexture = nullptr;

	PFN_PixelStorei PixelStorei = nullptr;


	PFN_Viewport Viewport = nullptr;

	PFN_ClearColor ClearColor = nullptr;
	PFN_Clear Clear = nullptr;

	PFN_Disable Disable = nullptr;

	PFN_DrawArrays DrawArrays = nullptr;
};


/* ============================================================
 * API
 * ============================================================ */

Api& api();

bool load(
	const GlProcLoader& loader,
	std::string& error
);


} // namespace glapi
} // namespace psxgpu


#undef PSXGPU_GL_APIENTRY