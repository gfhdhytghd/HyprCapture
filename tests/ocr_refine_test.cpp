#include "shared/ocr_refine.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>

using namespace hyprcapture;
namespace {
std::vector<OcrPoint> box(double x0,double y0,double x1,double y1) {
    return {{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
}
OcrResponse fixture() {
    OcrResponse response;
    response.warning="preserved warning"; response.apiFormat="simple";
    response.results.push_back({"中 文",box(0,0,44,12),.9,OcrLevel::Line,
        {{"中",box(2,0,21,12),.8},{"文",box(24,0,43,12),.7}}});
    return response;
}
// Serialize ALL metadata and polygons to check atomic no-op failures.
nlohmann::json encode(const OcrResponse& response) {
    auto polygon=[](const auto& points) {
        nlohmann::json out=nlohmann::json::array();
        for (auto p : points) out.push_back({p.x,p.y});
        return out;
    };
    nlohmann::json out={{"error",response.error},{"warning",response.warning},{"apiFormat",response.apiFormat},{"results",nlohmann::json::array()}};
    for (const auto& line : response.results) {
        nlohmann::json row={{"text",line.text},{"poly",polygon(line.poly)},{"level",int(line.level)},{"words",nlohmann::json::array()}};
        if (line.conf) row["conf"]=*line.conf;
        for (const auto& w : line.words) {
            nlohmann::json word={{"text",w.text},{"poly",polygon(w.poly)}};
            if (w.conf) word["conf"]=*w.conf;
            row["words"].push_back(word);
        }
        out["results"].push_back(row);
    }
    return out;
}
void checkEdges(const OcrResponse& response,double a,double b,double c,double d) {
    const auto& line=response.results[0];
    assert(line.words.size()==2);
    assert(std::abs(line.words[0].poly[0].x-a)<1e-6);
    assert(std::abs(line.words[0].poly[1].x-b)<1e-6);
    assert(std::abs(line.words[1].poly[0].x-c)<1e-6);
    assert(std::abs(line.words[1].poly[1].x-d)<1e-6);
}
}
int main() {
    std::vector<std::uint8_t> gray(44*12,255);
    for (int y=2;y<10;++y) for (int x=0;x<44;++x)
        if ((x>=5&&x<18)||(x>=25&&x<38)) gray[y*44+x]=0;
    const OcrImageView view{gray.data(),44,12,44};
    auto response=fixture(),before=response;
    const auto ranges=ocrWordRanges(response.results[0].text,response.results[0].words);
    refineOcrWordBoxes(view,response); checkEdges(response,5,18,25,38);
    assert(ocrWordRanges(response.results[0].text,response.results[0].words)==ranges);
    assert(response.warning==before.warning&&response.apiFormat==before.apiFormat);
    assert(response.results[0].conf==before.results[0].conf);
    assert(response.results[0].words[0].conf==before.results[0].words[0].conf);
    for (auto& pixel : gray) pixel=255-pixel;
    response=fixture(); refineOcrWordBoxes(view,response); checkEdges(response,5,18,25,38);
    for (auto& pixel : gray) pixel=255-pixel;
    // Padded RGBA stride and noisy gradient background have the same ink edges.
    std::vector<std::uint8_t> rgba(192*12,255);
    for (int y=0;y<12;++y) for (int x=0;x<44;++x) {
        const auto value=gray[y*44+x]?180+x+((x*7+y*13)%11):20+((x+y)%7);
        for (int channel=0;channel<3;++channel) rgba[y*192+x*4+channel]=value;
    }
    response=fixture(); refineOcrWordBoxes({rgba.data(),44,12,192,OcrPixelFormat::Rgba8},response);
    checkEdges(response,5,18,25,38);
    // Partly out-of-line word boxes are clamped, while the line remains intact.
    response=fixture(); response.results[0].words[0].poly=box(-5,-3,21,14);
    refineOcrWordBoxes(view,response); checkEdges(response,5,18,25,38);
    response=fixture(); response.results[0].words.clear();
    refineOcrWordBoxes(view,response); checkEdges(response,5,18,25,38);
    // Vertical detector quad reads from top to bottom. The output retains its
    // full cross-line extent and starts at the reading top-right corner.
    std::vector<std::uint8_t> vertical(12*44,255);
    for (int y=0;y<44;++y) for (int x=2;x<10;++x)
        if ((y>=5&&y<18)||(y>=25&&y<38)) vertical[y*12+x]=0;
    response=fixture(); response.results[0].poly=box(0,0,12,44);
    response.results[0].words[0].poly=box(0,2,12,21);
    response.results[0].words[1].poly=box(0,24,12,43);
    refineOcrWordBoxes({vertical.data(),12,44,12},response);
    assert(response.results[0].words[0].poly[0].x==12);
    assert(std::abs(response.results[0].words[0].poly[0].y-5)<1e-6);
    assert(std::abs(response.results[0].words[0].poly[1].y-18)<1e-6);
    // A real projective trapezoid; render two solid glyphs by the inverse map.
    auto point=[](double u,double v) -> OcrPoint { return {(10+80*u)/(1+.2*u),(10+16*v+3*u)/(1+.2*u)}; };
    auto quad=[&](double a,double b) { return std::vector<OcrPoint>{point(a,0),point(b,0),point(b,1),point(a,1)}; };
    std::vector<std::uint8_t> skew(100*40,255);
    for (int y=0;y<40;++y) for (int x=0;x<100;++x) {
        const double u=(x+.5-10)/(80-.2*(x+.5));
        const double v=((y+.5)*(1+.2*u)-10-3*u)/16;
        if (v>.15&&v<.85&&((u>=.12&&u<.4)||(u>=.58&&u<.85))) skew[y*100+x]=0;
    }
    response=fixture(); response.results[0].poly=quad(0,1);
    response.results[0].words[0].poly=quad(.05,.45); response.results[0].words[1].poly=quad(.55,.95);
    refineOcrWordBoxes({skew.data(),100,40,100},response);
    assert(std::hypot(response.results[0].words[0].poly[0].x-point(.12,0).x,
                      response.results[0].words[0].poly[0].y-point(.12,0).y)<2);
    assert(std::abs(response.results[0].words[1].poly[1].x-point(.85,0).x)<2);
    // Touching ink has no blank column. A thin bridge provides a minimum-ink
    // split, and must not cause token loss or overlapping output intervals.
    for (int x=18;x<25;++x) gray[6*44+x]=0;
    response=fixture(); refineOcrWordBoxes(view,response);
    assert(response.results[0].words.size()==2);
    assert(response.results[0].words[0].poly[1].x<=response.results[0].words[1].poly[0].x);
    assert(response.results[0].words[0].poly[0].x==5&&response.results[0].words[1].poly[1].x==38);
    // Blank/ambiguous images and malformed inputs are complete no-ops.
    auto noOp=[&](OcrImageView image,OcrResponse bad) {
        const auto original=encode(bad); refineOcrWordBoxes(image,bad); assert(encode(bad)==original);
    };
    std::vector<std::uint8_t> blank(44*12,255),checker(44*12);
    for (std::size_t i=0;i<checker.size();++i) checker[i]=(i%2)*255;
    noOp({blank.data(),44,12,44},fixture()); noOp({checker.data(),44,12,44},fixture());
    noOp({},fixture()); noOp({gray.data(),44,12,43},fixture());
    noOp({gray.data(),44,12,44,static_cast<OcrPixelFormat>(99)},fixture());
    response=fixture(); response.error="engine failed"; noOp(view,response);
    response=fixture(); response.results[0].poly=box(0,0,0,12); noOp(view,response);
    response=fixture(); response.results[0].poly=box(-1,0,44,12); noOp(view,response);
    response=fixture(); response.results[0].words[1].text="unmatched"; noOp(view,response);
    response=fixture(); response.results[0].words[1].poly=box(10,2,10,8); noOp(view,response);
    response=fixture(); response.results[0].words[1].poly[0].x=std::numeric_limits<double>::infinity(); noOp(view,response);
    response=fixture(); response.results[0].poly[2].y=1e7; noOp(view,response);
    response=fixture(); response.results[0].level=OcrLevel::Word; noOp(view,response);
    std::cout<<"OCR refinement: polarity, RGBA/stride, gradient/noise, tiny text, vertical, perspective, touching ink, fallback, clamping, atomic no-op passed\n";
}
