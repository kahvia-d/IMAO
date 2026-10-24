#include "Diagnostics/ReproCapture.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <Windows.h>
#include <compressapi.h>
using namespace ReproCapture;
void check(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
Json read(const std::filesystem::path& p) { std::ifstream f(p); Json j; f >> j; return j; }
std::vector<Json> records(const std::filesystem::path& p) { std::ifstream f(p / "records.jsonl"); std::vector<Json> r; std::string line; while(std::getline(f,line)) r.push_back(Json::parse(line)); return r; }
int main(int argc, char** argv) {
 try {
    if((argc == 4 || argc == 5) && std::string(argv[1]) == "--benchmark") {
        const int offeredFrames=argc==5?std::stoi(argv[4]):20; check(offeredFrames>0&&offeredFrames<=10000,"bounded benchmark frame count");
        auto source = cv::imread(argv[2], cv::IMREAD_UNCHANGED);
        check(!source.empty(), "benchmark source decoded");
        if(source.channels() == 3) cv::cvtColor(source,source,cv::COLOR_BGR2BGRA);
        {
            auto began=std::chrono::steady_clock::now(); size_t raw=source.total()*source.elemSize();
            std::vector<std::vector<uchar>> encoded(4); std::vector<std::thread> workers; std::vector<SIZE_T> sizes(4);
            for(size_t i=0;i<4;++i) workers.emplace_back([&,i] {
                size_t from=raw*i/4,to=raw*(i+1)/4; encoded[i].resize((to-from)*2+65536);
                COMPRESSOR_HANDLE c=nullptr; check(CreateCompressor(COMPRESS_ALGORITHM_XPRESS,nullptr,&c)!=FALSE,"chunk compressor");
                check(Compress(c,source.data+from,to-from,encoded[i].data(),encoded[i].size(),&sizes[i])!=FALSE,"chunk compressed"); CloseCompressor(c);
            });
            for(auto& worker:workers)worker.join(); size_t size=0;for(auto n:sizes)size+=n;
            std::cout<<Json({{"codec","xpress-four-chunks"},{"encodeMs",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count()},{"encodedBytes",size}}).dump()<<'\n';
        }
        for(DWORD algorithm : {COMPRESS_ALGORITHM_XPRESS,COMPRESS_ALGORITHM_XPRESS_HUFF}) {
            COMPRESSOR_HANDLE compressor=nullptr; DECOMPRESSOR_HANDLE decompressor=nullptr;
            check(CreateCompressor(algorithm,nullptr,&compressor)!=FALSE,"create OS compressor");
            check(CreateDecompressor(algorithm,nullptr,&decompressor)!=FALSE,"create OS decompressor");
            auto began=std::chrono::steady_clock::now();
            std::vector<uchar> bytes(source.total()*source.elemSize()*2+65536); SIZE_T written=0;
            check(Compress(compressor,source.data,source.total()*source.elemSize(),bytes.data(),bytes.size(),&written)!=FALSE,"OS compression");
            auto encodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
            std::vector<uchar> decoded(source.total()*source.elemSize()); SIZE_T out=0;
            check(Decompress(decompressor,bytes.data(),written,decoded.data(),decoded.size(),&out)!=FALSE && out==decoded.size() && std::memcmp(source.data,decoded.data(),decoded.size())==0,"OS compression lossless");
            CloseCompressor(compressor); CloseDecompressor(decompressor);
            std::cout << Json({{"codec",algorithm==COMPRESS_ALGORITHM_XPRESS?"xpress":"xpress-huff"},{"encodeMs",encodeMs},{"encodedBytes",written}}).dump() << '\n';
        }
        for(const auto& mode : std::vector<std::pair<std::string,std::vector<int>>>{{"opencv-default",{}},{"level0",{cv::IMWRITE_PNG_COMPRESSION,0}},{"level1-rle",{cv::IMWRITE_PNG_COMPRESSION,1,cv::IMWRITE_PNG_STRATEGY,cv::IMWRITE_PNG_STRATEGY_RLE}}}) {
            auto begin=std::chrono::steady_clock::now(); std::vector<uchar> encoded;
            cv::imencode(".png",source,encoded,mode.second);
            auto decoded=cv::imdecode(encoded,cv::IMREAD_UNCHANGED);
            check(cv::norm(source,decoded,cv::NORM_INF)==0,"benchmark PNG lossless readback");
            std::cout << Json({{"codec",mode.first},{"encodeAndVerifyMs",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()},{"encodedBytes",encoded.size()}}).dump() << '\n';
        }
        check(Start(std::filesystem::u8path(argv[3]), {{"benchmark",true},{"cadenceMs",80},{"offeredFrames",offeredFrames}}), "benchmark start");
        auto began=std::chrono::steady_clock::now(); size_t peak=0; double maxSubmitMs=0;
        for(int i=0;i<offeredFrames;++i) {
            std::this_thread::sleep_until(began+std::chrono::milliseconds(i*80));
            source.at<cv::Vec4b>(0,0)[0] = static_cast<uchar>(i);
            Scope frame({{"frameId",i+1},{"sourceSequence",i+1}});
            auto submitted=std::chrono::steady_clock::now(); Image("wgc",source);
            maxSubmitMs=(std::max)(maxSubmitMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-submitted).count());
            peak=(std::max)(peak,Status()["queuedBytes"].get<size_t>());
        }
        Stop(); Shutdown(); auto result=Status(); result["peakQueuedBytes"]=peak; result["maxSubmitMs"]=maxSubmitMs;
        result["totalWallMs"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
        result["width"]=source.cols; result["height"]=source.rows; result["channels"]=source.channels();
        std::cout << result.dump(2) << '\n'; return result["state"] == "completed" && result["savedImages"] == offeredFrames ? 0 : 2;
    }
    const auto root = std::filesystem::temp_directory_path() / L"IMao-repro-测试";
    check(Start(root), "start session");
    check(!Start(root), "reject overlapping session");
    cv::Mat source(17, 19, CV_8UC3, cv::Scalar(1, 22, 203));
    { Scope outer({{"frameId", 7}, {"requestId", 9}});
      Image("ownership-png", source); source.setTo(cv::Scalar(0,0,0));
      { Scope inner({{"requestId", 10}}); Record("ocr"); }
      Record("restored");
      std::thread other([] { Record("other"); }); other.join();
    }
    Log({{"text", "event"}}); Stop(); Shutdown();
    const auto s = Status(); check(s.at("state") == "completed", "completed after drain");
    check(s["elapsedMs"].is_number_integer() && s["durationMs"].is_number_integer(), "status milliseconds compatible with managed Int64");
    const auto first = std::filesystem::u8path(s.at("directory").get<std::string>());
    auto r = records(first); check(r.size() == 5, "all ordered records persisted");
    check(r[0]["context"]["frameId"] == 7 && r[1]["context"]["requestId"] == 10 && r[2]["context"]["requestId"] == 9, "nested correlation");
    check(r[3]["context"].empty() && r[4]["context"].empty(), "thread local context");
    std::ifstream png(first / std::filesystem::u8path(r[0]["image"]["path"].get<std::string>()), std::ios::binary);
    std::vector<uchar> bytes((std::istreambuf_iterator<char>(png)), {});
    auto decoded = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    check(decoded.size() == cv::Size(19,17) && decoded.at<cv::Vec3b>(0,0) == cv::Vec3b(1,22,203), "lossless owned clone and unicode path");
    const auto before = std::filesystem::file_size(first / "records.jsonl"); Record("late"); Shutdown();
    check(std::filesystem::file_size(first / "records.jsonl") == before, "no writes after completion");
    Options shortSession; shortSession.duration = std::chrono::milliseconds(40);
    check(Start(root, Json::object(), shortSession), "restart");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    check(!Active(), "duration stops without frames"); Shutdown();
    check(Status()["state"] == "completed" && Status()["directory"] != s["directory"] && Status()["savedImages"] == 0, "restart resets and unique directory");
    Options tiny; tiny.maxQueuedBytes = 1;
    check(Start(root, Json::object(), tiny), "tiny queue start"); Image("overflow", source); Shutdown();
    check(Status()["state"] == "failed" && Status()["missingImages"] == 1 && Status()["missingRecords"] == 1, "queue overflow explicitly fails");
    auto failed = std::filesystem::u8path(Status()["directory"].get<std::string>());
    check(read(failed / "completion.json")["complete"] == false, "incomplete manifest");
    Options disk; disk.maxDiskBytes = 1024;
    check(Start(root, Json::object(), disk), "disk-limited start");
    Image("disk-overflow", cv::Mat(256,256,CV_8UC3,cv::Scalar(4,5,6))); Shutdown();
    check(Status()["state"] == "failed" && Status()["missingImages"] == 1 && !Status()["error"].get<std::string>().empty(), "disk exhaustion explicit failure");
    check(Start(root), "concurrent producers session");
    std::vector<std::thread> producers;
    for(int t=0;t<4;++t) producers.emplace_back([t] { Scope scope({{"producer",t}}); for(int i=0;i<50;++i) { Record("parallel",{{"n",i}}); check(Status()["queuedBytes"].get<size_t>() <= 256*1024*1024, "queue remains bounded"); } });
    for(auto& producer:producers) producer.join(); Stop(); Shutdown();
    check(Status()["state"] == "completed" && Status()["savedRecords"] == 200 && Status()["queuedBytes"] == 0, "concurrent producers drain every accepted record");
    auto parallel = records(std::filesystem::u8path(Status()["directory"].get<std::string>()));
    for(size_t i=0;i<parallel.size();++i) check(parallel[i]["recordId"] == i+1, "record order monotonic");
    check(Start(root), "empty image session"); Image("empty",{}); Shutdown();
    check(Status()["state"] == "failed" && Status()["missingImages"] == 1, "empty image explicitly incomplete");
    Options invalid; invalid.duration = std::chrono::milliseconds(0); check(!Start(root, Json::object(), invalid), "invalid options rejected");
    for(int type:{CV_8UC1,CV_8UC3,CV_8UC4}) {
        cv::Mat original(128,137,type); cv::randu(original,0,256);
        check(Start(root),"WGC compression session"); Image("wgc",original); Stop(); Shutdown();
        auto path=std::filesystem::u8path(Status()["directory"].get<std::string>());
        auto entry=records(path).at(0); check(entry["image"]["encoding"]=="windows-xpress-chunks-v1","WGC uses lossless chunked XPRESS");
        std::ifstream file(path/std::filesystem::u8path(entry["image"]["path"].get<std::string>()),std::ios::binary);
        std::string magic(8,'\0');file.read(magic.data(),8);check(magic=="IMAOXPR1","chunk container magic");
        auto number=[&](int bytes){uint64_t n=0;for(int k=0;k<bytes;++k){int c=file.get();check(c!=EOF,"complete chunk header");n|=uint64_t(static_cast<unsigned char>(c))<<(8*k);}return n;};
        auto count=number(4);check(count==4,"four independent compression blocks");std::vector<uchar> restored;
        for(uint64_t k=0;k<count;++k){auto rawSize=number(8),compressedSize=number(8);std::vector<uchar> compressed(compressedSize),raw(rawSize);file.read(reinterpret_cast<char*>(compressed.data()),compressed.size());
            DECOMPRESSOR_HANDLE d=nullptr;check(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS,nullptr,&d)!=FALSE,"test decompressor");SIZE_T size=0;
            check(Decompress(d,compressed.data(),compressed.size(),raw.data(),raw.size(),&size)!=FALSE&&size==raw.size(),"chunk decompress");CloseDecompressor(d);restored.insert(restored.end(),raw.begin(),raw.end());}
        check(restored.size()==original.total()*original.elemSize()&&std::memcmp(restored.data(),original.data,restored.size())==0,"WGC byte exact including alpha and all channels");
    }
    check(Start(root),"old scoped generation");
    {
        Scope oldFrame({{"frameId",51}}); Record("old"); Stop(); Shutdown();
        check(Start(root),"new generation while old scope lives"); Image("stale",source);
    }
    { Scope freshFrame({{"frameId",52}}); Record("fresh"); }
    Stop(); Shutdown();
    auto generationRecords=records(std::filesystem::u8path(Status()["directory"].get<std::string>()));
    check(generationRecords.size()==1&&generationRecords[0]["stage"]=="fresh","stale scoped producer cannot contaminate restarted session");
    std::cout << "PASS ReproCapture lifecycle, ownership, correlation, Unicode, bounds and completion\n";
    return 0;
 } catch(const std::exception& e) { Shutdown(); std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
