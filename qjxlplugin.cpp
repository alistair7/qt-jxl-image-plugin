#include <QImageIOPlugin>

#include "qjxlhandler.h"


class QJxlPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "qt-jxl-image-plugin.json")

public:
    QJxlPlugin(QObject *parent = nullptr);
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override;
    QImageIOHandler *create(QIODevice *device, const QByteArray &format = QByteArray()) const override;
};

QJxlPlugin::QJxlPlugin(QObject *parent)
    : QImageIOPlugin(parent)
{
}


QImageIOPlugin::Capabilities QJxlPlugin::capabilities(QIODevice *device, const QByteArray &format) const
{
    if(device == nullptr)
        return (format == "jxl") ? QImageIOPlugin::CanRead : Capabilities{};

    return QJxlHandler::getReadableFormat(*device) == "jxl" ?
                QImageIOPlugin::CanRead :
                Capabilities{};
}

QImageIOHandler *QJxlPlugin::create(QIODevice *device, const QByteArray &format) const
{
    if(format == "jxl" || (format.isEmpty() && this->capabilities(device, "jxl") != 0))
    {
        QJxlHandler *hand = new QJxlHandler;
        hand->setDevice(device);
        hand->setFormat(format);
        return hand;
    }
    return nullptr;
}

#include "qjxlplugin.moc"
