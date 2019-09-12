#include <iostream>
#include <glow/glbase.h>
#include <glow/glutil.h>

#include <glow/GlBuffer.h>
#include <glow/GlFramebuffer.h>
#include <glow/GlProgram.h>
#include <glow/GlVertexArray.h>
#include <glow/ScopedBinder.h>

#include <algorithm>
#include <random>
#include <vector>
#include <opencv2/opencv.hpp>
#include <glow/GlSampler.h>

#include <glow/GlCapabilities.h>
#include <glow/GlState.h>
#include <glow/util/X11OffscreenContext.h>

#include "timer.h"
using namespace glow;
using namespace std;



std::vector<vec4> loadLidarPoints(const std::string& bin_file ) {
    // load point cloud
    std::fstream input(bin_file, std::ios::in | std::ios::binary);
    if(!input.good()){
        std::cerr << "Could not read file: " << bin_file << std::endl;
        exit(EXIT_FAILURE);
    }
    input.seekg(0, std::ios::beg);

    std::vector<vec4> points;

    int i;
    for (i=0; input.good() && !input.eof(); i++) {
        vec4 pt;
        float intensity;
        input.read((char *) &pt.x, 3*sizeof(float));
        input.read((char *) &intensity, sizeof(float));
        pt.w = 1.0;
        points.push_back(pt);
    }
    input.close();
//    std::cout << "Read KTTI point cloud with " << i << " points" << std::endl;
    return points;
}



inline void bilinearWeights(float x, float y, float* w00, float* w01,
                            float* w10, float* w11) {
    int x_floor = static_cast<int>(x);
    int y_floor = static_cast<int>(y);

    float dx = x - x_floor;
    float dy = y - y_floor;

    /* Compute rectangles using only 1 multiply (taken from LSD-SLAM). */
    *w11 = dx * dy;
    *w01 = dx - *w11;
    *w10 = dy - *w11;
    *w00 = 1.0f - dx - dy + *w11;

    return;
}


template <typename ChannelType, typename RetType>
inline RetType bilinearInterp(uint32_t rows, uint32_t cols, std::size_t step,
                              const void* data, float x, float y) {


    int x_floor = static_cast<int>(x);
    int y_floor = static_cast<int>(y);

    float w00, w01, w10, w11;
    bilinearWeights(x, y, &w00, &w01, &w10, &w11);

    const uint8_t* datab =
            &(static_cast<const uint8_t*>(data))[y_floor * step + x_floor * sizeof(ChannelType)];

    return w00 * (*reinterpret_cast<const ChannelType*>(datab)) +
           w01 * (*reinterpret_cast<const ChannelType*>(datab + sizeof(ChannelType))) +
           w10 * (*reinterpret_cast<const ChannelType*>(datab + step)) +
           w11 * (*reinterpret_cast<const ChannelType*>(datab + sizeof(ChannelType) + step));
}



template <typename ChannelType, typename RetType>
inline RetType bilinearInterp(const cv::Mat& img, float x, float y) {
    return bilinearInterp<ChannelType, RetType>(img.rows, img.cols, img.step,
                                                img.data, x, y);
}


inline int getPointsInCameraView(const std::vector<vec4>& cloud,
                                 const cv::Mat& image,
                                 const Eigen::Matrix4d& T_cam_lidar,
                                 const Eigen::Matrix<double,3,4>& camera_intrinsic,
                                 std::vector<vec4>& in_view_cloud,
                                 std::vector<Eigen::Vector3d>& uv_with_depth) {


    Eigen::Matrix<double,3,4> project_matix = camera_intrinsic*T_cam_lidar;

    uv_with_depth.clear();
    int in_view_cnt = 0;
    for (int i = 0; i < cloud.size(); i++) {
        auto  p3d = cloud.at(i);
        Eigen::Vector4d p4d(p3d.x, p3d.y,p3d.z,1.0);
        Eigen::Vector4d p4d_in_camera_frame = T_cam_lidar* p4d;
        Eigen::Vector3d p3d_in_camera = project_matix*p4d;
        if (p3d_in_camera(2) < 5 || p3d_in_camera(2) > 70) continue;

        Eigen::Vector3d uv(p3d_in_camera(0)/p3d_in_camera(2),
                           p3d_in_camera(1)/p3d_in_camera(2), p3d_in_camera(2));

        if (uv(0) >=0 && uv(0) <= image.cols && uv(1) >=0 && uv(1) <= image.rows) {
            in_view_cnt ++;
            vec4 pointXyzi;
            pointXyzi.x = (float)p4d_in_camera_frame[0];
            pointXyzi.y = (float)p4d_in_camera_frame[1];
            pointXyzi.z = (float)p4d_in_camera_frame[2];

            auto intensity = bilinearInterp<uchar,float>(image, float(uv(0)),
                                                         float(uv(1)));
            pointXyzi.w = intensity;

            uv_with_depth.push_back(uv);

            in_view_cloud.push_back(pointXyzi);
//            colors.push_back()
        }
    }

    return in_view_cnt;
}

struct PointInView {
    float x,y,z;
    float r,g,b;
    float u,v;
};


typedef vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d>> VecVector2d;

// Camera intrinsics
double fx = 718.856, fy = 718.856, cx = 607.1928, cy = 185.2157;

// useful typedefs
typedef Eigen::Matrix<double, 6, 6> Matrix6d;
typedef Eigen::Matrix<double, 2, 6> Matrix26d;
typedef Eigen::Matrix<double, 6, 1> Vector6d;

/// class for accumulator jacobians in parallel
class JacobianAccumulator {
public:
    JacobianAccumulator(
            const cv::Mat &img1_,
            const cv::Mat &img2_,
            const VecVector2d &px_ref_,
            const vector<double> depth_ref_,
            Eigen::Matrix4d &T21_) :
            img1(img1_), img2(img2_), px_ref(px_ref_), depth_ref(depth_ref_), T21(T21_) {
        projection = VecVector2d(px_ref.size(), Eigen::Vector2d(0, 0));
    }

    /// accumulate jacobians in a range
    void accumulate_jacobian(const cv::Range &range);

    /// get hessian matrix
    Matrix6d hessian() const { return H; }

    /// get bias
    Vector6d bias() const { return b; }

    /// get total cost
    double cost_func() const { return cost; }

    /// get projected points
    VecVector2d projected_points() const { return projection; }

    /// reset h, b, cost to zero
    void reset() {
        H = Matrix6d::Zero();
        b = Vector6d::Zero();
        cost = 0;
    }

private:
    const cv::Mat &img1;
    const cv::Mat &img2;
    const VecVector2d &px_ref;
    const vector<double> depth_ref;
    Eigen::Matrix4d &T21;
    VecVector2d projection; // projected points

    // std::mutex hessian_mutex;;
    Matrix6d H = Matrix6d::Zero();
    Vector6d b = Vector6d::Zero();
    double cost = 0;
};


Eigen::Matrix4d exp(const Eigen::VectorXd& x) {
    //  std::cout << "exponential of " << x.transpose() << std::endl;
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();

    // see  phd thesis of Hauke Strasdat, 2012 pp 47-53:
    Eigen::Vector3d v(x[0], x[1], x[2]);
    Eigen::Vector3d omega(x[3], x[4], x[5]);

    double theta = omega.norm();
    if (theta > 1e-10) {
        Eigen::Matrix3d omega_skew;
        omega_skew << 0, -omega[2], omega[1], omega[2], 0, -omega[0], -omega[1], omega[0], 0;
        Eigen::Matrix3d omega_skew_sqr = omega_skew * omega_skew;

        // Rotation:
        double alpha = sin(theta) / theta;
        double beta = (1 - cos(theta)) / (theta * theta);
        result.topLeftCorner(3, 3) = Eigen::Matrix3d::Identity() + alpha * omega_skew + beta * omega_skew_sqr;

        // Translation:
        double gamma = (1.0f - cos(theta)) / (theta * theta);
        double delta = (theta - sin(theta)) / (theta * theta * theta);

        Eigen::Matrix3d V = Eigen::Matrix3d::Identity() + gamma * omega_skew + delta * omega_skew_sqr;
        result.block<3, 1>(0, 3) = V * v;
    } else {
        result.block<3, 1>(0, 3) = v;  // just translation.
    }

    return result;
}
void DirectPoseEstimationSingleLayer(
        const cv::Mat &img1,
        const cv::Mat &img2,
        const VecVector2d &px_ref,
        const vector<double> depth_ref,
        Eigen::Matrix4d &T21) {

    const int iterations = 10;
    double cost = 0, lastCost = 0;
    JacobianAccumulator jaco_accu(img1, img2, px_ref, depth_ref, T21);

    for (int iter = 0; iter < iterations; iter++) {
        jaco_accu.reset();
        cv::parallel_for_(cv::Range(0, px_ref.size()),
                          std::bind(&JacobianAccumulator::accumulate_jacobian, &jaco_accu, std::placeholders::_1));
        Matrix6d H = jaco_accu.hessian();
        Vector6d b = jaco_accu.bias();

        // solve update and put it into estimation
        Vector6d update = H.ldlt().solve(b);;
        T21 = exp(update) * T21;
        cost = jaco_accu.cost_func();

        if (std::isnan(update[0])) {
            // sometimes occurred when we have a black or white patch and H is irreversible
            cout << "update is nan" << endl;
            break;
        }
        if (iter > 0 && cost > lastCost) {
            cout << "cost increased: " << cost << ", " << lastCost << endl;
            break;
        }
        if (update.norm() < 1e-3) {
            // converge
            break;
        }

        lastCost = cost;
        cout << "iteration: " << iter << ", cost: " << cost << endl;
    }

    // cout << "T21 = \n" << T21.matrix() << endl;


    // // plot the projected pixels here
    // cv::Mat img2_show;
    // cv::cvtColor(img2, img2_show, CV_GRAY2BGR);
    // VecVector2d projection = jaco_accu.projected_points();
    // for (size_t i = 0; i < px_ref.size(); ++i) {
    //     auto p_ref = px_ref[i];
    //     auto p_cur = projection[i];
    //     if (p_cur[0] > 0 && p_cur[1] > 0) {
    //         cv::circle(img2_show, cv::Point2f(p_cur[0], p_cur[1]), 2, cv::Scalar(0, 250, 0), 2);
    //         cv::line(img2_show, cv::Point2f(p_ref[0], p_ref[1]), cv::Point2f(p_cur[0], p_cur[1]),
    //                  cv::Scalar(0, 250, 0));
    //     }
    // }
    // cv::imshow("current", img2_show);
    // cv::waitKey();
}


// bilinear interpolation
inline float GetPixelValue(const cv::Mat &img, float x, float y) {
    // boundary check
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= img.cols) x = img.cols - 1;
    if (y >= img.rows) y = img.rows - 1;
    uchar *data = &img.data[int(y) * img.step + int(x)];
    float xx = x - floor(x);
    float yy = y - floor(y);
    return float(
            (1 - xx) * (1 - yy) * data[0] +
            xx * (1 - yy) * data[1] +
            (1 - xx) * yy * data[img.step] +
            xx * yy * data[img.step + 1]
    );
}

void JacobianAccumulator::accumulate_jacobian(const cv::Range &range) {

    // parameters
    const int half_patch_size = 1;
    int cnt_good = 0;
    Matrix6d hessian = Matrix6d::Zero();
    Vector6d bias = Vector6d::Zero();
    double cost_tmp = 0;

    for (size_t i = range.start; i < range.end; i++) {

        // compute the projection in the second image
        Eigen::Vector3d point_ref =
                depth_ref[i] * Eigen::Vector3d((px_ref[i][0] - cx) / fx, (px_ref[i][1] - cy) / fy, 1);
        Eigen::Vector3d point_cur = T21.topLeftCorner(3,3) * point_ref + T21.topRightCorner(3,1);
        if (point_cur[2] < 0)   // depth invalid
            continue;

        float u = fx * point_cur[0] / point_cur[2] + cx, v = fy * point_cur[1] / point_cur[2] + cy;
        if (u < half_patch_size || u > img2.cols - half_patch_size || v < half_patch_size ||
            v > img2.rows - half_patch_size)
            continue;

        projection[i] = Eigen::Vector2d(u, v);
        double X = point_cur[0], Y = point_cur[1], Z = point_cur[2],
                Z2 = Z * Z, Z_inv = 1.0 / Z, Z2_inv = Z_inv * Z_inv;
        cnt_good++;

        // and compute error and jacobian
        for (int x = -half_patch_size; x <= half_patch_size; x++)
            for (int y = -half_patch_size; y <= half_patch_size; y++) {

                double error = GetPixelValue(img1, px_ref[i][0] + x, px_ref[i][1] + y) -
                               GetPixelValue(img2, u + x, v + y);
                Matrix26d J_pixel_xi;
                Eigen::Vector2d J_img_pixel;

                J_pixel_xi(0, 0) = fx * Z_inv;
                J_pixel_xi(0, 1) = 0;
                J_pixel_xi(0, 2) = -fx * X * Z2_inv;
                J_pixel_xi(0, 3) = -fx * X * Y * Z2_inv;
                J_pixel_xi(0, 4) = fx + fx * X * X * Z2_inv;
                J_pixel_xi(0, 5) = -fx * Y * Z_inv;

                J_pixel_xi(1, 0) = 0;
                J_pixel_xi(1, 1) = fy * Z_inv;
                J_pixel_xi(1, 2) = -fy * Y * Z2_inv;
                J_pixel_xi(1, 3) = -fy - fy * Y * Y * Z2_inv;
                J_pixel_xi(1, 4) = fy * X * Y * Z2_inv;
                J_pixel_xi(1, 5) = fy * X * Z_inv;

                J_img_pixel = Eigen::Vector2d(
                        0.5 * (GetPixelValue(img2, u + 1 + x, v + y) - GetPixelValue(img2, u - 1 + x, v + y)),
                        0.5 * (GetPixelValue(img2, u + x, v + 1 + y) - GetPixelValue(img2, u + x, v - 1 + y))
                );

                // total jacobian
                Vector6d J = -1.0 * (J_img_pixel.transpose() * J_pixel_xi).transpose();

                hessian += J * J.transpose();
                bias += -error * J;
                cost_tmp += error * error;
            }
    }

    if (cnt_good) {
        // set hessian, bias and cost
        H += hessian;
        b += bias;
        cost += cost_tmp / cnt_good;
    }
}







int main(int argc, char** argv) {

    std::string image0_file = "/home/pang/data/dataset/kitti/00/image_0/000000.png";
    std::string image1_file = "/home/pang/data/dataset/kitti/00/image_0/000001.png";
    std::string lidarscan1_file = "/home/pang/data/dataset/kitti/00/velodyne/000001.bin";

    cv::Mat image0 = cv::imread(image0_file, CV_LOAD_IMAGE_COLOR);
    cv::Mat image1 = cv::imread(image1_file, CV_LOAD_IMAGE_COLOR);

    std::vector<vec4> lidar_points = loadLidarPoints(lidarscan1_file);
    Eigen::Matrix4f T_cam_lidar;
    T_cam_lidar <<4.276802385584e-04, -9.999672484946e-01, -8.084491683471e-03, -1.198459927713e-02,
            -7.210626507497e-03, 8.081198471645e-03, -9.999413164504e-01, -5.403984729748e-02,
            9.999738645903e-01, 4.859485810390e-04, -7.206933692422e-03, -2.921968648686e-01,
            0,0,0,1;

    float fx = 7.188560000000e+02;
    float fy = 7.188560000000e+02;
    float cx = 6.071928000000e+02;
    float cy = 1.852157000000e+02;

    Eigen::Matrix<double,3,4,Eigen::RowMajor> intrinsic_mat;
    intrinsic_mat << 7.188560000000e+02, 0.000000000000e+00, 6.071928000000e+02, 0.000000000000e+00,
            0.000000000000e+00, 7.188560000000e+02, 1.852157000000e+02, 0.000000000000e+00,
            0.000000000000e+00, 0.000000000000e+00, 1.000000000000e+00, 0.000000000000e+00;

    Eigen::Matrix4f T_WC0;
    T_WC0 << 1.000000e+00, 9.043680e-12, 2.326809e-11, 5.551115e-17,
            9.043683e-12, 1.000000e+00, 2.392370e-10, 3.330669e-16,
            2.326810e-11, 2.392370e-10, 9.999999e-01, -4.440892e-16,
            0,0,0,1;
    Eigen::Matrix4f T_WC1;
    T_WC1 << 9.999978e-01, 5.272628e-04, -2.066935e-03, -4.690294e-02,
            -5.296506e-04, 9.999992e-01, -1.154865e-03, -2.839928e-02,
            2.066324e-03, 1.155958e-03, 9.999971e-01, 8.586941e-01,
            0,0,0,1;

    Eigen::Matrix4f T_C0C1 = T_WC0.inverse() * T_WC1;

    Eigen::Matrix4f T_L0L1 = T_cam_lidar.inverse() * T_C0C1 * T_cam_lidar;


    uint32_t width = image1.cols;
    uint32_t height = image1.rows;
    vec4 intrinsic(fx, fy, cx, cy);
    vec2 image_wh(width, height);

    std::vector<vec4> in_view_cloud;
    std::vector<Eigen::Vector3d> uv_with_depth;
    cv::Mat gray_image;
    cv::cvtColor(image1, gray_image, CV_RGB2GRAY);
    auto cnt = getPointsInCameraView(lidar_points,
                                     gray_image,
                                     T_cam_lidar.cast<double>(),
                                     intrinsic_mat,
                                     in_view_cloud,
                                     uv_with_depth);




    std::cout << "lidar_points: " << lidar_points.size() << std::endl;
    std::cout << "in view points: " << in_view_cloud.size() << std::endl;

    std::vector<PointInView> point_in_view_vec;
    for (int i = 0; i < in_view_cloud.size(); i++) {
        PointInView pointInView;
        pointInView.x = in_view_cloud.at(i).x;
        pointInView.y = in_view_cloud.at(i).y;
        pointInView.z = in_view_cloud.at(i).z;

        pointInView.r = in_view_cloud.at(i).w;
        pointInView.g = in_view_cloud.at(i).w;
        pointInView.b = in_view_cloud.at(i).w;

        point_in_view_vec.push_back(pointInView);
    }

    std::cout << "get in view: " << point_in_view_vec.size() << std::endl;




    // init window
    glow::X11OffscreenContext ctx(3,3);  // OpenGl context
    glow::inititializeGLEW();





    cv::Mat float_image0, float_image1;
    image0.convertTo(float_image0, CV_32FC3);
    image1.convertTo(float_image1, CV_32FC3);

    GlTexture last_texture{width, height, TextureFormat::RGBA_FLOAT};
    last_texture.assign(PixelFormat::RGB, PixelType::FLOAT, float_image0.ptr());

    GlTexture cur_texture{width, height, TextureFormat::RGBA_FLOAT};
    cur_texture.assign(PixelFormat::RGB, PixelType::FLOAT, float_image1.ptr());

    GlTexture output0{width, height, TextureFormat::RGBA_FLOAT};
    GlRenderbuffer rbo(width, height, RenderbufferFormat::DEPTH_STENCIL);



//    glow::GlBuffer<PointInView> extractBuffer{glow::BufferTarget::ARRAY_BUFFER, glow::BufferUsage::DYNAMIC_DRAW};
//    glow::GlTransformFeedback extractFeedback;
//    std::vector<std::string> varyings{
//            "point_in_view_xyz",
//            "point_in_view_rgb",
//            "point_in_view_uv",
//    };
//    extractBuffer.reserve(2 * lidar_points.size());
//    extractFeedback.attach(varyings, extractBuffer);

    GlFramebuffer fbo(width, height);
    fbo.attach(FramebufferAttachment::COLOR0, output0);
    CheckGlError();
    fbo.attach(FramebufferAttachment::DEPTH_STENCIL, rbo);
    CheckGlError();


    GlProgram program;
    program.attach(GlShader::fromFile(ShaderType::VERTEX_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/blend_sum.vert"));
//    program.attach(GlShader::fromFile(ShaderType::GEOMETRY_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/blend_sum.geom"));
    program.attach(GlShader::fromFile(ShaderType::FRAGMENT_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/blend_sum.frag"));
//    program.attach(extractFeedback);

    program.link();

    vec2 wh(width, height);
    program.setUniform(GlUniform<Eigen::Matrix4f>("T_L0_L1", T_L0L1));
    program.setUniform(GlUniform<Eigen::Matrix4f>("T_Cam_Lidar", T_cam_lidar));
    program.setUniform(GlUniform<Eigen::Matrix4f>("T_Lidar_Cam", T_cam_lidar.inverse()));
    program.setUniform(GlUniform<vec2>("wh", wh));
    program.setUniform(GlUniform<vec4>("intrinsic", intrinsic));
    program.setUniform(GlUniform<int32_t>("cur_texture", 0));
    program.setUniform(GlUniform<int32_t>("last_texture", 1));

    glow::GlBuffer<PointInView> buffer{glow::BufferTarget::ARRAY_BUFFER,
                                       glow::BufferUsage::DYNAMIC_DRAW};
    buffer.assign(point_in_view_vec);
    GlVertexArray vao;
    vao.setVertexAttribute(0, buffer, 3, AttributeType::FLOAT, false, sizeof(PointInView),
                                   reinterpret_cast<GLvoid*>(0));
    vao.setVertexAttribute(1, buffer, 3, AttributeType::FLOAT, false, sizeof(PointInView),
                                   reinterpret_cast<GLvoid*>(3 * sizeof(GLfloat)));
    vao.setVertexAttribute(2, buffer, 2, AttributeType::INT, false, sizeof(PointInView),
                                   reinterpret_cast<GLvoid*>(6 * sizeof(GLfloat)));


    GlSampler sampler;
    sampler.setMagnifyingOperation(TexMagOp::NEAREST);
    sampler.setMinifyingOperation(TexMinOp::NEAREST); //

    glDisable(GL_DEPTH_TEST);

//    extractFeedback.bind();
    sampler.bind(0);
    fbo.bind();
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, width, height);
    program.bind();
    vao.bind();
    glActiveTexture(GL_TEXTURE0);
    cur_texture.bind();
    glActiveTexture(GL_TEXTURE1);
    last_texture.bind();

//    extractFeedback.begin(TransformFeedbackMode::POINTS);
    glDrawArrays(GL_POINTS, 0, lidar_points.size());
//    uint32_t extractedSize = extractFeedback.end();
//    extractBuffer.resize(extractedSize);

//    std::vector<PointInView> download_input_vec;
//    download_input_vec.reserve(2 * lidar_points.size());
//    extractBuffer.get(download_input_vec);

    vao.release();
    program.release();
    fbo.release();
//    extractFeedback.release();

    sampler.release(0);

    glActiveTexture(GL_TEXTURE0);
    cur_texture.release();
    glActiveTexture(GL_TEXTURE1);
    last_texture.release();

    glEnable(GL_DEPTH_TEST);


    // retrieve result
    std::vector<vec4> data0;
    output0.download(data0);

    cv::Mat out_image0(height,width, CV_8UC1);
    for (int i = 0; i < width* height; i++) {
        int x = i % width;
        int y = i / width;
        out_image0.at<uchar>(y,x) =   data0[i].x ;



    }

//    int total_in_view_cnt = 0;
//    cv::Mat1b point_image(height, width, CV_8UC1);
//    point_image.setTo(0);
//    cv::Mat image_gray;
//    cv::cvtColor(image1, image_gray, CV_RGB2GRAY);
//    for (auto i : download_input_vec) {
//        point_image.at<uchar>(i.v, i.u)= i.r;
//    }

//    std::cout << "total_in_view_cnt: " << total_in_view_cnt << std::endl;
//    cv::imshow("point_image", point_image);
    cv::imshow("image1", image1);
    cv::imshow("out_image0", out_image0);
    cv::waitKey(100000);

    return 0;
}