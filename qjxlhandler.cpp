/* qjxlhandler.cpp */

#include <limits>

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

// Suppress gcc switch fallthrough warnings
#ifdef __GNUC__
#define QJXLHANDLER_FALLTHROUGH __attribute__ ((fallthrough));
#else
#define QJXLHANDLER_FALLTHROUGH
#endif


/* The libjxl *_cxx.h headers define functions so I can't include them in qjxlhandler.h without linker errors.
 * So all the Jxl objects are defined in this source file. */
struct QJxlState
{
    JxlDecoderPtr dec;                  // Main decoder context.
    JxlThreadParallelRunnerPtr runner;  // (Just needs to exist.)
    JxlBasicInfo basicInfo;             // File metadata.
    JxlPixelFormat pixelFormat;         // Channel/depth info.
    QByteArray iccProfile;              // ICC blob, if available.

    std::unique_ptr<uint8_t[]> pixels;  // Single frame of decoded pixels.
    size_t pixelsLength;                // Size of frame in bytes.

    int currentImageNumber;             // Sequence no. of the last frame read() (0-indexed).
    int imageCount;                     // Total frames.
    int currentFrameDurationMs;         // Duration of current frame in milliseconds.
    float msPerTick;                    // Duration of a "tick" in milliseconds.
    int nextFrame;                      // Next frame Qt wants (implicit or via jumpToImage or jumpToNextImage).

    QByteArray fileData;                // Buffered input file.
};


inline static bool subscribeEvents(JxlDecoder *dec)
{
    const int events_wanted = JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE
#ifdef QJXLHANDLER_USE_ICC
                      | JXL_DEC_COLOR_ENCODING
#endif
    ;
    if(JxlDecoderSubscribeEvents(dec, events_wanted) != JXL_DEC_SUCCESS)
    {
        qWarning("Failed in JxlDecoderSubscribeEvents");
        return false;
    }
    return true;
}


QJxlHandler::QJxlHandler() :
    QImageIOHandler(),
    _state(nullptr),
    _progress(Invalid)
{
    /* QImageIOHandler is sometimes instantiated and destroyed just to call canRead(),
     * so don't work too hard in the constructor.  Decoder initialization is deferred until
     * the application calls read(). */
}


void QJxlHandler::_init()
{
    if(_progress != Invalid)
        return;

    _state.reset(new QJxlState
    {
        .dec = JxlDecoderMake(nullptr),
        .runner = JxlThreadParallelRunnerMake(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads()),
        // Default to 8-bit sampling.  Changes to 16-bit later if required.
        .pixelFormat = {
                          .num_channels = 4, // 3 colors + alpha
                          .data_type = JXL_TYPE_UINT8,
                          .endianness = JXL_NATIVE_ENDIAN,
                          .align = 0
                        },
        .currentImageNumber = -1,
        .imageCount = -1,
        .nextFrame = 0,
    });

    if(_state == nullptr)         return (void)qWarning("Failed to create state object");
    if(_state->dec == nullptr)    return (void)qWarning("Failed to create JxlDecoder");
    if(_state->runner == nullptr) return (void)qWarning("Failed to create JxlThreadParallelRunner");

    if(JxlDecoderSetParallelRunner(_state->dec.get(), JxlThreadParallelRunner, _state->runner.get()) != JXL_DEC_SUCCESS)
        return (void)qWarning("Failed in JxlDecoderSetParallelRunner");

    if(!subscribeEvents(_state->dec.get()))
        return;

    _progress = HaveState;
}

bool QJxlHandler::isInitialized() const
{
    return _progress > Invalid;
}



bool QJxlHandler::_rewind()
{
    JxlDecoderReset(_state->dec.get());

    if(JxlDecoderSetParallelRunner(_state->dec.get(), JxlThreadParallelRunner, _state->runner.get()) != JXL_DEC_SUCCESS)
    {
        qWarning("Failed in JxlDecoderSetParallelRunner");
        _state = nullptr;
        return false;
    }

    if(!subscribeEvents(_state->dec.get()))
    {
        _state = nullptr;
        return false;
    }

    if(JxlDecoderSetInput(_state->dec.get(), (const uint8_t*)_state->fileData.constData(), _state->fileData.size()) != JXL_DEC_SUCCESS)
    {
      qWarning("Failed in JxlDecoderSetInput");
      return false;
    }
    _state->currentFrameDurationMs = 0;
    _state->currentImageNumber = -1;
    //_state->imageCount  // Keep this populated
    _state->nextFrame = 0; // TODO: don't want to reset this if we're wrapping around to find the requested frame
    return true;
}

QJxlHandler::~QJxlHandler()
{
}

bool QJxlHandler::canRead() const
{
    if(_progress >= HaveState && !_state->fileData.isEmpty())
        return true;

    if (!device() || !device()->isReadable())
        return false;

    QByteArray mimeFormat = getReadableFormat(*device());

    if(mimeFormat.isEmpty())
        return false;

    setFormat("jxl");
    return true;
}

int QJxlHandler::currentImageNumber() const
{
    if(_progress < HaveBasicInfo)
        qWarning("Request for current image number before we have basic info");

    return _state->currentImageNumber;
}

QRect QJxlHandler::currentImageRect() const
{
    qWarning("currentImageRect is unsupported");
    return {};
}

int QJxlHandler::imageCount() const
{
    // libjxl doesn't seem to provide this without us decoding every frame
    if(_progress == Invalid || _state->imageCount == -1)
    {
        qWarning("Request for image count but we haven't counted them yet");
        return 0;
    }
    return _state->imageCount;
}

bool QJxlHandler::jumpToImage(int imageNumber)
{
    if(_progress >= HaveBasicInfo && !_state->basicInfo.have_animation)
    {
        qWarning("Jumping to frame %d but this isn't an animation", imageNumber);
        return false;
    }
    if(_state->imageCount > -1 && imageNumber >= _state->imageCount)
    {
        qWarning("Requested frame is out of range: %d", imageNumber);
        return false;
    }

    _state->nextFrame = imageNumber;

    if(imageNumber <= _state->currentImageNumber)
    {
        // To get a previous frame, have to start decoding from the beginning
        _rewind();
    }

    return true;
}

bool QJxlHandler::jumpToNextImage()
{
    if(_progress >= HaveBasicInfo && !_state->basicInfo.have_animation)
    {
        qWarning("Jumping to next frame but this isn't an animation");
        return false;
    }

    if(++_state->nextFrame == _state->imageCount)
    {
        qWarning("There is no next frame to jump to");
        _state->nextFrame--;
        return false;
    }

    return true;
}

int QJxlHandler::loopCount() const
{
    if(_progress < HaveBasicInfo)
    {
        qWarning("Request for loop count before we've read basicInfo structure");
        return 0;
    }

    // Qt doesn't document an "infinity" option, so use INT_MAX
    if(_state->basicInfo.have_animation)
        return _state->basicInfo.animation.num_loops > 0 ? _state->basicInfo.animation.num_loops : std::numeric_limits<int>::max();

    return 0;
}

int QJxlHandler::nextImageDelay() const
{
    return _state->basicInfo.have_animation ? _state->currentFrameDurationMs : 0;
}

QVariant QJxlHandler::option(ImageOption opt) const
{
    if(_progress < HaveBasicInfo &&
         (/*opt == ImageOption::Size || */
          opt == ImageOption::Animation)
      )
    {
        qWarning("Unable to provide option %d before basic info is available", (int)opt);
        return {};
    }

    switch(opt)
    {
    /*case ImageOption::Size:
        return QSize(_state->basicInfo.xsize, _state->basicInfo.ysize);*/
    case ImageOption::Animation:
        return _state->basicInfo.have_animation;
    default:
        qWarning("Request for unsupported option %d", (int)opt);
        return {};
    }
}


bool QJxlHandler::read(QImage* destImage)
{
    if (!destImage)
    {
        qWarning("Destination QImage is null");
        return false;
    }

    if(_progress < HaveState)
      _init();

    // Run the decoder until we have frame index _state->nextFrame in _state->pixels
    ReadUntil result = _readUntil(ReadUntil::NextFrameDecoded);
    if(result != ReadUntil::NextFrameDecoded && result != ReadUntil::End)
    {
        qWarning("Failed to decode frame");
        return false;
    }

    if(result == ReadUntil::End)
    {
        // We hit EOF while trying to get the next frame.  Loop back to frame 0.
        _rewind();
        if(_readUntil(ReadUntil::NextFrameDecoded) != ReadUntil::NextFrameDecoded)
        {
            qWarning("Restarted decoding but failed to get frame 0");
            _progress = Invalid;
            return false;
        }
    }


    unsigned bytesPerSample;
    QImage::Format qtPixelFormat;

    if(_state->pixelFormat.data_type == JXL_TYPE_UINT8)
    {
        bytesPerSample = 1;
        qtPixelFormat = QImage::Format_RGBA8888;
    }
    else if(_state->pixelFormat.data_type == JXL_TYPE_UINT16)
    {
        bytesPerSample = 2;
        qtPixelFormat = QImage::Format_RGBA64;

    }
    else
    {
        qWarning("Pixel format isn't set correctly");
        return false;
    }

    int stride = _state->basicInfo.xsize * _state->pixelFormat.num_channels * bytesPerSample;

    // Create the image and transfer pixel ownership to Qt
    auto pixelPtr = _state->pixels.release();
    *destImage = QImage(pixelPtr, static_cast<int>(_state->basicInfo.xsize), static_cast<int>(_state->basicInfo.ysize),
                        stride, qtPixelFormat, [](void* img) { delete [] (uint8_t*)img; }, pixelPtr);

#ifdef QJXLHANDLER_USE_ICC
    // Tell the QImage the colorspace of the pixels
    if(!_state->iccProfile.isEmpty())
    {
        QColorSpace cs = QColorSpace::fromIccProfile(QByteArray((const char*)_state->iccProfile.constData(), _state->iccProfile.size()));
        if(cs.isValid())
            destImage->setColorSpace(cs);
        else
            qWarning("Embedded colorspace unsupported; falling back on sRGB");
    }
#endif

    return true;
}


QJxlHandler::ReadUntil QJxlHandler::_readUntil(QJxlHandler::ReadUntil until)
{

    if(until == ReadUntil::BasicInfoAvailable && _progress >= HaveBasicInfo)
        return ReadUntil::BasicInfoAvailable;

    JxlDecoderStatus res;
    JxlDecoderStruct *dec = _state->dec.get();

    // Buffer whole JXL file.  TODO: Don't.
    if(_state->fileData.isEmpty())
    {
        if(device() == nullptr)
        {
            qWarning("Read attempted out of sequence - device is not set");
            return ReadUntil::Error;
        }
        _state->fileData = device()->readAll();

        if(JxlDecoderSetInput(dec, (const uint8_t*)_state->fileData.constData(), _state->fileData.size()) != JXL_DEC_SUCCESS)
        {
            qWarning("Failed in JxlDecoderSetInput");
            return ReadUntil::Error;
        }


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
     *
     * Also note, "Gwenview can only apply color profile on RGB32 or ARGB32 images" - so might be worth calling QImage.convertTo(ARGB32) if we have a profile.
     */


    // Can alternatively ask libjxl for the default, but it likes to use float, which QImage doesn't support:
    //JxlDecoderStatus res = JxlDecoderDefaultPixelFormat(dec.get(), &pixelFormat);

    JxlDecoderStatus decoderStatus;

    // Start decoding, handling interesting events along the way
    while((decoderStatus = JxlDecoderProcessInput(dec)) != JXL_DEC_SUCCESS)
    {
      
      switch(decoderStatus)
      {
      case JXL_DEC_BASIC_INFO:
          if(_progress >= HaveBasicInfo)
          {
              // Cautiously discard any buffered pixels
              _state->pixels = nullptr;
          }

          // Get image dimensions etc.
          if((res = JxlDecoderGetBasicInfo(dec, &_state->basicInfo)) != JXL_DEC_SUCCESS)
          {
              qWarning("Failed in JxlDecoderGetBasicInfo");
              return ReadUntil::Error;
          }

          // If metadata indicates > 8-bit depth, switch to 16-bit
          if(_state->basicInfo.bits_per_sample > 8)
          {
              _state->pixelFormat.data_type = JXL_TYPE_UINT16;
          }

          _progress = HaveBasicInfo;

          if(_state->basicInfo.have_animation)
          {
              _state->msPerTick = 1000 * _state->basicInfo.animation.tps_denominator / (float)_state->basicInfo.animation.tps_numerator;
              // imageCount remains -1 because we have to count them as we go
          }
          else
          {
              _state->imageCount = 1;
          }

          if(until == ReadUntil::BasicInfoAvailable)
              return ReadUntil::BasicInfoAvailable;

          break;


#ifdef QJXLHANDLER_USE_ICC
        case JXL_DEC_COLOR_ENCODING:

            // Get ICC color profile
            size_t icc_size;
            if (JxlDecoderGetICCProfileSize(dec, &_state->pixelFormat, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderGetICCProfileSize");
                continue;
            }
            _state->iccProfile.resize(icc_size);
            if (JxlDecoderGetColorAsICCProfile(dec, &_state->pixelFormat, JXL_COLOR_PROFILE_TARGET_DATA, (uint8_t*)_state->iccProfile.data(), _state->iccProfile.size()) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderGetColorAsICCProfile");
                _state->iccProfile = "";
                continue;
            }

            break;
#endif

        case JXL_DEC_NEED_IMAGE_OUT_BUFFER:

            // Time to allocate some space for the pixels
            if (JxlDecoderImageOutBufferSize(dec, &_state->pixelFormat, &_state->pixelsLength) != JXL_DEC_SUCCESS )
            {
                qWarning("Failed in JxlDecoderImageOutBufferSize");
                return ReadUntil::Error;
            }

            // Sanity check
            if (_state->pixelsLength != (size_t)_state->basicInfo.xsize * _state->basicInfo.ysize * _state->pixelFormat.num_channels * (_state->pixelFormat.data_type == JXL_TYPE_UINT8 ? 1 : 2))
            {
                qWarning("Pixel buffer size is %zu, but expected (%u * %u * %u * %u) = %zu",
                         _state->pixelsLength, _state->basicInfo.xsize, _state->basicInfo.ysize, _state->pixelFormat.num_channels, (_state->pixelFormat.data_type == JXL_TYPE_UINT8 ? 1 : 2),
                         (size_t)_state->basicInfo.xsize * _state->basicInfo.ysize * _state->pixelFormat.num_channels * (_state->pixelFormat.data_type == JXL_TYPE_UINT8 ? 1 : 2));
                return ReadUntil::Error;
            }
            
            // Normally after a frame is decoded, we'll pass it to Qt and set pixels to nullptr.
            // If we still own the data for whatever reason, keep the buffer we have and overwrite it.
            if(_state->pixels.get() == nullptr)
            {
                _state->pixels.reset(new uint8_t[_state->pixelsLength]);
                if(_state->pixels.get() == nullptr)
                {
                    qWarning("Failed to allocate %zu B", _state->pixelsLength);
                    return ReadUntil::Error;
                }
            }
            else
            {
                qWarning("Overwriting previously buffered pixels");
            }
            
            if (JxlDecoderSetImageOutBuffer(dec, &_state->pixelFormat, _state->pixels.get(), _state->pixelsLength) != JXL_DEC_SUCCESS)
            {
                qWarning("Failed in JxlDecoderSetImageOutBuffer");
                return ReadUntil::Error;
            }
            break;

        case JXL_DEC_FRAME:
            // Start of frame - can extract duration etc.
            if(_state->basicInfo.have_animation)
            {
                JxlFrameHeader frameHeader;
                if(JxlDecoderGetFrameHeader(dec, &frameHeader) != JXL_DEC_SUCCESS)
                {
                    qWarning("Failed in JxlDecoderGetFrameHeader");
                    return ReadUntil::Error;
                }

                _state->currentFrameDurationMs = (int)(_state->msPerTick * frameHeader.duration);

                //if(frameHeader.is_last) // This flag is NEVER set :(
                //    qDebug("This is the last frame; counted %d in total", _state->currentImageNumber+1);

            }
            break;

        case JXL_DEC_FULL_IMAGE:
            // End of frame

            _state->currentImageNumber ++;

            if(until == ReadUntil::NextFrameDecoded)
            {
                if(_state->nextFrame == _state->currentImageNumber)
                {
                    _state->nextFrame++;
                    return ReadUntil::NextFrameDecoded;
                }

                // If the frame we just decoded wasn't the next requested one (e.g. if jumpToImage() has been used), then ignore it and keep going.

            }
            break;

        case JXL_DEC_NEED_MORE_INPUT:
            qWarning("Input truncated");
            return ReadUntil::Error;

        case JXL_DEC_ERROR:
            qWarning("Error while decoding");
            return ReadUntil::Error;

        //case JXL_DEC_EXTENSIONS:
        //case JXL_DEC_PREVIEW_IMAGE:
        //case JXL_DEC_DC_IMAGE:
        default:
            qWarning("Unexpected result from JxlDecoderProcessInput");
            return ReadUntil::Error;
            
      }

    }
    
    // If we reach here, we've already returned all frames but Qt still wants the next frame.
    return ReadUntil::End;
}



void QJxlHandler::setOption(ImageOption opt, const QVariant& value)
{
    Q_UNUSED(value)
    qWarning("Caller tried to set unsupported option %d", (int)opt);
}

bool QJxlHandler::supportsOption(ImageOption option) const
{
    /* Supporting Size seems like a good idea, but I don't know how I'm supposed
     * to determine it before Qt requests it.  canRead() isn't supposed
     * to change the device state, and read() is too late...
     * I could just peek an arbitrary amount into device until I get basicInfo... */

    return /*option == ImageOption::Size ||*/
           option == ImageOption::Animation;
}



QByteArray QJxlHandler::getReadableFormat(QIODevice& device)
{
    const int signatureBytes = 12;
    QByteArray header = device.peek(signatureBytes);
    if(header.size() != signatureBytes)
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
