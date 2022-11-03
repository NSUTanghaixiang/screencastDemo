#include "PipewireStream.h"
#include <QDebug>

#include <QDateTime>
#include <cstring>
#include <sys/mman.h>




#if HAVE_DMA_BUF
const char * formatGLError(GLenum err)
{
    switch(err) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_STACK_OVERFLOW:
        return "GL_STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW:
        return "GL_STACK_UNDERFLOW";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return (QLatin1String("0x") + QString::number(err, 16)).toLocal8Bit().constData();
    }
}
#endif /* HAVE_DMA_BUF */


static const int BYTES_PER_PIXEL = 4;
static const uint MIN_SUPPORTED_XDP_KDE_SC_VERSION = 1;

PipewireStream::PipewireStream(QObject *parent)
    : QObject(parent)
{
    pwCoreEvents.version = PW_VERSION_CORE_EVENTS;
    pwCoreEvents.error = &onCoreError;

    pwStreamEvents.version = PW_VERSION_STREAM_EVENTS;
    pwStreamEvents.state_changed = &onStreamStateChanged;
    pwStreamEvents.param_changed = &onStreamParamChanged;
    pwStreamEvents.process = &onStreamProcess;

#if HAVE_DMA_BUF
    m_drmFd = open("/dev/dri/renderD128", O_RDWR);

    if (m_drmFd < 0) {
        qWarning() << "Failed to open drm render node: " << strerror(errno);
        return;
    }

    m_gbmDevice = gbm_create_device(m_drmFd);

    if (!m_gbmDevice) {
        qWarning() << "Cannot create GBM device: " << strerror(errno);
        return;
    }

    // Get the list of client extensions
    const char* clientExtensionsCString = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const QByteArray clientExtensionsString = QByteArray::fromRawData(clientExtensionsCString, qstrlen(clientExtensionsCString));
    if (clientExtensionsString.isEmpty()) {
        // If eglQueryString() returned NULL, the implementation doesn't support
        // EGL_EXT_client_extensions. Expect an EGL_BAD_DISPLAY error.
        qWarning() << "No client extensions defined! " << formatGLError(eglGetError());
        return;
    }

    m_egl.extensions = clientExtensionsString.split(' ');

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (!m_egl.extensions.contains(QByteArrayLiteral("EGL_EXT_platform_base")) ||
            !m_egl.extensions.contains(QByteArrayLiteral("EGL_MESA_platform_gbm"))) {
        qWarning() << "One of required EGL extensions is missing";
        return;
    }

    m_egl.display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, m_gbmDevice, nullptr);

    if (m_egl.display == EGL_NO_DISPLAY) {
        qWarning() << "Error during obtaining EGL display: " << formatGLError(eglGetError());
        return;
    }

    EGLint major, minor;
    if (eglInitialize(m_egl.display, &major, &minor) == EGL_FALSE) {
        qWarning() << "Error during eglInitialize: " << formatGLError(eglGetError());
        return;
    }

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        qWarning() << "bind OpenGL API failed";
        return;
    }

    m_egl.context = eglCreateContext(m_egl.display, nullptr, EGL_NO_CONTEXT, nullptr);

    if (m_egl.context == EGL_NO_CONTEXT) {
        qWarning() << "Couldn't create EGL context: " << formatGLError(eglGetError());
        return;
    }

    qDebug() << "Egl initialization succeeded";
    qDebug() << QStringLiteral("EGL version: %1.%2").arg(major).arg(minor);

    m_eglInitialized = true;
#endif /* HAVE_DMA_BUF */
}

PipewireStream::~PipewireStream()
{
    if (pwMainLoop) {
        pw_thread_loop_stop(pwMainLoop);
    }

    if (pwStream) {
        pw_stream_destroy(pwStream);
    }

    if (pwCore) {
        pw_core_disconnect(pwCore);
    }

    if (pwContext) {
        pw_context_destroy(pwContext);
    }

    if (pwMainLoop) {
        pw_thread_loop_destroy(pwMainLoop);
    }
}

void PipewireStream::onCoreError(void *data, uint32_t id, int seq, int res, const char *message)
{
    qWarning() << "onCoreError";
}

void PipewireStream::onStreamParamChanged(void *data, uint32_t id, const spa_pod *format)
{
    qWarning() << "Stream format changed";
    auto d = static_cast<PipewireStream *>(data);

    if (!format || id != SPA_PARAM_Format) {
        return;
    }

    d->videoFormat = new spa_video_info_raw();
    spa_format_video_raw_parse(format, d->videoFormat);
    auto width = d->videoFormat->size.width;
    auto height = d->videoFormat->size.height;
    auto stride = SPA_ROUND_UP_N(width * BYTES_PER_PIXEL, 4);
    auto size = height * stride;
    d->streamSize = QSize(width, height);

    uint8_t buffer[1024];
    auto builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    // setup buffers and meta header for new format
    const struct spa_pod *params[3];

#if HAVE_DMA_BUF
    const auto bufferTypes = d->m_eglInitialized ? (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr) :
                                                   (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
#else
    const auto bufferTypes = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
#endif /* HAVE_DMA_BUF */

    params[0] = reinterpret_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
                SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 1, 32),
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(bufferTypes)));
    params[1] = reinterpret_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))));
    params[2] = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
                SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size,
                SPA_POD_Int(sizeof(struct spa_meta_region))));
    pw_stream_update_params(d->pwStream, params, 3);
}

void PipewireStream::onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message)
{
    Q_UNUSED(data);

    qInfo() << "Stream state changed: " << pw_stream_state_as_string(state);

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        qWarning() << "pipewire stream error: " << error_message;
        break;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
        break;
    }
}

void PipewireStream::onStreamProcess(void *data)
{
    qWarning() << "onStreamProcess";
    auto d = static_cast<PipewireStream *>(data);

    pw_buffer* next_buffer;
    pw_buffer* buffer = nullptr;

    next_buffer = pw_stream_dequeue_buffer(d->pwStream);
    while (next_buffer) {
        buffer = next_buffer;
        next_buffer = pw_stream_dequeue_buffer(d->pwStream);

        if (next_buffer) {
            pw_stream_queue_buffer(d->pwStream, buffer);
        }
    }

    if (!buffer) {
        return;
    }


    d->handleFrame(buffer);

    pw_stream_queue_buffer(d->pwStream, buffer);
}

void PipewireStream::initPw()
{
    qInfo() << "Initializing Pipewire connectivity";

    // init pipewire (required)
    pw_init(nullptr, nullptr); // args are not used anyways

    pwMainLoop = pw_thread_loop_new("pipewire-main-loop", nullptr);
    pw_thread_loop_lock(pwMainLoop);

    pwContext = pw_context_new(pw_thread_loop_get_loop(pwMainLoop), nullptr, 0);
    if (!pwContext) {
        qWarning() << "Failed to create PipeWire context";
        return;
    }

    pwCore = pw_context_connect(pwContext, nullptr, 0);
    if (!pwCore) {
        qWarning() << "Failed to connect PipeWire context";
        return;
    }

    pw_core_add_listener(pwCore, &coreListener, &pwCoreEvents, this);

    pwStream = createReceivingStream();
    if (!pwStream) {
        qWarning() << "Failed to create PipeWire stream";
        return;
    }

    if (pw_thread_loop_start(pwMainLoop) < 0) {
        qWarning() << "Failed to start main PipeWire loop";
        isValid = false;
    }

    pw_thread_loop_unlock(pwMainLoop);
}

pw_stream *PipewireStream::createReceivingStream()
{
    spa_rectangle pwMinScreenBounds = SPA_RECTANGLE(1, 1);
    spa_rectangle pwMaxScreenBounds = SPA_RECTANGLE(UINT32_MAX, UINT32_MAX);

    spa_fraction pwFramerateMin = SPA_FRACTION(0, 1);
    spa_fraction pwFramerateMax = SPA_FRACTION(60, 1);

    pw_properties* reuseProps = pw_properties_new_string("pipewire.client.reuse=1");

    auto stream = pw_stream_new(pwCore, "krfb-fb-consume-stream", reuseProps);

    uint8_t buffer[1024] = {};
    const spa_pod *params[1];
    auto builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    params[0] = reinterpret_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6,
                                                    SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA,
                                                    SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                                    SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&pwMaxScreenBounds, &pwMinScreenBounds, &pwMaxScreenBounds),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&pwFramerateMin),
                SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&pwFramerateMax, &pwFramerateMin, &pwFramerateMax)));

    pw_stream_add_listener(stream, &streamListener, &pwStreamEvents, this);

    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, pwStreamNodeId, PW_STREAM_FLAG_AUTOCONNECT, params, 1) != 0) {
        isValid = false;
    }

    return stream;
}

void PipewireStream::handleFrame(pw_buffer *pwBuffer)
{
      qWarning()  << "handleFrame buffer" << QDateTime::currentDateTime();
    auto spaBuffer = pwBuffer->buffer;
    uint8_t *src = nullptr;

    if (spaBuffer->datas[0].chunk->size == 0) {
        qWarning()  << "discarding null buffer";
        return;
    }

    std::function<void()> cleanup;
    const qint64 srcStride = spaBuffer->datas[0].chunk->stride;
    if (spaBuffer->datas->type == SPA_DATA_MemFd) {
        uint8_t *map = static_cast<uint8_t*>(mmap(
            nullptr, spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset,
            PROT_READ, MAP_PRIVATE, spaBuffer->datas->fd, 0));

        if (map == MAP_FAILED) {
            qWarning() << "Failed to mmap the memory: " << strerror(errno);
            return;
        }
        src = SPA_MEMBER(map, spaBuffer->datas[0].mapoffset, uint8_t);

        cleanup = [map, spaBuffer] {
            munmap(map, spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset);
        };
    } else if (spaBuffer->datas[0].type == SPA_DATA_MemPtr) {
        src = static_cast<uint8_t*>(spaBuffer->datas[0].data);
    }
#if HAVE_DMA_BUF
    else if (spaBuffer->datas->type == SPA_DATA_DmaBuf) {
        if (!m_eglInitialized) {
            // Shouldn't reach this
            qWarning() << "Failed to process DMA buffer.";
            return;
        }

        gbm_import_fd_data importInfo = {static_cast<int>(spaBuffer->datas->fd), static_cast<uint32_t>(streamSize.width()),
                                         static_cast<uint32_t>(streamSize.height()), static_cast<uint32_t>(spaBuffer->datas[0].chunk->stride), GBM_BO_FORMAT_ARGB8888};
        gbm_bo *imported = gbm_bo_import(m_gbmDevice, GBM_BO_IMPORT_FD, &importInfo, GBM_BO_USE_SCANOUT);
        if (!imported) {
            qWarning() << "Failed to process buffer: Cannot import passed GBM fd - " << strerror(errno);
            return;
        }

        // bind context to render thread
        eglMakeCurrent(m_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_egl.context);

        // create EGL image from imported BO
        EGLImageKHR image = eglCreateImageKHR(m_egl.display, nullptr, EGL_NATIVE_PIXMAP_KHR, imported, nullptr);

        if (image == EGL_NO_IMAGE_KHR) {
            qWarning() << "Failed to record frame: Error creating EGLImageKHR - " << formatGLError(glGetError());
            gbm_bo_destroy(imported);
            return;
        }

        // create GL 2D texture for framebuffer
        GLuint texture;
        glGenTextures(1, &texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

        src = static_cast<uint8_t*>(malloc(srcStride * streamSize.height()));

        GLenum glFormat = GL_BGRA;
        switch (videoFormat->format) {
            case SPA_VIDEO_FORMAT_RGBx:
                glFormat = GL_RGBA;
                break;
            case SPA_VIDEO_FORMAT_RGBA:
                glFormat = GL_RGBA;
                break;
            case SPA_VIDEO_FORMAT_BGRx:
                glFormat = GL_BGRA;
                break;
            case SPA_VIDEO_FORMAT_RGB:
                glFormat = GL_RGB;
                break;
            case SPA_VIDEO_FORMAT_BGR:
                glFormat = GL_BGR;
                break;
            default:
                glFormat = GL_BGRA;
                break;
        }
        glGetTexImage(GL_TEXTURE_2D, 0, glFormat, GL_UNSIGNED_BYTE, src);

        if (!src) {
            qWarning() << "Failed to get image from DMA buffer.";
            gbm_bo_destroy(imported);
            return;
        }

        cleanup = [src] {
            free(src);
        };

        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(m_egl.display, image);

        gbm_bo_destroy(imported);
    }
#endif /* HAVE_DMA_BUF */

    struct spa_meta_region* videoMetadata =
    static_cast<struct spa_meta_region*>(spa_buffer_find_meta_data(
        spaBuffer, SPA_META_VideoCrop, sizeof(*videoMetadata)));

    if (videoMetadata && (videoMetadata->region.size.width > static_cast<uint32_t>(streamSize.width()) ||
                          videoMetadata->region.size.height > static_cast<uint32_t>(streamSize.height()))) {
        qWarning() << "Stream metadata sizes are wrong!";
        return;
    }

    // Use video metadata when video size from metadata is set and smaller than
    // video stream size, so we need to adjust it.
    bool videoFullWidth = true;
    bool videoFullHeight = true;
    if (videoMetadata && videoMetadata->region.size.width != 0 &&
        videoMetadata->region.size.height != 0) {
        if (videoMetadata->region.size.width < static_cast<uint32_t>(streamSize.width())) {
            videoFullWidth = false;
        } else if (videoMetadata->region.size.height < static_cast<uint32_t>(streamSize.height())) {
            videoFullHeight = false;
        }
    }

    QSize prevVideoSize = videoSize;
    if (!videoFullHeight || !videoFullWidth) {
        videoSize = QSize(videoMetadata->region.size.width, videoMetadata->region.size.height);
    } else {
        videoSize = streamSize;
    }

    if (!fb || videoSize != prevVideoSize) {
        if (fb) {
            free(fb);
        }
        fb = static_cast<char*>(malloc(videoSize.width() * videoSize.height() * BYTES_PER_PIXEL));

        if (!fb) {
            qWarning() << "Failed to allocate buffer";
            isValid = false;
            return;
        }

        //Q_EMIT q->frameBufferChanged();
    }

    const qint32 dstStride = videoSize.width() * BYTES_PER_PIXEL;
    Q_ASSERT(dstStride <= srcStride);

    if (!videoFullHeight && (videoMetadata->region.position.y + videoSize.height() <=  streamSize.height())) {
        src += srcStride * videoMetadata->region.position.y;
    }

    const int xOffset = !videoFullWidth && (videoMetadata->region.position.x + videoSize.width() <= streamSize.width())
                            ? videoMetadata->region.position.x * BYTES_PER_PIXEL : 0;

    char *dst = fb;
    for (int i = 0; i < videoSize.height(); ++i) {
        // Adjust source content based on crop video position if needed
        src += xOffset;
        std::memcpy(dst, src, dstStride);

        if (videoFormat->format == SPA_VIDEO_FORMAT_BGRA || videoFormat->format == SPA_VIDEO_FORMAT_BGRx) {
            for (int j = 0; j < dstStride; j += 4) {
                std::swap(dst[j], dst[j + 2]);
            }
        }

        src += srcStride - xOffset;
        dst += dstStride;
    }

    if (spaBuffer->datas->type == SPA_DATA_MemFd ||
        spaBuffer->datas->type == SPA_DATA_DmaBuf) {
        cleanup();
    }

    if (videoFormat->format != SPA_VIDEO_FORMAT_RGB) {
        const QImage::Format format = videoFormat->format == SPA_VIDEO_FORMAT_BGR  ? QImage::Format_BGR888
                                    : videoFormat->format == SPA_VIDEO_FORMAT_RGBx ? QImage::Format_RGBX8888
                                                                                   : QImage::Format_RGB32;

        QImage img((uchar*)fb, videoSize.width(), videoSize.height(), dstStride, format);
       // img.convertTo(QImage::Format_RGB888);
        static int i = 0;
        QString filename = QString("/home/uos/Pictures/output/output%1.png").arg(i++);
        qWarning() << "savefile" <<filename; //img.save(filename);
        emit ImageReady(&img);
    }

    //q->tiles.append(QRect(0, 0, videoSize.width(), videoSize.height()));
}

