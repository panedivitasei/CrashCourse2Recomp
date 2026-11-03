#pragma once

namespace cc2::display {
inline constexpr char kObjectiveScript[] = R"lua(
local owner=nil
local pending=nil
local remaining=0
local last_time=nil
local last_key=nil
local finish_popup=nil
local width=0.40
local padding=0.012
local header_height=0.036
local body_font=0.018
local text_width=width-2*padding
local body_height=2*padding+body_font*(1+0.9*3)

local function hud()
  if not gui or not gui.main or not gui.main.hud_p1 then return nil end
  return gui.main
end

local function reset(main)
  owner=main and GetUniqueId(main.hud_p1) or nil
  pending=nil
  remaining=0
  last_key=nil
  finish_popup=nil
  last_time=GetRealTotalTime()
end

local function queue(kind,title,description,key)
  if last_key==key then return end
  last_key=key
  pending={kind=kind,title=title,description=GetUnistring(description)}
end

local function notify(kind,mission,count)
  local main=hud()
  if not main or not mission then return end
  if owner~=GetUniqueId(main.hud_p1) then reset(main) end
  local target=math.max(0,tonumber(mission.Variable) or 0)
  local mission_type=mission.MissionType
  local current=math.max(0,math.floor(tonumber(count) or 0))
  local description=gui.GetMissionTxt()
  local title
  if kind==1 then
    local unit=""
    if mission_type=="SlideJump" or tonumber(mission_type)==6 then
      unit=" slide jumps"
    elseif mission_type=="Star" or tonumber(mission_type)==1 then
      unit=" stars grabbed"
    end
    if target>0 and current>=target then
      kind=4
      title="Objective Completed"
    else
      title=string.format("Objective Progress: %d/%d%s",current,target,unit)
    end
  elseif kind==2 then
    title="Objective Failed"
  else
    title="Objective Restored"
  end
  -- Completing a counter does not award stars; native medal checks and later failure events still apply.
  queue(kind,title,description,tostring(description)..":"..kind..":"..current)
end

local function panel(main)
  local p=main.cc2_objective
  if p then return p end
  main.AddControl("cc2_objective","GGRP:common/ui/empty",CreateV2(0,0),60)
  p=main.cc2_objective
  p.Visible=false
  p.Enabled=false
  p.AddControl("body","GPCT:common/ui/color/black",CreateV2(0,0),0)
  p.body.RelSize=CreateV2(width/1.4,body_height)
  p.body.RelScreenPos=CreateV2(0,header_height+body_height/2)
  p.body.RelOpacity=0.70
  p.AddControl("bar","GPCT:common/ui/hud/text_bar_bg",CreateV2(0,header_height/2),1)
  p.bar.RelSize=CreateV2(width/0.5333,0.064/0.1778)
  p.AddControl("title","GLBL:common/ui/men/world_sel/mission_desc",CreateV2(0,0),2)
  p.title.Text=ToUniString("")
  p.title.TextColor=CreateRgb(255,255,255)
  p.AddControl("description","GLBL:common/ui/men/world_sel/mission_desc",CreateV2(0,0),2)
  p.description.Text=ToUniString("")
  p.description.RelScreenPos=CreateV2(0,header_height+padding+0.3*text_width/2)
  p.description.RelSize=CreateV2(text_width,text_width)
  p.description.TextScale=body_font/(0.045*text_width)
  p.description.TextColor=CreateRgb(255,255,255)
  p.AddControl("complete","GPCT:common/ui/check_icon",CreateV2(-width/2+0.027,header_height/2),3)
  p.complete.Visible=false
  p.complete.RelSize=CreateV2(0.027/0.0391,0.036/0.0521)
  p.AddControl("failed","GPCT:common/ui/men/mp/online/lobby/gameclip_failed",CreateV2(-width/2+0.027,header_height/2),3)
  p.failed.Visible=false
  p.failed.RelSize=CreateV2(0.027/0.075,0.036/0.1)
  return p
end

local function place(main,p)
  local anchor=main.time_beat
  if not anchor then p.Visible=false;return false end
  local pos=anchor.RelScreenPos
  local size=anchor.RelSize
  -- The hud_right atlas frame ends at local (0.110,0.086), including its template offset.
  local right=pos.x+0.110*size.x
  local top=pos.y+0.086*size.y+0.018
  p.RelScreenPos=CreateV2(right-width/2,top)
  return true
end

local function fill(p,event)
  local icon=event.kind==2 or event.kind==4
  local inset=icon and 0.035 or 0
  local title_width=text_width-inset
  p.title.RelSize=CreateV2(title_width,title_width)
  p.title.RelScreenPos=CreateV2(inset/2,0.006+0.3*title_width/2)
  p.title.TextScale=0.021/(0.045*title_width)
  p.title.Text=ToUniString(event.title)
  p.complete.Visible=event.kind==4
  p.failed.Visible=event.kind==2
  p.description.Text=event.description
end

function CC2ObjectiveTick()
  local main=hud()
  if not main then reset(nil);return 0 end
  if owner~=GetUniqueId(main.hud_p1) then reset(main) end
  local now=GetRealTotalTime()
  local elapsed=math.max(0,now-(last_time or now))
  last_time=now
  local paused=gui.pause and gui.pause.window
  local p=main.cc2_objective
  if paused then
    if p then p.Visible=false end
    return 0
  end
  -- The shipped finish script creates obj_msg only after game.HasCompletedMission succeeds.
  local result=gui.popups and gui.popups.obj_msg
  if result and finish_popup~=GetUniqueId(result) then
    finish_popup=GetUniqueId(result)
    queue(4,"Objective Completed",gui.GetMissionTxt(),"finish:"..finish_popup)
  end
  local event=0
  if pending then
    local next_event=pending
    pending=nil
    remaining=0
    p=panel(main)
    p.Visible=false
    fill(p,next_event)
    remaining=5
    event=next_event.kind
  else
    remaining=math.max(0,remaining-elapsed)
  end
  if p then
    p.Visible=remaining>0 and place(main,p)
    p.RelOpacity=math.min(1,remaining/0.3)
  end
  return event
end

-- Preserve original handlers if a restored content update supplies them.
if type(ObjectiveTrackerShowProgress)~="function" then
  function ObjectiveTrackerShowProgress(mission,count) notify(1,mission,count) end
end
if type(ObjectiveTrackerShowFailed)~="function" then
  function ObjectiveTrackerShowFailed(mission) notify(2,mission,0) end
end
if type(ObjectiveTrackerShowRestored)~="function" then
  function ObjectiveTrackerShowRestored(mission) notify(3,mission,0) end
end
)lua";
}  // namespace cc2::display
