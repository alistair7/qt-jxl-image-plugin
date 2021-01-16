#ifndef QJXLHANDLER_H
#define QJXLHANDLER_H

#include <QImageIOHandler>

class QJxlHandler : public QImageIOHandler
{
public:
    explicit QJxlHandler();
    virtual ~QJxlHandler();

    virtual bool canRead() const override;
    virtual int currentImageNumber() const override;
    virtual QRect currentImageRect() const override;
    virtual int imageCount() const override;
    virtual int loopCount() const override;
    virtual int nextImageDelay() const override;
    virtual QVariant option(ImageOption option) const override;
    virtual bool read(QImage* destImage) override;
    virtual void setOption(ImageOption option, const QVariant &value) override;
    virtual bool supportsOption(ImageOption option) const override;

    static QByteArray getReadableFormat(QIODevice& device);
private:
    bool _readStarted; // Starts out false; set to true the first time read() is called
};


#endif // QJXLHANDLER_H
