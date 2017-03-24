#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <boost/weak_ptr.hpp>
#include <boost/python.hpp>
#include <boost/python/raw_function.hpp>
#include <glog/logging.h>
#include <x265.h>
#include <Python.h>
#include <opencv2/opencv.hpp>
#include <numpy/ndarrayobject.h>
#include <libde265/de265.h>
#include <picpac-cv.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <opencv2/opencv.hpp>
#include <glog/logging.h>
#include <boost/program_options.hpp>
#include "json11.hpp"

using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::vector;
using std::string;
using std::ostringstream;
using std::runtime_error;
using boost::asio::const_buffer;
using namespace boost::python;
using namespace json11;

string const VERTEX_SHADER = R"gl(
#version 330 core
layout(location = 0) in vec2 pos_in;
layout(location = 1) in vec3 tex_in;
out vec3 tex; 
uniform mat4 mvp;
void main(){
    gl_Position =  vec4(pos_in,0,1);
    tex = (mvp * vec4(tex_in, 1)).xyz;
}
)gl";

string const FRAGMENT_SHADER = R"gl(
#version 330 core
in vec3 tex;
//layout(location=0)
out float color;
uniform sampler3D sampler;
void main(){
    color = texture(sampler, tex).r;
}
)gl";

GLuint LoadShader (GLenum shaderType, string const &buf) {
	GLuint ShaderID = glCreateShader(shaderType);
	GLint Result = GL_FALSE;
	int InfoLogLength;
    char const *ptr = buf.c_str();
	glShaderSource(ShaderID, 1, &ptr , NULL);
	glCompileShader(ShaderID);
	// Check Vertex Shader
	glGetShaderiv(ShaderID, GL_COMPILE_STATUS, &Result);
	glGetShaderiv(ShaderID, GL_INFO_LOG_LENGTH, &InfoLogLength);
	if (InfoLogLength > 0){
		string msg;
        msg.resize(InfoLogLength+1);
		glGetShaderInfoLog(ShaderID, InfoLogLength, NULL, &msg[0]);
        LOG(WARNING) << msg;
	}
    CHECK(Result);
    return ShaderID;
}

GLuint LoadProgram (string const &vshader, string const &fshader) {
	// Create the shaders
	GLuint VertexShaderID = LoadShader(GL_VERTEX_SHADER, vshader);
	GLuint FragmentShaderID = LoadShader(GL_FRAGMENT_SHADER, fshader);
	// Link the program
	LOG(INFO) << "Linking program";
	GLuint program = glCreateProgram();
	glAttachShader(program, VertexShaderID);
	glAttachShader(program, FragmentShaderID);
	glLinkProgram(program);

	GLint Result = GL_FALSE;
	int InfoLogLength;
	// Check the program
	glGetProgramiv(program, GL_LINK_STATUS, &Result);
    CHECK(Result);
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &InfoLogLength);
	if ( InfoLogLength > 0 ){
		string msg;
        msg.resize(InfoLogLength+1);
		glGetProgramInfoLog(program, InfoLogLength, NULL, &msg[0]);
        LOG(WARNING) << msg;
	}
	
	glDetachShader(program, VertexShaderID);
	glDetachShader(program, FragmentShaderID);
	
	glDeleteShader(VertexShaderID);
	glDeleteShader(FragmentShaderID);

	return program;
}

#define CHECK_POWER_OF_TWO(x) CHECK(((x)&((x)-1)) == 0)

class Sampler {
    GLFWwindow* window;
    GLuint program;
    GLuint sampler;
    GLuint itexture, otexture;
    GLuint framebuffer;
    GLuint v_pos, v_tex, v_array;
    GLuint matrix;
    std::thread::id thread_id;

    void check_thread () {
        if (thread_id != std::this_thread::get_id()) {
            LOG(ERROR) << "Cross thread rendering is not working!";
            CHECK(false);
        }
    }
public:
    static constexpr int CUBE_SIZE = 64;
    static constexpr int VOLUME_SIZE = 512;
    static constexpr int VIEW_SIZE = 512;
    static constexpr int VIEW_PIXELS = VIEW_SIZE * VIEW_SIZE;
    Sampler (): thread_id(std::this_thread::get_id()) {
        LOG(WARNING) << "Constructing sampler";
        CHECK_POWER_OF_TWO(CUBE_SIZE);
        CHECK_POWER_OF_TWO(VOLUME_SIZE);
        CHECK_POWER_OF_TWO(VIEW_SIZE);
        CHECK(CUBE_SIZE * CUBE_SIZE * CUBE_SIZE == VIEW_SIZE * VIEW_SIZE);
        if(!glfwInit()) CHECK(false) << "Failed to initialize GLFW";
        glfwWindowHint(GLFW_SAMPLES, 4);
        glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // To make MacOS happy; should not be needed
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        // we are not going to use the window and will render to texture, so size doesn't matter
        window = glfwCreateWindow(32, 32, "", NULL, NULL);
        CHECK(window) << "Failed to open GLFW window";
        glfwMakeContextCurrent(window);
        // Initialize GLEW
        glewExperimental = true; // Needed for core profile
        if (glewInit() != GLEW_OK) {
            CHECK(false) << "Failed to initialize GLEW";
        }
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_TEXTURE_3D);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

        program = LoadProgram(VERTEX_SHADER, FRAGMENT_SHADER);
        sampler = glGetUniformLocation(program, "sampler");
        matrix = glGetUniformLocation(program, "mvp");

        vector<float> pos;
        vector<float> tex;

        int o = 0;
        for (int i = 0; i < CUBE_SIZE; ++i) {
            for (int j = 0; j < CUBE_SIZE; ++j) {
                for (int k = 0; k < CUBE_SIZE; ++k) {
                    pos.push_back(2.0*(o%VIEW_SIZE+1)/VIEW_SIZE-1);
                    pos.push_back(2.0*(o/VIEW_SIZE+1)/VIEW_SIZE-1);
                    tex.push_back(1.0 * k / CUBE_SIZE);
                    tex.push_back(1.0 * j / CUBE_SIZE);
                    tex.push_back(1.0 * i / CUBE_SIZE);
                    ++o;
                }
            }
        }

        glGenVertexArrays(1, &v_array);
        glBindVertexArray(v_array);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);

        // output position doesn't change
        glGenBuffers(1, &v_pos);
        glBindBuffer(GL_ARRAY_BUFFER, v_pos);
        glBufferData(GL_ARRAY_BUFFER, sizeof(pos[0]) * pos.size(), &pos[0], GL_STATIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

        // texture sampler position doesn't change
        glGenBuffers(1, &v_tex);
        glBindBuffer(GL_ARRAY_BUFFER, v_tex);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tex[0]) * tex.size(), &tex[0], GL_STATIC_DRAW);

		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, 0);

        glGenTextures(1, &otexture);
        glBindTexture(GL_TEXTURE_2D, otexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, VIEW_SIZE, VIEW_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, otexture, 0);

        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, VIEW_SIZE, VIEW_SIZE);
		glUseProgram(program);

        glGenTextures(1, &itexture);
        glBindTexture(GL_TEXTURE_3D, itexture);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    static void strip_pad_512 (int n, int *from, int *to, int *len, int *shift) {
        if (n > VOLUME_SIZE) {
            *from = (n - VOLUME_SIZE) / 2;
            *to = 0;
            *len = VOLUME_SIZE;
            *shift = *from;
        }
        else {
            *from = 0;
            *to = (VOLUME_SIZE - n)/2;
            *len = n;
            *shift = -*to;
        }
    }

    void texture_direct (PyObject *_array) {
        check_thread();
        PyArrayObject *array = (PyArrayObject *)_array;
        CHECK(array->dimensions[0] == VOLUME_SIZE);
        CHECK(array->dimensions[1] == VOLUME_SIZE);
        CHECK(array->dimensions[2] == VOLUME_SIZE);
        CHECK(array->strides[0] = VOLUME_SIZE * VOLUME_SIZE);
        CHECK(array->strides[1] = VOLUME_SIZE);
        CHECK(array->strides[2] = 1);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RED,
                     array->dimensions[2],
                     array->dimensions[1],
                     array->dimensions[0], 0, GL_RED, GL_UNSIGNED_BYTE, array->data);
        glUniform1i(sampler, 0);
    }

    void texture (PyArrayObject *array, glm::ivec3 *off, glm::ivec3 *len, glm::ivec3 *shift) {
        check_thread();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, itexture);
        vector<uint8_t> buf(VOLUME_SIZE * VOLUME_SIZE * VOLUME_SIZE, 0);
        // copy from array to continuous buf with padding & stripping
        int from_z, to_z, n_z, shift_z;
        int from_y, to_y, n_y, shift_y;
        int from_x, to_x, n_x, shift_x;

        strip_pad_512(array->dimensions[0], &from_z, &to_z, &n_z, &shift_z);
        strip_pad_512(array->dimensions[1], &from_y, &to_y, &n_y, &shift_y);
        strip_pad_512(array->dimensions[2], &from_x, &to_x, &n_x, &shift_x);

        *off = glm::ivec3(to_x, to_y, to_z);
        *len = glm::ivec3(n_x, n_y, n_z);
        *shift = glm::ivec3(shift_x, shift_y, shift_z);

        uint8_t const *from_z_ptr = (uint8_t const *)array->data + array->strides[0] * from_z;
        uint8_t *to_z_ptr = &buf[0] + (VOLUME_SIZE * VOLUME_SIZE) * to_z;
        for (int z = 0; z < n_z; ++z, from_z_ptr += array->strides[0], to_z_ptr += VOLUME_SIZE * VOLUME_SIZE) {
            uint8_t const *from_y_ptr = from_z_ptr + array->strides[1] * from_y;
            uint8_t *to_y_ptr = to_z_ptr + VOLUME_SIZE * to_y;
            for (int y = 0; y < n_y; ++y, from_y_ptr += array->strides[1], to_y_ptr += VOLUME_SIZE) {
                uint8_t const *from_x_ptr = from_y_ptr + array->strides[2] * from_x;
                uint8_t *to_x_ptr = to_y_ptr + to_x;
                for (int x = 0; x < n_x; ++x, from_x_ptr += array->strides[2], ++to_x_ptr) {
                    *to_x_ptr = *from_x_ptr;
                }
            }
        }
#if 0
        static int cccc = 0;
        for (unsigned i = 0; i < VOLUME_SIZE; ++i) {
        ++cccc;
        char bufxx[BUFSIZ];
        sprintf(bufxx, "zzz/%d.jpg", cccc);
        cv::imwrite(bufxx, cv::Mat(Sampler::VOLUME_SIZE, Sampler::VOLUME_SIZE, CV_8U, &buf[0] + i * VOLUME_SIZE * VOLUME_SIZE));
        }
#endif
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RED, VOLUME_SIZE, VOLUME_SIZE, VOLUME_SIZE,
                     0, GL_RED, GL_UNSIGNED_BYTE, &buf[0]);
        glUniform1i(sampler, 0);
    }

    void texture_indirect (PyObject *array) {
        glm::ivec3 a, b, c;
        texture((PyArrayObject *)array, &a, &b, &c);
        cout << "OFF: " << a[0] << ' ' << a[1] << ' ' << a[2] << endl;
        cout << "LEN: " << b[0] << ' ' << b[1] << ' ' << b[2] << endl;
        cout << "SHI: " << c[0] << ' ' << c[1] << ' ' << c[2] << endl;
    }

    PyObject *sample (glm::vec3 center, glm::vec3 rotate, float scale0) { //std::default_random_engine &rng) {
        check_thread();
        // 1 -> scale -> rotate -> shift
        float scale = scale0 * CUBE_SIZE / VOLUME_SIZE;

        /*
        cout << "CENTER: " << center[0] << ' ' << center[1] << ' ' << center[2] << endl;
        cout << "ANGLE: " << rotate[0] << ' ' << rotate[1] << ' ' << rotate[2] << endl;
        cout << "SCALE: " << scale0 << endl;
        */

        glm::mat4 mvp = glm::translate(float(1.0/VOLUME_SIZE) * center) *
                        glm::scale(glm::vec3(scale, scale, scale)) *
                        glm::rotate(float(rotate[2] * 180/M_PI),   // glm requires digrees
                                glm::vec3(
                                    sin(rotate[0]) * cos(rotate[1]),
                                    sin(rotate[0]) * sin(rotate[1]),
                                    cos(rotate[0]))) *
                        glm::translate(glm::vec3(-0.5, -0.5, -0.5));
        glUniformMatrix4fv(matrix, 1, GL_FALSE, &mvp[0][0]);

		glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_POINTS, 0, VIEW_PIXELS);

        glFinish();

        npy_intp dims[] = {CUBE_SIZE, CUBE_SIZE, CUBE_SIZE};
        PyObject *_oarray = PyArray_SimpleNew(3, &dims[0], NPY_UINT8);
        PyArrayObject *oarray = (PyArrayObject*)_oarray;

        CHECK(oarray->strides[2] == 1);
        CHECK(oarray->strides[1] == CUBE_SIZE);
        CHECK(oarray->strides[0] == CUBE_SIZE * CUBE_SIZE);
        memset(oarray->data, 0, CUBE_SIZE * CUBE_SIZE * CUBE_SIZE);

        glBindTexture(GL_TEXTURE_2D, otexture);
        GLint v;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &v);
        CHECK(v == VIEW_SIZE);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &v);
        CHECK(v == VIEW_SIZE);

        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, oarray->data);

        return _oarray;
    }

    object sample_simple (float x, float y, float z, float phi, float theta, float kappa, float scale) {
        return object(boost::python::handle<>(sample(glm::vec3(x, y, z), glm::vec3(phi, theta, kappa), scale)));
    }

    ~Sampler () {
        //check_thread();
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
        glDeleteBuffers(1, &v_pos);
        glDeleteBuffers(1, &v_tex);
        glDeleteTextures(1, &itexture);
        glDeleteTextures(1, &otexture);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteVertexArrays(1, &v_array);
        glDeleteProgram(program);
        glfwTerminate();
    }
};

boost::shared_ptr<Sampler> globalSampler = 0;

boost::shared_ptr<Sampler> get_sampler () {
    if (!globalSampler) {
		globalSampler = boost::shared_ptr<Sampler>(new Sampler());
    }
    return globalSampler;
}


/*
object render (PyObject *array, float z, float y, float x, float scale) {
    //Sampler renderer(64, 512, 512);
    glm::ivec3 off, len, shift;

    globalSampler->texture(array, &off, &len, &shift);
    std::default_random_engine rng;
    glm::vec3 center = off + glm::vec3(x, y, z);
    return renderer.sample(center, scale, rng);
    //return renderer.apply(array);
}
*/


class H265Encoder {
    std::ostream &os;
    x265_encoder *enc;
    x265_picture *pic;
    vector<uint8_t> framebuf;
    int rows, cols;

    void write_nal (x265_nal *p_nal, uint32_t i_nal) {
        for (unsigned i = 0; i < i_nal; ++i) {
            /*
            os.write(reinterpret_cast<char const *>(&p_nal[i].type), sizeof(p_nal[i].type));
            os.write(reinterpret_cast<char const *>(&p_nal[i].sizeBytes), sizeof(p_nal[i].sizeBytes));
            */
            os.write(reinterpret_cast<char const *>(p_nal[i].payload), p_nal[i].sizeBytes);
        }
    }
public:
    H265Encoder (std::ostream &_os, int _rows, int _cols, int frames = 0): os(_os), rows(_rows), cols(_cols) {
        char buf[200];
        int r;
        x265_param *param = x265_param_alloc();
        CHECK(param);
        //r = x265_param_default_preset(param, "medium", "fastdecode");
        r = x265_param_default_preset(param, "medium", "fastdecode");
        CHECK(r == 0);
        r = x265_param_apply_profile(param, "main");
        CHECK(r == 0);
        r = x265_param_parse(param, "lossless", NULL);
        CHECK(r == 0);
        /*
        if (frames > 0) {
            sprintf(buf, "%d", frames);
            r = x265_param_parse(param, "frames", buf);
            CHECK(r == 0);
        }
        */
        CHECK(param->internalBitDepth == 8);
        /*
        r = x265_param_parse(param, "input-depth", "8");
        CHECK(r == 0);
        */
        sprintf(buf, "%dx%d", cols, rows);
        r = x265_param_parse(param, "input-res", buf);
        CHECK(r == 0);
        r = x265_param_parse(param, "input-csp", "i400");
        CHECK(r == 0);
        r = x265_param_parse(param, "fps", "1");
        CHECK(r == 0);
        enc = x265_encoder_open(param);
        CHECK(enc);
        pic = x265_picture_alloc();
        CHECK(pic);
        x265_picture_init(param, pic);
        x265_param_free(param);
        x265_nal *p_nal;
        uint32_t i_nal;
        r = x265_encoder_headers(enc, &p_nal, &i_nal);
        CHECK(r >= 0);
        write_nal(p_nal, i_nal);
    }
    void encode (uint8_t *frame, unsigned stride1 = 0, unsigned stride2 = 0) {
        if (stride2 == 0) stride2 = sizeof(uint8_t);
        if (stride1 == 0) stride1 = cols * stride2;
        if (stride2 == sizeof(uint8_t)) {
            pic->planes[0] = frame;
            pic->stride[0] = stride1;
        }
        else {
            framebuf.resize(rows * cols);
            auto from_y = frame;
            unsigned o = 0;
            for (int y = 0; y < rows; ++y) {
                auto from_x = from_y;
                from_y += stride1;
                for (int x = 0; x < cols; ++x) {
                    framebuf[o++] = *from_x;
                    from_x += stride2;
                }
            }
            pic->planes[0] = &framebuf[0];
            pic->stride[0] = cols * sizeof(uint8_t);
        }
        pic->planes[1] = pic->planes[2] = NULL;
        x265_nal *p_nal;
        uint32_t i_nal;
        int r = x265_encoder_encode(enc, &p_nal, &i_nal, pic, NULL);
        CHECK(r >= 0);
        write_nal(p_nal, i_nal);
        ++pic->pts;
    }
    void flush () {
        for (;;) {
            x265_nal *p_nal;
            uint32_t i_nal;
            int r = x265_encoder_encode(enc, &p_nal, &i_nal, NULL, NULL);
            if (r <= 0) break;
            write_nal(p_nal, i_nal);
        }
    }
    ~H265Encoder () {
        x265_picture_free(pic);
        x265_encoder_close(enc);
    }
};

void h265decode (const_buffer buf, std::function<void(de265_image const *)> callback) {
    de265_decoder_context* ctx = de265_new_decoder();
    CHECK(ctx);
    de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, true);
    de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, false);
    de265_start_worker_threads(ctx, 1);
    de265_error err = de265_push_data(ctx,
            boost::asio::buffer_cast<void const *>(buf),
            boost::asio::buffer_size(buf),
            0, (void *)2);
    CHECK(err == DE265_OK);
    err = de265_flush_data(ctx);
    CHECK(err == DE265_OK);
    int more = 1;
    while (more) {
        err = de265_decode(ctx, &more);
        if (err != DE265_OK) break;
        const de265_image* img = de265_get_next_picture(ctx);
        for (;;) {
            de265_error warning = de265_get_warning(ctx);
            if (warning==DE265_OK) {
            	break;
            }
            fprintf(stderr,"WARNING: %s\n", de265_get_error_text(warning));
        }
        if (!img) continue;
        callback(img);
	}
	de265_free_decoder(ctx);
}

PyArrayObject *h265decode_array (const_buffer buf, Json const &meta) {
    int Z = meta["size"].array_items()[0].int_value();
    int Y = meta["size"].array_items()[1].int_value();
    int X = meta["size"].array_items()[2].int_value();
    npy_intp dims[] = {Z, Y, X};
    // allocate storage
    PyArrayObject *array = (PyArrayObject *)PyArray_SimpleNew(3, &dims[0], NPY_UINT8);
    CHECK(array);
    CHECK(array->strides[0] == X*Y);
    CHECK(array->strides[1] == X);
    CHECK(array->strides[2] == 1);

    uint8_t *to_z = (uint8_t *)array->data;
    int cnt_z = 0;
    h265decode(buf, [&to_z, &cnt_z, Y, X, array](const de265_image* img) {
        int cols = de265_get_image_width(img, 0);
        int rows = de265_get_image_height(img, 0);
        CHECK(rows == Y);
        CHECK(cols == X);
        CHECK(de265_get_chroma_format(img) == de265_chroma_mono);
        CHECK(de265_get_bits_per_pixel(img, 0) == 8);
        int stride;
        const uint8_t* frame = de265_get_image_plane(img, 0, &stride);
        // copy

        uint8_t *to_y = to_z;
        to_z += array->strides[0];
        ++cnt_z;
        for (int i = 0; i < rows; ++i, to_y += array->strides[1], frame += stride) {
            memcpy(to_y, frame, cols * sizeof(uint8_t));
        }
    });
    CHECK(cnt_z == Z);
    return array;
}

PyArrayObject *h265decode_array_sampled (const_buffer buf, Json const &meta, unsigned off, unsigned step) {
    int Z = meta["size"].array_items()[0].int_value();
    int Y = meta["size"].array_items()[1].int_value();
    int X = meta["size"].array_items()[2].int_value();
    int SZ = 0;
    for (int i = off; i < Z; i += step) {
        ++SZ;   // count output size
    }
    if (SZ == 0) return 0;
    
    npy_intp dims[] = {SZ, Y, X};
    // allocate storage
    PyArrayObject *array = (PyArrayObject *)PyArray_SimpleNew(3, &dims[0], NPY_UINT8);
    CHECK(array);
    CHECK(array->strides[0] == X*Y);
    CHECK(array->strides[1] == X);
    CHECK(array->strides[2] == 1);

    uint8_t *to_z = (uint8_t *)array->data;
    int z = 0;
    int next = off;
    h265decode(buf, [&to_z, &z, &next, &step, Y, X, array](const de265_image* img) {
        if (z < next) {
            z++;
            return;
        }
        next += step;
        z++;

        int cols = de265_get_image_width(img, 0);
        int rows = de265_get_image_height(img, 0);
        CHECK(rows == Y);
        CHECK(cols == X);
        CHECK(de265_get_chroma_format(img) == de265_chroma_mono);
        CHECK(de265_get_bits_per_pixel(img, 0) == 8);
        int stride;
        const uint8_t* frame = de265_get_image_plane(img, 0, &stride);
        // copy

        uint8_t *to_y = to_z;
        to_z += array->strides[0];
        for (int i = 0; i < rows; ++i, to_y += array->strides[1], frame += stride) {
            memcpy(to_y, frame, cols * sizeof(uint8_t));
        }
    });
    CHECK(to_z == (uint8_t *)array->data + array->strides[0] * SZ);
    return array;
}

string encode (PyObject *_array) {
    ostringstream ss;
    PyArrayObject *array((PyArrayObject *)_array);
    CHECK(array->nd == 3) << "not 3d array: " << array->nd;
    CHECK(array->descr->type_num == NPY_UINT8) << "not uint8 array";
    H265Encoder enc(ss, array->dimensions[1],
                        array->dimensions[2],
                        array->dimensions[0]);
    auto from_z = reinterpret_cast<uint8_t *>(array->data);
    for (unsigned z = 0; z < array->dimensions[0]; ++z) {
        enc.encode(from_z, array->strides[1], array->strides[2]);
        from_z += array->strides[0];
    }
    enc.flush();
    return ss.str();
}

/*
object mats2npy (vector<cv::Mat> const &images) {
    int rows = images[0].rows;
    int cols = images[0].cols;
    npy_intp images_dims[] = {images.size(), rows, cols};
    PyObject *_array = PyArray_SimpleNew(3, &images_dims[0], NPY_UINT8);
    PyArrayObject *array = (PyArrayObject*)_array;
    CHECK(array->strides[2] == 1);
    auto from_z = reinterpret_cast<uint8_t *>(array->data);
    for (unsigned z = 0; z < images.size(); ++z) {
        CHECK(images[z].isContinuous());
        auto from_y = from_z;
        from_z += array->strides[0];
        for (unsigned y = 0; y < rows; ++y) {
            memcpy(from_y, images[z].ptr<uint8_t const>(y), cols * sizeof(uint8_t));
            from_y += array->strides[1];
        }
    }
    return object(boost::python::handle<>(_array));
}
*/

object decode (string const &v, int Z, int Y, int X) {
    Json meta = Json::object{
        {"size", Json::array{Z, Y, X}}
    };
    return object(handle<>((PyObject *)h265decode_array(const_buffer(&v[0], v.size()), meta)));
}

namespace picpac {

#define PICPAC_VOLUME_CONFIG_UPDATE_ALL(C) \
    PICPAC_CONFIG_UPDATE(C,seed);\
    PICPAC_CONFIG_UPDATE(C,loop);\
    PICPAC_CONFIG_UPDATE(C,shuffle);\
    PICPAC_CONFIG_UPDATE(C,reshuffle);\
    PICPAC_CONFIG_UPDATE(C,stratify);\
    PICPAC_CONFIG_UPDATE(C,split);\
    PICPAC_CONFIG_UPDATE(C,split_fold);\
    PICPAC_CONFIG_UPDATE(C,split_negate);\
    PICPAC_CONFIG_UPDATE(C,mixin);\
    PICPAC_CONFIG_UPDATE(C,mixin_group_delta);\
    PICPAC_CONFIG_UPDATE(C,mixin_max);\
    PICPAC_CONFIG_UPDATE(C,cache);\
    PICPAC_CONFIG_UPDATE(C,preload);\
    PICPAC_CONFIG_UPDATE(C,threads);\
    PICPAC_CONFIG_UPDATE(C,channels);\
    PICPAC_CONFIG_UPDATE(C,min_size);\
    PICPAC_CONFIG_UPDATE(C,max_size);\
    PICPAC_CONFIG_UPDATE(C,resize_width);\
    PICPAC_CONFIG_UPDATE(C,resize_height);\
    PICPAC_CONFIG_UPDATE(C,crop_width);\
    PICPAC_CONFIG_UPDATE(C,crop_height);\
    PICPAC_CONFIG_UPDATE(C,round_div);\
    PICPAC_CONFIG_UPDATE(C,round_mod);\
    PICPAC_CONFIG_UPDATE(C,decode_mode);\
    PICPAC_CONFIG_UPDATE(C,annotate);\
    PICPAC_CONFIG_UPDATE(C,anno_type);\
    PICPAC_CONFIG_UPDATE(C,anno_copy);\
    PICPAC_CONFIG_UPDATE(C,anno_palette);\
    PICPAC_CONFIG_UPDATE(C,anno_color1); \
    PICPAC_CONFIG_UPDATE(C,anno_color2); \
    PICPAC_CONFIG_UPDATE(C,anno_color3); \
    PICPAC_CONFIG_UPDATE(C,anno_thickness);\
    PICPAC_CONFIG_UPDATE(C,anno_min_ratio); \
    PICPAC_CONFIG_UPDATE(C,perturb);\
    PICPAC_CONFIG_UPDATE(C,pert_colorspace); \
    PICPAC_CONFIG_UPDATE(C,pert_color1); \
    PICPAC_CONFIG_UPDATE(C,pert_color2); \
    PICPAC_CONFIG_UPDATE(C,pert_color3); \
    PICPAC_CONFIG_UPDATE(C,pert_angle); \
    PICPAC_CONFIG_UPDATE(C,pert_min_scale); \
    PICPAC_CONFIG_UPDATE(C,pert_max_scale); \
    PICPAC_CONFIG_UPDATE(C,pert_hflip); \
    PICPAC_CONFIG_UPDATE(C,pert_vflip); 

    std::mutex global_lock;

    struct Cube {
        PyObject *images;
        PyObject *labels;
    };

    vector<Cube> global_cube_pool;

    class CubeLoader: public ImageLoader {
    public:
        // we use pert_color2 & pert_color3 to sample rotate axis
        //
        struct Config: public ImageLoader::Config {
            unsigned samples0;
            unsigned samples1;
            unsigned pool;
            unsigned factor;
            Config (): samples0(4), samples1(4), pool(128), factor(1) {
            }
        } config;

        typedef Cube Value;

        CubeLoader (Config const &config_): ImageLoader(config_), config(config_) {
        }

        struct Nodule {
            glm::vec3 pos;
            float radius;
        };

        struct Sample {
            glm::vec3 pos;
            glm::vec3 angle;
            float scale;
            Sample () {}
            Sample (float x, float y, float z,
                    float phi, float theta, float kappa,
                    float s): pos(x, y, z), angle(phi, theta, kappa), scale(s) {
            }
        };

        static float l2norm (glm::vec3 const &p) {
            return std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        }

        PyObject *generate_labels (glm::vec3 center, glm::vec3 rotate, float scale0, vector<Nodule> const &from_nodules) const { //std::default_random_engine &rng) {

            CHECK_POWER_OF_TWO(config.factor);
            // 1. convert nodules to cube location
            glm::mat4 unrotate = glm::inverse(
                                    glm::rotate(float(rotate[2] * 180/M_PI),   // glm requires digrees
                                    glm::vec3(
                                        sin(rotate[0]) * cos(rotate[1]),
                                        sin(rotate[0]) * sin(rotate[1]),
                                        cos(rotate[0]))));

            int cs = Sampler::CUBE_SIZE/config.factor;
            float cs2 = cs/2.0;
            vector<Nodule> nodules;
            scale0 *= config.factor;
            glm::vec3 cc(cs2, cs2, cs2);
            static constexpr float SQRT3 = 1.7320508075688772;
            float box_radius = cs2 * SQRT3;

            for (auto const &nod: from_nodules) {
                Nodule nnod;
                nnod.pos = glm::vec3(unrotate*glm::vec4(nod.pos - center, 1))/scale0 + cs2;
                nnod.radius = nod.radius/scale0;

                float dist = l2norm(cc - nnod.pos);
                if (dist < nnod.radius + box_radius) {
                    nodules.push_back(nnod);
                }
            }

            if (nodules.empty()) return 0;
            npy_intp dims[] = {cs, cs, cs};
            PyObject *_array = PyArray_ZEROS(3, &dims[0], NPY_UINT8, 0);
            PyArrayObject *array = (PyArrayObject*)_array;
            CHECK(array->flags & NPY_ARRAY_C_CONTIGUOUS);
            for (auto const &nod: nodules) {
                float x = nod.pos[0];
                float y = nod.pos[1];
                float z = nod.pos[2];
                float r = nod.radius;
                int lb = int(floor(z-r));
                int ub = int(ceil(z+r));
                for (int i = lb; i <= ub; ++i) {
                    if (i < 0) continue;
                    if (i >= cs) continue;
                    float dz = i - z;
                    int r0 = round(sqrt(r * r - dz * dz));
                    if (r0 < 2) continue;
                    cv::Mat image(cs, cs, CV_8U, array->data + i * array->strides[0]);
                    cv::circle(image, cv::Point(int(round(x)), int(round(y))), r0, cv::Scalar(1), -1);
                }
            }
            return _array;
        }


        void load (RecordReader rr, PerturbVector const &p, Value *out,
            CacheValue *c, std::mutex *m) const {
            lock_guard lock(global_lock);
            if (global_cube_pool.size() < config.pool) {
                Record r;
                rr(&r);
                CHECK(r.size() > 1);
                string err;
                Json json = Json::parse(r.field_string(1), err);

                PyArrayObject *array = h265decode_array(r.field(0), json);
                CHECK(array);

                glm::ivec3 off, len, shift;
                get_sampler();
                globalSampler->texture(array, &off, &len, &shift);

                // nodules
                vector<Nodule> nodules;
                Json const &json_nodules = json["anno"];
                if (json_nodules.is_array()) {
                    for (auto const &nod: json_nodules.array_items()) {
                        vector<float> v;
                        for (auto const &x: nod.array_items()) {
                            v.push_back(x.number_value());
                        }
                        CHECK(v.size() == 4);
                        nodules.emplace_back();
                        nodules.back().pos = glm::vec3(v[2]-shift[0], v[1]-shift[1], v[0]-shift[2]);
                        nodules.back().radius = v[3];
                    }
                }
                // copy to ...
                // sample
                std::default_random_engine rng(p.shiftx);
                std::uniform_real_distribution<float> delta_color(-config.pert_color1, config.pert_color1);
                std::uniform_real_distribution<float> phi(-M_PI/2, M_PI/2);
                std::uniform_real_distribution<float> theta(-M_PI, M_PI);
                std::uniform_real_distribution<float> kappa(-M_PI, M_PI);
                std::uniform_real_distribution<float> linear_scale(config.pert_min_scale, config.pert_max_scale);

                vector<Sample> samples;

                int constexpr cs = Sampler::CUBE_SIZE;
                int constexpr cs2 = Sampler::CUBE_SIZE/2;
                if (config.perturb) {
                    for (unsigned i = 0; i < config.samples0; ++i) {
                        samples.emplace_back(
                                    off[0] + rng() % (len[0] - cs) + cs2,
                                    off[1] + rng() % (len[1] - cs) + cs2,
                                    off[2] + rng() % (len[2] - cs) + cs2,
                                    phi(rng), theta(rng), kappa(rng), linear_scale(rng)
                                );
                    }
                    vector<int> idx;    // nodule indexes to use
                    for (unsigned i = 0; i < nodules.size(); ++i) {
                        idx.push_back(i);
                    }
                    std::shuffle(idx.begin(), idx.end(), rng);
                    if (idx.size() > config.samples1) {
                        idx.resize(config.samples1);
                    }
                    else if (idx.size() > 0) {
                        // over sample
                        unsigned i = 0;
                        while (idx.size() < config.samples1) {
                            idx.push_back(idx[i++]);
                        }
                    }
                    for (int i: idx) {
                        // sample from nodules
                        auto const &nodule = nodules[i];
                        std::uniform_real_distribution<float> shift2(-nodule.radius, nodule.radius);
                        samples.emplace_back(
                                    nodule.pos[0] + shift2(rng),
                                    nodule.pos[1] + shift2(rng),
                                    nodule.pos[2] + shift2(rng),
                                    phi(rng), theta(rng), kappa(rng), linear_scale(rng)
                                );
                    }
                    std::shuffle(samples.begin(), samples.end(), rng);
                }
                else {
                    if (config.samples0 > 0) {
                        // full image
                        int constexpr vs2 = Sampler::VOLUME_SIZE/2;
                        samples.emplace_back(
                                    vs2, vs2, vs2,
                                    0, 0, 0, Sampler::VOLUME_SIZE / Sampler::CUBE_SIZE);
                    }
                    if (config.samples1 > 0) {
                        // all nodules
                        for (auto const &nodule: nodules) {
                            // sample from nodules
                            samples.emplace_back(
                                        nodule.pos[0],
                                        nodule.pos[1],
                                        nodule.pos[2],
                                        0, 0, 0, 1.0);
                        }
                    }
                }
                CHECK(samples.size());
                for (auto const &s: samples) {
                    Cube cube;
                    cube.images = globalSampler->sample(s.pos, s.angle, s.scale);
                    if (config.perturb) {
                        PyArrayObject *array = (PyArrayObject *)cube.images;
                        CHECK(array->flags & NPY_ARRAY_C_CONTIGUOUS);
                        cv::Mat mat(Sampler::VIEW_SIZE, Sampler::VIEW_SIZE, CV_8U, array->data);
                        mat += delta_color(rng);
                    }
                    cube.labels = generate_labels(s.pos, s.angle, s.scale, nodules);
                    global_cube_pool.push_back(cube);
                }
                Py_CLEAR(array);
                std::shuffle(global_cube_pool.begin(), global_cube_pool.end(), rng);
            }
            CHECK(global_cube_pool.size());
            *out = global_cube_pool.back();
            global_cube_pool.pop_back();
        }
    };

    class CubeStream: public PrefetchStream<CubeLoader> {
    public:
        CubeStream (std::string const &path, Config const &config) : PrefetchStream(fs::path(path), config) {
            global_cube_pool.clear();
        }

        tuple next () {
            Cube cube = PrefetchStream<CubeLoader>::next();
            if (cube.labels) {
                return make_tuple(object(boost::python::handle<>(cube.images)),
                              object(boost::python::handle<>(cube.labels)));
            }
            else {
                return make_tuple(object(boost::python::handle<>(cube.images)),
                              object());
            }
        }
    };

    object create_cube_stream (tuple args, dict kwargs) {
        object self = args[0];
        CHECK(len(args) > 1);
        string path = extract<string>(args[1]);
        CubeStream::Config config;
#define PICPAC_CONFIG_UPDATE(C, P) \
        C.P = extract<decltype(C.P)>(kwargs.get(#P, C.P))
        PICPAC_VOLUME_CONFIG_UPDATE_ALL(config);
        PICPAC_CONFIG_UPDATE(config,samples0); 
        PICPAC_CONFIG_UPDATE(config,samples1); 
        PICPAC_CONFIG_UPDATE(config,pool);
        PICPAC_CONFIG_UPDATE(config,factor);
#undef PICPAC_CONFIG_UPDATE
        CHECK(!config.cache) << "Cube stream should net be cached";
        if (!config.perturb) {
            LOG(WARNING) << "perturb == FALSE: for testing only, don't use for training.";
            LOG(WARNING) << "NODULES are in original resolution";
            LOG(WARNING) << "FULL IMAGES are of 1/8 resolution (512 to 64)";
        }
        CHECK(config.threads == 1) << "Cube stream only supports 1 threads.";
        CHECK(config.channels == 1) << "Cube stream only supports 1 channels.";
        CHECK(config.pool > 0);
        LOG(WARNING) << "preload: " << config.preload;
        return self.attr("__init__")(path, config);
    };

    class VolumeLoader: public ImageLoader {
        static void dummy_record_reader (Record *) {
            CHECK(false) << "Dummy record reader should never be invoked.";
        }
    public:
        struct Config: public ImageLoader::Config {
            int stride;
            Config (): stride(3) {
            }
        } config;
        typedef object Value;
        VolumeLoader (Config const &config_): ImageLoader(config_), config(config_) {
        }
        void load (RecordReader rr, PerturbVector const &p, Value *out,
            CacheValue *c = nullptr, std::mutex *m = nullptr) const {
            Record r;
            rr(&r);
            CHECK(r.size() > 1);
            string err;
            Json json = Json::parse(r.field_string(1), err);

            unsigned off = 0;
            if (config.perturb) off = p.shiftx % config.stride;

            std::mutex dummy_mutex;
            CHECK(config.stride > 0);
            PyArrayObject *array = h265decode_array_sampled(r.field(0), json, off, config.stride);
            CHECK(array);
            CHECK(array->flags & NPY_ARRAY_C_CONTIGUOUS);
            if (config.perturb) {
                ImageLoader::CacheValue cache;
                ImageLoader::Value loaded;
                cache.label = 0;
                cache.annotation = cv::Mat();
                uint8_t *z = (uint8_t *)array->data;
                cache.image = cv::Mat(array->dimensions[1], array->dimensions[2], CV_8U, (void *)z);
                ImageLoader::load(dummy_record_reader,p, &loaded, &cache, &dummy_mutex);
                CHECK(loaded.image.rows > 0);
                CHECK(loaded.image.type() == CV_8U);

                npy_intp dims[] = {array->dimensions[0], loaded.image.rows, loaded.image.cols};
                // allocate storage
                PyArrayObject *oarray = (PyArrayObject *)PyArray_SimpleNew(3, &dims[0], NPY_UINT8);
                CHECK(oarray);
                CHECK(oarray->flags & NPY_ARRAY_C_CONTIGUOUS);
                uint8_t *to_z = (uint8_t *)oarray->data;
                int total = loaded.image.rows * loaded.image.cols;
                uint8_t const *xxx = loaded.image.ptr<uint8_t const>(0);
                std::copy(xxx, xxx + total, to_z);
                for (int i = 1; i < array->dimensions[0]; ++i) {
                    z += array->strides[0];
                    to_z += total;
                    cache.image = cv::Mat(array->dimensions[1], array->dimensions[2], CV_8U, (void *)z);
                    ImageLoader::load(dummy_record_reader,p, &loaded, &cache, &dummy_mutex);
                    CHECK(loaded.image.rows == dims[1]);
                    CHECK(loaded.image.cols == dims[2]);
                    CHECK(loaded.image.type() == CV_8U);
                    xxx = loaded.image.ptr<uint8_t const>(0);
                    std::copy(xxx, xxx + total, to_z);
                }
                Py_CLEAR(array);
                array = oarray;
            }
            *out = object(boost::python::handle<>((PyObject *)array));
        }
    };

    class VolumeStream: public PrefetchStream<VolumeLoader> {
    public:
        VolumeStream (std::string const &path, Config const &config)
            : PrefetchStream(fs::path(path), config) {
        }
        object next () {
            return PrefetchStream<VolumeLoader>::next();
        }
    };

    object create_volume_stream (tuple args, dict kwargs) {
        object self = args[0];
        CHECK(len(args) > 1);
        string path = extract<string>(args[1]);
        VolumeStream::Config config;
#define PICPAC_CONFIG_UPDATE(C, P) \
        C.P = extract<decltype(C.P)>(kwargs.get(#P, C.P))
        PICPAC_VOLUME_CONFIG_UPDATE_ALL(config);
        PICPAC_CONFIG_UPDATE(config, stride); 
#undef PICPAC_CONFIG_UPDATE
        config.cache = false;
        config.channels = 1;
        return self.attr("__init__")(path, config);
    };

    static object return_iterator (tuple args, dict kwargs) {
        object self = args[0];
        self.attr("reset")();
        return self;
    };
}

using namespace picpac;

struct CubicLibGuard {
    CubicLibGuard() {
		LOG(INFO) << "Initializing PicPac3D library";
        import_array();
		//CHECK(globalSampler);
	}
    ~CubicLibGuard() {
		LOG(INFO) << "Cleaning PicPac3D library";
        globalSampler = 0;
		//delete globalSampler;
        for (auto &cube: global_cube_pool) {
            Py_CLEAR(cube.images);
            Py_CLEAR(cube.labels);
        }
        global_cube_pool.clear();
        x265_cleanup();
	}
};

BOOST_PYTHON_MODULE(picpac3d)
{

	class_<CubicLibGuard, boost::shared_ptr<CubicLibGuard>, boost::noncopyable>("CubicLibGuard", no_init);
	boost::shared_ptr<CubicLibGuard> picpac3dLibGuard(new CubicLibGuard());
    scope().attr("__libguard") = picpac3dLibGuard;

	class_<Sampler, boost::shared_ptr<Sampler>, boost::noncopyable>("Sampler", no_init)
        .def("texture_direct", &Sampler::texture_direct)
        .def("texture_indirect", &Sampler::texture_indirect)
        .def("sample", &Sampler::sample_simple)
        ;

    numeric::array::set_module_and_type("numpy", "ndarray");
    def("encode", ::encode);
    def("decode", ::decode);
    def("sampler", ::get_sampler);
    //def("render", ::render);
    class_<CubeStream::Config>("CubeStreamParams", init<>());
    class_<VolumeStream::Config>("VolumeStreamParams", init<>());
    class_<CubeStream, boost::noncopyable>("CubeStream", no_init)
        .def("__init__", raw_function(create_cube_stream), "exposed ctor")
        .def("__iter__", raw_function(return_iterator))
        .def(init<string, CubeStream::Config const&>()) // C++ constructor not exposed
        .def("next", &CubeStream::next)
        .def("size", &CubeStream::size)
        .def("reset", &CubeStream::reset)
        ;
    class_<VolumeStream, boost::noncopyable>("VolumeStream", no_init)
        .def("__init__", raw_function(create_volume_stream), "exposed ctor")
        .def("__iter__", raw_function(return_iterator))
        .def(init<string, VolumeStream::Config const&>()) // C++ constructor not exposed
        .def("next", &VolumeStream::next)
        .def("size", &VolumeStream::size)
        .def("reset", &VolumeStream::reset)
        ;
}

