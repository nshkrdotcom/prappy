#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
namespace sampling {
using Frame = int64_t;
constexpr Frame Rate=48000;
struct Span { Frame start=0,end=1;bool operator==(const Span&)const=default; };
class Editor {
    std::vector<Span> past,future;
    bool grouping=false,recorded=false;
public:
    Span span;
    Frame total=1;
    bool dirty=false;
    void beginGesture(){if(!grouping){grouping=true;recorded=false;}}
    void endGesture(){grouping=false;recorded=false;}
    void change(Span next) {
        next.start=std::clamp<Frame>(next.start,0,total-1);
        next.end=std::clamp<Frame>(next.end,next.start+1,total);
        if(next==span)return;
        if(!grouping || !recorded){past.push_back(span);if(past.size()>128)past.erase(past.begin());recorded=true;}future.clear();
        span=next;dirty=true;
    }
    void reset(Frame count,Span initial){endGesture();total=std::max<Frame>(1,count);span={std::clamp<Frame>(initial.start,0,total-1),std::clamp<Frame>(initial.end,1,total)};if(span.end<=span.start)span.end=span.start+1;past.clear();future.clear();dirty=false;}
    void undo(){endGesture();if(past.empty())return;future.push_back(span);span=past.back();past.pop_back();dirty=true;}
    void redo(){endGesture();if(future.empty())return;past.push_back(span);span=future.back();future.pop_back();dirty=true;}
    Frame snap(Frame value,const std::vector<Frame>& points,Frame distance)const{
        Frame best=value,delta=distance+1;
        for(auto frame:points)if(std::abs(frame-value)<delta){delta=std::abs(frame-value);best=frame;}
        return delta<=distance?best:value;
    }
};
}
