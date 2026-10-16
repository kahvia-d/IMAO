#include "YoloCoordinateDecoder.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <limits>

namespace CoordinateYolo {
namespace {
double V(const Json& o, int i) { return o.at("xyxy").at(i).get<double>(); }
double Center(const Json& o) { return (V(o,0)+V(o,2))/2; }
double Score(const Json& o) { return o.at("score").get<double>(); }
std::string Char(const Json& o) { return o.at("char").get<std::string>(); }
bool Digit(const Json& o) { const auto c=Char(o); return c.size()==1 && c[0]>='0' && c[0]<='9'; }
bool Sign(const Json& o) { return Char(o)=="-" || Char(o)=="+"; }
bool Valid(const Json& o) {
    if (!std::isfinite(Score(o)) || o.at("xyxy").size()!=4) return false;
    for (int i=0;i<4;++i) if (!std::isfinite(V(o,i))) return false;
    return V(o,2)>V(o,0) && V(o,3)>V(o,1);
}
double IoU(const Json& a,const Json& b) {
    const double area=std::max(0.,std::min(V(a,2),V(b,2))-std::max(V(a,0),V(b,0)))*
        std::max(0.,std::min(V(a,3),V(b,3))-std::max(V(a,1),V(b,1)));
    const double sum=(V(a,2)-V(a,0))*(V(a,3)-V(a,1))+(V(b,2)-V(b,0))*(V(b,3)-V(b,1))-area;
    return sum>0 ? area/sum : 0.;
}
double Median(std::vector<double> x) {
    std::sort(x.begin(),x.end()); const auto n=x.size();
    return n%2 ? x[n/2] : (x[n/2-1]+x[n/2])/2;
}
std::vector<Json> Unique(const Json& items) {
    std::vector<Json> sorted, unique;
    for (const auto& o:items) if (Score(o)>=.25) sorted.push_back(o);
    std::stable_sort(sorted.begin(),sorted.end(),[](const auto&a,const auto&b){return Score(a)>Score(b);});
    for (const auto&o:sorted) {
        bool duplicate=false;
        for (const auto&p:unique) if (Char(o)==Char(p)&&IoU(o,p)>.45) duplicate=true;
        if (!duplicate) unique.push_back(o);
    }
    std::stable_sort(unique.begin(),unique.end(),[](const auto&a,const auto&b){return Center(a)<Center(b);});
    return unique;
}
using Group=std::vector<Json>;
std::vector<Group> Groups(const std::vector<Json>& items,double width) {
    std::vector<Group> groups; double right=0;
    for (const auto&o:items) {
        if (groups.empty()||V(o,0)-right>std::max(12.,.75*width)) {groups.push_back({});right=V(o,2);}
        else right=std::max(right,V(o,2));
        groups.back().push_back(o);
    }
    return groups;
}
std::string Text(const Group& g) {std::string s;for(const auto&o:g)s+=Char(o);return s;}
bool Continuous(const Group& g,double width) {
    for(size_t i=1;i<g.size();++i)if(Center(g[i])-Center(g[i-1])>1.8*width)return false;
    return true;
}
bool Conflicts(const Group& g) {
    for(size_t i=0;i<g.size();++i)for(size_t j=i+1;j<g.size();++j)
        if(Char(g[i])!=Char(g[j])&&IoU(g[i],g[j])>.45)return true;
    return false;
}
std::optional<int> Integer(const std::string& s) {
    if(!std::regex_match(s,std::regex("[+-]?[0-9]+")))return {};
    try {const auto v=std::stoll(s);if(v>=INT32_MIN&&v<=INT32_MAX)return static_cast<int>(v);}catch(...){}
    return {};
}
double MinScore(const Group& g) {double s=1;for(const auto&o:g)s=std::min(s,Score(o));return s;}
bool Dominant(const Json& digit,const Json& confirmation) {
    const Json* best=nullptr;
    for(const auto&o:confirmation)if(Score(o)>=.25&&IoU(o,digit)>=.5&&(!best||Score(o)>Score(*best)))best=&o;
    if(!best||Char(*best)!=Char(digit))return false;
    for(const auto&o:confirmation)if(Score(o)>=.25&&IoU(o,digit)>=.5&&Char(o)!=Char(*best)&&Score(*best)<2*Score(o))return false;
    return true;
}
Json Resolve(const Json& raw,const Json& contrast) {
    std::vector<double> commas;
    for(const auto&o:raw)if(Char(o)==","&&Score(o)>=.25)commas.push_back(Center(o));
    if(commas.size()<2)return raw;
    std::sort(commas.begin(),commas.end());std::vector<size_t> order;std::vector<bool> removed(raw.size());
    for(size_t k=0;k<raw.size();++k)order.push_back(k);
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return Score(raw[a])>Score(raw[b]);});
    for(auto k:order) {
        const auto&o=raw[k];if(removed[k]||!Digit(o)||Score(o)<.25||Center(o)>=commas[1])continue;
        std::vector<size_t> alternatives;
        for(size_t j=0;j<raw.size();++j)if(!removed[j]&&Digit(raw[j])&&Char(raw[j])!=Char(o)&&Score(raw[j])>=.25&&IoU(raw[j],o)>=.5)alternatives.push_back(j);
        if(alternatives.empty())continue;
        bool strong=true;for(auto j:alternatives)if(Score(o)<2*Score(raw[j]))strong=false;
        if(strong&&Dominant(o,contrast))for(auto j:alternatives)removed[j]=true;
    }
    Json result=Json::array();for(size_t k=0;k<raw.size();++k)if(!removed[k])result.push_back(raw[k]);return result;
}
bool Stroke(const cv::Mat& im,const Json& sign) {
    const int a=static_cast<int>(std::floor(V(sign,0))), b=static_cast<int>(std::ceil(V(sign,2)));
    const int t=static_cast<int>(std::floor(V(sign,1))), d=static_cast<int>(std::ceil(V(sign,3)));
    if(V(sign,0)<0||V(sign,1)<0||V(sign,2)>im.cols||V(sign,3)>im.rows||b-a<3||d-t<3)return false;
    cv::Mat gray(d-t,b-a,CV_32F);
    for(int y=t;y<d;++y)for(int x=a;x<b;++x) {
        const auto p=im.at<cv::Vec3b>(y,x);gray.at<float>(y-t,x-a)=(p[0]+p[1]+p[2])/3.f;
    }
    double low,high;cv::minMaxLoc(gray,&low,&high);double row=0,col=0;
    for(int y=0;y<gray.rows-1;++y)for(int x=0;x<gray.cols;++x)row+=std::abs(gray.at<float>(y+1,x)-gray.at<float>(y,x));
    for(int y=0;y<gray.rows;++y)for(int x=0;x<gray.cols-1;++x)col+=std::abs(gray.at<float>(y,x+1)-gray.at<float>(y,x));
    row/=((gray.rows-1)*gray.cols);col/=(gray.rows*(gray.cols-1));
    if(high-low<4||row<1||row<1.5*col)return false;
    std::vector<double> profile(gray.rows-1);
    for(int y=0;y<gray.rows-1;++y)for(int x=1;x<gray.cols-1;++x)profile[y]+=(gray.at<float>(y+1,x)-gray.at<float>(y,x))/(gray.cols-2);
    const int top=static_cast<int>(std::max_element(profile.begin(),profile.end())-profile.begin());
    const int bottom=static_cast<int>(std::min_element(profile.begin(),profile.end())-profile.begin());
    const double floor=std::max(1.5,.08*(high-low));
    if(profile[top]<floor||profile[bottom]>-floor||std::abs(top-bottom)<1)return false;
    int ts=0,bs=0;
    for(int x=1;x<gray.cols-1;++x){ts+=gray.at<float>(top+1,x)-gray.at<float>(top,x)>=1.5;bs+=gray.at<float>(bottom+1,x)-gray.at<float>(bottom,x)<=-1.5;}
    if(std::min(ts,bs)/double(gray.cols-2)<.60)return false;
    const int margin=std::max(2,static_cast<int>(std::nearbyint((b-a)*.2)));
    if(a-margin<0||b+margin>im.cols)return false;
    double outside=0;
    for(int start:{a-margin,b}) {
        double sum=0;
        for(int y:{top,bottom})for(int x=start;x<start+margin;++x) {
            const auto p=im.at<cv::Vec3b>(t+y,x),q=im.at<cv::Vec3b>(t+y+1,x);
            sum+=std::abs((int(q[0])+q[1]+q[2]-int(p[0])-p[1]-p[2])/3.f);
        }
        outside=std::max(outside,sum/(2*margin));
    }
    const double inside=(std::abs(profile[top])+std::abs(profile[bottom]))/2;
    return inside>=1.3*outside&&inside-outside>=1;
}
Json Geometry(const Json& raw,const Json& selected) {
    const auto unique=Unique(raw);std::vector<double> widths;
    for(const auto&o:unique)if(Digit(o))widths.push_back(V(o,2)-V(o,0));
    if(widths.empty())return nullptr;
    const double width=Median(widths);Json match=nullptr;int count=0;
    for(const auto& g:Groups(unique,width)) {
        std::vector<size_t> commas;for(size_t i=0;i<g.size();++i)if(Char(g[i])==",")commas.push_back(i);
        if(commas.size()<2)continue;
        Group prefix(g.begin(),g.begin()+commas[1]+1);
        if(Text(prefix)!=selected["textXY"].get<std::string>()+",")continue;
        Group x,y;
        for(size_t i=0;i<commas[0];++i)if(Digit(g[i]))x.push_back(g[i]);
        for(size_t i=commas[0]+1;i<commas[1];++i)if(Digit(g[i]))y.push_back(g[i]);
        if(x.empty()||y.empty())continue;
        match={{"width",width},{"x",(Center(x.front())-4)/width},{"y",(Center(y.front())-Center(x.back()))/width},
            {"xs",Sign(g.front())},{"ys",Sign(g[commas[0]+1])},{"xd",x.front()},{"yd",y.front()}};++count;
    }
    return count==1?match:Json(nullptr);
}
// Return suspect axes; null means an unexplained layout and therefore abstention.
Json Audit(const Json& raw,const Json& selected) {
    if(selected.is_null())return nullptr;
    const auto f=Geometry(raw,selected);if(f.is_null())return nullptr;
    const double width=f["width"];
    if(width<19.53681640625||width>31.200585937499998)return Json::array();
    Json suspects=Json::array();
    for(int axis=0;axis<2;++axis) {
        if(f[axis==0?"xs":"ys"].get<bool>())continue;
        const double value=f[axis==0?"x":"y"];
        const double ulo=axis==0?1.5611089586260667:1.3089860670310716, uhi=axis==0?1.9318369967347224:1.620053439764766;
        const double slo=axis==0?2.239551485178347:1.8723558009895704, shi=axis==0?2.6686694289991504:2.2941243508495077;
        if(value>=ulo&&value<=uhi)continue;
        if(value<slo||value>shi)return nullptr;
        suspects.push_back(axis);
    }
    return suspects;
}
}

Json Decode(const Json& detections) {
    for(const auto&o:detections)if(!Valid(o)||Char(o).size()!=1||std::string("0123456789-,+:").find(Char(o))==std::string::npos)return nullptr;
    const auto unique=Unique(detections);std::vector<double> widths;
    for(const auto&o:unique)if(Digit(o))widths.push_back(V(o,2)-V(o,0));
    if(widths.empty())return nullptr;
    const double width=Median(widths);const auto groups=Groups(unique,width);Json candidates=Json::array();
    for(size_t index=0;index<groups.size();++index) {
        const auto&g=groups[index];std::vector<size_t> commas;
        for(size_t i=0;i<g.size();++i)if(Char(g[i])==",")commas.push_back(i);
        if(commas.size()<2)continue;
        const size_t end=commas[1]+1;Group prefix(g.begin(),g.begin()+end),suffix(g.begin()+end,g.end());
        for(size_t i=0;i<end;++i)for(size_t j=0;j<g.size();++j)
            if(i!=j&&Char(g[i])!=Char(g[j])&&IoU(g[i],g[j])>.45)return nullptr;
        std::smatch match;const auto pt=Text(prefix);
        if(!std::regex_match(pt,match,std::regex("([+-]?[0-9]+),([+-]?[0-9]+),")))continue;
        for(size_t c=2;c<commas.size();++c) {
            bool alternative=false;for(const auto&o:suffix)if(Char(o)!=","&&IoU(g[commas[c]],o)>.45)alternative=true;
            if(!alternative)return nullptr;
        }
        if(!Continuous(prefix,width))return nullptr;
        if(index>0) {
            const auto&p=groups[index-1];double right=0;for(const auto&o:p)right=std::max(right,V(o,2));
            if(std::regex_match(Text(p),std::regex("[+-]?[0-9]+|[+-]"))&&V(g.front(),0)-right<=2*width)return nullptr;
        }
        const auto x=Integer(match[1]),y=Integer(match[2]);if(!x||!y)continue;
        std::optional<int> z;
        if(!suffix.empty()&&!Conflicts(suffix)&&Continuous(suffix,width)) {
            z=Integer(Text(suffix));
            if(index+1<groups.size()) {
                const auto&next=groups[index+1];double right=0;for(const auto&o:g)right=std::max(right,V(o,2));
                if(std::regex_match(Text(next),std::regex("[+-]?[0-9]+"))&&V(next.front(),0)-right<=2*width)z.reset();
            }
        }
        Json candidate={{"xy",{*x,*y}},{"z",z?Json(*z):Json(nullptr)},{"xyScore",MinScore(prefix)},
            {"textXY",pt.substr(0,pt.size()-1)},{"observedGroupText",Text(g)},
            {"zScore",z?Json(MinScore(suffix)):Json(nullptr)}};
        candidates.push_back(candidate);
    }
    return candidates.size()==1?candidates[0]:Json(nullptr);
}

Json Select(const Json& raw,const Json& contrast,const cv::Mat& contrastImage,bool calibrated) {
    for(const auto*route:{&raw,&contrast})for(const auto&o:*route)if(!Valid(o))return nullptr;
    Json resolved=Resolve(raw,contrast), selected=Decode(resolved);
    if(selected.is_null())return nullptr;
    if(calibrated) {
        const auto axes=Audit(resolved,selected);if(axes.is_null())return nullptr;
        if(!axes.empty()) {
            if(contrastImage.size()!=cv::Size(892,48)||contrastImage.type()!=CV_8UC3)return nullptr;
            const auto f=Geometry(resolved,selected);const double width=f["width"];Json additions=resolved;
            auto expected=selected["xy"];
            for(const auto&axisValue:axes) {
                const int axis=axisValue.get<int>();if(expected[axis].get<int>()<=0)return nullptr;
                const auto&digit=f[axis==0?"xd":"yd"];const double h=V(digit,3)-V(digit,1);
                const double cx=Center(digit)-.84*width,cy=V(digit,1)+.63*h;
                bool neighbor=false;
                for(const auto&o:contrast)if(Score(o)>=.25&&IoU(o,digit)>=.5) {
                    if(Char(o)!=Char(digit))return nullptr;
                    if(Score(o)>=.5)neighbor=true;
                }
                if(!neighbor)return nullptr;
                Json signs=Json::array();
                for(const auto&o:contrast) {
                    const double w=V(o,2)-V(o,0),height=V(o,3)-V(o,1);
                    if(Char(o)!="-"||Score(o)<.5||w<.35*width||w>.95*width||height<.10*h||height>.40*h||
                        std::abs(Center(o)-cx)>.25*width||std::abs((V(o,1)+V(o,3))/2-cy)>.15*h||
                        V(o,0)<V(digit,0)-1.05*width||V(o,2)>V(digit,0)+.10*width)continue;
                    bool conflict=false;for(const auto&p:contrast)if(Score(p)>=.25&&Char(p)!="-"&&IoU(p,o)>.45)conflict=true;
                    if(!conflict&&Stroke(contrastImage,o))signs.push_back(o);
                }
                const auto unique=Unique(signs);if(unique.size()!=1)return nullptr;
                additions.push_back(unique[0]);expected[axis]=-expected[axis].get<int>();
            }
            selected=Decode(additions);if(selected.is_null()||selected["xy"]!=expected)return nullptr;
            const auto post=Audit(additions,selected);if(post.is_null()||!post.empty())return nullptr;
        }
    }
    // Validate every mandatory observed digit on the independent preprocessing route.
    const auto base=Decode(resolved);if(base.is_null())return nullptr;
    const auto unique=Unique(resolved);std::vector<double> widths;
    for(const auto&o:unique)if(Digit(o))widths.push_back(V(o,2)-V(o,0));
    bool any=false;
    for(const auto&group:Groups(unique,Median(widths))) {
        if(Text(group)!=base["observedGroupText"].get<std::string>())continue;
        int commas=0;
        for(const auto&o:group) {
            if(Char(o)==","&&++commas==2)break;
            if(Digit(o)){any=true;if(!Dominant(o,contrast))return nullptr;}
        }
    }
    return any?selected:Json(nullptr);
}
}
