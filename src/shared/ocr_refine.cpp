#include "shared/ocr_refine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>

namespace hyprcapture {
namespace {
double length(OcrPoint a, OcrPoint b) { return std::hypot(a.x-b.x, a.y-b.y); }
double cross(OcrPoint a, OcrPoint b, OcrPoint c) {
    return (b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
}
// Projective unit square -> reading quad, with its inverse for input words.
struct Projective {
    std::array<double, 9> m{}, inverse{};
    explicit Projective(const std::array<OcrPoint, 4>& p) {
        const double dx=p[0].x-p[1].x+p[2].x-p[3].x, dy=p[0].y-p[1].y+p[2].y-p[3].y;
        const double ax=p[1].x-p[2].x, bx=p[3].x-p[2].x;
        const double ay=p[1].y-p[2].y, by=p[3].y-p[2].y;
        const double det=ax*by-bx*ay;
        const double g=(dx*by-bx*dy)/det, h=(ax*dy-dx*ay)/det;
        m={p[1].x-p[0].x+g*p[1].x, p[3].x-p[0].x+h*p[3].x, p[0].x,
           p[1].y-p[0].y+g*p[1].y, p[3].y-p[0].y+h*p[3].y, p[0].y, g,h,1};
        const auto& a=m;
        inverse={a[4]*a[8]-a[5]*a[7], a[2]*a[7]-a[1]*a[8], a[1]*a[5]-a[2]*a[4],
                 a[5]*a[6]-a[3]*a[8], a[0]*a[8]-a[2]*a[6], a[2]*a[3]-a[0]*a[5],
                 a[3]*a[7]-a[4]*a[6], a[1]*a[6]-a[0]*a[7], a[0]*a[4]-a[1]*a[3]};
    }
    static OcrPoint map(const std::array<double,9>& a, double x, double y) {
        const double d=a[6]*x+a[7]*y+a[8];
        return {(a[0]*x+a[1]*y+a[2])/d, (a[3]*x+a[4]*y+a[5])/d};
    }
    OcrPoint point(double x,double y) const { return map(m,x,y); }
};
struct Interval { double left, right; };
std::vector<OcrWord> estimates(const OcrResult& line, const Projective& frame) {
    // ICU and Live Text use UAX #29 graphemes and the same 1:2 width model.
    if (line.text.size()>OcrMaxBytes) return {};
    UErrorCode error=U_ZERO_ERROR;
    UText utf8=UTEXT_INITIALIZER;
    utext_openUTF8(&utf8,line.text.data(),line.text.size(),&error);
    UBreakIterator* iterator=ubrk_open(UBRK_CHARACTER,"root",nullptr,0,&error);
    if (iterator&&U_SUCCESS(error)) ubrk_setUText(iterator,&utf8,&error);
    struct Token { std::size_t begin,end; double left,right; bool latin; };
    std::vector<Token> tokens;
    double total=0;
    if (iterator&&U_SUCCESS(error)) {
        int32_t begin=ubrk_first(iterator);
        for (int32_t end=ubrk_next(iterator);end!=UBRK_DONE;begin=end,end=ubrk_next(iterator)) {
            int32_t at=begin; UChar32 c=0;
            U8_NEXT(line.text.data(),at,end,c);
            const double left=total; total+=ocrWideCharacter(c)?1.:.5;
            const bool latin=(u_isalnum(c)&&!ocrWideCharacter(c));
            if (u_isUWhiteSpace(c)) continue;
            if (!tokens.empty()&&latin&&tokens.back().latin&&tokens.back().end==std::size_t(begin)) {
                tokens.back().end=end; tokens.back().right=total;
            } else tokens.push_back({std::size_t(begin),std::size_t(end),left,total,latin});
            if (tokens.size()>OcrMaxWords) { tokens.clear(); break; }
        }
    }
    if (iterator) ubrk_close(iterator);
    utext_close(&utf8);
    std::vector<OcrWord> words;
    if (total>0) for (const auto& token : tokens) {
        const double lo=token.left/total,hi=token.right/total;
        words.push_back({line.text.substr(token.begin,token.end-token.begin),
                         {frame.point(lo,0),frame.point(hi,0),frame.point(hi,1),frame.point(lo,1)}, {}});
    }
    return words;
}
int otsu(const std::array<std::size_t,256>& histogram, std::size_t count) {
    double total=0;
    for (int i=0;i<256;++i) total+=i*double(histogram[i]);
    double sum=0,best=-1; std::size_t weight=0; int threshold=0;
    for (int i=0;i<255;++i) {
        weight+=histogram[i]; sum+=i*double(histogram[i]);
        if (!weight||weight==count) continue;
        const double delta=sum/weight-(total-sum)/(count-weight);
        const double variance=double(weight)*double(count-weight)*delta*delta;
        if (variance>best) { best=variance; threshold=i; }
    }
    return threshold;
}
bool refineLine(OcrImageView image,OcrResult& line,std::size_t& budget,std::size_t& alignmentBudget) {
    if (line.level!=OcrLevel::Line||line.text.empty()) return false;
    const auto quad=ocrReadingQuad(line.poly);
    if (!quad) return false;
    // Never invent pixels beyond the supplied image.
    for (auto p : *quad) if (p.x<0||p.y<0||p.x>image.width||p.y>image.height) return false;
    const double nativeWidth=std::max(length((*quad)[0],(*quad)[1]),length((*quad)[3],(*quad)[2]));
    const double nativeHeight=std::max(length((*quad)[0],(*quad)[3]),length((*quad)[1],(*quad)[2]));
    if (nativeWidth<2||nativeHeight<2||nativeWidth>32768||nativeHeight>32768) return false;
    const int width=std::max(2,int(std::round(nativeWidth))),height=std::max(2,int(std::round(nativeHeight)));
    const std::size_t count=std::size_t(width)*height;
    if (count>budget) return false;
    budget-=count;
    const Projective frame(*quad);
    // A convex quad inside the image maps the entire strip inside the image.
    // Reject ill-conditioned transforms once, rather than on every sample.
    for (const auto value : frame.m) if (!std::isfinite(value)) return false;
    for (const auto corner : std::array<OcrPoint,4>{{{0,0},{1,0},{1,1},{0,1}}}) {
        if (frame.m[6]*corner.x+frame.m[7]*corner.y+1<1e-8) return false;
        const auto p=frame.point(corner.x,corner.y);
        if (!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<-.01||p.y<-.01||p.x>image.width+.01||p.y>image.height+.01) return false;
    }
    auto words=line.words.empty()?estimates(line,frame):line.words;
    if (words.empty()||words.size()>OcrMaxWords||!ocrWordRanges(line.text,words)) return false;
    std::vector<Interval> input;
    for (const auto& word : words) {
        if (word.poly.size()<3||word.poly.size()>OcrMaxPoints) return false;
        double area=0;
        for (std::size_t j=0;j<word.poly.size();++j) {
            const auto a=word.poly[j],b=word.poly[(j+1)%word.poly.size()];
            area+=a.x*b.y-a.y*b.x;
        }
        if (!std::isfinite(area)||area<=1e-6) return false;
        double lo=1e100,hi=-1e100;
        for (auto p : word.poly) {
            if (!std::isfinite(p.x)||!std::isfinite(p.y)) return false;
            const auto unit=Projective::map(frame.inverse,p.x,p.y);
            if (!std::isfinite(unit.x)||!std::isfinite(unit.y)) return false;
            lo=std::min(lo,unit.x*width); hi=std::max(hi,unit.x*width);
        }
        lo=std::clamp(lo,0.,double(width)); hi=std::clamp(hi,0.,double(width));
        if (hi-lo<.25||(!input.empty()&&(lo+hi<=input.back().left+input.back().right))) return false;
        input.push_back({lo,hi});
    }
    std::array<std::size_t,256> histogram{};
    std::vector<std::uint8_t> strip(count);
    for (int y=0;y<height;++y) {
        const auto& m=frame.m;
        const double v=(y+.5)/height;
        double nx=m[0]*.5/width+m[1]*v+m[2],ny=m[3]*.5/width+m[4]*v+m[5],d=m[6]*.5/width+m[7]*v+1;
        const double sx=m[0]/width,sy=m[3]/width,sd=m[6]/width;
        for (int x=0;x<width;++x,nx+=sx,ny+=sy,d+=sd) {
            const double inverse=1/d;
            const int px=std::clamp(int(nx*inverse),0,image.width-1),py=std::clamp(int(ny*inverse),0,image.height-1);
            const auto* pixel=image.data+std::size_t(py)*image.stride+std::size_t(px)*(image.format==OcrPixelFormat::Gray8?1:4);
            const auto gray=image.format==OcrPixelFormat::Gray8?pixel[0]:std::uint8_t((77*pixel[0]+150*pixel[1]+29*pixel[2]+128)>>8);
            strip[std::size_t(y)*width+x]=gray; ++histogram[gray];
        }
    }
    const int threshold=otsu(histogram,count);
    const auto dark=std::accumulate(histogram.begin(),histogram.begin()+threshold+1,std::size_t(0));
    const bool darkInk=dark<count-dark;
    const double density=double(std::min(dark,count-dark))/count;
    if (density<=.01||density>=.49) return false;
    std::vector<int> ink(width,0);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x)
        ink[x]+=(strip[std::size_t(y)*width+x]<=threshold)==darkInk;
    std::vector<double> boundaries{0};
    auto snap = [&] {
        for (std::size_t i=1;i<input.size();++i) {
            const double edge=(input[i-1].right+input[i].left)*.5;
            const double radius=std::max(2.,.45*std::min(input[i-1].right-input[i-1].left,input[i].right-input[i].left));
            const int low=std::max(0,int(std::floor(edge-radius))),high=std::min(width,int(std::ceil(edge+radius))+1);
            double bestDistance=1e100,boundary=edge;
            for (int x=low;x<high;) {
                if (ink[x]) { ++x; continue; }
                const int begin=x;
                while (x<high&&!ink[x]) ++x;
                const double middle=(begin+x)*.5;
                if (std::abs(middle-edge)<bestDistance) { bestDistance=std::abs(middle-edge); boundary=middle; }
            }
            if (bestDistance==1e100) {
                int minimum=height+1;
                for (int x=low;x<high;++x) if (ink[x]<minimum||(ink[x]==minimum&&std::abs(x+.5-edge)<bestDistance)) {
                    minimum=ink[x]; bestDistance=std::abs(x+.5-edge); boundary=x+.5;
                }
            }
            if (boundary<=boundaries.back()||boundary>=width) return false;
            boundaries.push_back(boundary);
        }
        boundaries.push_back(width);
        return true;
    };
    auto tighten = [&](bool checkNarrow) {
        for (std::size_t i=0;i<words.size();++i) {
            int lo=std::clamp(int(std::ceil(boundaries[i])),0,width),hi=std::clamp(int(std::ceil(boundaries[i+1])),0,width);
            while (lo<hi&&!ink[lo]) ++lo;
            while (hi>lo&&!ink[hi-1]) --hi;
            if (hi<=lo) return false;
            if (checkNarrow && input[i].right-input[i].left<height*.35 && hi-lo>2*(input[i].right-input[i].left)) return false;
            words[i].poly={frame.point(double(lo)/width,0),frame.point(double(hi)/width,0),
                           frame.point(double(hi)/width,1),frame.point(double(lo)/width,1)};
        }
        return true;
    };
    bool success=!line.words.empty()&&snap()&&tighten(true);
    if (!success) {
        // Missing words have no alignment signal beyond the 1:2 model. Also,
        // very narrow native boxes can put BOTH midpoint searches beyond a
        // punctuation mark. Recover a monotonic alignment of occupied runs.
        // This is bounded and uses no engine/font-specific offsets.
        std::vector<Interval> runs;
        for (int x=0;x<width;) {
            if (!ink[x]) { ++x; continue; }
            const int begin=x;
            while (x<width&&ink[x]) ++x;
            runs.push_back({double(begin),double(x)});
        }
        const auto n=words.size(),m=runs.size();
        if (m<n||n>1024||m>4096||n*m>1024*1024) return false;
        std::vector<double> gapCost(m,0);
        for (std::size_t j=1;j<m;++j) {
            const double gap=(runs[j].left-runs[j-1].right)/height;
            gapCost[j]=gapCost[j-1]+gap*gap;
        }
        const double inf=1e100;
        std::vector<double> previous(m+1,inf),next(m+1,inf);
        std::vector<std::size_t> parent((n+1)*(m+1));
        previous[0]=0;
        for (std::size_t i=0;i<n;++i) {
            std::fill(next.begin(),next.end(),inf);
            double weight=0; int characters=0; bool alnum=false;
            for (int32_t at=0;at<int32_t(words[i].text.size());) {
                UChar32 c; U8_NEXT(words[i].text.data(),at,int32_t(words[i].text.size()),c);
                if (u_charType(c)==U_NON_SPACING_MARK) continue;
                ++characters; weight+=ocrWideCharacter(c)?1.:.5; alnum|=u_isalnum(c);
            }
            const double target=line.words.empty()?height*(alnum?weight*.9:.18):input[i].right-input[i].left;
            const double scale=std::max(height*.5,target);
            const double center=(input[i].left+input[i].right)*.5;
            // At most 4 projected components per codepoint, plus a small
            // allowance for disconnected radicals. This bounds DP work.
            const std::size_t limit=std::min<std::size_t>(m,4*characters+4);
            const std::size_t transitions=m*limit;
            if (transitions>alignmentBudget) return false;
            alignmentBudget-=transitions;
            const double inverseScale=1/scale,inverseHeight=1./height,inverseParts=1./std::max(1,characters);
            for (std::size_t end=i+1;end<=m-(n-i-1);++end) {
                const auto first=end>limit?end-limit:0;
                for (auto begin=std::max(i,first);begin<end;++begin) {
                    if (previous[begin]==inf) continue;
                    const double span=runs[end-1].right-runs[begin].left;
                    const double widthError=(span-target)*inverseScale;
                    const double centerError=((runs[begin].left+runs[end-1].right)*.5-center)*inverseHeight;
                    const double partsError=(double(end-begin)-characters)*inverseParts;
                    const double cost=previous[begin]+4*widthError*widthError+
                        (line.words.empty()?.005:.25)*centerError*centerError+.2*partsError*partsError+
                        8*(gapCost[end-1]-gapCost[begin]);
                    if (cost<next[end]) { next[end]=cost; parent[(i+1)*(m+1)+end]=begin; }
                }
            }
            previous.swap(next);
        }
        if (previous[m]==inf) return false;
        boundaries.assign(n+1,0); boundaries[n]=width;
        auto end=m;
        for (auto i=n;i>0;--i) {
            const auto begin=parent[i*(m+1)+end];
            if (i>1) boundaries[i-1]=(runs[begin-1].right+runs[begin].left)*.5;
            end=begin;
        }
        if (!tighten(false)) return false;
    }
    line.words=std::move(words);
    return true;
}
} // namespace

std::vector<OcrWord> estimatedOcrWords(const OcrResult& line) {
    const auto quad=ocrReadingQuad(line.poly);
    return quad?estimates(line,Projective(*quad)):std::vector<OcrWord>{};
}
bool ocrWideCharacter(char32_t c) {
    return (c>=0x1100&&c<=0x11ff)||(c>=0x2e80&&c<=0xa4cf)||(c>=0xac00&&c<=0xd7af)||
           (c>=0xf900&&c<=0xfaff)||(c>=0xfe10&&c<=0xfe6f)||(c>=0xff01&&c<=0xff60)||c>=0x1f000;
}
std::optional<std::array<OcrPoint,4>> ocrReadingQuad(const std::vector<OcrPoint>& polygon) {
    if (polygon.size()!=4) return {};
    std::array<OcrPoint,4> p;
    std::copy(polygon.begin(),polygon.end(),p.begin());
    for (std::size_t i=0;i<4;++i) {
        if (!std::isfinite(p[i].x)||!std::isfinite(p[i].y)||std::abs(p[i].x)>1e7||std::abs(p[i].y)>1e7||
            cross(p[i],p[(i+1)%4],p[(i+2)%4])<1e-6) return {};
    }
    const double width=std::max(length(p[0],p[1]),length(p[3],p[2]));
    const double height=std::max(length(p[0],p[3]),length(p[1],p[2]));
    if (width<1e-6||height<1e-6) return {};
    if (height>=1.5*width) std::rotate(p.begin(),p.begin()+1,p.end());
    return p;
}
void refineOcrWordBoxes(OcrImageView image,OcrResponse& response) {
    const auto channels=image.format==OcrPixelFormat::Gray8?1u:4u;
    if ((image.format!=OcrPixelFormat::Gray8&&image.format!=OcrPixelFormat::Rgba8)||!response||!image.data||image.width<=0||image.height<=0||image.width>100000||image.height>100000||
        image.stride<std::size_t(image.width)*channels||
        image.stride>std::numeric_limits<std::size_t>::max()/std::size_t(image.height)||response.results.size()>OcrMaxResults) return;
    std::size_t budget=32*1024*1024; // Bound work and allocation for hostile geometry.
    std::size_t alignmentBudget=8*1024*1024;
    for (auto& line : response.results) refineLine(image,line,budget,alignmentBudget);
}
} // namespace hyprcapture
