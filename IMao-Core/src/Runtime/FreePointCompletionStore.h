#pragma once
#include "FarmCompletionStore.h"
#include "RoutePlanningModel.h"

// Local-only ledger, deliberately absent from MarkerCompletionStore and its outbox.
class FreePointCompletionStore {
public:
    explicit FreePointCompletionStore(std::filesystem::path root, FarmCompletionStore::EpochSource epoch = {})
        :root_(std::move(root)),epoch_(std::move(epoch)) {}
    bool Completed(const std::string& profile, const ItemDatas& item) {
        std::scoped_lock lock(mutex_);Select(profile);Expire();
        return document_.at(Table(item)).contains(AutoRoute::Key(item));
    }
    void Set(const std::string& profile, const ItemDatas& item, bool value) {
        if(!AutoRoute::IsFreeStop(item)||item.freeRouteId.empty()||item.itemId.empty()||item.layer.stateId<=0)
            throw std::invalid_argument("自由点身份无效");
        std::scoped_lock lock(mutex_);Select(profile);Expire();
        auto next=document_;auto& table=next[Table(item)];
        const auto key=AutoRoute::Key(item);
        if(value) table[key]=true;else table.erase(key);
        if(next==document_)return;
        WriteTextAtomically(Path(),next.dump(2));document_=std::move(next);
    }
    bool TakeExpired(const std::string& profile) {
        std::scoped_lock lock(mutex_);Select(profile);Expire();
        return std::exchange(expired_,false);
    }
    void RemoveRoute(const std::string& profile,const std::string& routeId) {
        if(routeId.empty()||!std::all_of(routeId.begin(),routeId.end(),[](unsigned char c){return std::isalnum(c)||c=='-'||c=='_';}))
            throw std::invalid_argument("invalid-route");
        std::scoped_lock lock(mutex_);Select(profile);
        auto next=document_;const auto prefix="free-route:"+routeId+":";
        for(const auto* name:{"permanent","daily"}) {
            auto& table=next[name];
            for(auto it=table.begin();it!=table.end();) {
                const auto separator=it.key().find(':');
                if(separator!=std::string::npos&&it.key().compare(separator+1,prefix.size(),prefix)==0)it=table.erase(it);
                else ++it;
            }
        }
        if(next==document_)return;
        WriteTextAtomically(Path(),next.dump(2));document_=std::move(next);
    }
private:
    std::filesystem::path root_;
    FarmCompletionStore::EpochSource epoch_;
    std::mutex mutex_;
    std::string profile_;
    nlohmann::json document_;
    bool expired_=false;
    std::int64_t Epoch() const { return epoch_?epoch_():FarmCompletionStore::CurrentEpoch(); }
    static const char* Table(const ItemDatas& item) {
        return item.freeCategory==FreePointCategory::Collectible?"permanent":"daily";
    }
    std::filesystem::path Path() const { return root_/"profiles"/(profile_+".free.json"); }
    void Select(const std::string& profile) {
        if(profile==profile_)return;
        if(profile.empty()||profile.size()>96||!std::all_of(profile.begin(),profile.end(),[](unsigned char c){
            return std::isalnum(c)||c=='-'||c=='_';}))throw std::invalid_argument("invalid-profile");
        auto next=nlohmann::json{{"schemaVersion",1},{"profileId",profile},{"epoch",Epoch()},
            {"permanent",nlohmann::json::object()},{"daily",nlohmann::json::object()}};
        const auto path=root_/"profiles"/(profile+".free.json");
        if(std::filesystem::exists(path)) {
            std::ifstream input(path);auto loaded=nlohmann::json::parse(input);
            if(loaded.value("schemaVersion",0)!=1||loaded.value("profileId",std::string{})!=profile||
                !loaded.at("permanent").is_object()||!loaded.at("daily").is_object()||!loaded.at("epoch").is_number_integer())
                throw std::runtime_error("自由点完成记录损坏，请恢复本地备份");
            next=std::move(loaded);
        }
        profile_=profile;document_=std::move(next);expired_=false;
    }
    void Expire() {
        const auto now=Epoch();if(document_.at("epoch")==now)return;
        document_["daily"]=nlohmann::json::object();document_["epoch"]=now;expired_=true;
        // Reads remain correct even if a transient file lock prevents persisting the reset.
        try{WriteTextAtomically(Path(),document_.dump(2));}catch(const std::exception&){}
    }
};
