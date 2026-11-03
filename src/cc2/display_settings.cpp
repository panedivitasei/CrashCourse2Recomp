#include "cc2/display_settings.h"
#include "cc2/freecam.h"
#include "cc2/shop_grid.h"
#include "cc2/display_menu_lua.h"
#include "cc2/objective_hud_lua.h"
#include "dcc2_tu1_map.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "gpu/gpu.h"

REXCVAR_DECLARE(int32_t, gpu_resolution_scale);
REXCVAR_DECLARE(int32_t, gpu_shadow_resolution_scale);
REXCVAR_DECLARE(int32_t, gpu_fps_cap);
REXCVAR_DECLARE(bool, gpu_fps);
REXCVAR_DECLARE(bool, gpu_vsync);
// The SDK's own window_width and window_height are logical pixels; these are the physical ones the settings page lists.
REXCVAR_DEFINE_INT32(cc2_window_width, 0, "Display", "Windowed-mode width in pixels; 0 leaves the default window");
REXCVAR_DEFINE_INT32(cc2_window_height, 0, "Display", "Windowed-mode height in pixels; 0 leaves the default window");
REXCVAR_DEFINE_STRING(cc2_aspect_ratio, "16:9", "Display", "Picture aspect ratio: 4:3, 16:9 or 16:10");
REXCVAR_DEFINE_BOOL(cc2_hide_hud, false, "Display", "Hide the in-course HUD; menus and the pause menu stay");
REXCVAR_DEFINE_BOOL(cc2_close_up, false, "Display", "Close-up follow camera in courses");
extern "C" REX_FUNC(sub_824C8BF8);
extern "C" REX_FUNC(sub_824C9300);
extern "C" REX_FUNC(sub_824CA4C0);
extern "C" REX_FUNC(sub_822DCCA8);  // item grid selection and viewport
extern "C" REX_FUNC(sub_82B03230);  // shop preview and action refresh

namespace cc2::display {
namespace {
struct Settings {
  int scale=100, shadow=100, aspect=1, resolution=4, fullscreen=0, limit=60, vsync=0, fps=0, hide_hud=0, close_up=0, freecam=0;
};
constexpr std::array<std::array<int,2>,11> kSizes{{{640,480},{800,600},{1024,768},{1280,800},
    {1280,720},{1600,900},{1680,1050},{1920,1080},{1920,1200},{2560,1440},{2560,1600}}};
Settings current, original;
std::atomic<int> scale{100}, shadow{100}, aspect{1}, limit{60}, focus{0}, action{0}, navigation{0};
std::atomic<bool> fps_counter{false};
std::atomic<int> mouse_active{0};
std::atomic<bool> mouse_moved{false};
std::atomic<int> scrolling{0}, clicked_row{0}, expanded_button{0};
std::atomic<float> scroll_to{-1};
int published_mouse=-1;  // CC2MouseHover as the installed script last saw it
std::atomic<rex::ui::Window*> window{nullptr};
bool in_menu=false;
bool slider_window_pending=false;
uint32_t installed_state=0;
uint32_t menu_state=0;
std::filesystem::path settings_path;
std::mutex input_mutex;
uint16_t previous_buttons=0;
int previous_direction=0;
int previous_vertical=0;
std::chrono::steady_clock::time_point repeat_at;
std::chrono::steady_clock::time_point vertical_repeat_at;

uint32_t Read(uint32_t p) {
  return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint32_t>*>(p);
}
void Write(uint32_t p, uint32_t v) {
  *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(p)=v;
}
void Clamp(Settings& s) {
  s.scale=std::clamp(s.scale,20,200);
  s.shadow=std::clamp(s.shadow,50,400);
  s.aspect=std::clamp(s.aspect,0,2);
  s.resolution=std::clamp(s.resolution,0,int(kSizes.size()-1));
  s.fullscreen=!!s.fullscreen;
  s.limit=std::clamp(s.limit,30,144);
  s.vsync=!!s.vsync;
  s.hide_hud=!!s.hide_hud;
  s.freecam=!!s.freecam;
  s.close_up=!!s.close_up;
  s.fps=!!s.fps;
}
void Publish(bool change_window) {
  Clamp(current);
  scale.store(current.scale);
  shadow.store(current.shadow);
  aspect.store(current.aspect);
  limit.store(current.limit);
  fps_counter.store(current.fps!=0);
  cc2::gpu::SetVsync(current.vsync!=0);
  if (change_window) {
    auto* w=window.load();
    if (w) {
      const Settings s=current;
      w->app_context().CallInUIThreadDeferred([w,s] {
        if (window.load()!=w) return;
        if (w->IsFullscreen()!=(s.fullscreen!=0)) w->SetFullscreen(s.fullscreen!=0);
        if (!s.fullscreen) {
          w->SetDesiredLogicalSize(w->SizeToLogical(kSizes[s.resolution][0]),
                                  w->SizeToLogical(kSizes[s.resolution][1]));
        }
      });
    }
  }
}
constexpr const char* kRatios[]={"4:3","16:9","16:10"};
// The end of a toml value on a key line: the first '#' outside quotes, with the spaces before it.
size_t ValueEnd(const std::string& line,size_t from) {
  char quote=0;
  size_t end=line.size();
  for (size_t i=from;i<line.size();++i) {
    const char c=line[i];
    if (quote) {
      if (c=='\\' && quote=='"') ++i;
      else if (c==quote) quote=0;
    } else if (c=='"' || c=='\'') quote=c;
    else if (c=='#') {end=i;break;}
  }
  while (end>from && (line[end-1]==' ' || line[end-1]=='\t')) --end;
  return end;
}
// Saves the settings into the config the game boots from. Only their own lines change, so comments and every
// other key stay as the owner wrote them; a key that is not there yet is appended.
void Save() {
  const std::pair<std::string,std::string> values[]={
      {"gpu_resolution_scale",std::to_string(current.scale)},
      {"gpu_shadow_resolution_scale",std::to_string(current.shadow)},
      {"cc2_aspect_ratio",std::string("\"")+kRatios[current.aspect]+"\""},
      {"cc2_window_width",std::to_string(kSizes[current.resolution][0])},
      {"cc2_window_height",std::to_string(kSizes[current.resolution][1])},
      {"fullscreen",current.fullscreen ? "true" : "false"},
      {"gpu_fps_cap",std::to_string(current.limit)},
      {"gpu_vsync",current.vsync ? "true" : "false"},
      {"gpu_fps",current.fps ? "true" : "false"},
      {"cc2_hide_hud",current.hide_hud ? "true" : "false"},
      {"cc2_close_up",current.close_up ? "true" : "false"}};
  std::string text;
  {
    std::ifstream in(settings_path,std::ios::binary);
    if (in) text.assign(std::istreambuf_iterator<char>(in),{});
  }
  const std::string eol=text.find("\r\n")!=std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  for (size_t at=0;at<text.size();) {
    const size_t end=text.find('\n',at);
    std::string line=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end==std::string::npos) break;
    at=end+1;
  }
  std::vector<std::string> added;
  for (const auto& [key,value] : values) {
    bool found=false;
    for (auto& line : lines) {
      const size_t start=line.find_first_not_of(" \t");
      if (start==std::string::npos || line.compare(start,key.size(),key)!=0) continue;
      const size_t equals=line.find_first_not_of(" \t",start+key.size());
      if (equals==std::string::npos || line[equals]!='=') continue;
      const size_t from=std::min(line.find_first_not_of(" \t",equals+1),line.size());
      line=line.substr(0,from)+(from==equals+1 ? " " : "")+value+line.substr(ValueEnd(line,from));
      found=true;
    }
    if (!found) added.push_back(key+" = "+value);
  }
  if (!added.empty()) {
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    lines.push_back("");
    lines.push_back("# Display settings, saved by the in-game settings page.");
    for (auto& line : added) lines.push_back(std::move(line));
  }
  const auto temporary=settings_path.string()+".tmp";
  {
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    for (const auto& line : lines) out<<line<<eol;
    if (!out) { REXLOG_ERROR("[display] cannot save settings"); return; }
  }
  std::error_code ec;
  std::filesystem::copy_file(temporary,settings_path,std::filesystem::copy_options::overwrite_existing,ec);
  if (ec) REXLOG_ERROR("[display] cannot save settings: {}",ec.message());
  else std::filesystem::remove(temporary,ec);
}
int Lua(PPCContext& ctx,uint8_t* base,uint32_t L,const char* source,bool function=false,
        uint32_t* thread=nullptr,uint32_t* userdata=nullptr) {
  const uint32_t saved_top_offset=Read(L+8)-Read(L+32);
  const PPCContext saved=ctx;
  const uint32_t length=uint32_t(std::strlen(source));
  const uint32_t address=(ctx.r1.u32-length-0x200)&~15u;
  std::memcpy(base+address,source,length+1);
  ctx.r1.u32=address-0x80;
  ctx.r3.u64=L;
  if (function) {
    ctx.r4.s64=-10002;
    ctx.r5.u64=address;
    sub_824C8BF8(ctx,base);
    ctx.r3.u64=0;
  } else {
    ctx.r4.u64=address;
    ctx.r5.u64=length;
    sub_824CA4C0(ctx,base);
  }
  int result=-1;
  if (ctx.r3.u32==0) {
    ctx.r3.u64=L;ctx.r4.u64=0;ctx.r5.u64=1;ctx.r6.u64=0;
    sub_824C9300(ctx,base);
    if (ctx.r3.u32==0) {
      const uint32_t value=Read(L+8)-16;
      if (thread && Read(value+8)==8) *thread=Read(value);
      if (userdata && Read(value+8)==7 && Read(Read(value)+16)>=4) *userdata=Read(Read(value)+24);
      result=Read(value+8)==3 ? int(*reinterpret_cast<const rex::be<double>*>(base+value)) : 0;
    } else {
      static int errors=0;
      if (errors++<8) {
        const uint32_t value=Read(L+8)-16;
        const uint32_t text=Read(value);
        if (Read(value+8)==4 && text) REXLOG_WARN("[display] Lua: {}",std::string(reinterpret_cast<char*>(base+text+16),std::min(Read(text+12),512u)));
      }
    }
  }
  ctx=saved;
  Write(L+8,Read(L+32)+saved_top_offset);
  return result;
}
void Values(PPCContext& ctx,uint8_t* base,uint32_t L) {
  char text[512];
  std::snprintf(text,sizeof(text),"CC2DisplayValues={'%d%%','%d%%','%s','%d x %d','%s','%d FPS','%s','%s','%s','%s','%s'};"
      "CC2DisplayFractions={%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f}",
      current.scale,current.shadow,kRatios[current.aspect],kSizes[current.resolution][0],kSizes[current.resolution][1],
      current.fullscreen?"ON":"OFF",current.limit,current.vsync?"ON":"OFF",current.fps?"ON":"OFF",
      current.hide_hud?"ON":"OFF",current.close_up?"ON":"OFF",current.freecam?"ON":"OFF",
      (current.scale-20)/180.0,(current.shadow-50)/350.0,current.aspect/2.0,current.resolution/double(kSizes.size()-1),
      double(current.fullscreen),(current.limit-30)/114.0,double(current.vsync),double(current.fps),double(current.hide_hud),
      double(current.close_up),double(current.freecam));
  Lua(ctx,base,L,text);
}
}

void Initialize(rex::ui::Window* w) {
  window.store(w);
  settings_path=rex::filesystem::GetExecutableFolder()/"crashcourse2.toml";
  current.scale=REXCVAR_GET(gpu_resolution_scale);
  current.shadow=REXCVAR_GET(gpu_shadow_resolution_scale);
  current.limit=REXCVAR_GET(gpu_fps_cap)>0 ? REXCVAR_GET(gpu_fps_cap) : 144;
  current.fullscreen=w->IsFullscreen();
  current.vsync=REXCVAR_GET(gpu_vsync) ? 1 : 0;
  current.hide_hud=REXCVAR_GET(cc2_hide_hud) ? 1 : 0;
  current.freecam=0;  // the free camera is a per-course tool and never starts on
  current.close_up=REXCVAR_GET(cc2_close_up) ? 1 : 0;
  current.fps=REXCVAR_GET(gpu_fps) ? 1 : 0;
  for (int i=0;i<3;++i) if (REXCVAR_GET(cc2_aspect_ratio)==kRatios[i]) current.aspect=i;
  bool sized=false;
  for (size_t i=0;i<kSizes.size();++i) {
    if (kSizes[i][0]!=REXCVAR_GET(cc2_window_width) || kSizes[i][1]!=REXCVAR_GET(cc2_window_height)) continue;
    current.resolution=int(i);
    sized=true;
  }
  // Legacy display-settings.txt: imported into the config once, then deleted.
  const auto legacy=rex::filesystem::GetExecutableFolder()/"display-settings.txt";
  std::ifstream in(legacy);
  Settings loaded;
  const bool migrate=bool(in>>loaded.scale>>loaded.aspect>>loaded.resolution>>loaded.fullscreen>>loaded.limit);
  if (migrate) {
    // A five-field file has no V-Sync field; vsync then keeps the cvar's value.
    loaded.vsync=current.vsync;
    in>>loaded.vsync;
    in.close();
    loaded.hide_hud=current.hide_hud;
    loaded.close_up=current.close_up;
    loaded.fps=current.fps;
    current=loaded;
    Clamp(current);
    Save();
    std::error_code ec;
    std::filesystem::remove(legacy,ec);
    REXLOG_INFO("[display] moved display-settings.txt into crashcourse2.toml");
  }
  Publish(migrate || sized);
}
void Shutdown() {window.store(nullptr);focus.store(0);}
int ResolutionScale() {return scale.load();}
int ShadowScale() {return shadow.load();}
int FrameLimit() {return limit.load();}
bool FpsCounter() {return fps_counter.load();}
float AspectRatio() {const int a=aspect.load();return a==0 ? 4.0f/3.0f : a==2 ? 1.6f : 16.0f/9.0f;}

void MouseActive(bool active) { mouse_active.store(active ? 1 : 0); }
void MouseMoved() { mouse_moved.store(true); }
int FocusRow() { return focus.load(); }
void MouseWheel(PPCContext& ctx, uint8_t* base, int steps) {
  if (!steps) return;
  if (ShopScroll(ctx,base,-steps)) return;
  if (focus.load()>=100) scrolling.fetch_add(-steps);
  else ScrollText(ctx,base,-steps*0.08f,true);
}
void MouseScrollTo(float value) { scroll_to.store(std::clamp(value,0.0f,1.0f)); }
int ExpandedMenuButton() { return expanded_button.load(); }
void ScrollText(PPCContext& ctx, uint8_t* base, float value, bool relative) {
  if (!menu_state) return;
  char text[128];
  std::snprintf(text,sizeof(text),"CC2ScrollValue=%.6f;CC2ScrollRelative=%s;return CC2ScrollText()",
                value,relative ? "true" : "false");
  Lua(ctx,base,menu_state,text);
}

bool MainMenuPointer(PPCContext& ctx, uint8_t* base, int button, bool click) {
  if (!menu_state) return false;
  char text[128];
  std::snprintf(text,sizeof(text),"CC2MainMenuButton=%d;CC2MainMenuClick=%s;return CC2MainMenuPointer()",
                button,click ? "true" : "false");
  return Lua(ctx,base,menu_state,text)>0;
}

bool CoursePointer(PPCContext& ctx, uint8_t* base, float x, float y, bool click) {
  if (!menu_state) return false;
  char text[160];
  std::snprintf(text,sizeof(text),"CC2CourseX=%.9g;CC2CourseY=%.9g;CC2CourseClick=%s;return CC2CoursePointer()",
                x,y,click ? "true" : "false");
  const int target=Lua(ctx,base,menu_state,text);
  return target>0;
}

namespace {
uint32_t ShopViewer(PPCContext& ctx, uint8_t* base) {
  uint32_t viewer = 0;
  if (menu_state) Lua(ctx,base,menu_state,"return CC2ShopViewer()",false,nullptr,&viewer);
  return viewer;
}
ShopGrid ReadShopGrid(uint32_t viewer) {
  const uint32_t grid=viewer+8;
  const uint32_t items=Read(grid);
  return {items ? int(Read(items+16)) : 0,int(Read(grid+16)),int(Read(grid+20)),
          int(Read(grid+8)),int(Read(grid+12))};
}
void SelectShopGrid(PPCContext& ctx, uint8_t* base, uint32_t viewer, const ShopGrid& grid) {
  const PPCContext saved=ctx;
  ctx.r3.u64=viewer+8;ctx.r4.u64=grid.selected;ctx.r5.u64=grid.first;
  sub_822DCCA8(ctx,base);
  ctx.r3.u64=viewer;
  sub_82B03230(ctx,base);
  ctx=saved;
}
}

bool ShopScroll(PPCContext& ctx, uint8_t* base, int rows) {
  const uint32_t viewer=ShopViewer(ctx,base);
  if (!viewer) return false;
  auto grid=ReadShopGrid(viewer);
  if (grid.Scroll(rows)) SelectShopGrid(ctx,base,viewer,grid);
  return true;
}

bool ShopPointer(PPCContext& ctx, uint8_t* base, float x, float y, std::string_view tab,
                 uint32_t control, bool click) {
  if (!menu_state) return false;
  constexpr std::string_view names[] = {"featured", "rewinds", "powerups", "jinxes", "fx", "course"};
  int index = 0;
  for (int i = 0; i < 6; ++i) if (tab == names[i]) index = i + 1;
  char text[192];
  std::snprintf(text, sizeof(text), "CC2ShopX=%.9g;CC2ShopY=%.9g;CC2ShopTab=%d;CC2ShopClick=%s;return CC2ShopPointer()",
                x, y, index, click ? "true" : "false");
  const int target = Lua(ctx, base, menu_state, text);
  if (target > 0) {
    if (click && target>=9) ShopScroll(ctx,base,target==9 ? -1 : 1);
    return true;
  }
  if (!control) return false;
  const uint32_t viewer=ShopViewer(ctx,base);
  if (!viewer) return false;
  auto grid=ReadShopGrid(viewer);
  if (!grid.Valid()) return false;
  for (int slot=0;slot<std::min(grid.capacity,grid.total-grid.first);++slot) {
    const uint32_t handle=viewer+8+32+slot*8;
    if (Read(handle)!=Read(control+16) || Read(handle+4)!=Read(control+20)) continue;
    if (grid.selected!=slot) {
      grid.selected=slot;
      SelectShopGrid(ctx,base,viewer,grid);
    }
    if (click) Lua(ctx,base,menu_state,"local v=CC2ShopViewer();if v then v:DoAAction() end");
    return true;
  }
  return false;
}

bool MouseSlider(PPCContext& ctx, uint8_t* base, int row, float fraction) {
  if (!in_menu || !menu_state || row<1 || row>14 || !std::isfinite(fraction)) return false;
  fraction=std::clamp(fraction,0.0f,1.0f);
  char text[160];
  std::snprintf(text,sizeof(text),"CC2SliderRow=%d;CC2SliderValue=%.9g;return CC2DisplaySetSlider()",row,fraction);
  if (Lua(ctx,base,menu_state,text)<=0) return false;
  if (row<=3) return true;
  int* value=nullptr;
  int target=0;
  switch (row) {
    case 4:value=&current.scale;target=20+5*int(std::lround(fraction*36));break;
    case 5:value=&current.shadow;target=50+25*int(std::lround(fraction*14));break;
    case 6:value=&current.aspect;target=int(std::lround(fraction*2));break;
    case 7:value=&current.resolution;target=int(std::lround(fraction*(kSizes.size()-1)));break;
    case 8:value=&current.fullscreen;target=fraction>=0.5f;break;
    case 9:value=&current.limit;target=30+int(std::lround(fraction*114));break;
    case 10:value=&current.vsync;target=fraction>=0.5f;break;
    case 11:value=&current.fps;target=fraction>=0.5f;break;
    case 12:value=&current.hide_hud;target=fraction>=0.5f;break;
    case 13:value=&current.close_up;target=fraction>=0.5f;break;
    case 14:value=&current.freecam;target=fraction>=0.5f;break;
  }
  if (*value!=target) {
    *value=target;
    // Keep the drag geometry fixed until release when changing the window or image shape.
    if (row>=6 && row<=8) slider_window_pending=true;
    else Publish(false);
    Values(ctx,base,menu_state);
  }
  return true;
}
void MouseSliderEnd() {
  if (!slider_window_pending) return;
  slider_window_pending=false;
  Publish(true);
}

bool MouseClick(int row) {
  if (focus.load()<100 || row<1 || row>11) return false;
  clicked_row.store(row+3);
  action.store(row);
  return true;
}

void FilterInput(uint32_t user,rex::input::X_INPUT_GAMEPAD* pad) {
  if (user || !pad) return;
  std::lock_guard lock(input_mutex);
  const int selected=focus.load();
  const uint16_t buttons=pad->buttons;
  const uint16_t pressed=buttons&~previous_buttons;
  previous_buttons=buttons;
  if (selected<100) {previous_direction=0;previous_vertical=0;return;}
  const auto now=std::chrono::steady_clock::now();
  const int vertical=(buttons&2 || int16_t(pad->thumb_ly)<-16000) ? 1 :
                     (buttons&1 || int16_t(pad->thumb_ly)>16000) ? -1 : 0;
  if (vertical && (vertical!=previous_vertical || now>=vertical_repeat_at)) {
    navigation.fetch_add(vertical);
    vertical_repeat_at=now+std::chrono::milliseconds(vertical!=previous_vertical ? 350 : 150);
  }
  previous_vertical=vertical;
  pad->buttons=buttons&~uint16_t(3);
  pad->thumb_ly=0;
  if (pressed&0x1000) action.store(100);
  if (pressed&0x2000) action.store(101);
  if (pressed&0x8000) action.store(102);
  if (selected>100) {
    const int direction=(buttons&8 || int16_t(pad->thumb_lx)>16000) ? 1 :
                        (buttons&4 || int16_t(pad->thumb_lx)<-16000) ? -1 : 0;
    if (direction && (direction!=previous_direction || now>=repeat_at)) {
      action.store((selected-100)*direction);
      repeat_at=now+std::chrono::milliseconds(direction!=previous_direction ? 350 : 100);
    }
    previous_direction=direction;
    pad->buttons=uint16_t(pad->buttons)&~uint16_t(12);
    pad->thumb_lx=0;
  } else previous_direction=0;
}

void Tick(PPCContext& ctx,uint8_t* base) {
  const uint32_t interp=Read(kGA_82EB7CD0);
  const uint32_t main_state=interp ? Read(interp+8) : 0;
  if (!main_state) return;
  if (installed_state!=main_state) {
    menu_state=0;
    if (Lua(ctx,base,main_state,
            "CC2DisplayThread=coroutine.create(function() end); return CC2DisplayThread",
            false,&menu_state)<0 || !menu_state) return;
    if (Lua(ctx,base,menu_state,kMenuScript)<0) return;
    if (Lua(ctx,base,menu_state,kObjectiveScript)<0) return;
    REXLOG_DEBUG("[objectives] native tracker callbacks installed");
    installed_state=main_state;
    published_mouse=-1;  // a fresh Lua state has no CC2MouseHover until it is published again
    REXLOG_DEBUG("[display] native settings script installed");
    Values(ctx,base,menu_state);
  }
  const uint32_t L=menu_state;
  const int objective_event=Lua(ctx,base,L,"CC2ObjectiveTick",true);
  Lua(ctx,base,L,"CC2TitleSlotsTick",true);
  Lua(ctx,base,L,"CC2MainTipsTick",true);
  // The HUD's layer is hidden only while a course runs; the pause menu and messages sit on other layers. The same
  // call reports the course, pause and single-player state the free camera keys off.
  const int flags=Lua(ctx,base,L,current.hide_hud ? "return CC2HudTick(true)" : "return CC2HudTick(false)");
  const bool course=flags>0 && (flags&4);
  static bool was_course=false;
  if (was_course && !course && current.freecam) {
    // The free camera is for the course it was switched on in; the row reads OFF again afterwards.
    current.freecam=0;
    original.freecam=0;
    Values(ctx,base,L);
  }
  was_course=course;
  cc2::freecam::Tick(current.freecam!=0,current.close_up!=0,course,flags>0 && (flags&8));
  if (objective_event>0) {
    constexpr const char* events[]={"", "progress", "failed", "restored", "completed"};
    REXLOG_INFO("[objectives] HUD {} notification",events[std::min(objective_event,4)]);
  }
  const int command=action.exchange(0);
  if (command && in_menu) {
    if (command==100) Save();
    else if (command==101) {current=original;Publish(true);}
    else if (command==102) {current=Settings{};Publish(true);}
    else {
      const int direction=command>0 ? 1 : -1;
      switch (std::abs(command)) {
        case 1:current.scale+=direction*5;break;
        case 2:current.shadow+=direction*25;break;
        case 3:current.aspect=(current.aspect+3+direction)%3;break;
        case 4:current.resolution=(current.resolution+int(kSizes.size())+direction)%int(kSizes.size());break;
        case 5:current.fullscreen=!current.fullscreen;break;
        case 6:current.limit+=direction;break;
        case 7:current.vsync=!current.vsync;break;
        case 8:current.fps=!current.fps;break;
        case 9:current.hide_hud=!current.hide_hud;break;
        case 10:current.close_up=!current.close_up;break;
        case 11:current.freecam=!current.freecam;break;
      }
      Publish(std::abs(command)==4 || std::abs(command)==5);
      Lua(ctx,base,L,"CC2DisplayChanged=true");
    }
    Values(ctx,base,L);
    REXLOG_INFO("[display] scale {} shadows {} aspect {} window {}x{} fullscreen {} limit {} vsync {} hide hud {} close up {} freecam {}",
                current.scale,current.shadow,current.aspect,kSizes[current.resolution][0],kSizes[current.resolution][1],
                current.fullscreen,current.limit,current.vsync,current.hide_hud,current.close_up,current.freecam);
  }
  const int mouse=mouse_active.load();
  if (mouse!=published_mouse) {
    published_mouse=mouse;
    Lua(ctx,base,L,mouse ? "CC2MouseHover=1" : "CC2MouseHover=0");
  }
  const bool moved=mouse_moved.exchange(false);
  const int click=clicked_row.exchange(0);
  if (click && in_menu) {
    const std::string script="CC2DisplaySelect="+std::to_string(click);
    Lua(ctx,base,L,script.c_str());
  }
  const int move=navigation.exchange(0);
  const int scroll=scrolling.exchange(0);
  const float target=scroll_to.exchange(-1);
  if (in_menu && (scroll || target>=0)) {
    const std::string script="CC2DisplayScroll="+std::to_string(scroll)+
        ";CC2DisplayScrollTo="+(target>=0 ? std::to_string(target) : "nil");
    Lua(ctx,base,L,script.c_str());
  }
  if (moved && mouse && in_menu && !move && !scroll && target<0) Lua(ctx,base,L,"CC2MouseMoved=true");
  if (move && in_menu) {
    const std::string script="CC2DisplayMove="+std::to_string(move);
    Lua(ctx,base,L,script.c_str());
  }
  expanded_button.store(std::max(0,Lua(ctx,base,L,"CC2ExpandedMenuButton",true)));
  const int selected=Lua(ctx,base,L,"CC2DisplayTick",true);
  if (selected<0) return;
  focus.store(std::max(selected,0));
  if (selected>=100 && !in_menu) original=current;
  in_menu=selected>=100;
}
}
