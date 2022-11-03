#ifndef PIPEWIRESTRAEM_H
#define PIPEWIRESTRAEM_H

#include <QWidget>

#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>


#define HAVE_DMA_BUF 1

#if HAVE_DMA_BUF
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#endif /* HAVE_DMA_BUF */


class PipewireStream : public QObject
{
    Q_OBJECT

public:
    PipewireStream(QObject *parent = nullptr);
    ~PipewireStream();

    static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message);
    static void onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format);
    static void onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message);
    static void onStreamProcess(void *data);


    void initPw();

    // pw handling
    pw_stream *createReceivingStream();
    void handleFrame(pw_buffer *pwBuffer);


    // pipewire stuff
    struct pw_context *pwContext = nullptr;
    struct pw_core *pwCore = nullptr;
    struct pw_stream *pwStream = nullptr;
    struct pw_thread_loop *pwMainLoop = nullptr;

    // wayland-like listeners
    // ...of events that happen in pipewire server
    spa_hook coreListener = {};
    spa_hook streamListener = {};

    // event handlers
    pw_core_events pwCoreEvents = {};
    pw_stream_events pwStreamEvents = {};

    uint pwStreamNodeId = 0;

    // negotiated video format
    spa_video_info_raw *videoFormat = nullptr;

    // screen geometry holder
    QSize streamSize;
    QSize videoSize;

    // Allowed devices
    uint devices = 0;

    // sanity indicator
    bool isValid = true;

    char *fb = nullptr;

#if HAVE_DMA_BUF
    struct EGLStruct {
        QList<QByteArray> extensions;
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
    };

    bool m_eglInitialized = false;
    qint32 m_drmFd = 0; // for GBM buffer mmap
    gbm_device *m_gbmDevice = nullptr; // for passed GBM buffer retrieval

    EGLStruct m_egl;
#endif /* HAVE_DMA_BUF */
signals:
    void ImageReady(QImage* image);

};
#endif // PIPEWIRESTRAEM_H
