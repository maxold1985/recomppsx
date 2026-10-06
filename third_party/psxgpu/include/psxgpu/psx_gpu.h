#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace psxgpu {

class PsxGpu {
public:
	static const int VramWidth = 1024;
	static const int VramHeight = 512;
	using Read32 = std::function<uint32_t(uint32_t)>;
	using Write32 = std::function<void(uint32_t, uint32_t)>;
	bool m_videoField;
	PsxGpu();
	void vblank();
	void reset();
	void writeGp0(uint32_t value);
	void writeGp1(uint32_t value);
	uint32_t readData();
	void setVideoField(bool field);
	uint32_t readStatus() const;
	void dmaLinkedList(uint32_t startAddress, const Read32& read32, uint32_t maxNodes = 1u << 20);
	void dmaBlockToGpu(uint32_t startAddress, uint32_t wordCount, const Read32& read32);
	void dmaGpuToRam(uint32_t startAddress, uint32_t wordCount, const Write32& write32);
	void dmaOtc(uint32_t startAddress, uint32_t wordCount, const Write32& write32);
	const uint16_t* vramData() const { return m_vram.data(); }
	uint16_t* vramData() { return m_vram.data(); }
	int displayX() const { return m_displayX; }
	int displayY() const { return m_displayY; }
	int displayWidth() const;
	int displayHeight() const;
	bool displayDisabled() const { return m_displayDisabled; }
	bool display24Bit() const { return (m_displayMode & (1u << 4)) != 0; }
	void copyDisplayRgba(std::vector<uint8_t>& rgba) const;
	uint32_t drawMode() const { return m_drawMode; }
	uint32_t textureWindow() const { return m_textureWindow; }
private:
	struct Vertex { Vertex() : x(0.0f), y(0.0f), u(0.0f), v(0.0f), r(255.0f), g(255.0f), b(255.0f) {} float x,y,u,v,r,g,b; };
	struct TextureState { TextureState() : clut(0), tpage(0), textured(false), rawTexture(false), semiTransparent(false) {} uint16_t clut,tpage; bool textured,rawTexture,semiTransparent; };
	void startGp0Packet(uint32_t firstWord);
	void executeGp0Packet();
	int fixedPacketWords(uint8_t command) const;
	bool isPolylineTerminator(uint32_t value) const;
	void commandFillRectangle();
	void commandPolygon();
	void commandLine();
	void commandRectangle();
	void commandVramCopy();
	void commandCpuToVramHeader();
	void commandVramToCpuHeader();
	void commandEnvironment(uint8_t command, uint32_t word);
	void drawTriangle(const Vertex& a,const Vertex& b,const Vertex& c,const TextureState& tex);
	void drawLineSegment(const Vertex& a,const Vertex& b,bool semiTransparent,uint16_t tpage);
	void drawRectanglePrimitive(const Vertex& origin,int width,int height,const TextureState& tex);
	uint16_t sampleTexture(int u,int v,uint16_t clut,uint16_t tpage) const;
	void applyTextureWindow(int& u,int& v) const;
	void writePixel(int x,int y,uint16_t foreground,bool semiTransparent,uint16_t tpage,bool texturedStp);
	uint16_t blendPixel(uint16_t back,uint16_t front,uint16_t tpage) const;
	uint16_t color24To555(uint32_t color,int x,int y,bool dither) const;
	uint16_t modulateTexture(uint16_t texel,float r,float g,float b,int x,int y) const;
	uint16_t& vramAt(int x,int y);
	const uint16_t& vramAt(int x,int y) const;
	static int wrapX(int x); static int wrapY(int y); static int16_t sign11(uint32_t value); static uint8_t clamp8(int value);
	std::vector<uint16_t> m_vram; std::vector<uint32_t> m_packet;
	int m_expectedWords=0; bool m_polylineMode=false;
	bool m_cpuToVram=false,m_vramToCpu=false; int m_transferX=0,m_transferY=0,m_transferW=0,m_transferH=0,m_transferPixelsDone=0,m_transferPixelsTotal=0;
	uint32_t m_drawMode=0,m_textureWindow=0; int m_drawAreaX1=0,m_drawAreaY1=0,m_drawAreaX2=VramWidth-1,m_drawAreaY2=VramHeight-1,m_drawOffsetX=0,m_drawOffsetY=0;
	bool m_forceMaskBit=false,m_checkMaskBit=false,m_displayDisabled=true; int m_displayX=0,m_displayY=0; uint32_t m_displayMode=0,m_dmaDirection=0,m_gpuReadLatch=0,m_horizontalRange=0,m_verticalRange=0;
};
} // namespace psxgpu
