#include "Feature/Match/OwnedFlannMatcher.h"
#include <iostream>
#include <cmath>
#include <memory>
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* text){if(!ok){std::cerr<<"FAIL: "<<text<<"\n";++failures;}};
    for (int rows : {2,128,4096}) {
        cv::Mat train(rows,128,CV_32F), query(24,128,CV_32F);
        cv::RNG rng(19);rng.fill(train,cv::RNG::UNIFORM,-.1,.1);rng.fill(query,cv::RNG::UNIFORM,-.1,.1);
        train.row(0).copyTo(query.row(0));
        cv::setRNGSeed(73);
        cv::FlannBasedMatcher original;original.add(std::vector<cv::Mat>{train});original.train();
        std::vector<std::vector<cv::DMatch>> before,after;
        original.knnMatch(query,before,2);
        cv::setRNGSeed(73);
        auto owned=std::make_unique<OwnedFlannMatcher>(train);
        check(owned->DescriptorBytes()==train.total()*train.elemSize(),"matcher retains exactly one descriptor matrix");
        check(owned->IndexBytes()>0,"index memory is reported separately from descriptors");
        // Releasing the caller must not leave a dangling training pointer.
        train.release(); original.clear();
        owned->knnMatch(query,after,2);
        check(before.size()==after.size(),"query row count unchanged");
        for(size_t i=0;i<before.size();++i){
            check(before[i].size()==after[i].size(),"neighbor count unchanged");
            for(size_t j=0;j<before[i].size() && j<after[i].size();++j){
                check(before[i][j].queryIdx==after[i][j].queryIdx && before[i][j].trainIdx==after[i][j].trainIdx && before[i][j].imgIdx==after[i][j].imgIdx,"same seeded trees retain original row identities");
                check(before[i][j].distance==after[i][j].distance,"L2 distances exactly match the existing matcher");
            }
        }
        owned->knnMatch(query,after,2);
        check(after[0][0].distance==0 && after[0][0].trainIdx==0,"repeated query preserves exact match after caller releases data");
    }
    if(!failures) std::cout<<"All owned FLANN tests passed.\n";
    return failures?1:0;
}
