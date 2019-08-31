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
        }
    }

    return in_view_cnt;
}



int main(int argc, char** argv) {

    std::string image_file = "/home/pang/disk/dataset/kitti/00/image_0/000000.png";
    std::string lidarscan_file = "/home/pang/disk/dataset/kitti/00/velodyne/000000.bin";

    cv::Mat image = cv::imread(image_file, CV_LOAD_IMAGE_COLOR);
//    cv::imshow("image", image);
//    cv::waitKey(3000);
    std::vector<vec4> lidar_points = loadLidarPoints(lidarscan_file);
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

    uint32_t width = image.cols;
    uint32_t height = image.rows;
    vec4 intrinsic(fx, fy, cx, cy);
    vec2 image_wh(width, height);

    std::vector<vec4> in_view_cloud;
    std::vector<Eigen::Vector3d> uv_with_depth;
    cv::Mat gray_image;
    cv::cvtColor(image, gray_image, CV_RGB2GRAY);
    auto cnt = getPointsInCameraView(lidar_points,
                                     gray_image,
                                     T_cam_lidar.cast<double>(),
                                     intrinsic_mat,
                                     in_view_cloud,
                                     uv_with_depth);


    std::cout << "lidar_points: " << lidar_points.size() << std::endl;
    std::cout << "in view points: " << in_view_cloud.size() << std::endl;



    // init window
    glow::X11OffscreenContext ctx(3,3);  // OpenGl context
    glow::inititializeGLEW();

    //  std::cout << "On entry: " << GlState::queryAll() << std::endl;


//    ASSERT_NO_THROW(_CheckGlError(__FILE__, __LINE__));


    cv::Mat float_image;
    image.convertTo(float_image, CV_32FC3);

    GlTexture input_texture{width, height, TextureFormat::RGBA_FLOAT};
    input_texture.assign(PixelFormat::RGB, PixelType::FLOAT, float_image.ptr());


    GlTexture output0{width, height, TextureFormat::RGBA_FLOAT};
    GlRenderbuffer rbo(width, height, RenderbufferFormat::DEPTH_STENCIL);

    GlFramebuffer fbo(width, height);
    fbo.attach(FramebufferAttachment::COLOR0, output0);
    CheckGlError();
    fbo.attach(FramebufferAttachment::DEPTH_STENCIL, rbo);
    CheckGlError();


    GlProgram program;
    program.attach(GlShader::fromFile(ShaderType::VERTEX_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/detect_in_view_fbo_tf.vert"));
    program.attach(GlShader::fromFile(ShaderType::GEOMETRY_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/detect_in_view_fbo_tf.geom"));
    program.attach(GlShader::fromFile(ShaderType::FRAGMENT_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/detect_in_view_fbo_tf.frag"));
    program.link();

    vec2 wh(width, height);
    program.setUniform(GlUniform<Eigen::Matrix4f>("T_cam_lidar", T_cam_lidar));
    program.setUniform(GlUniform<vec2>("wh", wh));
    program.setUniform(GlUniform<vec4>("intrinsic", intrinsic));

    program.setUniform(GlUniform<int32_t>("input_texture", 0));


    GlBuffer<vec4> point_buffer{BufferTarget::ARRAY_BUFFER, BufferUsage::STATIC_DRAW};



    point_buffer.assign(lidar_points);

    GlVertexArray vao;
    // 1. set
    vao.setVertexAttribute(0, point_buffer, 4, AttributeType::FLOAT, false, 4 * sizeof(float), nullptr);
    // 2. enable
    vao.enableVertexAttribute(0);

    GlSampler sampler;
    sampler.setMagnifyingOperation(TexMagOp::NEAREST);
    sampler.setMinifyingOperation(TexMinOp::NEAREST); //

    glDisable(GL_DEPTH_TEST);

    sampler.bind(0);
    fbo.bind();
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, width, height);
    program.bind();
    vao.bind();
    glActiveTexture(GL_TEXTURE0);
    input_texture.bind();

    glDrawArrays(GL_POINTS, 0, point_buffer.size());

    vao.release();
    program.release();
    fbo.release();

    sampler.release(0);

    glActiveTexture(GL_TEXTURE0);
    input_texture.release();

    glEnable(GL_DEPTH_TEST);


    // retrieve result
    std::vector<vec4> data0;
    output0.download(data0);

    cv::Mat out_image0(height,width, CV_8UC1);
    for (int i = 0; i < width* height; i++) {
        int x = i % width;
        int y = i / width;
        out_image0.at<uchar>(y,x) =   data0[i].x ;
//        out_image0.at<cv::Vec3b>(y,x)[1] =   data0[i].y ;
//        out_image0.at<cv::Vec3b>(y,x)[2] =   data0[i].z ;


    }

    cv::imshow("image", image);
    cv::imshow("out_image0", out_image0);
    cv::waitKey(100000);

    return 0;
}