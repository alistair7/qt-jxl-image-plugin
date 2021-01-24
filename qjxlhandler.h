#ifndef QJXLHANDLER_H
#define QJXLHANDLER_H

#include <memory>
#include <QImageIOHandler>


struct QJxlState;

class QJxlHandler : public QImageIOHandler
{
public:
    explicit QJxlHandler();
    virtual ~QJxlHandler();

    virtual bool canRead() const override;
    virtual int currentImageNumber() const override;
    virtual QRect currentImageRect() const override;
    virtual int imageCount() const override;
    virtual bool jumpToImage(int imageNumber) override;
    virtual bool jumpToNextImage() override;
    virtual int loopCount() const override;
    virtual int nextImageDelay() const override;
    virtual QVariant option(ImageOption option) const override;
    virtual bool read(QImage* destImage) override;
    virtual void setOption(ImageOption option, const QVariant &value) override;
    virtual bool supportsOption(ImageOption option) const override;

    static QByteArray getReadableFormat(QIODevice& device);
    bool isInitialized() const;

private:

    // Private structure to maintain state between calls to read()
    std::unique_ptr<QJxlState> _state;

    enum Progress
    {
        Invalid,
        HaveState,
        HaveBasicInfo,
    };
    Progress _progress;


    void _init();

    enum ReadUntil
    {
        BasicInfoAvailable,  // Just read enough of the file to determine the image properties.
        NextFrameDecoded,
        End,
        Error,
    };

    // Begin or continue decoding until the specified event occurs.
    // Decoding will stop on errors (Error) or EOF (End) regardless.
    // The event that stopped the decoding is returned.
    ReadUntil _readUntil(ReadUntil until);

    // Reset internal state so we can start decoding from the beginning.
    bool _rewind();

};


#endif // QJXLHANDLER_H
