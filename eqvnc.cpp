/*
 * Copyright (C) 2013
 * Martin Lambers <marlam@marlam.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>

#include <cstdio>
#include <cstring>
#include <cmath>

#include <GL/glew.h>
#include <eq/eq.h>
#include <rfb/rfbclient.h>

/* Types and global variables */

typedef struct {
    int x, y, w, h;
} rectangle_t;

typedef enum {
    screen_canvas,
    screen_wall,
    screen_cylinder
} screen_t;

static rfbClient* vnc_client = NULL;

/* A few little helpers */

static float deg_to_rad(float deg)
{
    return static_cast<float>(M_PI) / 180.0f * deg;
}

static float rad_to_deg(float rad)
{
    return 180.0f / static_cast<float>(M_PI) * rad;
}

static float dot(const float v[3], const float w[3])
{
    return v[0] * w[0] + v[1] * w[1] + v[2] * w[2];
}

static void cross(const float v[3], const float w[3], float c[3])
{
    c[0] = v[1] * w[2] - v[2] * w[1];
    c[1] = v[2] * w[0] - v[0] * w[2];
    c[2] = v[0] * w[1] - v[1] * w[0];
}

static float length(const float v[3])
{
    return std::sqrt(dot(v, v));
}

/* Equalizer code */

class eq_init_data : public co::Object
{
public:
    eq::uint128_t frame_data_id;
    bool view_only;
    screen_t screen;
    float wall[9];      // bottom left, bottom right, top left
    float cylinder[10]; // cylinder center, cylinder up vector, radius,
                        // azimuth center, azimuth range, polar range
    float head_matrix[16];

    eq_init_data()
    {
    }

protected:
    virtual ChangeType getChangeType() const
    {
        return co::Object::STATIC;
    }

    virtual void getInstanceData(co::DataOStream& os)
    {
        os << frame_data_id;
        os << view_only;
        os << static_cast<int>(screen);
#if EQ_VERSION_GE(1,6,0)
        os << co::Array<float>(wall, 9);
        os << co::Array<float>(cylinder, 10);
        os << co::Array<float>(head_matrix, 16);
#else
        os.write(wall, 9 * sizeof(float));
        os.write(cylinder, 10 * sizeof(float));
        os.write(head_matrix, 16 * sizeof(float));
#endif
    }

    virtual void applyInstanceData(co::DataIStream& is)
    {
        int x;
        is >> frame_data_id;
        is >> view_only;
        is >> x; screen = static_cast<screen_t>(x);
#if EQ_VERSION_GE(1,6,0)
        is >> co::Array<float>(wall, 9);
        is >> co::Array<float>(cylinder, 10);
        is >> co::Array<float>(head_matrix, 16);
#else
        is.read(wall, 9 * sizeof(float));
        is.read(cylinder, 10 * sizeof(float));
        is.read(head_matrix, 16 * sizeof(float));
#endif
    }
};

class eq_frame_data : public co::Object
{
public:
    int vnc_width;
    int vnc_height;
    float canvas[6]; // width, height, and relative rectangle x,y,w,h
    std::vector<unsigned int> vnc_framebuffer; // 32 bit BGRA pixels
    std::vector<rectangle_t> vnc_dirty_rectangles;

    eq_frame_data() : vnc_width(0), vnc_height(0)
    {
    }

protected:
    virtual ChangeType getChangeType() const
    {
        return co::Object::INSTANCE;
    }

    virtual void getInstanceData(co::DataOStream& os)
    {
        os << vnc_width << vnc_height;
#if EQ_VERSION_GE(1,6,0)
        os << co::Array<float>(canvas, 6);
#else
        os.write(canvas, 6 * sizeof(float));
#endif
        size_t n = vnc_dirty_rectangles.size();
        os << n;
        for (size_t i = 0; i < n; i++) {
            rectangle_t r = vnc_dirty_rectangles[i];
            os << r.x << r.y << r.w << r.h;
            for (int y = r.y; y < r.y + r.h; y++) {
                for (int x = r.x; x < r.x + r.w; x++) {
                    os << vnc_framebuffer[y * vnc_width + x];
                }
            }
        }
    }

    virtual void applyInstanceData(co::DataIStream& is)
    {
        int w, h;
        is >> w >> h;
        if (w != vnc_width || h != vnc_height)
            vnc_framebuffer.resize(w * h);
        vnc_width = w;
        vnc_height = h;
#if EQ_VERSION_GE(1,6,0)
        is >> co::Array<float>(canvas, 6);
#else
        is.read(canvas, 6 * sizeof(float));
#endif
        size_t n;
        is >> n;
        for (size_t i = 0; i < n; i++) {
            rectangle_t r;
            is >> r.x >> r.y >> r.w >> r.h;
            for (int y = r.y; y < r.y + r.h; y++) {
                for (int x = r.x; x < r.x + r.w; x++) {
                    is >> vnc_framebuffer[y * vnc_width + x];
                }
            }
        }
    }
};

static rfbKeySym eqkey_to_rfbkey(eq::KeyEvent key)
{
    // Both Equalizer and libvncclient transmit ASCII characters as is.
    // We just translate the special constants defined by Equalizer and
    // assume the rest is ASCII.
    // XXX: This is wrong. Furthermore, libvncclient knows a lot more
    // special keys than Equalizer, and Equalizer does not give us modifier
    // state, so keyboard interaction is currently very limited.
    rfbKeySym k;
    switch (key.key) {
    case eq::KC_ESCAPE:    k = XK_Escape;    break;
    case eq::KC_BACKSPACE: k = XK_BackSpace; break;
    case eq::KC_RETURN:    k = XK_Return;    break;
    case eq::KC_TAB:       k = XK_Tab;       break;
    case eq::KC_HOME:      k = XK_Home;      break;
    case eq::KC_LEFT:      k = XK_Left;      break;
    case eq::KC_UP:        k = XK_Up;        break;
    case eq::KC_RIGHT:     k = XK_Right;     break;
    case eq::KC_DOWN:      k = XK_Down;      break;
    case eq::KC_PAGE_UP:   k = XK_Page_Up;   break;
    case eq::KC_PAGE_DOWN: k = XK_Page_Down; break;
    case eq::KC_END:       k = XK_End;       break;
    case eq::KC_F1:        k = XK_F1;        break;
    case eq::KC_F2:        k = XK_F2;        break;
    case eq::KC_F3:        k = XK_F3;        break;
    case eq::KC_F4:        k = XK_F4;        break;
    case eq::KC_F5:        k = XK_F5;        break;
    case eq::KC_F6:        k = XK_F6;        break;
    case eq::KC_F7:        k = XK_F7;        break;
    case eq::KC_F8:        k = XK_F8;        break;
    case eq::KC_F9:        k = XK_F9;        break;
    case eq::KC_F10:       k = XK_F10;       break;
    case eq::KC_F11:       k = XK_F11;       break;
    case eq::KC_F12:       k = XK_F12;       break;
    case eq::KC_F13:       k = XK_F13;       break;
    case eq::KC_F14:       k = XK_F14;       break;
    case eq::KC_F15:       k = XK_F15;       break;
    case eq::KC_F16:       k = XK_F16;       break;
    case eq::KC_F17:       k = XK_F17;       break;
    case eq::KC_F18:       k = XK_F18;       break;
    case eq::KC_F19:       k = XK_F19;       break;
    case eq::KC_F20:       k = XK_F20;       break;
    case eq::KC_F21:       k = XK_F21;       break;
    case eq::KC_F22:       k = XK_F22;       break;
    case eq::KC_F23:       k = XK_F23;       break;
    case eq::KC_F24:       k = XK_F24;       break;
    default:               k = key.key;      break;
    }
    return k;
}

static void eqptr_to_rfbptr(const eq::PointerEvent& e,
        const eq::PixelViewport& pvp, const eq::Viewport& vp,
        const float canvas[6], int vnc_width, int vnc_height,
        int& x, int& y, int& buttons)
{
    // Event position relative to channel
    float event_channel_x = e.x / static_cast<float>(pvp.w);
    float event_channel_y = e.y / static_cast<float>(pvp.h);
    // Event position relative to canvas
    float event_canvas_x = vp.x + event_channel_x * vp.w;
    float event_canvas_y = vp.y + ((1.0f - event_channel_y) * vp.h);
    event_canvas_y = 1.0f - event_canvas_y;
    // Event position relative to canvas drawing area
    float event_canvas_area_x = (event_canvas_x - canvas[2]) / canvas[4];
    float event_canvas_area_y = (event_canvas_y - canvas[3]) / canvas[5];
    // Event pixel position in VNC
    float event_x = event_canvas_area_x * vnc_width;
    float event_y = event_canvas_area_y * vnc_height;
    // Integer event position in VNC
    x = event_x;
    if (x < 0)
        x = 0;
    if (x > vnc_width - 1)
        x = vnc_width - 1;
    y = event_y;
    if (y < 0)
        y = 0;
    if (y > vnc_height - 1)
        y = vnc_height - 1;
    // Buttons and wheel
    buttons = 0;
    if (e.buttons & eq::PTR_BUTTON1)
        buttons |= rfbButton1Mask;
    if (e.buttons & eq::PTR_BUTTON2)
        buttons |= rfbButton2Mask;
    if (e.buttons & eq::PTR_BUTTON3)
        buttons |= rfbButton3Mask;
    if (e.xAxis > 0 || e.yAxis > 0)
        buttons |= rfbWheelUpMask;
    if (e.xAxis < 0 || e.yAxis < 0)
        buttons |= rfbWheelDownMask;
    //fprintf(stderr, "MOUSE: (%d,%d) in (%dx%d) channel to (%d,%d) in (%dx%d) VNC with buttons %d\n",
    //        e.x, e.y, pvp.w, pvp.h, x, y, vnc_width, vnc_height, buttons);
}

class eq_config : public eq::Config
{
public:
    eq_init_data init_data;
    eq_frame_data frame_data;

    eq_config(eq::ServerPtr parent) : eq::Config(parent)
    {
    }

    bool init(bool view_only, screen_t screen,
            const float screen_def[10], const float head_matrix[16])
    {
        registerObject(&frame_data);
        init_data.frame_data_id = frame_data.getID();
        init_data.view_only = view_only;
        init_data.screen = screen;
        if (screen == screen_canvas) {
            if (getCanvases().size() == 0) {
                fprintf(stderr, "The Equalizer configuration does not define a canvas\n");
                return false;
            }
        } else if (screen == screen_wall) {
            for (int i = 0; i < 9; i++)
                init_data.wall[i] = screen_def[i];
        } else {
            for (int i = 0; i < 10; i++)
                init_data.cylinder[i] = screen_def[i];
        }
        for (int i = 0; i < 16; i++)
            init_data.head_matrix[i] = head_matrix[i];
        registerObject(&init_data);
        return eq::Config::init(init_data.getID());
    }

    virtual bool exit()
    {
        bool ret = eq::Config::exit();
        deregisterObject(&init_data);
        deregisterObject(&frame_data);
        return ret;
    }

    virtual uint32_t startFrame()
    {
        if (init_data.screen == screen_canvas) {
            frame_data.canvas[0] = getCanvases()[0]->getWall().getWidth();
            frame_data.canvas[1] = getCanvases()[0]->getWall().getHeight();
            float ar = frame_data.vnc_width / static_cast<float>(frame_data.vnc_height);
            float canvas_ar = frame_data.canvas[0] / frame_data.canvas[1];
            if (ar >= canvas_ar) {
                frame_data.canvas[4] = 1.0f;
                frame_data.canvas[5] = canvas_ar / ar;
            } else {
                frame_data.canvas[4] = ar / canvas_ar;
                frame_data.canvas[5] = 1.0f;
            }
            frame_data.canvas[2] = (1.0f - frame_data.canvas[4]) / 2.0f;
            frame_data.canvas[3] = (1.0f - frame_data.canvas[5]) / 2.0f;
        }
        eq::Matrix4f hm;
        for (int i = 0; i < 16; i++)
            hm.array[i] = init_data.head_matrix[i];
        getObservers().at(0)->setHeadMatrix(hm);
        const eq::uint128_t version = frame_data.commit();
        return eq::Config::startFrame(version);
    }

    virtual bool handleEvent(const eq::ConfigEvent* event)
    {
        if (eq::Config::handleEvent(event))
            return true;
        if (init_data.view_only)
            return false;
        if (!vnc_client)
            return false;
        if (event->data.type == eq::Event::KEY_PRESS) {
            SendKeyEvent(vnc_client, eqkey_to_rfbkey(event->data.keyPress), TRUE);
            return true;
        } else if (event->data.type == eq::Event::KEY_RELEASE) {
            SendKeyEvent(vnc_client, eqkey_to_rfbkey(event->data.keyRelease), FALSE);
            return true;
        } else if (init_data.screen == screen_canvas) {
            if (event->data.type == eq::Event::CHANNEL_POINTER_MOTION) {
                int vnc_x, vnc_y, vnc_buttons;
                eqptr_to_rfbptr(event->data.pointerMotion, event->data.context.pvp, event->data.context.vp,
                        frame_data.canvas, frame_data.vnc_width, frame_data.vnc_height,
                        vnc_x, vnc_y, vnc_buttons);
                SendPointerEvent(vnc_client, vnc_x, vnc_y, vnc_buttons);
            } else if (event->data.type == eq::Event::CHANNEL_POINTER_BUTTON_PRESS) {
                int vnc_x, vnc_y, vnc_buttons;
                eqptr_to_rfbptr(event->data.pointerButtonPress, event->data.context.pvp, event->data.context.vp,
                        frame_data.canvas, frame_data.vnc_width, frame_data.vnc_height,
                        vnc_x, vnc_y, vnc_buttons);
                SendPointerEvent(vnc_client, vnc_x, vnc_y, vnc_buttons);
            } else if (event->data.type == eq::Event::CHANNEL_POINTER_BUTTON_RELEASE) {
                int vnc_x, vnc_y, vnc_buttons;
                eqptr_to_rfbptr(event->data.pointerButtonRelease, event->data.context.pvp, event->data.context.vp,
                        frame_data.canvas, frame_data.vnc_width, frame_data.vnc_height,
                        vnc_x, vnc_y, vnc_buttons);
                SendPointerEvent(vnc_client, vnc_x, vnc_y, vnc_buttons);
            } else if (event->data.type ==
#if EQ_VERSION_GE(1,6,0)
                    eq::Event::CHANNEL_POINTER_WHEEL
#else
                    eq::Event::WINDOW_POINTER_WHEEL
#endif
                    ) {
                int vnc_x, vnc_y, vnc_buttons;
                eqptr_to_rfbptr(event->data.pointerWheel, event->data.context.pvp, event->data.context.vp,
                        frame_data.canvas, frame_data.vnc_width, frame_data.vnc_height,
                        vnc_x, vnc_y, vnc_buttons);
                SendPointerEvent(vnc_client, vnc_x, vnc_y, vnc_buttons);
            }
        }
        return false;
    }
};

class eq_node : public eq::Node
{
public:
    eq_init_data init_data;

    eq_node(eq::Config* parent) : eq::Node(parent)
    {
    }

protected:
    virtual bool configInit(const eq::uint128_t& init_id)
    {
        if (!eq::Node::configInit(init_id)) {
            return false;
        }
        eq_config* config = static_cast<eq_config*>(getConfig());
        if (!config->mapObject(&init_data, init_id)) {
            return false;
        }
        return true;
    }

    virtual bool configExit()
    {
        eq::Config* config = getConfig();
        config->unmapObject(&init_data);
        return eq::Node::configExit();
    }
};

class eq_pipe : public eq::Pipe
{
public:
    eq_frame_data frame_data;

    eq_pipe(eq::Node* parent) : eq::Pipe(parent)
    {
    }

protected:
    virtual bool configInit(const eq::uint128_t& init_id)
    {
        if (!eq::Pipe::configInit(init_id))
            return false;
        eq_config* config = static_cast<eq_config*>(getConfig());
        eq_node* node = static_cast<eq_node*>(getNode());
        if (!config->mapObject(&frame_data, node->init_data.frame_data_id))
            return false;
        return true;
    }

    virtual bool configExit()
    {
        eq::Config* config = getConfig();
        config->unmapObject(&frame_data);
        return eq::Pipe::configExit();
    }

    virtual void frameStart(const eq::uint128_t& frame_id, const uint32_t frame_number)
    {
        frame_data.sync(frame_id);
        eq::Pipe::frameStart(frame_id, frame_number);
    }
};

class eq_window : public eq::Window
{
public:
    GLuint tex;
    int tex_w, tex_h;
    bool tex_updated;

    eq_window(eq::Pipe* parent) : eq::Window(parent),
        tex(0), tex_updated(false)
    {
    }

protected:
    virtual void frameStart(const eq::uint128_t& frame_id, const uint32_t frame_number)
    {
        const eq_pipe* pipe = static_cast<eq_pipe*>(getPipe());
        const eq_frame_data& frame_data = pipe->frame_data;
        if (tex == 0 || tex_w != frame_data.vnc_width || tex_h != frame_data.vnc_height) {
            if (tex == 0)
                glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_data.vnc_width, frame_data.vnc_height, 0,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
            tex_w = frame_data.vnc_width;
            tex_h = frame_data.vnc_height;
            tex_updated = false;
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        if (!tex_updated) {
            // TODO: optimize this. Use a PBO, and upload only the bounding rectangle
            // of all dirty rectangles to the texture. Or upload all dirty rectangles
            // individually? Not sure which is faster in typical scenarios.
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_data.vnc_width, frame_data.vnc_height,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &(frame_data.vnc_framebuffer[0]));
            tex_updated = true;
        }
        eq::Window::frameStart(frame_id, frame_number);
    }

    virtual void frameFinish(const eq::uint128_t& frame_id, const uint32_t frame_number)
    {
        tex_updated = false;
        eq::Window::frameFinish(frame_id, frame_number);
    }
};

class eq_channel : public eq::Channel
{
public:
    eq_channel(eq::Window *parent) : eq::Channel(parent)
    {
    }

protected:
    virtual void frameDraw(const eq::uint128_t& frame_id)
    {
        eq::Channel::frameDraw(frame_id);
        const eq_node* node = static_cast<eq_node*>(getNode());
        const eq_pipe* pipe = static_cast<eq_pipe*>(getPipe());
        const eq_window* window = static_cast<eq_window*>(getWindow());
        const eq_init_data& init_data = node->init_data;
        const eq_frame_data& frame_data = pipe->frame_data;
        // Setup GL state
        glDisable(GL_LIGHTING);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, window->tex);
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        if (node->init_data.screen == screen_canvas) {
            // Determine the quad for this channel's area on the canvas
            const eq::Viewport &canvas_channel_area = getViewport();
            float quad_x = ((frame_data.canvas[2] - canvas_channel_area.x) / canvas_channel_area.w - 0.5f) * 2.0f;
            float quad_y = ((frame_data.canvas[3] - canvas_channel_area.y) / canvas_channel_area.h - 0.5f) * 2.0f;
            float quad_w = 2.0f * frame_data.canvas[4] / canvas_channel_area.w;
            float quad_h = 2.0f * frame_data.canvas[5] / canvas_channel_area.h;
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 1.0f);
            glVertex2f(quad_x, quad_y);
            glTexCoord2f(1.0f, 1.0f);
            glVertex2f(quad_x + quad_w, quad_y);
            glTexCoord2f(1.0f, 0.0f);
            glVertex2f(quad_x + quad_w, quad_y + quad_h);
            glTexCoord2f(0.0f, 0.0f);
            glVertex2f(quad_x, quad_y + quad_h);
            glEnd();
        } else if (node->init_data.screen == screen_wall) {
            float bl[3] = { init_data.wall[0], init_data.wall[1], init_data.wall[2] };
            float br[3] = { init_data.wall[3], init_data.wall[4], init_data.wall[5] };
            float tl[3] = { init_data.wall[6], init_data.wall[7], init_data.wall[8] };
            float tr[3] = { br[0] + (tl[0] - bl[0]), br[1] + (tl[1] - bl[1]), br[2] + (tl[2] - bl[2]) };
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 1.0f);
            glVertex3fv(bl);
            glTexCoord2f(1.0f, 1.0f);
            glVertex3fv(br);
            glTexCoord2f(1.0f, 0.0f);
            glVertex3fv(tr);
            glTexCoord2f(0.0f, 0.0f);
            glVertex3fv(tl);
            glEnd();
        } else { // node->init_data.screen == screen_cylinder 
            float center[3] = { init_data.cylinder[0], init_data.cylinder[1], init_data.cylinder[2] };
            float up[3] = { init_data.cylinder[3], init_data.cylinder[4], init_data.cylinder[5] };
            float radius = init_data.cylinder[6];
            float phi_center = init_data.cylinder[7];
            float phi_range = init_data.cylinder[8];
            float theta_range = init_data.cylinder[9];
            float py = radius * std::tan(theta_range / 2.0f);
            float default_up[3] = { 0.0f, 1.0f, 0.0f };
            float rot_axis[3];
            float rot_angle;
            cross(default_up, up, rot_axis);
            rot_angle = std::acos(dot(default_up, up) / std::sqrt(dot(default_up, default_up) * dot(up, up)));
            glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
            glRotatef(rad_to_deg(rot_angle), rot_axis[0], rot_axis[1], rot_axis[2]);
            glTranslatef(center[0], center[1], center[2]);
            glBegin(GL_TRIANGLE_STRIP);
            const int N = 1000;
            for (int x = 0; x <= N; x++) {
                float xf = x / static_cast<float>(N);
                float phi = phi_center + (xf - 0.5f) * phi_range;
                float px = radius * std::cos(phi);
                float pz = radius * std::sin(phi);
                glTexCoord2f(xf, 0.0f);
                glVertex3f(px, py, pz);
                glTexCoord2f(xf, 1.0f);
                glVertex3f(px, -py, pz);
            }
            glEnd();
        }
    }
};

class eq_node_factory : public eq::NodeFactory
{
public:
    virtual eq::Config* createConfig(eq::ServerPtr parent) { return new eq_config(parent); }
    virtual eq::Node* createNode(eq::Config* parent) { return new eq_node(parent); }
    virtual eq::Pipe* createPipe(eq::Node* parent) { return new eq_pipe(parent); }
    virtual eq::Window* createWindow(eq::Pipe* parent) { return new eq_window(parent); }
    virtual eq::Channel* createChannel(eq::Window* parent) { return new eq_channel(parent); }
};

eq_config* appnode_eq_config = NULL;


/* libvncclient callbacks */

static rfbBool vnc_resize(rfbClient* client)
{
    //fprintf(stderr, "RESIZE to %dx%d\n", client->width, client->height);
    eq_frame_data& frame_data = appnode_eq_config->frame_data;
    frame_data.vnc_width = client->width;
    frame_data.vnc_height = client->height;
    frame_data.vnc_framebuffer.resize(client->width * client->height);
    frame_data.vnc_dirty_rectangles.clear();
    rectangle_t r = { 0, 0, client->width, client->height };
    frame_data.vnc_dirty_rectangles.push_back(r);
    client->updateRect.x = 0;
    client->updateRect.y = 0;
    client->updateRect.w = client->width;
    client->updateRect.h = client->height;
    client->frameBuffer = reinterpret_cast<uint8_t*>(&(frame_data.vnc_framebuffer[0]));
    client->format.bitsPerPixel = 32;
    client->format.depth = 8;
    client->format.redMax = 255;
    client->format.greenMax = 255;
    client->format.blueMax = 255;
    client->format.redShift = 16;
    client->format.greenShift = 8;
    client->format.blueShift = 0;
    SetFormatAndEncodings(client);
    return TRUE;
}

static void vnc_update(rfbClient* /* client */, int x, int y, int w, int h)
{
    //fprintf(stderr, "UPDATING %dx%d rectangle at %d,%d\n", w, h, x, y);
    eq_frame_data& frame_data = appnode_eq_config->frame_data;
    rectangle_t r = { x, y, w, h };
    frame_data.vnc_dirty_rectangles.push_back(r);
}


/* main() */

static bool get_screen(const char* opt, screen_t& screen, float screen_def[10])
{
    if (std::strcmp(opt, "canvas") == 0) {
        screen = screen_canvas;
        return true;
    } else if (std::sscanf(opt, "wall,%f,%f,%f,%f,%f,%f,%f,%f,%f",
                screen_def + 0, screen_def + 1, screen_def + 2,
                screen_def + 3, screen_def + 4, screen_def + 5,
                screen_def + 6, screen_def + 7, screen_def + 8) == 9) {
        screen = screen_wall;
        return true;
    } else if (std::sscanf(opt, "cylinder,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
                screen_def + 0, screen_def + 1, screen_def + 2,
                screen_def + 3, screen_def + 4, screen_def + 5,
                screen_def + 6, screen_def + 7, screen_def + 8,
                screen_def + 9) == 10) {
        screen = screen_cylinder;
        screen_def[7] = deg_to_rad(screen_def[7]);
        screen_def[8] = deg_to_rad(screen_def[8]);
        screen_def[9] = deg_to_rad(screen_def[9]);
        return true;
    }
    return false;
}

static bool get_lookat(const char* opt, float head_matrix[16])
{
    float eye[3], center[3], up[3];
    if (std::sscanf(opt, "%f,%f,%f,%f,%f,%f,%f,%f,%f",
                eye + 0, eye + 1, eye + 2,
                center + 0, center + 1, center + 2,
                up + 0, up + 1, up + 2) == 9) {
        float view[3], view_len;
        float s[3], s_len;
        float u[3];
        view[0] = center[0] - eye[0];
        view[1] = center[1] - eye[1];
        view[2] = center[2] - eye[2];
        view_len = length(view);
        view[0] /= view_len;
        view[1] /= view_len;
        view[2] /= view_len;
        cross(view, up, s);
        s_len = length(s);
        s[0] /= s_len;
        s[1] /= s_len;
        s[2] /= s_len;
        cross(s, view, u);
        head_matrix[0 * 4 + 0] = s[0];
        head_matrix[0 * 4 + 1] = u[0];
        head_matrix[0 * 4 + 2] = -view[0];
        head_matrix[0 * 4 + 3] = 0.0f;
        head_matrix[1 * 4 + 0] = s[1];
        head_matrix[1 * 4 + 1] = u[1];
        head_matrix[1 * 4 + 2] = -view[1];
        head_matrix[1 * 4 + 3] = 0.0f;
        head_matrix[2 * 4 + 0] = s[2];
        head_matrix[2 * 4 + 1] = u[2];
        head_matrix[2 * 4 + 2] = -view[2];
        head_matrix[2 * 4 + 3] = 0.0f;
        head_matrix[3 * 4 + 0] = eye[0];
        head_matrix[3 * 4 + 1] = eye[1];
        head_matrix[3 * 4 + 2] = eye[2];
        head_matrix[3 * 4 + 3] = 1.0f;
        return true;
    }
    return false;
}

int main(int argc, char* argv[])
{
    /* Initialize Equalizer */
    eq_node_factory enf;
    if (!eq::init(argc, argv, &enf)) {
        fprintf(stderr, "Equalizer initialization failed\n");
        return 1;
    }
    appnode_eq_config = static_cast<eq_config*>(eq::getConfig(argc, argv));
    // The following code is only executed on the application node because
    // eq::getConfig() does not return on other nodes.
    if (!appnode_eq_config) {
        fprintf(stderr, "Cannot get Equalizer configuration\n");
        return 1;
    }
    // Get command line options
    bool view_only = false;
    screen_t screen = screen_canvas;
    float screen_def[10];
    float head_matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--view-only") == 0) {
            view_only = true;
        } else if (std::strcmp(argv[i], "--screen") == 0) {
            if (!get_screen(argv[i + 1], screen, screen_def)) {
                fprintf(stderr, "Invalid argument to --screen\n");
                return 1;
            }
            i++;
        } else if (std::strncmp(argv[i], "--screen=", 9) == 0) {
            if (!get_screen(argv[i] + 9, screen, screen_def)) {
                fprintf(stderr, "Invalid argument to --screen\n");
                return 1;
            }
        } else if (std::strcmp(argv[i], "--lookat") == 0) {
            if (!get_lookat(argv[i + 1], head_matrix)) {
                fprintf(stderr, "Invalid argument to --lookat\n");
                return 1;
            }
            i++;
        } else if (std::strncmp(argv[i], "--lookat=", 9) == 0) {
            if (!get_lookat(argv[i] + 9, head_matrix)) {
                fprintf(stderr, "Invalid argument to --lookat\n");
                return 1;
            }
        }
    }
    if (!appnode_eq_config->init(view_only, screen, screen_def, head_matrix)) {
        fprintf(stderr, "Cannot initialize Equalizer configuration\n");
        return 1;
    }

    /* Initialize the VNC client */
    vnc_client = rfbGetClient(8, 3, 4); // 32 bpp
    vnc_client->MallocFrameBuffer = vnc_resize;
    vnc_client->canHandleNewFBSize = TRUE;
    vnc_client->GotFrameBufferUpdate = vnc_update;
    vnc_client->listenPort = LISTEN_PORT_OFFSET;
    vnc_client->listen6Port = LISTEN_PORT_OFFSET;
    if (!rfbInitClient(vnc_client, &argc, argv)) {
        fprintf(stderr, "Cannot initialize VNC client\n");
        return 1;
    }

    /* Run the viewer */
    while (appnode_eq_config->isRunning()) {
        int i = WaitForMessage(vnc_client, 10);
        if (i > 0 && !HandleRFBServerMessage(vnc_client)) {
            fprintf(stderr, "VNC event handling failed\n");
            return 1;
        }
        appnode_eq_config->startFrame();
        appnode_eq_config->finishFrame();
        eq_frame_data& frame_data = appnode_eq_config->frame_data;
        frame_data.vnc_dirty_rectangles.clear();
    }

    return 0;
}
