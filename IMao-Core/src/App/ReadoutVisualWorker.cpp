#include "ReadoutVisualWorker.h"
#include "../Coordinate/VisualLocalization/RecoveryPolicy.h"
#include "../Runtime/FrameStorageTrace.h"
#include "../Diagnostics/ReproCapture.h"
ReadoutVisualWorker::ReadoutVisualWorker(std::weak_ptr<LocalizationSignal> signal)
 :worker_([this](ReadoutVisualRequest request){return Compute(std::move(request));},
  [signal]{if(auto wake=signal.lock())wake->NotifyResult();}){}
ReadoutVisualResult ReadoutVisualWorker::Compute(ReadoutVisualRequest request){
 const auto start=std::chrono::steady_clock::now();ReadoutVisualResult result;result.source=std::move(request);
 auto captureReadTrace=FrameStorage::Observe(result.source.snapshot,FrameStorage::ReaderRole::ReadoutWorker,result.source.frame);
 const auto& source=result.source;
 ReproCapture::Scope repro({{"captureRun",source.session},{"frameId",source.frame},
  {"requestId",source.globalRequest},{"stage","readout-visual"}});
 if(!source.minimap.empty())ReproCapture::Image("readout-minimap-crop",source.minimap);
 if(!source.resources||!source.resources->visualIndexReady)throw std::runtime_error("readout visual resources unavailable");
 if(resources_!=source.resources){context_.reset();resources_=source.resources;context_=std::make_unique<VisualTrackingContext>(resources_);}
 result.prepared=GlobalVisualLocalizer::PrepareMinimap(source.minimap,result.normalized,result.features,
  source.trustedReference.empty()?nullptr:&source.trustedReference,&result.diagnostics);
 if(!result.normalized.empty())ReproCapture::Image("readout-minimap-normalized",result.normalized);
 if(!result.diagnostics.featureMask.empty())ReproCapture::Image("readout-minimap-mask",result.diagnostics.featureMask);
 if(result.prepared){
  if(source.globalHint){
   const auto& hint=*source.globalHint;
   result.globalMatched=context_->Track(result.normalized,result.features,hint.sceneId,hint.mapCenter,result.global,256)&&
    HasReacquisitionSupport(result.global.affineEstimated,result.global.inlierCount,result.global.inlierRatio,result.global.coveredQuadrants);
   result.global.ocrHintMatched=hint.ocrHintMatched;
  }
  if(source.scene>0){
   result.localMatched=context_->Track(result.normalized,result.features,source.scene,source.center,result.local,0,source.terrainScale);
   if(!result.localMatched){
    result.localMatched=context_->Track(result.normalized,result.features,source.scene,source.center,result.local,256)&&
     HasReacquisitionSupport(result.local.affineEstimated,result.local.inlierCount,result.local.inlierRatio,result.local.coveredQuadrants);
   }
  }
 }
 result.durationMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 ReproCapture::Record("readout-visual-result",{{"prepared",result.prepared},{"localMatched",result.localMatched},
  {"globalMatched",result.globalMatched},{"keypoints",result.features.imgKeypoints.size()},{"durationMs",result.durationMs}});
 return result;
}
