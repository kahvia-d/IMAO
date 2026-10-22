/* Original code has been submitted by Liu Liu. Here is the copyright.
----------------------------------------------------------------------------------
 * An OpenCV Implementation of SURF
 * Further Information Refer to "SURF: Speed-Up Robust Feature"
 * Author: Liu Liu
 * liuliu.1987+opencv@gmail.com
 *
 * There are still serveral lacks for this experimental implementation:
 * 1.The interpolation of sub-pixel mentioned in article was not implemented yet;
 * 2.A comparison with original libSurf.so shows that the hessian detector is not a 100% match to their implementation;
 * 3.Due to above reasons, I recommanded the original one for study and reuse;
 *
 * However, the speed of this implementation is something comparable to original one.
 *
 * Copyright© 2008, Liu Liu All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *  Redistributions of source code must retain the above
 *  copyright notice, this list of conditions and the following
 *  disclaimer.
 *  Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following
 *  disclaimer in the documentation and/or other materials
 *  provided with the distribution.
 *  The name of Contributor may not be used to endorse or
 *  promote products derived from this software without
 *  specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

// Derived from OpenCV-contrib 4.11.0 surf.cpp (Hessian detection and descriptors).
// Preserve all filters, maxima/interpolation arithmetic and sort order. Keep
// three adjacent layers instead of every octave; parallelize independent rows.
#include "StreamingSurf.h"
#include "ExactDescriptorMatcher.h"
#include <chrono>
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
namespace {
using namespace cv;
static const int SURF_HAAR_SIZE0 = 9;

// Wavelet size increment between layers. This should be an even number,
// such that the wavelet sizes in an octave are either all even or all odd.
// This ensures that when looking for the neighbours of a sample, the layers
// above and below are aligned correctly.
static const int SURF_HAAR_SIZE_INC = 6;


struct SurfHF
{
    int p0, p1, p2, p3;
    float w;

    SurfHF(): p0(0), p1(0), p2(0), p3(0), w(0) {}
};

inline float calcHaarPattern( const int* origin, const SurfHF* f, int n )
{
    double d = 0;
    for( int k = 0; k < n; k++ )
        d += (origin[f[k].p0] + origin[f[k].p3] - origin[f[k].p1] - origin[f[k].p2])*f[k].w;
    return (float)d;
}

static void
resizeHaarPattern( const int src[][5], SurfHF* dst, int n, int oldSize, int newSize, int widthStep )
{
    float ratio = (float)newSize/oldSize;
    for( int k = 0; k < n; k++ )
    {
        int dx1 = cvRound( ratio*src[k][0] );
        int dy1 = cvRound( ratio*src[k][1] );
        int dx2 = cvRound( ratio*src[k][2] );
        int dy2 = cvRound( ratio*src[k][3] );
        dst[k].p0 = dy1*widthStep + dx1;
        dst[k].p1 = dy2*widthStep + dx1;
        dst[k].p2 = dy1*widthStep + dx2;
        dst[k].p3 = dy2*widthStep + dx2;
        dst[k].w = src[k][4]/((float)(dx2-dx1)*(dy2-dy1));
    }
}

/*
 * Calculate the determinant and trace sign of the Hessian for a layer of the
 * scale-space pyramid
 */
static void calcLayerDetAndTraceSign( const Mat& sum, int size, int sampleStep,
                                  Mat& det, Mat& trace )
{
    const int NX=3, NY=3, NXY=4;
    const int dx_s[NX][5] = { {0, 2, 3, 7, 1}, {3, 2, 6, 7, -2}, {6, 2, 9, 7, 1} };
    const int dy_s[NY][5] = { {2, 0, 7, 3, 1}, {2, 3, 7, 6, -2}, {2, 6, 7, 9, 1} };
    const int dxy_s[NXY][5] = { {1, 1, 4, 4, 1}, {5, 1, 8, 4, -1}, {1, 5, 4, 8, -1}, {5, 5, 8, 8, 1} };

    SurfHF Dx[NX], Dy[NY], Dxy[NXY];

    if( size > sum.rows-1 || size > sum.cols-1 )
       return;

    resizeHaarPattern( dx_s , Dx , NX , 9, size, sum.cols );
    resizeHaarPattern( dy_s , Dy , NY , 9, size, sum.cols );
    resizeHaarPattern( dxy_s, Dxy, NXY, 9, size, sum.cols );

    /* The integral image 'sum' is one pixel bigger than the source image */
    int samples_i = 1+(sum.rows-1-size)/sampleStep;
    int samples_j = 1+(sum.cols-1-size)/sampleStep;

    /* Ignore pixels where some of the kernel is outside the image */
    int margin = (size/2)/sampleStep;

    parallel_for_(Range(0, samples_i), [&](const Range& rows) {
    for( int i = rows.start; i < rows.end; i++ )
    {
        const int* sum_ptr = sum.ptr<int>(i*sampleStep);
        float* det_ptr = &det.at<float>(i+margin, margin);
        schar* trace_ptr = &trace.at<schar>(i+margin, margin);
        for( int j = 0; j < samples_j; j++ )
        {
            float dx  = calcHaarPattern( sum_ptr, Dx , 3 );
            float dy  = calcHaarPattern( sum_ptr, Dy , 3 );
            float dxy = calcHaarPattern( sum_ptr, Dxy, 4 );
            sum_ptr += sampleStep;
            det_ptr[j] = dx*dy - 0.81f*dxy*dxy;
            // Only the Laplacian sign is used by maxima/class_id. Evaluate the
            // same float sum before encoding; +/-0 and NaN still map to zero.
            const float traceValue = dx + dy;
            trace_ptr[j] = static_cast<schar>((traceValue > 0) - (traceValue < 0));
        }
    }
    });
}


/*
 * Maxima location interpolation as described in "Invariant Features from
 * Interest Point Groups" by Matthew Brown and David Lowe. This is performed by
 * fitting a 3D quadratic to a set of neighbouring samples.
 *
 * The gradient vector and Hessian matrix at the initial keypoint location are
 * approximated using central differences. The linear system Ax = b is then
 * solved, where A is the Hessian, b is the negative gradient, and x is the
 * offset of the interpolated maxima coordinates from the initial estimate.
 * This is equivalent to an iteration of Netwon's optimisation algorithm.
 *
 * N9 contains the samples in the 3x3x3 neighbourhood of the maxima
 * dx is the sampling step in x
 * dy is the sampling step in y
 * ds is the sampling step in size
 * point contains the keypoint coordinates and scale to be modified
 *
 * Return value is 1 if interpolation was successful, 0 on failure.
 */
static int
interpolateKeypoint( float N9[3][9], int dx, int dy, int ds, KeyPoint& kpt )
{
    Vec3f b(-(N9[1][5]-N9[1][3])/2,  // Negative 1st deriv with respect to x
            -(N9[1][7]-N9[1][1])/2,  // Negative 1st deriv with respect to y
            -(N9[2][4]-N9[0][4])/2); // Negative 1st deriv with respect to s

    Matx33f A(
        N9[1][3]-2*N9[1][4]+N9[1][5],            // 2nd deriv x, x
        (N9[1][8]-N9[1][6]-N9[1][2]+N9[1][0])/4, // 2nd deriv x, y
        (N9[2][5]-N9[2][3]-N9[0][5]+N9[0][3])/4, // 2nd deriv x, s
        (N9[1][8]-N9[1][6]-N9[1][2]+N9[1][0])/4, // 2nd deriv x, y
        N9[1][1]-2*N9[1][4]+N9[1][7],            // 2nd deriv y, y
        (N9[2][7]-N9[2][1]-N9[0][7]+N9[0][1])/4, // 2nd deriv y, s
        (N9[2][5]-N9[2][3]-N9[0][5]+N9[0][3])/4, // 2nd deriv x, s
        (N9[2][7]-N9[2][1]-N9[0][7]+N9[0][1])/4, // 2nd deriv y, s
        N9[0][4]-2*N9[1][4]+N9[2][4]);           // 2nd deriv s, s

    Vec3f x = A.solve(b, DECOMP_LU);

    bool ok = (x[0] != 0 || x[1] != 0 || x[2] != 0) &&
        std::abs(x[0]) <= 1 && std::abs(x[1]) <= 1 && std::abs(x[2]) <= 1;

    if( ok )
    {
        kpt.pt.x += x[0]*dx;
        kpt.pt.y += x[1]*dy;
        kpt.size = (float)cvRound( kpt.size + x[2]*ds );
    }
    return ok;
}

// Only the layer-local maxima routine is retained from the original invoker.
struct SURFFindInvoker {
    static void findMaximaInLayer(const Mat& sum, const Mat& mask_sum,
        const std::vector<Mat>& dets, const std::vector<Mat>& traces,
        const std::vector<int>& sizes, std::vector<KeyPoint>& keypoints,
        int octave, int layer, float hessianThreshold, int sampleStep);
    static Mutex findMaximaInLayer_m;
};

Mutex SURFFindInvoker::findMaximaInLayer_m;


/*
 * Find the maxima in the determinant of the Hessian in a layer of the
 * scale-space pyramid
 */
void SURFFindInvoker::findMaximaInLayer( const Mat& sum, const Mat& mask_sum,
                   const std::vector<Mat>& dets, const std::vector<Mat>& traces,
                   const std::vector<int>& sizes, std::vector<KeyPoint>& keypoints,
                   int octave, int layer, float hessianThreshold, int sampleStep )
{
    // Wavelet Data
    const int NM=1;
    const int dm[NM][5] = { {0, 0, 9, 9, 1} };
    SurfHF Dm;

    int size = sizes[layer];

    // The integral image 'sum' is one pixel bigger than the source image
    int layer_rows = (sum.rows-1)/sampleStep;
    int layer_cols = (sum.cols-1)/sampleStep;

    // Ignore pixels without a 3x3x3 neighbourhood in the layer above
    int margin = (sizes[layer+1]/2)/sampleStep+1;

    if( !mask_sum.empty() )
       resizeHaarPattern( dm, &Dm, NM, 9, size, mask_sum.cols );

    int step = (int)(dets[layer].step/dets[layer].elemSize());

    if (layer_rows <= 2*margin || layer_cols <= 2*margin) return;
    parallel_for_(Range(margin, layer_rows-margin), [&](const Range& rows) {
    for( int i = rows.start; i < rows.end; i++ )
    {
        const float* det_ptr = dets[layer].ptr<float>(i);
        const schar* trace_ptr = traces[layer].ptr<schar>(i);
        for( int j = margin; j < layer_cols-margin; j++ )
        {
            float val0 = det_ptr[j];
            if( val0 > hessianThreshold )
            {
                /* Coordinates for the start of the wavelet in the sum image. There
                   is some integer division involved, so don't try to simplify this
                   (cancel out sampleStep) without checking the result is the same */
                int sum_i = sampleStep*(i-(size/2)/sampleStep);
                int sum_j = sampleStep*(j-(size/2)/sampleStep);

                /* The 3x3x3 neighbouring samples around the maxima.
                   The maxima is included at N9[1][4] */

                const float *det1 = &dets[layer-1].at<float>(i, j);
                const float *det2 = &dets[layer].at<float>(i, j);
                const float *det3 = &dets[layer+1].at<float>(i, j);
                float N9[3][9] = { { det1[-step-1], det1[-step], det1[-step+1],
                                     det1[-1]  , det1[0] , det1[1],
                                     det1[step-1] , det1[step] , det1[step+1]  },
                                   { det2[-step-1], det2[-step], det2[-step+1],
                                     det2[-1]  , det2[0] , det2[1],
                                     det2[step-1] , det2[step] , det2[step+1]  },
                                   { det3[-step-1], det3[-step], det3[-step+1],
                                     det3[-1]  , det3[0] , det3[1],
                                     det3[step-1] , det3[step] , det3[step+1]  } };

                /* Check the mask - why not just check the mask at the center of the wavelet? */
                if( !mask_sum.empty() )
                {
                    const int* mask_ptr = &mask_sum.at<int>(sum_i, sum_j);
                    float mval = calcHaarPattern( mask_ptr, &Dm, 1 );
                    if( mval < 0.5 )
                        continue;
                }

                /* Non-maxima suppression. val0 is at N9[1][4]*/
                if( val0 > N9[0][0] && val0 > N9[0][1] && val0 > N9[0][2] &&
                    val0 > N9[0][3] && val0 > N9[0][4] && val0 > N9[0][5] &&
                    val0 > N9[0][6] && val0 > N9[0][7] && val0 > N9[0][8] &&
                    val0 > N9[1][0] && val0 > N9[1][1] && val0 > N9[1][2] &&
                    val0 > N9[1][3]                    && val0 > N9[1][5] &&
                    val0 > N9[1][6] && val0 > N9[1][7] && val0 > N9[1][8] &&
                    val0 > N9[2][0] && val0 > N9[2][1] && val0 > N9[2][2] &&
                    val0 > N9[2][3] && val0 > N9[2][4] && val0 > N9[2][5] &&
                    val0 > N9[2][6] && val0 > N9[2][7] && val0 > N9[2][8] )
                {
                    /* Calculate the wavelet center coordinates for the maxima */
                    float center_i = sum_i + (size-1)*0.5f;
                    float center_j = sum_j + (size-1)*0.5f;

                    KeyPoint kpt( center_j, center_i, (float)sizes[layer],
                                  -1, val0, octave, trace_ptr[j] );

                    /* Interpolate maxima location within the 3x3x3 neighbourhood  */
                    int ds = size - sizes[layer-1];
                    int interp_ok = interpolateKeypoint( N9, sampleStep, sampleStep, ds, kpt );

                    /* Sometimes the interpolation step gives a negative size etc. */
                    if( interp_ok  )
                    {
                        /*printf( "KeyPoint %f %f %d\n", point.pt.x, point.pt.y, point.size );*/
                        cv::AutoLock lock(findMaximaInLayer_m);
                        keypoints.push_back(kpt);
                    }
                }
            }
        }
    }
    });
}

struct KeypointGreater
{
    inline bool operator()(const KeyPoint& kp1, const KeyPoint& kp2) const
    {
        if(kp1.response > kp2.response) return true;
        if(kp1.response < kp2.response) return false;
        if(kp1.size > kp2.size) return true;
        if(kp1.size < kp2.size) return false;
        if(kp1.octave > kp2.octave) return true;
        if(kp1.octave < kp2.octave) return false;
        if(kp1.pt.y < kp2.pt.y) return false;
        if(kp1.pt.y > kp2.pt.y) return true;
        return kp1.pt.x < kp2.pt.x;
    }
};

static const float SURF_ORI_SIGMA = 2.5f;
static const float SURF_DESC_SIGMA = 3.3f;
static const int SURF_ORI_SEARCH_INC = 5;

struct SURFInvoker : ParallelLoopBody
{
    enum { ORI_RADIUS = 6, ORI_WIN = 60, PATCH_SZ = 20 };

    SURFInvoker( const Mat& _img, const Mat& _sum,
                 std::vector<KeyPoint>& _keypoints, Mat& _descriptors,
                 bool _extended, bool _upright )
    {
        keypoints = &_keypoints;
        descriptors = &_descriptors;
        img = &_img;
        sum = &_sum;
        extended = _extended;
        upright = _upright;

        // Simple bound for number of grid points in circle of radius ORI_RADIUS
        const int nOriSampleBound = (2*ORI_RADIUS+1)*(2*ORI_RADIUS+1);

        // Allocate arrays
        apt.resize(nOriSampleBound);
        aptw.resize(nOriSampleBound);
        DW.resize(PATCH_SZ*PATCH_SZ);

        /* Coordinates and weights of samples used to calculate orientation */
        Mat G_ori = getGaussianKernel( 2*ORI_RADIUS+1, SURF_ORI_SIGMA, CV_32F );
        nOriSamples = 0;
        for( int i = -ORI_RADIUS; i <= ORI_RADIUS; i++ )
        {
            for( int j = -ORI_RADIUS; j <= ORI_RADIUS; j++ )
            {
                if( i*i + j*j <= ORI_RADIUS*ORI_RADIUS )
                {
                    apt[nOriSamples] = Point(i,j);
                    aptw[nOriSamples++] = G_ori.at<float>(i+ORI_RADIUS,0) * G_ori.at<float>(j+ORI_RADIUS,0);
                }
            }
        }
        CV_Assert( nOriSamples <= nOriSampleBound );

        /* Gaussian used to weight descriptor samples */
        Mat G_desc = getGaussianKernel( PATCH_SZ, SURF_DESC_SIGMA, CV_32F );
        for( int i = 0; i < PATCH_SZ; i++ )
        {
            for( int j = 0; j < PATCH_SZ; j++ )
                DW[i*PATCH_SZ+j] = G_desc.at<float>(i,0) * G_desc.at<float>(j,0);
        }
    }

    void operator()(const Range& range) const CV_OVERRIDE
    {
        /* X and Y gradient wavelet data */
        const int NX=2, NY=2;
        const int dx_s[NX][5] = {{0, 0, 2, 4, -1}, {2, 0, 4, 4, 1}};
        const int dy_s[NY][5] = {{0, 0, 4, 2, 1}, {0, 2, 4, 4, -1}};

        // Optimisation is better using nOriSampleBound than nOriSamples for
        // array lengths.  Maybe because it is a constant known at compile time
        const int nOriSampleBound =(2*ORI_RADIUS+1)*(2*ORI_RADIUS+1);

        float X[nOriSampleBound], Y[nOriSampleBound], angle[nOriSampleBound];
        uchar PATCH[PATCH_SZ+1][PATCH_SZ+1];
        float DX[PATCH_SZ][PATCH_SZ], DY[PATCH_SZ][PATCH_SZ];
        Mat _patch(PATCH_SZ+1, PATCH_SZ+1, CV_8U, PATCH);

        int dsize = extended ? 128 : 64;

        int k, k1 = range.start, k2 = range.end;
        float maxSize = 0;
        for( k = k1; k < k2; k++ )
        {
            maxSize = std::max(maxSize, (*keypoints)[k].size);
        }
        int imaxSize = std::max(cvCeil((PATCH_SZ+1)*maxSize*1.2f/9.0f), 1);
        cv::AutoBuffer<uchar> winbuf(imaxSize*imaxSize);

        for( k = k1; k < k2; k++ )
        {
            int i, j, kk, nangle;
            float* vec;
            SurfHF dx_t[NX], dy_t[NY];
            KeyPoint& kp = (*keypoints)[k];
            float size = kp.size;
            Point2f center = kp.pt;
            /* The sampling intervals and wavelet sized for selecting an orientation
             and building the keypoint descriptor are defined relative to 's' */
            float s = size*1.2f/9.0f;
            /* To find the dominant orientation, the gradients in x and y are
             sampled in a circle of radius 6s using wavelets of size 4s.
             We ensure the gradient wavelet size is even to ensure the
             wavelet pattern is balanced and symmetric around its center */
            int grad_wav_size = 2*cvRound( 2*s );
            if( sum->rows < grad_wav_size || sum->cols < grad_wav_size )
            {
                /* when grad_wav_size is too big,
                 * the sampling of gradient will be meaningless
                 * mark keypoint for deletion. */
                kp.size = -1;
                continue;
            }

            float descriptor_dir = 360.f - 90.f;
            if (upright == 0)
            {
                resizeHaarPattern( dx_s, dx_t, NX, 4, grad_wav_size, sum->cols );
                resizeHaarPattern( dy_s, dy_t, NY, 4, grad_wav_size, sum->cols );
                for( kk = 0, nangle = 0; kk < nOriSamples; kk++ )
                {
                    int x = cvRound( center.x + apt[kk].x*s - (float)(grad_wav_size-1)/2 );
                    int y = cvRound( center.y + apt[kk].y*s - (float)(grad_wav_size-1)/2 );
                    if( y < 0 || y >= sum->rows - grad_wav_size ||
                        x < 0 || x >= sum->cols - grad_wav_size )
                        continue;
                    const int* ptr = &sum->at<int>(y, x);
                    float vx = calcHaarPattern( ptr, dx_t, 2 );
                    float vy = calcHaarPattern( ptr, dy_t, 2 );
                    X[nangle] = vx*aptw[kk];
                    Y[nangle] = vy*aptw[kk];
                    nangle++;
                }
                if( nangle == 0 )
                {
                    // No gradient could be sampled because the keypoint is too
                    // near too one or more of the sides of the image. As we
                    // therefore cannot find a dominant direction, we skip this
                    // keypoint and mark it for later deletion from the sequence.
                    kp.size = -1;
                    continue;
                }

                phase( Mat(1, nangle, CV_32F, X), Mat(1, nangle, CV_32F, Y), Mat(1, nangle, CV_32F, angle), true );

                float bestx = 0, besty = 0, descriptor_mod = 0;
                for( i = 0; i < 360; i += SURF_ORI_SEARCH_INC )
                {
                    float sumx = 0, sumy = 0, temp_mod;
                    for( j = 0; j < nangle; j++ )
                    {
                        int d = std::abs(cvRound(angle[j]) - i);
                        if( d < ORI_WIN/2 || d > 360-ORI_WIN/2 )
                        {
                            sumx += X[j];
                            sumy += Y[j];
                        }
                    }
                    temp_mod = sumx*sumx + sumy*sumy;
                    if( temp_mod > descriptor_mod )
                    {
                        descriptor_mod = temp_mod;
                        bestx = sumx;
                        besty = sumy;
                    }
                }
                descriptor_dir = fastAtan2( -besty, bestx );
            }
            kp.angle = descriptor_dir;
            if( !descriptors || !descriptors->data )
                continue;

            /* Extract a window of pixels around the keypoint of size 20s */
            int win_size = (int)((PATCH_SZ+1)*s);
            CV_Assert( imaxSize >= win_size );
            Mat win(win_size, win_size, CV_8U, winbuf.data());

            if( !upright )
            {
                descriptor_dir *= (float)(CV_PI/180);
                float sin_dir = -std::sin(descriptor_dir);
                float cos_dir =  std::cos(descriptor_dir);

                /* Subpixel interpolation version (slower). Subpixel not required since
                the pixels will all get averaged when we scale down to 20 pixels */
                /*
                float w[] = { cos_dir, sin_dir, center.x,
                -sin_dir, cos_dir , center.y };
                CvMat W = cvMat(2, 3, CV_32F, w);
                cvGetQuadrangleSubPix( img, &win, &W );
                */

                float win_offset = -(float)(win_size-1)/2;
                float start_x = center.x + win_offset*cos_dir + win_offset*sin_dir;
                float start_y = center.y - win_offset*sin_dir + win_offset*cos_dir;
                uchar* WIN = win.data;
#if 0
                // Nearest neighbour version (faster)
                for( i = 0; i < win_size; i++, start_x += sin_dir, start_y += cos_dir )
                {
                    float pixel_x = start_x;
                    float pixel_y = start_y;
                    for( j = 0; j < win_size; j++, pixel_x += cos_dir, pixel_y -= sin_dir )
                    {
                        int x = std::min(std::max(cvRound(pixel_x), 0), img->cols-1);
                        int y = std::min(std::max(cvRound(pixel_y), 0), img->rows-1);
                        WIN[i*win_size + j] = img->at<uchar>(y, x);
                    }
                }
#else
                int ncols1 = img->cols-1, nrows1 = img->rows-1;
                size_t imgstep = img->step;
                for( i = 0; i < win_size; i++, start_x += sin_dir, start_y += cos_dir )
                {
                    double pixel_x = start_x;
                    double pixel_y = start_y;
                    for( j = 0; j < win_size; j++, pixel_x += cos_dir, pixel_y -= sin_dir )
                    {
                        int ix = cvFloor(pixel_x), iy = cvFloor(pixel_y);
                        if( (unsigned)ix < (unsigned)ncols1 &&
                            (unsigned)iy < (unsigned)nrows1 )
                        {
                            float a = (float)(pixel_x - ix), b = (float)(pixel_y - iy);
                            const uchar* imgptr = &img->at<uchar>(iy, ix);
                            WIN[i*win_size + j] = (uchar)
                                cvRound(imgptr[0]*(1.f - a)*(1.f - b) +
                                        imgptr[1]*a*(1.f - b) +
                                        imgptr[imgstep]*(1.f - a)*b +
                                        imgptr[imgstep+1]*a*b);
                        }
                        else
                        {
                            int x = std::min(std::max(cvRound(pixel_x), 0), ncols1);
                            int y = std::min(std::max(cvRound(pixel_y), 0), nrows1);
                            WIN[i*win_size + j] = img->at<uchar>(y, x);
                        }
                    }
                }
#endif
            }
            else
            {
                // extract rect - slightly optimized version of the code above
                // TODO: find faster code, as this is simply an extract rect operation,
                //       e.g. by using cvGetSubRect, problem is the border processing
                // descriptor_dir == 90 grad
                // sin_dir == 1
                // cos_dir == 0

                float win_offset = -(float)(win_size-1)/2;
                int start_x = cvRound(center.x + win_offset);
                int start_y = cvRound(center.y - win_offset);
                uchar* WIN = win.data;
                for( i = 0; i < win_size; i++, start_x++ )
                {
                    int pixel_x = start_x;
                    int pixel_y = start_y;
                    for( j = 0; j < win_size; j++, pixel_y-- )
                    {
                        int x = MAX( pixel_x, 0 );
                        int y = MAX( pixel_y, 0 );
                        x = MIN( x, img->cols-1 );
                        y = MIN( y, img->rows-1 );
                        WIN[i*win_size + j] = img->at<uchar>(y, x);
                    }
                }
            }
            // Scale the window to size PATCH_SZ so each pixel's size is s. This
            // makes calculating the gradients with wavelets of size 2s easy
            resize(win, _patch, _patch.size(), 0, 0, INTER_AREA);

            // Calculate gradients in x and y with wavelets of size 2s
            for( i = 0; i < PATCH_SZ; i++ )
                for( j = 0; j < PATCH_SZ; j++ )
                {
                    float dw = DW[i*PATCH_SZ + j];
                    float vx = (PATCH[i][j+1] - PATCH[i][j] + PATCH[i+1][j+1] - PATCH[i+1][j])*dw;
                    float vy = (PATCH[i+1][j] - PATCH[i][j] + PATCH[i+1][j+1] - PATCH[i][j+1])*dw;
                    DX[i][j] = vx;
                    DY[i][j] = vy;
                }

            // Construct the descriptor
            vec = descriptors->ptr<float>(k);
            for( kk = 0; kk < dsize; kk++ )
                vec[kk] = 0;
            double square_mag = 0;
            if( extended )
            {
                // 128-bin descriptor
                for( i = 0; i < 4; i++ )
                    for( j = 0; j < 4; j++ )
                    {
                        for(int y = i*5; y < i*5+5; y++ )
                        {
                            for(int x = j*5; x < j*5+5; x++ )
                            {
                                float tx = DX[y][x], ty = DY[y][x];
                                if( ty >= 0 )
                                {
                                    vec[0] += tx;
                                    vec[1] += (float)fabs(tx);
                                } else {
                                    vec[2] += tx;
                                    vec[3] += (float)fabs(tx);
                                }
                                if ( tx >= 0 )
                                {
                                    vec[4] += ty;
                                    vec[5] += (float)fabs(ty);
                                } else {
                                    vec[6] += ty;
                                    vec[7] += (float)fabs(ty);
                                }
                            }
                        }
                        for( kk = 0; kk < 8; kk++ )
                            square_mag += vec[kk]*vec[kk];
                        vec += 8;
                    }
            }
            else
            {
                // 64-bin descriptor
                for( i = 0; i < 4; i++ )
                    for( j = 0; j < 4; j++ )
                    {
                        for(int y = i*5; y < i*5+5; y++ )
                        {
                            for(int x = j*5; x < j*5+5; x++ )
                            {
                                float tx = DX[y][x], ty = DY[y][x];
                                vec[0] += tx; vec[1] += ty;
                                vec[2] += (float)fabs(tx); vec[3] += (float)fabs(ty);
                            }
                        }
                        for( kk = 0; kk < 4; kk++ )
                            square_mag += vec[kk]*vec[kk];
                        vec+=4;
                    }
            }

            // unit vector is essential for contrast invariance
            vec = descriptors->ptr<float>(k);
            float scale = (float)(1./(std::sqrt(square_mag) + FLT_EPSILON));
            for( kk = 0; kk < dsize; kk++ )
                vec[kk] *= scale;
        }
    }

    // Parameters
    const Mat* img;
    const Mat* sum;
    std::vector<KeyPoint>* keypoints;
    Mat* descriptors;
    bool extended;
    bool upright;

    // Pre-calculated values
    int nOriSamples;
    std::vector<Point> apt;
    std::vector<float> aptw;
    std::vector<float> DW;
};


// Dispatch the upstream kernel independently of OpenCV's process-wide guard.
static void DescribeViewportSurf(const Mat& gray, std::vector<KeyPoint>& points,
    Mat& descriptors, const std::function<bool()>& interrupted,
    ViewportSurfStatistics* statistics) {
    if (points.empty()) return;
    Mat sum; integral(gray, sum, CV_32S);
    descriptors.create(static_cast<int>(points.size()), 128, CV_32F);
    const SURFInvoker describe(gray, sum, points, descriptors, true, true);
    constexpr int chunkSize = 32;
    // Caller participates; at most three extra threads. Global OpenCV thread
    // settings and another caller's parallel region do not affect dispatch.
    const unsigned workerCount = std::min({4u, std::max(1u, std::thread::hardware_concurrency()),
        static_cast<unsigned>((points.size()+chunkSize-1)/chunkSize)});
    if (statistics) {
        statistics->descriptorWorkers = workerCount;
        for (const auto& point : points) {
            const int window = static_cast<int>(21 * (point.size*1.2f/9.0f));
            statistics->descriptorWindowPixels += static_cast<uint64_t>(window)*window;
            statistics->descriptorMaxWindow = std::max(statistics->descriptorMaxWindow, window);
        }
    }
    std::atomic<int> next{0};
    std::atomic<bool> stopped{false};
    std::mutex callbackMutex;
    std::exception_ptr failure;
    const auto work = [&] {
        try {
            while (!stopped.load(std::memory_order_relaxed)) {
                const int start = next.fetch_add(chunkSize, std::memory_order_relaxed);
                if (start >= static_cast<int>(points.size())) break;
                {
                    // Callbacks remain serialized; pixel work holds no mutex.
                    std::scoped_lock lock(callbackMutex);
                    if (stopped.load(std::memory_order_relaxed)) break;
                    if (interrupted && interrupted()) throw SearchInterrupted{};
                }
                describe(Range(start, std::min(start+chunkSize, static_cast<int>(points.size()))));
            }
        } catch (...) {
            stopped.store(true, std::memory_order_relaxed);
            std::scoped_lock lock(callbackMutex);
            if (!failure) failure = std::current_exception();
        }
    };
    {
        std::vector<std::jthread> workers;
        workers.reserve(workerCount-1);
        try {
            for (unsigned i=1; i<workerCount; ++i) workers.emplace_back(work);
            work();
        } catch (...) {
            stopped.store(true, std::memory_order_relaxed);
            throw; // Join before points, descriptors or sum are destroyed.
        }
    }
    if (failure) std::rethrow_exception(failure);
    // Same upstream deletion of unrepresentable scales, retaining row order.
    int retained=0;
    const int count=static_cast<int>(points.size());
    for (int i=0; i<count; ++i) if (points[i].size>0) {
        if (i>retained) {
            points[retained]=points[i];
            std::memcpy(descriptors.ptr(retained),descriptors.ptr(i),128*sizeof(float));
        }
        ++retained;
    }
    if (retained<count) {
        points.resize(retained);
        Mat kept=descriptors.rowRange(0,retained);
        kept.copyTo(descriptors);
    }
}

}
void ExtractViewportSurf(const cv::Mat& gray, std::vector<cv::KeyPoint>& points,
    cv::Mat& descriptors, const std::function<bool()>& interrupted, ViewportSurfStatistics* statistics) {
    const auto checkpoint = [&] { if (interrupted && interrupted()) throw SearchInterrupted{}; };
    CV_Assert(!gray.empty() && gray.type() == CV_8UC1);
    points.clear(); descriptors.release();
    if (statistics) *statistics = {};
    const auto started = std::chrono::steady_clock::now();
    std::vector<cv::KeyPoint> found;
    checkpoint();
    {
        cv::Mat sum; cv::integral(gray, sum, CV_32S);
        for (int octave=0; octave<4; ++octave) {
            const int step=1<<octave;
            std::vector<cv::Mat> dets(3),traces(3);
            std::vector<int> sizes(3);
            for (int middle=1; middle<=3; ++middle) {
                if (middle==1) {
                    for(int layer=0;layer<3;++layer) {
                        checkpoint();
                        sizes[layer]=(SURF_HAAR_SIZE0+SURF_HAAR_SIZE_INC*layer)<<octave;
                        dets[layer].create(gray.rows/step,gray.cols/step,CV_32F);
                        traces[layer].create(gray.rows/step,gray.cols/step,CV_8S);
                        calcLayerDetAndTraceSign(sum,sizes[layer],step,dets[layer],traces[layer]);
                    }
                } else {
                    std::swap(dets[0],dets[1]);std::swap(dets[1],dets[2]);
                    std::swap(traces[0],traces[1]);std::swap(traces[1],traces[2]);
                    sizes[0]=sizes[1];sizes[1]=sizes[2];
                    sizes[2]=(SURF_HAAR_SIZE0+SURF_HAAR_SIZE_INC*(middle+1))<<octave;
                    checkpoint();calcLayerDetAndTraceSign(sum,sizes[2],step,dets[2],traces[2]);
                }
                if (statistics) {
                    size_t held = sum.total() * sum.elemSize();
                    for (const auto& m : dets) held += m.total() * m.elemSize();
                    for (const auto& m : traces) held += m.total() * m.elemSize();
                    statistics->detectorScratchBytes = std::max(statistics->detectorScratchBytes, held);
                }
                checkpoint();
                SURFFindInvoker::findMaximaInLayer(sum,cv::Mat{},dets,traces,sizes,found,octave,1,100.f,step);
            }
        }
    }
    checkpoint();
    std::sort(found.begin(),found.end(),KeypointGreater());
    const auto detected = std::chrono::steady_clock::now();
    cv::Mat computed;
    DescribeViewportSurf(gray,found,computed,interrupted,statistics);
    checkpoint();
    if (statistics) {
        statistics->detectMilliseconds = std::chrono::duration<double,std::milli>(detected-started).count();
        statistics->describeMilliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-detected).count();
    }
    points=std::move(found); descriptors=std::move(computed);
}
