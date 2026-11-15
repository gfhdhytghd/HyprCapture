#include "shared/ocr_refine.hpp"
#include <nlohmann/json.hpp>
#include <png.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace hyprcapture;
using Json=nlohmann::json;
int main(int argc,char** argv) {
    const bool estimatesOnly=argc==4&&std::string_view(argv[1])=="--estimates";
    if (argc!=3&&!estimatesOnly) return 2;
    if (estimatesOnly) ++argv;
    try {
        Json fixtures; std::ifstream(argv[1])>>fixtures;
        Json output=Json::array();
        for (const auto& fixture : fixtures) {
            const std::string file=fixture.at("file");
            const auto path=std::filesystem::path(argv[2])/file;
            png_image png{}; png.version=PNG_IMAGE_VERSION;
            if (!png_image_begin_read_from_file(&png,path.c_str())) throw std::runtime_error(png.message);
            png.format=PNG_FORMAT_RGBA;
            std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(png));
            if (!png_image_finish_read(&png,nullptr,pixels.data(),0,nullptr)) throw std::runtime_error(png.message);
            auto response=parseOcrApiJson(fixture.at("response").dump());
            if (!response) throw std::runtime_error(response.error);
            if (estimatesOnly) for (auto& line : response.results) line.words=estimatedOcrWords(line);
            const auto before=response;
            if (!estimatesOnly) refineOcrWordBoxes({pixels.data(),int(png.width),int(png.height),std::size_t(png.width)*4,OcrPixelFormat::Rgba8},response);
            if (response.error!=before.error||response.warning!=before.warning||response.apiFormat!=before.apiFormat||response.results.size()!=before.results.size())
                throw std::runtime_error("response metadata changed");
            Json rows=Json::array();
            for (std::size_t i=0;i<response.results.size();++i) {
                const auto& line=response.results[i]; const auto& original=before.results[i];
                if (line.text!=original.text||line.conf!=original.conf||line.level!=original.level||line.poly.size()!=original.poly.size())
                    throw std::runtime_error("line metadata changed");
                for (std::size_t j=0;j<line.poly.size();++j) if (line.poly[j].x!=original.poly[j].x||line.poly[j].y!=original.poly[j].y)
                    throw std::runtime_error("line polygon changed");
                if (!original.words.empty()) {
                    if (line.words.size()!=original.words.size()||ocrWordRanges(line.text,line.words)!=ocrWordRanges(original.text,original.words))
                        throw std::runtime_error("token count or ranges changed");
                    for (std::size_t j=0;j<line.words.size();++j) if (line.words[j].text!=original.words[j].text||line.words[j].conf!=original.words[j].conf)
                        throw std::runtime_error("word text/confidence changed");
                }
                Json row={{"text",line.text},{"box",Json::array()},{"words",Json::array()}};
                for (auto p : line.poly) row["box"].push_back({p.x,p.y});
                for (const auto& word : line.words) {
                    Json box=Json::array(); for (auto p : word.poly) box.push_back({p.x,p.y});
                    row["words"].push_back({{"text",word.text},{"box",box}});
                }
                rows.push_back(row);
            }
            Json item={{"file",file},{"response",{{"results",rows}}}};
            if (!estimatesOnly&&file=="dense.png") {
                std::vector<double> times;
                for (int i=0;i<101;++i) {
                    auto sample=before;
                    const auto start=std::chrono::steady_clock::now();
                    refineOcrWordBoxes({pixels.data(),int(png.width),int(png.height),std::size_t(png.width)*4,OcrPixelFormat::Rgba8},sample);
                    const auto finish=std::chrono::steady_clock::now();
                    times.push_back(std::chrono::duration<double,std::milli>(finish-start).count());
                }
                std::sort(times.begin(),times.end());
                item["timing"]={{"medianMs",times[50]},{"p95Ms",times[95]},{"maxMs",times.back()},{"iterations",times.size()}};
            }
            png_image_free(&png);
            output.push_back(item);
        }
        std::cout<<output.dump()<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
