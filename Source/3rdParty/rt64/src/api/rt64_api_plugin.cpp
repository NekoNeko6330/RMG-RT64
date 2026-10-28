//
// RT64
//

#include "rt64_api_common.h"

#include <cstring>
#include <SDL.h>

#include "gui/rt64_inspector.h"
#include "hle/rt64_present_queue.h"
#include "plume_vulkan.h"

#if defined(RT64_PLUGIN_CONFIG_GUI)
#   include "rt64_api_config_qt.h"
#endif

#define PLUGIN_NAME                 "RT64 Video Plugin"
#define PLUGIN_VERSION              0x020509
#define VIDEO_PLUGIN_API_VERSION    0x020200
#define CONFIG_API_VERSION          0x020300

#ifndef NDEBUG
#   define LOG_PLUGIN_API_CALLS
#endif

/* definitions of pointers to Core video extension functions */
ptr_VidExt_InitWithRenderMode CoreVideo_InitWithRenderMode = NULL;
ptr_VidExt_Quit CoreVideo_Quit = NULL;
ptr_VidExt_SetCaption CoreVideo_SetCaption = NULL;
ptr_VidExt_ToggleFullScreen CoreVideo_ToggleFullScreen = NULL;
ptr_VidExt_ResizeWindow CoreVideo_ResizeWindow = NULL;
ptr_VidExt_VK_GetSurface CoreVideo_VK_GetSurface = NULL;
ptr_VidExt_VK_GetInstanceExtensions CoreVideo_VK_GetInstanceExtensions = NULL;
ptr_VidExt_SetVideoMode CoreVideo_SetVideoMode = NULL;

#ifdef _WIN32
#define DLSYM(a, b) GetProcAddress(a, b)
#else
#include <dlfcn.h>
#define DLSYM(a, b) dlsym(a, b)
#endif

// Vulkan hooks for plume when the emulator owns the window (mupen64plus video extension).

static bool VidExt_VulkanGetInstanceExtensions(const char ***extensionNames, uint32_t *extensionCount) {
    m64p_error ret = CoreVideo_InitWithRenderMode(M64P_RENDER_VULKAN);
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreVideo_InitWithRenderMode failed with error code %i.\n", ret);
        return false;
    }

    ret = CoreVideo_VK_GetInstanceExtensions(extensionNames, extensionCount);
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreVideo_VK_GetInstanceExtensions failed with error code %i.\n", ret);
        return false;
    }

    return true;
}

static bool VidExt_VulkanCreateSurface(VkInstance instance, VkSurfaceKHR *surface) {
    m64p_error ret = CoreVideo_SetVideoMode(RT64::API.windowWidth, RT64::API.windowHeight, 0, M64VIDEO_WINDOWED, M64VIDEOFLAG_SUPPORT_RESIZING);
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreVideo_SetVideoMode failed with error code %i.\n", ret);
        return false;
    }

    ret = CoreVideo_VK_GetSurface((void **)(surface), (void *)(instance));
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreVideo_VK_GetSurface failed with error code %i.\n", ret);
        return false;
    }

    return true;
}

static bool VidExt_VulkanGetWindowSize(uint32_t *width, uint32_t *height) {
    *width = RT64::API.windowWidth;
    *height = RT64::API.windowHeight;
    return true;
}

static void VidExt_VulkanDestroyInstance() {
    CoreVideo_Quit();
}

// Queries the refresh rate of the primary display through SDL. The video extension has no way
// to expose this, so this is a best-effort estimate used for the "Display" refresh rate mode.
static uint32_t DetectDisplayRefreshRate() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_VIDEO) && (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)) {
        return 0;
    }

    SDL_DisplayMode displayMode = {};
    if (SDL_GetCurrentDisplayMode(0, &displayMode) != 0) {
        return 0;
    }

    return (displayMode.refresh_rate > 0) ? uint32_t(displayMode.refresh_rate) : 0;
}

DLLEXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context, void (*DebugCallback)(void *, int, const char *)) {
    CoreVideo_InitWithRenderMode = (ptr_VidExt_InitWithRenderMode)DLSYM(CoreLibHandle, "VidExt_InitWithRenderMode");
    CoreVideo_Quit = (ptr_VidExt_Quit)DLSYM(CoreLibHandle, "VidExt_Quit");
    CoreVideo_SetCaption = (ptr_VidExt_SetCaption)DLSYM(CoreLibHandle, "VidExt_SetCaption");
    CoreVideo_ToggleFullScreen = (ptr_VidExt_ToggleFullScreen)DLSYM(CoreLibHandle, "VidExt_ToggleFullScreen");
    CoreVideo_ResizeWindow = (ptr_VidExt_ResizeWindow)DLSYM(CoreLibHandle, "VidExt_ResizeWindow");
    CoreVideo_VK_GetSurface = (ptr_VidExt_VK_GetSurface)DLSYM(CoreLibHandle, "VidExt_VK_GetSurface");
    CoreVideo_VK_GetInstanceExtensions = (ptr_VidExt_VK_GetInstanceExtensions)DLSYM(CoreLibHandle, "VidExt_VK_GetInstanceExtensions");
    CoreVideo_SetVideoMode = (ptr_VidExt_SetVideoMode)DLSYM(CoreLibHandle, "VidExt_SetVideoMode");
    if (CoreVideo_InitWithRenderMode == NULL ||
        CoreVideo_Quit == NULL ||
        CoreVideo_SetCaption == NULL ||
        CoreVideo_ToggleFullScreen == NULL ||
        CoreVideo_ResizeWindow == NULL ||
        CoreVideo_VK_GetSurface == NULL ||
        CoreVideo_VK_GetInstanceExtensions == NULL ||
        CoreVideo_SetVideoMode == NULL) {
        return M64ERR_SYSTEM_FAIL;
    }
    return M64ERR_SUCCESS;
}

DLLEXPORT m64p_error CALL PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

DLLEXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities) {
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_GFX;

    if (PluginVersion != NULL)
        *PluginVersion = PLUGIN_VERSION;

    if (APIVersion != NULL)
        *APIVersion = VIDEO_PLUGIN_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = PLUGIN_NAME;

    if (Capabilities != NULL)
        *Capabilities = 0;

    return M64ERR_SUCCESS;
}

#ifdef _WIN32
DLLEXPORT void CALL GetDllInfo(pj64::PLUGIN_INFO *PluginInfo) {
    PluginInfo->Version = 0x103;
    PluginInfo->Type = pj64::PLUGIN_TYPE_VIDEO;
    strncpy(PluginInfo->Name, PLUGIN_NAME, sizeof(PluginInfo->Name));
}
#endif

#if defined(RT64_PLUGIN_CONFIG_GUI)
DLLEXPORT m64p_error CALL PluginConfig(void *parent) {
    RT64ShowConfigDialog(parent);
    return M64ERR_SUCCESS;
}
#endif

DLLEXPORT void CALL MoveScreen(int xpos, int ypos) {
    // Do nothing.
}

DLLEXPORT void CALL RomClosed(void) {
    if (RT64::API.app != nullptr) {
        RT64::API.app->end();
        RT64::API.app.reset();
    }

    plume::VulkanExternal = plume::VulkanExternalHooks();
}

DLLEXPORT int CALL RomOpen(void) {
    const bool isPJ64 = (RT64::API.apiType == RT64::APIType::Project64);
    if (isPJ64) {
        RT64::ApplicationWindow *appWindow = RT64::API.app->appWindow.get();
        appWindow->makeResizable();
    }
    return 1;
}

DLLEXPORT void CALL ViStatusChanged(void) { }

DLLEXPORT void CALL ViWidthChanged(void) { }

DLLEXPORT int CALL InitiateGFX(PluginGraphicsInfo graphicsInfo) {
    // Determine if this is a PJ64 plugin by checking if the HWND element is actually valid.
#ifdef _WIN32
    const bool isPJ64 = IsWindow(HWND(graphicsInfo.project64.hWnd));
#else
    const bool isPJ64 = false;
#endif
    if (isPJ64) {
        RT64::API.apiType = RT64::APIType::Project64;
    }
    else {
        RT64::API.apiType = RT64::APIType::Mupen64Plus;
    }

    // Store core information in application.
    RT64::Application::Core appCore;
    appCore.window = {};

    RT64::ApplicationConfiguration appConfig;
    switch (RT64::API.apiType) {
    case RT64::APIType::Mupen64Plus:
        RT64::InitiateGFXCore<MupenGraphicsInfo>(appCore, graphicsInfo.mupen64plus);

        // The emulator owns the window. Route window queries and Vulkan surface creation through the video extension.
        appConfig.externalWindow = true;
        appConfig.externalRefreshRate = DetectDisplayRefreshRate();
        plume::VulkanExternal.getInstanceExtensions = &VidExt_VulkanGetInstanceExtensions;
        plume::VulkanExternal.createSurface = &VidExt_VulkanCreateSurface;
        plume::VulkanExternal.getWindowSize = &VidExt_VulkanGetWindowSize;
        plume::VulkanExternal.destroyInstance = &VidExt_VulkanDestroyInstance;
        break;
#ifdef _WIN32
    case RT64::APIType::Project64:
        RT64::InitiateGFXCore<Project64GraphicsInfo>(appCore, graphicsInfo.project64);
        appCore.window = plume::RenderWindow(graphicsInfo.project64.hWnd);
        break;
#endif
    default:
        break;
    }

    // Make new application with core information and set it up.
    RT64::API.app = std::make_unique<RT64::Application>(appCore, appConfig);
    return (RT64::API.app->setup(0) == RT64::Application::SetupResult::Success);
}

DLLEXPORT void CALL ResizeVideoOutput(int width, int height) {
    if ((width > 0) && (height > 0)) {
        RT64::API.windowWidth = uint32_t(width);
        RT64::API.windowHeight = uint32_t(height);
    }
}

DLLEXPORT void CALL FBRead(uint32_t addr) {
    // Unused.
}

DLLEXPORT void CALL FBWrite(uint32_t addr, uint32_t size) {
    // Unused.
}

DLLEXPORT void CALL FBGetFrameBufferInfo(void *p) {
    // Unused.
}

DLLEXPORT void CALL ShowCFB(void) {
    // Unused.
}

DLLEXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int bFront) {
    // Unused.
}

DLLEXPORT void CALL SetRenderingCallback(void (*callback)(int)) {
    // Unused.
}

DLLEXPORT void CALL CaptureScreen(const char *Directory) {
    // Unused.
}

// Input forwarded by the emulator so the developer inspector can be used without RT64 owning the window.
// Keys are SDL scancodes as sent by mupen64plus through M64CMD_SEND_SDL_KEYDOWN/KEYUP.

static RT64::Inspector *LockedInspector(std::unique_lock<std::recursive_mutex> &lock) {
    RT64::Application *app = RT64::API.app.get();
    if ((app == nullptr) || (app->presentQueue == nullptr)) {
        return nullptr;
    }

    lock = std::unique_lock<std::recursive_mutex>(app->presentQueue->inspectorMutex);
    return app->presentQueue->inspector.get();
}

DLLEXPORT void CALL SDL_KeyDown(int keymod, int keysym) {
    std::unique_lock<std::recursive_mutex> lock;
    RT64::Inspector *inspector = LockedInspector(lock);
    if (inspector != nullptr) {
        inspector->injectKeyEvent(SDL_Scancode(keysym), keymod, true);
    }
}

DLLEXPORT void CALL SDL_KeyUp(int keymod, int keysym) {
    if (keysym == SDL_SCANCODE_F10) {
        if (RT64::API.app != nullptr) {
            RT64::API.app->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
        }

        return;
    }

    std::unique_lock<std::recursive_mutex> lock;
    RT64::Inspector *inspector = LockedInspector(lock);
    if (inspector != nullptr) {
        inspector->injectKeyEvent(SDL_Scancode(keysym), keymod, false);
    }
}

DLLEXPORT void CALL MouseMove(int x, int y) {
    std::unique_lock<std::recursive_mutex> lock;
    RT64::Inspector *inspector = LockedInspector(lock);
    if (inspector != nullptr) {
        inspector->injectMouseMove(float(x), float(y));
    }
}

DLLEXPORT void CALL MouseButton(int left, int right) {
    std::unique_lock<std::recursive_mutex> lock;
    RT64::Inspector *inspector = LockedInspector(lock);
    if (inspector != nullptr) {
        inspector->injectMouseButton(0, left != 0);
        inspector->injectMouseButton(1, right != 0);
    }
}
