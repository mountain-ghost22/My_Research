#include "include/frame.h"
#include "include/converter.h"
#include "include/orbmatcher.h"
#include <thread>
using namespace cv;


long unsigned int Frame::nNextId=0;
bool Frame::mbInitialComputations=true;
float Frame::cx, Frame::cy, Frame::fx, Frame::fy, Frame::invfx, Frame::invfy;
float Frame::mnMinX, Frame::mnMinY, Frame::mnMaxX, Frame::mnMaxY;
float Frame::mfGridElementWidthInv, Frame::mfGridElementHeightInv;

Frame::Frame()
{}

/**
 * @brief Copy constructor
 *
 * 复制构造函数, mLastFrame = Frame(mCurrentFrame)
 */
Frame::Frame(const Frame &frame)
    :mpORBvocabulary(frame.mpORBvocabulary), mpORBextractorLeft(frame.mpORBextractorLeft),
     mTimeStamp(frame.mTimeStamp), mK(frame.mK.clone()), mDistCoef(frame.mDistCoef.clone()),
     N(frame.N), mvKeys(frame.mvKeys), mvKeysUn(frame.mvKeysUn), mBowVec(frame.mBowVec),
     mFeatVec(frame.mFeatVec), mDescriptors(frame.mDescriptors.clone()),
     mvpMapPoints(frame.mvpMapPoints), mvbOutlier(frame.mvbOutlier), mnId(frame.mnId),
     mpReferenceKF(frame.mpReferenceKF), mnScaleLevels(frame.mnScaleLevels),
     mfScaleFactor(frame.mfScaleFactor), mfLogScaleFactor(frame.mfLogScaleFactor),
     mvScaleFactors(frame.mvScaleFactors), mvInvScaleFactors(frame.mvInvScaleFactors),
     mvLevelSigma2(frame.mvLevelSigma2), mvInvLevelSigma2(frame.mvInvLevelSigma2)
{
    for(int i=0;i<FRAME_GRID_COLS;i++)
        for(int j=0; j<FRAME_GRID_ROWS; j++)
            mGrid[i][j]=frame.mGrid[i][j];

    if(!frame.mTcw.empty())//非空
        SetPose(frame.mTcw);
}

// 单目初始化
Frame::Frame(const cv::Mat &imGray, const double &timeStamp, ORBextractor* extractor,ORBVocabulary* voc, cv::Mat &K, cv::Mat &distCoef)
    :mpORBvocabulary(voc),mpORBextractorLeft(extractor),  mTimeStamp(timeStamp),
      mK(K.clone()),mDistCoef(distCoef.clone())
{
    // Frame ID
    mnId=nNextId++;

    // Scale Level Info
    mnScaleLevels = mpORBextractorLeft->GetLevels();// 返回金子塔当前层数（0-7）
    mfScaleFactor = mpORBextractorLeft->GetScaleFactor();// 返回尺度因子（1.2）
    mfLogScaleFactor = log(mfScaleFactor);// 取对数
    // 返回各层金字塔对应的尺度，这是一个vector，第一层是1，下一层是前一层的1.2倍
    mvScaleFactors = mpORBextractorLeft->GetScaleFactors();
    mvInvScaleFactors = mpORBextractorLeft->GetInverseScaleFactors();// 也是一个vector，是上面vector的倒数
    mvLevelSigma2 = mpORBextractorLeft->GetScaleSigmaSquares();// 尺度的平方，vector类型
    mvInvLevelSigma2 = mpORBextractorLeft->GetInverseScaleSigmaSquares();// 也是一个vector，是上面vector的倒数

    // ORB extraction
    ExtractORB(imGray);

    N = mvKeys.size();

    if(mvKeys.empty())
        return;

    // 调用OpenCV的矫正函数矫正orb提取的特征点
    UndistortKeyPoints();

    mvpMapPoints = std::vector<MapPoint*>(N,static_cast<MapPoint*>(NULL));
    mvbOutlier = std::vector<bool>(N,false);

    // This is done only for the first Frame (or after a change in the calibration)
    if(mbInitialComputations)
    {
        ComputeImageBounds(imGray);

        mfGridElementWidthInv=static_cast<float>(FRAME_GRID_COLS)/static_cast<float>(mnMaxX-mnMinX);
        mfGridElementHeightInv=static_cast<float>(FRAME_GRID_ROWS)/static_cast<float>(mnMaxY-mnMinY);

        fx = K.at<float>(0,0);
        fy = K.at<float>(1,1);
        cx = K.at<float>(0,2);
        cy = K.at<float>(1,2);
        invfx = 1.0f/fx;
        invfy = 1.0f/fy;

        mbInitialComputations=false;
    }

    AssignFeaturesToGrid();
}

void Frame::AssignFeaturesToGrid()
{
    int nReserve = 0.5f*N/(FRAME_GRID_COLS*FRAME_GRID_ROWS);
    for(unsigned int i=0; i<FRAME_GRID_COLS;i++)
        for (unsigned int j=0; j<FRAME_GRID_ROWS;j++)
            mGrid[i][j].reserve(nReserve);

    // 在mGrid中记录了各特征点
    for(int i=0;i<N;i++)
    {
        const cv::KeyPoint &kp = mvKeysUn[i];

        int nGridPosX, nGridPosY;
        if(PosInGrid(kp,nGridPosX,nGridPosY))
            mGrid[nGridPosX][nGridPosY].push_back(i);
    }
}

/**
 * @brief Frame::ExtractORB 提取输入图像的特征点和描述子
 * @param im 输入图像
 */
void Frame::ExtractORB(const cv::Mat &im)
{
    (*mpORBextractorLeft)(im,mvKeys,mDescriptors);
}

/**
 * @brief Set the camera pose.
 *
 * 设置相机姿态，随后会调用 UpdatePoseMatrices() 来改变mRcw,mRwc等变量的值
 * @param Tcw Transformation from world to camera
 */
void Frame::SetPose(cv::Mat Tcw)
{
    mTcw = Tcw.clone();
    UpdatePoseMatrices();
}

/**
 * @brief Computes rotation, translation and camera center matrices from the camera pose.
 *
 * 根据Tcw计算mRcw、mtcw和mRwc、mOw
 */
void Frame::UpdatePoseMatrices()
{
    // [x_camera 1] = [R|t]*[x_world 1]，坐标为齐次形式
    // x_camera = R*x_world + t
    mRcw = mTcw.rowRange(0,3).colRange(0,3);
    mRwc = mRcw.t();//转置
    mtcw = mTcw.rowRange(0,3).col(3);//平移向量
    // mtcw, 即相机坐标系下相机坐标系到世界坐标系间的向量, 向量方向由相机坐标系指向世界坐标系
    // mOw, 即世界坐标系下世界坐标系到相机坐标系间的向量, 向量方向由世界坐标系指向相机坐标系
    mOw = -mRwc*mtcw;
}

/**
 * @brief 判断一个点是否在视野内
 *
 * 计算了重投影坐标，观测方向夹角，预测在当前帧的尺度
 * @param  pMP             MapPoint
 * @param  viewingCosLimit 视角和平均视角的方向阈值
 * @return                 true if is in view
 * @see SearchLocalPoints()
 */
bool Frame::isInFrustum(MapPoint *pMP, float viewingCosLimit)
{
    pMP->mbTrackInView = false;

    // 3D in absolute coordinates
    cv::Mat P = pMP->GetWorldPos();

    // 3D in camera coordinates
    // 3D点P在相机坐标系下的坐标
    const cv::Mat Pc = mRcw*P+mtcw; // 这里的Rt是经过初步的优化后的
    const float &PcX = Pc.at<float>(0);
    const float &PcY = Pc.at<float>(1);
    const float &PcZ = Pc.at<float>(2);

    // Check positive depth
    if(PcZ<0.0f)
        return false;

    // Project in image and check it is not outside
    // V-D 1) 将MapPoint投影到当前帧, 并判断是否在图像内
    const float invz = 1.0f/PcZ;
    const float u=fx*PcX*invz+cx;
    const float v=fy*PcY*invz+cy;

    if(u<mnMinX || u>mnMaxX)
        return false;
    if(v<mnMinY || v>mnMaxY)
        return false;

    // Check distance is in the scale invariance region of the MapPoint
    // V-D 3) 计算MapPoint到相机中心的距离, 并判断是否在尺度变化的距离内
    const float maxDistance = pMP->GetMaxDistanceInvariance();
    const float minDistance = pMP->GetMinDistanceInvariance();
    // 世界坐标系下，相机到3D点P的向量, 向量方向由相机指向3D点P
    const cv::Mat PO = P-mOw;
    const float dist = cv::norm(PO);

    if(dist<minDistance || dist>maxDistance)
        return false;

    // Check viewing angle
    // V-D 2) 计算当前视角和平均视角夹角的余弦值, 若小于cos(60), 即夹角大于60度则返回
    cv::Mat Pn = pMP->GetNormal();

    const float viewCos = PO.dot(Pn)/dist;

    if(viewCos<viewingCosLimit)
        return false;

    // Predict scale in the image
    // V-D 4) 根据深度预测尺度（对应特征点在一层）
    const int nPredictedLevel = pMP->PredictScale(dist,this);

    // Data used by the tracking
    // 标记该点将来要被投影
    pMP->mbTrackInView = true;
    pMP->mTrackProjX = u;
    pMP->mTrackProjY = v;
    pMP->mnTrackScaleLevel = nPredictedLevel;
    pMP->mTrackViewCos = viewCos;

    return true;
}

/**
 * @brief 找到在 以x,y为中心,边长为2r的方形内且在[minLevel, maxLevel]的特征点
 * @param x        图像坐标u，这个坐标是在初始化第一帧中
 * @param y        图像坐标v
 * @param r        边长
 * @param minLevel 最小尺度
 * FRAME_GRID_COLS 64
 * @param maxLevel 最大尺度
 * @return         满足条件的特征点的序号
 */
std::vector<size_t> Frame::GetFeaturesInArea(const float &x, const float  &y, const float  &r, const int minLevel, const int maxLevel) const
{
    std::vector<size_t> vIndices;
    vIndices.reserve(N);

    // 坐标乘以mfGridElementWidthInv和mfGridElementHeightInv就可以确定在哪个格子，降低匹配复杂度
    // 步骤一：确定该特征点在x方向上可能在最小窗口位置
    const int nMinCellX = cv::max(0, (int)floor((x-mnMinX-r)*mfGridElementWidthInv));
    if(nMinCellX>=FRAME_GRID_COLS)
        return vIndices;

    // 步骤二：确定该特征点在x方向上可能在最大窗口位置
    const int nMaxCellX = cv::min((int)FRAME_GRID_COLS-1,(int)ceil((x-mnMinX+r)*mfGridElementWidthInv));
    if(nMaxCellX<0)
        return vIndices;

    // 步骤三：确定该特征点在y方向上可能在最小窗口位置
    const int nMinCellY = cv::max(0,(int)floor((y-mnMinY-r)*mfGridElementHeightInv));
    if(nMinCellY>=FRAME_GRID_ROWS)
        return vIndices;

    // 步骤四：确定该特征点在y方向上可能在最大窗口位置
    const int nMaxCellY = min((int)FRAME_GRID_ROWS-1,(int)ceil((y-mnMinY+r)*mfGridElementHeightInv));
    if(nMaxCellY<0)
        return vIndices;

    const bool bCheckLevels = (minLevel>0) || (maxLevel>=0);// false

    // 步骤五：遍历这些可能的窗口，保存与该特征点在同一个窗口的，所有特征点。
    for(int ix = nMinCellX; ix<=nMaxCellX; ix++)
    {
        for(int iy = nMinCellY; iy<=nMaxCellY; iy++)
        {
            const std::vector<size_t> vCell = mGrid[ix][iy];
            if(vCell.empty())
                continue;

            for(size_t j=0, jend=vCell.size(); j<jend; j++)
            {
                const cv::KeyPoint &kpUn = mvKeysUn[vCell[j]];
                if(bCheckLevels)
                {
                    if(kpUn.octave<minLevel)
                        continue;
                    if(maxLevel>=0)
                        if(kpUn.octave>maxLevel)
                            continue;
                }

                const float distx = kpUn.pt.x-x;
                const float disty = kpUn.pt.y-y;

                //
                if(fabs(distx)<r && fabs(disty)<r)
                    vIndices.push_back(vCell[j]);
            }
        }
    }

    return vIndices;
}

bool Frame::PosInGrid(const cv::KeyPoint &kp, int &posX, int &posY)
{
    posX = round((kp.pt.x-mnMinX)*mfGridElementWidthInv);
    posY = round((kp.pt.y-mnMinY)*mfGridElementHeightInv);

    //Keypoint's coordinates are undistorted, which could cause to go out of the image
    if(posX<0 || posX>=FRAME_GRID_COLS || posY<0 || posY>=FRAME_GRID_ROWS)
        return false;

    return true;
}

/**
 * @brief Bag of Words Representation
 *
 * 计算词包mBowVec和mFeatVec，其中mFeatVec记录了属于第i个node（在第4层）的ni个描述子
 * @see CreateInitialMapMonocular() TrackReferenceKeyFrame() Relocalization()
 */
void Frame::ComputeBoW()
{
    if(mBowVec.empty())
    {
        std::vector<cv::Mat> vCurrentDesc = Converter::toDescriptorVector(mDescriptors);
        mpORBvocabulary->transform(vCurrentDesc,mBowVec,mFeatVec,4);
    }
}

// 调用OpenCV的矫正函数矫正orb提取的特征点
void Frame::UndistortKeyPoints()
{
    // 如果没有图像是矫正过的，没有失真
    if(mDistCoef.at<float>(0)==0.0)// changed from == to !=
    {
        mvKeysUn=mvKeys;
        return;
    }

    // Fill matrix with points
    // N为提取的特征点数量，将N个特征点保存在N*2的mat中
    cv::Mat mat(N,2,CV_32F);
    for(int i=0; i<N; i++)
    {
        mat.at<float>(i,0)=mvKeys[i].pt.x;
        mat.at<float>(i,1)=mvKeys[i].pt.y;
    }

    // Undistort points
    // 调整mat的通道为2，矩阵的行列形状不变
    mat=mat.reshape(2);
    cv::undistortPoints(mat,mat,mK,mDistCoef,cv::Mat(),mK); // 用cv的函数进行失真校正
    mat=mat.reshape(1);

    // Fill undistorted keypoint vector
    // 存储校正后的特征点
    mvKeysUn.resize(N);
    for(int i=0; i<N; i++)
    {
        cv::KeyPoint kp = mvKeys[i];
        kp.pt.x=mat.at<float>(i,0);
        kp.pt.y=mat.at<float>(i,1);
        mvKeysUn[i]=kp;
    }
}

void Frame::ComputeImageBounds(const cv::Mat &imLeft)
{
    if(mDistCoef.at<float>(0)!=0.0)
    {
        // 矫正前四个边界点：(0,0) (cols,0) (0,rows) (cols,rows)
        cv::Mat mat(4,2,CV_32F);
        mat.at<float>(0,0)=0.0f;         //左上
        mat.at<float>(0,1)=0.0f;
        mat.at<float>(1,0)=imLeft.cols; //右上
        mat.at<float>(1,1)=0.0f;
        mat.at<float>(2,0)=0.0f;         //左下
        mat.at<float>(2,1)=imLeft.rows;
        mat.at<float>(3,0)=imLeft.cols; //右下
        mat.at<float>(3,1)=imLeft.rows;

        // Undistort corners
        mat=mat.reshape(2);// channel change to 2
        cv::undistortPoints(mat,mat,mK,mDistCoef,cv::Mat(),mK);
        mat=mat.reshape(1);// channel change to 1

        // after modify distort image the range
        mnMinX = cv::min(mat.at<float>(0,0),mat.at<float>(2,0));//左上和左下横坐标最小的
        mnMaxX = cv::max(mat.at<float>(1,0),mat.at<float>(3,0));//右上和右下横坐标最大的
        mnMinY = cv::min(mat.at<float>(0,1),mat.at<float>(1,1));//左上和右上纵坐标最小的
        mnMaxY = cv::max(mat.at<float>(2,1),mat.at<float>(3,1));//左下和右下纵坐标最小的
    }
    else
    {
        mnMinX = 0.0f;
        mnMaxX = imLeft.cols;
        mnMinY = 0.0f;
        mnMaxY = imLeft.rows;
    }
}
