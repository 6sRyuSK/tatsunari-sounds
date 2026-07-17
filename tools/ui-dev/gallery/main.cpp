#include "GalleryFrame.h"
#include "Bridge.h"

#include <visage/app.h>

//
// Gallery entry point. On Emscripten, runEventLoop() installs the browser main
// loop and unwinds the C++ stack (keeping the runtime alive), so the app + gallery
// must outlive this call — hence static storage. The bridge holds a pointer to the
// gallery for the whole session.
//
namespace
{
    constexpr int kWidth  = 780;
    constexpr int kHeight = 720;
}

int runExample()
{
    static visage::ApplicationWindow app;
    static GalleryFrame gallery;

    app.addChild (gallery);
    gallery.setBounds (0.0f, 0.0f, static_cast<float> (kWidth), static_cast<float> (kHeight));
    gallery::setBridgeTarget (&gallery);

    app.setTitle ("Factory UI · Visage Gallery");
    app.show (kWidth, kHeight);
    app.runEventLoop();
    return 0;
}

#if defined(_WIN32)
 #include <windows.h>
int WINAPI WinMain (HINSTANCE, HINSTANCE, LPSTR, int) { return runExample(); }
#else
int main (int, char**) { return runExample(); }
#endif
