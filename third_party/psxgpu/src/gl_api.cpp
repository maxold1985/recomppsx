#include "gl_api.h"

#include <string>

namespace psxgpu { namespace glapi {

static Api g_api;

Api& api(){ return g_api; }

static void* get(const GlProcLoader& loader, const char* name, std::string& error)
{
	void* p = loader ? loader(name) : nullptr;
	if (!p && error.empty()) error = std::string("OpenGL function not found: ") + name;
	return p;
}

#define LOAD_GL(member, name) g_api.member = reinterpret_cast<decltype(g_api.member)>(get(loader, name, error)); if (!g_api.member) return false
bool load(const GlProcLoader& loader, std::string& error)
{
	error.clear();
	LOAD_GL(CreateShader,"glCreateShader"); LOAD_GL(ShaderSource,"glShaderSource"); LOAD_GL(CompileShader,"glCompileShader");
	LOAD_GL(GetShaderiv,"glGetShaderiv"); LOAD_GL(GetShaderInfoLog,"glGetShaderInfoLog"); LOAD_GL(DeleteShader,"glDeleteShader");
	LOAD_GL(CreateProgram,"glCreateProgram"); LOAD_GL(AttachShader,"glAttachShader"); LOAD_GL(LinkProgram,"glLinkProgram");
	LOAD_GL(GetProgramiv,"glGetProgramiv"); LOAD_GL(GetProgramInfoLog,"glGetProgramInfoLog"); LOAD_GL(DeleteProgram,"glDeleteProgram");
	LOAD_GL(UseProgram,"glUseProgram"); LOAD_GL(GetUniformLocation,"glGetUniformLocation"); LOAD_GL(Uniform1i,"glUniform1i");
	LOAD_GL(GenVertexArrays,"glGenVertexArrays"); LOAD_GL(BindVertexArray,"glBindVertexArray"); LOAD_GL(DeleteVertexArrays,"glDeleteVertexArrays");
	LOAD_GL(GenTextures,"glGenTextures"); LOAD_GL(BindTexture,"glBindTexture"); LOAD_GL(DeleteTextures,"glDeleteTextures");
	LOAD_GL(TexParameteri,"glTexParameteri"); LOAD_GL(TexImage2D,"glTexImage2D"); LOAD_GL(TexSubImage2D,"glTexSubImage2D");
	LOAD_GL(ActiveTexture,"glActiveTexture"); LOAD_GL(PixelStorei,"glPixelStorei"); LOAD_GL(Viewport,"glViewport");
	LOAD_GL(ClearColor,"glClearColor"); LOAD_GL(Clear,"glClear"); LOAD_GL(Disable,"glDisable"); LOAD_GL(DrawArrays,"glDrawArrays");
	return true;
}
#undef LOAD_GL
} }
