
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <math.h>

#include <X11/Xlib.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "drm/drm_fourcc.h"

#include "socket.h"
#include "window.h"
#include "render.h"

void parse_arguments(int argc, char **argv, int *is_server);
int* create_data(size_t size);
void rotate_data(int* data, size_t size);

#define CASE_STR( value ) case value: return #value; 
const char* eglGetErrorString( EGLint error )
{
    switch( error )
    {
    CASE_STR( EGL_SUCCESS             )
    CASE_STR( EGL_NOT_INITIALIZED     )
    CASE_STR( EGL_BAD_ACCESS          )
    CASE_STR( EGL_BAD_ALLOC           )
    CASE_STR( EGL_BAD_ATTRIBUTE       )
    CASE_STR( EGL_BAD_CONTEXT         )
    CASE_STR( EGL_BAD_CONFIG          )
    CASE_STR( EGL_BAD_CURRENT_SURFACE )
    CASE_STR( EGL_BAD_DISPLAY         )
    CASE_STR( EGL_BAD_SURFACE         )
    CASE_STR( EGL_BAD_MATCH           )
    CASE_STR( EGL_BAD_PARAMETER       )
    CASE_STR( EGL_BAD_NATIVE_PIXMAP   )
    CASE_STR( EGL_BAD_NATIVE_WINDOW   )
    CASE_STR( EGL_CONTEXT_LOST        )
    default: return "Unknown";
    }
}
#undef CASE_STR

int main(int argc, char **argv)
{
    // Parse arguments
    int is_server;
    parse_arguments(argc, argv, &is_server);

    // Create X11 window
    Display *x11_display;
    Window x11_window;
    create_x11_window(is_server, &x11_display, &x11_window);

    // Initialize EGL
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    initialize_egl(x11_display, x11_window, &egl_display, &egl_context, &egl_surface);

	assert(eglGetCurrentContext());

	printf("GL_VERSION=%s\n", glGetString(GL_VERSION));

    // Setup GL scene
    gl_setup_scene();


    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT =
            (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");

    
	EGLint formats[128];
    EGLint num_formats = 0;
    
	EGLBoolean ok = eglQueryDmaBufFormatsEXT(egl_display, 128, formats, &num_formats);
    assert(ok);
	printf("eglQueryDmaBufFormatsEXT: ");
    for (int i = 0; i < num_formats; ++i)
		printf("%.4s ", (char*)&formats[i]);
	printf("\n");
                            
    // Server texture data
    const size_t TEXTURE_DATA_WIDTH = 256;
    const size_t TEXTURE_DATA_HEIGHT = TEXTURE_DATA_WIDTH;
    const size_t TEXTURE_DATA_SIZE = TEXTURE_DATA_WIDTH * TEXTURE_DATA_HEIGHT;
	int* texture_data = create_data(TEXTURE_DATA_SIZE);

    // -----------------------------
    // --- Texture sharing start ---
    // -----------------------------

    // Socket paths for sending/receiving file descriptor and image storage data
    const char *SERVER_FILE = "/tmp/test_server";
    const char *CLIENT_FILE = "/tmp/test_client";
    // Custom image storage data description to transfer over socket
    struct texture_storage_metadata_t
    {
        int fourcc;
        EGLuint64KHR modifiers;
        EGLint stride;
        EGLint offset;
    };

    // GL texture that will be shared
    GLuint texture;

    // The next `if` block contains server code in the `true` branch and client code in the `false` branch. The `true` branch is always executed first and the `false` branch after it (in a different process). This is because the server loops at the end of the branch until it can send a message to the client and the client blocks at the start of the branch until it has a message to read. This way the whole `if` block from top to bottom represents the order of events as they happen.
    if (is_server)
    {
        // GL: Create and populate the texture
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEXTURE_DATA_WIDTH, TEXTURE_DATA_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEXTURE_DATA_WIDTH, TEXTURE_DATA_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, texture_data);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // EGL: Create EGL image from the GL texture
        EGLImage image = eglCreateImage(egl_display,
                                        egl_context,
                                        EGL_GL_TEXTURE_2D,
                                        (EGLClientBuffer)(uint64_t)texture,
                                        NULL);
        assert(image != EGL_NO_IMAGE);
		printf("eglCreateImage: %s image=%p\n", eglGetErrorString(eglGetError()));

        // The next line works around an issue in radeonsi driver (fixed in master at the time of writing). If you are
        // having problems with texture rendering until the first texture update you can uncomment this line
        // glFlush();

        // EGL (extension: EGL_MESA_image_dma_buf_export): Get file descriptor (texture_dmabuf_fd) for the EGL image and get its
        // storage data (texture_storage_metadata)
		int texture_dmabuf_fd = -1;
		struct texture_storage_metadata_t texture_storage_metadata = { 'BAD ', 0xbadf00d, -2, -3 };

        int num_planes;
        PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA =
            (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
        EGLBoolean queried = eglExportDMABUFImageQueryMESA(egl_display,
                                                           image,
                                                           &texture_storage_metadata.fourcc,
                                                           &num_planes,
														   &texture_storage_metadata.modifiers);
        

        assert(queried);
        assert(num_planes == 1);
        PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA =
            (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");


		printf("eglExportDMABUFImageQueryMESA: %s fourcc:%.4s planes:%d modifiers:0x%lx\n",
			eglGetErrorString(eglGetError()),
			(char*)&texture_storage_metadata.fourcc,
			num_planes,
			texture_storage_metadata.modifiers);

		assert(texture_storage_metadata.fourcc == DRM_FORMAT_XBGR8888);
		//assert(texture_storage_metadata.fourcc == DRM_FORMAT_ABGR8888);

        EGLBoolean exported = eglExportDMABUFImageMESA(egl_display,
                                                       image,
                                                       &texture_dmabuf_fd,
                                                       &texture_storage_metadata.stride,
                                                       &texture_storage_metadata.offset);

		printf("eglExportDMABUFImageMESA: %s, ok=%d fd=%d, stride:%d offset:%d\n",
			eglGetErrorString(eglGetError()),
			exported,
			texture_dmabuf_fd,
			texture_storage_metadata.stride,
			texture_storage_metadata.offset);

        assert(exported);

        // Unix Domain Socket: Send file descriptor (texture_dmabuf_fd) and texture storage data (texture_storage_metadata)
        int sock = create_socket(SERVER_FILE);
        while (connect_socket(sock, CLIENT_FILE) != 0)
            ;
        write_fd(sock, texture_dmabuf_fd, &texture_storage_metadata, sizeof(texture_storage_metadata));
        close(sock);
        close(texture_dmabuf_fd);
    }
    else
    {
        // Unix Domain Socket: Receive file descriptor (texture_dmabuf_fd) and texture storage data (texture_storage_metadata)
        int texture_dmabuf_fd;
        struct texture_storage_metadata_t texture_storage_metadata;

        int sock = create_socket(CLIENT_FILE);
        read_fd(sock, &texture_dmabuf_fd, &texture_storage_metadata, sizeof(texture_storage_metadata));
        close(sock);

        // EGL (extension: EGL_EXT_image_dma_buf_import): Create EGL image from file descriptor (texture_dmabuf_fd) and storage
        // data (texture_storage_metadata)
        EGLAttrib const attribute_list[] = {
            EGL_WIDTH, TEXTURE_DATA_WIDTH,
            EGL_HEIGHT, TEXTURE_DATA_HEIGHT,
            EGL_LINUX_DRM_FOURCC_EXT, texture_storage_metadata.fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT, texture_dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, texture_storage_metadata.offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, texture_storage_metadata.stride,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (uint32_t)(texture_storage_metadata.modifiers & ((((uint64_t)1) << 33) - 1)),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (uint32_t)((texture_storage_metadata.modifiers>>32) & ((((uint64_t)1) << 33) - 1)),
            EGL_NONE};
        EGLImage image = eglCreateImage(egl_display,
                                        NULL,
                                        EGL_LINUX_DMA_BUF_EXT,
                                        (EGLClientBuffer)NULL,
                                        attribute_list);
        assert(image != EGL_NO_IMAGE);
        close(texture_dmabuf_fd);

        // GLES (extension: GL_OES_EGL_image_external): Create GL texture from EGL image
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // -----------------------------
    // --- Texture sharing end ---
    // -----------------------------

    time_t last_time = time(NULL);
    while (1)
    {
        // Draw scene (uses shared texture)
        gl_draw_scene(texture);
        eglSwapBuffers(egl_display, egl_surface);

        // Update texture data each second to see that the client didn't just copy the texture and is indeed referencing
        // the same texture data.
        if (is_server)
        {
            time_t cur_time = time(NULL);
            if (last_time < cur_time)
            {
                last_time = cur_time;
                rotate_data(texture_data, TEXTURE_DATA_SIZE);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEXTURE_DATA_WIDTH, TEXTURE_DATA_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, texture_data);
            }
        }

        // Check for errors
        assert(glGetError() == GL_NO_ERROR);
        assert(eglGetError() == EGL_SUCCESS);
    }

    return 0;
}

void help()
{
    printf("USAGE:\n"
           "    dmabufshare server\n"
           "    dmabufshare client\n");
}

void parse_arguments(int argc, char **argv, int *is_server)
{
    if (2 == argc)
    {
        if (strcmp(argv[1], "server") == 0)
        {
            *is_server = 1;
        }
        else if (strcmp(argv[1], "client") == 0)
        {
            *is_server = 0;
        }
        else if (strcmp(argv[1], "--help") == 0)
        {
            help();
            exit(0);
        }
        else
        {
            help();
            exit(-1);
        }
    }
    else
    {
        help();
        exit(-1);
    }
}

int* create_data(size_t size)
{
    size_t edge = sqrt(size);
    assert(edge * edge == size);
    size_t half_edge = edge / 2;

    assert(sizeof(int)==4);
	int* data = malloc(size * sizeof(int));

    // Paint the texture like so:
    // RG
    // BW
    // where R - red, G - green, B - blue, W - white
    int red = 0x000000FF;
    int green = 0x0000FF00;
    int blue = 0X00FF0000;
    int white = 0x00FFFFFF;
    for (size_t i = 0; i < size; i++) {
        size_t x = i % edge;
        size_t y = i / edge;

        if (x < half_edge) {
            if (y < half_edge) {
                data[i] = red;
            } else {
                data[i] = blue;
            }
        } else {
            if (y < half_edge) {
                data[i] = green;
            } else {
                data[i] = white;
            }
        }
    }

    return data;
}

void rotate_data(int* data, size_t size)
{
    size_t edge = sqrt(size);
    assert(edge * edge == size);
    size_t half_edge = edge / 2;

    for (size_t i = 0; i < half_edge * half_edge; i++) {
        size_t x = i % half_edge;
        size_t y = i / half_edge;

        int temp = data[x + y * edge];
        data[x + y * edge] = data[(x + half_edge) + y * edge];
        data[(x + half_edge) + y * edge] = data[(x + half_edge) + (y + half_edge) * edge];
        data[(x + half_edge) + (y + half_edge) * edge] = data[x + (y + half_edge) * edge];
        data[x + (y + half_edge) * edge] = temp;
    }
}
