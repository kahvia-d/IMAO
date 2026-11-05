#include "YoloCoordinateDecoder.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <limits>

namespace CoordinateYolo {
namespace {
const std::regex kInteger("[+-]?[0-9]+");
const std::regex kCoordinatePrefix("([+-]?[0-9]+),([+-]?[0-9]+),");
const std::regex kNumberOrSign("[+-]?[0-9]+|[+-]");
constexpr double kUnsignedXMin = 1.5611089586260667, kUnsignedXMax = 1.9318369967347224;
constexpr double kSignedXMin = 2.239551485178347, kSignedXMax = 2.6686694289991504;
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
    const auto& left = a.at("xyxy");
    const auto& right = b.at("xyxy");
    const double ax = left.at(0), ar = left.at(2), bx = right.at(0), br = right.at(2);
    const double width = std::min(ar, br) - std::max(ax, bx);
    if (width <= 0) return 0.;
    const double ay = left.at(1), ab = left.at(3), by = right.at(1), bb = right.at(3);
    const double height = std::min(ab, bb) - std::max(ay, by);
    if (height <= 0) return 0.;
    const double area = width * height;
    const double sum = (ar-ax)*(ab-ay) + (br-bx)*(bb-by) - area;
    return sum>0 ? area/sum : 0.;
}
double Median(std::vector<double> x) {
    std::sort(x.begin(),x.end()); const auto n=x.size();
    return n%2 ? x[n/2] : (x[n/2-1]+x[n/2])/2;
}
std::vector<Json> Unique(const Json& items) {
    struct Candidate {
        const Json* box;
        std::string character;
        double score, left, right, center;
        bool digit;
    };
    std::vector<Candidate> sorted, kept;
    sorted.reserve(items.size());
    for (const auto& box : items) {
        const double score = Score(box);
        if (!(score >= .25)) continue;
        const double left = V(box,0), right = V(box,2);
        sorted.push_back({&box, Char(box), score, left, right, (left+right)/2, Digit(box)});
    }
    std::stable_sort(sorted.begin(),sorted.end(),[](const Candidate& a,const Candidate& b){return a.score>b.score;});
    for (const auto& glyph : sorted) {
        bool duplicate = false, strongerLeft = false, strongerRight = false;
        bool closeDigit = false, coversComma = false;
        for (const auto& prior : kept) {
            if (prior.right <= glyph.left || prior.left >= glyph.right) continue;
            if (glyph.digit && prior.character == "," && prior.score > glyph.score &&
                prior.center > glyph.left && prior.center < glyph.right) coversComma = true;
            if (glyph.character != prior.character) continue;
            const double overlap = IoU(*glyph.box,*prior.box);
            if (overlap > .45) { duplicate = true; break; }
            if (!glyph.digit || prior.score <= 2*glyph.score) continue;
            const double totalWidth = (glyph.right-glyph.left)+(prior.right-prior.left);
            if (overlap > .15 && std::abs(glyph.center-prior.center) < .35*totalWidth) closeDigit = true;
            if (overlap > .25) {
                strongerLeft |= prior.center < glyph.center;
                strongerRight |= prior.center > glyph.center;
            }
        }
        // A weak box straddling two digits, or a digit and its comma, cannot
        // be counted as another copy of the same digit.
        if (!duplicate && !(strongerLeft && strongerRight) && !(closeDigit && coversComma))
            kept.push_back(glyph);
    }
    std::stable_sort(kept.begin(),kept.end(),[](const Candidate& a,const Candidate& b){return a.center<b.center;});
    std::vector<Json> unique;
    unique.reserve(kept.size());
    for (const auto& glyph : kept) unique.push_back(*glyph.box);
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
    if(!std::regex_match(s,kInteger))return {};
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
Json Geometry(const std::vector<Json>& unique,const Json& selected) {
    std::vector<double> widths;
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
Json Audit(const Json& f) {
    if(f.is_null())return nullptr;
    const double width=f["width"];
    if(width<19.53681640625||width>31.200585937499998)return Json::array();
    Json suspects=Json::array();
    for(int axis=0;axis<2;++axis) {
        if(f[axis==0?"xs":"ys"].get<bool>())continue;
        const double value=f[axis==0?"x":"y"];
        const double ulo=axis==0?kUnsignedXMin:1.3089860670310716, uhi=axis==0?kUnsignedXMax:1.620053439764766;
        const double slo=axis==0?kSignedXMin:1.8723558009895704, shi=axis==0?kSignedXMax:2.2941243508495077;
        if(value>=ulo&&value<=uhi)continue;
        if(value<slo||value>shi)return nullptr;
        suspects.push_back(axis);
    }
    return suspects;
}
}

bool HasDigitAtLeftBoundary(const Json& raw, const Json& contrast) {
    for(const auto& digit:raw) {
        if(!Digit(digit) || Score(digit)<.5)continue;
        const double width=V(digit,2)-V(digit,0);
        if(V(digit,0)-4 < .84*width && Dominant(digit,contrast))return true;
    }
    return false;
}

bool HasNominalTextSpan(const Json& detections, int contentWidth) {
    std::vector<Json> boxes;
    std::vector<double> widths;
    for (const auto& box : detections) {
        if (Score(box) < .65) continue;
        boxes.push_back(box);
        if (Digit(box)) widths.push_back(V(box,2)-V(box,0));
    }
    if (widths.empty()) return false;
    const double width = Median(widths);
    std::sort(boxes.begin(),boxes.end(),[](const Json& a,const Json& b){ return Center(a)<Center(b); });
    std::vector<double> advances;
    for (size_t i = 1; i < boxes.size(); ++i) {
        const auto& a = boxes[i-1];
        const auto& b = boxes[i];
        if (!Digit(a) || !Digit(b)) continue;
        const double advance = Center(b)-Center(a);
        if (advance >= .70*width && advance <= 1.35*width) advances.push_back(advance);
    }
    if (advances.size() < 5) return false;
    // The annotated native crops span 34.67 to 36.20 ordinary digit advances.
    // A wider ROI can put an unsigned first digit in the usual minus position.
    const double span = contentWidth / Median(advances);
    return span >= 34.4 && span <= 36.45;
}

std::vector<DigitDispute> FindDigitDisputes(const Json& raw, const Json& contrast) {
    std::vector<DigitDispute> disputes;
    for (const auto& weaker : raw) {
        if (!Digit(weaker)) continue;
        for (const auto& stronger : raw) {
            if (!Digit(stronger) || Char(stronger)==Char(weaker) || Score(stronger)<=Score(weaker) ||
                IoU(stronger,weaker)<.5) continue;
            double same = 0, other = 0;
            for (const auto& box : contrast) {
                if (IoU(box,weaker)<.5) continue;
                if (Char(box)==Char(weaker)) same=std::max(same,Score(box));
                if (Char(box)==Char(stronger)) other=std::max(other,Score(box));
            }
            if (same>0 && other>same)
                disputes.push_back({Char(weaker),Center(weaker),V(weaker,2)-V(weaker,0),
                                    std::min(other,Score(stronger))});
        }
    }
    return disputes;
}

static Json DecodeWithBoxes(const Json& detections, std::vector<Json>& unique) {
    for(const auto&o:detections)if(!Valid(o)||Char(o).size()!=1||std::string("0123456789-,+:").find(Char(o))==std::string::npos)return nullptr;
    unique=Unique(detections);std::vector<double> widths;
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
        if(!std::regex_match(pt,match,kCoordinatePrefix))continue;
        for(size_t c=2;c<commas.size();++c) {
            bool alternative=false;for(const auto&o:suffix)if(Char(o)!=","&&IoU(g[commas[c]],o)>.45)alternative=true;
            if(!alternative)return nullptr;
        }
        if(!Continuous(prefix,width))return nullptr;
        if(index>0) {
            const auto&p=groups[index-1];double right=0;for(const auto&o:p)right=std::max(right,V(o,2));
            if(std::regex_match(Text(p),kNumberOrSign)&&V(g.front(),0)-right<=2*width)return nullptr;
        }
        const auto x=Integer(match[1]),y=Integer(match[2]);if(!x||!y)continue;
        std::optional<int> z;
        if(!suffix.empty()&&!Conflicts(suffix)&&Continuous(suffix,width)) {
            z=Integer(Text(suffix));
            if(index+1<groups.size()) {
                const auto&next=groups[index+1];double right=0;for(const auto&o:g)right=std::max(right,V(o,2));
                if(std::regex_match(Text(next),kInteger)&&V(next.front(),0)-right<=2*width)z.reset();
            }
        }
        Json candidate={{"xy",{*x,*y}},{"z",z?Json(*z):Json(nullptr)},{"xyScore",MinScore(prefix)},
            {"textXY",pt.substr(0,pt.size()-1)},{"observedGroupText",Text(g)},
            {"zScore",z?Json(MinScore(suffix)):Json(nullptr)}};
        candidates.push_back(candidate);
    }
    return candidates.size()==1?candidates[0]:Json(nullptr);
}

Json Decode(const Json& detections) {
    std::vector<Json> boxes;
    return DecodeWithBoxes(detections, boxes);
}

bool CanSelectRaw(const Json& detections) {
    const auto commas = std::count_if(detections.begin(), detections.end(),
        [](const Json& box){return Char(box)==",";});
    if (commas < 2) return false;
    if (!Decode(detections).is_null()) return true;
    // Resolve can change an invalid raw prefix only by removing a different
    // overlapping digit with at most half the stronger digit's score.
    for (const auto& stronger : detections) {
        if (!Digit(stronger)) continue;
        for (const auto& weaker : detections)
            if (Score(stronger)>=2*Score(weaker) && Digit(weaker) && Char(stronger)!=Char(weaker) &&
                IoU(stronger,weaker)>=.5) return true;
    }
    return false;
}

Json Select(const Json& raw,const Json& contrast,const cv::Mat& contrastImage,bool calibrated,
            std::optional<SamplingContext> sampling) {
    for(const auto*route:{&raw,&contrast})for(const auto&o:*route)if(!Valid(o))return nullptr;
    const Json resolved=Resolve(raw,contrast);
    std::vector<Json> unique;
    const Json base=DecodeWithBoxes(resolved,unique);
    if(base.is_null())return nullptr;
    Json selected = base;
    if(calibrated) {
        const auto f=Geometry(unique,selected);
        const auto axes=Audit(f);if(axes.is_null())return nullptr;
        if(!axes.empty()) {
            if(contrastImage.size()!=cv::Size(892,48)||contrastImage.type()!=CV_8UC3)return nullptr;
            const double width=f["width"];Json additions=resolved;
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
            std::vector<Json> recovered;
            selected=DecodeWithBoxes(additions,recovered);if(selected.is_null()||selected["xy"]!=expected)return nullptr;
            const auto post=Audit(Geometry(recovered,selected));if(post.is_null()||!post.empty())return nullptr;
        }
    }
    // Validate every mandatory observed digit on the independent preprocessing route.
    std::vector<double> widths;
    for(const auto&o:unique)if(Digit(o))widths.push_back(V(o,2)-V(o,0));
    bool any=false;
    const double digitWidth=Median(widths);
    for(const auto&group:Groups(unique,digitWidth)) {
        if(Text(group)!=base["observedGroupText"].get<std::string>())continue;
        // Prepare adds four pixels of border. An unsigned X needs enough real
        // image to its left to distinguish it from a minus cut off by the ROI.
        if(!contrastImage.empty() && Digit(group.front()) && selected["xy"][0]>=0 &&
           V(group.front(),0)-4 < .84*digitWidth)return nullptr;
        // Uniform resampling preserves the calibrated first-digit offset in
        // glyph widths once we subtract its horizontal translation. Preserve
        // the unsigned-X audit even when both views miss the same thin minus.
        if (sampling && sampling->checkMargin && Digit(group.front()) && selected["xy"][0] >= 0) {
            const double x = (Center(group.front()) - 4 - sampling->translation) / digitWidth;
            if (sampling->calibrated && (x < kUnsignedXMin || x > kUnsignedXMax)) return nullptr;
            if (!sampling->calibrated && x >= kSignedXMin && x <= kSignedXMax) return nullptr;
        }
        const auto comma=std::find_if(group.begin(),group.end(),[](const Json& o){return Char(o)==",";});
        if(comma!=group.end() && std::next(comma)!=group.end()) {
            const auto& yStart=*std::next(comma);
            // In the training font the comma directly touches an unsigned Y.
            // A sign consumes at least 0.67 digit widths in the source annotations.
            if(Digit(yStart) && selected["xy"][1]>=0 && V(yStart,0)-V(*comma,2)>.4*digitWidth)return nullptr;
        }
        int commas=0;
        for(const auto&o:group) {
            if(Char(o)==","&&++commas==2)break;
            if(Digit(o)){
                any=true;
                if(!Dominant(o,contrast))return nullptr;
                if (sampling && sampling->disputes) {
                    const double sourceX = 4 + (Center(o)-4-sampling->translation)/sampling->horizontalGain;
                    for (const auto& dispute : *sampling->disputes)
                        if (dispute.weakerDigit==Char(o) && Score(o)<dispute.competingScore &&
                            std::abs(sourceX-dispute.center)<.35*dispute.width) return nullptr;
                }
            }
        }
    }
    return any?selected:Json(nullptr);
}
}
