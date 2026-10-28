#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace sampling {
enum class AmplitudeMode { Actual, Visible, Manual };

// Display-only state. No audio buffer, splice, or preparation has access to this gain.
struct AmplitudeScale {
    static constexpr double MinGain=1./64,MaxGain=1024,Silence=1e-6;
    AmplitudeMode mode=AmplitudeMode::Actual;
    double gain=1;
    static double bounded(double value){return std::isfinite(value)?std::clamp(value,MinGain,MaxGain):1.;}
    void actual(){mode=AmplitudeMode::Actual;gain=1;}
    void fit(double peak,bool continuous){
        mode=continuous?AmplitudeMode::Visible:AmplitudeMode::Manual;
        gain=std::isfinite(peak)&&peak>Silence?bounded(1/peak):1.;
    }
    void drag(double initial,double pixels){mode=AmplitudeMode::Manual;gain=bounded(initial*std::exp2(std::clamp(pixels/80.,-20.,20.)));}
    double project(double sample,bool right)const{return (right?-1.:1.)+.9*std::clamp(sample*gain,-1.,1.);}
    double db()const{return 20*std::log10(gain);}
};

struct PeakBin {int64_t start=0,end=0;double lmin=0,lmax=0,rmin=0,rmax=0;};

// Raw source amplitude stays separate from the display transform. A bounded envelope
// is rebuilt on viewport/data changes, then reprojected only when its scale changes.
struct StereoEnvelope {
    std::vector<double> x,lmin,lmax,rmin,rmax;
    double peak=0;
    bool individual=false;
    void clear(){x.clear();lmin.clear();lmax.clear();rmin.clear();rmax.clear();peak=0;individual=false;}
    void add(double frame,double lo,double hi,double ro,double rh){
        x.push_back(frame);lmin.push_back(lo);lmax.push_back(hi);rmin.push_back(ro);rmax.push_back(rh);
        peak=std::max({peak,std::abs(lo),std::abs(hi),std::abs(ro),std::abs(rh)});
    }
    bool fromPcm(std::span<const float> audio,int64_t origin,int64_t first,int64_t end,size_t budget){
        clear();
        if(audio.size()%2 || first<origin || end<=first || end>origin+static_cast<int64_t>(audio.size()/2))return false;
        const auto step=std::max<int64_t>(1,(end-first+static_cast<int64_t>(std::max<size_t>(1,budget))-1)/static_cast<int64_t>(std::max<size_t>(1,budget)));
        individual=step==1;
        for(auto at=first;at<end;at+=step){
            const auto i=static_cast<size_t>(at-origin)*2;double lo=audio[i],hi=lo,ro=audio[i+1],rh=ro;
            for(auto j=at+1;j<std::min(end,at+step);++j){const auto n=static_cast<size_t>(j-origin)*2;lo=std::min(lo,static_cast<double>(audio[n]));hi=std::max(hi,static_cast<double>(audio[n]));ro=std::min(ro,static_cast<double>(audio[n+1]));rh=std::max(rh,static_cast<double>(audio[n+1]));}
            add(static_cast<double>(at),lo,hi,ro,rh);
        }
        return true;
    }
    void fromOverview(std::span<const PeakBin> bins,int64_t first,int64_t end){
        clear();for(const auto& bin:bins)if(bin.end>first && bin.start<end)add(static_cast<double>(bin.start),bin.lmin,bin.lmax,bin.rmin,bin.rmax);
    }
    void project(const AmplitudeScale& scale,StereoEnvelope& output)const{
        output.clear();output.x=x;output.individual=individual;output.peak=peak;
        for(size_t i=0;i<x.size();++i){output.lmin.push_back(scale.project(lmin[i],false));output.lmax.push_back(scale.project(lmax[i],false));output.rmin.push_back(scale.project(rmin[i],true));output.rmax.push_back(scale.project(rmax[i],true));}
    }
};
}
