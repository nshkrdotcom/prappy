#pragma once
#include "sample_editor.h"
#include "sample_client.h"
#include "sample_audition.h"
#include "audio.h"
#include "waveform_amplitude.h"
#include "waveform_viewport.h"
#include <array>
#include <set>
namespace sampling {
class Workspace {
    fs::path directory;
    std::function<void()> notify;
    std::unique_ptr<Client> client;
    Connection connection;
    Json libraries=Json::array(),items=Json::array(),collections=Json::array(),capture,region,overview,timing,metadata;
    Json fields=Json::object(),pending=Json::array();
    std::set<std::string> editedFacets;
    Editor editor;
    Audition audition;
    EditorTransport transport;
    Span auditionSelection;
    uint64_t transportEpoch=0;
    uint64_t auditionChunks=0;
    bool boundaryHeld=false,hadOpen=false;
    uint64_t generation=0;
    std::string library,notice,libraryQuery,facet,facetValue,regionName="New sample",kind="unknown",collectionName;
    std::string serverUrl="http://127.0.0.1:8765",tokenFile;
    std::string metadataTarget,browsePath,auditionPath,analysisJob;
    std::chrono::steady_clock::time_point jobDue{};
    size_t browseOffset=0,browseTotal=0;
    int metadataTargetRevision=1,metadataRevision=0;
    int tab=0,snapMode=0,selectedAnchor=0;
    bool fit=true,sourcePending=false,connectionAttempted=false;
    double viewportStart=0,viewportEnd=48000;
    Frame pcmStart=0,pcmEnd=0;
    std::vector<float> pcm;
    AmplitudeScale amplitude;
    StereoEnvelope envelope,displayEnvelope;
    std::vector<PeakBin> overviewBins;
    uint64_t waveformRevision=0,envelopeRevision=~uint64_t{0},envelopeBuilds=0;
    Frame envelopeStart=-1,envelopeEnd=-1;
    size_t envelopeBudget=0;
    double projectedGain=-1,rulerInitial=1;
    bool fitAmplitudeNow=false,envelopePcm=false;
    std::vector<Frame> beats,onsets;
    std::vector<Json> anchors;
    std::string gridId;int gridRevision=0;
    float previewGain=.7f;
    std::chrono::steady_clock::time_point detailDue{},persistDue{};
    Span persistedSpan;
    bool draftDirty=false;
    void get(std::string tag,std::string path);
    void command(std::string operation,Json payload);
    void browse();
    void openCapture(Json, std::optional<Span> = {});
    void loadDetails(bool refresh=false);
    Json draftSnapshot()const;
    void discardConflicts();
    void persist();
    void restore();
    void metadataFields(const Json&);
    Json humanFacts(bool forSample=false)const;
    void requestAudition(Frame,bool);
    void beginAudition(Audio&,BoundaryCheck=BoundaryCheck::None,std::optional<Frame> = {},bool paused=false);
    void stopTransport();
    void seekTransport(Audio&,Frame);
    void playbackOverlay();
    void process(Audio&);
    void waveform(Audio&,const std::function<void(const char*,bool)>&);
public:
    bool open=false;
    Workspace(fs::path,std::function<void()>);
    ~Workspace();
    void connect();
    void mark(Audio&);
    void toggleTransport(Audio&);
    void tick(Audio&);
    void draw(Audio&,const std::function<void(const char*,bool)>&);
    bool active()const{return open;}
    bool animating()const{return open&&(transport.state==PlaybackState::Playing||transport.state==PlaybackState::Buffering);}
    std::string diagnostics()const{return Json{{"open",open},{"capture",capture.is_null()?Json(nullptr):capture.at("id")},{"region",region},{"start",editor.span.start},{"end",editor.span.end},{"pcm_frames",pcm.size()/2},{"beats",beats.size()},{"pending",pending.size()},{"notice",notice},{"amplitude_mode",static_cast<int>(amplitude.mode)},{"amplitude_gain",amplitude.gain},{"visible_peak",envelope.peak},{"envelope_builds",envelopeBuilds},{"waveform_exact_pcm",envelopePcm},{"viewport_start",viewportStart},{"viewport_end",viewportEnd},{"pcm_start",pcmStart},{"pcm_end",pcmEnd},{"playback_state",transport.status()},{"playback_scope",static_cast<int>(transport.scope)},{"playhead",transport.cursor},{"playback_start",transport.range.start},{"playback_end",transport.range.end},{"playback_loop",transport.loop},{"playback_chunks",auditionChunks}}.dump();}
};
}
