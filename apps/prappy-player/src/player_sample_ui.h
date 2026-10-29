#pragma once
#include "sample_handoff.h"
#include "imgui.h"
#include <functional>

// Player-only interaction state; it never owns a canonical region or issues a save.
struct PlayerSampleSelection {
    enum class Drag { None,Start,End,Move,New };
    std::string source,contextSource;
    uint64_t loadedSerial=0,contextSerial=0;
    bool active=false;
    double start=0,end=0,anchor=0,contextTime=0;
    sampling::SecondsSpan before;
    Drag drag=Drag::None;
    void sync(const std::string& path,uint64_t serial){if(source!=path||loadedSerial!=serial){source=path;loadedSerial=serial;active=false;drag=Drag::None;}}
    void begin(double at,double duration){
        if(duration<=0)return;
        start=std::clamp(at,0.,std::max(0.,duration-std::min(4.,duration)));end=std::min(duration,start+4.);active=true;drag=Drag::None;
    }
    bool cancel(){
        if(drag!=Drag::None){start=before.start;end=before.end;drag=Drag::None;return false;}
        active=false;return true;
    }
    void draw(ImVec2 min,ImVec2 max,double position,double duration,bool compact,
              const std::function<void(std::optional<sampling::SecondsSpan>,sampling::OpenSource)>& open,
              const std::function<void(const char*,bool)>& observe){
        if(duration<=0)return;
        const bool hovered=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)&&!ImGui::IsPopupOpen("waveform-actions");
        const auto time=[&](float x){return duration*std::clamp(static_cast<double>((x-min.x)/std::max(1.f,max.x-min.x)),0.,1.);};
        const auto pixel=[&](double t){return min.x+static_cast<float>(t/duration)*(max.x-min.x);};
        if(hovered&&ImGui::IsMouseReleased(ImGuiMouseButton_Right)){contextSource=source;contextSerial=loadedSerial;contextTime=time(ImGui::GetIO().MousePos.x);ImGui::OpenPopup("waveform-actions");}
        if(ImGui::BeginPopup("waveform-actions")){
            if(source!=contextSource||loadedSerial!=contextSerial)ImGui::CloseCurrentPopup();
            else{
                const auto item=[&](const char* label,const char* shortcut,const char* id,bool enabled=true){const bool hit=ImGui::MenuItem(label,shortcut,false,enabled);observe(id,hit);return hit;};
                if(item("Open song in sample editor","Ctrl+M","waveform-open-editor"))open({},sampling::OpenSource::Cue);
                if(item("Select sample on waveform...","Ctrl+E","waveform-select"))begin(contextTime,duration);
                if(item("Open selection in editor","Enter","waveform-open-selection",active))open(sampling::SecondsSpan{start,end},sampling::OpenSource::Selection);
                if(item("Samples from this song","Ctrl+Alt+M","waveform-samples"))open({},sampling::OpenSource::Samples);
                if(item("Cancel selection","Esc","waveform-cancel-menu",active))active=false;
            }
            ImGui::EndPopup();
        }
        if(!active)return;
        if(hovered&&ImGui::IsMouseClicked(0)){
            anchor=time(ImGui::GetIO().MousePos.x);before={start,end};
            const float x=ImGui::GetIO().MousePos.x;
            drag=std::abs(x-pixel(start))<=9?Drag::Start:std::abs(x-pixel(end))<=9?Drag::End:anchor>start&&anchor<end?Drag::Move:Drag::New;
        }
        if(drag!=Drag::None&&ImGui::IsMouseDown(0)&&ImGui::IsMouseDragging(0)){
            const double at=time(ImGui::GetIO().MousePos.x),sample=1./sampling::Rate;
            if(drag==Drag::Start)start=std::clamp(at,0.,end-sample);
            else if(drag==Drag::End)end=std::clamp(at,start+sample,duration);
            else if(drag==Drag::Move){const double width=before.end-before.start;start=std::clamp(before.start+at-anchor,0.,duration-width);end=start+width;}
            else{start=std::min(anchor,at);end=std::max(anchor,at);if(end-start<sample){start=before.start;end=before.end;}}
        }
        if(ImGui::IsMouseReleased(0))drag=Drag::None;
        if(hovered)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled({pixel(start),min.y},{pixel(end),max.y},IM_COL32(55,220,160,45));
        draw->AddLine({pixel(start),min.y},{pixel(start),max.y},IM_COL32(80,255,175,255),3);
        draw->AddLine({pixel(end),min.y},{pixel(end),max.y},IM_COL32(255,185,50,255),3);
        if(compact)return;
        ImGui::Text("Sample: %.3f - %.3f s (%.3f s)",start,end,end-start);
        const bool transfer=ImGui::Button("Open selection in editor (Enter)");observe("waveform-confirm",transfer);
        if(transfer)open(sampling::SecondsSpan{start,end},sampling::OpenSource::Selection);
        ImGui::SameLine();const bool cancelClicked=ImGui::Button("Cancel (Esc)");observe("waveform-cancel",cancelClicked);if(cancelClicked)active=false;
        ImGui::TextDisabled("Drag a range; drag either edge or move the band. Song playhead: %.3f s",position);
    }
};
