#include "MapUiStateController.h"

MapUiState MapUiStateController::Classify(const MapUiEvidence& evidence) {
    if (evidence.bigMapConfirmed) return MapUiState::BigMap;
    if (evidence.minimapVisible) return MapUiState::Gameplay;
    return MapUiState::Unknown;
}

MapUiStateUpdate MapUiStateController::Update(const MapUiEvidence& evidence) {
    MapUiStateUpdate update;
    update.previous = state_;
    update.current = state_;
    update.observed = Classify(evidence);

    if (update.observed == MapUiState::Unknown) {
        if (pending_ != MapUiState::Unknown) {
            pending_ = MapUiState::Unknown;
            pendingFrames_ = 1;
        }
        else {
            ++pendingFrames_;
        }
        // A capture can lose HUD features during an animation or one bad DXGI
        // frame. Treat a missing minimap as an actual HUD disappearance only
        // after ten observations. This protects localization state only;
        // current-frame visibility independently suppresses cached markers.
        // 跑图状态下保留 10 帧防抖以免单帧漏检导致小地图缓存闪断；
        // 已在大地图内点击切换地图进入转场动画时，收窄到 4 帧尽快清空旧图，避免转场期继续发无效的旧图 Local512。
        const int unknownThreshold = (state_ == MapUiState::BigMap) ? 4 : 10;
        if (state_ != MapUiState::Unknown && pendingFrames_ >= unknownThreshold) {
            state_ = MapUiState::Unknown;
            update.current = state_;
            update.changed = true;
        }
        return update;
    }

    if (pending_ != update.observed) {
        pending_ = update.observed;
        pendingFrames_ = 1;
    }
    else {
        ++pendingFrames_;
    }

    // 同时命中罗盘模板验证与缩放条等强特征时允许单帧直通大地图，常规证据仍保持 2 帧防抖。
    const int requiredFrames = (update.observed == MapUiState::BigMap && evidence.strongBigMapConfirmed) ? 1 : 2;
    if (pendingFrames_ >= requiredFrames && state_ != update.observed) {
        state_ = update.observed;
        update.current = state_;
        update.changed = true;
    }
    return update;
}

void MapUiStateController::Reset() {
    state_ = MapUiState::Unknown;
    pending_ = MapUiState::Unknown;
    pendingFrames_ = 0;
}

const char* MapUiStateController::StateName(MapUiState state) {
    switch (state) {
    case MapUiState::Unknown: return "Unknown";
    case MapUiState::Gameplay: return "Gameplay";
    case MapUiState::EnteringBigMap: return "EnteringBigMap";
    case MapUiState::BigMap: return "BigMap";
    case MapUiState::LeavingBigMap: return "LeavingBigMap";
    }
    return "Unknown";
}
