#pragma once
#include "sample_editor.h"
namespace sampling {
enum class PlaybackScope { Selection, Context, Recording };
enum class PlaybackState { Stopped, Loading, Playing, Paused, Buffering, Error };
enum class BoundaryCheck { None, Start, End };
struct EditorTransport {
    PlaybackScope scope=PlaybackScope::Selection;
    PlaybackState state=PlaybackState::Stopped;
    BoundaryCheck boundary=BoundaryCheck::None;
    Span range;
    Frame cursor=0;
    bool loop=true,follow=false;
    bool engaged()const{return state==PlaybackState::Loading||state==PlaybackState::Playing||state==PlaybackState::Paused||state==PlaybackState::Buffering;}
    bool effectiveLoop()const{return loop&&boundary==BoundaryCheck::None;}
    Span bounds(Span selection,Frame total)const{
        Span result=selection;
        if(boundary==BoundaryCheck::Start)result={selection.start-Rate/4,selection.start+Rate};
        else if(boundary==BoundaryCheck::End)result={selection.end-Rate,selection.end+Rate/4};
        else if(scope==PlaybackScope::Context)result={selection.start-Rate,selection.end+Rate};
        else if(scope==PlaybackScope::Recording)result={0,total};
        result.start=std::clamp<Frame>(result.start,0,total-1);result.end=std::clamp<Frame>(result.end,result.start+1,total);return result;
    }
    void stop(){state=PlaybackState::Stopped;cursor=range.start;boundary=BoundaryCheck::None;}
    const char* label()const{
        if(boundary==BoundaryCheck::Start)return "Start check";
        if(boundary==BoundaryCheck::End)return "End check";
        return scope==PlaybackScope::Selection?"Selection":scope==PlaybackScope::Context?"Context":"Recording";
    }
    const char* status()const{
        switch(state){case PlaybackState::Loading:return "Loading";case PlaybackState::Playing:return "Playing";case PlaybackState::Paused:return "Paused";case PlaybackState::Buffering:return "Buffering";case PlaybackState::Error:return "Playback error";default:return "Stopped";}
    }
};
}
