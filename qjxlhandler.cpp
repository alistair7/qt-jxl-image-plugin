/* qjxlhandler.cpp */

#include <limits>
#include <memory>

#include <QtCore/QVariant>
#include <QtCore/QSize>
#include <QtGui/QImage>

#include <jxl/decode_cxx.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include "qjxlhandler.h"

// QImage supports ICC profiles since 5.14
#if QT_VERSION >= 0x050D00
#include <QtGui/QColorSpace>
#define QJXLHANDLER_USE_ICC
#endif


QJxlHandler::QJxlHandler() :
    QImageIOHandler(),
    _readStarted(false)
{
}

QJxlHandler::~QJxlHandler()
{
}

bool QJxlHandler::canRead() const
{
    if (!device())
        return false;

    QByteArray mimeFormat = getReadableFormat(*device());

    if(mimeFormat.isEmpty())
        return false;

    setFormat("jxl");
    return true;
}

int QJxlHandler::currentImageNumber() const
{
    // Not doing animation atm
    return _readStarted ? 0 : -1;
}

QRect QJxlHandler::currentImageRect() const
{
    return {};
}

int QJxlHandler::imageCount() const
{
    return 0;
}

int QJxlHandler::loopCount() const
{
    return 0;
}

int QJxlHandler::nextImageDelay() const
{
    return 0;
}

QVariant QJxlHandler::option(ImageOption opt) const
{
    Q_UNUSED(opt);
    return {};
}

bool QJxlHandler::read(QImage* destImage)
{
    if (!destImage) {
        qWarning("Destination QImage is null");
        return false;
    }

    _readStarted = true;

#ifdef EVERY_PIC_IS_A_SMALL_CYAN_SQUARE
    uint32_t *pix = new uint32_t[10*10];
    if(!pix)
    {
        qWarning("Malloc failed");
        return false;
    }
    for(int i=0; i<10*10; i++)
        pix[i] = 0xFF00FFFF;

    *destImage = QImage((uchar*)pix, 10, 10, 10*4, QImage::Format_ARGB32, [](void* img) { delete [] (uint32_t*)img; }, pix);
    return true;
#endif


    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    if(dec.get() == nullptr) {
        qWarning("Failed to create JxlDecoder");
        return false;
    }
    
    // Tell decoder to use as many threads as it wants
    size_t nThreads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
    JxlThreadParallelRunnerPtr runner = JxlThreadParallelRunnerMake(nullptr, nThreads);
    if(runner.get() == nullptr) {
        qWarning("Failed to create JxlThreadParallelRunner");
        return false;
    }
    if(JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner, runner.get()) != JXL_DEC_SUCCESS) {
        qWarning("Failed in JxlDecoderSetParallelRunner");
        return false;
    }

    // Buffer whole JXL file.  TODO: Don't.
    QByteArray fileData = device()->readAll();



    // Decoding will pause on interesting events
    int events_wanted = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE;

#ifdef QJXLHANDLER_USE_ICC
    QByteArray icc_profile;
    events_wanted |= JXL_DEC_COLOR_ENCODING;
#endif

    if(JxlDecoderSubscribeEvents(dec.get(), events_wanted) != JXL_DEC_SUCCESS)
    {
        qWarning("Failed in JxlDecoderSubscribeEvents");
        return false;
    }
    

    /* If I'm interpreting the docs right...
     *
     *  - JXL_*_ENDIAN has no influence on channel order - just byte order within 16/32-bit samples.
     *  - Asking libjxl for 4 channels currently means RGBA, but the API looks like it will change in this area.
     *  - QtImage::Format_ARGB32 expects channels in an endian-dependent order.  (So (uint32_t)0xFF00FFFF always produces cyan but (char*)"\xFF\x00\xFF\xFF" may not)
     *  - QtImage::Format_RGBA8888 expects RGBA bytes in that order regardless of endianness.
     *  - QtImage::Format_RGBA64 expects RGBA in that order, but byte order within each 16-bit sample is endian-dependent.
     *
     * Hence...
     *
     *   (JXL_TYPE_UINT8,  *)                 <-> QtImage::Format_RGBA8888
     *   (JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN) <-> QtImage::Format_RGBA64
     *
     * ...and if you want anything else you have to do platform-specific byte shuffling.
     */


    // Can alternatively ask libjxl for the default, but it likes to use float, which QImage doesn't support:
    //JxlDecoderStatus res = JxlDecoderDefaultPixelFormat(dec.get(), &pixelFormat);

    JxlPixelFormat pixelFormat = { .num_channels = 4, // 3 colors + alpha
                                   .data_type = JXL_TYPE_UINT8,
                                   .endianness = JXL_NATIVE_ENDIAN, // doesn't affect channel order, just order of 16/32-bit samples
                                   .align = 0
                                 };
    JxlBasicInfo basicInfo;
    QImage::Format qtPixelFormat = QImage::Format_RGBA8888;
    unsigned bytesPerSample = 1;
    std::unique_ptr<uint8_t[]> pixels; // Decoded pixel data
    JxlDecoderStatus res;

    // Feed the decoder with bytes from the device
    size_t avail_in = fileData.size();
    const uint8_t *next_in = (uint8_t*)fileData.constData();
    

    // Start decoding, handling interesting events along the way
    while((res = JxlDecoderProcessInput(dec.get(), &next_in, &avail_in)) != JXL_DEC_SUCCESS)
    {
      
      switch(res)
      {
        case JXL_DEC_BASIC_INFO:
            // Get image dimensions etc.
            if(JxlDecoderGetBasicInfo(dec.get(), &basicInfo) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderGetBasicInfo");
                return false;
            }
            if(basicInfo.have_animation)
            {
                qWarning("Input is an animation, which we can't yet handle");
            }

            // If metadata indicates > 8-bit depth, switch to 16-bit
            if(basicInfo.bits_per_sample > 8)
            {
                pixelFormat.data_type = JXL_TYPE_UINT16;
                bytesPerSample = 2;
                qtPixelFormat = QImage::Format_RGBA64;
            }

            break;


#ifdef QJXLHANDLER_USE_ICC
        case JXL_DEC_COLOR_ENCODING:
            // Get ICC color profile
            size_t icc_size;
            if (JxlDecoderGetICCProfileSize(dec.get(), &pixelFormat, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderGetICCProfileSize");
                continue;
            }
            icc_profile.resize(icc_size);
            if (JxlDecoderGetColorAsICCProfile(dec.get(), &pixelFormat, JXL_COLOR_PROFILE_TARGET_DATA, (uchar*)icc_profile.data(), icc_profile.size()) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderGetColorAsICCProfile");
                icc_profile = "";
                continue;
            }

            break;
#endif

        case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
            // Time to allocate some space for the pixels
            size_t bytes_required;
            if (JxlDecoderImageOutBufferSize(dec.get(), &pixelFormat, &bytes_required) != JXL_DEC_SUCCESS )
            {
                qWarning("Failed in JxlDecoderImageOutBufferSize");
                return false;
            }
            // Sanity check
            if (bytes_required != (size_t)basicInfo.xsize * basicInfo.ysize * pixelFormat.num_channels * bytesPerSample)
            {
                qWarning("Pixel buffer size is %zu, but expected (%u * %u * %u * %u) = %zu",
                         bytes_required, basicInfo.xsize, basicInfo.ysize, pixelFormat.num_channels, bytesPerSample,
                         (size_t)basicInfo.xsize * basicInfo.ysize * pixelFormat.num_channels * bytesPerSample);
                return false;
            }
            
            pixels.reset(new uint8_t[bytes_required]);
            if(pixels.get() == nullptr)
            {
                qWarning("Failed to allocate %zu B", bytes_required);
                return false;
            }
            
            if (JxlDecoderSetImageOutBuffer(dec.get(), &pixelFormat, pixels.get(), bytes_required) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderSetImageOutBuffer");
                return false;
            }

        case JXL_DEC_FULL_IMAGE:
            // If the file contains multiple images (e.g. an animation), we'll hit this multiple times.
            // We'll also waste time decoding each one until we're left with the last frame.
            break;

        case JXL_DEC_NEED_MORE_INPUT:
            if(avail_in == 0) {
                // No more data to be had
                qWarning("Input truncated");
                return false;
            }
            continue;

        case JXL_DEC_ERROR:
            qWarning("Error while decoding");
            return false;

        //case JXL_DEC_EXTENSIONS:
        //case JXL_DEC_PREVIEW_IMAGE:
        //case JXL_DEC_DC_IMAGE:
        default:
            qWarning("Unexpected result from JxlDecoderProcessInput");
            return false;
            
      }
    }
    

    int stride = basicInfo.xsize * pixelFormat.num_channels * bytesPerSample;

    // Create the image and transfer pixel ownership to Qt
    auto pixelPtr = pixels.release();
    *destImage = QImage(pixelPtr, static_cast<int>(basicInfo.xsize), static_cast<int>(basicInfo.ysize),
                        stride, qtPixelFormat, [](void* img) { delete [] (uint8_t*)img; }, pixelPtr);

#ifdef QJXLHANDLER_USE_ICC
    // Tell the QImage the colorspace of the pixels
    if(!icc_profile.isEmpty())
    {
        QColorSpace cs = QColorSpace::fromIccProfile(QByteArray((const char*)icc_profile.constData(), icc_profile.size()));
        if(cs.isValid())
            destImage->setColorSpace(cs);
        else
            qWarning("Embedded colorspace unsupported; falling back on sRGB");
    }
#endif

    
    return true;
}



void QJxlHandler::setOption(ImageOption opt, const QVariant& value)
{
    Q_UNUSED(opt)
    Q_UNUSED(value)
    return;
}

bool QJxlHandler::supportsOption(ImageOption option) const
{
    switch(option)
    {
    case ImageFormat:
    case Size:
        // maybe one day
    default:
        return false;
    }
}



QByteArray QJxlHandler::getReadableFormat(QIODevice& device)
{
    // TODO: Maybe return different formats if it's e.g. an image sequence.

    QByteArray header = device.peek(12);
    if(header.size() != 12)
    {
        qInfo("Only got %dB from peek", header.size());
        return {};
    }

    switch(JxlSignatureCheck((uint8_t*)header.constData(), header.size()))
    {
        case JXL_SIG_NOT_ENOUGH_BYTES:
            qWarning("Signature check wants more than 12 bytes");
            return {};
        case JXL_SIG_INVALID:
            //qWarning("Signature invalid");
            return {};
        case JXL_SIG_CODESTREAM:
        case JXL_SIG_CONTAINER:
            return "jxl";
    }

    return {};
}
