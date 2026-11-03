// cc2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include "cc2/kernel/xam/apps/xlivebase_app.h"  // InitializeTitleKernel
#include <rex/platform.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include <jmlive/jmlive.h>
#include <rexglue_guide/guide.h>

#include "cc2/scene_driver.h"
#include "cc2/display_settings.h"
#include "cc2/freecam.h"
#include "cc2/achievement_catalog.h"
#include "cc2/online.h"
#include "cc2/lobby.h"
#include "cc2/mouse_ui.h"
#include "cc2/keyboard_bindings.h"
#include "gpu/gpu.h"
#include "gpu/gpu_overlay.h"
#include "kernel/export_override.h"
#include "cc2/kernel/xam/input_assignment.h"
#include "audio/xma_kick.h"
#include "kernel/content_seed.h"
#include "kernel/vfs_cull.h"

#if REX_PLATFORM_WIN32
#include "cc2/audio/xaudio2/xaudio2_audio_system.h"
#endif

REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, log_file);
REXCVAR_DECLARE(std::string, user_name);
REXCVAR_DECLARE(std::string, cc2_server_url);
REXCVAR_DECLARE(bool, online_mode);

// sentient.cpp: commit the guest null page + install read-fault tolerance
// (the boot-race guard). Idempotent.
void DCC2_CommitGuestNullPage();

// Minimal-render cvars (sentient.cpp).
REXCVAR_DECLARE(bool, dcc2_minimal_render);
REXCVAR_DECLARE(std::string, dcc2_performance_cull);

REXCVAR_DEFINE_STRING(dump_image_path, "", "CC2",
                      "Write the decrypted, patched guest image to this path and exit.");

class Cc2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Cc2App>(new Cc2App(ctx, "cc2", PPCImageConfig));
  }

  // The runtime resolves paths before it loads the config, so a toml-set game_data_root is read
  // here first; its later reload reapplies the same values.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    paths.config_path =
        rex::filesystem::GetExecutableFolder() / "crashcourse2.toml";
    if (std::filesystem::exists(paths.config_path)) {
      rex::cvar::LoadConfig(paths.config_path);
    }
    // Installed builds (JMinstaller sets user_data_root) log to crashcourse2.log beside the exe, flushed each second.
    if (!REXCVAR_GET(user_data_root).empty()) {
      if (REXCVAR_GET(log_file).empty()) {
        rex::cvar::SetFlagByName("log_file", (rex::filesystem::GetExecutableFolder() / "crashcourse2.log").string());
      }
      if (rex::cvar::GetFlagSource("log_flush_interval") == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName("log_flush_interval", "1");
      }
    }
    // A named log file gets one fresh log per launch, no rotation history.
    const std::string log = REXCVAR_GET(log_file);
    if (!log.empty()) {
      std::error_code ec;
      std::filesystem::remove(log, ec);
      const std::filesystem::path rotated = std::filesystem::path(log).replace_extension(".1.log");
      std::filesystem::remove(rotated, ec);
    }
    std::string game_root = REXCVAR_GET(game_data_root);
    if (!game_root.empty()) {
      std::filesystem::path p = game_root;
      if (p.is_relative()) {
        p = rex::filesystem::GetExecutableFolder() / p;
      }
      paths.game_data_root = p.lexically_normal();
    }
    // JMinstaller's layout, which dev builds default to: user data in userdata\Crash Course 2, the avatar shared in
    // userdata\Avatar.
    const std::filesystem::path userdata = rex::filesystem::GetUserFolder() / "JMstudios" / "userdata";
    const std::string user_root = REXCVAR_GET(user_data_root);
    paths.user_data_root = user_root.empty() ? userdata / "Crash Course 2" : std::filesystem::path(user_root);
    for (const auto& [flag, folder] : {std::pair<const char*, const char*>{"avatar_data_dir", "Manifest"},
                                       {"avatar_cache_dir", "Cache"},
                                       {"avatar_closet_dir", "Closet"}}) {
      if (rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(flag, (userdata / "Avatar" / folder).string());
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // No emulated GPU: D3D runs recompiled against the title's edge layer and nothing reads the ring.
    config.graphics = nullptr;
    config.gpu_plugin.clear();
    // Identity settings shared by every ReXGlue title; loaded after the game toml so they win.
    const auto shared = user_data_root() / "user.toml";
    if (std::filesystem::exists(shared)) {
      rex::cvar::LoadConfig(shared);
    }
#if REX_PLATFORM_WIN32
    // Title XAudio2 backend in place of the SDK's SDL default; the FMOD
    // master-mix injection in sentient.cpp feeds it.
    config.audio_factory = REX_AUDIO_BACKEND(xaudio2::XAudio2AudioSystem);
#endif
    // kernel_init runs inside Runtime::Setup, the earliest point where
    // file_system() and the resolved game_data_root are both live. The title
    // kernel init mirrors the SDK's bring-up but registers the title's
    // XLiveBaseApp for app id 0xFC (AppManager keeps the first registration
    // for an id, so the SDK default is replaced, not wrapped).
    config.kernel_init = [](rex::Runtime* runtime, rex::system::KernelState* kernel_state) {
      cc2::PromoteTitleExports();
      cc2::SeedUserData(runtime);
      cc2::kernel::xam::apps::InitializeTitleKernel(runtime, kernel_state);
      if (REXCVAR_GET(dcc2_minimal_render)) {
        cc2::InstallVfsCull(runtime, REXCVAR_GET(dcc2_performance_cull));
      }
    };
  }

  // Detached mode: ImGui draws through the native renderer (gpu_overlay.h).
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return cc2::gpu::CreateOverlayDrawer();
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    rex::ReXApp::OnConfigureFonts(atlas);
    rexglue_guide::ConfigureFonts(atlas);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Installed builds (JMinstaller sets user_data_root) have no debug (F3) or settings (F4) overlay.
    if (!REXCVAR_GET(user_data_root).empty()) {
      for (const char* bind : {"bind_debug_overlay", "bind_settings"}) {
        if (rex::cvar::GetFlagSource(bind) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(bind, "");
      }
    }
    if (!game_data_root().empty()) {
      cc2::gpu::SetOverlayFontRoot(game_data_root() / "data" / "art" / "common" / "ui" / "fnt");
    }
    const std::filesystem::path userdata = rex::filesystem::GetUserFolder() / "JMstudios" / "userdata";
    AdoptSavedLogin(userdata / "JMlive" / "gamejolt-login.bin");
    jmlive::Configure({.gamertag = REXCVAR_GET(user_name),
                       .server_url = REXCVAR_GET(cc2_server_url),
                       .client_version = "2.0.0",  // ProductVersion in cc2.rc
                       .online = REXCVAR_GET(online_mode)});
    rexglue_guide::Install(this, drawer,
                           {.title_name = "Crash Course 2",
                            .data_dir = userdata / "JMlive",
                            .on_quit = [this] { OnWindowCloseRequested(); },
                            .immediate_drawer = immediate_drawer(),
                            .config_file = rex::filesystem::GetExecutableFolder() / "crashcourse2.toml"});
  }

  // Moves a legacy Game Jolt login (<user_data_root>\cc2, same sealed format) to JMlive's.
  void AdoptSavedLogin(const std::filesystem::path& jmlive_login) const {
    const std::filesystem::path old = user_data_root() / "cc2" / "gamejolt-login.bin";
    std::error_code ec;
    if (!std::filesystem::exists(old, ec) || std::filesystem::exists(jmlive_login, ec)) return;
    std::filesystem::create_directories(jmlive_login.parent_path(), ec);
    if (std::filesystem::copy_file(old, jmlive_login, ec)) std::filesystem::remove(old, ec);
  }

  // Game input is held while ImGui has the mouse; while the Guide is up, xam_input.cpp blanks it after the Guide has
  // each player's pad.
  void OnPostSetup() override {
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    cc2::kernel::xam::InstallInputAssignment(input);
    if (input && imgui_drawer()) {
      input->SetActiveCallback(
          [this] { return rexglue_guide::IsOpen() || !imgui_drawer()->GetIO().WantCaptureMouse; });
    }
  }

  // UI thread: records one ImGui frame for the renderer's next present.
  void DrawOverlay() {
    if (!imgui_drawer() || !window()) return;
    rex::ui::AppUIDrawContext context(window()->GetActualPhysicalWidth(), window()->GetActualPhysicalHeight());
    cc2::gpu::BeginOverlayFrame();
    imgui_drawer()->Draw(context);
    cc2::gpu::EndOverlayFrame();
  }

  // The Guide has its own unlock toasts and achievements page.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    return nullptr;
  }
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override { return nullptr; }

  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) {
      return false;
    }
    window()->SetTitle("Crash Course 2");
    if (!game_data_root().empty()) {
      cc2::gpu::SetOverlayFontRoot(game_data_root() / "data" / "art" / "common" / "ui" / "fnt");
    }
    cc2::keyboard::Initialize();
    // The device comes up here, before any guest thread exists, so the first Swap already has a swap chain.
    cc2::display::Initialize(window());
    cc2::freecam::Initialize(window());
    cc2::mouse::Initialize(window());
    cc2::lobby::Initialize(window());
    // One overlay frame request queued per present.
    cc2::gpu::SetOverlayFrameCallback([this] {
      if (overlay_queued_.exchange(true)) return;
      if (!app_context().CallInUIThread([this] {
            overlay_queued_ = false;
            DrawOverlay();
          })) {
        overlay_queued_ = false;
      }
    });
    cc2::gpu::Initialize(window());
    return true;
  }

  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override { cc2::gpu::RequestResize(); }

  // SDL quit callbacks hold the event-watch lock; closing there can deadlock against input polling.
  // Defer teardown and the final close until the UI loop releases that lock.
  bool OnWindowCloseRequested() override {
    if (!close_queued_) {
      close_queued_ = true;
      if (!app_context().CallInUIThreadDeferred([this] {
            cc2::online::Stop();
            cc2::lobby::Shutdown();
            cc2::mouse::Shutdown();
            cc2::display::Shutdown();
            cc2::freecam::Shutdown();
            cc2::gpu::SetOverlayFrameCallback(nullptr);
            cc2::gpu::Shutdown();
            window()->RequestClose();
          })) {
        close_queued_ = false;
        REXLOG_ERROR("Could not queue window close");
      }
    }
    return false;
  }

  // The dump loads in IDA as a raw big-endian PowerPC binary at PPCImageConfig.image_base.
  void OnPostLoadXexImage() override {
    const std::string out = REXCVAR_GET(dump_image_path);
    if (out.empty()) {
      return;
    }
    const auto* bytes = runtime()->memory()->TranslateVirtual<const uint8_t*>(
        PPCImageConfig.image_base);
    FILE* f = std::fopen(out.c_str(), "wb");
    if (f && bytes) {
      std::fwrite(bytes, 1, PPCImageConfig.image_size, f);
    }
    if (f) {
      std::fclose(f);
    }
    REXLOG_INFO("Dumped guest image to {} ({} bytes, base {:#x})", out,
                uint32_t(PPCImageConfig.image_size), uint32_t(PPCImageConfig.image_base));
    app_context().QuitFromUIThread();
  }

  void OnPreLaunchModule() override {
    // Kernel Dispatch can enter guest code on an unbuilt object within ~0.1s of launch, so the
    // null-page guard has to be armed before any guest thread exists.
    DCC2_CommitGuestNullPage();
    cc2::LogOverrideCheck(runtime());
    SetGuestFrameStats([] { return cc2::scene::GuestFrameStats(); });
    cc2::EnsureContentHeaders(runtime(), runtime()->kernel_state());
    rex::audio::StartXmaKicker();
    cc2::achievements::WriteCatalog(runtime());
    cc2::online::Start(runtime());
  }

  void OnShutdown() override {
    rexglue_guide::Shutdown();
    cc2::display::Shutdown();
    cc2::gpu::SetOverlayFrameCallback(nullptr);
    cc2::gpu::Shutdown();
    rex::audio::StopXmaKicker();
  }

 private:
  bool close_queued_ = false;
  std::atomic<bool> overlay_queued_{false};
};
