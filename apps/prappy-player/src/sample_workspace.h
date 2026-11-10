#pragma once
#include "sample_editor.h"
#include "sample_client.h"
#include "sample_audition.h"
#include "audio.h"
#include "waveform_amplitude.h"
#include "waveform_viewport.h"
#include "sample_handoff.h"
#include "prepared_catalog.h"
#include <array>
#include <set>
#include <map>
namespace sampling {
class Workspace {
    struct PlayingSongMark {prepared::SourceRef source;std::string asset,timeline;Frame rate,frame;uint64_t ticket;bool submitted=false,failed=false;};
    std::optional<PlayingSongMark> pendingMark;
    Json preparedManifest;
    Json contextManifest;
    std::string contextPath;
    std::vector<PeakBin> contextBins;
    Frame rate=Rate;
    SampleLibrary sampleLibrary;
    SourceHandoff handoff;
    std::string sourceLibrary,activeDraft;
    Json sourceRegions=Json::array();
    size_t sourceRegionOffset=0,sourceRegionTotal=0;
    bool selectionDefined=false,storageBlocked=false;
    std::string storageError;
    fs::path directory;
    std::function<void()> notify;
    std::unique_ptr<Client> client;
    Connection connection;
    Json libraries=Json::array(),items=Json::array(),collections=Json::array(),capture,region,overview,timing,metadata;
    Json fields=Json::object(),pending=Json::array();
    std::set<std::string> editedFacets;
    Editor editor;
    PcmBudget pcmBudget;
    std::shared_ptr<NativeSession> nativeSession;
    std::shared_ptr<NativePlayback> nativePlayback,incomingPlayback,speculativePlayback;
    Span speculativeWindow{};
    bool incomingPrefetched=false;
    Client::Transport backendTransport;
    bool sharedPlayerSession=false;
    EditorTransport transport;
    Span auditionSelection;
    uint64_t auditionChunks=0;
    Frame auditionSubmitted=0;
    bool boundaryHeld=false;
    uint64_t generation=0;
    std::string library,notice,libraryQuery,facet,facetValue,regionName="New sample",kind="unknown",collectionName;
    std::string detailPath,descriptorPath;
    bool connectionReady=false,readOnly=true,nativeSave=false,evaluationTrial=false;
    std::set<std::string> supportedOperations;
    std::string serverUrl="http://127.0.0.1:8765",tokenFile;
    std::string metadataTarget,browsePath,analysisJob;
    std::chrono::steady_clock::time_point jobDue{};
    size_t browseOffset=0,browseTotal=0;
    int metadataTargetRevision=1,metadataRevision=0;
    int tab=0,snapMode=0,selectedAnchor=0;
    bool fit=true,connectionAttempted=false;
    double viewportStart=0,viewportEnd=48000;
    Span monitorRange()const;
    std::string audioEndpoint()const;
    void resetNativeSession();
    void closeNativeSession();
    AmplitudeScale amplitude;
    StereoEnvelope envelope,displayEnvelope;
    std::vector<PeakBin> overviewBins,detailBins;
    uint64_t waveformRevision=0,envelopeRevision=~uint64_t{0},envelopeBuilds=0;
    Frame envelopeStart=-1,envelopeEnd=-1;
    size_t envelopeBudget=0;
    double projectedGain=-1,rulerInitial=1;
    bool fitAmplitudeNow=false;
    std::vector<Frame> beats,onsets;
    std::vector<Json> anchors;
    std::string gridId;int gridRevision=0;
    float previewGain=.7f;
    std::chrono::steady_clock::time_point detailDue{},persistDue{},overviewDue{};
    Span persistedSpan;
    bool draftDirty=false;
    void get(std::string tag,std::string path);
    void command(std::string operation,Json payload);
    void sendPending(Json&,std::shared_ptr<const NativeSession::Clip> = {});
    void browse();
    SourceRef sourceRef()const;
    DraftValue draftValue()const;
    void stash();
    void activateDraft(const std::string&,bool fetch=true);
    void newDraft(Span,bool variant=false,bool fetch=true);
    void fetchSourceRegions(size_t offset=0);
    void chooseRegion(const Json&);
    void submitMark();
    void finishMark(const Json&);
    void openCapture(Json, std::optional<Span> = {},std::optional<Frame> cue = {});
    void loadDetails(bool refresh=false);
    void loadOverview();
    Json draftSnapshot()const;
    void discardConflicts();
    void persist();
    void restore();
    void metadataFields(const Json&);
    Json humanFacts(bool forSample=false)const;
    void beginAudition(Audio&,BoundaryCheck=BoundaryCheck::None,std::optional<Frame> = {},bool paused=false);
    void stopTransport();
    void seekTransport(Audio&,Frame);
    void playbackOverlay();
    void process();
    bool current(const Request&)const;
    void waveform(Audio&,const std::function<void(const char*,bool)>&);
public:
    bool open=false;
    Workspace(fs::path,std::function<void()>,PcmBudget memory=PcmBudget{},Client::Transport backendTransport={});
    ~Workspace();
    void connect();
    void returnToPlayer();
    void mark(Audio&,std::optional<SecondsSpan> selection={},OpenSource action=OpenSource::Cue);
    void prefetch(Audio&,std::optional<SecondsSpan> = {});
    void toggleTransport(Audio&);
    void tick(Audio&);
    void draw(Audio&,const std::function<void(const char*,bool)>&);
    bool active()const{return open;}
    bool animating()const{return open&&(transport.state==PlaybackState::Playing||transport.state==PlaybackState::Buffering);}
    const std::string& preparationStatus()const{return notice;}
    std::string diagnostics()const{return Json{{"shared_player_session",sharedPlayerSession},{"prefetch_frames",speculativePlayback&&speculativePlayback->session()?speculativePlayback->session()->frames():0},{"prefetch_ready",speculativePlayback&&speculativePlayback->session()&&speculativePlayback->session()->covers(speculativeWindow.start,speculativeWindow.end)},{"open",open},{"read_only",readOnly},{"capture",capture.is_null()?Json(nullptr):capture.at("id")},{"region",region},{"start",editor.span.start},{"end",editor.span.end},{"pcm_frames",nativeSession?nativeSession->frames():0},{"beats",beats.size()},{"pending",pending.size()},{"notice",notice},{"amplitude_mode",static_cast<int>(amplitude.mode)},{"amplitude_gain",amplitude.gain},{"visible_peak",envelope.peak},{"envelope_builds",envelopeBuilds},{"pcm_selection_ready",nativeSession&&nativeSession->covers(editor.span.start,editor.span.end)},{"waveform_overview_bins",overviewBins.size()},{"waveform_context_bins",contextBins.size()},{"waveform_detail_bins",detailBins.size()},{"waveform_stride",detailBins.empty()?Frame{0}:detailBins.front().end-detailBins.front().start},{"waveform_bins",overviewBins.size()+detailBins.size()},{"viewport_start",viewportStart},{"viewport_end",viewportEnd},{"pcm_start",nativeSession?nativeSession->first():0},{"pcm_end",nativeSession?nativeSession->last():0},{"pcm_bytes",pcmBudget.used()},{"pcm_budget_bytes",pcmBudget.limit()},{"playback_state",transport.status()},{"playback_scope",static_cast<int>(transport.scope)},{"playhead",transport.cursor},{"playback_start",transport.range.start},{"playback_end",transport.range.end},{"playback_loop",transport.loop},{"playback_chunks",auditionChunks},{"audio_frames_submitted",auditionSubmitted},{"selection_defined",selectionDefined},{"source_library",sourceLibrary},{"active_draft",activeDraft},{"workspace_count",sampleLibrary.all().size()},{"draft_count",capture.is_null()?size_t{0}:sampleLibrary.at(sourceRef()).drafts.size()},{"source_region_total",sourceRegionTotal},{"preparation",notice},{"mark_pending",pendingMark.has_value()},{"mark_frame",pendingMark?pendingMark->frame:Frame{-1}},{"mark_failed",pendingMark&&pendingMark->failed}}.dump();}
};
}
