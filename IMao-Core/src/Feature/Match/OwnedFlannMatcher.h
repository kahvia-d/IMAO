#pragma once
#include <opencv2/features2d.hpp>
#include <opencv2/flann.hpp>
#include <cmath>
#include <memory>
#include <atomic>

// One immutable, continuous candidate matrix. The Mat reference owns its bytes
// until AFTER the index is destroyed. Candidate builders must never modify it.
// Same KD-tree/L2/checks parameters and DMatch conversion as OpenCV's matcher,
// without DescriptorCollection's merge copy or miniflann's features_clone.
class OwnedFlannMatcher {
public:
    explicit OwnedFlannMatcher(const cv::Mat& descriptors) : data_(descriptors) {
        CV_Assert(!data_.empty() && data_.type()==CV_32FC1 && data_.isContinuous());
        heldDescriptorBytes_=data_.total()*data_.elemSize();
        liveDescriptorBytes_.fetch_add(heldDescriptorBytes_,std::memory_order_relaxed);
        try {
            const cvflann::Matrix<float> view(data_.ptr<float>(),data_.rows,data_.cols);
            index_=std::make_unique<cvflann::Index<cvflann::L2<float>>>(view,cvflann::KDTreeIndexParams(4));
            index_->buildIndex();
            heldIndexBytes_=static_cast<size_t>(index_->usedMemory());
            liveIndexBytes_.fetch_add(heldIndexBytes_,std::memory_order_relaxed);
        } catch (...) {
            index_.reset();
            liveDescriptorBytes_.fetch_sub(heldDescriptorBytes_,std::memory_order_relaxed);
            throw;
        }
    }
    ~OwnedFlannMatcher() {
        // Counters are decremented only after releasing the actual index and Mat.
        index_.reset(); data_.release();
        liveIndexBytes_.fetch_sub(heldIndexBytes_,std::memory_order_relaxed);
        liveDescriptorBytes_.fetch_sub(heldDescriptorBytes_,std::memory_order_relaxed);
    }
    static size_t LiveDescriptorBytes() { return liveDescriptorBytes_.load(std::memory_order_relaxed); }
    static size_t LiveIndexBytes() { return liveIndexBytes_.load(std::memory_order_relaxed); }
    void knnMatch(const cv::Mat& query, std::vector<std::vector<cv::DMatch>>& pairs, int k) {
        pairs.clear();
        if(query.empty()) return;
        CV_Assert(query.type()==CV_32FC1 && query.cols==data_.cols && k>0 && k<=data_.rows);
        cv::Mat continuous=query.isContinuous()?query:query.clone();
        cv::Mat indices(query.rows,k,CV_32S), distances(query.rows,k,CV_32F);
        const cvflann::Matrix<float> queries(continuous.ptr<float>(),query.rows,query.cols);
        cvflann::Matrix<int> nearest(indices.ptr<int>(),query.rows,k);
        cvflann::Matrix<float> squared(distances.ptr<float>(),query.rows,k);
        index_->knnSearch(queries,nearest,squared,k,cvflann::SearchParams(32));
        pairs.resize(query.rows);
        for(int row=0;row<query.rows;++row) {
            pairs[row].reserve(k);
            for(int neighbor=0;neighbor<k;++neighbor) {
                const int id=indices.at<int>(row,neighbor);
                if(id>=0) pairs[row].emplace_back(row,id,0,std::sqrt(distances.at<float>(row,neighbor)));
            }
        }
    }
    size_t DescriptorBytes() const { return heldDescriptorBytes_; }
    size_t IndexBytes() const { return heldIndexBytes_; }
private:
    inline static std::atomic_size_t liveDescriptorBytes_{0}, liveIndexBytes_{0};
    size_t heldDescriptorBytes_=0, heldIndexBytes_=0;
    cv::Mat data_;
    std::unique_ptr<cvflann::Index<cvflann::L2<float>>> index_;
};
