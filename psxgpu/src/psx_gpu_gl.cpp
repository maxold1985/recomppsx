#include "psxgpu/psx_gpu_gl.h"
#include "gl_api.h"

#include <algorithm>
#include <vector>
#include <cstdio>
namespace psxgpu {

namespace {

using namespace glapi;

GLuint compileShader(GLenum type, const char* source, std::string& error)
{
	Api& gl = api();
	GLuint shader = gl.CreateShader(type);
	gl.ShaderSource(shader, 1, &source, nullptr);
	gl.CompileShader(shader);
	GLint ok = 0;
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint length = 0;
		gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		std::vector<char> log(static_cast<size_t>(std::max(length, 1)));
		gl.GetShaderInfoLog(shader, length, nullptr, log.data());
		error = log.data();
		gl.DeleteShader(shader);
		return 0;
	}
	return shader;
}

} // namespace

PsxGpuGlPresenter::PsxGpuGlPresenter() = default;

PsxGpuGlPresenter::~PsxGpuGlPresenter()
{
	shutdown();
}

bool PsxGpuGlPresenter::initialize(const GlProcLoader& loader)
{
	if (m_initialized)
		return true;
	if (!glapi::load(loader, m_lastError))
		return false;
	if (!buildProgram())
		return false;

	auto& gl = glapi::api();
	std::printf(
	"[GL DEBUG] this=%p GetProgramiv=%p CreateProgram=%p LinkProgram=%p\n",
	(void*)this,
	(void*)gl.GetProgramiv,
	(void*)gl.CreateProgram,
	(void*)gl.LinkProgram
	);
	std::fflush(stdout);
	gl.GenVertexArrays(1, &m_vao);
	gl.GenTextures(1, &m_texture);
	gl.BindTexture(glapi::GL_TEXTURE_2D, m_texture);
	gl.TexParameteri(glapi::GL_TEXTURE_2D, glapi::GL_TEXTURE_MIN_FILTER, glapi::GL_NEAREST);
	gl.TexParameteri(glapi::GL_TEXTURE_2D, glapi::GL_TEXTURE_MAG_FILTER, glapi::GL_NEAREST);
	gl.TexParameteri(glapi::GL_TEXTURE_2D, glapi::GL_TEXTURE_WRAP_S, glapi::GL_CLAMP_TO_EDGE);
	gl.TexParameteri(glapi::GL_TEXTURE_2D, glapi::GL_TEXTURE_WRAP_T, glapi::GL_CLAMP_TO_EDGE);
	gl.PixelStorei(glapi::GL_UNPACK_ALIGNMENT, 1);
	m_initialized = true;
	return true;
}

bool PsxGpuGlPresenter::buildProgram()
{
	static const char* vs = R"GLSL(
#version 330 core
out vec2 v_uv;
void main() {
    vec2 p;
    if (gl_VertexID == 0) p = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) p = vec2(3.0, -1.0);
    else p = vec2(-1.0, 3.0);
    v_uv = p * 0.5 + 0.5;
    v_uv.y = 1.0 - v_uv.y;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

	static const char* fs = R"GLSL(
#version 330 core
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_tex;
void main() {
    o_color = texture(u_tex, v_uv);
}
)GLSL";

	auto& gl = glapi::api();
	const glapi::GLuint v = compileShader(glapi::GL_VERTEX_SHADER, vs, m_lastError);
	if (!v) return false;
	const glapi::GLuint f = compileShader(glapi::GL_FRAGMENT_SHADER, fs, m_lastError);
	if (!f) {
		gl.DeleteShader(v);
		
		std::printf(
		"[GL DEBUG] this=%p GetProgramiv=%p CreateProgram=%p LinkProgram=%p\n",
		(void*)this,
		(void*)gl.GetProgramiv,
		(void*)gl.CreateProgram,
		(void*)gl.LinkProgram
		);
		std::fflush(stdout);
		
		return false;
		
	}
	m_program = gl.CreateProgram();
	gl.AttachShader(m_program, v);
	gl.AttachShader(m_program, f);
	gl.LinkProgram(m_program);
	gl.DeleteShader(v);
	gl.DeleteShader(f);
	glapi::GLint ok = 0;
	gl.GetProgramiv(m_program, glapi::GL_LINK_STATUS, &ok);
	if (!ok) {
		glapi::GLint length = 0;
		gl.GetProgramiv(m_program, glapi::GL_INFO_LOG_LENGTH, &length);
		std::vector<char> log(static_cast<size_t>(std::max(length, 1)));
		gl.GetProgramInfoLog(m_program, length, nullptr, log.data());
		m_lastError = log.data();
		gl.DeleteProgram(m_program);
		m_program = 0;
		return false;
	}
	gl.UseProgram(m_program);
	const glapi::GLint sampler = gl.GetUniformLocation(m_program, "u_tex");
	if (sampler >= 0)
		gl.Uniform1i(sampler, 0);
	return true;
}

void PsxGpuGlPresenter::shutdown()
{
	if (!m_initialized)
		return;
	auto& gl = glapi::api();
	if (m_texture) gl.DeleteTextures(1, &m_texture);
	if (m_vao) gl.DeleteVertexArrays(1, &m_vao);
	if (m_program) gl.DeleteProgram(m_program);
	m_texture = 0;
	m_vao = 0;
	m_program = 0;
	m_initialized = false;
}

void PsxGpuGlPresenter::uploadDisplay(const PsxGpu& gpu)
{
	gpu.copyDisplayRgba(m_rgba);
	const int width = gpu.displayWidth();
	const int height = gpu.displayHeight();
	auto& gl = glapi::api();
	gl.ActiveTexture(glapi::GL_TEXTURE0);
	gl.BindTexture(glapi::GL_TEXTURE_2D, m_texture);
	if (width != m_texWidth || height != m_texHeight) {
		m_texWidth = width;
		m_texHeight = height;
		gl.TexImage2D(glapi::GL_TEXTURE_2D, 0, glapi::GL_RGBA8, width, height, 0,
			glapi::GL_RGBA, glapi::GL_UNSIGNED_BYTE, m_rgba.data());
	} else {
		gl.TexSubImage2D(glapi::GL_TEXTURE_2D, 0, 0, 0, width, height,
			glapi::GL_RGBA, glapi::GL_UNSIGNED_BYTE, m_rgba.data());
	}
}

void PsxGpuGlPresenter::present(const PsxGpu& gpu, int framebufferWidth, int framebufferHeight)
{
	if (!m_initialized)
		return;
	auto& gl = glapi::api();
	gl.Viewport(0, 0, framebufferWidth, framebufferHeight);
	gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	gl.Clear(glapi::GL_COLOR_BUFFER_BIT);
	if (gpu.displayDisabled())
		return;
	uploadDisplay(gpu);
	gl.Disable(glapi::GL_BLEND);
	gl.Disable(glapi::GL_DEPTH_TEST);
	gl.Disable(glapi::GL_CULL_FACE);
	gl.UseProgram(m_program);
	gl.BindVertexArray(m_vao);
	gl.ActiveTexture(glapi::GL_TEXTURE0);
	gl.BindTexture(glapi::GL_TEXTURE_2D, m_texture);
	gl.DrawArrays(glapi::GL_TRIANGLES, 0, 3);
}

} // namespace psxgpu
