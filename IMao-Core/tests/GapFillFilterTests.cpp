#include "Runtime/GapFillFilter.h"
#include <iostream>
#include <chrono>
#include <stdexcept>
int main() {
    using json=nlohmann::json;
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("gap fill assertion failed");};
    const std::string id="1477783180906614784";
    const std::set<GapFillCatalog::Key> known{{"8",id},{"909",id}};
    auto doc=json{{"schemaVersion",1},{"states",{{"8",json::array({id})}}}};
    auto good=GapFillCatalog::Parse(doc,known);
    check(good.error.empty()&&good.points.contains({"8",id})&&!good.points.contains({"909",id}));
    auto bad=doc;bad["states"]["8"].push_back(id);check(!GapFillCatalog::Parse(bad,known).error.empty());
    bad=doc;bad["states"]["8"][0]=1477783180906614784ULL;check(!GapFillCatalog::Parse(bad,known).error.empty());
    bad=doc;bad["schemaVersion"]=2;check(!GapFillCatalog::Parse(bad,known).error.empty());
    check(!GapFillCatalog::Parse(doc,{}).error.empty());
    check(GapFillFilter::Allows(8,id));const auto revision=GapFillFilter::Revision();
    GapFillFilter::SetEnabled(true);check(GapFillFilter::Revision()>revision);
    check(!GapFillFilter::Allows(8,id)&&!GapFillFilter::Allows(8,id,true));
    const auto root=std::filesystem::temp_directory_path()/std::filesystem::path("imao-gap-filter-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {std::ofstream out(root/"marker-detectability.json");out<<doc;}
    GapFillFilter::Load(root,known);
    check(GapFillFilter::Available()&&GapFillFilter::Allows(8,id)&&!GapFillFilter::Allows(909,id));
    check(!GapFillFilter::Allows(8,id,true)&&!GapFillFilter::Allows(8,"unknown"));
    const auto loaded=GapFillFilter::Revision();
    {std::ofstream out(root/"marker-detectability.json");out<<"invalid";}
    GapFillFilter::Load(root,known);
    check(!GapFillFilter::Available()&&!GapFillFilter::Allows(8,id)&&GapFillFilter::Revision()>loaded);
    std::filesystem::remove_all(root);
    GapFillFilter::SetEnabled(false);check(GapFillFilter::Allows(8,id,true));
    std::cout<<"Gap-fill identity, failure policy and revision OK\n";
}
