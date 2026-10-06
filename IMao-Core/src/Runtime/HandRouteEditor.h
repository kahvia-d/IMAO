#pragma once
#include "RoutePlanningModel.h"
#include <map>

namespace AutoRoute {
inline const HandRouteNode* FindHandNode(const HandRouteEditor& editor,const std::string& id) {
    const auto found=std::find_if(editor.nodes.begin(),editor.nodes.end(),[&](const auto& n){return n.id==id;});
    return found==editor.nodes.end()?nullptr:&*found;
}
// Validate the entire forest, including disconnected fragments. Empty order means it
// has no single navigable chain, rather than silently choosing the first fragment.
inline std::vector<std::string> HandRouteOrder(const HandRouteEditor& editor) {
    if(editor.nodes.size()>MaxTargets)throw std::invalid_argument("单条路线最多 500 点");
    std::unordered_set<std::string> ids,points;
    for(const auto& node:editor.nodes)
        if(node.id.empty()||!ids.insert(node.id).second||!points.insert(Key(node.point)).second)
            throw std::invalid_argument("编辑点位身份无效或重复");
    std::map<std::string,std::string> next,previous;
    for(const auto& edge:editor.edges) {
        if(edge.from==edge.to||!ids.contains(edge.from)||!ids.contains(edge.to)||
            !next.emplace(edge.from,edge.to).second||!previous.emplace(edge.to,edge.from).second)
            throw std::invalid_argument("路线不能自连接或分叉");
    }
    std::vector<std::string> order;std::size_t chains=0,visited=0;
    for(const auto& [from,to]:next)if(!previous.contains(from)) {
        ++chains;std::string id=from;
        std::vector<std::string> chain{id};
        while(next.contains(id)){++visited;id=next.at(id);chain.push_back(id);}
        if(chains==1)order=std::move(chain);
    }
    if(visited!=editor.edges.size())throw std::invalid_argument("路线不能形成闭环");
    return chains==1?order:std::vector<std::string>{};
}
inline std::vector<ItemDatas> HandRouteStops(const HandRouteEditor& editor) {
    std::vector<ItemDatas> points;
    for(const auto& id:HandRouteOrder(editor))points.push_back(FindHandNode(editor,id)->point);
    return points;
}
inline HandRouteEditor EditorFromPlan(const Plan& plan) {
    if(plan.handEditor)return *plan.handEditor;
    HandRouteEditor editor;
    for(const auto& point:plan.stops) {
        const auto id="n"+std::to_string(editor.nextNodeId++);
        if(!editor.nodes.empty())editor.edges.push_back({editor.nodes.back().id,id});
        editor.nodes.push_back({id,point});
    }
    for(const auto& node:editor.nodes)if(IsFreeStop(node.point)&&node.point.itemId.starts_with("free:")&&node.point.itemId.size()>5&&
        std::all_of(node.point.itemId.begin()+5,node.point.itemId.end(),[](unsigned char c){return std::isdigit(c);})) {
        try{editor.nextFreeId=std::max(editor.nextFreeId,static_cast<std::size_t>(std::stoull(node.point.itemId.substr(5)))+1);}catch(const std::exception&){}
    }
    return editor;
}
}
