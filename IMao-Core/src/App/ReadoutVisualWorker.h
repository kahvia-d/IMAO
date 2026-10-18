#pragma once
#include "../Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "../Runtime/LatestTaskWorker.h"
#include "../Runtime/LocalizationSignal.h"
#include "../Runtime/VisualProbeEnvelope.h"
struct ReadoutVisualRequest : VisualProbeEnvelope {
 std::uint64_t globalRequest=0;int scene=0;
 Coordinate center;double terrainScale=0;
 cv::Mat snapshot,minimap,trustedReference;
 std::shared_ptr<const RuntimeFeatureResources> resources;
 std::optional<VisualLocalizationCandidate> globalHint;
};
struct ReadoutVisualResult {
 ReadoutVisualRequest source;
 cv::Mat normalized;ImageFeatureData features;MinimapFeatureDiagnostics diagnostics;
 VisualLocalizationCandidate local,global;bool prepared=false,localMatched=false,globalMatched=false;
 double durationMs=0;

};
class ReadoutVisualWorker {
public:
 explicit ReadoutVisualWorker(std::weak_ptr<LocalizationSignal> signal);
 bool Submit(ReadoutVisualRequest request){return worker_.Submit(std::move(request));}
 bool CanSubmit(){return worker_.CanSubmit();}
 bool HasResult(){return worker_.HasResult();}
 bool Take(ReadoutVisualResult& result,std::string& error){return worker_.Take(result,error);}
 void Cancel(){worker_.Cancel();}
 void Shutdown(){worker_.Shutdown();context_.reset();resources_.reset();}
private:
 ReadoutVisualResult Compute(ReadoutVisualRequest request);
 std::shared_ptr<const RuntimeFeatureResources> resources_;
 std::unique_ptr<VisualTrackingContext> context_;
 LatestTaskWorker<ReadoutVisualRequest,ReadoutVisualResult> worker_;
};
