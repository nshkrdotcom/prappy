#pragma once
#include "analysis.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <vector>

// Both listening and sample editing use the same bounded stereo plot primitive.
namespace sampling {
inline void plotStereo(const char* prefix,const std::vector<double>& x,
                       const std::vector<double>& leftMin,const std::vector<double>& leftMax,
                       const std::vector<double>& rightMin,const std::vector<double>& rightMax,bool samples=false) {
    if(x.empty())return;
    const std::string left=std::string(prefix)+" L",right=std::string(prefix)+" R";
    if(samples) {
        ImPlot::PlotLine(left.c_str(),x.data(),leftMin.data(),static_cast<int>(x.size()));
        ImPlot::PlotLine(right.c_str(),x.data(),rightMin.data(),static_cast<int>(x.size()));
    } else {
        ImPlot::PlotShaded(left.c_str(),x.data(),leftMin.data(),leftMax.data(),static_cast<int>(x.size()));
        ImPlot::PlotShaded(right.c_str(),x.data(),rightMin.data(),rightMax.data(),static_cast<int>(x.size()));
    }
}
}
class WaveformView {
    std::shared_ptr<const analysis::Result> source;
    std::vector<double> x,lmin,lmax,rmin,rmax;
    int width=0;
public:
    size_t rebuilds=0;
    int vertices=0;
    double markerRatio=0;
    explicit WaveformView(SDL_Renderer*) {}
    void resetTexture(){source.reset();width=0;}
    void prepare(const std::shared_ptr<const analysis::Result>& waveform,float targetWidth) {
        const auto pixels=std::max(1,static_cast<int>(targetWidth));
        if(source==waveform && width==pixels)return;
        source=waveform;width=pixels;x.clear();lmin.clear();lmax.clear();rmin.clear();rmax.clear();
        if(!source || source->levels.empty())return;
        size_t level=0;
        while(level+1<source->levels.size() && source->levels[level].size()>static_cast<size_t>(pixels)*2)++level;
        const auto& peaks=source->levels[level];
        const auto count=std::min(peaks.size(),static_cast<size_t>(pixels));
        for(size_t i=0;i<count;++i){
            const size_t begin=i*peaks.size()/count,end=std::max(begin+1,(i+1)*peaks.size()/count);
            analysis::Peak p=peaks[begin];
            for(size_t j=begin+1;j<end;++j){p.leftMin=std::min(p.leftMin,peaks[j].leftMin);p.leftMax=std::max(p.leftMax,peaks[j].leftMax);p.rightMin=std::min(p.rightMin,peaks[j].rightMin);p.rightMax=std::max(p.rightMax,peaks[j].rightMax);}
            x.push_back(count==1?0.:static_cast<double>(i)/static_cast<double>(count-1));
            lmin.push_back(1+p.leftMin*.9);lmax.push_back(1+p.leftMax*.9);
            rmin.push_back(-1+p.rightMin*.9);rmax.push_back(-1+p.rightMax*.9);
        }
        ++rebuilds;
    }
    void draw(const std::shared_ptr<const analysis::Result>& waveform,ImVec2 min,ImVec2 max,double ratio) {
        prepare(waveform,max.x-min.x);
        markerRatio=std::clamp(ratio,0.,1.);
        const auto cursor=ImGui::GetCursorScreenPos();
        const auto item=ImGui::GetCurrentContext()->LastItemData;
        auto draw=ImGui::GetWindowDrawList();const auto before=draw->VtxBuffer.Size;
        ImGui::SetCursorScreenPos(min);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding,ImVec2(0,0));
        ImPlot::PushStyleColor(ImPlotCol_PlotBg,ImVec4(.094f,.114f,.122f,1));
        if(ImPlot::BeginPlot("##waveform",ImVec2(max.x-min.x,max.y-min.y),ImPlotFlags_CanvasOnly|ImPlotFlags_NoInputs|ImPlotFlags_NoFrame)){
            ImPlot::SetupAxes(nullptr,nullptr,ImPlotAxisFlags_NoDecorations,ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxesLimits(0,1,-2.1,2.1,ImGuiCond_Always);
            sampling::plotStereo("Wave",x,lmin,lmax,rmin,rmax);
            const auto marker=ImPlot::PlotToPixels(markerRatio,0);
            ImPlot::GetPlotDrawList()->AddRectFilled({marker.x,min.y},{marker.x+2,max.y},IM_COL32(245,189,99,255));
            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor();ImPlot::PopStyleVar();
        vertices=draw->VtxBuffer.Size-before;
        ImGui::SetCursorScreenPos(cursor);
        ImGui::GetCurrentContext()->LastItemData=item;
    }
};
