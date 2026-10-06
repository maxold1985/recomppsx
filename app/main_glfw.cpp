#include "psxexe.h"
#include "psxrecomp/generated_game.h"
#include "psxrecomp/psx_runtime.h"
#include "psxrecomp/trace.h"
#include "psxgpu/psx_gpu_gl.h"

#include <GLFW/glfw3.h>
#ifdef _WIN32
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <exception>
#include <memory>
#include <string>

#if defined(_MSC_VER)
#  define PSX_U64_FMT "%I64u"
#  define PSX_U64_HEX_FMT "%016I64X"
#else
#  define PSX_U64_FMT "%llu"
#  define PSX_U64_HEX_FMT "%016llX"
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

enum {
    IDC_LOG_BUTTON = 4100,
    IDC_LOG_CPU,
    IDC_LOG_GPU,
    IDC_LOG_IRQ,
    IDC_LOG_BIOS,
    IDC_LOG_CD,
    IDC_LOG_DMA,
    IDC_LOG_HLE,
    IDC_LOG_RUNTIME,
    IDC_LOG_OTHER,
    IDC_EMU_START,
    IDC_EMU_PAUSE,
    IDC_EMU_STOP
};

enum EmulationRunState {
    EmulationRunning = 0,
    EmulationPaused,
    EmulationStopped
};

volatile EmulationRunState g_emulationState = EmulationStopped;
volatile bool g_emulationStarted = false;
volatile bool g_restartGuest = false;

struct LogControls {
    HWND button;
    HWND startButton;
    HWND pauseButton;
    HWND stopButton;
    HWND checks[psxrecomp::TraceCategoryCount];
    bool visible;
    LogControls() : button(0), startButton(0), pauseButton(0), stopButton(0), visible(false) {
        for (int i=0;i<psxrecomp::TraceCategoryCount;++i) checks[i]=0;
    }
};

LogControls g_logControls;

void set_log_panel_visible(bool visible)
{
    g_logControls.visible=visible;
    for(int i=0;i<psxrecomp::TraceCategoryCount;++i)
        if(g_logControls.checks[i])
            ShowWindow(g_logControls.checks[i], visible ? SW_SHOW : SW_HIDE);
}

void sync_log_checkboxes()
{
    for(int i=0;i<psxrecomp::TraceCategoryCount;++i){
        if(!g_logControls.checks[i]) continue;
        const LRESULT checked=SendMessageA(g_logControls.checks[i],BM_GETCHECK,0,0);
        psxrecomp::traceSetCategoryEnabled(
            static_cast<psxrecomp::TraceCategory>(i),
            checked==BST_CHECKED);
    }
}

LRESULT CALLBACK log_panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC oldProc=reinterpret_cast<WNDPROC>(
        GetPropA(hwnd,"PSX_OLD_WNDPROC"));

    if(msg==WM_COMMAND){
        const int id=LOWORD(wp);
        if(id==IDC_LOG_BUTTON && HIWORD(wp)==BN_CLICKED){
            set_log_panel_visible(!g_logControls.visible);
            return 0;
        }
        if(id==IDC_EMU_START && HIWORD(wp)==BN_CLICKED){
            if(g_emulationState==EmulationStopped && g_emulationStarted)
                g_restartGuest=true;
            g_emulationStarted=true;
            g_emulationState=EmulationRunning;
            return 0;
        }
        if(id==IDC_EMU_PAUSE && HIWORD(wp)==BN_CLICKED){
            if(g_emulationState==EmulationRunning)
                g_emulationState=EmulationPaused;
            return 0;
        }
        if(id==IDC_EMU_STOP && HIWORD(wp)==BN_CLICKED){
            if(g_emulationStarted)
                g_restartGuest=true;
            g_emulationState=EmulationStopped;
            return 0;
        }
        if(id>=IDC_LOG_CPU && id<=IDC_LOG_OTHER && HIWORD(wp)==BN_CLICKED){
            sync_log_checkboxes();
            return 0;
        }
    }
    return oldProc ? CallWindowProcA(oldProc,hwnd,msg,wp,lp)
                   : DefWindowProcA(hwnd,msg,wp,lp);
}

void create_log_controls(GLFWwindow* window)
{
    HWND hwnd=glfwGetWin32Window(window);
    if(!hwnd) return;

    WNDPROC oldProc=reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd,GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(log_panel_proc)));
    SetPropA(hwnd,"PSX_OLD_WNDPROC",reinterpret_cast<HANDLE>(oldProc));

    HFONT font=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    g_logControls.button=CreateWindowA("BUTTON","Logs",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,8,8,72,24,
        hwnd,reinterpret_cast<HMENU>(IDC_LOG_BUTTON),
        GetModuleHandleA(0),0);
    SendMessageA(g_logControls.button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);

    g_logControls.startButton=CreateWindowA("BUTTON","Start",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,88,8,72,24,
        hwnd,reinterpret_cast<HMENU>(IDC_EMU_START),GetModuleHandleA(0),0);
    g_logControls.pauseButton=CreateWindowA("BUTTON","Pause",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,168,8,72,24,
        hwnd,reinterpret_cast<HMENU>(IDC_EMU_PAUSE),GetModuleHandleA(0),0);
    g_logControls.stopButton=CreateWindowA("BUTTON","Stop",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,248,8,72,24,
        hwnd,reinterpret_cast<HMENU>(IDC_EMU_STOP),GetModuleHandleA(0),0);
    SendMessageA(g_logControls.startButton,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    SendMessageA(g_logControls.pauseButton,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    SendMessageA(g_logControls.stopButton,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);

    const char* names[psxrecomp::TraceCategoryCount]={
        "CPU","GPU","IRQ","BIOS","CD-ROM","DMA","HLE","Runtime","Other"
    };
    for(int i=0;i<psxrecomp::TraceCategoryCount;++i){
        g_logControls.checks[i]=CreateWindowA("BUTTON",names[i],
            WS_CHILD|BS_AUTOCHECKBOX,8,38+i*23,100,21,
            hwnd,reinterpret_cast<HMENU>(IDC_LOG_CPU+i),
            GetModuleHandleA(0),0);
        SendMessageA(g_logControls.checks[i],WM_SETFONT,
            reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageA(g_logControls.checks[i],BM_SETCHECK,
            psxrecomp::traceCategoryEnabled(
                static_cast<psxrecomp::TraceCategory>(i))
                ? BST_CHECKED : BST_UNCHECKED,0);
    }
    set_log_panel_visible(false);
}
#endif

namespace {

FILE* g_logFile = 0;
volatile unsigned long g_guestPc = 0;
volatile unsigned long long g_guestCycles = 0;
volatile long g_hostFrame = 0;

void log_line(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);

    va_end(ap);

    if (g_logFile) {
        va_start(ap, fmt);
        std::vfprintf(g_logFile, fmt, ap);
        va_end(ap);
        std::fflush(g_logFile);
    }
}

void log_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    std::fflush(stderr);
    va_end(ap);

    if (g_logFile) {
        va_start(ap, fmt);
        std::vfprintf(g_logFile, fmt, ap);
        va_end(ap);
        std::fflush(g_logFile);
    }
}

void glfw_error_callback(int error, const char* description)
{
    log_error("[GLFW ERROR] %d: %s\n", error,
              description ? description : "(null)");
}

#ifdef _WIN32
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info)
{
    const DWORD code = info && info->ExceptionRecord
        ? info->ExceptionRecord->ExceptionCode : 0;
    const void* address = info && info->ExceptionRecord
        ? info->ExceptionRecord->ExceptionAddress : 0;

    log_error("\n[HOST CRASH] Windows exception 0x%08lX at %p\n",
              (unsigned long)code, address);
    log_error("[HOST CRASH] last guest PC=0x%08lX cycles=" PSX_U64_FMT " hostFrame=%ld\n",
              (unsigned long)g_guestPc,
              (unsigned long long)g_guestCycles,
              (long)g_hostFrame);

#  if defined(_M_IX86)
    if (info && info->ContextRecord) {
        CONTEXT* c = info->ContextRecord;
        log_error("[HOST CRASH] EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
                  (unsigned long)c->Eip, (unsigned long)c->Esp,
                  (unsigned long)c->Ebp, (unsigned long)c->Eax,
                  (unsigned long)c->Ebx, (unsigned long)c->Ecx,
                  (unsigned long)c->Edx);
    }
#  elif defined(_M_X64)
    if (info && info->ContextRecord) {
        CONTEXT* c = info->ContextRecord;
        log_error("[HOST CRASH] RIP=" PSX_U64_HEX_FMT " RSP=" PSX_U64_HEX_FMT " RBP=" PSX_U64_HEX_FMT "\n",
                  (unsigned long long)c->Rip,
                  (unsigned long long)c->Rsp,
                  (unsigned long long)c->Rbp);
    }
#  endif

    log_error("[HOST CRASH] See psx_recomp_gl.log for the boot trace.\n");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

bool file_exists(const char* path)
{
    if (!path || !*path) return false;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void print_usage()
{
    log_error("usage: psx_recomp_gl <PS-X EXE> [disc.bin|disc.iso|CDROOT]\n");
    log_error("example: psx_recomp_gl SLPS_027.11 CDROOT\n");
}

} // namespace

int main(int argc, char** argv)
{
    psxrecomp::traceOpen("psx_full_trace.log");
    psxrecomp::tracePrintf("[TRACE] full trace started; console + psx_full_trace.log\n");
    g_logFile = std::fopen("psx_recomp_gl.log", "wb");
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#endif

    log_line("[BOOT] psx_recomp_gl starting\n");
    log_line("[BOOT] argc=%d\n", argc);
    for (int i = 0; i < argc; ++i)
        log_line("[BOOT] argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");

    if (argc < 2) {
        print_usage();
        if (g_logFile) std::fclose(g_logFile);
        return 1;
    }

    if (!file_exists(argv[1])) {
        log_error("[FATAL] PS-X EXE not found: %s\n", argv[1]);
        if (g_logFile) std::fclose(g_logFile);
        return 2;
    }

    glfwSetErrorCallback(glfw_error_callback);

    try {
        log_line("[BOOT] loading PS-X EXE...\n");
        const PsxExeImage image = load_psx_exe(argv[1]);
        log_line("[BOOT] EXE entry=0x%08X load=0x%08X size=0x%X\n",
                 image.initial_pc, image.load_address,
                 (unsigned)image.payload.size());

        log_line("[BOOT] constructing PSX runtime on heap...\n");
        std::unique_ptr<psxrecomp::PsxRuntime> runtime(new psxrecomp::PsxRuntime());

        log_line("[BOOT] loading executable into guest RAM...\n");
        runtime->loadExecutable(image);
        g_guestPc = runtime->cpu().pc;
        g_guestCycles = runtime->cpu().cycles;
        log_line("[BOOT] guest PC=0x%08X SP=0x%08X\n",
                 runtime->cpu().pc, runtime->cpu().gpr[29]);

        if (argc >= 3) {
            log_line("[BOOT] mounting disc/CDROOT: %s\n", argv[2]);
            if (!runtime->mountDisc(argv[2])) {
                log_error("[WARN] could not mount disc/CDROOT: %s\n", argv[2]);
            } else {
                log_line("[BOOT] disc/CDROOT mounted\n");
            }
        } else {
            log_line("[WARN] no disc/CDROOT argument supplied\n");
        }

        log_line("[BOOT] glfwInit...\n");
        if (!glfwInit()) {
            log_error("[FATAL] glfwInit failed\n");
            if (g_logFile) std::fclose(g_logFile);
            return 3;
        }
        log_line("[BOOT] GLFW %s\n", glfwGetVersionString());

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);

        log_line("[BOOT] creating OpenGL 3.3 window...\n");
        GLFWwindow* window = glfwCreateWindow(
            960, 720,
            "R3000A Recomp + PSX Runtime + OpenGL",
            0, 0);
        if (!window) {
            log_error("[FATAL] glfwCreateWindow failed\n");
            glfwTerminate();
            if (g_logFile) std::fclose(g_logFile);
            return 4;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
#ifdef _WIN32
        create_log_controls(window);
#endif

        const GLubyte* glVersion = glGetString(GL_VERSION);
        const GLubyte* glRenderer = glGetString(GL_RENDERER);
        const GLubyte* glVendor = glGetString(GL_VENDOR);
        log_line("[GL] version=%s\n", glVersion ? (const char*)glVersion : "(null)");
        log_line("[GL] renderer=%s\n", glRenderer ? (const char*)glRenderer : "(null)");
        log_line("[GL] vendor=%s\n", glVendor ? (const char*)glVendor : "(null)");

        log_line("[BOOT] initializing PSX GPU OpenGL presenter...\n");
        psxgpu::PsxGpuGlPresenter presenter;
        if (!presenter.initialize([](const char* name) -> void* {
            return reinterpret_cast<void*>(glfwGetProcAddress(name));
        })) {
            log_error("[FATAL] OpenGL presenter init failed: %s\n",
                      presenter.lastError());
            glfwDestroyWindow(window);
            glfwTerminate();
            if (g_logFile) std::fclose(g_logFile);
            return 5;
        }

        log_line("[BOOT] entering recompiled game loop\n");
        const int kBlocksPerHostFrame = 50000;
        bool warnedStall = false;

        while (!glfwWindowShouldClose(window)) {
            ++g_hostFrame;
            glfwPollEvents();

            const bool up = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
            const bool down = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
            const bool left = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            const bool right = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            if(g_restartGuest && g_emulationState==EmulationRunning){
                log_line("[CONTROL] Start after Stop: restarting guest\n");
                runtime->loadExecutable(image);
                if(argc>=3) runtime->mountDisc(argv[2]);
                g_guestPc=runtime->cpu().pc;
                g_guestCycles=runtime->cpu().cycles;
                warnedStall=false;
                g_restartGuest=false;
            }

            psxrecomp::PsxPadSio& pad = runtime->pad();
            pad.setButton(psxrecomp::PsxPadSio::Up, up);
            pad.setButton(psxrecomp::PsxPadSio::Down, down);
            pad.setButton(psxrecomp::PsxPadSio::Left, left);
            pad.setButton(psxrecomp::PsxPadSio::Right, right);
            pad.setButton(psxrecomp::PsxPadSio::Cross, glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::Circle, glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::Square, glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::Triangle, glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::L1, glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::R1, glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::Start, glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS);
            pad.setButton(psxrecomp::PsxPadSio::Select, glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

            if(g_emulationState==EmulationRunning) {
            for (int i = 0; i < kBlocksPerHostFrame; ++i) {
                g_guestPc = runtime->cpu().pc;
                g_guestCycles = runtime->cpu().cycles;

                if (runtime->handleHle()) {
                    runtime->advance(20);
                    continue;
                }

                const uint32_t oldPc = runtime->cpu().pc;
                const uint64_t oldCycles = runtime->cpu().cycles;

                run_recompiled(runtime->cpu(), runtime->memory(), runtime->gte());

                g_guestPc = runtime->cpu().pc;
                g_guestCycles = runtime->cpu().cycles;

                const uint64_t elapsed = runtime->cpu().cycles - oldCycles;
                runtime->advance(static_cast<uint32_t>(elapsed ? elapsed : 1));

                if (runtime->cpu().pc == oldPc && elapsed == 0 && !runtime->handleHle()) {
                    if (!warnedStall) {
                        log_error("[STALL] unrecompiled/unknown guest PC 0x%08X at cycle " PSX_U64_FMT "\n",
                                  runtime->cpu().pc,
                                  (unsigned long long)runtime->cpu().cycles);
                        log_error("[STALL] window remains open; see generated code/runtime coverage\n");
                        warnedStall = true;
                    }
                    break;
                }
            }
            }

            if ((g_hostFrame % 300) == 0) {
                log_line("[RUN] frame=%ld guestPC=0x%08lX cycles=" PSX_U64_FMT "\n",
                         (long)g_hostFrame,
                         (unsigned long)g_guestPc,
                         (unsigned long long)g_guestCycles);
            }

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            presenter.present(runtime->gpu(), width, height);
            glfwSwapBuffers(window);
        }

        log_line("[SHUTDOWN] normal exit\n");
        presenter.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    catch (const std::exception& e) {
        log_error("[FATAL C++] %s\n", e.what());
        log_error("[FATAL C++] guest PC=0x%08lX cycles=" PSX_U64_FMT "\n",
                  (unsigned long)g_guestPc,
                  (unsigned long long)g_guestCycles);
        if (g_logFile) std::fclose(g_logFile);
        return 10;
    }
    catch (...) {
        log_error("[FATAL C++] unknown exception\n");
        log_error("[FATAL C++] guest PC=0x%08lX cycles=" PSX_U64_FMT "\n",
                  (unsigned long)g_guestPc,
                  (unsigned long long)g_guestCycles);
        if (g_logFile) std::fclose(g_logFile);
        return 11;
    }

    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = 0;
    }
    return 0;
}
