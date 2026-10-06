#pragma once

#include "psx_gpu.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace psxgpu {

using GlProcLoader = std::function<void*(const char*)>;

class PsxGpuGlPresenter {
public:
	PsxGpuGlPresenter();
	~PsxGpuGlPresenter();

	bool initialize(const GlProcLoader& loader);
	void shutdown();
	void present(const PsxGpu& gpu, int framebufferWidth, int framebufferHeight);

	const char* lastError() const { return m_lastError.c_str(); }

private:
	bool buildProgram();
	void uploadDisplay(const PsxGpu& gpu);

	uint32_t m_program = 0;
	uint32_t m_vao = 0;
	uint32_t m_texture = 0;
	int m_texWidth = 0;
	int m_texHeight = 0;
	std::vector<uint8_t> m_rgba;
	std::string m_lastError;
	bool m_initialized = false;
};

} // namespace psxgpu
