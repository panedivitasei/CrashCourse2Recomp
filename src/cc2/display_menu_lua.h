#pragma once
namespace cc2::display {
inline constexpr char kMenuScript[] = R"lua(
local cc2_selected=1
local cc2_first=1
local cc2_scroll_position=0
local cc2_layout_pending=true
local cc2_mouse=false
local function cc2_course_visible(c)
  while c do
    if c.Visible==false or c.Visible==0 or c.Enabled==false or c.Enabled==0 then return false end
    c=c.parent
  end
  return true
end
local function cc2_course_hit(c,x,y,width,height)
  if not c or not cc2_course_visible(c) or c.GetAbsOpacity()<=0 then return false end
  local p=c.GetAbsScreenPos()
  local s=c.GetAbsSize()
  return math.abs(x-p.x)<=width*math.abs(s.x)/2 and math.abs(y-p.y)<=height*math.abs(s.y)/2
end
local function cc2_course_call(root,callback)
  -- Zone callbacks read the script's implicit owner as well as their self argument.
  local env=getfenv(callback)
  local previous=rawget(env,"me")
  rawset(env,"me",root)
  local ok,message=pcall(callback,root)
  rawset(env,"me",previous)
  if not ok then error(message,0) end
end
function CC2CoursePointer()
  if not gui or (gui.pause and gui.pause.window) or (guimessage and guimessage.IsAnyMessageDisplayed()) then return 0 end
  local root=gui.screens
  if not root or not root.items or not root.viewer or not root.NextCat or not root.PrevCat or
      root.CurrentCourse==nil or confirmVisible or not cc2_course_visible(root) then return 0 end
  local x,y=CC2CourseX,CC2CourseY
  local click=CC2CourseClick
  -- Native picture dimensions follow each control's current transform, including menu transitions.
  if cc2_course_hit(root.but_left,x,y,0.0488,0.0651) then
    if click then cc2_course_call(root,root.PrevCat) end
    return 8
  end
  if cc2_course_hit(root.but_right,x,y,0.0488,0.0651) then
    if click then cc2_course_call(root,root.NextCat) end
    return 9
  end
  local target,selected
  for i=1,7 do
    local item=root.items["item"..i]
    if item then
      if item.IsSelected then selected=i end
      local bg=item.unlock
      if not bg or not cc2_course_visible(bg) then bg=item.block_bg end
      -- Adjacent rows are 0.061 units apart; omit the texture's transparent vertical padding.
      if cc2_course_visible(item) and cc2_course_hit(bg,x,y,0.5333,0.061) then target=i end
    end
  end
  if not target or not selected then return 0 end
  local item=root.items["item"..target]
  -- Navigate the viewer so its launch target, highlight and course preview agree.
  for i=1,math.abs(target-selected) do
    if target>selected then root:NextLevel() else root:PrevLevel() end
  end
  if not item.IsSelected then return 0 end
  if click then root.RunCmd(1) end
  return target
end
local cc2_main_hovered=nil
local cc2_main_options=nil
function CC2MainMenuPointer()
  if CC2MainMenuButton==-1 then cc2_main_hovered=nil;cc2_main_options=nil;return 0 end
  local options=gui and gui.main and gui.main.options
  if not options or not cc2_course_visible(options) or confirmVisible or
      (guimessage and guimessage.IsAnyMessageDisplayed()) then
    cc2_main_hovered=nil
    cc2_main_options=nil
    return 0
  end
  if options~=cc2_main_options then cc2_main_hovered=nil;cc2_main_options=options end
  local index=CC2MainMenuButton
  local target=(index==1 or index==2) and options["but"..index] or nil
  if target and not cc2_course_visible(target) then target=nil end
  if cc2_main_hovered~=target then
    local previous=cc2_main_hovered
    cc2_main_hovered=target
    if previous then
      previous.but_border.Visible=false
      previous.tab.Value=0
      if not previous.clicked then previous.sel.B2FadeTo(1,0,0.2) end
    end
    if target then
      -- Keep native highlight animation while hover leaves the open submenu intact.
      local env=getfenv(tab_large_mouseover)
      local owner=rawget(env,"me")
      rawset(env,"me",target)
      local ok,message=pcall(tab_large_mouseover,2)
      rawset(env,"me",owner)
      if not ok then error(message,0) end
      PlaySound("CC2/Menus/SFX_Menu_Move")
      g_mainMenuBut=index
      g_mainMenuSub=0
    end
  end
  if not target or not CC2MainMenuClick then return 0 end
  if target.clicked and g_showSubOptions==index then return 1 end
  if g_showSubOptions and g_showSubOptions~=0 then
    RestoreButtonTextState()
    for i=1,2 do
      local c=options["but"..i]
      c.clicked=false
      if c~=target then c.sel.B2FadeTo(1,0,0.2) end
    end
    for i=1,4 do
      local name="sub_"..i
      options.ReplaceControl(name,"GGRP:common/ui/men/opt/empty")
      options[name].Visible=false
    end
    g_showSubOptions=0
    g_clickedOnMainMenu=false
  end
  return 0
end
local function cc2_shop_root()
  if not gui or confirmVisible or (guimessage and guimessage.IsAnyMessageDisplayed()) then return nil end
  local root=gui.screens
  if gui.pause and gui.pause.window then root=gui.pause.window end
  if not root or not root.options or not root.options.featured or not root.viewer or
      not root.Cats or type(root.Cat)~="number" or root.Cat<1 or root.Cat>#root.Cats or
      not cc2_course_visible(root) then return nil end
  return root
end
function CC2ShopViewer()
  local root=cc2_shop_root()
  return root and root.viewer
end
function CC2ShopPointer()
  local root=cc2_shop_root()
  if not root then return 0 end
  local click=CC2ShopClick
  local x,y=CC2ShopX,CC2ShopY
  if cc2_course_hit(root.options.but_lb,x,y,0.0488,0.0651) then
    if click then root.RunCmd(3) end
    return 7
  end
  if cc2_course_hit(root.options.but_rb,x,y,0.0488,0.0651) then
    if click then root.RunCmd(2) end
    return 8
  end
  if cc2_course_hit(root.but_up,x,y,0.164,0.035) then return 9 end
  if cc2_course_hit(root.but_down,x,y,0.164,0.035) then return 10 end
  local names={"featured","rewinds","powerups","jinxes","fx","course"}
  local name=names[CC2ShopTab or 0]
  local tab=name and root.options[name]
  if not tab or not cc2_course_visible(tab) then return 0 end
  for i,c in ipairs(root.Cats) do
    if c==tab then
      if not click then return i end
      -- Hover only changes the tab artwork; step the native viewer to keep its item grid and category in sync.
      for step=1,math.abs(i-root.Cat) do
        if root.Cat<i then root.viewer:NextCategory();root.Cat=root.Cat+1
        else root.viewer:PrevCategory();root.Cat=root.Cat-1 end
      end
      tab.ForceMouseOver()
      return i
    end
  end
  return 0
end
function CC2ExpandedMenuButton()
  local options=gui and gui.main and gui.main.options
  if not options or not g_showSubOptions or g_showSubOptions==0 then return 0 end
  for i=1,8 do
    local c=options["but"..i]
    if c and c.clicked then return i end
  end
  return 0
end
function CC2ScrollText()
  if not gui or (guimessage and guimessage.IsAnyMessageDisplayed()) then return 0 end
  local root=gui.screens
  if gui.pause and gui.pause.window then root=gui.pause.window end
  if not root then return 0 end
  local pane=root.scroll_text or root.text
  if not pane or not pane.scrollbar or not pane.text then return 0 end
  local value=CC2ScrollValue or 0
  if CC2ScrollRelative then value=pane.text.Percentage+value end
  value=math.max(0,math.min(1,value))
  pane.text.ScrollSpeed=0
  scrollingTextUp=false
  scrollingTextDown=false
  pane.text.Percentage=value
  pane.scrollbar.Value=value
  return 1
end
local function cc2_scroll_frame(parent)
  if parent.cc2_frame then return end
  parent.AddControl("cc2_frame","GGRP:common/ui/empty",CreateV2(0,0),0)
  local frame=parent.cc2_frame
  frame.RelZSort=-2
  frame.Enabled=false
  local pieces={
    {"background","frame_bkg",0,-0.0194,0.6023,0.5082,0},
    {"bottom","scroll_bg_bottom",0,0.2542,1,1,1},
    {"right","scroll_bg_right",0.4047,0,1,1.006,1},
    {"left","scroll_bg_left",-0.4055,0,1,1.006,1},
    {"top","scroll_bg_top",0,-0.2542,1,1,1}
  }
  -- Overlap the side joins to close the native frame's fractional-pixel gaps.
  for _,p in ipairs(pieces) do
    frame.AddControl(p[1],"GPCT:common/ui/men/"..p[2],CreateV2(p[3],p[4]),0)
    frame[p[1]].RelSize=CreateV2(p[5],p[6])
    frame[p[1]].RelZSort=p[7]
  end
end
function CC2DisplaySetSlider()
  if not gui or (guimessage and guimessage.IsAnyMessageDisplayed()) then return 0 end
  local root=gui.screens
  if gui.pause and gui.pause.window then root=gui.pause.window end
  local names={"music","sfx","vibration","cc2_scale","cc2_shadow","cc2_aspect","cc2_resolution","cc2_fullscreen","cc2_limit","cc2_vsync","cc2_fps","cc2_hidehud","cc2_closeup","cc2_freecam"}
  local row=CC2SliderRow
  local name=names[row or 0]
  local c=root and root.sliders and name and root.sliders[name]
  if not c or not c.bar or not cc2_course_visible(c) then return 0 end
  local value=math.max(0,math.min(1,CC2SliderValue))
  CC2DisplaySelect=row
  if row==1 then
    g_musicVolume=value
    resources.MusicMasterVolume=value
  elseif row==2 then
    g_soundVolume=value
    resources.SoundMasterVolume=value
  elseif row==3 and g_vibrationIntensity~=value then
    g_vibrationIntensity=value
    gui.TestRumble(g_vibrationGamepad,0.3,value,value)
  end
  if row<=3 then c.bar.Value=value end
  return 1
end
-- Hides the whole main layer while a course runs: that layer is the HUD then. Menus, the pause layer, popups and
-- the editor keep theirs, and the layer comes back the moment the course ends.
-- Returns 1 or 2 (HUD shown or hidden) plus 4 while a course runs, 8 while the pause menu is up and 16 in single player.
function CC2HudTick(hide)
  if not gui or not gui.main then return 0 end
  -- The course's end is announced through the game's own Log call before the outro waits; the "press A" that
  -- follows is bound to a HUD control, which must be visible to take input.
  if not CC2LogWrapped and type(Log)=="function" then
    CC2LogWrapped=true
    local log=Log
    Log=function(m,...)
      if type(m)=="string" and string.find(m,"going to outro",1,true) then CC2CourseOver=true end
      return log(m,...)
    end
  end
  -- A new course builds a new HUD, which clears the flag; the outro layer itself lingers across courses.
  local hud=gui.main.hud_p1
  local owner=hud and GetUniqueId and GetUniqueId(hud) or nil
  if owner~=CC2HudOwner then CC2HudOwner=owner CC2CourseOver=false end
  local over=CC2CourseOver and true or false
  local in_course=Exist and Exist("Game") and hud~=nil and not over and
      not (type(InEditor)=="function" and InEditor())
  local want=not (hide and in_course)
  if gui.main.Visible~=want then gui.main.Visible=want end
  local paused=gui.pause and gui.pause.window and true or false
  local single=game and type(game.InSinglePlayer)=="function" and game.InSinglePlayer() and true or false
  CC2SinglePlayer=single
  return (want and 1 or 2)+(in_course and 4 or 0)+(paused and 8 or 0)+(single and 16 or 0)
end
function CC2DisplayTick()
  local moved=CC2MouseMoved
  local scroll=CC2DisplayScroll or 0
  local scroll_to=CC2DisplayScrollTo
  local move=CC2DisplayMove or 0
  local clicked=CC2DisplaySelect
  CC2DisplaySelect=nil
  CC2MouseMoved=false
  CC2DisplayScroll=0
  CC2DisplayScrollTo=nil
  CC2DisplayMove=0
  if not gui then return 0 end
  local root=gui.screens
  if gui.pause and gui.pause.window then root=gui.pause.window end
  local pane=root and (root.scroll_text or root.text)
  if pane and pane.scrollbar and pane.text and not pane.cc2_frame then
    if pane[""] then pane[""].Visible=false end
    cc2_scroll_frame(pane)
  end
  if not root or not root.sliders then return 0 end
  local s=root.sliders
  if not s.music or not s.sfx then return 0 end
  local names={"music","sfx","vibration","cc2_scale","cc2_shadow","cc2_aspect","cc2_resolution","cc2_fullscreen","cc2_limit","cc2_vsync","cc2_fps","cc2_hidehud","cc2_closeup","cc2_freecam"}
  if not s.cc2_scale then
    cc2_selected=1
    cc2_first=1
    cc2_scroll_position=0
    cc2_layout_pending=true
    if root[""] then root[""].Visible=false end
    s.RelScreenPos=CreateV2(-0.1201,0)
    s.RelSize=CreateV2(1,1)
    cc2_scroll_frame(s)
    for i=4,14 do
      s.AddControl(names[i],"GGRP:common/ui/men/h_opt/set/music_volume",CreateV2(0,0),0)
    end
    s.AddControl("cc2_scrollbar","GBAR:common/ui/men/h_opt/how_to/scrollbar",CreateV2(0.4271,-0.0181),0)
    s.cc2_scrollbar.RelZSort=1
    s.cc2_scrollbar.RelOrientation=90
    s.cc2_scrollbar.RelSize=CreateV2(1,1)
  end
  -- The free camera row exists only in single player; the list is one shorter everywhere else.
  local count=CC2SinglePlayer and 14 or 13
  if cc2_selected>count then cc2_selected=count end
  if clicked and clicked<=count then cc2_selected=clicked end
  local previous_first=cc2_first
  if scroll_to then cc2_scroll_position=math.max(0,math.min(1,scroll_to))*(count-4) end
  if scroll~=0 then cc2_scroll_position=math.max(0,math.min(count-4,cc2_scroll_position+scroll)) end
  cc2_first=1+math.floor(cc2_scroll_position+0.5)
  local scrolling=scroll~=0 or scroll_to~=nil
  if move~=0 then
    cc2_selected=((cc2_selected-1+move)%count)+1
    PlaySound("CC2/Menus/SFX_Menu_Move")
  end
  local mouse=(CC2MouseHover or 0)==1
  local pad_takeover=cc2_mouse and not mouse
  cc2_mouse=mouse
  -- Consume each pointer move once; navigation and a changing row layout take priority.
  if mouse and moved and not clicked and move==0 and not scrolling and not cc2_layout_pending then
    for i=cc2_first,math.min(cc2_first+3,count) do
      local n=names[i]
      if s[n] and s[n].HasFocus() then
        if i~=cc2_selected then PlaySound("CC2/Menus/SFX_Menu_Move") end
        cc2_selected=i
        break
      end
    end
  end
  if CC2DisplayChanged then
    CC2DisplayChanged=false
    PlaySound("CC2/Menus/SFX_Menu_Switch")
  end
  if move~=0 or pad_takeover then
    if cc2_selected<cc2_first then cc2_first=cc2_selected end
    if cc2_selected>cc2_first+3 then cc2_first=cc2_selected-3 end
    cc2_scroll_position=cc2_first-1
  end
  s.cc2_scrollbar.Value=cc2_scroll_position/(count-4)
  cc2_layout_pending=cc2_first~=previous_first
  for i,n in ipairs(names) do
    local c=s[n]
    c.RelScreenPos=CreateV2(0,-0.17+(i-cc2_first)*0.12)
    c.RelSize=CreateV2(0.78,0.78)
    local shown=i>=cc2_first and i<cc2_first+4 and i<=count
    c.Visible=shown
    -- Rows scrolled out stay disabled, so the pointer cannot focus one it cannot see and jump the list.
    c.Enabled=shown and (mouse or i==cc2_selected)
    c.sel.ClearBh2Queue()
    c.sel.Visible=i==cc2_selected
    c.sel.RelOpacity=i==cc2_selected and 1 or 0
    c[""].Visible=i~=cc2_selected
  end
  local selected_shown=cc2_selected>=cc2_first and cc2_selected<cc2_first+4
  if move~=0 or (not mouse and selected_shown and not s[names[cc2_selected]].HasFocus()) then
    s[names[cc2_selected]].ForceUpdate()
    s[names[cc2_selected]].ForceCursorOver()
  end
  local labels={"Resolution Scale:","Shadow Resolution:","Aspect Ratio:","Window Resolution:","Fullscreen:","Frame Limit:","V-Sync:","FPS Counter:","Hide HUD:","Close Up:","Freecam:"}
  for i=1,11 do
    local c=s[names[i+3]]
    local text=ToUniString(labels[i].."  "..CC2DisplayValues[i])
    c[""].Text=text
    c.sel.Text=text
    c.bar.Value=CC2DisplayFractions[i]
  end
  if cc2_selected>3 then return 100+cc2_selected-3 end
  return 100
end
-- The title's exit fades player 1's slot only; once the profile load has started, slots 2-4 take its opacity.
local cc2_title_exit=false
local function cc2_shown(c) return c and c.Visible and c.Visible~=0 end
function CC2TitleSlotsTick()
  local c=g_controllers
  if not c or not gui or not gui.InSplashScreen then cc2_title_exit=false return 0 end
  if bLoadingContent then cc2_title_exit=true return 0 end
  if not cc2_title_exit or not cc2_shown(c[1]) then return 0 end
  local o=c[1].RelOpacity
  if o and o<1 then
    for i=2,4 do
      if cc2_shown(c[i]) then c[i].RelOpacity=o end
    end
  end
  return 0
end
-- The World Tour and Time Trials tips belong on screen only while the Single Player options are open; the game hides
-- them only on B and on each other's mouse-over.
function CC2MainTipsTick()
  local tip=gui and gui.main and gui.main.tip
  if not tip or (g_showSubOptions==1 and g_mainMenuBut==1) then return 0 end
  if cc2_shown(tip.time_trial) then tip.time_trial.Visible=0 end
  if cc2_shown(tip.world_tour) then tip.world_tour.Visible=0 end
  return 0
end
)lua";
}
