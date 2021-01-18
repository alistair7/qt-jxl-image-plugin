## JPEG XL Image Format Plugin for Qt ##

### Features ###
* Allows Qt applications to read single-image [JPEG XL](https://jpeg.org/jpegxl/index.html) files with depth of up to 16-bits per channel (wider gamuts will get silently converted to 16-bit by the decoder).
* Supports all compression modes: lossy, lossless, and transcoded JPEG.

### Non-Features ###
* No support for writing.
* No support for animations (the last frame is returned as a static image).
* Ambiguous support for CMYK(A) colorspaces.  I _think_ the decoder will handle them by converting to RGBA, but this is untested, and the libjxl API is unfinished in this area.

### Disclaimers ###
This software is provided with no warranty.  libjxl is under heavy development, and breaking API changes are somewhat likely.
I'm far from being an expert in Qt, and I know very little about cmake, so I'm relying on what the QtCreator IDE saw fit to generate.

### Build ###

#### Build Dependencies ####
* cmake.
* Qt5 or Qt6 development libraries - specifically the Gui component.
* [libjxl](https://gitlab.com/wg1/jpeg-xl) with development headers.

```
git clone https://github.com/alistair7/qt-jxl-image-plugin.git
mkdir qjxl-build
cd qjxl-build
cmake ../qt-jxl-image-plugin
make
```

Alternatively, open CMakeLists.txt in QtCreator and hit Build. Note, if you build in Debug configuration, you must have a Debug build of Qt in order to load it later.

### Install ###
You need to install qt-jxl-image-plugin.so to a location where Qt can find it. This is system dependent and it won't be your normal library path.

Try
`sudo install -m644 qt-jxl-image-plugin.so "$(qmake -query QT_INSTALL_PLUGINS)/imageformats"`

(For me this is /usr/lib/x86_64-linux-gnu/qt5/plugins/imageformats)

### Hints ###
* To check whether a Qt app is successfully loading the plugin, run the app with `QT_DEBUG_PLUGINS=1` in its environment.
* I found that KDE apps that loaded the plugin, and probed its capabilities, and got a positive "CanRead" response for .jxl files, would still not attempt to actually invoke the handler and read the file.  It was necessary to associate the .jxl extension with the mime type image/jxl (matching the entry in qt-jxl-image-plugin.json) through System Settings > Applications > File Associations.
