#pragma once
namespace cc2::lobby {
// Quick Match's button opens the matchmaking popup and calls netman.QuickMatch(popup); the hook keeps that popup and
// adds the lobby list to its box from the sign-in screen's pieces, the way the game adds its own status rows. The
// lobby host runs the Private Match host path on the popup; members and Cancel close it, and members then join
// through an accepted invite.
inline constexpr char kLobbyScript[] = R"lua(
local cc2_lobby_rows={"lb_row1","lb_row2","lb_row3","lb_row4","lb_row5","lb_row6"}
-- Offsets from the popup's inner control to the top-left of its text area; labels draw 0.05 above their row anchor.
local cc2_lobby_left=-0.327
local cc2_lobby_top=-0.139
local cc2_lobby_row_step=0.046
local cc2_lobby_row_size=0.62
local cc2_lobby_status_size=0.7
local cc2_lobby_widths={}
local cc2_lobby_ready=false
local cc2_lobby_displayed=false
local cc2_lobby_added={}
CC2LobbyRequests=CC2LobbyRequests or 0
local function cc2_lobby_join(status)
  CC2LobbyStatus=status
  CC2LobbyRequests=CC2LobbyRequests+1
end
function CC2LobbyInstall()
  if not netman then return 0 end
  if CC2LobbyHooked and netman.QuickMatch==cc2_lobby_join then return 1 end
  local nm=netman
  CC2LobbyNetman=nm
  if nm.QuickMatch~=cc2_lobby_join and type(nm.QuickMatch)=="function" then CC2LobbyQuickMatch=nm.QuickMatch end
  local ok=pcall(function() nm.QuickMatch=cc2_lobby_join end)
  if not ok or nm.QuickMatch~=cc2_lobby_join then
    -- A native netman rejects new fields, so the global becomes a proxy that forwards everything else.
    netman=setmetatable({QuickMatch=cc2_lobby_join},{__index=nm})
  end
  CC2LobbyHooked=true
  return netman.QuickMatch==cc2_lobby_join and 1 or 0
end
local function cc2_lobby_loaded(c)
  return c and type(c.GetAbsScreenPos)=="function"
end
-- The pane layer only keeps the row master resident; the rows themselves go on the popup.
local function cc2_lobby_layers()
  if not gui then return nil end
  if not gui.cc2_lobby then gui.CreateGuiLayer("cc2_lobby",242,"gui") end
  -- Hidden from the start, or the settings pane draws a frame before its sliders are hidden.
  gui.cc2_lobby.Visible=false
  return gui.cc2_lobby
end
-- Children of the popup's inner control scale and fade with its opening and closing animations.
local function cc2_lobby_box()
  local p=gui and gui.popups
  if not p or not cc2_lobby_loaded(p.inner) or type(p.inner.AddControl)~="function" then return nil end
  return p.inner
end
local function cc2_lobby_hide()
  if gui and cc2_lobby_displayed then
    local p=cc2_lobby_box()
    if p then
      for _,n in ipairs(cc2_lobby_rows) do
        if cc2_lobby_loaded(p[n]) then p[n].Visible=false end
      end
      if cc2_lobby_loaded(p.lb_status) then p.lb_status.Visible=false end
    end
    gui.DisplayGroup("cc2_lobby","common/ui/empty")
  end
  cc2_lobby_ready=false
  cc2_lobby_displayed=false
  cc2_lobby_added={}
end
local function cc2_lobby_add(p,name,master,x,y)
  if not cc2_lobby_loaded(p[name]) and not cc2_lobby_added[name] then
    p.AddControl(name,master,CreateV2(x,y),0)
    cc2_lobby_added[name]=true
  end
  return p[name]
end
local function cc2_lobby_build()
  local s=cc2_lobby_layers()
  if not s then return nil,10 end
  if not cc2_lobby_displayed then
    gui.DisplayGroup("cc2_lobby","common/ui/men/h_opt/set/sliders")
    cc2_lobby_displayed=true
    return nil,11
  end
  if not cc2_lobby_loaded(s.music) or not cc2_lobby_loaded(s.sfx) or not cc2_lobby_loaded(s.vibration) then
    return nil,11
  end
  for _,n in ipairs({"music","sfx","vibration"}) do
    s[n].Visible=false
    s[n].Enabled=false
  end
  local p=cc2_lobby_box()
  if not p then return nil,14 end
  local rows_ready=true
  for i,n in ipairs(cc2_lobby_rows) do
    local c=cc2_lobby_add(p,n,"GGRP:common/ui/men/h_opt/set/music_volume",
                          cc2_lobby_left,cc2_lobby_top+0.05+(i-1)*cc2_lobby_row_step)
    if not cc2_lobby_loaded(c) or not cc2_lobby_loaded(c.bar) or
        not cc2_lobby_loaded(c[""]) or not cc2_lobby_loaded(c.sel) or
        type(c.sel.ClearBh2Queue)~="function" then
      rows_ready=false
    else
      c.RelSize=CreateV2(cc2_lobby_row_size,cc2_lobby_row_size)
      -- The popup's box shares z 0 and would draw over the labels.
      c.RelZSort=5
      c[""].TextColor=CreateRgb(255,255,255)
      c.bar.Visible=false
      c.Enabled=false
    end
  end
  if not rows_ready then return nil,12 end
  local status=cc2_lobby_add(p,"lb_status","GLBL:common/ui/sys/msg/text",cc2_lobby_left,cc2_lobby_top)
  if not cc2_lobby_loaded(status) then return nil,13 end
  status.Enabled=false
  status.RelZSort=5
  status.RelSize=CreateV2(cc2_lobby_status_size,cc2_lobby_status_size)
  cc2_lobby_ready=true
  return p,1
end
function CC2LobbyTick(visible,selected,count,status,sound,r1,r2,r3,r4,r5,r6)
  if not visible then cc2_lobby_hide();return 0 end
  if not gui then return 10 end
  local p=cc2_lobby_ready and cc2_lobby_box() or nil
  -- A popup that closed under the list took the rows with it.
  if p and not cc2_lobby_loaded(p.lb_status) then
    cc2_lobby_ready=false
    cc2_lobby_added={}
    p=nil
  end
  if not p then
    local stage
    p,stage=cc2_lobby_build()
    if not p then return stage end
  end
  if sound==1 then PlaySound("CC2/Menus/SFX_Menu_Move")
  elseif sound==2 then PlaySound("CC2/Menus/SFX_Menu_Click") end
  -- Text is centred on its anchor and draws size*(GetTextWidth()-0.052) wide, so anchors move to left-align it.
  local function place(c,label,size,y)
    local w=size*math.max(label.GetTextWidth()-0.052,0)
    c.RelScreenPos=CreateV2(cc2_lobby_left+w/2,y)
    return w
  end
  local labels={r1,r2,r3,r4,r5,r6}
  for i,n in ipairs(cc2_lobby_rows) do
    local c=p[n]
    local shown=i<=count
    c.Visible=shown
    if shown then
      local text=ToUniString(labels[i])
      c[""].Text=text
      c.sel.Text=text
      c.sel.ClearBh2Queue()
      c.sel.Visible=i==selected
      c.sel.RelOpacity=i==selected and 1 or 0
      c[""].Visible=i~=selected
      cc2_lobby_widths[i]=place(c,c[""],cc2_lobby_row_size,cc2_lobby_top+0.05+(i-1)*cc2_lobby_row_step)
    end
    c.bar.Visible=false
  end
  -- Status text goes under the last row, or first when there are none.
  p.lb_status.Visible=status~=""
  if status~="" then
    p.lb_status.Text=ToUniString(status)
    place(p.lb_status,p.lb_status,cc2_lobby_status_size,cc2_lobby_top+count*cc2_lobby_row_step)
  end
  return 1
end
function CC2LobbyHit(x,y,count)
  local p=cc2_lobby_ready and cc2_lobby_box() or nil
  if not p or not cc2_lobby_loaded(p.lb_status) then return 0 end
  -- Labels, not row anchors: the anchor sits below its text.
  local half=cc2_lobby_row_step/2
  for i=1,count do
    local q=p[cc2_lobby_rows[i]][""].GetAbsScreenPos()
    local w=(cc2_lobby_widths[i] or 0)/2+0.02
    if math.abs(y-q.y)<=half and math.abs(x-q.x)<=w then return i end
  end
  -- The popup's own Cancel prompt.
  if cc2_lobby_loaded(p.but_b) then
    local b=p.but_b.GetAbsScreenPos()
    if math.abs(y-b.y)<=0.03 and x>=b.x-0.13 and x<=b.x+0.03 then return -1 end
  end
  return 0
end
-- The same calls the Private Match button makes, on the popup Quick Match opened.
function CC2LobbyHost()
  local s=CC2LobbyStatus
  local nm=CC2LobbyNetman or netman
  if not s or not nm or type(nm.PrivateMatch)~="function" then return 0 end
  s.AddProp("Failed",function(me) me:AddAction(GetUnistring("matchmaking/status/create_error")) end)
  nm.PrivateMatch(s)
  CC2LobbyHosting=s
  return 1
end
function CC2LobbyHostStatus()
  local s=CC2LobbyHosting
  CC2LobbyHosting=nil
  CC2LobbyStatus=nil
  if s then s:AddAction(GetUnistring("matchmaking/status/creating_private")) end
  return s and 1 or 0
end
function CC2LobbyCancel()
  local s=CC2LobbyStatus
  local nm=CC2LobbyNetman or netman
  CC2LobbyStatus=nil
  if nm and type(nm.CancelMatchmaking)=="function" then nm.CancelMatchmaking() end
  if s and s.Close then s.Close() end
  return 1
end
)lua";
}  // namespace cc2::lobby
