#pragma once
#include "Feature/Match/EncodedExactMatcher.h"
#include "Feature/Match/FeatureRowCache.h"
#include "Runtime/FrameStorageAccounting.h"
#include <numeric>
#include <cstring>
#include <chrono>
#include <iostream>

template<class Check> void TestEncodedCandidates(Check check) {
    ImageFeatureData source;
    cv::Mat backing(9004,132,CV_8U); cv::RNG(417).fill(backing,cv::RNG::UNIFORM,0,256);
    source.imgDescriptors=backing(cv::Rect(2,2,128,9000));
    source.descriptorEncoding={{4500,1.f/4096,-128},{9000,1.f/2048,-64}};
    for(int i=0;i<9000;++i) source.imgKeypoints.emplace_back(float(i),float(i%73),1.f);
    source.imgKeypoints[4096]=source.imgKeypoints[0];
    source.imgDescriptors.row(0).copyTo(source.imgDescriptors.row(4096));
    std::vector<uint32_t> rows(9000);std::iota(rows.begin(),rows.end(),0);
    rows.push_back(8999); rows.push_back(0);
    auto plain=UniqueFeatureSubset(source,rows), encoded=UniqueEncodedFeatureSubset(source,rows);
    check(encoded.imgDescriptors.type()==CV_8UC1,"candidate subset keeps original encoded bytes");
    const auto expanded=encoded.FloatDescriptors();
    check(expanded.size()==plain.imgDescriptors.size() && std::memcmp(expanded.data,plain.imgDescriptors.data,expanded.total()*sizeof(float))==0,
        "encoded candidate deduplication, row order and decoding preserve every float bit");
    check(encoded.imgKeypoints.size()==plain.imgKeypoints.size(),"encoded candidate keeps every unique keypoint");
    for(size_t i=0;i<plain.imgKeypoints.size();++i)
        check(encoded.imgKeypoints[i].pt==plain.imgKeypoints[i].pt,"encoded candidate geometry order is unchanged");
    cv::Mat query=plain.imgDescriptors.rowRange(0,36).clone(); query.row(0).copyTo(query.row(1));
    std::vector<std::vector<cv::DMatch>> a,b,c,d;
    MatchDescriptorsExactly(plain.imgDescriptors,query,a,b);
    MatchDescriptorsExactly(encoded,query,c,d);
    const auto equal=[](const auto& x,const auto& y) {
        if(x.size()!=y.size())return false;
        for(size_t i=0;i<x.size();++i){if(x[i].size()!=y[i].size())return false;
            for(size_t j=0;j<x[i].size();++j)if(x[i][j].queryIdx!=y[i][j].queryIdx || x[i][j].trainIdx!=y[i][j].trainIdx ||
                std::memcmp(&x[i][j].distance,&y[i][j].distance,sizeof(float)))return false;}
        return true;
    };
    check(equal(a,c)&&equal(b,d),"encoded matching keeps exact forward/reverse distances, IDs and ties");
    {
        FrameStorage::Allocator census(cv::Mat::getDefaultAllocator(),nullptr,true);
        const auto restore=wil::scope_exit([&]{cv::Mat::setDefaultAllocator(census.Base());});
        cv::Mat::setDefaultAllocator(&census);
        MatchDescriptorsExactly(source,query,c,d);
        check(census.Read().matPeakBytes<=3*1024*1024,"encoded matching only expands bounded batches, never the full candidate matrix");
        check(census.Read().matLiveBytes==0,"encoded matcher releases its actual decode and distance buffers");
        bool cancelled=false;int calls=0;
        try{MatchDescriptorsExactly(source,query,c,d,[&]{return ++calls==2;});}catch(const SearchInterrupted&){cancelled=true;}
        check(cancelled&&census.Read().matLiveBytes==0,"encoded matching cancellation releases scratch buffers");
        calls=0; cancelled=false;
        try{MatchDescriptorsExactly(source,query,c,d,[&]{return ++calls==10;});}catch(const SearchInterrupted&){cancelled=true;}
        check(cancelled&&census.Read().matLiveBytes==0,"encoded cancellation after a completed distance batch releases scratch");
        ImageFeatureData invalid=source; invalid.descriptorEncoding={{4096,1.f/4096,-128}};
        bool rejected=false;
        try{MatchDescriptorsExactly(invalid,query,c,d);}catch(const std::runtime_error&){rejected=true;}
        check(rejected&&census.Read().matLiveBytes==0,"invalid later encoding releases already used decode buffers");
        MatchDescriptorsExactly(source,plain.imgDescriptors.rowRange(0,512),c,d);
        check(census.Read().matPeakBytes<=4*1024*1024,"128-column encoded scratch stays within four MiB at larger query counts");
    }
    std::vector<uint32_t> alternating{4500,2,4501,3,4502,4,4500,2};
    const auto reordered=UniqueEncodedFeatureSubset(source,alternating).FloatDescriptors();
    const auto reorderedPlain=UniqueFeatureSubset(source,alternating).imgDescriptors;
    check(reordered.size()==reorderedPlain.size() && std::memcmp(reordered.data,reorderedPlain.data,reordered.total()*sizeof(float))==0,
        "encoding segments follow reordered mixed-scale candidate rows");
    FeatureRowCache floatCache(source,6000), byteCache(source,6000,true);
    std::vector<uint32_t> first{0,1,2,3,4,5,6,7},second{8,9,10,11,12,13,14,15};
    auto f=floatCache.Get(first), e=byteCache.Get(first);
    check(e->imgDescriptors.type()==CV_8UC1 && byteCache.Get(first)==e,"runtime encoded cache stores compact immutable hits");
    floatCache.Get(second);byteCache.Get(second);
    check(floatCache.Get(first)!=f && byteCache.Get(first)!=e,"encoded cache preserves float-equivalent admission and eviction policy");
    ImageFeatureData legacy=source;legacy.imgDescriptors=source.FloatDescriptors();legacy.descriptorEncoding.clear();
    FeatureRowCache fallback(legacy,6000,true);
    check(fallback.Get(first)->imgDescriptors.type()==CV_32FC1,"legacy float source remains compatible");
}

// Alternating order limits drift from CPU frequency and background load.
inline int BenchmarkEncodedCandidates() {
    cv::setNumThreads(4);
    for (int count : {1024, 8192, 32768}) {
        ImageFeatureData source;
        source.imgDescriptors.create(count,128,CV_8U);
        cv::RNG(417).fill(source.imgDescriptors,cv::RNG::UNIFORM,0,256);
        source.descriptorEncoding={{count,1.f/4096,-128}};
        for(int i=0;i<count;++i) source.imgKeypoints.emplace_back(float(i),float(i%73),1.f);
        std::vector<uint32_t> rows(count); std::iota(rows.begin(),rows.end(),0);
        const auto plain=UniqueFeatureSubset(source,rows), encoded=UniqueEncodedFeatureSubset(source,rows);
        for(int queries : {36,128,512}) {
            cv::Mat query=plain.imgDescriptors.rowRange(0,queries).clone();
            std::vector<std::vector<cv::DMatch>> a,b;
            std::vector<double> times[2];
            for(int repeat=0;repeat<14;++repeat) for(int order=0;order<2;++order) {
                const int mode=(repeat+order)%2;
                const auto start=std::chrono::steady_clock::now();
                if(mode) MatchDescriptorsExactly(encoded,query,a,b);
                else MatchDescriptorsExactly(plain.imgDescriptors,query,a,b);
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                if(repeat>=2) times[mode].push_back(ms);
            }
            for(auto& values:times) std::sort(values.begin(),values.end());
            std::cout << "encoded-benchmark rows=" << count << " queries=" << queries
                << " floatMedianMs=" << times[0][times[0].size()/2]
                << " encodedMedianMs=" << times[1][times[1].size()/2]
                << " floatBytes=" << plain.imgDescriptors.total()*plain.imgDescriptors.elemSize()
                << " encodedBytes=" << encoded.imgDescriptors.total()*encoded.imgDescriptors.elemSize() << "\n";
        }
    }
    return 0;
}
