//
// RT64
//

#pragma once

#include <chrono>
#include <mutex>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "common/rt64_common.h"
#include "common/rt64_user_configuration.h"
#include "render/rt64_render_worker.h"
#include "hle/rt64_application_window.h"
#include "plume_render_interface.h"

namespace RT64 {
    struct VulkanContext;

    struct Inspector {
        RenderDevice *device = nullptr;
        const RenderSwapChain *swapChain = nullptr;
        UserConfiguration::GraphicsAPI graphicsAPI;
        SDL_Window *sdlWindow = nullptr;
        bool externalWindow = false;
        std::chrono::steady_clock::time_point externalFrameTime;
        std::unique_ptr<RenderDescriptorSet> descriptorSet;
        std::unique_ptr<VulkanContext> vulkanContext;
        std::mutex frameMutex;

        Inspector(RenderDevice *device, const RenderSwapChain *swapChain, UserConfiguration::GraphicsAPI graphicsAPI, SDL_Window *sdlWindow, bool externalWindow = false);
        ~Inspector();
        void setIniPath(const std::filesystem::path &path);
        void newFrame(RenderWorker *worker);
        void endFrame();
        void draw(RenderCommandList *commandList);
#   ifdef _WIN32
        bool handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
#   endif
        bool handleSdlEvent(SDL_Event *event);

        // Input injection for hosts that own the window and forward their events (keys are SDL scancodes).
        void injectKeyEvent(SDL_Scancode scancode, int keymod, bool down);
        void injectMouseMove(float x, float y);
        void injectMouseButton(int button, bool down);
    };
};
