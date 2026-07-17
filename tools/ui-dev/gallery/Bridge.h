#pragma once

class GalleryFrame;

//
// The JS <-> WASM bridge (extern "C" functions in Bridge.cpp) needs a live
// GalleryFrame to act on. main.cpp calls this once, after the gallery has been
// added to the window.
//
namespace gallery
{
    void setBridgeTarget (GalleryFrame* gallery);
}
