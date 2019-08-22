#include <iostream>
#include <fstream>
#include <Eigen/Core>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
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
#include <glow/GlUniform.h>
#include <glow/util/X11OffscreenContext.h>

using namespace glow;


std::vector<vec3> loadLidarPoints(const std::string& bin_file ) {
    // load point cloud
    std::fstream input(bin_file, std::ios::in | std::ios::binary);
    if(!input.good()){
        std::cerr << "Could not read file: " << bin_file << std::endl;
        exit(EXIT_FAILURE);
    }
    input.seekg(0, std::ios::beg);

    std::vector<vec3> points;

    int i;
    for (i=0; input.good() && !input.eof(); i++) {
        vec3 pt;
        float intensity;
        input.read((char *) &pt.x, 3*sizeof(float));
        input.read((char *) &intensity, sizeof(float));
        points.push_back(pt);
    }
    input.close();
//    std::cout << "Read KTTI point cloud with " << i << " points" << std::endl;
    return points;
}
int main() {
    std::string image_file = "/home/pang/disk/dataset/kitti/00/image_0/000000.png";
    std::string lidarscan_file = "/home/pang/disk/dataset/kitti/00/velodyne/000000.bin";

    cv::Mat image = cv::imread(image_file, CV_LOAD_IMAGE_COLOR);
//    cv::imshow("image", image);
//    cv::waitKey(3000);
    std::vector<vec3> lidar_points = loadLidarPoints(lidarscan_file);


    int image_width = image.cols;
    int image_height = image.rows;
    std::cout << "lidar_points: " << lidar_points.size() << std::endl;
//    for (auto i : lidar_points) {
//        std::cout << i.x << " " << i.y << " " << i.z << std::endl;
//    }


   Eigen::Matrix4f T_cam_lidar;
   T_cam_lidar <<4.276802385584e-04, -9.999672484946e-01, -8.084491683471e-03, -1.198459927713e-02,
                 -7.210626507497e-03, 8.081198471645e-03, -9.999413164504e-01, -5.403984729748e-02,
                 9.999738645903e-01, 4.859485810390e-04, -7.206933692422e-03, -2.921968648686e-01,
                 0,0,0,1;

    float fx = 7.188560000000e+02;
    float fy = 7.188560000000e+02;
    float cx = 6.071928000000e+02;
    float cy = 1.852157000000e+02;

    vec4 intrinsic(fx, fy, cx, cy);
    vec2 image_wh(image_width, image_height);


    // init window
    glow::X11OffscreenContext ctx(3,3);  // OpenGl context
    glow::inititializeGLEW();


    glow::GlBuffer<vec3> input_vec{glow::BufferTarget::ARRAY_BUFFER,
                                   glow::BufferUsage::DYNAMIC_DRAW};  // feedback stores updated input_vec inside input_vec.

    glow::GlBuffer<vec3> extractBuffer{glow::BufferTarget::ARRAY_BUFFER, glow::BufferUsage::DYNAMIC_DRAW};
    glow::GlProgram extractProgram;
    glow::GlTransformFeedback extractFeedback;


    input_vec.assign(lidar_points);
    std::cout << "input_vec: " << input_vec.size() << std::endl;
    std::vector<std::string> varyings{
            "position_out",
    };
    extractBuffer.reserve(2 * input_vec.size());
    extractFeedback.attach(varyings, extractBuffer);

    glow::GlVertexArray vao_input_vec;
    // now we can set the vertex attributes. (the "shallow copy" of input_vec now contains the correct id.
    vao_input_vec.setVertexAttribute(0, input_vec, 3, AttributeType::FLOAT, false, sizeof(vec3),
                                     reinterpret_cast<GLvoid*>(0));


    extractProgram.attach(GlShader::fromFile(ShaderType::VERTEX_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/detect_in_view.vert"));
    extractProgram.attach(GlShader::fromFile(ShaderType::FRAGMENT_SHADER, "/home/pang/suma_ws/src/glow/samples/shader/empty.frag"));
    extractProgram.attach(extractFeedback);
    extractProgram.link();

    extractProgram.setUniform(GlUniform<Eigen::Matrix4f>("T_cam_lidar", T_cam_lidar));
    extractProgram.setUniform(GlUniform<vec2>("image_wh", image_wh));
    extractProgram.setUniform(GlUniform<vec4>("intrinsic", intrinsic));



    extractFeedback.bind();
    extractProgram.bind();

    glEnable(GL_RASTERIZER_DISCARD);

    extractProgram.bind();
    extractFeedback.bind();
    vao_input_vec.bind();



    extractFeedback.begin(TransformFeedbackMode::POINTS);
    glDrawArrays(GL_POINTS, 0, input_vec.size());
    uint32_t extractedSize = extractFeedback.end();

    extractBuffer.resize(extractedSize);

    std::vector<vec3> download_input_vec;
    download_input_vec.reserve(2 * input_vec.size());
    extractBuffer.get(download_input_vec);
    std::cout << "download_input_vec: " << download_input_vec.size() << std::endl;

//
//    for (int i = 0; i < download_input_vec.size(); i ++) {
//        std::cout << download_input_vec.at(i).x - lidar_points.at(i).x << " "
//                << download_input_vec.at(i).y - lidar_points.at(i).y << " "
//                << download_input_vec.at(i).z - lidar_points.at(i).z << std::endl;
//    }


    for (auto i : download_input_vec) {
        std::cout << i.x << " " << i.y << " " << i.z << std::endl;
    }



    return 0;
}