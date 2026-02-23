//
// Controls:
//   - Mouse wheel: zoom (or +/- keys if wheel not supported)
//   - Left drag: pan
//   - L: toggle leaf-only labels
//   - V: toggle layout orientation (Horizontal <-> Vertical)
//   - F: toggle fullscreen
//   - C: toggle curved Bezier links vs straight links
//   - ESC: quit

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>

#include "tinyxml2.h"
#include <GL/glut.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------- Config ----------------------------

static bool  LABEL_LEAVES_ONLY  = false;   // press 'L' to toggle

// Linear layout spacing
static float LEVEL_GAP          = 65.0f;   // distance per depth level
static float SIBLING_GAP        = 40.0f;   // spacing between adjacent leaves

enum class LayoutOrientation { Horizontal, Vertical };
static LayoutOrientation g_layout = LayoutOrientation::Vertical; // press 'V' to toggle

// Links
static bool  LINKS_CURVED       = true;    // press 'C' to toggle
static int   BEZIER_SAMPLES     = 28;      // segments per edge curve (if LINKS_CURVED)

// Less curvy control: lower = straighter
static float BEZIER_BEND_FACTOR = 0.75f;

// Stroke text (rotatable)
static void* LABEL_STROKE_FONT  = GLUT_STROKE_ROMAN;
static float LABEL_STROKE_SCALE = 0.060f; // world scaling; tune for your data

// Label offset from node circle (world units)
static float LABEL_VERT_OFFSET  = 5.0f;   // in vert mode, distance away from node
static float LABEL_HORZ_OFFSET  = 10.0f;  // in horz mode, distance away from node

// Endpoint circles
static float ENDPOINT_RADIUS    = 3.0f;  // world units
static int   CIRCLE_SEGS        = 18;

// Base view height in world units (used for ortho & pixel->world conversion)
static float BASE_HALF_H        = 400.0f;

struct Node {
    std::string id;
    std::string text;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    int depth = 0;
    int leafCount = 0;

    float angle = 0.0f;
    float radius = 0.0f;
    float x = 0.0f, y = 0.0f;
};

static int g_autoId = 1;
static std::unique_ptr<Node> g_root;

static int   g_winW = 1000;
static int   g_winH = 900;

static float g_zoom = 1.0f;
static float g_panX = 0.0f, g_panY = 0.0f;

static bool  g_dragging = false;
static int   g_lastMouseX = 0, g_lastMouseY = 0;

static bool g_fullscreen = false;
static int  g_winX = 100, g_winY = 100;               // restore position
static int  g_winW_prev = 1000, g_winH_prev = 900;    // restore size

static float g_rotDeg = 0.0f;

static void drawFilledCircle(float cx, float cy, float r, int segs) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segs; ++i) {
        float a = (2.0f * float(M_PI)) * (float(i) / float(segs));
        glVertex2f(cx + std::cos(a) * r, cy + std::sin(a) * r);
    }
    glEnd();
}

enum class TextAlign { Start, Center, End };

static float strokeTextWidth(void* font, const std::string& s) {
    float w = 0.0f;
    for (unsigned char c : s) w += float(glutStrokeWidth(font, c));
    return w;
}

static void drawStrokeStringRotatedAligned(float x, float y,
                                           float angleDeg,
                                           float scale,
                                           void* font,
                                           const std::string& s,
                                           TextAlign align)
{
    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    glRotatef(angleDeg, 0.0f, 0.0f, 1.0f);
    glScalef(scale, scale, 1.0f);

    float w = strokeTextWidth(font, s);
    if (align == TextAlign::Center) glTranslatef(-0.5f * w, 0.0f, 0.0f);
    else if (align == TextAlign::End) glTranslatef(-w, 0.0f, 0.0f);

    for (unsigned char c : s) glutStrokeCharacter(font, c);
    glPopMatrix();
}

static std::string getAttr(tinyxml2::XMLElement* el, const char* name) {
    const char* v = el->Attribute(name);
    return v ? std::string(v) : std::string();
}

static std::unique_ptr<Node> parseNode(tinyxml2::XMLElement* xmlNode, Node* parent) {
    auto n = std::make_unique<Node>();
    n->parent = parent;

    n->text = getAttr(xmlNode, "TEXT");
    n->id   = getAttr(xmlNode, "ID");

    if (n->id.empty()) n->id = "auto_" + std::to_string(g_autoId++);
    if (n->text.empty()) n->text = n->id;

    for (tinyxml2::XMLElement* c = xmlNode->FirstChildElement("node"); c; c = c->NextSiblingElement("node")) {
        auto child = parseNode(c, n.get());
        n->children.insert(n->children.begin(), std::move(child));
    }
    return n;
}

static std::unique_ptr<Node> loadFreeMind(const char* path) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path) != tinyxml2::XML_SUCCESS) {
        std::fprintf(stderr, "Failed to load %s\n", path);
        return nullptr;
    }

    auto* mapEl = doc.FirstChildElement("map");
    if (!mapEl) { std::fprintf(stderr, "No <map> element.\n"); return nullptr; }

    auto* rootEl = mapEl->FirstChildElement("node");
    if (!rootEl) { std::fprintf(stderr, "No root <node> element.\n"); return nullptr; }

    return parseNode(rootEl, nullptr);
}

// ---------------------------- Layout ----------------------------

static int computeDepthAndLeaves(Node* n, int depth) {
    n->depth = depth;
    if (n->children.empty()) { n->leafCount = 1; return 1; }

    int sum = 0;
    for (auto& ch : n->children) sum += computeDepthAndLeaves(ch.get(), depth + 1);
    n->leafCount = std::max(1, sum);
    return n->leafCount;
}

static void assignLinearPositions(Node* n, float& cursor, LayoutOrientation orient) {
    if (n->children.empty()) {
        if (orient == LayoutOrientation::Horizontal) {
            n->x = float(n->depth) * LEVEL_GAP;
            n->y = cursor;
        } else {
            n->x = cursor;
            n->y = -float(n->depth) * LEVEL_GAP;
        }
        cursor += SIBLING_GAP;
        return;
    }

    float firstCoord = 0.0f;
    float lastCoord  = 0.0f;
    bool first = true;

    for (auto& ch : n->children) {
        assignLinearPositions(ch.get(), cursor, orient);
        float childCoord = (orient == LayoutOrientation::Horizontal) ? ch->y : ch->x;
        if (first) { firstCoord = lastCoord = childCoord; first = false; }
        else { lastCoord = childCoord; }
    }

    float mid = 0.5f * (firstCoord + lastCoord);
    if (orient == LayoutOrientation::Horizontal) {
        n->x = float(n->depth) * LEVEL_GAP;
        n->y = mid;
    } else {
        n->x = mid;
        n->y = -float(n->depth) * LEVEL_GAP;
    }
}

static void assignAnglesFromLinks(Node* n) {
    if (n->parent) {
        float dx = n->x - n->parent->x;
        float dy = n->y - n->parent->y;
        n->angle = std::atan2(dy, dx);
        n->radius = std::sqrt(dx*dx + dy*dy);
    } else {
        n->angle = 0.0f;
        n->radius = 0.0f;
    }
    for (auto& ch : n->children) assignAnglesFromLinks(ch.get());
}

static void computeLayout() {
    computeDepthAndLeaves(g_root.get(), 0);

    float cursor = 0.0f;
    assignLinearPositions(g_root.get(), cursor, g_layout);

    // Recenter around origin
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    std::vector<Node*> stack;
    stack.push_back(g_root.get());
    while (!stack.empty()) {
        Node* n = stack.back(); stack.pop_back();
        minX = std::min(minX, n->x); maxX = std::max(maxX, n->x);
        minY = std::min(minY, n->y); maxY = std::max(maxY, n->y);
        for (auto& ch : n->children) stack.push_back(ch.get());
    }
    float cx = 0.5f * (minX + maxX);
    float cy = 0.5f * (minY + maxY);

    stack.push_back(g_root.get());
    while (!stack.empty()) {
        Node* n = stack.back(); stack.pop_back();
        n->x -= cx;
        n->y -= cy;
        for (auto& ch : n->children) stack.push_back(ch.get());
    }

    assignAnglesFromLinks(g_root.get());
}

// ---------------------------- Link Drawing ----------------------------

static void bezier3(float p0x, float p0y,
                    float p1x, float p1y,
                    float p2x, float p2y,
                    float p3x, float p3y,
                    float t,
                    float& outx, float& outy)
{
    float u = 1.0f - t;
    float b0 = u*u*u;
    float b1 = 3*u*u*t;
    float b2 = 3*u*t*t;
    float b3 = t*t*t;
    outx = b0*p0x + b1*p1x + b2*p2x + b3*p3x;
    outy = b0*p0y + b1*p1y + b2*p2y + b3*p3y;
}

static void drawLinkStraight(const Node* parent, const Node* child) {
    glBegin(GL_LINES);
    glVertex2f(parent->x, parent->y);
    glVertex2f(child->x,  child->y);
    glEnd();
}

static void drawLinkBezier(const Node* parent, const Node* child) {
    float p0x = parent->x, p0y = parent->y;
    float p3x = child->x,  p3y = child->y;

    float bend = BEZIER_BEND_FACTOR * LEVEL_GAP;
    float p1x, p1y, p2x, p2y;

    if (g_layout == LayoutOrientation::Horizontal) {
        p1x = p0x + bend; p1y = p0y;
        p2x = p3x - bend; p2y = p3y;
    } else {
        p1x = p0x; p1y = p0y - bend;
        p2x = p3x; p2y = p3y + bend;
    }

    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= BEZIER_SAMPLES; ++i) {
        float t = float(i) / float(BEZIER_SAMPLES);
        float x, y;
        bezier3(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, x, y);
        glVertex2f(x, y);
    }
    glEnd();
}

static void drawEdgesRecursive(const Node* n) {
    for (const auto& ch : n->children) {
        glColor4f(0.45f, 0.45f, 0.45f, 0.55f);
        glLineWidth(1.0f);

        if (LINKS_CURVED) drawLinkBezier(n, ch.get());
        else              drawLinkStraight(n, ch.get());

        glColor4f(0.30f, 0.30f, 0.30f, 0.95f);
        float r = ENDPOINT_RADIUS;
        drawFilledCircle(n->x,  n->y,  r, CIRCLE_SEGS);
        drawFilledCircle(ch->x, ch->y, r, CIRCLE_SEGS);

        drawEdgesRecursive(ch.get());
    }
}

static void drawLabelsRecursive(const Node* n) {
    glColor4f(0.10f, 0.10f, 0.10f, 1.0f);

    float scale = LABEL_STROKE_SCALE;

    bool isLeaf = n->children.empty();
    bool shouldDraw = (n == g_root.get()) || (!LABEL_LEAVES_ONLY || isLeaf);
    if (shouldDraw) {

        float ax = n->x;
        float ay;

        if (g_layout == LayoutOrientation::Vertical)
        	ay = n->y - (ENDPOINT_RADIUS + LABEL_VERT_OFFSET);
        else
        	ay = n->y - (ENDPOINT_RADIUS + LABEL_HORZ_OFFSET);

        // Screen-space desired orientation
        float desiredOnScreenDeg = (g_layout == LayoutOrientation::Vertical) ? -90.0f : 0.0f;

        // Counter-rotate scene rotation so label stays at desired screen orientation
        float anglePassed = desiredOnScreenDeg - g_rotDeg;

        drawStrokeStringRotatedAligned(ax, ay, anglePassed, scale,
                                       LABEL_STROKE_FONT, n->text, TextAlign::Start);
    }

    for (const auto& ch : n->children) drawLabelsRecursive(ch.get());
}

static void setupOrtho() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    float aspect = (g_winH != 0) ? float(g_winW) / float(g_winH) : 1.0f;
    float halfH = BASE_HALF_H / g_zoom;
    float halfW = halfH * aspect;

    glOrtho(-halfW, halfW, -halfH, halfH, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glTranslatef(-g_panX, -g_panY, 0.0f);
    glRotatef(g_rotDeg, 0.0f, 0.0f, 1.0f);
}

static void display() {
    glClearColor(1,1,1,1);
    glClear(GL_COLOR_BUFFER_BIT);

    setupOrtho();

    drawEdgesRecursive(g_root.get());
    drawLabelsRecursive(g_root.get());

    glutSwapBuffers();
}

static void idle() { }

static void reshape(int w, int h) {
    g_winW = std::max(1, w);
    g_winH = std::max(1, h);
    glViewport(0, 0, g_winW, g_winH);
    glutPostRedisplay();
}

static void keyboard(unsigned char key, int, int) {
    if (key == 27) std::exit(0); // ESC

    if (key == '+' || key == '=') g_zoom = std::min(20.0f, g_zoom * 1.1f);
    if (key == '-' || key == '_') g_zoom = std::max(0.1f,  g_zoom * 0.9f);

    if (key == 'l' || key == 'L') LABEL_LEAVES_ONLY = !LABEL_LEAVES_ONLY;

    // Toggle layout orientation
    if (key == 'v' || key == 'V') {
        g_layout = (g_layout == LayoutOrientation::Horizontal)
                 ? LayoutOrientation::Vertical
                 : LayoutOrientation::Horizontal;
        computeLayout();
    }

    // Toggle curved/straight links
    if (key == 'c' || key == 'C') LINKS_CURVED = !LINKS_CURVED;

    // Fullscreen toggle
    if (key == 'f' || key == 'F') {
        if (!g_fullscreen) {
            g_fullscreen = true;
            g_winW_prev = g_winW;
            g_winH_prev = g_winH;
            g_winX = glutGet(GLUT_WINDOW_X);
            g_winY = glutGet(GLUT_WINDOW_Y);
            glutFullScreen();
        } else {
            g_fullscreen = false;
            glutReshapeWindow(g_winW_prev, g_winH_prev);
            glutPositionWindow(g_winX, g_winY);
        }
    }

    glutPostRedisplay();
}

static void mouse(int button, int state, int x, int y) {
    if (button == GLUT_LEFT_BUTTON) {
        if (state == GLUT_DOWN) {
            g_dragging = true;
            g_lastMouseX = x;
            g_lastMouseY = y;
        } else {
            g_dragging = false;
        }
    }

    if (state == GLUT_DOWN) {
        if (button == 3) {
            g_zoom = std::min(20.0f, g_zoom * 1.1f);
            glutPostRedisplay();
        } else if (button == 4) {
            g_zoom = std::max(0.1f,  g_zoom * 0.9f);
            glutPostRedisplay();
        }
    }
}

static void motion(int x, int y) {
    if (!g_dragging) return;

    int dx = x - g_lastMouseX;
    int dy = y - g_lastMouseY;
    g_lastMouseX = x;
    g_lastMouseY = y;

    float viewHalfH = BASE_HALF_H / g_zoom;
    float worldPerPixel = (2.0f * viewHalfH) / float(std::max(1, g_winH));

    g_panX -= float(dx) * worldPerPixel;
    g_panY += float(dy) * worldPerPixel;

    glutPostRedisplay();
}

int main(int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "sample.mm";

    g_root = loadFreeMind(path);
    if (!g_root) return 1;

    computeLayout();

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(g_winW, g_winH);
    glutInitWindowPosition(g_winX, g_winY);
    glutCreateWindow("Linear Hierarchy");

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    glutIdleFunc(idle);

    glutMainLoop();
    return 0;
}
