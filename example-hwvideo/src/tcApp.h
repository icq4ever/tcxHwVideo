#pragma once

#include <TrussC.h>
#include "tcxHwVideoPlayer.h"

using namespace std;
using namespace tc;

class tcApp : public App {
public:
    void setup() override;
    void update() override;
    void draw() override;
    void keyPressed(int key) override;
    void filesDropped(const vector<string>& files) override;

private:
    void loadVideo(const string& path);

    tcx::HwVideoPlayer player_;
    string statusText_ = "Drop a video file to play";
};
