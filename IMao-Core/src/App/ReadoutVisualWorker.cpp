#include "ReadoutVisualWorker.h"
#include "../Coordinate/VisualLocalization/RecoveryPolicy.h"
ReadoutVisualWorker::ReadoutVisualWorker(std::weak_ptr<LocalizationSignal> signal)
 :worker_([this](ReadoutVisualRequest request){return Compute(std::move(request));},
  [signal]{if(auto wake=signal.lock())wake->NotifyResult();}){}
ReadoutVisualResult ReadoutVisualWorker::Compute(ReadoutVisualRequest request){
 const auto start=std::chrono::steady_clock::now();ReadoutVisualResult result;result.source=std::move(request);
 const auto& source=result.source;
 if(!source.resources||!source.resources->visualIndexReady)throw std::runtime_error("readout visual resources unavailable");
 if(resources_!=source.resources){context_.reset();resources_=source.resources;context_=std::make_unique<VisualTrackingContext>(resources_);}
 result.prepared=GlobalVisualLocalizer::PrepareMinimap(source.minimap,result.normalized,result.features,
  source.trustedReference.empty()?nullptr:&source.trustedReference,&result.diagnostics);
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
 return result;
}
