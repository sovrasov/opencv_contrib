/*M///////////////////////////////////////////////////////////////////////////////////////
 //
 //  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
 //
 //  By downloading, copying, installing or using the software you agree to this license.
 //  If you do not agree to this license, do not download, install,
 //  copy or use the software.
 //
 //
 //                           License Agreement
 //                For Open Source Computer Vision Library
 //
 // Copyright (C) 2013, OpenCV Foundation, all rights reserved.
 // Third party copyrights are property of their respective owners.
 //
 // Redistribution and use in source and binary forms, with or without modification,
 // are permitted provided that the following conditions are met:
 //
 //   * Redistribution's of source code must retain the above copyright notice,
 //     this list of conditions and the following disclaimer.
 //
 //   * Redistribution's in binary form must reproduce the above copyright notice,
 //     this list of conditions and the following disclaimer in the documentation
 //     and/or other materials provided with the distribution.
 //
 //   * The name of the copyright holders may not be used to endorse or promote products
 //     derived from this software without specific prior written permission.
 //
 // This software is provided by the copyright holders and contributors "as is" and
 // any express or implied warranties, including, but not limited to, the implied
 // warranties of merchantability and fitness for a particular purpose are disclaimed.
 // In no event shall the Intel Corporation or contributors be liable for any direct,
 // indirect, incidental, special, exemplary, or consequential damages
 // (including, but not limited to, procurement of substitute goods or services;
 // loss of use, data, or profits; or business interruption) however caused
 // and on any theory of liability, whether in contract, strict liability,
 // or tort (including negligence or otherwise) arising in any way out of
 // the use of this software, even if advised of the possibility of such damage.
 //
 //M*/

#include "precomp.hpp"
#include "opencv2/video/tracking.hpp"
#include "opencv2/imgproc.hpp"
#include <algorithm>
#include <limits.h>

namespace
{
using namespace cv;

#undef MEDIAN_FLOW_TRACKER_DEBUG_LOGS
#ifdef MEDIAN_FLOW_TRACKER_DEBUG_LOGS
#define dprintf(x) printf x
#else
#define dprintf(x) do{} while(false)
#endif

/*
 *  TrackerMedianFlow
 */
/*
 * TODO:
 * add "non-detected" answer in algo --> test it with 2 rects --> frame-by-frame debug in TLD --> test it!!
 * take all parameters out
 *              asessment framework
 *
 *
 * FIXME:
 * when patch is cut from image to compute NCC, there can be problem with size
 * optimize (allocation<-->reallocation)
 */

class TrackerMedianFlowImpl : public TrackerMedianFlow{
public:
    TrackerMedianFlowImpl(TrackerMedianFlow::Params paramsIn) {params=paramsIn;isInit=false;}
    void read( const FileNode& fn );
    void write( FileStorage& fs ) const;
private:
    bool initImpl( const Mat& image, const Rect2d& boundingBox );
    bool updateImpl( const Mat& image, Rect2d& boundingBox );
    bool medianFlowImpl(Mat oldImage,Mat newImage,Rect2d& oldBox);
    Rect2d vote(const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,const Rect2d& oldRect,Point2f& mD);
    //FIXME: this can be optimized: current method uses sort->select approach, there are O(n) selection algo for median; besides
    //it makes copy all the time
    float dist(Point2f p1,Point2f p2);
    std::string type2str(int type);
    void computeStatistics(std::vector<float>& data,int size=-1);
    void check_FB(const Mat& oldImage,const Mat& newImage,
                  const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,std::vector<bool>& status);
    void check_NCC(const Mat& oldImage,const Mat& newImage,
                   const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,std::vector<bool>& status);

    TrackerMedianFlow::Params params;
};

template<typename T>
T getMedian( const std::vector<T>& values );

template<typename T>
T getMedianAndDoPartition( std::vector<T>& values );

class TrackerMedianFlowModel : public TrackerModel{
public:
    TrackerMedianFlowModel(TrackerMedianFlow::Params /*params*/){}
    Rect2d getBoundingBox(){return boundingBox_;}
    void setBoudingBox(Rect2d boundingBox){boundingBox_=boundingBox;}
    Mat getImage(){return image_;}
    void setImage(const Mat& image){image.copyTo(image_);}
protected:
    Rect2d boundingBox_;
    Mat image_;
    void modelEstimationImpl( const std::vector<Mat>& /*responses*/ ){}
    void modelUpdateImpl(){}
};

void TrackerMedianFlowImpl::read( const cv::FileNode& fn )
{
    params.read( fn );
}

void TrackerMedianFlowImpl::write( cv::FileStorage& fs ) const
{
    params.write( fs );
}

bool TrackerMedianFlowImpl::initImpl( const Mat& image, const Rect2d& boundingBox ){
    model=Ptr<TrackerMedianFlowModel>(new TrackerMedianFlowModel(params));
    ((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->setImage(image);
    ((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->setBoudingBox(boundingBox);
    return true;
}

bool TrackerMedianFlowImpl::updateImpl( const Mat& image, Rect2d& boundingBox ){
    Mat oldImage=((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->getImage();

    Rect2d oldBox=((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->getBoundingBox();
    if(!medianFlowImpl(oldImage,image,oldBox)){
        return false;
    }
    boundingBox=oldBox;
    ((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->setImage(image);
    ((TrackerMedianFlowModel*)static_cast<TrackerModel*>(model))->setBoudingBox(oldBox);
    return true;
}

struct NaNChecker
{
    bool operator()(const Point2f& p)
    {
        return isnan(p.x) || isnan(p.y);
    }
};

void removeNanPointsFromVector(std::vector<Point2f>& vec) {
    std::vector<Point2f>::iterator iter_remove;
    iter_remove = std::remove_if(vec.begin(), vec.end(), NaNChecker());
    vec.erase(iter_remove, vec.end());
}

bool TrackerMedianFlowImpl::medianFlowImpl(Mat oldImage,Mat newImage,Rect2d& oldBox){
    std::vector<Point2f> pointsToTrackOld,pointsToTrackNew;

    Mat oldImage_gray,newImage_gray;
    if (oldImage.channels() != 1)
        cvtColor( oldImage, oldImage_gray, COLOR_BGR2GRAY );
    else
        oldImage.copyTo(oldImage_gray);

    if (newImage.channels() != 1)
        cvtColor( newImage, newImage_gray, COLOR_BGR2GRAY );
    else
        newImage.copyTo(newImage_gray);

    //"open ended" grid
    for(int i=0;i<params.pointsInGrid;i++){
        for(int j=0;j<params.pointsInGrid;j++){
            pointsToTrackOld.push_back(
                        Point2f((float)(oldBox.x+((1.0*oldBox.width)/params.pointsInGrid)*j+.5*oldBox.width/params.pointsInGrid),
                                (float)(oldBox.y+((1.0*oldBox.height)/params.pointsInGrid)*i+.5*oldBox.height/params.pointsInGrid)));
        }
    }

    std::vector<uchar> status(pointsToTrackOld.size());
    std::vector<float> errors(pointsToTrackOld.size());
    calcOpticalFlowPyrLK(oldImage_gray, newImage_gray,pointsToTrackOld,pointsToTrackNew,status,errors,
                         params.winSize, params.maxLevel, params.termCriteria, 0);

    CV_Assert(pointsToTrackNew.size() == pointsToTrackOld.size());
    CV_Assert(status.size() == pointsToTrackOld.size());
    dprintf(("\t%d after LK forward\n",(int)pointsToTrackOld.size()));

    float quiet_NaN = std::numeric_limits<float>::quiet_NaN();
    cv::Point2f NaN_point(quiet_NaN, quiet_NaN);

    int num_good_points_after_optical_flow = 0;
    for(size_t i=0;i<pointsToTrackOld.size();i++){
        if (status[i]==1) {
            num_good_points_after_optical_flow++;
        } else {
            pointsToTrackOld[i] = NaN_point;
            pointsToTrackNew[i] = NaN_point;
        }
    }

    dprintf(("\t num_good_points_after_optical_flow = %d\n",num_good_points_after_optical_flow));

    if (num_good_points_after_optical_flow == 0) {
        return false;
    }

    removeNanPointsFromVector(pointsToTrackOld);
    removeNanPointsFromVector(pointsToTrackNew);

    CV_Assert(pointsToTrackOld.size() == (size_t)num_good_points_after_optical_flow);
    CV_Assert(pointsToTrackNew.size() == (size_t)num_good_points_after_optical_flow);

    dprintf(("\t%d after LK forward after removing points with bad status\n",(int)pointsToTrackOld.size()));

    std::vector<bool> filter_status(pointsToTrackOld.size(), true);
    check_FB(oldImage_gray, newImage_gray, pointsToTrackOld, pointsToTrackNew, filter_status);
    check_NCC(oldImage_gray, newImage_gray, pointsToTrackOld, pointsToTrackNew, filter_status);

    // filter
    int num_good_points_after_filtering = 0;
    for(size_t i=0;i<pointsToTrackOld.size();i++){
        if(filter_status[i]){
            num_good_points_after_filtering++;
        } else {
            pointsToTrackOld[i] = NaN_point;
            pointsToTrackNew[i] = NaN_point;
        }
    }
    dprintf(("\t num_good_points_after_filtering = %d\n",num_good_points_after_filtering));

    if(num_good_points_after_filtering == 0){
        return false;
    }

    removeNanPointsFromVector(pointsToTrackOld);
    removeNanPointsFromVector(pointsToTrackNew);

    CV_Assert(pointsToTrackOld.size() == (size_t)num_good_points_after_filtering);
    CV_Assert(pointsToTrackNew.size() == (size_t)num_good_points_after_filtering);

    dprintf(("\t%d after LK backward\n",(int)pointsToTrackOld.size()));

    std::vector<Point2f> di(pointsToTrackOld.size());
    for(size_t i=0; i<pointsToTrackOld.size(); i++){
        di[i] = pointsToTrackNew[i]-pointsToTrackOld[i];
    }

    Point2f mDisplacement;
    oldBox=vote(pointsToTrackOld,pointsToTrackNew,oldBox,mDisplacement);

    std::vector<float> displacements;
    for(size_t i=0;i<di.size();i++){
        di[i]-=mDisplacement;
        displacements.push_back((float)sqrt(di[i].ddot(di[i])));
    }
    float median_displacements = getMedianAndDoPartition(displacements);
    dprintf(("\tmedian of length of difference of displacements = %f\n", median_displacements));
    if(median_displacements > params.maxMedianLengthOfDisplacementDifference){
        dprintf(("\tmedian flow tracker returns false due to big median length of difference between displacements\n"));
        return false;
    }

    return true;
}

Rect2d TrackerMedianFlowImpl::vote(const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,const Rect2d& oldRect,Point2f& mD){
    Rect2d newRect;
    Point2d newCenter(oldRect.x+oldRect.width/2.0,oldRect.y+oldRect.height/2.0);
    const size_t n=oldPoints.size();

    if (n==1) {
        newRect.x=oldRect.x+newPoints[0].x-oldPoints[0].x;
        newRect.y=oldRect.y+newPoints[0].y-oldPoints[0].y;
        newRect.width=oldRect.width;
        newRect.height=oldRect.height;
        mD.x = newPoints[0].x-oldPoints[0].x;
        mD.y = newPoints[0].y-oldPoints[0].y;
        return newRect;
    }

    float xshift=0,yshift=0;
    std::vector<float> buf_for_location(n, 0.);
    for(size_t i=0;i<n;i++){  buf_for_location[i]=newPoints[i].x-oldPoints[i].x;  }
    xshift=getMedianAndDoPartition(buf_for_location);
    newCenter.x+=xshift;
    for(size_t i=0;i<n;i++){  buf_for_location[i]=newPoints[i].y-oldPoints[i].y;  }
    yshift=getMedianAndDoPartition(buf_for_location);
    newCenter.y+=yshift;
    mD=Point2f((float)xshift,(float)yshift);

    std::vector<double> buf_for_scale(n*(n-1)/2, 0.0);
    for(size_t i=0,ctr=0;i<n;i++){
        for(size_t j=0;j<i;j++){
            double nd=norm(newPoints[i] - newPoints[j]);
            double od=norm(oldPoints[i] - oldPoints[j]);
            buf_for_scale[ctr]=(od==0.0)?0.0:(nd/od);
            ctr++;
        }
    }

    double scale=getMedianAndDoPartition(buf_for_scale);
    dprintf(("xshift, yshift, scale = %f %f %f\n",xshift,yshift,scale));
    newRect.x=newCenter.x-scale*oldRect.width/2.0;
    newRect.y=newCenter.y-scale*oldRect.height/2.0;
    newRect.width=scale*oldRect.width;
    newRect.height=scale*oldRect.height;
    dprintf(("rect old [%f %f %f %f]\n",oldRect.x,oldRect.y,oldRect.width,oldRect.height));
    dprintf(("rect [%f %f %f %f]\n",newRect.x,newRect.y,newRect.width,newRect.height));

    return newRect;
}

void TrackerMedianFlowImpl::computeStatistics(std::vector<float>& data,int size){
    int binnum=10;
    if(size==-1){
        size=(int)data.size();
    }
    float mini=*std::min_element(data.begin(),data.begin()+size),maxi=*std::max_element(data.begin(),data.begin()+size);
    std::vector<int> bins(binnum,(int)0);
    for(int i=0;i<size;i++){
        bins[std::min((int)(binnum*(data[i]-mini)/(maxi-mini)),binnum-1)]++;
    }
    for(int i=0;i<binnum;i++){
        dprintf(("[%4f,%4f] -- %4d\n",mini+(maxi-mini)/binnum*i,mini+(maxi-mini)/binnum*(i+1),bins[i]));
    }
}

void TrackerMedianFlowImpl::check_FB(const Mat& oldImage,const Mat& newImage,
                                     const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,std::vector<bool>& status){

    if(status.empty()) {
        status=std::vector<bool>(oldPoints.size(),true);
    }

    std::vector<uchar> LKstatus(oldPoints.size());
    std::vector<float> errors(oldPoints.size());
    std::vector<float> FBerror(oldPoints.size());
    std::vector<Point2f> pointsToTrackReprojection;
    calcOpticalFlowPyrLK(newImage, oldImage,newPoints,pointsToTrackReprojection,LKstatus,errors,
                         params.winSize, params.maxLevel, params.termCriteria, 0);

    for(size_t i=0;i<oldPoints.size();i++){
        FBerror[i]=(float)norm(oldPoints[i]-pointsToTrackReprojection[i]);
    }
    float FBerrorMedian=getMedian(FBerror);
    dprintf(("point median=%f\n",FBerrorMedian));
    dprintf(("FBerrorMedian=%f\n",FBerrorMedian));
    for(size_t i=0;i<oldPoints.size();i++){
        status[i]=status[i] && (FBerror[i] <= FBerrorMedian);
    }
}
void TrackerMedianFlowImpl::check_NCC(const Mat& oldImage,const Mat& newImage,
                                      const std::vector<Point2f>& oldPoints,const std::vector<Point2f>& newPoints,std::vector<bool>& status){

    std::vector<float> NCC(oldPoints.size(),0.0);
    Mat p1,p2;

    for (size_t i = 0; i < oldPoints.size(); i++) {
        getRectSubPix( oldImage, params.winSizeNCC, oldPoints[i],p1);
        getRectSubPix( newImage, params.winSizeNCC, newPoints[i],p2);

        const int patch_area=params.winSizeNCC.area();
        double s1=sum(p1)(0),s2=sum(p2)(0);
        double n1=norm(p1),n2=norm(p2);
        double prod=p1.dot(p2);
        double sq1=sqrt(n1*n1-s1*s1/patch_area),sq2=sqrt(n2*n2-s2*s2/patch_area);
        double ares=(sq2==0)?sq1/abs(sq1):(prod-s1*s2/patch_area)/sq1/sq2;

        NCC[i] = (float)ares;
    }
    float median = getMedian(NCC);
    for(size_t i = 0; i < oldPoints.size(); i++) {
        status[i] = status[i] && (NCC[i] >= median);
    }
}

template<typename T>
T getMedian(const std::vector<T>& values)
{
    std::vector<T> copy(values);
    return getMedianAndDoPartition(copy);
}

template<typename T>
T getMedianAndDoPartition(std::vector<T>& values)
{
    size_t size = values.size();
    if(size%2==0)
    {
        std::nth_element(values.begin(), values.begin() + size/2-1, values.end());
        T firstMedian = values[size/2-1];

        std::nth_element(values.begin(), values.begin() + size/2, values.end());
        T secondMedian = values[size/2];

        return (firstMedian + secondMedian) / (T)2;
    }
    else
    {
        size_t medianIndex = (size - 1) / 2;
        std::nth_element(values.begin(), values.begin() + medianIndex, values.end());

        return values[medianIndex];
    }
}

} /* anonymous namespace */

namespace cv
{
/*
 * Parameters
 */
TrackerMedianFlow::Params::Params() {
    pointsInGrid=10;
    winSize = Size(3,3);
    maxLevel = 5;
    termCriteria = TermCriteria(TermCriteria::COUNT|TermCriteria::EPS,20,0.3);
    winSizeNCC = Size(30,30);
    maxMedianLengthOfDisplacementDifference = 10;
}

void TrackerMedianFlow::Params::read( const cv::FileNode& fn ){
    *this = TrackerMedianFlow::Params();

    if (!fn["winSize"].empty())
        fn["winSize"] >> winSize;

    if(!fn["winSizeNCC"].empty())
        fn["winSizeNCC"] >> winSizeNCC;

    if(!fn["pointsInGrid"].empty())
        fn["pointsInGrid"] >> pointsInGrid;

    if(!fn["maxLevel"].empty())
        fn["maxLevel"] >> maxLevel;

    if(!fn["maxMedianLengthOfDisplacementDifference"].empty())
        fn["maxMedianLengthOfDisplacementDifference"] >> maxMedianLengthOfDisplacementDifference;

    if(!fn["termCriteria_maxCount"].empty())
        fn["termCriteria_maxCount"] >> termCriteria.maxCount;

    if(!fn["termCriteria_epsilon"].empty())
        fn["termCriteria_epsilon"] >> termCriteria.epsilon;
}

void TrackerMedianFlow::Params::write( cv::FileStorage& fs ) const{
    fs << "pointsInGrid" << pointsInGrid;
    fs << "winSize" << winSize;
    fs << "maxLevel" << maxLevel;
    fs << "termCriteria_maxCount" << termCriteria.maxCount;
    fs << "termCriteria_epsilon" << termCriteria.epsilon;
    fs << "winSizeNCC" << winSizeNCC;
    fs << "maxMedianLengthOfDisplacementDifference" << maxMedianLengthOfDisplacementDifference;
}

Ptr<TrackerMedianFlow> TrackerMedianFlow::createTracker(const TrackerMedianFlow::Params &parameters){
    return Ptr<TrackerMedianFlowImpl>(new TrackerMedianFlowImpl(parameters));
}

} /* namespace cv */
