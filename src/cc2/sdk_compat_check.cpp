// Compile-time guard for the SDK surface cc2 builds on.
// Nothing here runs: every check is a static_assert, so an SDK change that moves one of these fails the build naming the API.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/downmix.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/register_file.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ppc/function.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#define CC2_SDK_SIG(expr, type, api) \
  static_assert(std::is_same_v<decltype(expr), type>, "SDK drift: " api " changed signature")
#define CC2_SDK_BASE(base, derived, api) \
  static_assert(std::is_base_of_v<base, derived>, "SDK drift: " api " base class changed")

// Hook macros are the codegen/title contract; losing one breaks every hook file at once.
#ifndef REX_EXPORT
#error "SDK drift: REX_EXPORT (rex/hook.h) is gone"
#endif
#ifndef REX_HOOK_RAW
#error "SDK drift: REX_HOOK_RAW (rex/hook.h) is gone"
#endif
#ifndef REX_FUNC
#error "SDK drift: REX_FUNC (rex/ppc/func.h) is gone"
#endif
#ifndef REX_AUDIO_BACKEND
#error "SDK drift: REX_AUDIO_BACKEND (rex/runtime.h) is gone"
#endif
#ifndef REX_DEFINE_APP
#error "SDK drift: REX_DEFINE_APP (rex/ui/windowed_app.h) is gone"
#endif
#ifndef REX_KERNEL_STATE
#error "SDK drift: REX_KERNEL_STATE (rex/system/kernel_state.h) is gone"
#endif
#ifndef REX_KERNEL_MEMORY
#error "SDK drift: REX_KERNEL_MEMORY (rex/system/kernel_state.h) is gone"
#endif
#ifndef REX_KERNEL_OBJECTS
#error "SDK drift: REX_KERNEL_OBJECTS (rex/system/kernel_state.h) is gone"
#endif
#if !defined(REXCVAR_DECLARE) || !defined(REXCVAR_GET) || !defined(REXCVAR_DEFINE_STRING) || \
    !defined(REXCVAR_DEFINE_BOOL) || !defined(REXCVAR_DEFINE_INT32) || !defined(REXCVAR_DEFINE_DOUBLE)
#error "SDK drift: a REXCVAR_* macro (rex/cvar.h) is gone"
#endif
#if !defined(REXLOG_INFO) || !defined(REXLOG_WARN) || !defined(REXLOG_ERROR) || !defined(REXKRNL_WARN)
#error "SDK drift: a REXLOG_/REXKRNL_ macro (rex/logging.h) is gone"
#endif

namespace cc2::sdk_compat {

// REX_FUNC must still spell the PPCFunc signature that REX_HOOK_RAW bodies and the registry share.
REX_FUNC(cc2_sdk_compat_rex_func_probe);
CC2_SDK_SIG(cc2_sdk_compat_rex_func_probe, PPCFunc, "REX_FUNC / PPCFunc");
static_assert(std::is_same_v<PPCFunc, void(PPCContext&, uint8_t*)>, "SDK drift: PPCFunc changed signature");

// Mid-asm hooks: codegen declares `bool/void Name(PPCRegister&...)` and hands it ctx.rN / ctx.fN.
static_assert(std::is_same_v<PPCRegister, rex::ppc::Register>, "SDK drift: PPCRegister alias moved");
CC2_SDK_SIG(std::declval<PPCContext&>().r3, PPCRegister, "PPCContext::r3 (midasm hook register)");
CC2_SDK_SIG(std::declval<PPCContext&>().f1, PPCRegister, "PPCContext::f1 (midasm hook register)");
CC2_SDK_SIG(std::declval<PPCRegister&>().u64, uint64_t, "PPCRegister::u64");
CC2_SDK_SIG(std::declval<PPCRegister&>().u32, uint32_t, "PPCRegister::u32");
CC2_SDK_SIG(std::declval<PPCRegister&>().f64, double, "PPCRegister::f64");
CC2_SDK_SIG(std::declval<PPCContext&>().lr, uint64_t, "PPCContext::lr");
CC2_SDK_SIG(std::declval<PPCContext&>().last_indirect_target, uint32_t,
            "PPCContext::last_indirect_target (function_dispatch.cpp)");

// Function registry: export_override.cpp walks both tables and patches the runtime DLL's copy by its mangled name.
CC2_SDK_SIG(PPCFuncMapping::guest, size_t, "PPCFuncMapping::guest");
CC2_SDK_SIG(PPCFuncMapping::host, PPCFunc*, "PPCFuncMapping::host");
CC2_SDK_SIG(PPCFuncMappings, PPCFuncMapping[], "PPCFuncMappings");
using Registry = std::vector<std::pair<const char*, PPCFunc*>>;
CC2_SDK_SIG(&rex::ppc::GetPPCFuncRegistry, Registry& (*)(),
            "rex::ppc::GetPPCFuncRegistry (its mangled static is kRuntimeRegistry in export_override.cpp)");
static_assert(std::is_constructible_v<rex::ppc::detail::PPCFuncRegistrar, const char*, PPCFunc*>,
              "SDK drift: rex::ppc::detail::PPCFuncRegistrar (REX_EXPORT) changed");
CC2_SDK_SIG(&rex::runtime::ResolveIndirectFunction, PPCFunc* (*)(uint32_t),
            "rex::runtime::ResolveIndirectFunction (title override in function_dispatch.cpp)");
CC2_SDK_SIG(std::declval<rex::runtime::FunctionDispatcher&>().GetFunction(0u), PPCFunc*,
            "FunctionDispatcher::GetFunction");

// App shell. The hooks are protected, so the pointers are formed from inside a derived probe.
struct AppProbe : rex::ReXApp {
  static void Check() {
    CC2_SDK_SIG(&AppProbe::OnConfigurePaths, void (rex::ReXApp::*)(rex::PathConfig&),
                "ReXApp::OnConfigurePaths");
    CC2_SDK_SIG(&AppProbe::OnPreSetup, void (rex::ReXApp::*)(rex::RuntimeConfig&), "ReXApp::OnPreSetup");
    CC2_SDK_SIG(&AppProbe::OnCreateDialogs, void (rex::ReXApp::*)(rex::ui::ImGuiDrawer*),
                "ReXApp::OnCreateDialogs");
    CC2_SDK_SIG(&AppProbe::SetupPresentation, bool (rex::ReXApp::*)(), "ReXApp::SetupPresentation");
    CC2_SDK_SIG(&AppProbe::OnPostLoadXexImage, void (rex::ReXApp::*)(), "ReXApp::OnPostLoadXexImage");
    CC2_SDK_SIG(&AppProbe::OnPreLaunchModule, void (rex::ReXApp::*)(), "ReXApp::OnPreLaunchModule");
    CC2_SDK_SIG(&AppProbe::OnShutdown, void (rex::ReXApp::*)(), "ReXApp::OnShutdown");
    CC2_SDK_SIG(&AppProbe::runtime, rex::Runtime* (rex::ReXApp::*)() const, "ReXApp::runtime");
    CC2_SDK_SIG(&AppProbe::window, rex::ui::Window* (rex::ReXApp::*)() const, "ReXApp::window");
    CC2_SDK_SIG(&AppProbe::immediate_drawer, rex::ui::ImmediateDrawer* (rex::ReXApp::*)() const,
                "ReXApp::immediate_drawer");
    CC2_SDK_SIG(&AppProbe::game_data_root, const std::filesystem::path& (rex::ReXApp::*)() const,
                "ReXApp::game_data_root");
    CC2_SDK_SIG(&AppProbe::user_data_root, const std::filesystem::path& (rex::ReXApp::*)() const,
                "ReXApp::user_data_root");
  }
  // A "no matching constructor" error here means ReXApp(WindowedAppContext&, string_view, PPCImageInfo) drifted.
  AppProbe(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
      : rex::ReXApp(ctx, name, info) {}
};
CC2_SDK_BASE(rex::ui::WindowedApp, rex::ReXApp, "ReXApp : ui::WindowedApp (Cc2App::Create)");
CC2_SDK_SIG(std::declval<rex::PathConfig&>().config_path, std::filesystem::path, "PathConfig::config_path");
CC2_SDK_SIG(std::declval<rex::PathConfig&>().game_data_root, std::filesystem::path,
            "PathConfig::game_data_root");
CC2_SDK_SIG(std::declval<rex::PathConfig&>().user_data_root, std::filesystem::path,
            "PathConfig::user_data_root");
CC2_SDK_SIG(std::declval<rex::ui::Window&>().SetTitle(std::string_view{}), void, "ui::Window::SetTitle");
CC2_SDK_SIG(std::declval<rex::ui::Window&>().SetPresenter(nullptr), void, "ui::Window::SetPresenter");
CC2_SDK_SIG(std::declval<rex::ui::WindowedAppContext&>().QuitFromUIThread(), void,
            "WindowedAppContext::QuitFromUIThread");

// RuntimeConfig: cc2 sets audio_factory and kernel_init; graphics/gpu_plugin stay at defaults (gpu_plugin comes from the toml cvar).
CC2_SDK_SIG(rex::RuntimeConfig::kernel_init, std::function<void(rex::Runtime*, rex::system::KernelState*)>,
            "RuntimeConfig::kernel_init");
CC2_SDK_SIG(rex::RuntimeConfig::audio_factory,
            std::function<std::unique_ptr<rex::system::IAudioSystem>(rex::runtime::FunctionDispatcher*)>,
            "RuntimeConfig::audio_factory");
CC2_SDK_SIG(rex::RuntimeConfig::graphics, std::unique_ptr<rex::system::IGraphicsSystem>,
            "RuntimeConfig::graphics");
CC2_SDK_SIG(rex::RuntimeConfig::gpu_plugin, std::string, "RuntimeConfig::gpu_plugin");

// Runtime accessors.
CC2_SDK_SIG(&rex::Runtime::instance, rex::Runtime* (*)(), "Runtime::instance");
CC2_SDK_SIG(&rex::Runtime::memory, rex::memory::Memory* (rex::Runtime::*)() const, "Runtime::memory");
CC2_SDK_SIG(&rex::Runtime::kernel_state, rex::system::KernelState* (rex::Runtime::*)() const,
            "Runtime::kernel_state");
CC2_SDK_SIG(&rex::Runtime::graphics_system, rex::system::IGraphicsSystem* (rex::Runtime::*)() const,
            "Runtime::graphics_system");
CC2_SDK_SIG(&rex::Runtime::audio_system, rex::system::IAudioSystem* (rex::Runtime::*)() const,
            "Runtime::audio_system");
CC2_SDK_SIG(&rex::Runtime::input_system, rex::system::IInputSystem* (rex::Runtime::*)() const,
            "Runtime::input_system");
CC2_SDK_SIG(&rex::Runtime::function_dispatcher, rex::runtime::FunctionDispatcher* (rex::Runtime::*)() const,
            "Runtime::function_dispatcher");

// Guest memory.
CC2_SDK_SIG(std::declval<rex::memory::Memory&>().TranslateVirtual(0u), uint8_t*, "Memory::TranslateVirtual");
CC2_SDK_SIG(std::declval<rex::memory::Memory&>().TranslateVirtual<const char*>(0u), const char*,
            "Memory::TranslateVirtual<T>");
CC2_SDK_SIG(std::declval<rex::memory::Memory&>().SystemHeapAlloc(0u, 0u), uint32_t, "Memory::SystemHeapAlloc");
CC2_SDK_SIG(std::declval<rex::memory::Memory&>().LookupHeap(0u), rex::memory::BaseHeap*, "Memory::LookupHeap");
CC2_SDK_SIG(std::declval<rex::memory::Memory&>().HostToGuestVirtual(nullptr), uint32_t,
            "Memory::HostToGuestVirtual");
CC2_SDK_SIG(&rex::memory::BaseHeap::QueryProtect, bool (rex::memory::BaseHeap::*)(uint32_t, uint32_t*),
            "BaseHeap::QueryProtect");
static_assert(uint32_t(rex::memory::kMemoryProtectRead) != 0, "SDK drift: memory::kMemoryProtectRead is gone");

// Kernel state and modules.
CC2_SDK_SIG(REX_KERNEL_STATE(), rex::system::KernelState*, "REX_KERNEL_STATE");
CC2_SDK_SIG(REX_KERNEL_MEMORY(), rex::memory::Memory*, "REX_KERNEL_MEMORY");
CC2_SDK_SIG(&rex::system::kernel_state, rex::system::KernelState* (*)(), "system::kernel_state");
CC2_SDK_SIG(&rex::system::KernelState::app_manager,
            rex::system::xam::AppManager* (rex::system::KernelState::*)() const, "KernelState::app_manager");
CC2_SDK_SIG(&rex::system::KernelState::emulator, rex::Runtime* (rex::system::KernelState::*)() const,
            "KernelState::emulator");
CC2_SDK_SIG(&rex::system::KernelState::BroadcastNotification,
            void (rex::system::KernelState::*)(rex::system::XNotificationID, uint32_t),
            "KernelState::BroadcastNotification");
CC2_SDK_SIG(&rex::system::KernelState::CompleteOverlappedImmediate,
            void (rex::system::KernelState::*)(uint32_t, rex::X_RESULT), "KernelState::CompleteOverlappedImmediate");
CC2_SDK_SIG(std::declval<rex::system::KernelState&>().LoadKernelModule<rex::kernel::xboxkrnl::XboxkrnlModule>(),
            rex::system::object_ref<rex::system::KernelModule>, "KernelState::LoadKernelModule<T>");
CC2_SDK_SIG(REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(0u),
            rex::system::object_ref<rex::system::XEvent>, "ObjectTable::LookupObject<XEvent>");
static_assert(std::is_constructible_v<rex::kernel::xboxkrnl::XboxkrnlModule, rex::Runtime*,
                                      rex::system::KernelState*> &&
                  std::is_constructible_v<rex::kernel::xam::XamModule, rex::Runtime*, rex::system::KernelState*> &&
                  std::is_constructible_v<rex::kernel::xbdm::XbdmModule, rex::Runtime*, rex::system::KernelState*>,
              "SDK drift: kernel module ctor (Runtime*, KernelState*) changed (InitializeTitleKernel)");
CC2_SDK_SIG(&rex::system::XThread::GetCurrentThread, rex::system::XThread* (*)(), "XThread::GetCurrentThread");
CC2_SDK_SIG(&rex::system::XThread::active_cpu, uint8_t (rex::system::XThread::*)() const, "XThread::active_cpu");
CC2_SDK_SIG(&rex::runtime::ThreadState::Get, rex::runtime::ThreadState* (*)(), "ThreadState::Get");
CC2_SDK_SIG(&rex::runtime::ThreadState::context, PPCContext* (rex::runtime::ThreadState::*)() const,
            "ThreadState::context");
CC2_SDK_SIG(&rex::runtime::current_ppc_context, PPCContext* (*)(), "runtime::current_ppc_context");

// XAM apps: the title replaces 0xFC and re-registers the SDK's other apps in the same order.
struct XamAppProbe : rex::system::xam::App {
  // A "no matching constructor" error here means xam::App(KernelState*, uint32_t) drifted.
  explicit XamAppProbe(rex::system::KernelState* ks) : App(ks, 0xFC) {}
  rex::X_HRESULT DispatchMessageSync(uint32_t, uint32_t, uint32_t) override { return 0; }
};
static_assert(std::is_constructible_v<XamAppProbe, rex::system::KernelState*>,
              "SDK drift: system::xam::App(KernelState*, uint32_t) or DispatchMessageSync changed");
CC2_SDK_SIG(&rex::system::xam::App::DispatchMessageSync,
            rex::X_HRESULT (rex::system::xam::App::*)(uint32_t, uint32_t, uint32_t), "xam::App::DispatchMessageSync");
CC2_SDK_BASE(rex::system::xam::App, rex::kernel::xam::apps::XgiApp, "XgiApp");
CC2_SDK_BASE(rex::system::xam::App, rex::kernel::xam::apps::XmpApp, "XmpApp");
CC2_SDK_BASE(rex::system::xam::App, rex::kernel::xam::apps::XamApp, "XamApp");
static_assert(std::is_constructible_v<rex::kernel::xam::apps::XgiApp, rex::system::KernelState*> &&
                  std::is_constructible_v<rex::kernel::xam::apps::XmpApp, rex::system::KernelState*> &&
                  std::is_constructible_v<rex::kernel::xam::apps::XamApp, rex::system::KernelState*>,
              "SDK drift: XgiApp/XmpApp/XamApp(KernelState*) ctor changed");
static_assert(!std::is_final_v<rex::kernel::xam::apps::XgiApp>, "SDK drift: XgiApp became final (Cc2XgiApp)");
CC2_SDK_SIG(&rex::system::xam::AppManager::RegisterApp,
            void (rex::system::xam::AppManager::*)(std::unique_ptr<rex::system::xam::App>), "AppManager::RegisterApp");

// Overlays and frame capture.
struct DialogProbe : rex::ui::ImGuiDialog {
  explicit DialogProbe(rex::ui::ImGuiDrawer* d) : ImGuiDialog(d) {}
  static void Check() {
    CC2_SDK_SIG(&DialogProbe::OnDraw, void (rex::ui::ImGuiDialog::*)(ImGuiIO&), "ImGuiDialog::OnDraw");
  }
};
static_assert(std::is_constructible_v<DialogProbe, rex::ui::ImGuiDrawer*>,
              "SDK drift: ImGuiDialog(ImGuiDrawer*) changed");
CC2_SDK_SIG(&rex::ui::ImmediateDrawer::CreateTexture,
            std::unique_ptr<rex::ui::ImmediateTexture> (rex::ui::ImmediateDrawer::*)(
                uint32_t, uint32_t, rex::ui::ImmediateTextureFilter, bool, const uint8_t*),
            "ImmediateDrawer::CreateTexture");
CC2_SDK_SIG(&rex::ui::Presenter::CaptureGuestOutput, bool (rex::ui::Presenter::*)(rex::ui::RawImage&),
            "Presenter::CaptureGuestOutput");
CC2_SDK_SIG(&rex::system::IGraphicsSystem::presenter,
            rex::ui::Presenter* (rex::system::IGraphicsSystem::*)() const, "IGraphicsSystem::presenter");
CC2_SDK_SIG(rex::ui::RawImage::width, uint32_t, "RawImage::width");
CC2_SDK_SIG(rex::ui::RawImage::height, uint32_t, "RawImage::height");
CC2_SDK_SIG(rex::ui::RawImage::stride, size_t, "RawImage::stride");
CC2_SDK_SIG(rex::ui::RawImage::data, std::vector<uint8_t>, "RawImage::data");

// Audio backend and XMA kick.
CC2_SDK_BASE(rex::system::IAudioSystem, rex::audio::AudioSystem, "audio::AudioSystem");
struct AudioSystemProbe : rex::audio::AudioSystem {
  static void Check() {
    CC2_SDK_SIG(&AudioSystemProbe::CreateDriver,
                rex::X_STATUS (rex::audio::AudioSystem::*)(size_t, rex::thread::Semaphore*,
                                                           rex::audio::AudioDriver**),
                "AudioSystem::CreateDriver");
    CC2_SDK_SIG(&AudioSystemProbe::DestroyDriver, void (rex::audio::AudioSystem::*)(rex::audio::AudioDriver*),
                "AudioSystem::DestroyDriver");
    CC2_SDK_SIG(&AudioSystemProbe::Initialize, void (rex::audio::AudioSystem::*)(), "AudioSystem::Initialize");
  }
  // A "no matching constructor" error here means AudioSystem(FunctionDispatcher*) drifted.
  explicit AudioSystemProbe(rex::runtime::FunctionDispatcher* fd) : AudioSystem(fd) {}
};
CC2_SDK_SIG(&rex::audio::AudioSystem::SubmitFrame, void (rex::audio::AudioSystem::*)(size_t, uint32_t),
            "AudioSystem::SubmitFrame");
CC2_SDK_SIG(&rex::audio::AudioSystem::xma_decoder, rex::audio::XmaDecoder* (rex::audio::AudioSystem::*)() const,
            "AudioSystem::xma_decoder");
CC2_SDK_SIG(&rex::audio::AudioDriver::SubmitFrame, void (rex::audio::AudioDriver::*)(uint32_t),
            "AudioDriver::SubmitFrame");
CC2_SDK_SIG(&rex::audio::XmaDecoder::WriteRegister, void (rex::audio::XmaDecoder::*)(uint32_t, uint32_t),
            "XmaDecoder::WriteRegister");
CC2_SDK_SIG(rex::audio::XmaRegister::Context0Kick, const uint32_t, "XmaRegister::Context0Kick");
CC2_SDK_SIG(&rex::audio::GetStereoFold, rex::audio::StereoFold (*)(), "audio::GetStereoFold");
CC2_SDK_SIG(&rex::audio::GetSurroundMix, rex::audio::SurroundMix (*)(), "audio::GetSurroundMix");
CC2_SDK_SIG(&rex::audio::GetOutputGain, float (*)(), "audio::GetOutputGain");
CC2_SDK_SIG(&rex::chrono::Clock::guest_time_scalar, double (*)(), "chrono::Clock::guest_time_scalar");

// Input.
CC2_SDK_SIG(&rex::input::InputSystem::GetState,
            rex::X_RESULT (rex::input::InputSystem::*)(uint32_t, rex::input::X_INPUT_STATE*), "InputSystem::GetState");
CC2_SDK_SIG(rex::input::X_INPUT_STATE::gamepad, rex::input::X_INPUT_GAMEPAD, "X_INPUT_STATE::gamepad");
CC2_SDK_SIG(rex::input::X_INPUT_STATE::packet_number, rex::be<uint32_t>, "X_INPUT_STATE::packet_number");
CC2_SDK_SIG(rex::input::X_INPUT_GAMEPAD::buttons, rex::be<uint16_t>, "X_INPUT_GAMEPAD::buttons");
static_assert(sizeof(rex::input::X_INPUT_GAMEPAD) == 12, "SDK drift: X_INPUT_GAMEPAD layout changed");

// Logging, cvars, host helpers.
CC2_SDK_SIG(&rex::FlushLogging, void (*)(), "rex::FlushLogging");
CC2_SDK_SIG(&rex::cvar::LoadConfig, void (*)(const std::filesystem::path&), "cvar::LoadConfig");
CC2_SDK_SIG(&rex::filesystem::GetExecutableFolder, std::filesystem::path (*)(), "filesystem::GetExecutableFolder");
CC2_SDK_SIG(&rex::filesystem::GetUserFolder, std::filesystem::path (*)(), "filesystem::GetUserFolder");
CC2_SDK_SIG(&rex::thread::MaybeYield, void (*)(), "thread::MaybeYield");
CC2_SDK_SIG(&rex::arch::ExceptionHandler::Install,
            void (*)(rex::arch::ExceptionHandler::Handler, void*), "arch::ExceptionHandler::Install");
static_assert(std::is_same_v<rex::arch::ExceptionHandler::Handler, bool (*)(rex::arch::Exception*, void*)>,
              "SDK drift: arch::ExceptionHandler::Handler changed");

}  // namespace cc2::sdk_compat
