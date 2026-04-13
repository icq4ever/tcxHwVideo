#include "tcApp.h"

void tcApp::setup() {
    logNotice("tcApp") << "=== HwVideo Player Example ===";
    logNotice("tcApp") << "Drop a video file to play";
    logNotice("tcApp") << "Keys: Space=Play/Pause, R=Restart, L=Loop, []=Speed, Arrows=Seek/Vol";
}

void tcApp::loadVideo(const string& path) {
    if (player_.load(path)) {
        player_.play();
        statusText_ = "Loaded: " + path
                    + " [" + player_.getHwAccelName() + "]";
        logNotice("tcApp") << "HW backend: " << player_.getHwAccelName();
    } else {
        statusText_ = "Failed to load: " + path;
        logError("tcApp") << "Failed to load: " << path;
    }
}

void tcApp::update() {
    player_.update();
}

void tcApp::draw() {
    clear(0.1f);

    if (player_.isLoaded()) {
        // Draw video centered
        float vw    = player_.getWidth();
        float vh    = player_.getHeight();
        float scale = min(getWindowWidth() / vw, (getWindowHeight() - 80.f) / vh);
        float x     = (getWindowWidth()  - vw * scale) / 2.f;
        float y     = (getWindowHeight() - 80.f - vh * scale) / 2.f;

        resetStyle();
        player_.draw(x, y, vw * scale, vh * scale);

        // HW accel badge
        bool hw = player_.isUsingHwAccel();
        setColor(hw ? Color(0.2f, 0.8f, 0.3f) : Color(0.8f, 0.5f, 0.2f));
        drawBitmapString("[" + player_.getHwAccelName() + "]", 20, 24);

        // Info bar
        setColor(0.8f, 0.8f, 0.85f);
        string info = format("{}x{} | {:.1f}s / {:.1f}s | {} | Speed: {:.2f}x",
            (int)vw, (int)vh,
            player_.getCurrentTime(), player_.getDuration(),
            player_.isPlaying() ? "Playing" : (player_.isPaused() ? "Paused" : "Stopped"),
            player_.getSpeed());
        drawBitmapString(info, 20, getWindowHeight() - 50);

        setColor(0.5f, 0.5f, 0.55f);
        drawBitmapString("Space: Play/Pause | R: Restart | L: Loop | []: Speed | 0-9: Seek | Arrows: Seek",
                         20, getWindowHeight() - 30);
    } else {
        setColor(0.6f, 0.6f, 0.65f);
        drawBitmapString(statusText_, 20, getWindowHeight() / 2.f);
    }
}

void tcApp::keyPressed(int key) {
    if (!player_.isLoaded()) return;

    switch (key) {
        case ' ':
            if (player_.isPlaying()) {
                player_.setPaused(!player_.isPaused());
            } else {
                player_.play();
            }
            break;
        case 'r': case 'R':
            player_.stop();
            player_.play();
            break;
        case 'l': case 'L':
            player_.setLoop(!player_.isLoop());
            logNotice("tcApp") << "Loop: " << (player_.isLoop() ? "ON" : "OFF");
            break;
        case KEY_LEFT:
            player_.setPosition(player_.getPosition() - 0.05f);
            break;
        case KEY_RIGHT:
            player_.setPosition(player_.getPosition() + 0.05f);
            break;
        case KEY_UP:
            player_.setVolume(player_.getVolume() + 0.1f);
            break;
        case KEY_DOWN:
            player_.setVolume(player_.getVolume() - 0.1f);
            break;
        case '[':
            player_.setSpeed(player_.getSpeed() - 0.25f);
            break;
        case ']':
            player_.setSpeed(player_.getSpeed() + 0.25f);
            break;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            player_.setPosition((key - '0') * 0.1f);
            break;
        case '0':
            player_.setPosition(0.f);
            break;
    }
}

void tcApp::filesDropped(const vector<string>& files) {
    if (!files.empty()) {
        loadVideo(files[0]);
    }
}
