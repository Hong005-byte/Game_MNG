// HD-2D 3D renderer -- see the plan doc (tingly-snuggling-pearl.md) for the
// Phase 1 write-up. Renders every zone as a real perspective-projected, lit
// 3D scene instead of the flat top-down 2D drawZone(). Started as a Town-
// Square-only prototype; as of 2026-08-07 it's the renderer for all 8
// zones (see drawZone()'s now-unconditional dispatch) -- the old flat 2D
// path is kept in GameWorld.cpp, unreachable, as an easy revert if this
// ever needs rolling back.
//
// Deliberately a separate translation unit from the ~7000-line
// GameWorld.cpp rather than growing it further -- same GameWorld class,
// just physically split out.
//
// All math here is plain CPU-side C++ (see Math3D.h) producing ordinary
// 2D screen-space triangles submitted through the same sf::VertexArray +
// window.draw() mechanism every pixel-art helper elsewhere in this project
// already uses. No OpenGL calls, no depth buffer -- depth is a manual
// back-to-front sort, the same painter's-algorithm idea drawZone's
// existing NPC/building/tree Y-sort already relies on, just extended to
// real camera-space depth.
//
// Phase 2 added real pitched roofs and point lights (windows/lamps) tied
// into nightFactor(). Phase 3 replaced the placeholder boxes for trees/
// bushes/lamps/NPCs/player with actual billboarded pixel-art -- the same
// drawTree/drawBush/drawLamp/drawPixelPerson sprites the 2D world already
// draws, pre-rendered once into an offscreen texture (see getBillboard3D)
// and reused every frame as a camera-facing quad. Opaque building/ground
// geometry and textured billboards are sorted into ONE global back-to-
// front order and flushed as batched draw calls per contiguous same-
// texture/same-blend-mode run, so a billboard genuinely behind a building
// from the camera's angle is actually occluded instead of always drawing
// on top.
//
// This pass (rolling out to every zone) also added: locked-business dimming
// and construction-site/empty-plot flat lots (mirroring drawBuilding's 2D
// state machine, minus its signboard/material-list detail -- see
// draw3DZone's building loop), floating name labels (simple screen-space
// text projected through the same camera, not billboarded geometry), grass-
// patch ground tinting, and forageable billboards (Highlands' berry
// pickups). Deliberately NOT done: every business still renders as the same
// generic box+gable-roof regardless of type -- the 2D world's ~20 distinct
// hand-shaped silhouettes (Farm/Mine/Dock/Field/etc.) have no 3D
// equivalent yet. That's the natural next pass if more detail is wanted.

#include "GameWorld.h"
#include "Localization.h"
#include <algorithm>
#include <vector>

namespace {
    std::uint8_t clamp8_3d(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }
    sf::Color shade3d(sf::Color c, int delta) {
        return sf::Color(clamp8_3d(c.r + delta), clamp8_3d(c.g + delta), clamp8_3d(c.b + delta), c.a);
    }
    // Desaturate + darken toward flat gray -- the 3D equivalent of the 2D
    // world's drawLockOverlay dimming rectangle (see draw3DZone's building
    // loop), applied to the wall/roof color directly instead of an overlay
    // quad since there's no cheap way to draw a translucent box over an
    // already-lit 3D box without a second full geometry pass.
    sf::Color dimForLock3d(sf::Color c) {
        int avg = (c.r + c.g + c.b) / 3;
        auto mix = [&](std::uint8_t v) { return clamp8_3d(static_cast<int>((v + avg) * 0.5f * 0.55f)); };
        return sf::Color(mix(c.r), mix(c.g), mix(c.b), c.a);
    }

    // sf::String's implicit conversion from std::string assumes the local
    // ANSI code page, which mangles UTF-8-encoded Chinese text -- same
    // helper as GameWorld.cpp's file-local toSfString (not reachable from
    // here, this is a separate translation unit).
    sf::String toSfString3d(const std::string& utf8) { return sf::String::fromUtf8(utf8.begin(), utf8.end()); }

    // A tileable near-white "grain" texture -- a grid of cells with slight
    // deterministic per-cell brightness variance, optionally divided by
    // darker mortar/seam lines. Meant to be multiplied by a face's own real
    // lit color (see addFace's `tex` param), so it deliberately never
    // supplies actual hue -- only value/luminance noise, same relationship
    // as a grayscale bump/detail map. One shared generator with different
    // cell counts/aspect ratios (and seams on/off) covers stone-block,
    // plaster-grain, and wood-shingle-row material "textures" without
    // needing three different hand-authored patterns -- see where each is
    // baked in draw3DZone for the exact params used per material. `rt`'s
    // own `size` must already be set (this only draws into it); tiling
    // depends on `rt.setRepeated(true)` having been set by the caller (see
    // getBillboard3D's `repeated` param) -- this function doesn't need to
    // know that, a grid pattern is trivially tileable on its own as long as
    // it's drawn within [0, size) with no seam sitting exactly on the wrap
    // boundary doubling up.
    void bakeGrainPattern(sf::RenderTexture& rt, sf::Vector2u size, int cellsX, int cellsY, bool seams) {
        rt.clear(sf::Color(250, 250, 250));
        float cw = static_cast<float>(size.x) / static_cast<float>(cellsX);
        float ch = static_cast<float>(size.y) / static_cast<float>(cellsY);
        for (int r = 0; r < cellsY; ++r) {
            for (int c = 0; c < cellsX; ++c) {
                int variance = ((r * 7 + c * 13) % 5 - 2) * 7;
                std::uint8_t v = clamp8_3d(248 + variance);
                sf::RectangleShape cell(sf::Vector2f(cw + 0.5f, ch + 0.5f)); // +0.5 so cells overlap a hair, no hairline gaps between them
                cell.setPosition(sf::Vector2f(static_cast<float>(c) * cw, static_cast<float>(r) * ch));
                cell.setFillColor(sf::Color(v, v, v));
                rt.draw(cell);
            }
        }
        if (!seams) return;
        sf::Color seamColor(196, 196, 196);
        for (int c = 1; c < cellsX; ++c) { // skips 0/cellsX -- a seam exactly on the wrap boundary would double up with itself when tiled
            sf::RectangleShape line(sf::Vector2f(1.4f, static_cast<float>(size.y)));
            line.setPosition(sf::Vector2f(static_cast<float>(c) * cw, 0.f));
            line.setFillColor(seamColor);
            rt.draw(line);
        }
        for (int r = 1; r < cellsY; ++r) {
            sf::RectangleShape line(sf::Vector2f(static_cast<float>(size.x), 1.4f));
            line.setPosition(sf::Vector2f(0.f, static_cast<float>(r) * ch));
            line.setFillColor(seamColor);
            rt.draw(line);
        }
    }

    constexpr float kPlayerBoxSize = 26.f; // mirrors GameWorld.cpp's own kPlayerSize (file-local there)

    // Directional "sun" light -- overhead-and-toward-camera so the faces the
    // camera actually sees (south-facing walls/roof slopes, since
    // getZoneCamera3D always looks from south to north) catch real light
    // instead of sitting in flat ambient-only shadow.
    //
    // The literal 2D `kLightDirection` (light from the upper-left, shadows
    // point down-right) would put the sun's Z component toward -Z/north --
    // which is also the direction every building's south wall's outward
    // normal points *away* from, so that wall (the one and only wall face
    // the camera can ever actually see, since the north wall is always
    // back-face-culled) would get zero direct light and read as a flat dark
    // silhouette no matter how tall it's drawn. That was the real cause of
    // 2026-08-07's "buildings all look hollow/no detail" report -- walls
    // were never genuinely unlit-looking due to a size/geometry bug, they
    // were just permanently in the sun's own shadow. Flipping Z here (sun
    // toward +Z/south, i.e. roughly over the camera's shoulder) keeps the
    // X-based left/right shading variation from the original 2D convention
    // but makes the readable side of every building the lit side.
    const Vec3 kSunDirToSource = Vec3(-0.45f, 0.78f, 0.55f).normalized();

    // A single point light (building window / lamp head) -- color is a 0..1
    // tint (not 0..255) so it can be multiplied straight into a normalized
    // base color; `intensity` already has the day/night scaling baked in
    // by the caller (see nightFactor()-driven light-building below).
    struct PointLight3D {
        Vec3 pos;
        Vec3 color;
        float intensity;
    };

    struct LightingContext {
        std::vector<PointLight3D> lights;
        float sunStrength; // dims at night, same shape as the 2D dayNightTint's darkening
        float ambient;     // slightly lower at night for more contrast, same idea as the 2D night tint
    };

    sf::Color litColor(sf::Color base, Vec3 normal, Vec3 worldPos, const LightingContext& lc) {
        float sunDiffuse = std::max(0.f, dot(normal, kSunDirToSource)) * lc.sunStrength;
        float baseR = static_cast<float>(base.r) / 255.f;
        float baseG = static_cast<float>(base.g) / 255.f;
        float baseB = static_cast<float>(base.b) / 255.f;

        // Ambient + sun bounded to [ambient, 1] -- NOT a plain sum, or a
        // brightly sun-facing surface (dot ~1) blows straight past 1.0 and
        // clips to flat white regardless of its own color.
        float total = lc.ambient + (1.f - lc.ambient) * sunDiffuse;
        float r = baseR * total, g = baseG * total, b = baseB * total;

        for (const auto& L : lc.lights) {
            Vec3 toLight = L.pos - worldPos;
            float distSq = toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z;
            float dist = std::sqrt(distSq);
            Vec3 dir = dist > 1e-4f ? toLight * (1.f / dist) : Vec3(0.f, 1.f, 0.f);
            float ndotl = std::max(0.f, dot(normal, dir));
            // 2026-08-10 lighting-strengthen pass ("全部光影,全部加强" -- push
            // every light source harder across the board): falloff
            // coefficient eased 0.003->0.0022 so a light's warm influence
            // reaches visibly farther across a wall instead of pooling
            // tightly around its own base, and the per-light cap raised
            // 0.65->0.85 so a nearby light can push a surface noticeably
            // brighter before capping out.
            float atten = L.intensity / (1.f + 0.0022f * distSq);
            // Hard per-light cap -- a point light close to its own surface
            // (a lamp head, a window) always reads as "touching a light
            // bulb" at full strength no matter how the falloff curve is
            // tuned, so cap the boost any single light can contribute
            // instead of solving for a falloff that behaves at every
            // distance from point-blank to far away.
            float contrib = std::min(ndotl * atten, 0.85f);
            r += baseR * L.color.x * contrib;
            g += baseG * L.color.y * contrib;
            b += baseB * L.color.z * contrib;
        }
        return sf::Color(clamp8_3d(static_cast<int>(r * 255.f)), clamp8_3d(static_cast<int>(g * 255.f)),
            clamp8_3d(static_cast<int>(b * 255.f)), base.a);
    }

    // A world-space triangle/quad already reduced to projected
    // screen-space corners + a color -- everything past this point is
    // pushing triangles into a VertexArray, same as the rest of the
    // codebase. `texture` is null for opaque building/ground geometry
    // (flat-lit color only) and non-null for a billboard (uv holds its
    // texture coordinates, color is just the day/night tint multiplier).
    // Triangles reuse the quad slot with p[2]==p[3].
    struct ScreenQuad {
        sf::Vector2f p[4];
        sf::Vector2f uv[4];
        sf::Color color;
        float sortDepth;
        const sf::Texture* texture = nullptr;
        bool additive = false; // true for glow billboards (see addGlowBillboard) -- drawn with BlendAdd instead of the default alpha blend
    };

    struct Projected {
        sf::Vector2f screen;
        float depth = 0.f;
        bool valid = false;
    };

    Projected projectPoint(const Mat4& viewProj, Vec3 world, sf::Vector2u windowSize) {
        float cx, cy, cz, cw;
        viewProj.transformPoint(world, cx, cy, cz, cw);
        (void)cz;
        if (cw <= 0.01f) return {}; // behind the camera -- leave .valid false
        float ndcX = cx / cw, ndcY = cy / cw;
        Projected p;
        p.screen = sf::Vector2f(
            (ndcX * 0.5f + 0.5f) * static_cast<float>(windowSize.x),
            (1.f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(windowSize.y)); // screen Y grows downward
        p.depth = cw;
        p.valid = true;
        return p;
    }

    // Appends one quad face if it's front-facing (normal points back
    // toward the camera) -- the intra-object hidden-face culling that
    // stands in for a hardware depth buffer here. `a,b,c,d` must already be
    // wound so `normal` is this face's true outward-facing direction.
    //
    // `tex`/`uvWorldPerTile` (2026-08-07, both optional/default off --
    // every pre-existing call is unaffected): opaque faces used to be flat-
    // shaded color only, same as a billboard's texture=nullptr case just
    // permanently. When `tex` is set, this now ALSO tiles a real texture
    // across the face (SFML multiplies texture color by vertex color per
    // pixel, same as the billboards already do) -- `q.color` still carries
    // the real lit material color from `litColor` below, so the texture
    // only needs to supply per-pixel VALUE variation (grain/mortar/seams),
    // never actual hue -- see bakeGrainPattern's own comment. UV is derived
    // from the face's own world-space edge lengths divided by
    // `uvWorldPerTile` (world units per texture tile) so texel density
    // stays consistent across differently-sized faces without the caller
    // hand-computing anything -- relies on the quad actually being a
    // parallelogram (true for every face this renderer builds).
    void addFace(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize,
        Vec3 eye, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        Vec3 center = (a + b + c + d) * 0.25f;
        if (dot(normal, center - eye) >= 0.f) return; // back-facing from here, skip

        Projected pa = projectPoint(viewProj, a, windowSize);
        Projected pb = projectPoint(viewProj, b, windowSize);
        Projected pc = projectPoint(viewProj, c, windowSize);
        Projected pd = projectPoint(viewProj, d, windowSize);
        if (!pa.valid || !pb.valid || !pc.valid || !pd.valid) return;

        ScreenQuad q;
        q.p[0] = pa.screen; q.p[1] = pb.screen; q.p[2] = pc.screen; q.p[3] = pd.screen;
        q.color = litColor(baseColor, normal, center, lc);
        q.sortDepth = (pa.depth + pb.depth + pc.depth + pd.depth) * 0.25f;
        if (tex) {
            sf::Vector2u ts = tex->getSize();
            float uTiles = (b - a).length() / uvWorldPerTile;
            float vTiles = (d - a).length() / uvWorldPerTile;
            q.uv[0] = sf::Vector2f(0.f, 0.f);
            q.uv[1] = sf::Vector2f(uTiles * static_cast<float>(ts.x), 0.f);
            q.uv[2] = sf::Vector2f(uTiles * static_cast<float>(ts.x), vTiles * static_cast<float>(ts.y));
            q.uv[3] = sf::Vector2f(0.f, vTiles * static_cast<float>(ts.y));
            q.texture = tex;
        }
        out.push_back(q);
    }

    // Same as addFace but for a 3-point face (roof gable ends) -- degenerate
    // quad with the last corner repeated, everything downstream (sorting,
    // triangulation) already handles that shape fine.
    void addTri(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize,
        Vec3 eye, Vec3 a, Vec3 b, Vec3 c, Vec3 normal, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        addFace(out, viewProj, windowSize, eye, a, b, c, c, normal, baseColor, lc, tex, uvWorldPerTile);
    }

    // A vertical quad face (bl/br at the bottom edge, tl/tr at the top edge,
    // same Y each) split into 3 horizontal stripes -- highlight at top,
    // unchanged base in the middle, shadow at the bottom -- instead of one
    // flat-shaded quad. Same convention as the 2D world's own drawPixelPanel
    // (hi = shade(base,+32), sh = shade(base,-32)), just applied to a real
    // lit 3D face instead of a flat sprite fill. Used specifically for
    // *building walls* (see addBandedBox below), not every small prop box --
    // a fence post or door only a few units tall doesn't have room for 3
    // visually distinct stripes and just gets busier/worse for it, so this
    // is opt-in per call site rather than folded into plain addBox.
    void addBandedVerticalFace(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 bl, Vec3 br, Vec3 tr, Vec3 tl, Vec3 normal, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        Vec3 bl13 = bl + (tl - bl) * (1.f / 3.f), tl13 = bl + (tl - bl) * (2.f / 3.f);
        Vec3 br13 = br + (tr - br) * (1.f / 3.f), tr13 = br + (tr - br) * (2.f / 3.f);
        // Band deltas bumped +/-26 -> +/-36 (2026-08-10 lighting-strengthen
        // pass) -- every building wall in the game routes through this one
        // function, so widening its shadow/highlight split is the single
        // biggest-reach lever for making surfaces read as more strongly lit/
        // shaded without touching each hero building's own call sites.
        addFace(out, viewProj, windowSize, eye, bl, br, br13, bl13, normal, shade3d(baseColor, -36), lc, tex, uvWorldPerTile);   // shadow band
        addFace(out, viewProj, windowSize, eye, bl13, br13, tr13, tl13, normal, baseColor, lc, tex, uvWorldPerTile);              // base band
        addFace(out, viewProj, windowSize, eye, tl13, tr13, tr, tl, normal, shade3d(baseColor, 36), lc, tex, uvWorldPerTile);     // highlight band
    }

    // All (visible) faces of an axis-aligned box spanning [pos, pos+size]
    // in world space (Y is up). Bottom face is never emitted -- an
    // overhead camera never sees it.
    void addBox(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, Vec3 size, sf::Color baseColor, const LightingContext& lc, bool includeTop = true) {
        Vec3 p000 = pos;
        Vec3 p100 = pos + Vec3(size.x, 0.f, 0.f);
        Vec3 p010 = pos + Vec3(0.f, size.y, 0.f);
        Vec3 p001 = pos + Vec3(0.f, 0.f, size.z);
        Vec3 p110 = pos + Vec3(size.x, size.y, 0.f);
        Vec3 p101 = pos + Vec3(size.x, 0.f, size.z);
        Vec3 p011 = pos + Vec3(0.f, size.y, size.z);
        Vec3 p111 = pos + Vec3(size.x, size.y, size.z);

        if (includeTop)
            addFace(out, viewProj, windowSize, eye, p010, p110, p111, p011, Vec3(0.f, 1.f, 0.f), baseColor, lc);
        addFace(out, viewProj, windowSize, eye, p000, p100, p110, p010, Vec3(0.f, 0.f, -1.f), baseColor, lc); // north
        addFace(out, viewProj, windowSize, eye, p101, p001, p011, p111, Vec3(0.f, 0.f, 1.f), baseColor, lc);  // south
        addFace(out, viewProj, windowSize, eye, p001, p000, p010, p011, Vec3(-1.f, 0.f, 0.f), baseColor, lc); // west
        addFace(out, viewProj, windowSize, eye, p100, p101, p111, p110, Vec3(1.f, 0.f, 0.f), baseColor, lc);  // east
    }

    // Same as addBox but every side face goes through addBandedVerticalFace
    // instead of one flat addFace -- reserved for actual building walls
    // (see draw3DZone's building loop and every hero building's wall bands),
    // where a big single-tone panel was the main thing reported as reading
    // "flat"/"not solid"/"no detail" (2026-08-07). Never includes a top
    // face -- every call site that needs one already has a roof (or, for
    // Clinic's flat-roofed blocks, a parapet deck) going on top of it.
    void addBandedBox(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, Vec3 size, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        Vec3 p000 = pos;
        Vec3 p100 = pos + Vec3(size.x, 0.f, 0.f);
        Vec3 p010 = pos + Vec3(0.f, size.y, 0.f);
        Vec3 p001 = pos + Vec3(0.f, 0.f, size.z);
        Vec3 p110 = pos + Vec3(size.x, size.y, 0.f);
        Vec3 p101 = pos + Vec3(size.x, 0.f, size.z);
        Vec3 p011 = pos + Vec3(0.f, size.y, size.z);
        Vec3 p111 = pos + Vec3(size.x, size.y, size.z);

        addBandedVerticalFace(out, viewProj, windowSize, eye, p000, p100, p110, p010, Vec3(0.f, 0.f, -1.f), baseColor, lc, tex, uvWorldPerTile); // north
        addBandedVerticalFace(out, viewProj, windowSize, eye, p101, p001, p011, p111, Vec3(0.f, 0.f, 1.f), baseColor, lc, tex, uvWorldPerTile);  // south
        addBandedVerticalFace(out, viewProj, windowSize, eye, p001, p000, p010, p011, Vec3(-1.f, 0.f, 0.f), baseColor, lc, tex, uvWorldPerTile); // west
        addBandedVerticalFace(out, viewProj, windowSize, eye, p100, p101, p111, p110, Vec3(1.f, 0.f, 0.f), baseColor, lc, tex, uvWorldPerTile);  // east
    }

    // A 4-sided pyramid over a footprint [pos, pos+size] (XZ) rising to a
    // single apex above the center -- the Mine/Gold Mine mound (2D's
    // drawPixelMound rock hump, see addMineProps below). Slope normals are
    // hand-picked reasonable directions (mostly up, tilted toward each
    // compass side) rather than derived from the actual triangle geometry --
    // addFace/addTri take the lighting/cull normal as a caller-supplied
    // parameter regardless of vertex winding, so this is exact enough for a
    // plausible-looking shaded slope without the extra work of deriving it
    // per pyramid from `height`/footprint ratio.
    void addPyramid(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, sf::Vector2f size, float height, sf::Color color, const LightingContext& lc) {
        Vec3 nw(pos.x, pos.y, pos.z), ne(pos.x + size.x, pos.y, pos.z);
        Vec3 se(pos.x + size.x, pos.y, pos.z + size.y), sw(pos.x, pos.y, pos.z + size.y);
        Vec3 apex(pos.x + size.x * 0.5f, pos.y + height, pos.z + size.y * 0.5f);
        addTri(out, viewProj, windowSize, eye, nw, ne, apex, Vec3(0.f, 0.6f, -0.8f).normalized(), color, lc);
        addTri(out, viewProj, windowSize, eye, se, sw, apex, Vec3(0.f, 0.6f, 0.8f).normalized(), color, lc);
        addTri(out, viewProj, windowSize, eye, ne, se, apex, Vec3(0.8f, 0.6f, 0.f).normalized(), color, lc);
        addTri(out, viewProj, windowSize, eye, sw, nw, apex, Vec3(-0.8f, 0.6f, 0.f).normalized(), color, lc);
    }

    // A gable roof over a building footprint [pos, pos+size] (XZ), sitting
    // on top of a wall of height `wallY`, ridge running along X (so the
    // camera -- looking down from the south -- sees the two roof slopes
    // rather than end-on). `roofRise` is how far above the wall the ridge
    // sits.
    void addGableRoof(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, Vec3 size, float wallY, float roofRise, sf::Color roofColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        float midZ = pos.z + size.z * 0.5f;
        Vec3 ridgeW(pos.x, wallY + roofRise, midZ);
        Vec3 ridgeE(pos.x + size.x, wallY + roofRise, midZ);
        Vec3 nw(pos.x, wallY, pos.z), ne(pos.x + size.x, wallY, pos.z);
        Vec3 sw(pos.x, wallY, pos.z + size.z), se(pos.x + size.x, wallY, pos.z + size.z);

        Vec3 nSlopeNormal = cross(ridgeE - ridgeW, nw - ridgeW).normalized();
        addFace(out, viewProj, windowSize, eye, nw, ne, ridgeE, ridgeW, nSlopeNormal, roofColor, lc, tex, uvWorldPerTile);
        Vec3 sSlopeNormal = cross(ridgeW - ridgeE, se - ridgeE).normalized();
        addFace(out, viewProj, windowSize, eye, se, sw, ridgeW, ridgeE, sSlopeNormal, roofColor, lc, tex, uvWorldPerTile);

        sf::Color gableColor = shade3d(roofColor, -32); // 2026-08-10: deepened alongside addBandedVerticalFace's own contrast bump, same "stronger shadow/light split" pass
        addTri(out, viewProj, windowSize, eye, nw, sw, ridgeW, Vec3(-1.f, 0.f, 0.f), gableColor, lc);
        addTri(out, viewProj, windowSize, eye, se, ne, ridgeE, Vec3(1.f, 0.f, 0.f), gableColor, lc);
    }

    // Same idea as addGableRoof, rotated 90 degrees -- ridge runs front-to-
    // back (along Z) instead of side-to-side, so the gable END (the
    // triangular peaked wall, not a roof slope) is what faces the camera.
    // This is the classic "pointed cottage front" silhouette real half-
    // timbered reference buildings read as -- addGableRoof's ridge-along-X
    // version only ever shows the camera a long roof slope face-on, which
    // was part of why buildings read as flat/simple. Only emits the two
    // roof-material slope faces; the gable end walls themselves (south =
    // camera-facing, north = hidden) are the caller's job, same material as
    // the wall below them, not roof shingle color -- see
    // addRecruitmentCenterBuilding for the full assembly with a timber-
    // braced south gable.
    void addFrontGableRoof(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, sf::Vector2f footprint, float wallY, float roofRise, sf::Color roofColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        float midX = pos.x + footprint.x * 0.5f;
        Vec3 ridgeS(midX, wallY + roofRise, pos.z + footprint.y);
        Vec3 ridgeN(midX, wallY + roofRise, pos.z);
        Vec3 sw(pos.x, wallY, pos.z + footprint.y), se(pos.x + footprint.x, wallY, pos.z + footprint.y);
        Vec3 nw(pos.x, wallY, pos.z), ne(pos.x + footprint.x, wallY, pos.z);

        // Derived directly from the roof's own rise/run within the X-Y
        // cross-section (perpendicular to the slope, pointing outward and
        // up) rather than a cross-product off the quad's own corners --
        // more reliable than chasing winding-order sign errors for a
        // mirrored pair of faces.
        float run = footprint.x * 0.5f;
        Vec3 westNormal = Vec3(-roofRise, run, 0.f).normalized();
        Vec3 eastNormal = Vec3(roofRise, run, 0.f).normalized();
        addFace(out, viewProj, windowSize, eye, sw, nw, ridgeN, ridgeS, westNormal, roofColor, lc, tex, uvWorldPerTile);
        addFace(out, viewProj, windowSize, eye, se, ne, ridgeN, ridgeS, eastNormal, roofColor, lc, tex, uvWorldPerTile);
    }

    // A single-slope canopy awning (higher at the back edge, lower at the
    // front edge it overhangs), sliced into vertical stripes along its
    // width and alternating between two colors -- the striped market-stall
    // canopy look (see addMarketBuilding below). A real market awning is a
    // flat cloth sheet on one slope, not a gable's two, so this is its own
    // shape rather than a themed call to addGableRoof/addFrontGableRoof.
    // `pos` is the ground point under the BACK edge (y assumed 0, same
    // convention as addFarmProps/addGableRoof's own `pos`); `depth` runs
    // toward the camera (south, +Z) same as everywhere else in this file.
    void addStripedAwning(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, float width, float depth, float backH, float frontH, sf::Color colorA, sf::Color colorB,
        int stripes, const LightingContext& lc) {
        float stripeW = width / static_cast<float>(stripes);
        // Derived from rise/run in the Z-Y cross-section, same approach as
        // addFrontGableRoof's slope normals -- more reliable than a cross
        // product that depends on getting the quad's winding order right.
        float rise = backH - frontH;
        Vec3 normal = Vec3(0.f, depth, rise).normalized();
        for (int i = 0; i < stripes; ++i) {
            float x0 = pos.x + stripeW * static_cast<float>(i);
            Vec3 backL(x0, pos.y + backH, pos.z), backR(x0 + stripeW, pos.y + backH, pos.z);
            Vec3 frontR(x0 + stripeW, pos.y + frontH, pos.z + depth), frontL(x0, pos.y + frontH, pos.z + depth);
            sf::Color c = (i % 2 == 0) ? colorA : colorB;
            addFace(out, viewProj, windowSize, eye, frontL, frontR, backR, backL, normal, c, lc);
        }
        // A small scalloped valance drop along the front eave -- alternating
        // short flaps in the same two colors, the classic market-canopy trim
        // visible in the reference, and a cheap way to give the otherwise-
        // flat front edge some real silhouette.
        float valanceH = std::min(9.f, frontH * 0.3f);
        for (int i = 0; i < stripes; ++i) {
            float x0 = pos.x + stripeW * static_cast<float>(i);
            sf::Color c = shade3d((i % 2 == 0) ? colorA : colorB, -24); // 2026-08-10: same contrast-strengthen pass as addBandedVerticalFace
            Vec3 a(x0, pos.y + frontH, pos.z + depth), bb(x0 + stripeW, pos.y + frontH, pos.z + depth);
            Vec3 cc(x0 + stripeW, pos.y + frontH - valanceH, pos.z + depth), d(x0, pos.y + frontH - valanceH, pos.z + depth);
            addFace(out, viewProj, windowSize, eye, a, bb, cc, d, Vec3(0.f, 0.f, 1.f), c, lc);
        }
    }

    // A thin band across a sloped roof face, interpolated between a
    // parallel "eave" edge (eaveA/eaveB) and "ridge" edge (ridgeA/ridgeB) at
    // slope fractions [t0,t1] -- addFacadeBeam/addFacadeBeamX only work on
    // a flat (fixed-Z or fixed-X) wall plane; a roof slope is tilted in two
    // axes at once, so this interpolates directly between the two lines
    // that actually define the slope instead of trying to reuse those.
    // `offset` nudges the band outward along `normal` a little so it sits
    // proud of the slope's own plain fill instead of z-fighting with it.
    void addSlopeBand(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 eaveA, Vec3 eaveB, Vec3 ridgeA, Vec3 ridgeB, float t0, float t1, Vec3 normal, float offset, sf::Color color, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        Vec3 off = normal * offset;
        Vec3 a = eaveA + (ridgeA - eaveA) * t0 + off;
        Vec3 bb = eaveB + (ridgeB - eaveB) * t0 + off;
        Vec3 c = eaveB + (ridgeB - eaveB) * t1 + off;
        Vec3 d = eaveA + (ridgeA - eaveA) * t1 + off;
        addFace(out, viewProj, windowSize, eye, a, bb, c, d, normal, color, lc, tex, uvWorldPerTile);
    }

    // Several addSlopeBand rows across one roof slope -- shingle-row
    // texture layered on top of the slope's own plain fill (addGableRoof/
    // addFrontGableRoof), the tilted-surface equivalent of addFacadeBeam's
    // "many small real quads instead of one flat color" trick for walls.
    // `eaveA`/`ridgeA` and `eaveB`/`ridgeB` must correspond to the SAME end
    // of the slope (e.g. both the west corner, both the east corner) --
    // this is the axis addSlopeBand interpolates each edge along.
    void addShingleRows(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 eaveA, Vec3 eaveB, Vec3 ridgeA, Vec3 ridgeB, Vec3 normal, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        sf::Color rowColor = shade3d(baseColor, 30); // 2026-08-10: same contrast-strengthen pass as addBandedVerticalFace
        constexpr int kRows = 5;
        for (int i = 0; i < kRows; ++i) {
            float t0 = 0.08f + static_cast<float>(i) * 0.18f;
            float t1 = std::min(t0 + 0.11f, 0.97f);
            addSlopeBand(out, viewProj, windowSize, eye, eaveA, eaveB, ridgeA, ridgeB, t0, t1, normal, 0.8f, rowColor, lc, tex, uvWorldPerTile);
        }
    }

    // A camera-facing (Y-axis-locked, so sprites stay upright rather than
    // tilting with camera pitch -- the standard billboard convention for
    // isometric/2.5D games with pixel-art characters) textured quad.
    // `base` is the world-space ground point the sprite stands on; the
    // sprite's own texture already has its own baked-in shading (drawTree/
    // drawPixelPerson etc. shade themselves), so unlike addFace this
    // doesn't run the surface through the directional/point lighting model
    // at all -- only a flat day/night tint (see `tint`), the same idea as
    // the 2D world's own dayNightTint overlay. `additive` is for
    // addGlowBillboard below -- everything else leaves it at the default.
    void addBillboard(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize,
        Vec3 billboardRight, Vec3 base, float width, float height, const sf::Texture& tex, sf::Color tint,
        bool additive = false) {
        Vec3 right = billboardRight * (width * 0.5f);
        Vec3 up(0.f, height, 0.f);
        Vec3 p0 = base - right;            // bottom-left
        Vec3 p1 = base + right;            // bottom-right
        Vec3 p2 = base + right + up;       // top-right
        Vec3 p3 = base - right + up;       // top-left

        Projected pp0 = projectPoint(viewProj, p0, windowSize);
        Projected pp1 = projectPoint(viewProj, p1, windowSize);
        Projected pp2 = projectPoint(viewProj, p2, windowSize);
        Projected pp3 = projectPoint(viewProj, p3, windowSize);
        if (!pp0.valid || !pp1.valid || !pp2.valid || !pp3.valid) return;

        ScreenQuad q;
        q.p[0] = pp0.screen; q.p[1] = pp1.screen; q.p[2] = pp2.screen; q.p[3] = pp3.screen;
        sf::Vector2u ts = tex.getSize();
        q.uv[0] = sf::Vector2f(0.f, static_cast<float>(ts.y));
        q.uv[1] = sf::Vector2f(static_cast<float>(ts.x), static_cast<float>(ts.y));
        q.uv[2] = sf::Vector2f(static_cast<float>(ts.x), 0.f);
        q.uv[3] = sf::Vector2f(0.f, 0.f);
        q.color = tint;
        q.sortDepth = (pp0.depth + pp1.depth + pp2.depth + pp3.depth) * 0.25f;
        q.texture = &tex;
        q.additive = additive;
        out.push_back(q);
    }

    // A soft round glow sprite, camera-facing like addBillboard, drawn with
    // additive blending (see the flush loop's per-batch blend mode) instead
    // of the normal alpha blend -- the actual "bloom" for a light source:
    // without this, `lc.lights` only brightens whatever surface a light
    // happens to shine on (see litColor), there's nothing to mark the light
    // *itself* as a glowing point the way a real lamp or lit window reads.
    // Reuses the exact same soft-halo texture the 2D world's drawGlow
    // renders into, via the billboard cache, so it's visually consistent
    // with every other glow in the game rather than a second, different-
    // looking implementation.
    void addGlowBillboard(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize,
        Vec3 billboardRight, Vec3 pos, float size, const sf::Texture& glowTex, sf::Color tint) {
        addBillboard(out, viewProj, windowSize, billboardRight, pos, size, size, glowTex, tint, /*additive=*/true);
    }

    // Ground/decoration quads (the grass plane, paths, water, sand, every
    // flat-plot archetype's own ground fill) can span most of Town
    // Square's 820-unit depth in one call, while a ScreenQuad's sort key is
    // just the *average* depth of its 4 corners. For a quad that large the
    // average is a bad stand-in for "how far away is this" -- it can come
    // out nearer than a building that's actually much farther north, so
    // the painter's-algorithm sort below draws the ground on top of that
    // building, erasing it completely (this was the actual cause of the
    // north-row buildings -- market/staff/doctor -- disappearing: reported
    // 2026-08-07). Slicing any quad taller in world Z than kGroundSliceZ
    // into narrower strips keeps each strip's own depth range small enough
    // that its corner-average sort key is *usually* accurate.
    //
    // "Usually" wasn't good enough -- this exact class of bug kept
    // recurring (Clinic's flat roof deck was another instance, fixed by
    // routing it through this same function) and the user eventually asked
    // for a blanket guarantee instead of chasing each new occurrence one
    // building at a time ("把这种土地的图层直接set在最低层,层度全部低于角
    // 色,屋子等等" -- just force every ground-layer quad to the very back,
    // always, below the player/every building, full stop). `kGroundDepthBias`
    // is added (not overwritten) to each slice's own already-computed
    // sortDepth -- large enough that even the single nearest ground slice
    // in any zone still outranks (sorts as farther than) the single
    // farthest non-ground quad, so ground can never again erase anything,
    // while still preserving each slice's own correct depth *relative to
    // other ground quads* (a Farm's own brown fill still draws over the
    // base grass beneath it, a closer ground slice still literally is
    // "more biased forward" than a farther one, etc.) since the same
    // constant offset doesn't change their relative ordering among
    // themselves -- only guarantees they can never outrank real geometry.
    constexpr float kGroundSliceZ = 60.f;
    constexpr float kGroundDepthBias = 1.0e6f; // real clip-space w values in this renderer never get anywhere close to this
    // `applyGroundBias` (2026-08-07, "后面的部分是透明的,可以看到我自己
    //还有树" -- a direct, previously-unforeseen consequence of the
    // kGroundDepthBias fix above): that fix's own guarantee -- ground can
    // never outrank real geometry -- assumed every caller was *actual
    // ground-level terrain*, where "nothing legitimately needs to occlude
    // from behind the ground plane" is a safe bet. Clinic's flat roof deck
    // reuses this same function (see addFlatRoof, needed the exact same
    // Z-slicing this function already does) but is NOT ground-level -- it's
    // an elevated surface a tree or the player standing north of the
    // building absolutely can and should be hidden behind, from the right
    // camera angle. Forcing it to "always sort as farthest" (the bias) means
    // it can now never win a depth fight against ANYTHING, including things
    // that are genuinely farther away and ought to be occluded by it --
    // which is exactly what let the player's own sprite and a tree show
    // through the roof once the camera panned far enough to expose it.
    // Defaults to true (every pre-existing ground/terrain call is
    // unaffected); only addFlatRoof's own roof-deck call passes false.
    void addGroundQuad(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        float x0, float z0, float sizeX, float sizeZ, float y, sf::Color color, const LightingContext& lc, bool applyGroundBias = true) {
        int slices = std::max(1, static_cast<int>(std::ceil(sizeZ / kGroundSliceZ)));
        float sliceZ = sizeZ / static_cast<float>(slices);
        for (int i = 0; i < slices; ++i) {
            float zi = z0 + sliceZ * static_cast<float>(i);
            Vec3 p0(x0, y, zi), p1(x0 + sizeX, y, zi), p2(x0 + sizeX, y, zi + sliceZ), p3(x0, y, zi + sliceZ);
            std::size_t before = out.size();
            addFace(out, viewProj, windowSize, eye, p0, p1, p2, p3, Vec3(0.f, 1.f, 0.f), color, lc);
            if (applyGroundBias && out.size() > before) out.back().sortDepth += kGroundDepthBias; // only if addFace actually pushed one (it can silently skip a back-facing slice)
        }
    }

    // ---- Bespoke per-business 3D shapes (2026-08-07 follow-up) --
    // mirroring the 2D world's drawFarmShape/drawMineShape/etc (see
    // GameWorld.cpp): those 9 archetypes + Cottage are the ones the 2D
    // world's drawBuilding dispatch treats as fully bespoke rather than a
    // themed variant of a shared shape, and the exact set draw3DZone's
    // building loop below now special-cases instead of falling through to
    // the generic box+gable-roof. Every color/offset here is copied
    // straight from its 2D counterpart's constants -- these are meant to
    // read as "the same building, seen in 3D now", not a redesign.

    // A low raised curb traced around a flat plot's footprint (farm/mine/
    // lumber/quarry/etc below, and the construction-site/empty-plot lots in
    // draw3DZone's building loop) -- every one of these shapes' 2D
    // counterparts outlines its base drawPixelPanel in a dark border
    // (mostly the same (25,20,15) regardless of fill color), which is what
    // actually makes a flat-colored plot read as a distinct "place" instead
    // of just a patch of slightly-different-colored grass. addGroundQuad has
    // no equivalent -- it's a bare unbordered fill -- so a plot whose color
    // happens to be close to the grass (the lumber yard's mossy green-brown
    // was the reported case: "just 3 logs floating there") had nothing
    // marking its edges at all. Built from 4 real (lit, sortable) boxes
    // rather than a flat screen-space line specifically so it reads as
    // actual raised detail, not just a thin outline that can get lost at
    // distance.
    void addPlotBorder(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        sf::Vector2f pos, sf::Vector2f size, sf::Color color, const LightingContext& lc) {
        constexpr float t = 6.f, h = 10.f;
        addBox(out, viewProj, windowSize, eye, Vec3(pos.x, 0.f, pos.y), Vec3(size.x, h, t), color, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(pos.x, 0.f, pos.y + size.y - t), Vec3(size.x, h, t), color, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(pos.x, 0.f, pos.y), Vec3(t, h, size.y), color, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(pos.x + size.x - t, 0.f, pos.y), Vec3(t, h, size.y), color, lc);
    }

    // A crop bed's own fill color + prop scatter, keyed by the farm's
    // ACTUAL currently-selected crop (Game::farmCropId(), one of the 7 real
    // CropType ids in Business.cpp -- wheat/strawberry/corn/watermelon/
    // pumpkin/sweetpotato/cabbage). `style` picks which prop loop below
    // draws on top of the fill: 0 = flat only (wheat -- no raised prop
    // reads cleaner for a grain field than a repeated bump would), 1 = corn
    // stalk+ear boxes, 2 = round mound (pumpkin/sweetpotato/watermelon,
    // color/size vary per crop so they don't all look like the same
    // vegetable), 3 = a billboard cluster (cabbage reuses the general
    // veggieTex decal; strawberry reuses the existing `forageable` berry-
    // cluster decal outright rather than baking a near-duplicate, same
    // "share a decal across contexts" call Bank/Inn/Storefront already made).
    struct FarmCropVisual { sf::Color ground, prop, propCap; int style; };
    FarmCropVisual farmCropVisual(const std::string& cropId) {
        if (cropId == "corn") return { sf::Color(168, 178, 72), sf::Color(96, 132, 60), sf::Color(214, 188, 78), 1 };
        if (cropId == "pumpkin") return { sf::Color(150, 108, 44), sf::Color(214, 126, 44), sf::Color(90, 132, 64), 2 };
        if (cropId == "sweetpotato") return { sf::Color(120, 74, 54), sf::Color(150, 86, 60), sf::Color(178, 114, 82), 2 };
        if (cropId == "watermelon") return { sf::Color(72, 124, 70), sf::Color(64, 128, 78), sf::Color(46, 96, 60), 2 };
        if (cropId == "cabbage") return { sf::Color(86, 132, 64), sf::Color(), sf::Color(), 3 };
        if (cropId == "strawberry") return { sf::Color(96, 140, 72), sf::Color(), sf::Color(), 4 };
        return { sf::Color(198, 182, 68), sf::Color(), sf::Color(), 0 }; // wheat, and the fallback for any unrecognized id
    }

    // v3 (2026-08-10, first a richer-but-messy v2 -- see this file's own
    // memory log -- then user feedback from a real in-game screenshot:
    // "有点丑...就划分好来那个土地种什么...当用户选了那个作物,农场里的作物
    // 会更换去对应的" -- looks messy, clearly divide what each plot grows,
    // and ideally the farm's own crops should switch to match whichever
    // crop the player actually has selected). v2's mistake, found by
    // re-reading the game's own data model rather than guessing again from
    // a screenshot: `farm` only ever grows ONE crop at a time (see
    // `Game::farmCropId()`, already used by this same building's own
    // floating label) -- v2 invented 5 different simultaneous crop types
    // (wheat/corn/cabbage/"carrot"/"potato", the last 2 not even real
    // CropType ids in Business.cpp) with zero relationship to the actual
    // selected crop, which is both wrong and exactly why it read as
    // cluttered/arbitrary rather than "a farm growing something". Fixed by
    // making every row show the SAME crop (via `farmCropVisual` above,
    // keyed off `cropId`, a new parameter sourced from `game_.farmCropId()`
    // at the one dispatch call site) and adding a real per-row border (see
    // `addRowBorder` below) so "which land grows what" reads as clean
    // divided plots instead of colored fill bleeding into its neighbors. ----
    void addFarmProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& chickenTex, const sf::Texture& pigTex, const sf::Texture& veggieTex,
        const sf::Texture& flowerTex, const sf::Texture& forageTex, const sf::Texture& cabbageTex, const std::string& cropId) {
        sf::Color soilColor(107, 84, 48);
        sf::Color beamColor(58, 40, 26);
        sf::Color troughColor(110, 108, 104);
        sf::Color waterColor(120, 168, 196);
        sf::Color hayColor(198, 168, 78);
        sf::Color hayCapColor(220, 196, 108);
        sf::Color cartColor(118, 86, 52);
        sf::Color wheelColor(60, 44, 28);
        sf::Color plantBoxColor(96, 68, 40);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, soilColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // A thin raised curb around one row's own footprint -- addPlotBorder
        // itself (t=6,h=10) is tuned for a whole plot; at a single row's
        // much narrower width that thickness would eat most of the row's
        // own visible fill, so this is a lighter-weight local version
        // scaled for a single crop row instead of reusing addPlotBorder
        // directly.
        auto addRowBorder = [&](float x0, float z0, float w, float d) {
            constexpr float t = 2.2f, h = 4.f;
            sf::Color rc(48, 38, 22);
            addBox(out, viewProj, windowSize, eye, Vec3(x0, 0.f, z0), Vec3(w, h, t), rc, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(x0, 0.f, z0 + d - t), Vec3(w, h, t), rc, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(x0, 0.f, z0), Vec3(t, h, d), rc, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(x0 + w - t, 0.f, z0), Vec3(t, h, d), rc, lc);
        };

        // ---- 5 crop-bed rows (north portion, z:5-49) -- all the SAME
        // crop (matching the farm's own single active `cropId`), each
        // individually bordered so the field reads as clearly divided,
        // tended rows instead of one undifferentiated colored rectangle. ----
        FarmCropVisual cv = farmCropVisual(cropId);
        constexpr int kBeds = 5;
        float gap = b.size.x / static_cast<float>(kBeds);
        float bedZ0 = b.position.y + 5.f, bedD = 44.f;
        for (int i = 0; i < kBeds; ++i) {
            float x0 = b.position.x + gap * static_cast<float>(i) + gap * 0.2f;
            float bedW = gap * 0.55f;
            addGroundQuad(out, viewProj, windowSize, eye, x0, bedZ0, bedW, bedD, 3.f, cv.ground, lc);
            addRowBorder(x0, bedZ0, bedW, bedD);
            switch (cv.style) {
            case 0: { // wheat (2026-08-10 follow-up, "小麦的造型可以立体一点"
                // -- v3's flat-fill-only wheat read as the least 3D of any
                // crop; a real field of thin golden stalks with a seed-head
                // cap gives it actual raised volume, denser/thinner than
                // corn's own stalks (6 per row across 2 staggered lanes,
                // height varied) so it reads as planted grain rather than
                // corn's fewer, thicker stalks.
                const float stalkT[] = { 0.14f, 0.3f, 0.46f, 0.62f, 0.78f, 0.92f };
                for (std::size_t si = 0; si < std::size(stalkT); ++si) {
                    float h = 13.f + ((si % 2) ? 4.f : 0.f);
                    float lane = (si % 2) ? 0.72f : 0.28f;
                    Vec3 sp(x0 + bedW * lane, 3.f, bedZ0 + bedD * stalkT[si]);
                    addBox(out, viewProj, windowSize, eye, sp, Vec3(1.6f, h, 1.6f), sf::Color(180, 168, 74), lc);
                    addBox(out, viewProj, windowSize, eye, sp + Vec3(-0.8f, h, -0.8f), Vec3(3.2f, 4.f, 3.2f), sf::Color(224, 202, 104), lc);
                }
                break;
            }
            case 1: // corn -- a thin stalk + a yellow "ear" cap.
                for (float t : { 0.22f, 0.5f, 0.78f }) {
                    Vec3 sp(x0 + bedW * 0.5f, 3.f, bedZ0 + bedD * t);
                    addBox(out, viewProj, windowSize, eye, sp, Vec3(2.5f, 16.f, 2.5f), cv.prop, lc);
                    addBox(out, viewProj, windowSize, eye, sp + Vec3(-1.f, 10.f, -1.f), Vec3(4.5f, 5.f, 4.5f), cv.propCap, lc);
                }
                break;
            case 2: // pumpkin/sweetpotato/watermelon -- 2 round mounds (up-facing-cap trick, same as tree stumps/quarry rubble).
                for (float t : { 0.32f, 0.68f }) {
                    Vec3 mp(x0 + bedW * 0.5f - 4.f, 3.f, bedZ0 + bedD * t);
                    addBox(out, viewProj, windowSize, eye, mp, Vec3(8.f, 6.f, 8.f), cv.prop, lc);
                    addBox(out, viewProj, windowSize, eye, mp + Vec3(1.5f, 6.f, 1.5f), Vec3(5.f, 1.5f, 5.f), cv.propCap, lc);
                }
                break;
            case 3: // cabbage (2026-08-10 follow-up, "卷心菜就是绿色球体吧"
                // -- should read as a green sphere): a low round base mound
                // (real 3D volume, same up-facing-cap trick as the mound
                // crops above) topped with the new farm_cabbage decal --
                // 3 concentric circles faking sphere shading, the same
                // "layered flat shapes" trick every curved prop in this
                // renderer relies on (statues/fountain/pig) -- instead of
                // the previous generic mixed-veggie cluster, which read as
                // loose scattered produce, not a round head.
                for (float t : { 0.28f, 0.58f, 0.86f }) {
                    Vec3 cp(x0 + bedW * 0.5f - 5.f, 3.f, bedZ0 + bedD * t);
                    addBox(out, viewProj, windowSize, eye, cp, Vec3(10.f, 4.f, 10.f), sf::Color(78, 116, 58), lc);
                    addBillboard(out, viewProj, windowSize, billboardRight, cp + Vec3(5.f, 4.f, 5.f), 15.f, 15.f, cabbageTex, sf::Color::White);
                }
                break;
            case 4: // strawberry -- reuses the existing Highlands forageable berry-cluster decal outright.
                for (float t : { 0.25f, 0.55f, 0.85f }) {
                    addBillboard(out, viewProj, windowSize, billboardRight, Vec3(x0 + bedW * 0.5f, 3.f, bedZ0 + bedD * t), 14.f, 14.f, forageTex, sf::Color::White);
                }
                break;
            }
        }

        // ---- Farmyard strip, south of the beds (z:54-80) -- water trough
        // + a chicken and a pig, hay bales, a wheelbarrow of vegetables,
        // lantern posts, and potted plants. All checked by hand against
        // each other for overlap, same discipline every flat-plot
        // archetype's own layout round has relied on. ----
        Vec3 troughPos(b.position.x + 6.f, 0.f, b.position.y + 58.f);
        addBox(out, viewProj, windowSize, eye, troughPos, Vec3(24.f, 8.f, 10.f), troughColor, lc);
        addBox(out, viewProj, windowSize, eye, troughPos + Vec3(2.f, 5.f, 2.f), Vec3(20.f, 2.5f, 6.f), waterColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(b.position.x + 16.f, 0.f, b.position.y + 70.f), 16.f, 16.f, chickenTex, sf::Color::White);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(b.position.x + 4.f, 0.f, b.position.y + 74.f), 22.f, 18.f, pigTex, sf::Color::White);

        Vec3 hayBase(b.position.x + 86.f, 0.f, b.position.y + 56.f);
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                Vec3 hp = hayBase + Vec3(static_cast<float>(i) * 11.f, 0.f, static_cast<float>(j) * 11.f);
                addBox(out, viewProj, windowSize, eye, hp, Vec3(9.f, 8.f, 9.f), hayColor, lc);
                addBox(out, viewProj, windowSize, eye, hp + Vec3(1.f, 8.f, 1.f), Vec3(7.f, 1.5f, 7.f), hayCapColor, lc);
            }
        }

        Vec3 cartPos(b.position.x + 50.f, 0.f, b.position.y + 66.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, cartPos + Vec3(8.f, 8.f, 5.f), 18.f, 14.f, veggieTex, sf::Color::White);

        for (float lx : { 34.f, 78.f }) {
            Vec3 lanternPos(b.position.x + lx, 0.f, b.position.y + 60.f);
            addBox(out, viewProj, windowSize, eye, lanternPos, Vec3(3.f, 24.f, 3.f), beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos + Vec3(1.5f, 28.f, 1.5f), 16.f, glowTex, sf::Color(255, 200, 120, 160));
        }

        for (const auto& p : { sf::Vector2f(40.f, 76.f), sf::Vector2f(72.f, 77.f) }) {
            Vec3 boxPos(b.position.x + p.x, 0.f, b.position.y + p.y);
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(6.f, 6.f, 6.f), plantBoxColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, boxPos + Vec3(3.f, 6.f, 3.f), 12.f, 14.f, flowerTex, sf::Color::White);
        }
    }

    // Shared by Mine and Gold Mine (only the rock color + an extra sparkle
    // pass differ -- see addGoldMineProps below), same as the 2D versions
    // sharing everything but drawPixelMound's color and the sparkle loop.
    // Mound height and arch proportions retuned 2026-08-07 (reported as "the
    // mountain is too small, the door is too big") -- a 62-tall mound over a
    // 110x80 footprint read as a squashed bump rather than a hill, and a
    // 34-tall/0.34-width entrance ate more than half of it.
    void addMineProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, sf::Color rockColor) {
        addPyramid(out, viewProj, windowSize, eye, Vec3(b.position.x, 0.f, b.position.y), sf::Vector2f(b.size.x, b.size.y), 130.f, rockColor, lc);
        float archW = b.size.x * 0.20f, archD = b.size.y * 0.22f, archHeight = 24.f;
        Vec3 archPos(b.position.x + b.size.x * 0.5f - archW * 0.5f, 0.f, b.position.y + b.size.y - archD);
        addBox(out, viewProj, windowSize, eye, archPos, Vec3(archW, archHeight, archD), sf::Color(18, 18, 20), lc);
        addBox(out, viewProj, windowSize, eye, archPos - Vec3(5.f, 0.f, 0.f), Vec3(5.f, archHeight + 6.f, archD), sf::Color(94, 62, 32), lc);
        addBox(out, viewProj, windowSize, eye, archPos + Vec3(archW, 0.f, 0.f), Vec3(5.f, archHeight + 6.f, archD), sf::Color(94, 62, 32), lc);
    }

    void addGoldMineProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex) {
        addMineProps(out, viewProj, windowSize, eye, b, lc, sf::Color(150, 130, 80));
        const sf::Vector2f sparkles[] = { { 0.28f, 0.55f }, { 0.68f, 0.45f } };
        for (const auto& s : sparkles) {
            Vec3 p(b.position.x + b.size.x * s.x, 46.f, b.position.y + b.size.y * s.y);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, p, 22.f, glowTex, sf::Color(255, 215, 90, 210));
        }
    }

    // v2 (2026-08-07, from the user's own twelfth reference image -- a full
    // working lumber camp: felled trees and stumps, a 2-stack log pile, a
    // plank stack, a small steam-powered loader/crane with a smokestack,
    // and a stream running along one edge). This stays in the flat-plot-
    // archetype family (ground fill + `addPlotBorder` + raised props, same
    // as Farm/Mine/Quarry/etc) rather than becoming an enclosed building --
    // "Lumber Camp" is still the tier-1 RAW-harvesting business, and every
    // other tier-1 producer across every zone shares this same "flat plot,
    // not a walled structure" pattern (see addSawmillBuilding's own header
    // comment for the mix-up that almost broke that consistency, and why
    // this reference actually went to the *sawmill* instead, one round
    // before this one). A lean-to shed is still just raised props on a
    // flat plot in this renderer's own convention, same as Kitchen's porch
    // -- 4 posts and a roof slab, no walls/foundation, so it doesn't cross
    // into "this is secretly a building" territory.
    void addLumberProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex) {
        sf::Color logColor(124, 84, 44);
        sf::Color logCapColor(172, 134, 88);
        sf::Color stumpColor(112, 76, 40);
        sf::Color stumpCapColor(196, 164, 110);
        sf::Color plankColor(180, 148, 96);
        sf::Color waterColor(90, 150, 190);
        sf::Color craneColor(100, 96, 90);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color smokeColor(210, 210, 214, 90);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(96, 124, 60), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // A stream along the west edge (genuine ground-level fill, keeps
        // the default depth bias).
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, 10.f, b.size.y, 0.5f, waterColor, lc);

        // Main log pile -- FIX (2026-08-07, "可以把那个木头分散一点吗,粘在
        // 一起有点软" -- the old version's 3 rows were all the same 68-unit
        // length, same Y, directly touching edge to edge with zero gap, so
        // from this camera's angle they read as one continuous flat mass
        // instead of 3 distinct logs. Individual logs now vary in length/
        // Z-position/height and sit with real gaps between them -- 3 base
        // logs side by side with small (2-3 unit) gaps, 2 more nestled on
        // TOP of those gaps (offset up in Y, like logs actually resting on
        // top of an uneven pile), 1 more capping the top -- a real stacked
        // pile with visible seams between pieces, not a flat slab. Each
        // still gets its own end-cap (these logs lie along X, so the face
        // the camera sees is the long side -- the cap is the small lighter
        // patch offset toward -X, same trick as before, just per-log now
        // instead of per-row). Kept deliberately tidy/stacked rather than
        // scattered -- "乱" (don't make it messy) came right after the
        // "spread it out" request, so this reads as an organized woodpile,
        // not litter. ----
        {
            struct LogSpec { float dx, dz, dy, len, diam; };
            const LogSpec mainLogs[] = {
                { 0.f, 0.f, 0.f, 62.f, 14.f },
                { 4.f, 17.f, 0.f, 54.f, 13.f },
                { 8.f, 32.f, 0.f, 58.f, 14.f },
                { 3.f, 9.f, 12.5f, 44.f, 12.f },
                { 6.f, 24.f, 12.5f, 38.f, 12.f },
                { 4.f, 17.f, 23.5f, 28.f, 11.f },
            };
            Vec3 pileOrigin(b.position.x + 20.f, 0.f, b.position.y + 14.f);
            for (const auto& lg : mainLogs) {
                Vec3 lp(pileOrigin.x + lg.dx, lg.dy, pileOrigin.z + lg.dz);
                addBox(out, viewProj, windowSize, eye, lp, Vec3(lg.len, lg.diam, lg.diam), logColor, lc);
                addBox(out, viewProj, windowSize, eye, Vec3(lp.x - 4.f, lp.y, lp.z), Vec3(8.f, lg.diam + 0.5f, lg.diam), logCapColor, lc);
            }
        }
        // A second, smaller stack just north of the main one -- freshly
        // felled logs not yet added to the main pile, same "individual
        // logs with real gaps" treatment.
        {
            struct LogSpec { float dx, dz, dy, len, diam; };
            const LogSpec extraLogs[] = {
                { 0.f, 0.f, 0.f, 34.f, 12.f },
                { 3.f, 14.f, 0.f, 30.f, 11.f },
                { 2.f, 6.f, 11.5f, 24.f, 10.f },
            };
            Vec3 stackOrigin(b.position.x + 18.f, 0.f, b.position.y + 2.f);
            for (const auto& lg : extraLogs) {
                Vec3 lp(stackOrigin.x + lg.dx, lg.dy, stackOrigin.z + lg.dz);
                addBox(out, viewProj, windowSize, eye, lp, Vec3(lg.len, lg.diam, lg.diam), logColor, lc);
                addBox(out, viewProj, windowSize, eye, Vec3(lp.x - 4.f, lp.y, lp.z), Vec3(8.f, lg.diam + 0.5f, lg.diam), logCapColor, lc);
            }
        }

        // Tree stumps -- a base box + a lighter, flat "cut surface" cap on
        // top (the up-facing equivalent of the log piles' own end-cap
        // trick), scattered in the cleared area south of the main pile.
        const sf::Vector2f stumps[] = { { 48.f, 72.f }, { 68.f, 68.f }, { 88.f, 74.f } };
        for (const auto& sp : stumps) {
            Vec3 sPos(b.position.x + sp.x, 0.f, b.position.y + sp.y);
            addBox(out, viewProj, windowSize, eye, sPos, Vec3(10.f, 9.f, 10.f), stumpColor, lc);
            addBox(out, viewProj, windowSize, eye, sPos + Vec3(0.f, 9.f, 0.f), Vec3(10.f, 1.5f, 10.f), stumpCapColor, lc);
        }

        // Plank stack, east side.
        Vec3 plankBase(b.position.x + 92.f, 0.f, b.position.y + 10.f);
        for (int i = 0; i < 5; ++i) {
            addBox(out, viewProj, windowSize, eye, plankBase + Vec3(0.f, static_cast<float>(i) * 2.6f, 0.f), Vec3(16.f, 2.2f, 14.f), shade3d(plankColor, (i % 2) * 8 - 4), lc);
        }

        // Small steam-powered loader -- a boiler body + smokestack + smoke
        // puff (reusing the exact glow infrastructure every chimney in this
        // file already relies on), plus a simple crane arm reaching toward
        // the main pile with a log dangling from it.
        Vec3 boilerPos(b.position.x + 96.f, 0.f, b.position.y + 40.f);
        addBox(out, viewProj, windowSize, eye, boilerPos, Vec3(14.f, 24.f, 12.f), craneColor, lc);
        addBox(out, viewProj, windowSize, eye, boilerPos + Vec3(4.f, 24.f, 3.f), Vec3(6.f, 10.f, 6.f), shade3d(craneColor, -15), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, boilerPos + Vec3(7.f, 38.f, 6.f), 16.f, glowTex, smokeColor);
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 2.f, 0.f, boilerPos.z - 8.f), Vec3(4.f, 34.f, 4.f), beamColor, lc); // arm post
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 26.f, 30.f, boilerPos.z - 7.f), Vec3(26.f, 4.f, 4.f), beamColor, lc); // arm reaching toward the pile
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 24.f, 14.f, boilerPos.z - 6.f), Vec3(2.f, 16.f, 2.f), beamColor, lc); // hoist chain
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 30.f, 6.f, boilerPos.z - 9.f), Vec3(14.f, 10.f, 10.f), logColor, lc); // the log it's hoisting

        // Small lean-to shed -- 4 posts + a flat roof slab, Kitchen's own
        // porch-roof technique, no walls (this stays "raised props", not a
        // secretly-enclosed building -- see this function's own header
        // comment).
        float shedX0 = b.position.x + 16.f, shedZ0 = b.position.y + 64.f, shedW = 18.f, shedD = 14.f, shedH = 26.f;
        for (float px : { shedX0 + 2.f, shedX0 + shedW - 2.f }) {
            for (float pz : { shedZ0 + 2.f, shedZ0 + shedD - 2.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(3.f, shedH, 3.f), beamColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(shedX0, shedH, shedZ0), Vec3(shedW, 3.f, shedD), roofColor, lc);
    }

    // v2 (2026-08-10, from the user's own reference image -- a working
    // quarry: an excavation pit worked by heavy equipment, loose rock
    // rubble, a neat stack of cut stone blocks, a crane hoisting a block,
    // and log/plank clutter). Scope-checked via AskUserQuestion first (same
    // "which business does this reference actually mean" caution
    // addSawmillBuilding's own header comment describes needing for the
    // 伐木场/锯木厂 mix-up) -- the user confirmed the reference maps to
    // "quarry" itself (tier-1 raw material) rather than "mason" (tier-2
    // stone processing), so like addLumberProps' own v2 rewrite this stays
    // in the flat-plot-archetype family (ground fill + addPlotBorder +
    // raised props), not an enclosed building.
    void addQuarryProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex) {
        sf::Color groundColor(98, 98, 102);
        sf::Color pitFloorColor(182, 176, 164);
        sf::Color pitWallColor(120, 118, 116);
        sf::Color rockColor(152, 152, 156);
        sf::Color rockColorAlt(134, 132, 130);
        sf::Color cutStoneColor(196, 190, 176);
        sf::Color craneColor(100, 96, 90);
        sf::Color beamColor(58, 40, 26);
        sf::Color cartColor(118, 86, 52);
        sf::Color wheelColor(60, 44, 28);
        sf::Color roofColor(90, 56, 38);
        sf::Color smokeColor(210, 210, 214, 90);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, groundColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // ---- The excavation pit: a recessed floor (same "second, narrower
        // ground quad layered on top of the first" trick Farm's own crop
        // rows already rely on) plus 2 interior walls on the north/west
        // sides only -- the sides a camera looking south-to-north over the
        // pit's own near (south) lip actually looks INTO. The south edge is
        // the open mouth of the pit (where you'd stand at the rim), so it
        // deliberately gets no wall -- same reasoning real quarries need no
        // "wall" at the side you walk in from. Kept to one depth tier
        // rather than stepped terraces -- a multi-tier band needs each
        // band's own height to sum exactly (see this file's Town Hall
        // gap-bug lesson) and there's no way to check that by eye here. ----
        float pitX0 = b.position.x + 12.f, pitZ0 = b.position.y + 8.f, pitW = 50.f, pitD = 46.f, pitDepth = 16.f;
        addGroundQuad(out, viewProj, windowSize, eye, pitX0, pitZ0, pitW, pitD, -pitDepth, pitFloorColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(pitX0, -pitDepth, pitZ0), Vec3(pitW, pitDepth, 6.f), pitWallColor, lc);              // north interior wall (far side, faces the camera)
        addBox(out, viewProj, windowSize, eye, Vec3(pitX0, -pitDepth, pitZ0), Vec3(6.f, pitDepth, pitD), shade3d(pitWallColor, -10), lc); // west interior wall, a touch darker (catches less direct sun than the north-facing one)

        // Loose boulders on solid ground just south of the pit's own lip --
        // same "individual pieces with real gaps, not touching" lesson
        // addLumberProps' own log-pile fix landed on, applied to irregular
        // rock chunks instead of logs. Kept clear of the pit's own z-range
        // (ends at z=54) so they read as sitting on solid ground at the rim,
        // not floating over the hole.
        {
            struct RockSpec { float dx, dz, dy, w, h, d; sf::Color c; };
            const RockSpec lipRocks[] = {
                { 20.f, 58.f, 0.f, 14.f, 10.f, 12.f, rockColor },
                { 36.f, 62.f, 0.f, 11.f, 8.f, 10.f, rockColorAlt },
                { 26.f, 68.f, 0.f, 9.f, 7.f, 9.f, rockColor },
            };
            for (const auto& r : lipRocks) {
                addBox(out, viewProj, windowSize, eye, Vec3(b.position.x + r.dx, r.dy, b.position.y + r.dz), Vec3(r.w, r.h, r.d), r.c, lc);
            }
        }

        // East-side rubble pile -- a loose cluster of already-quarried rock
        // chunks awaiting the cart, varied sizes with real gaps between them
        // (one nestled on top of the gap between the other two, same idea
        // as the lumber pile's own stacking).
        {
            struct RockSpec { float dx, dz, dy, w, h, d; sf::Color c; };
            const RockSpec pile[] = {
                { 66.f, 8.f,  0.f,  16.f, 12.f, 14.f, rockColor },
                { 74.f, 22.f, 0.f,  13.f, 10.f, 12.f, rockColorAlt },
                { 68.f, 24.f, 12.f, 10.f, 8.f,  9.f,  rockColor },
                { 80.f, 10.f, 0.f,  12.f, 9.f,  11.f, rockColorAlt },
            };
            for (const auto& r : pile) {
                addBox(out, viewProj, windowSize, eye, Vec3(b.position.x + r.dx, r.dy, b.position.y + r.dz), Vec3(r.w, r.h, r.d), r.c, lc);
            }
        }

        // Cut stone block stack -- neat, pale, already-squared blocks
        // (distinct from the rough rubble pile above), reusing
        // addLumberProps' own plank-stack layering technique.
        Vec3 stackBase(b.position.x + 90.f, 0.f, b.position.y + 6.f);
        for (int i = 0; i < 4; ++i) {
            addBox(out, viewProj, windowSize, eye, stackBase + Vec3(0.f, static_cast<float>(i) * 6.6f, 0.f), Vec3(18.f, 6.f, 20.f), shade3d(cutStoneColor, (i % 2) * 6 - 3), lc);
        }

        // Steam-powered hoist crane -- the exact boiler/smokestack/arm/
        // hoist-chain assembly addLumberProps' own loader already
        // established, dangling a cut stone block instead of a log.
        Vec3 boilerPos(b.position.x + 96.f, 0.f, b.position.y + 40.f);
        addBox(out, viewProj, windowSize, eye, boilerPos, Vec3(14.f, 24.f, 12.f), craneColor, lc);
        addBox(out, viewProj, windowSize, eye, boilerPos + Vec3(4.f, 24.f, 3.f), Vec3(6.f, 10.f, 6.f), shade3d(craneColor, -15), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, boilerPos + Vec3(7.f, 38.f, 6.f), 16.f, glowTex, smokeColor);
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 2.f, 0.f, boilerPos.z - 8.f), Vec3(4.f, 34.f, 4.f), beamColor, lc);           // arm post
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 26.f, 30.f, boilerPos.z - 7.f), Vec3(26.f, 4.f, 4.f), beamColor, lc);          // arm reaching toward the rubble pile
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 24.f, 14.f, boilerPos.z - 6.f), Vec3(2.f, 16.f, 2.f), beamColor, lc);           // hoist chain
        addBox(out, viewProj, windowSize, eye, Vec3(boilerPos.x - 30.f, 6.f, boilerPos.z - 9.f), Vec3(12.f, 10.f, 12.f), cutStoneColor, lc);      // the stone block it's hoisting

        // Small ore cart -- a body box + a single crude wheel block (no
        // curved primitive in this renderer, same call every barrel/cart
        // shape here already makes), loaded with a couple of small chunks.
        Vec3 cartPos(b.position.x + 68.f, 0.f, b.position.y + 60.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(2.f, 8.f, 2.f), Vec3(6.f, 5.f, 5.f), rockColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(8.f, 8.f, 3.f), Vec3(5.f, 4.f, 4.f), rockColorAlt, lc);

        // Small lean-to tool shed, west side -- 4 posts + a flat roof slab,
        // Kitchen/addLumberProps' own porch-roof technique, no walls (stays
        // "raised props", not a secretly-enclosed building).
        float shedX0 = b.position.x + 6.f, shedZ0 = b.position.y + 58.f, shedW = 18.f, shedD = 14.f, shedH = 24.f;
        for (float px : { shedX0 + 2.f, shedX0 + shedW - 2.f }) {
            for (float pz : { shedZ0 + 2.f, shedZ0 + shedD - 2.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(3.f, shedH, 3.f), beamColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(shedX0, shedH, shedZ0), Vec3(shedW, 3.f, shedD), roofColor, lc);
    }

    void addPastureProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& sheepTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(102, 140, 70), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        Vec3 railPos(b.position.x + 4.f, 0.f, b.position.y + b.size.y - 22.f);
        addBox(out, viewProj, windowSize, eye, railPos, Vec3(b.size.x - 8.f, 6.f, 4.f), sf::Color(150, 118, 76), lc);
        for (float x = b.position.x + 4.f; x < b.position.x + b.size.x - 2.f; x += 16.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(x, 0.f, b.position.y + b.size.y - 16.f), Vec3(4.f, 22.f, 4.f), sf::Color(150, 118, 76), lc);
        }
        const sf::Vector2f puffs[] = { { 0.30f, 0.35f }, { 0.55f, 0.50f }, { 0.72f, 0.30f } };
        for (const auto& pf : puffs) {
            Vec3 p(b.position.x + b.size.x * pf.x, 0.f, b.position.y + b.size.y * pf.y);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 26.f, 22.f, sheepTex, dayNightTint);
        }
    }

    void addOrchardProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fruitTreeTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(90, 130, 66), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                Vec3 p(b.position.x + b.size.x * (0.2f + 0.3f * static_cast<float>(col)), 0.f,
                    b.position.y + b.size.y * (0.35f + 0.4f * static_cast<float>(row)));
                addBillboard(out, viewProj, windowSize, billboardRight, p, 32.f, 42.f, fruitTreeTex, dayNightTint);
            }
        }
    }

    void addHerbGardenProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& herbTuftTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(74, 58, 40), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        const sf::Vector2f offsets[] = {
            { 0.15f, 0.25f }, { 0.35f, 0.55f }, { 0.55f, 0.30f }, { 0.75f, 0.60f },
            { 0.25f, 0.75f }, { 0.65f, 0.20f }, { 0.85f, 0.40f }, { 0.45f, 0.80f },
        };
        for (const auto& off : offsets) {
            Vec3 p(b.position.x + b.size.x * off.x, 0.f, b.position.y + b.size.y * off.y);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 20.f, 20.f, herbTuftTex, dayNightTint);
        }
    }

    void addVineyardProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& grapeTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(107, 84, 48), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        constexpr int rows = 4;
        float gap = b.size.x / static_cast<float>(rows);
        for (int i = 0; i < rows; ++i) {
            float x = b.position.x + gap * static_cast<float>(i) + gap * 0.5f;
            // A low trellis rail running the row's full depth -- the 2D
            // version draws this as a thin top-down line the same length;
            // in 3D that's a short, shallow box rather than a tall post.
            addBox(out, viewProj, windowSize, eye, Vec3(x - 2.f, 0.f, b.position.y + 5.f), Vec3(4.f, 24.f, b.size.y - 10.f), sf::Color(120, 90, 55), lc);
            for (int j = 0; j < 3; ++j) {
                float z = b.position.y + 15.5f + static_cast<float>(j) * 16.f;
                Vec3 p(x, 12.f, z);
                addBillboard(out, viewProj, windowSize, billboardRight, p, 10.f, 10.f, grapeTex, dayNightTint);
            }
        }
    }

    // A thin timber-frame beam lying flat IN THE PLANE of a south-facing
    // wall (fixed world Z == wallZ), from wall-local 2D point p1 to p2 --
    // half-timbered corner posts/rails/X-braces (see
    // addRecruitmentCenterBuilding below) are decorative surface elements
    // applied against the wall, not separate volumes sticking out of it, so
    // a single coplanar quad (offset a couple units in front of the wall
    // fill to avoid z-fighting) is both the correct look and far cheaper
    // than a proper oriented 3D beam. `p1`/`p2` are (x, y) in world X /
    // world-up Y -- Z is fixed at `wallZ` for both ends.
    void addFacadeBeam(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        float wallZ, sf::Vector2f p1, sf::Vector2f p2, float thickness, sf::Color color, const LightingContext& lc) {
        float dx = p2.x - p1.x, dy = p2.y - p1.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) return;
        float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
        Vec3 a(p1.x + nx, p1.y + ny, wallZ), b(p2.x + nx, p2.y + ny, wallZ);
        Vec3 c(p2.x - nx, p2.y - ny, wallZ), d(p1.x - nx, p1.y - ny, wallZ);
        addFace(out, viewProj, windowSize, eye, a, b, c, d, Vec3(0.f, 0.f, 1.f), color, lc);
    }

    // Same idea as addFacadeBeam but for a wall at fixed world X (the east/
    // west side walls) instead of fixed Z -- `p1`/`p2` are (z, y) wall-local
    // points. Whichever side wall the camera can actually see depends on
    // where the player currently is (see getZoneCamera3D's panning) --
    // addFace's own back-face cull already handles that correctly as long
    // as `normal` is the true (+1,0,0)/(-1,0,0) for whichever side this
    // call is for, so both sides can just always be added unconditionally.
    void addFacadeBeamX(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        float wallX, sf::Vector2f p1, sf::Vector2f p2, float thickness, Vec3 normal, sf::Color color, const LightingContext& lc) {
        float dz = p2.x - p1.x, dy = p2.y - p1.y;
        float len = std::sqrt(dz * dz + dy * dy);
        if (len < 1e-3f) return;
        float nz = -dy / len * thickness * 0.5f, ny = dz / len * thickness * 0.5f;
        Vec3 a(wallX, p1.y + ny, p1.x + nz), b(wallX, p2.y + ny, p2.x + nz);
        Vec3 c(wallX, p2.y - ny, p2.x - nz), d(wallX, p1.y - ny, p1.x - nz);
        addFace(out, viewProj, windowSize, eye, a, b, c, d, normal, color, lc);
    }

    // Cottage (the shared "sleep"/"eat"/"doctor" placeholder shape) is fully
    // retired as of the Clinic hero building below -- "sleep" and "eat" had
    // already moved to their own addInnBuilding/addKitchenBuilding, and
    // "doctor" (its last remaining user) moves to addClinicBuilding here, so
    // there are no callers left. Removed rather than left as dead code.

    // Staff Office / "Adventurer Recruitment Center" -- the game's first
    // hand-styled hero building (2026-08-07, from the user's own reference
    // image, v2 after their "still too boxy, don't get locked into single-
    // box construction" feedback on v1). Two real volumes now instead of
    // one box: a taller main block with a front-facing (camera-facing)
    // timber-braced gable peak and a jettied (overhanging) upper floor, and
    // a shorter right-side wing that pokes out further south than the main
    // block with its own roof, a balcony, and hanging banners -- matching
    // the reference's asymmetric massing ("the right side sticks out more")
    // instead of one flat rectangular footprint. Only `b.id == "staff"`
    // uses this -- see this file's header comment for what's still generic.
    // Still not a literal recreation: no oriented/rotated-box primitive
    // exists in this renderer (addFacadeBeam/addFacadeBeamX fake diagonal
    // timber as flat coplanar quads on a wall instead, which only works
    // because real half-timbering IS basically a flat applied pattern), and
    // the reference's own sign text/exact banner count aren't reproduced.
    void addRecruitmentCenterBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plaster(214, 196, 158); // warmer/more yellow than Cottage's plaster -- reads as a distinct building material up against it
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color doorColor(80, 52, 28);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);
        sf::Color bannerColor(176, 40, 40);

        // A wall-plane beam call bound to a given fixed-Z plane -- the main
        // block uses two different planes (the ground floor's own southZ,
        // and the jettied upper floor/gable's own southZ further out), so
        // this takes the plane as a parameter instead of capturing one.
        auto beamAt = [&](float wallZ, float x1, float y1, float x2, float y2, float thick) {
            addFacadeBeam(out, viewProj, windowSize, eye, wallZ, sf::Vector2f(b.position.x + x1, y1), sf::Vector2f(b.position.x + x2, y2), thick, beamColor, lc);
        };

        // ---- Main block: taller, front-gabled, upper floor jetties out ----
        float mainW = b.size.x * 0.60f;
        Vec3 mainPos(b.position.x, 0.f, b.position.y);

        float wallH2 = wallH * 1.15f; // this hero building reads taller/grander than a plain ServiceHall box
        float foundationH = wallH2 * 0.12f;
        float groundWallH = wallH2 * 0.36f;
        float trimH = 5.f;
        float upperWallH = wallH2 - foundationH - groundWallH - trimH;
        float wallTop = wallH2;
        float gableRise = roofRise * 1.5f; // steeper peak than the generic buildings' roof -- a dramatic front gable is the whole point of this shape
        float jettyDepth = 10.f;           // how far the upper floor overhangs the ground floor toward the camera
        float mainDepthJettied = b.size.y + jettyDepth;

        // uvWorldPerTile tuned per material so its texture repeats a
        // believable number of times across a wall this size, instead of
        // one tile stretched blurrily over the whole thing (too big) or so
        // many repeats it reads as static noise (too small) -- see each
        // texture's own cell count in draw3DZone's bake calls.
        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        addBandedBox(out, viewProj, windowSize, eye, mainPos, Vec3(mainW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(mainPos.x, foundationH, mainPos.z), Vec3(mainW, groundWallH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);
        // Jetty trim -- a dark band at the overhang transition (real half-
        // timbered jetties have exactly this kind of trim board) sized to
        // the WIDER jettied footprint, bridging the step between the ground
        // floor's depth and the upper floor's.
        Vec3 trimPos(mainPos.x, foundationH + groundWallH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(mainW, trimH, mainDepthJettied), beamColor, lc);
        Vec3 upperWallPos(mainPos.x, foundationH + groundWallH + trimH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperWallPos, Vec3(mainW, upperWallH, mainDepthJettied), plaster, lc, &plasterTex, kPlasterUv);

        addFrontGableRoof(out, viewProj, windowSize, eye, Vec3(mainPos.x, 0.f, mainPos.z), sf::Vector2f(mainW, mainDepthJettied), wallTop, gableRise, roofColor, lc, &shingleTex, kShingleUv);

        // Gable end walls -- south (camera-facing) gets the reference's
        // timber-braced pediment; north is the same flat wall, just always
        // back-face-culled from this camera, kept for correctness.
        float midXlocal = mainW * 0.5f;
        Vec3 gsw(mainPos.x, wallTop, mainPos.z + mainDepthJettied), gse(mainPos.x + mainW, wallTop, mainPos.z + mainDepthJettied);
        Vec3 gnw(mainPos.x, wallTop, mainPos.z), gne(mainPos.x + mainW, wallTop, mainPos.z);
        Vec3 ridgeS(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z + mainDepthJettied);
        Vec3 ridgeN(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z);
        addTri(out, viewProj, windowSize, eye, gsw, gse, ridgeS, Vec3(0.f, 0.f, 1.f), plaster, lc, &plasterTex, kPlasterUv);
        addTri(out, viewProj, windowSize, eye, gne, gnw, ridgeN, Vec3(0.f, 0.f, -1.f), plaster, lc, &plasterTex, kPlasterUv);

        // Shingle-row texture on both roof slopes, and a ridge cap beam --
        // addFrontGableRoof's own fill is now the real shingle texture, and
        // these bands layer real GEOMETRIC row structure on top of that
        // (see addShingleRows' own comment) -- texture for fine per-pixel
        // grain, real lit geometry for the macro row shape/shadowing,
        // together instead of either alone.
        {
            float run = mainW * 0.5f;
            Vec3 westN = Vec3(-gableRise, run, 0.f).normalized(), eastN = Vec3(gableRise, run, 0.f).normalized();
            addShingleRows(out, viewProj, windowSize, eye, gsw, gnw, ridgeS, ridgeN, westN, roofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, gse, gne, ridgeS, ridgeN, eastN, roofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(ridgeN.x - 2.5f, ridgeN.y - 1.5f, ridgeN.z), Vec3(5.f, 5.f, mainDepthJettied), shade3d(roofColor, -22), lc);
        }

        float upperSouthZ = mainPos.z + mainDepthJettied + 1.5f;
        float southZ = mainPos.z + b.size.y + 1.5f;

        // King-post + 2 diagonal braces up into the gable peak -- continues
        // the timber theme up past the roofline instead of stopping at the
        // eave, matching the reference's cross-braced pediment.
        beamAt(upperSouthZ, midXlocal, wallTop, midXlocal, wallTop + gableRise - 6.f, 7.f);
        beamAt(upperSouthZ, 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(upperSouthZ, mainW - 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);

        // Ground-floor timber: corner posts + a rail just under the trim.
        beamAt(southZ, 6.f, foundationH, 6.f, foundationH + groundWallH - 2.f, 7.f);
        beamAt(southZ, mainW - 6.f, foundationH, mainW - 6.f, foundationH + groundWallH - 2.f, 7.f);
        beamAt(southZ, 6.f, foundationH + groundWallH - 4.f, mainW - 6.f, foundationH + groundWallH - 4.f, 6.f);

        // Upper (jettied) floor timber: corner posts + a dense X across the
        // whole band -- this is the band the reference shows most densely
        // braced, directly under the gable.
        float upperBase = foundationH + groundWallH + trimH, upperTop = wallTop - 2.f, upperMidY = (upperBase + upperTop) * 0.5f;
        beamAt(upperSouthZ, 6.f, upperBase, 6.f, upperTop, 7.f);
        beamAt(upperSouthZ, mainW - 6.f, upperBase, mainW - 6.f, upperTop, 7.f);
        beamAt(upperSouthZ, 6.f, upperMidY, midXlocal, upperTop, 6.f);
        beamAt(upperSouthZ, midXlocal, upperMidY, 6.f, upperTop, 6.f);
        beamAt(upperSouthZ, midXlocal, upperMidY, mainW - 6.f, upperTop, 6.f);
        beamAt(upperSouthZ, mainW - 6.f, upperMidY, midXlocal, upperTop, 6.f);

        // Door, centered on the ground floor, plus a flat awning box.
        float doorW = mainW * 0.22f, doorH = groundWallH * 0.92f;
        Vec3 doorPos(mainPos.x + mainW * 0.5f - doorW * 0.5f, 0.f, mainPos.z + b.size.y - 1.f);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 4.f), doorColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x - 5.f, doorH + 3.f, mainPos.z + b.size.y - 9.f),
            Vec3(doorW + 10.f, 4.f, 13.f), shade3d(roofColor, -12), lc);

        // Two multi-pane windows flanking the door -- a real "+" mullion
        // cross (not a diagonal X) through each pane for an actual 4-pane
        // read, plus a bloom halo at night.
        float winSize = mainW * 0.16f, winY = foundationH + (groundWallH - winSize) * 0.4f;
        float winXsLocal[2] = { mainW * 0.15f, mainW * 0.69f };
        for (float lx : winXsLocal) {
            Vec3 winPos(mainPos.x + lx, winY, mainPos.z + b.size.y - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            beamAt(southZ, lx, winY + winSize * 0.5f, lx + winSize, winY + winSize * 0.5f, 2.f);
            beamAt(southZ, lx + winSize * 0.5f, winY, lx + winSize * 0.5f, winY + winSize, 2.f);
            addGlowBillboard(out, viewProj, windowSize, billboardRight,
                Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, mainPos.z + b.size.y), winSize * 1.6f, glowTex, sf::Color(255, 214, 140, 150));
            // A pair of shutters flanking each pane -- cheap extra silhouette
            // detail on what would otherwise be a big blank stretch of wall
            // between the door and the corner posts.
            float shutterW = winSize * 0.4f;
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }

        // Small corbel blocks under the jetty trim -- reads as the timber
        // brackets actually holding an overhanging upper floor up, not just
        // a floating ledge.
        for (float cx = 10.f; cx < mainW - 6.f; cx += 18.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(mainPos.x + cx, foundationH + groundWallH - 6.f, mainPos.z + b.size.y - 5.f), Vec3(4.f, 6.f, 7.f), beamColor, lc);
        }

        // Hanging shop sign, mounted on the jetty trim over the door.
        float signW = mainW * 0.5f, signH = 18.f;
        Vec3 signPos(mainPos.x + mainW * 0.5f - signW * 0.5f, foundationH + groundWallH - 2.f, mainPos.z + b.size.y + 5.f);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x, signPos.y + signH, upperSouthZ - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x + signW - 2.5f, signPos.y + signH, upperSouthZ - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // Side-wall accents (east/west corner posts) -- whichever face the
        // camera can actually see depends on the player's position (see
        // addFacadeBeamX's comment), so both are always added and addFace's
        // own back-face cull sorts out which one renders.
        float sideZ0 = mainPos.z + b.size.y - 34.f, sideZ1 = mainPos.z + b.size.y - 4.f;
        addFacadeBeamX(out, viewProj, windowSize, eye, mainPos.x - 1.5f, sf::Vector2f(sideZ0, foundationH), sf::Vector2f(sideZ1, foundationH + groundWallH - 2.f), 6.f, Vec3(-1.f, 0.f, 0.f), beamColor, lc);
        addFacadeBeamX(out, viewProj, windowSize, eye, mainPos.x + mainW + 1.5f, sf::Vector2f(sideZ0, foundationH), sf::Vector2f(sideZ1, foundationH + groundWallH - 2.f), 6.f, Vec3(1.f, 0.f, 0.f), beamColor, lc);

        // ---- Right wing: shorter, pokes out further south than the main
        // block (the reference's asymmetric massing), its own side-gabled
        // roof for contrast against the main block's front gable, a
        // balcony deck with a railing, and 2 hanging banners. ----
        float wingX0 = mainPos.x + mainW;
        float wingW = b.size.x - mainW;
        float wingJetty = 18.f; // pokes out further south than even the main block's own jetty (10) -- "the right side sticks out more"
        float wingDepth = b.size.y + wingJetty;
        float wingWallH = wallH * 0.62f;
        float wingFoundationH = wingWallH * 0.16f;
        Vec3 wingPos(wingX0, 0.f, b.position.y);

        addBandedBox(out, viewProj, windowSize, eye, wingPos, Vec3(wingW, wingFoundationH, wingDepth), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(wingPos.x, wingFoundationH, wingPos.z), Vec3(wingW, wingWallH - wingFoundationH, wingDepth), plaster, lc, &plasterTex, kPlasterUv);
        float wingRoofRise = roofRise * 0.7f;
        sf::Color wingRoofColor = shade3d(roofColor, -6);
        addGableRoof(out, viewProj, windowSize, eye, wingPos, Vec3(wingW, wingWallH, wingDepth), wingWallH, wingRoofRise, wingRoofColor, lc, &shingleTex, kShingleUv);
        // Same shingle-row overlay + ridge cap as the main roof, on the
        // wing's own (side-gabled, ridge-along-X) slopes.
        {
            float wingMidZ = wingPos.z + wingDepth * 0.5f;
            Vec3 wRidgeW(wingPos.x, wingWallH + wingRoofRise, wingMidZ), wRidgeE(wingPos.x + wingW, wingWallH + wingRoofRise, wingMidZ);
            Vec3 wNw(wingPos.x, wingWallH, wingPos.z), wNe(wingPos.x + wingW, wingWallH, wingPos.z);
            Vec3 wSw(wingPos.x, wingWallH, wingPos.z + wingDepth), wSe(wingPos.x + wingW, wingWallH, wingPos.z + wingDepth);
            Vec3 wNormalN = cross(wRidgeE - wRidgeW, wNw - wRidgeW).normalized();
            Vec3 wNormalS = cross(wRidgeW - wRidgeE, wSe - wRidgeE).normalized();
            addShingleRows(out, viewProj, windowSize, eye, wNw, wNe, wRidgeW, wRidgeE, wNormalN, wingRoofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, wSw, wSe, wRidgeW, wRidgeE, wNormalS, wingRoofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(wRidgeW.x, wRidgeW.y - 1.5f, wRidgeW.z - 2.5f), Vec3(wingW, 5.f, 5.f), shade3d(wingRoofColor, -22), lc);
        }

        // Balcony deck -- a thin dark platform at roughly the main block's
        // floor-transition height, protruding south past the wing's own
        // wall face so it actually reads as a balcony, not a wall stripe.
        float balconyY = foundationH + groundWallH;
        float balconyDepth = 16.f;
        float balconyX = wingX0 + 4.f, balconyW = wingW - 8.f;
        Vec3 balconyPos(balconyX, balconyY, wingPos.z + wingDepth);
        addBandedBox(out, viewProj, windowSize, eye, balconyPos, Vec3(balconyW, 4.f, balconyDepth), beamColor, lc);

        float railTopY = balconyY + 18.f, railZ = balconyPos.z + balconyDepth - 3.f;
        for (float rx = balconyX + 3.f; rx < balconyX + balconyW - 2.f; rx += 10.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(rx, balconyY + 4.f, railZ), Vec3(3.f, 14.f, 3.f), beamColor, lc);
        }
        addBox(out, viewProj, windowSize, eye, Vec3(balconyX, railTopY, railZ), Vec3(balconyW, 3.f, 3.f), beamColor, lc);

        // 2 hanging banners near the balcony -- a short pole + a flat cloth
        // panel, one toward each end of the wing.
        float bannerZ = balconyPos.z - 6.f;
        float bannerXs[2] = { balconyX + 4.f, balconyX + balconyW - 13.f };
        for (float bx : bannerXs) {
            addBox(out, viewProj, windowSize, eye, Vec3(bx + 5.f, wingWallH - 4.f, bannerZ), Vec3(2.5f, 34.f, 2.5f), beamColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(bx, wingWallH - 30.f, bannerZ), Vec3(9.f, 26.f, 1.5f), bannerColor, lc);
        }

        // Chimney on the main block, offset near the wing junction.
        //
        // FIX (2026-08-07 detail pass, pre-emptive -- same bug class caught
        // on Kitchen's chimneys and Town Hall's flags: a flat constant Y
        // against `addFrontGableRoof`'s own sloped surface. The error here
        // was smaller (~3 world units, vs Kitchen's ~11 and Town Hall's
        // ~22) since 0.82 happens to land closer to where the flawed
        // constant was already roughly right, which is likely why it never
        // got reported -- but it's the exact same mistake, just a smaller
        // one, so fixed the same way rather than leaving it for a future
        // round to catch.) Also added the smoke puff every sibling
        // building's own chimney gets -- this one never had it. ----
        {
            float chimneyLocalX = mainW * 0.82f;
            float distFromRidge = std::abs(chimneyLocalX - midXlocal);
            float chimneyRoofY = wallTop + gableRise * (1.f - distFromRidge / midXlocal);
            Vec3 chimneyPos(mainPos.x + chimneyLocalX, chimneyRoofY, mainPos.z + b.size.y * 0.22f);
            addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, 28.f, 11.f), sf::Color(96, 90, 86), lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, 32.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));
        }

        // Stone steps up to the door (2026-08-07 detail pass) -- the same
        // "a real entrance has steps" read Town Hall/Clinic's own porches
        // already added, never carried over here.
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x - 6.f, 0.f, mainPos.z + b.size.y + 5.f), Vec3(doorW + 12.f, 3.f, 5.f), shade3d(stone, -10), lc);
    }

    // Bank -- second hero building (2026-08-07, from the user's own second
    // reference image: heavy corner masonry with quoin blocks, an OPEN
    // ground floor counter/alcove instead of a door -- a vault and a wall
    // of storage drawers visible inside instead of a closed facade -- a
    // solid stone upper floor, and a second sign wrapping onto the side
    // wall). Only `b.id == "bank"` uses this. Differentiated from Staff
    // Office on purpose (stone-dominant not plaster-dominant, a plain side-
    // gabled roof not a dramatic front gable, an open counter not a door)
    // so the two hero buildings don't just look like reskins of each other.
    void addBankBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex,
        const sf::Texture& vaultTex, const sf::Texture& cabinetTex) {
        sf::Color stone(122, 118, 112);
        sf::Color quoinStone(140, 136, 128); // a shade lighter than the wall stone -- quoins are meant to visually pop off the wall plane, not blend into it
        sf::Color plaster(198, 186, 160);
        sf::Color beamColor(58, 40, 26);
        sf::Color woodColor(112, 78, 44);
        sf::Color roofColor(78, 48, 34);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);

        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float wallH2 = wallH * 1.05f;
        float groundH = wallH2 * 0.5f;
        float trimH = 5.f;
        float upperH = wallH2 - groundH - trimH;
        float wallTop = wallH2;

        // ---- Ground floor: two stone piers with an OPEN counter/alcove
        // between them instead of a closed wall + door. ----
        float pierW = b.size.x * 0.15f;
        Vec3 westPier(basePos.x, 0.f, basePos.z), eastPier(basePos.x + b.size.x - pierW, 0.f, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, westPier, Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, eastPier, Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);

        float openW = b.size.x - pierW * 2.f;
        float openX = basePos.x + pierW;
        // Recessed back wall, set well north of the front face -- the piers
        // extend the full depth, but nothing fills the middle span in front
        // of this, so the counter reads as a real deep alcove instead of a
        // shallow dent in a flat wall.
        float backWallDepth = 14.f;
        Vec3 backWallPos(openX, 0.f, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, backWallPos, Vec3(openW, groundH * 0.86f, backWallDepth), stone, lc, &stoneTex, kStoneUv);
        float backWallFaceZ = backWallPos.z + backWallDepth;

        // Counter -- a long low wood box at the very front of the alcove.
        addBandedBox(out, viewProj, windowSize, eye, Vec3(openX, 0.f, basePos.z + b.size.y - 10.f), Vec3(openW, groundH * 0.32f, 10.f), woodColor, lc);

        // Vault + drawer-cabinet decals, mounted flat on the back wall's
        // south face -- camera-facing billboards, same convention as every
        // other sprite decal, work fine here since this face always points
        // at the camera (it's the wall the whole alcove opens onto).
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.28f, 0.f, backWallFaceZ), 30.f, 30.f, vaultTex, sf::Color::White);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.68f, 4.f, backWallFaceZ), 36.f, 32.f, cabinetTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.5f, groundH * 0.55f, basePos.z + b.size.y * 0.6f), 70.f, glowTex, sf::Color(255, 214, 140, 90));

        // ---- Trim + solid upper floor (full width -- unlike the ground
        // floor, nothing is open up here) with windows. ----
        Vec3 trimPos(basePos.x, groundH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(b.size.x, trimH, b.size.y), beamColor, lc);
        Vec3 upperPos(basePos.x, groundH + trimH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperPos, Vec3(b.size.x, upperH, b.size.y), stone, lc, &stoneTex, kStoneUv);

        float southZ = basePos.z + b.size.y + 1.5f;
        float winSize = b.size.x * 0.14f, winY = groundH + trimH + upperH * 0.32f;
        float winXsLocal[3] = { b.size.x * 0.16f, b.size.x * 0.5f - winSize * 0.5f, b.size.x * 0.84f - winSize };
        float shutterW = winSize * 0.4f;
        for (float lx : winXsLocal) {
            Vec3 winPos(basePos.x + lx, winY, basePos.z + b.size.y - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(basePos.x + lx, winY + winSize * 0.5f), sf::Vector2f(basePos.x + lx + winSize, winY + winSize * 0.5f), 2.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(basePos.x + lx + winSize * 0.5f, winY), sf::Vector2f(basePos.x + lx + winSize * 0.5f, winY + winSize), 2.f, beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, basePos.z + b.size.y), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 140));
            // Shutters (2026-08-07 detail pass) -- Staff Office/Storefront's
            // own convention, never carried over to Bank's own windows.
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }

        // ---- Roof: plain side-gable (ridge along X), deliberately less
        // dramatic than Staff Office's front gable -- the ground floor is
        // this building's whole point, the roof shouldn't compete with it. ----
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, wallTop, b.size.y), wallTop, roofRise * 0.85f, roofColor, lc, &shingleTex, kShingleUv);

        // ---- Corner quoins along the south-west corner -- alternating-
        // size stone blocks straddling the actual corner edge (both the
        // south and west faces at once), the classic "masonry corner"
        // detail from the reference. No oriented-box primitive exists here
        // (see this file's header comment), so each block is a plain cube
        // centered on the corner and protruding a couple units past BOTH
        // wall planes -- reads as wrapping the corner without needing one. ----
        {
            float qy = 0.f;
            bool big = true;
            while (qy < wallTop - 6.f) {
                float qh = big ? 17.f : 10.f, qs = big ? 15.f : 10.f;
                addBox(out, viewProj, windowSize, eye, Vec3(basePos.x - qs * 0.5f, qy, basePos.z + b.size.y - qs * 0.5f), Vec3(qs, qh - 1.f, qs), quoinStone, lc);
                qy += qh;
                big = !big;
            }
        }

        // ---- Hanging signs: main sign south, a second sign wrapping onto
        // the east side wall (the reference's two-street-frontage read). ----
        float signW = b.size.x * 0.46f, signH = 18.f;
        Vec3 signPos(basePos.x + b.size.x * 0.5f - signW * 0.5f, groundH + trimH + upperH * 0.7f, southZ - 1.f);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x, signPos.y + signH + 4.f, signPos.z - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x + signW - 2.5f, signPos.y + signH + 4.f, signPos.z - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        float eastX = basePos.x + b.size.x + 1.5f;
        float sideSignW = b.size.y * 0.34f, sideSignZ0 = basePos.z + b.size.y * 0.12f;
        addFacadeBeamX(out, viewProj, windowSize, eye, eastX, sf::Vector2f(sideSignZ0, groundH + trimH + 6.f), sf::Vector2f(sideSignZ0 + 4.f, groundH + trimH + 6.f + signH), 2.f, Vec3(1.f, 0.f, 0.f), beamColor, lc);
        addFacadeBeamX(out, viewProj, windowSize, eye, eastX, sf::Vector2f(sideSignZ0 + sideSignW - 4.f, groundH + trimH + 6.f), sf::Vector2f(sideSignZ0 + sideSignW, groundH + trimH + 6.f + signH), 2.f, Vec3(1.f, 0.f, 0.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(eastX - 1.f, groundH + trimH + 4.f, sideSignZ0), Vec3(3.f, signH, sideSignW), signColor, lc);
    }

    // Inn -- third hero building (2026-08-07, from the user's own third
    // reference image: a tall front-gabled timber building with a lower
    // "private residence" wing protruding out to the LEFT, a small picket
    // fence enclosing a little yard in front of that wing, a west-side
    // balcony with a railing and potted flowers overlooking it, and an OPEN
    // ground-floor check-in counter -- no door -- on the right, with a wall
    // of hanging room keys and a shelf of goods visible inside instead of a
    // closed facade). Only `b.id == "sleep"` uses this (the building itself
    // is renamed "Inn" -- see Localization.cpp's "sleep" entry -- alongside
    // the new paid room-tier sleep system, see Game::innTiers/trySleep).
    //
    // v2 (2026-08-07, after "I want the left-protruding wing and the little
    // fence too, and the counter should still read as a counter even though
    // it's open" feedback on v1's single-volume version): now a real 2-
    // volume building, same idea as Staff Office's main-block-plus-wing
    // massing but mirrored (wing on the west/left here, not the east) and
    // crossed with Bank's open-counter-alcove for the ground floor.
    void addInnBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex,
        const sf::Texture& keysTex, const sf::Texture& cabinetTex) {
        sf::Color stone(112, 108, 104);
        sf::Color plaster(206, 190, 152);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(84, 52, 36);
        sf::Color woodColor(112, 78, 44);
        sf::Color windowColor(255, 214, 140);
        sf::Color doorColor(80, 52, 28);
        sf::Color signColor(120, 84, 48);
        sf::Color plantPotColor(150, 90, 60);
        sf::Color plantColor(96, 150, 80);

        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        // ---- Left wing: shorter "private residence" volume, pokes out
        // south further than the main block -- and a small picket fence
        // enclosing a little yard in front of its own door. ----
        float wingW = b.size.x * 0.34f;
        Vec3 wingPos(b.position.x, 0.f, b.position.y);
        float wingWallH = wallH * 0.56f;
        float wingFoundationH = wingWallH * 0.15f;
        float wingJetty = 14.f; // pokes out further south than the main block's own 9-unit jetty below
        float wingDepth = b.size.y + wingJetty;

        addBandedBox(out, viewProj, windowSize, eye, wingPos, Vec3(wingW, wingFoundationH, wingDepth), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(wingPos.x, wingFoundationH, wingPos.z), Vec3(wingW, wingWallH - wingFoundationH, wingDepth), plaster, lc, &plasterTex, kPlasterUv);
        float wingRoofRise = roofRise * 0.6f;
        sf::Color wingRoofColor = shade3d(roofColor, -6);
        addGableRoof(out, viewProj, windowSize, eye, wingPos, Vec3(wingW, wingWallH, wingDepth), wingWallH, wingRoofRise, wingRoofColor, lc, &shingleTex, kShingleUv);
        {
            float wingMidZ = wingPos.z + wingDepth * 0.5f;
            Vec3 wRidgeW(wingPos.x, wingWallH + wingRoofRise, wingMidZ), wRidgeE(wingPos.x + wingW, wingWallH + wingRoofRise, wingMidZ);
            Vec3 wNw(wingPos.x, wingWallH, wingPos.z), wNe(wingPos.x + wingW, wingWallH, wingPos.z);
            Vec3 wSw(wingPos.x, wingWallH, wingPos.z + wingDepth), wSe(wingPos.x + wingW, wingWallH, wingPos.z + wingDepth);
            Vec3 wNormalN = cross(wRidgeE - wRidgeW, wNw - wRidgeW).normalized();
            Vec3 wNormalS = cross(wRidgeW - wRidgeE, wSe - wRidgeE).normalized();
            addShingleRows(out, viewProj, windowSize, eye, wNw, wNe, wRidgeW, wRidgeE, wNormalN, wingRoofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, wSw, wSe, wRidgeW, wRidgeE, wNormalS, wingRoofColor, lc, &shingleTex, kShingleUv);
        }

        // Residence door -- a real closed door, unlike the counter side.
        float wingDoorW = wingW * 0.3f, wingDoorH = (wingWallH - wingFoundationH) * 0.7f;
        Vec3 wingDoorPos(wingPos.x + wingW * 0.5f - wingDoorW * 0.5f, wingFoundationH, wingPos.z + wingDepth - 1.f);
        addBox(out, viewProj, windowSize, eye, wingDoorPos, Vec3(wingDoorW, wingDoorH, 4.f), doorColor, lc);
        // A couple of low steps down from the door.
        addBox(out, viewProj, windowSize, eye, Vec3(wingDoorPos.x - 3.f, 0.f, wingPos.z + wingDepth + 1.f), Vec3(wingDoorW + 6.f, 4.f, 5.f), stone, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(wingDoorPos.x - 5.f, -3.f, wingPos.z + wingDepth + 5.f), Vec3(wingDoorW + 10.f, 4.f, 5.f), stone, lc);

        // A small picket fence enclosing a little yard between the wing's
        // door and the street -- one run along the south (front) edge, two
        // short return runs along the sides.
        {
            auto fenceRun = [&](float x0, float z0, float x1, float z1) {
                float dx = x1 - x0, dz = z1 - z0;
                float len = std::sqrt(dx * dx + dz * dz);
                if (len < 1.f) return;
                int posts = std::max(2, static_cast<int>(len / 7.f));
                constexpr float postH = 11.f;
                for (int i = 0; i <= posts; ++i) {
                    float t = static_cast<float>(i) / static_cast<float>(posts);
                    addBox(out, viewProj, windowSize, eye, Vec3(x0 + dx * t - 1.f, 0.f, z0 + dz * t - 1.f), Vec3(2.f, postH, 2.f), beamColor, lc);
                }
                float railY = postH * 0.6f;
                if (std::abs(dx) >= std::abs(dz))
                    addBox(out, viewProj, windowSize, eye, Vec3(std::min(x0, x1), railY, z0 - 1.f), Vec3(len, 2.f, 2.f), beamColor, lc);
                else
                    addBox(out, viewProj, windowSize, eye, Vec3(x0 - 1.f, railY, std::min(z0, z1)), Vec3(2.f, 2.f, len), beamColor, lc);
            };
            float yardZ = wingPos.z + wingDepth + 16.f;
            float yardX0 = wingPos.x - 6.f, yardX1 = wingPos.x + wingW * 0.9f;
            fenceRun(yardX0, yardZ, yardX1, yardZ);                 // front edge
            fenceRun(yardX0, wingPos.z + wingDepth, yardX0, yardZ); // west return
            fenceRun(yardX1, wingPos.z + wingDepth, yardX1, yardZ); // east return
            // A couple of potted flowers just inside the fence line.
            for (float fx : { yardX0 + 8.f, yardX1 - 8.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(fx - 3.f, 0.f, yardZ - 6.f), Vec3(6.f, 5.f, 6.f), plantPotColor, lc);
                addBox(out, viewProj, windowSize, eye, Vec3(fx - 4.f, 5.f, yardZ - 7.f), Vec3(8.f, 6.f, 8.f), plantColor, lc);
            }
        }

        // ---- Main block (right side): the tall front-gabled counter
        // building, same recipe as Staff Office's main volume. ----
        float mainW = b.size.x - wingW;
        Vec3 mainPos(b.position.x + wingW, 0.f, b.position.y);
        float wallH2 = wallH * 1.12f;
        float groundH = wallH2 * 0.36f;
        float trimH = 5.f;
        float upperH = wallH2 - groundH - trimH;
        float wallTop = wallH2;
        float gableRise = roofRise * 1.4f;
        float jettyDepth = 9.f;
        float mainDepthJettied = b.size.y + jettyDepth;

        // Open check-in counter -- a solid pier against the wing on the
        // west, a solid pier at the east corner, open in between (still a
        // real counter desk + recessed shelf wall inside, not just empty
        // space -- see the addBillboard/addBandedBox calls below).
        float pierW = mainW * 0.16f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(mainPos.x, 0.f, mainPos.z), Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(mainPos.x + mainW - pierW, 0.f, mainPos.z), Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);

        float openW = mainW - pierW * 2.f, openX = mainPos.x + pierW;
        float backWallDepth = 14.f;
        Vec3 backWallPos(openX, 0.f, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, backWallPos, Vec3(openW, groundH * 0.82f, backWallDepth), plaster, lc, &plasterTex, kPlasterUv);
        float backWallFaceZ = backWallPos.z + backWallDepth;
        // The counter desk itself -- a real waist-high wood slab spanning
        // most of the opening, right at the front where a player walking up
        // would actually stand at it.
        addBandedBox(out, viewProj, windowSize, eye, Vec3(openX, 0.f, mainPos.z + b.size.y - 12.f), Vec3(openW, groundH * 0.34f, 12.f), woodColor, lc);

        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.28f, groundH * 0.58f, backWallFaceZ), 30.f, 20.f, keysTex, sf::Color::White);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.68f, 4.f, backWallFaceZ), 34.f, 30.f, cabinetTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.5f, groundH * 0.5f, mainPos.z + b.size.y * 0.6f), 60.f, glowTex, sf::Color(255, 214, 140, 90));

        // ---- Trim + jettied upper floor (timber-framed) + front gable ----
        Vec3 trimPos(mainPos.x, groundH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(mainW, trimH, mainDepthJettied), beamColor, lc);
        Vec3 upperPos(mainPos.x, groundH + trimH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperPos, Vec3(mainW, upperH, mainDepthJettied), plaster, lc, &plasterTex, kPlasterUv);

        addFrontGableRoof(out, viewProj, windowSize, eye, Vec3(mainPos.x, 0.f, mainPos.z), sf::Vector2f(mainW, mainDepthJettied), wallTop, gableRise, roofColor, lc, &shingleTex, kShingleUv);

        float midXlocal = mainW * 0.5f;
        Vec3 gsw(mainPos.x, wallTop, mainPos.z + mainDepthJettied), gse(mainPos.x + mainW, wallTop, mainPos.z + mainDepthJettied);
        Vec3 gnw(mainPos.x, wallTop, mainPos.z), gne(mainPos.x + mainW, wallTop, mainPos.z);
        Vec3 ridgeS(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z + mainDepthJettied);
        Vec3 ridgeN(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z);
        addTri(out, viewProj, windowSize, eye, gsw, gse, ridgeS, Vec3(0.f, 0.f, 1.f), plaster, lc, &plasterTex, kPlasterUv);
        addTri(out, viewProj, windowSize, eye, gne, gnw, ridgeN, Vec3(0.f, 0.f, -1.f), plaster, lc, &plasterTex, kPlasterUv);
        {
            float run = mainW * 0.5f;
            Vec3 westN = Vec3(-gableRise, run, 0.f).normalized(), eastN = Vec3(gableRise, run, 0.f).normalized();
            addShingleRows(out, viewProj, windowSize, eye, gsw, gnw, ridgeS, ridgeN, westN, roofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, gse, gne, ridgeS, ridgeN, eastN, roofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(ridgeN.x - 2.5f, ridgeN.y - 1.5f, ridgeN.z), Vec3(5.f, 5.f, mainDepthJettied), shade3d(roofColor, -22), lc);
        }

        float upperSouthZ = mainPos.z + mainDepthJettied + 1.5f;
        auto beamAt = [&](float wallZ, float x1, float y1, float x2, float y2, float thick) {
            addFacadeBeam(out, viewProj, windowSize, eye, wallZ, sf::Vector2f(mainPos.x + x1, y1), sf::Vector2f(mainPos.x + x2, y2), thick, beamColor, lc);
        };
        float upperBase = groundH + trimH, upperTop = wallTop - 2.f, upperMidY = (upperBase + upperTop) * 0.5f;
        beamAt(upperSouthZ, 6.f, upperBase, 6.f, upperTop, 7.f);
        beamAt(upperSouthZ, mainW - 6.f, upperBase, mainW - 6.f, upperTop, 7.f);
        beamAt(upperSouthZ, 6.f, upperMidY, midXlocal, upperTop, 6.f);
        beamAt(upperSouthZ, midXlocal, upperMidY, 6.f, upperTop, 6.f);
        beamAt(upperSouthZ, midXlocal, upperMidY, mainW - 6.f, upperTop, 6.f);
        beamAt(upperSouthZ, mainW - 6.f, upperMidY, midXlocal, upperTop, 6.f);
        beamAt(upperSouthZ, midXlocal, wallTop, midXlocal, wallTop + gableRise - 6.f, 7.f); // king post
        beamAt(upperSouthZ, 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(upperSouthZ, mainW - 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);

        float winSize = mainW * 0.16f, winY = upperBase + (upperTop - upperBase) * 0.28f;
        float winXsLocal[2] = { mainW * 0.18f, mainW * 0.66f };
        float shutterW = winSize * 0.4f;
        for (float lx : winXsLocal) {
            Vec3 winPos(mainPos.x + lx, winY, upperSouthZ - 1.5f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            beamAt(upperSouthZ, lx, winY + winSize * 0.5f, lx + winSize, winY + winSize * 0.5f, 2.f);
            beamAt(upperSouthZ, lx + winSize * 0.5f, winY, lx + winSize * 0.5f, winY + winSize, 2.f);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, upperSouthZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 140));
            // Shutters (2026-08-07 detail pass) -- Staff Office/Bank/
            // Storefront's own convention, never carried over to Inn.
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }

        // ---- Balcony on the west side of the main block's upper floor --
        // now sits directly over the wing's own roof, railing + a couple of
        // potted flower boxes (the reference's own balcony planters). ----
        float balconyW = 16.f, balconyDepth = b.size.y * 0.5f;
        Vec3 balconyPos(mainPos.x - balconyW, upperBase, mainPos.z + b.size.y * 0.2f);
        addBandedBox(out, viewProj, windowSize, eye, balconyPos, Vec3(balconyW, 4.f, balconyDepth), beamColor, lc);
        float railTopY = upperBase + 4.f + 16.f;
        for (float rz = balconyPos.z + 3.f; rz < balconyPos.z + balconyDepth - 2.f; rz += 10.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(balconyPos.x + 2.f, upperBase + 4.f, rz), Vec3(3.f, 16.f, 3.f), beamColor, lc);
        }
        addBox(out, viewProj, windowSize, eye, Vec3(balconyPos.x + 2.f, railTopY, balconyPos.z), Vec3(3.f, 3.f, balconyDepth), beamColor, lc);
        for (float rz : { balconyPos.z + 8.f, balconyPos.z + balconyDepth - 14.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(balconyPos.x + 1.f, railTopY - 3.f, rz), Vec3(6.f, 4.f, 6.f), plantPotColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(balconyPos.x, railTopY + 1.f, rz - 1.f), Vec3(8.f, 5.f, 8.f), plantColor, lc);
        }

        // ---- Hanging sign, mounted on the jetty trim over the counter. ----
        float signW = mainW * 0.5f, signH = 18.f;
        Vec3 signPos(mainPos.x + mainW * 0.5f - signW * 0.5f, groundH - 2.f, mainPos.z + b.size.y + 5.f);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x, signPos.y + signH + 4.f, signPos.z - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x + signW - 2.5f, signPos.y + signH + 4.f, signPos.z - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // ---- Chimney ----
        //
        // FIX (2026-08-07 detail pass, pre-emptive -- same bug class caught
        // on Kitchen's chimneys, Town Hall's flags, and Staff Office's own
        // chimney: a flat constant Y against `addFrontGableRoof`'s sloped
        // surface, this time off by ~7 world units. Computed per-position
        // now, same fix, and added the smoke puff every sibling building's
        // own chimney gets -- this one never had it either. ----
        {
            float chimneyLocalX = mainW * 0.78f;
            float distFromRidge = std::abs(chimneyLocalX - midXlocal);
            float chimneyRoofY = wallTop + gableRise * (1.f - distFromRidge / midXlocal);
            Vec3 chimneyPos(mainPos.x + chimneyLocalX, chimneyRoofY, mainPos.z + b.size.y * 0.24f);
            addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, 26.f, 11.f), sf::Color(96, 90, 86), lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, 30.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));
        }
    }

    // Kitchen -- fourth hero building (2026-08-07, from the user's own
    // fourth reference image: a stone building with a rounded stone-arched
    // doorway, wall lanterns either side of it, a steep front-gabled roof
    // with an exposed king-post truss, a covered porch/awning on one side
    // with barrels and pottery underneath, 2 smoking chimneys, and outdoor
    // patio seating out front). Only `b.id == "eat"` uses this ("doctor",
    // the other id that used to share Cottage's shape with it, still does
    // for now). Reuses the same front-gable/timber-frame recipe as the
    // other 3 hero buildings; the one genuinely new piece is the arch --
    // this renderer has no curved-geometry primitive, so the rounded stone
    // doorway is a single flat billboard decal (`archTex`, drawn once via
    // getBillboard3D the same way the Bank's vault/Inn's keys are) mounted
    // on the wall, the same "fake it with a sprite" trick used everywhere
    // else a shape doesn't fit the box/beam toolkit.
    void addKitchenBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex, const sf::Texture& archTex) {
        sf::Color stone(126, 118, 106);
        sf::Color plaster(206, 190, 152);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color woodColor(112, 78, 44);
        sf::Color signColor(120, 84, 48);
        sf::Color chimneyColor(112, 106, 100);
        sf::Color smokeColor(210, 210, 214, 90);
        sf::Color windowColor(255, 214, 140);
        sf::Color quoinStone(150, 146, 138); // lighter than the base stone -- quoins pop off the wall plane, same call Bank's own quoins made

        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float wallH2 = wallH * 1.02f;
        float stoneWallH = wallH2 * 0.58f;   // reference reads stone-heavy low down
        float trimH = 5.f;
        float upperH = wallH2 - stoneWallH - trimH; // timber-framed band right under the eave/gable
        float wallTop = wallH2;
        float gableRise = roofRise * 1.35f;

        // ---- Walls: stone lower band, timber-framed upper band -- no
        // jetty/open counter here, this one's a plain closed building. ----
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, stoneWallH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        Vec3 trimPos(basePos.x, stoneWallH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(b.size.x, trimH, b.size.y), beamColor, lc);
        Vec3 upperPos(basePos.x, stoneWallH + trimH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperPos, Vec3(b.size.x, upperH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);

        addFrontGableRoof(out, viewProj, windowSize, eye, basePos, sf::Vector2f(b.size.x, b.size.y), wallTop, gableRise, roofColor, lc, &shingleTex, kShingleUv);

        float midXlocal = b.size.x * 0.5f;
        Vec3 gsw(basePos.x, wallTop, basePos.z + b.size.y), gse(basePos.x + b.size.x, wallTop, basePos.z + b.size.y);
        Vec3 gnw(basePos.x, wallTop, basePos.z), gne(basePos.x + b.size.x, wallTop, basePos.z);
        Vec3 ridgeS(basePos.x + midXlocal, wallTop + gableRise, basePos.z + b.size.y);
        Vec3 ridgeN(basePos.x + midXlocal, wallTop + gableRise, basePos.z);
        addTri(out, viewProj, windowSize, eye, gsw, gse, ridgeS, Vec3(0.f, 0.f, 1.f), plaster, lc, &plasterTex, kPlasterUv);
        addTri(out, viewProj, windowSize, eye, gne, gnw, ridgeN, Vec3(0.f, 0.f, -1.f), plaster, lc, &plasterTex, kPlasterUv);
        {
            float run = b.size.x * 0.5f;
            Vec3 westN = Vec3(-gableRise, run, 0.f).normalized(), eastN = Vec3(gableRise, run, 0.f).normalized();
            addShingleRows(out, viewProj, windowSize, eye, gsw, gnw, ridgeS, ridgeN, westN, roofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, gse, gne, ridgeS, ridgeN, eastN, roofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(ridgeN.x - 2.5f, ridgeN.y - 1.5f, ridgeN.z), Vec3(5.f, 5.f, b.size.y), shade3d(roofColor, -22), lc);
        }

        float southZ = basePos.z + b.size.y + 1.5f;
        auto beamAt = [&](float x1, float y1, float x2, float y2, float thick) {
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(basePos.x + x1, y1), sf::Vector2f(basePos.x + x2, y2), thick, beamColor, lc);
        };
        // Timber band: corner posts + an X across the whole upper band, and
        // the exposed king-post truss continuing up into the gable peak --
        // the reference's most visible timber detail.
        float upperBase = stoneWallH + trimH, upperTop = wallTop - 2.f, upperMidY = (upperBase + upperTop) * 0.5f;
        beamAt(6.f, upperBase, 6.f, upperTop, 7.f);
        beamAt(b.size.x - 6.f, upperBase, b.size.x - 6.f, upperTop, 7.f);
        beamAt(6.f, upperMidY, midXlocal, upperTop, 6.f);
        beamAt(midXlocal, upperMidY, 6.f, upperTop, 6.f);
        beamAt(midXlocal, upperMidY, b.size.x - 6.f, upperTop, 6.f);
        beamAt(b.size.x - 6.f, upperMidY, midXlocal, upperTop, 6.f);
        beamAt(midXlocal, wallTop, midXlocal, wallTop + gableRise - 6.f, 7.f); // king post
        beamAt(4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(b.size.x - 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);

        // ---- Arched stone doorway (see this function's header comment)
        // plus 2 wall lanterns flanking it. ----
        float archW = b.size.x * 0.26f, archH = stoneWallH * 0.92f;
        Vec3 archCenter(basePos.x + b.size.x * 0.5f, 0.f, southZ);
        addBillboard(out, viewProj, windowSize, billboardRight, archCenter, archW, archH, archTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenter.x - archW * 0.85f, archH * 0.55f, southZ), 20.f, glowTex, sf::Color(255, 200, 120, 150));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenter.x + archW * 0.85f, archH * 0.55f, southZ), 20.f, glowTex, sf::Color(255, 200, 120, 150));

        // ---- 2 stone-band windows flanking the arch, each with a real "+"
        // mullion, flanking shutters, and a bloom glow -- the reference
        // itself never called these out, but with the whole building
        // looked at again this was the one hero building with literally
        // zero traditional windows (2026-08-07 detail pass, "往hd-2d的方向
        // ...精细化" -- pushing this building further toward the same
        // surface-detail density the earlier hero buildings already got).
        // Kept inside the stone band alongside the arch -- the timber band
        // above is fully occupied by the king-post truss bracing already,
        // no room up there for a window.
        {
            float winSize = b.size.x * 0.13f, winY = stoneWallH * 0.32f;
            for (float lx : { b.size.x * 0.13f, b.size.x * 0.74f }) {
                Vec3 winPos(basePos.x + lx, winY, southZ - 1.f);
                addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
                beamAt(lx, winY + winSize * 0.5f, lx + winSize, winY + winSize * 0.5f, 2.f);
                beamAt(lx + winSize * 0.5f, winY, lx + winSize * 0.5f, winY + winSize, 2.f);
                addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 140));
                float shutterW = winSize * 0.4f;
                addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
                addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            }
        }

        // ---- Corner quoins on the south-east corner -- mirrors Bank's own
        // alternating-block corner detail exactly, same reasoning (a real
        // "masonry corner" read, no oriented-box primitive so each block
        // just straddles the corner). Only the east corner -- the west one
        // already has the porch's own posts/roofline sitting right there,
        // so a second stack of quoins on top of that would read as
        // cluttered rather than intentional. ----
        {
            float qy = 0.f;
            bool big = true;
            while (qy < stoneWallH - 6.f) {
                float qh = big ? 15.f : 9.f, qs = big ? 14.f : 9.f;
                addBox(out, viewProj, windowSize, eye, Vec3(basePos.x + b.size.x - qs * 0.5f, qy, basePos.z + b.size.y - qs * 0.5f), Vec3(qs, qh - 1.f, qs), quoinStone, lc);
                qy += qh;
                big = !big;
            }
        }

        // ---- Porch/awning on the west side: a flat roof slab on 2 posts,
        // with a couple of barrels underneath. ----
        float porchW = b.size.x * 0.28f, porchDepth = b.size.y * 0.42f, porchRoofY = stoneWallH * 0.62f;
        Vec3 porchPos(basePos.x - porchW, 0.f, basePos.z + b.size.y * 0.15f);
        addBox(out, viewProj, windowSize, eye, Vec3(porchPos.x, porchRoofY, porchPos.z), Vec3(porchW, 3.f, porchDepth), shade3d(roofColor, -10), lc);
        addBox(out, viewProj, windowSize, eye, Vec3(porchPos.x + 3.f, 0.f, porchPos.z + 3.f), Vec3(4.f, porchRoofY, 4.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(porchPos.x + 3.f, 0.f, porchPos.z + porchDepth - 7.f), Vec3(4.f, porchRoofY, 4.f), beamColor, lc);
        for (float bx : { porchPos.x + porchW * 0.35f, porchPos.x + porchW * 0.7f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(bx, 0.f, porchPos.z + porchDepth * 0.5f), Vec3(9.f, 13.f, 9.f), woodColor, lc);
        }

        // ---- Outdoor patio: a small table + 2 stools out front. ----
        float tableX = basePos.x + b.size.x * 0.78f, tableZ = southZ + 14.f;
        addBox(out, viewProj, windowSize, eye, Vec3(tableX - 8.f, 10.f, tableZ - 8.f), Vec3(16.f, 2.f, 16.f), woodColor, lc);
        for (float ix : { -1.f, 1.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(tableX + ix * 14.f - 4.f, 0.f, tableZ - 4.f), Vec3(8.f, 9.f, 8.f), woodColor, lc);
        }
        addBox(out, viewProj, windowSize, eye, Vec3(tableX - 3.f, 0.f, tableZ + 12.f), Vec3(6.f, 12.f, 6.f), sf::Color(150, 90, 60), lc); // a jar beside the table

        // ---- Hanging sign ----
        float signW = b.size.x * 0.48f, signH = 18.f;
        Vec3 signPos(basePos.x + b.size.x * 0.5f - signW * 0.5f, upperBase - 2.f, southZ + 4.f);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x, signPos.y + signH + 4.f, southZ - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(signPos.x + signW - 2.5f, signPos.y + signH + 4.f, southZ - 1.f), Vec3(2.5f, 8.f, 2.5f), beamColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // ---- 2 chimneys, each with a cap lip and a soft smoke puff.
        //
        // FIX HISTORY (2026-08-07): a flat constant Y (wrong for a sloped
        // roof) -> computed from the roof's own rise/run but with a stray
        // +3 standoff ("浮空了") -> min of the footprint's 2 edges (still
        // wrong -- see below). None of these were ever going to work,
        // because they all shared the same flawed premise: approximating a
        // box that sits on a SLOPE with a single flat Y. This renderer has
        // no depth buffer (see this file's own header comment -- painter's-
        // algorithm sort by whole-quad average depth, not per-pixel), so
        // the 3 fixes the user separately proposed (Alpha-Test/Cutout
        // material mode, a bottom sprite pivot, a "always use real 3D
        // models" rule) don't map onto this codebase: there's no material/
        // shader system here at all (SFML `sf::VertexArray` + hand-sorted
        // triangles, not a game-engine render pipeline), the chimney was
        // never a rotating camera-facing sprite to begin with (only trees/
        // bushes/people/decals are billboards -- every building including
        // this one is already real 3D box/face geometry), and it's already
        // built the same way the building's own walls are. The actual fix
        // has to happen at the geometry level, not a rendering-mode toggle.
        //
        // Real fix: instead of an axis-aligned box (flat bottom) resting on
        // a slope, build the chimney's own 4 side faces with a BOTTOM edge
        // that follows the same linear rise/run `addFrontGableRoof` itself
        // uses -- i.e. the chimney's base is shaped to match the roof it's
        // sitting on (exactly how a real chimney's base flashing is cut to
        // fit a sloped roof), not a flat plane pretending the roof under it
        // is flat. West/east faces stay flat vertical rectangles (each at
        // its own correct height, computed independently); north/south
        // faces and the top cap become slanted parallelograms connecting
        // those two heights, using the exact same rise/run normal-
        // derivation addFrontGableRoof's own slopes already use. No
        // approximation left to get wrong -- every point of the base is
        // its own exact roof height, so nothing can float OR embed. ----
        // FIX (2026-08-07, "我看烟囱好像直接不见掉了" -- a real bug, not
        // another approximation problem): this takes a WORLD-space X now
        // (the call sites below pass `basePos.x + cx`, needed to build
        // actual world-space face corners), but the distance-from-ridge
        // math here was still comparing that world X directly against
        // `b.size.x * 0.5f` -- a LOCAL half-width, not the ridge's actual
        // world X (`basePos.x + b.size.x*0.5`). For any non-trivial
        // basePos.x that's a wildly wrong distance (hundreds of units off),
        // which fed a wildly wrong height -- almost certainly placing both
        // chimneys' geometry so far below the roof (or behind/outside the
        // camera's near/far planes) that nothing visible was left to draw.
        // Fixed by subtracting the ridge's real world X.
        auto roofHeightAtX = [&](float worldX) {
            float dist = std::abs(worldX - (basePos.x + b.size.x * 0.5f));
            return wallTop + gableRise * (1.f - dist / (b.size.x * 0.5f));
        };
        auto addChimneyOnSlope = [&](float cx, float cz, float w, float d, float h, sf::Color baseColor) {
            float x0 = basePos.x + cx, x1 = basePos.x + cx + w;
            float z0 = cz, z1 = cz + d;
            float hW = roofHeightAtX(x0), hE = roofHeightAtX(x1); // "W"/"E" name the box's own two X edges, not compass direction -- whichever is ridge-ward just comes out taller automatically
            addFace(out, viewProj, windowSize, eye, Vec3(x0, hW, z0), Vec3(x0, hW, z1), Vec3(x0, hW + h, z1), Vec3(x0, hW + h, z0), Vec3(-1.f, 0.f, 0.f), baseColor, lc);
            addFace(out, viewProj, windowSize, eye, Vec3(x1, hE, z1), Vec3(x1, hE, z0), Vec3(x1, hE + h, z0), Vec3(x1, hE + h, z1), Vec3(1.f, 0.f, 0.f), baseColor, lc);
            addFace(out, viewProj, windowSize, eye, Vec3(x0, hW, z0), Vec3(x1, hE, z0), Vec3(x1, hE + h, z0), Vec3(x0, hW + h, z0), Vec3(0.f, 0.f, -1.f), baseColor, lc);
            addFace(out, viewProj, windowSize, eye, Vec3(x1, hE, z1), Vec3(x0, hW, z1), Vec3(x0, hW + h, z1), Vec3(x1, hE + h, z1), Vec3(0.f, 0.f, 1.f), baseColor, lc);
            // Top cap: same rise/run-derived normal addFrontGableRoof's own
            // slope faces use (works for either sign of hW-hE, i.e. whichever
            // edge happens to be ridge-ward), just over this box's own
            // narrower span instead of the whole roof's.
            Vec3 topNormal = Vec3(hW - hE, w, 0.f).normalized();
            addFace(out, viewProj, windowSize, eye, Vec3(x0, hW + h, z0), Vec3(x1, hE + h, z0), Vec3(x1, hE + h, z1), Vec3(x0, hW + h, z1), topNormal, baseColor, lc);
            return (hW + hE) * 0.5f + h; // average top height, for the cap lip/smoke to anchor off of -- doesn't need to be exact, both are small decorative extras
        };
        for (float cx : { b.size.x * 0.60f, b.size.x * 0.72f }) {
            float cz = basePos.z + b.size.y * 0.3f;
            float avgTopY = addChimneyOnSlope(cx, cz, 10.f, 10.f, 26.f, chimneyColor);
            Vec3 capPos(basePos.x + cx - 1.f, avgTopY, cz - 1.f);
            addBox(out, viewProj, windowSize, eye, capPos, Vec3(12.f, 3.f, 12.f), shade3d(chimneyColor, -15), lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, capPos + Vec3(6.f, 6.f, 6.f), 16.f, glowTex, smokeColor);
        }
    }

    // Town Hall -- fifth hero building, and the grandest one (2026-08-07,
    // from the user's own fifth reference image: a tall 2-floor stone-and-
    // timber municipal building with an arched entrance flanked by potted
    // topiaries and barrels, TWO rows of shuttered windows, several flags
    // on the roofline, a smoking chimney, and -- the one genuinely new
    // element -- an attached square clock tower with a pyramidal spire and
    // a clock face, taller than the main roofline). Only `b.id ==
    // "townhall"` uses this. Two side-by-side volumes again, same idea as
    // Staff Office/Inn's main-block-plus-wing massing, but the second
    // volume here is a tower, not a lower wing -- reuses `addPyramid`
    // (originally built for the Mine/Gold Mine mounds) for the spire
    // instead of a new primitive, and the clock face is another flat
    // billboard decal (`clockTex`), same "fake the round shape with a
    // sprite" trick as Kitchen's arched doorway.
    void addTownHallBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex,
        const sf::Texture& archTex, const sf::Texture& clockTex) {
        sf::Color stone(124, 116, 104);
        sf::Color plaster(198, 182, 148);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(88, 54, 36);
        sf::Color windowColor(255, 214, 140);
        sf::Color chimneyColor(112, 106, 100);
        sf::Color smokeColor(210, 210, 214, 90);
        sf::Color plantPotColor(150, 90, 60);
        sf::Color plantColor(90, 140, 76);
        sf::Color stepColor(150, 146, 138);
        sf::Color quoinStone(146, 138, 122); // lighter than the base stone -- quoins pop off the wall plane, same call Bank/Kitchen/Warehouse's own quoins made
        const sf::Color flagColors[3] = { sf::Color(60, 90, 170), sf::Color(176, 40, 40), sf::Color(210, 190, 60) };

        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        // v2 (2026-08-07, after the user's screenshot -- see below for the
        // real bug that caught, plus "left AND right lower buildings, all
        // stuck together, tower next to the shorter right one"): 4 volumes
        // side by side now, left wing / main block / right wing / tower,
        // all sharing the same b.position.y/b.size.y depth (no jetty) so
        // they're genuinely flush against each other instead of just
        // adjacent footprints.
        // Wings bumped wider (0.18->0.22) and taller (0.5->0.7) after "too
        // short/too narrow, looks squashed" feedback -- still clearly
        // shorter than the main block (wallH*1.3) so the height hierarchy
        // from the reference reads, just not as squat as v1. Tower widened
        // again after that (0.14->0.20, wingW nudged back to 0.20 to make
        // room) -- at 0.14 and ~215 units tall it was a >14:1 height:width
        // sliver that read as a flat line/flagpole rather than a real
        // volume ("更扁了" -- looks even flatter -- was this, not the
        // wings). See the tower's own section below for the matching
        // spire/height retune.
        float wingW = b.size.x * 0.20f;
        float towerW = b.size.x * 0.20f;
        float mainW = b.size.x - wingW * 2.f - towerW;
        float leftWingX0 = b.position.x;
        float mainX0 = leftWingX0 + wingW;
        float rightWingX0 = mainX0 + mainW;
        float towerX0 = rightWingX0 + wingW;

        // ---- Side wings: identical shorter side-gabled volumes flanking
        // the main block on both sides. ----
        float wingWallH = wallH * 0.7f;
        float wingFoundationH = wingWallH * 0.16f;
        float wingRoofRise = roofRise * 0.65f;
        sf::Color wingRoofColor = shade3d(roofColor, -8);
        // `isLeft` (2026-08-07 detail pass): only the LEFT wing's own
        // south-west corner is actually the outermost, exposed corner of
        // this whole 4-volume complex -- its east side butts flush against
        // the main block, and the right wing's own east side butts flush
        // against the tower, so neither of those 2 seams would ever show a
        // corner anyway. Quoins only make sense on the one corner that's
        // genuinely a corner.
        auto addSideWing = [&](float wingX0, bool isLeft) {
            Vec3 wp(wingX0, 0.f, b.position.y);
            addBandedBox(out, viewProj, windowSize, eye, wp, Vec3(wingW, wingFoundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
            addBandedBox(out, viewProj, windowSize, eye, Vec3(wp.x, wingFoundationH, wp.z), Vec3(wingW, wingWallH - wingFoundationH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);
            addGableRoof(out, viewProj, windowSize, eye, wp, Vec3(wingW, wingWallH, b.size.y), wingWallH, wingRoofRise, wingRoofColor, lc, &shingleTex, kShingleUv);
            float wMidZ = wp.z + b.size.y * 0.5f;
            Vec3 wRidgeW(wp.x, wingWallH + wingRoofRise, wMidZ), wRidgeE(wp.x + wingW, wingWallH + wingRoofRise, wMidZ);
            Vec3 wNw(wp.x, wingWallH, wp.z), wNe(wp.x + wingW, wingWallH, wp.z);
            Vec3 wSw(wp.x, wingWallH, wp.z + b.size.y), wSe(wp.x + wingW, wingWallH, wp.z + b.size.y);
            Vec3 wNormalN = cross(wRidgeE - wRidgeW, wNw - wRidgeW).normalized();
            Vec3 wNormalS = cross(wRidgeW - wRidgeE, wSe - wRidgeE).normalized();
            addShingleRows(out, viewProj, windowSize, eye, wNw, wNe, wRidgeW, wRidgeE, wNormalN, wingRoofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, wSw, wSe, wRidgeW, wRidgeE, wNormalS, wingRoofColor, lc, &shingleTex, kShingleUv);
            float wSize = wingW * 0.3f, wY = wingFoundationH + (wingWallH - wingFoundationH) * 0.3f;
            Vec3 winPos(wp.x + wingW * 0.5f - wSize * 0.5f, wY, wp.z + b.size.y - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(wSize, wSize, 3.f), windowColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + wSize * 0.5f, wY + wSize * 0.5f, wp.z + b.size.y), wSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
            // Shutters flanking the window (2026-08-07 detail pass) --
            // Staff Office/Bank/Storefront's own convention, never carried
            // over to Town Hall's wings.
            float shutterW = wSize * 0.4f;
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, wY, winPos.z), Vec3(shutterW, wSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + wSize + 2.f, wY, winPos.z), Vec3(shutterW, wSize, 2.5f), shade3d(beamColor, 12), lc);

            if (isLeft) {
                // Quoins on the one genuinely exposed corner of the whole
                // complex (see this lambda's own header comment).
                float qy = 0.f;
                bool big = true;
                while (qy < wingFoundationH - 5.f) {
                    float qh = big ? 12.f : 8.f, qs = big ? 11.f : 8.f;
                    addBox(out, viewProj, windowSize, eye, Vec3(wp.x - qs * 0.5f, qy, wp.z + b.size.y - qs * 0.5f), Vec3(qs, qh - 1.f, qs), quoinStone, lc);
                    qy += qh;
                    big = !big;
                }
            }
        };
        addSideWing(leftWingX0, true);
        addSideWing(rightWingX0, false);

        // ---- Main block (center): the grandest of the 5 front-gabled
        // buildings so far -- 2 full window rows instead of 1. ----
        Vec3 mainPos(mainX0, 0.f, b.position.y);
        float wallH2 = wallH * 1.3f;
        float groundH = wallH2 * 0.32f, midTrim = 4.f, topTrim = 4.f;
        // FIX (2026-08-07): this used to be `wallH2 * 0.32f` too -- an
        // independent fraction, not a remainder -- so groundH+midTrim+
        // upperH+topTrim never actually summed to wallH2/wallTop below.
        // That left a real, un-textured gap between the top of the actual
        // wall geometry and where the roof/gable triangle started -- the
        // "hollow, I can see myself through it" the user's screenshot
        // caught. Every other hero building already computes one band as
        // the remainder (see Kitchen's `upperH`/Bank's `upperH`/Inn's
        // `upperH`) specifically so the bands always sum to the wall's own
        // full height; this one just didn't follow that until now.
        float upperH = wallH2 - groundH - midTrim - topTrim;
        float wallTop = wallH2;
        float gableRise = roofRise * 1.3f;

        addBandedBox(out, viewProj, windowSize, eye, mainPos, Vec3(mainW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        Vec3 midTrimPos(mainPos.x, groundH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, midTrimPos, Vec3(mainW, midTrim, b.size.y), beamColor, lc);
        Vec3 midPos(mainPos.x, groundH + midTrim, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, midPos, Vec3(mainW, upperH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);
        Vec3 topTrimPos(mainPos.x, groundH + midTrim + upperH, mainPos.z);
        addBandedBox(out, viewProj, windowSize, eye, topTrimPos, Vec3(mainW, topTrim, b.size.y), beamColor, lc);

        addFrontGableRoof(out, viewProj, windowSize, eye, mainPos, sf::Vector2f(mainW, b.size.y), wallTop, gableRise, roofColor, lc, &shingleTex, kShingleUv);

        float midXlocal = mainW * 0.5f;
        Vec3 gsw(mainPos.x, wallTop, mainPos.z + b.size.y), gse(mainPos.x + mainW, wallTop, mainPos.z + b.size.y);
        Vec3 gnw(mainPos.x, wallTop, mainPos.z), gne(mainPos.x + mainW, wallTop, mainPos.z);
        Vec3 ridgeS(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z + b.size.y);
        Vec3 ridgeN(mainPos.x + midXlocal, wallTop + gableRise, mainPos.z);
        addTri(out, viewProj, windowSize, eye, gsw, gse, ridgeS, Vec3(0.f, 0.f, 1.f), plaster, lc, &plasterTex, kPlasterUv);
        addTri(out, viewProj, windowSize, eye, gne, gnw, ridgeN, Vec3(0.f, 0.f, -1.f), plaster, lc, &plasterTex, kPlasterUv);
        {
            float run = mainW * 0.5f;
            Vec3 westN = Vec3(-gableRise, run, 0.f).normalized(), eastN = Vec3(gableRise, run, 0.f).normalized();
            addShingleRows(out, viewProj, windowSize, eye, gsw, gnw, ridgeS, ridgeN, westN, roofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, gse, gne, ridgeS, ridgeN, eastN, roofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(ridgeN.x - 2.5f, ridgeN.y - 1.5f, ridgeN.z), Vec3(5.f, 5.f, b.size.y), shade3d(roofColor, -22), lc);
        }

        float southZ = mainPos.z + b.size.y + 1.5f;
        auto beamAt = [&](float x1, float y1, float x2, float y2, float thick) {
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(mainPos.x + x1, y1), sf::Vector2f(mainPos.x + x2, y2), thick, beamColor, lc);
        };
        // Corner posts full height + a rail at each floor line + the
        // king-post gable truss -- 2 full timber-framed floors reads as
        // noticeably grander than the other buildings' single band.
        beamAt(6.f, 0.f, 6.f, wallTop - 2.f, 7.f);
        beamAt(mainW - 6.f, 0.f, mainW - 6.f, wallTop - 2.f, 7.f);
        beamAt(6.f, groundH, mainW - 6.f, groundH, 6.f);
        beamAt(6.f, groundH + midTrim + upperH, mainW - 6.f, groundH + midTrim + upperH, 6.f);
        beamAt(midXlocal, wallTop, midXlocal, wallTop + gableRise - 6.f, 7.f); // king post
        beamAt(4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(mainW - 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);

        // 2 window rows -- ground floor flanking the door, upper floor a
        // full row of 3.
        float winSize = mainW * 0.13f;
        float groundWinY = groundH * 0.28f;
        float shutterW = winSize * 0.4f;
        for (float lx : { mainW * 0.14f, mainW * 0.74f }) {
            Vec3 winPos(mainPos.x + lx, groundWinY, southZ - 1.5f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            beamAt(lx, groundWinY + winSize * 0.5f, lx + winSize, groundWinY + winSize * 0.5f, 2.f);
            beamAt(lx + winSize * 0.5f, groundWinY, lx + winSize * 0.5f, groundWinY + winSize, 2.f);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, groundWinY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, groundWinY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }
        float upperWinY = groundH + midTrim + upperH * 0.3f;
        for (float lx : { mainW * 0.12f, mainW * 0.44f, mainW * 0.76f }) {
            Vec3 winPos(mainPos.x + lx, upperWinY, southZ - 1.5f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            beamAt(lx, upperWinY + winSize * 0.5f, lx + winSize, upperWinY + winSize * 0.5f, 2.f);
            beamAt(lx + winSize * 0.5f, upperWinY, lx + winSize * 0.5f, upperWinY + winSize, 2.f);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, upperWinY + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, upperWinY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, upperWinY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }

        // ---- Arched entrance, lanterns, topiaries + barrels flanking it. ----
        float archW = mainW * 0.22f, archH = groundH * 0.9f;
        Vec3 archCenter(mainPos.x + mainW * 0.5f, 0.f, southZ);
        addBillboard(out, viewProj, windowSize, billboardRight, archCenter, archW, archH, archTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenter.x - archW * 0.9f, archH * 0.55f, southZ), 18.f, glowTex, sf::Color(255, 200, 120, 150));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenter.x + archW * 0.9f, archH * 0.55f, southZ), 18.f, glowTex, sf::Color(255, 200, 120, 150));
        for (float sgn : { -1.f, 1.f }) {
            float px = archCenter.x + sgn * archW * 1.6f;
            addBox(out, viewProj, windowSize, eye, Vec3(px - 4.f, 0.f, southZ - 2.f), Vec3(8.f, 6.f, 8.f), plantPotColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(px - 5.f, 6.f, southZ - 3.f), Vec3(10.f, 12.f, 10.f), plantColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(px + sgn * 14.f - 3.f, 0.f, southZ + 4.f), Vec3(6.f, 10.f, 6.f), sf::Color(120, 78, 48), lc); // a barrel
        }

        // ---- Stone steps up to the entrance (2026-08-07 detail pass) --
        // the grandest of the 5 hero buildings was the one still missing
        // the "a real civic building has steps up to its own door" read
        // every reference implied. 2 shallow stacked steps, same technique
        // Clinic's own porch steps already use. ----
        addBox(out, viewProj, windowSize, eye, Vec3(archCenter.x - archW * 0.7f, 0.f, southZ + 5.f), Vec3(archW * 1.4f, 3.f, 6.f), stepColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(archCenter.x - archW * 0.6f, 3.f, southZ + 9.f), Vec3(archW * 1.2f, 3.f, 5.f), shade3d(stepColor, -14), lc);

        // ---- Flags along the roofline.
        //
        // FIX (2026-08-07, "红色旗子陷入下去了" -- same bug class as
        // Kitchen's own chimneys, same root cause: a flat constant Y
        // (wallTop + gableRise*0.5) doesn't match `addFrontGableRoof`'s
        // real slope height, which varies with X. The middle flag (i=1,
        // fx at exactly midXlocal) sits right AT the ridge, where the true
        // roof height is wallTop+gableRise (the FULL rise) -- but the old
        // constant only ever gave it half that, so its pole base sat a
        // real ~22 units below the actual ridge surface there, sunk deep
        // into the roof. The outer 2 flags were off by less (they're
        // farther from the ridge, where the constant happens to be closer
        // to correct) which is why only the middle one -- flagColors[1],
        // red -- read as visibly wrong. Computed per-flag from the same
        // rise/run ramp now, same technique the chimney fix used (a flag
        // pole is thin enough that, unlike the chimney, one point
        // evaluation per pole is precise enough -- no need for the
        // sloped-face construction). ----
        // (Same round, "蓝色旗子...出问题了" -- the ridge flag's base is now
        // exactly correct, but every pole still used the same fixed 30-unit
        // height regardless of where it's actually planted. The 2 outer
        // flags sit much lower on the slope (their own correct base is
        // ~0.4*gableRise above wallTop, vs the ridge flag's full gableRise)
        // -- with an unscaled 30-unit pole on top, their TIPS actually end
        // up rising slightly *above* the ridge line itself while planted
        // well down the slope, which reads as detached/floating from most
        // angles even though the base itself is geometrically correct.
        // Scaled the pole height down the farther a flag sits from the
        // ridge, so poles read as following the roofline -- tallest right
        // at the peak, shorter out toward the eaves -- instead of 3
        // identical masts poking up by the same amount regardless of how
        // low their own footing is.)
        for (int i = 0; i < 3; ++i) {
            float fxLocal = mainW * (0.2f + 0.3f * static_cast<float>(i));
            float distFromRidge = std::abs(fxLocal - midXlocal);
            float ridgeFrac = distFromRidge / midXlocal; // 0 at the ridge, 1 at the eave
            float roofHere = wallTop + gableRise * (1.f - ridgeFrac);
            float poleH = 30.f - 10.f * ridgeFrac;
            // Z was at 0.4*b.size.y (2026-08-07 fix, "这两个旗子...放在后
            // 方,这样导致其中很难看到" -- 40% of the way from the north/
            // back edge sits toward the far half of the ridge's own depth,
            // which reads as tucked away rather than clearly planted along
            // it). Centered on the ridge's own depth instead.
            Vec3 poleBase(mainPos.x + fxLocal, roofHere, mainPos.z + b.size.y * 0.5f);
            addBox(out, viewProj, windowSize, eye, poleBase, Vec3(2.f, poleH, 2.f), beamColor, lc);
            addBox(out, viewProj, windowSize, eye, poleBase + Vec3(2.f, poleH - 12.f, -1.f), Vec3(11.f, 8.f, 1.5f), flagColors[i % 3], lc);
        }

        // ---- Chimney with smoke ----
        Vec3 chimneyPos(mainPos.x + mainW * 0.30f, wallTop + gableRise * 0.4f, mainPos.z + b.size.y * 0.26f);
        addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, 28.f, 11.f), chimneyColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, 32.f, 5.5f), 16.f, glowTex, smokeColor);

        // ---- Clock tower: attaches flush to the RIGHT wing's east edge
        // (not directly to the main block -- "the tower should be next to
        // the shorter right building") -- a tall stone column past the
        // main roofline, topped with a pyramidal spire (reusing the Mine/
        // Gold Mine mound primitive) and a clock-face decal. ----
        Vec3 towerPos(towerX0, 0.f, b.position.y);
        float towerDepth = b.size.y * 0.7f, towerInset = (b.size.y - towerDepth) * 0.5f;
        towerPos.z += towerInset;
        float towerBodyH = wallTop + gableRise + 34.f; // rises past the main roof peak, trimmed down from +46 now that towerW is wider -- keeps the height:width ratio from ballooning right back up
        addBandedBox(out, viewProj, windowSize, eye, towerPos, Vec3(towerW, towerBodyH, towerDepth), stone, lc, &stoneTex, kStoneUv);
        // A couple of timber corner accents up the tower for material
        // continuity with the main block.
        addFacadeBeam(out, viewProj, windowSize, eye, towerPos.z + towerDepth + 1.5f, sf::Vector2f(towerPos.x + 5.f, 0.f), sf::Vector2f(towerPos.x + 5.f, towerBodyH - 4.f), 6.f, beamColor, lc);
        addFacadeBeam(out, viewProj, windowSize, eye, towerPos.z + towerDepth + 1.5f, sf::Vector2f(towerPos.x + towerW - 5.f, 0.f), sf::Vector2f(towerPos.x + towerW - 5.f, towerBodyH - 4.f), 6.f, beamColor, lc);

        // Clock face + a small protective hood above it (2026-08-07 detail
        // pass, "时钟的模型也调一下") -- real clock towers almost always
        // have a little canopy/lip shielding the face from rain, and it
        // gives the tower's own south face some real geometry breaking up
        // the plain stone band instead of just a flat decal floating on it.
        //
        // FIX (same round, "时钟出问题了" -- addBillboard's own `base` param
        // is the sprite's BOTTOM edge, not its center, same convention as
        // every tree/person/decal in this file (see addBillboard's own
        // header comment) -- but the hood below was placed at
        // `clockBase.y + clockSize*0.5`, i.e. HALFWAY up the decal's own
        // height, not above it, so it cut straight through the middle of
        // the clock face instead of sitting over the top of it. Renamed the
        // anchor from `clockCenter` to `clockBase` to make that convention
        // explicit, and moved the hood to `clockBase.y + clockSize` -- the
        // decal's own true top edge. ----
        float clockSize = towerW * 0.62f;
        Vec3 clockBase(towerPos.x + towerW * 0.5f, towerBodyH * 0.82f, towerPos.z + towerDepth + 1.f);
        addBillboard(out, viewProj, windowSize, billboardRight, clockBase, clockSize, clockSize, clockTex, sf::Color::White);
        addBox(out, viewProj, windowSize, eye, Vec3(clockBase.x - clockSize * 0.5f - 1.f, clockBase.y + clockSize, towerPos.z + towerDepth - 1.f), Vec3(clockSize + 2.f, 3.f, 5.f), shade3d(stone, -18), lc);

        float spireBase = towerBodyH;
        float spireH = towerW * 1.3f;
        addPyramid(out, viewProj, windowSize, eye, Vec3(towerPos.x, spireBase, towerPos.z), sf::Vector2f(towerW, towerDepth), spireH, shade3d(roofColor, -12), lc); // scales with towerW now instead of a flat 40 -- stayed proportioned to the wider tower instead of going flat-topped-looking

        // A small finial ball atop the spire's own apex (2026-08-07 detail
        // pass) -- a plain pointed pyramid tip reads as slightly unfinished
        // on a civic building; a real weathervane-style cap is a cheap,
        // recognizable flourish for very little extra geometry.
        addBox(out, viewProj, windowSize, eye, Vec3(towerPos.x + towerW * 0.5f - 2.5f, spireBase + spireH - 2.f, towerPos.z + towerDepth * 0.5f - 2.5f), Vec3(5.f, 7.f, 5.f), shade3d(roofColor, -22), lc);
    }

    // Market -- sixth hero building (2026-08-07, from the user's own sixth
    // reference image: an open-air market square -- striped canopy stalls in
    // blue/white, green/white and red/white, produce piled on wood counters,
    // barrels, and a freestanding signboard out front). Unlike every hero
    // building so far this ISN'T a walled structure at all -- the reference
    // has no enclosing walls for the market itself, just canopies over open
    // counters -- so this follows the flat-plot-plus-props family
    // (addFarmProps/addPastureProps/etc, ground fill + addPlotBorder + raised
    // props) rather than the wall+roof family, just with much more built-up
    // detail per prop than those simpler archetypes get, matching this
    // building's own turn as a hero. Only `b.id == "market"` uses this.
    // Deliberately NOT attempted (same "world/decoration, not this
    // building's own geometry" call Town Hall's own reference plaza got):
    // the fountain/statues/carts/NPCs the reference's wider scene shows
    // around the market -- those belong to Town Square's own decoration
    // layer, not this one business's footprint.
    void addMarketBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& crateTex) {
        sf::Color groundColor(176, 158, 122); // packed dirt/cobble -- distinct enough from grass that the plot's own edge still reads (see addPlotBorder's header comment on the lumber-yard bug this avoids)
        sf::Color postColor(74, 52, 32);
        sf::Color counterColor(120, 84, 46);
        sf::Color barrelColor(112, 76, 42);
        sf::Color barrelBandColor(70, 46, 26);
        // Blue/white, green/white, red/white -- the reference's own 3 stall
        // awnings, left to right.
        const sf::Color stripeColors[3][2] = {
            { sf::Color(64, 108, 176), sf::Color(232, 228, 214) },
            { sf::Color(78, 132, 70),  sf::Color(232, 228, 214) },
            { sf::Color(176, 62, 54),  sf::Color(232, 228, 214) },
        };

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, groundColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // Depth budget across the plot's own b.size.y, back (north) to front
        // (south/camera-facing): a small back margin, the canopy itself, a
        // gap, the counter, then a front margin left clear for the
        // freestanding sign + implied crowd space -- fixed fractions that
        // sum to well under 1.0 so nothing overhangs the plot's own south
        // edge into the road/plaza beyond it (the Town Hall gap bug earlier
        // in this file was exactly this kind of band-math mistake, just for
        // vertical wall bands instead of a horizontal depth budget).
        constexpr int kStalls = 3;
        float gap = b.size.x * 0.04f;
        float stallW = (b.size.x - gap * static_cast<float>(kStalls - 1)) / static_cast<float>(kStalls);
        float postT = 5.f;
        float backH = 58.f, frontH = 42.f;
        float backMargin = b.size.y * 0.10f;
        float awningDepth = b.size.y * 0.55f;
        float counterGap = b.size.y * 0.02f;
        float counterDepth = b.size.y * 0.20f;

        for (int i = 0; i < kStalls; ++i) {
            float x0 = b.position.x + static_cast<float>(i) * (stallW + gap);
            float zBack = b.position.y + backMargin;
            float zFront = zBack + awningDepth;

            // 4 corner posts -- back posts full backH, front posts the
            // shorter frontH, so each post's own top exactly meets the
            // sloped canopy resting on it instead of poking through it.
            for (float px : { x0, x0 + stallW - postT }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, zBack), Vec3(postT, backH, postT), postColor, lc);
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, zFront - postT), Vec3(postT, frontH, postT), postColor, lc);
            }

            addStripedAwning(out, viewProj, windowSize, eye, Vec3(x0, 0.f, zBack), stallW, awningDepth, backH, frontH,
                stripeColors[i][0], stripeColors[i][1], 6, lc);

            // Counter -- a low wood box at the front of the stall, same "the
            // open span should still read as having a counter" call Inn's
            // v2 made for its own open check-in desk.
            float counterZ0 = zFront + counterGap;
            Vec3 counterPos(x0 + stallW * 0.08f, 0.f, counterZ0);
            addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(stallW * 0.84f, 18.f, counterDepth), counterColor, lc);

            // Produce crate decal on the counter -- a wood crate with a few
            // round fruits/vegetables poking over the rim, the market's own
            // basket-icon motif (see drawBuilding's 2D "market" icon) reused
            // as an actual 3D-scene prop instead of a flat UI icon.
            addBillboard(out, viewProj, windowSize, billboardRight,
                Vec3(counterPos.x + stallW * 0.42f, 18.f, counterZ0 + counterDepth * 0.5f), 28.f, 26.f, crateTex, sf::Color::White);

            // A barrel beside the two OUTER stalls only, tucked just north
            // of their own back posts -- reads as shared storage flanking
            // the market's footprint rather than a fourth identical prop
            // repeated at every stall.
            if (i == 0 || i == kStalls - 1) {
                float bx = (i == 0) ? (x0 + 2.f) : (x0 + stallW - 16.f);
                Vec3 barrelPos(bx, 0.f, zBack - 6.f);
                addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(14.f, 22.f, 14.f), barrelColor, lc);
                addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.8f, 9.f, -0.8f), Vec3(15.6f, 4.f, 15.6f), barrelBandColor, lc);
            }
        }

        // Freestanding market signboard out front, centered in the margin
        // deliberately left clear of the stalls/counters above -- the
        // reference's own standing "集市" sign. The board stays plain (no
        // literal text baked in) -- same call every prior hero building's
        // own hanging sign made, since the floating name label already
        // covers that.
        float signPostH = 34.f, signW = b.size.x * 0.26f, signH = 16.f;
        float signCx = b.position.x + b.size.x * 0.5f;
        float signZ = b.position.y + backMargin + awningDepth + counterGap + counterDepth + 2.f;
        addBox(out, viewProj, windowSize, eye, Vec3(signCx - signW * 0.5f + 3.f, 0.f, signZ), Vec3(3.f, signPostH, 3.f), postColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(signCx + signW * 0.5f - 6.f, 0.f, signZ), Vec3(3.f, signPostH, 3.f), postColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(signCx - signW * 0.5f, signPostH - signH, signZ - 1.f), Vec3(signW, signH, 3.f), counterColor, lc);
    }

    // Clinic ("doctor") -- seventh hero building (2026-08-07, from the
    // user's own seventh reference image: a 2-story wood-and-stone building
    // with a FLAT roof and a low parapet railing (someone standing up there),
    // a green medical cross painted on the upper facade, a shorter single-
    // story wing to the right with a row of windows, a covered porch with an
    // arched stone doorway and lanterns at the seam between the two volumes,
    // and dense flower planter boxes lining the whole front). Only
    // `b.id == "doctor"` uses this -- replaces the old shared Cottage
    // placeholder shape (now retired, see its removal note above), since
    // "sleep"/"eat" had already moved to their own hero buildings and this
    // was Cottage's last remaining caller.
    //
    // This renderer's first FLAT-roofed hero building -- every one before
    // this (Staff/Bank/Inn/Kitchen/Town Hall) used a pitched gable. The flat
    // roof + parapet is a genuinely different silhouette family, built here
    // as a small local lambda (`addFlatRoof`) rather than a new top-level
    // primitive since it's only 2 call sites (main block + wing) and is
    // simple enough (one up-facing deck quad + 4 low border boxes, the same
    // "4 boxes around a footprint" idea as addPlotBorder, just elevated to
    // wall-top height instead of sitting on the ground).
    void addClinicBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& archTex,
        const sf::Texture& crossTex, const sf::Texture& flowerTex) {
        sf::Color stone(122, 118, 112);
        sf::Color plankWall(150, 130, 104); // weathered wood-plank siding, not plaster -- reads distinctly against every other hero building's stone/plaster-dominant walls
        sf::Color beamColor(58, 40, 26);
        sf::Color roofDeck(96, 92, 88);
        sf::Color parapetColor(108, 104, 98);
        sf::Color windowColor(255, 214, 140);
        sf::Color plantPotColor(150, 90, 60);
        sf::Color plantColor(90, 140, 76);
        sf::Color benchColor(110, 78, 46);
        sf::Color stepColor(150, 146, 138);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        // A flat roof deck (one up-facing quad) plus a low parapet border
        // traced around the same footprint -- see this function's own
        // header comment for why this is a local lambda, not a new
        // top-level primitive.
        //
        // FIX (2026-08-07, "顶层图层把绿十字招牌吃掉了" -- the cross decal
        // was getting painted over): the deck used to be one raw addFace
        // spanning the building's full ~80-unit Z depth at an elevated Y --
        // exactly the "one quad averages a near corner and a far corner
        // into a bogus 'nearer than it should be' sort key, so it paints on
        // top of things actually in front of it" bug this file's own
        // history already fixed once for the ground plane (see
        // addGroundQuad's own header comment -- that's precisely what
        // erased the north-row buildings originally). 80 units is past
        // kGroundSliceZ's 60-unit slicing threshold, so it tripped the same
        // bug at roof height instead of ground level. Fixed by routing the
        // deck through addGroundQuad itself (it already takes an arbitrary
        // `y`, not just ground level) instead of a fresh unsliced addFace.
        auto addFlatRoof = [&](Vec3 pos, float w, float d, float deckY, float parapetH) {
            addGroundQuad(out, viewProj, windowSize, eye, pos.x, pos.z, w, d, deckY, roofDeck, lc, /*applyGroundBias=*/false); // elevated roof surface, not ground-level -- see addGroundQuad's own header comment on why this one opts out
            constexpr float t = 6.f;
            addBox(out, viewProj, windowSize, eye, Vec3(pos.x, deckY, pos.z), Vec3(w, parapetH, t), parapetColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(pos.x, deckY, pos.z + d - t), Vec3(w, parapetH, t), parapetColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(pos.x, deckY, pos.z), Vec3(t, parapetH, d), parapetColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(pos.x + w - t, deckY, pos.z), Vec3(t, parapetH, d), parapetColor, lc);
        };

        // ---- Two volumes, flush (no jetty), same idea as Town Hall's
        // side-by-side massing: taller 2-story main block (west/left) plus
        // a shorter single-story wing (east/right). ----
        float mainW = b.size.x * 0.56f, wingW = b.size.x - mainW;
        Vec3 mainPos(b.position.x, 0.f, b.position.y);
        Vec3 wingPos(mainPos.x + mainW, 0.f, b.position.y);
        float seamX = wingPos.x;
        float southZ = b.position.y + b.size.y + 1.5f;

        // ---- Main block ----
        float wallH2 = wallH * 1.15f;
        float foundationH = wallH2 * 0.16f;
        float upperH = wallH2 - foundationH;
        addBandedBox(out, viewProj, windowSize, eye, mainPos, Vec3(mainW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(mainPos.x, foundationH, mainPos.z), Vec3(mainW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addFlatRoof(mainPos, mainW, b.size.y, wallH2, 14.f);
        addBox(out, viewProj, windowSize, eye, Vec3(mainPos.x + mainW * 0.58f, wallH2 + 2.f, mainPos.z + b.size.y * 0.3f), Vec3(14.f, 12.f, 14.f), benchColor, lc); // a crate left up on the roof deck -- the reference shows someone standing up there with something beside them

        // ---- South-west corner quoins (2026-08-07 detail pass) -- same
        // alternating-block technique Bank/Kitchen/Warehouse already use.
        // Sized down from their usual 15/10 to 10/7 -- the ground-floor
        // window below sits close enough to this corner (mainW*0.16) that
        // full-size quoins would crowd right up against it. ----
        {
            float qy = 0.f;
            bool big = true;
            while (qy < foundationH - 6.f) {
                float qh = big ? 11.f : 7.f, qs = big ? 10.f : 7.f;
                addBox(out, viewProj, windowSize, eye, Vec3(mainPos.x - qs * 0.5f, qy, mainPos.z + b.size.y - qs * 0.5f), Vec3(qs, qh - 1.f, qs), shade3d(stone, 14), lc);
                qy += qh;
                big = !big;
            }
        }

        // Ground-floor window (west side, away from the shared porch/door
        // at the seam).
        float winSize = mainW * 0.14f;
        Vec3 winG(mainPos.x + mainW * 0.16f, foundationH * 0.22f, southZ - 1.f);
        addBox(out, viewProj, windowSize, eye, winG, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winG.x, winG.y + winSize * 0.5f), sf::Vector2f(winG.x + winSize, winG.y + winSize * 0.5f), 2.f, beamColor, lc);
        addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winG.x + winSize * 0.5f, winG.y), sf::Vector2f(winG.x + winSize * 0.5f, winG.y + winSize), 2.f, beamColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winG.x + winSize * 0.5f, winG.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));

        // Upper-floor windows, paired on the west half -- the east half is
        // given over to the medical cross sign below.
        float upperWinY = foundationH + upperH * 0.55f;
        for (float lx : { mainW * 0.14f, mainW * 0.34f }) {
            Vec3 winPos(mainPos.x + lx, upperWinY, southZ - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x, winPos.y + winSize * 0.5f), sf::Vector2f(winPos.x + winSize, winPos.y + winSize * 0.5f), 2.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x + winSize * 0.5f, winPos.y), sf::Vector2f(winPos.x + winSize * 0.5f, winPos.y + winSize), 2.f, beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        }

        // Medical cross sign -- a flat decal (no curved/rotated-geometry
        // primitive exists here, same "fake it as a sprite" call every
        // other hero building's arch/clock decal made), mounted on the
        // east half of the upper facade.
        //
        // FIX (2026-08-07, second real bug from the same screenshot round --
        // "顶层图层把绿十字招牌吃掉了"): this used to put the glow at the
        // EXACT SAME position as the decal, sized even LARGER than it
        // (mainW*0.62 glow over a mainW*0.34-wide decal) -- a big bright
        // additive halo dead-center on top of a small crisp graphic reads
        // as a green blur, not "a lit sign with a glow around it". Every
        // OTHER hero building's own graphic decal avoids exactly this: see
        // Kitchen's `archTex` above, whose 2 lantern glows sit OFFSET TO
        // THE SIDES of the arch, never overlapping the arch artwork itself.
        // Moved to the same flanking-lantern treatment here instead of a
        // co-located halo -- the decal stays fully legible, the glow still
        // reads as "this sign is lit" from its own 2 flanking points.
        // FIX (2026-08-07, same round -- user's own guess, and correct: "是
        // 不是把屋顶图层贴在了绿十字那边"): the decal's own top edge
        // (foundationH + upperH*0.52 + upperH*0.5 = wallH2 + ~1.85) sat
        // ABOVE `wallTop`/the roof deck's own height -- a real, if small,
        // sliver poking past the roofline where the flat roof's geometry
        // (and the parapet border sitting right on top of it) legitimately
        // does cover it. Shrunk the decal and re-centered it lower on the
        // wall so its top edge sits with real margin below wallH2 instead
        // of just barely -- and, unlike, past -- it.
        Vec3 crossPos(mainPos.x + mainW * 0.74f, foundationH + upperH * 0.40f, southZ);
        float crossW = mainW * 0.30f, crossH = upperH * 0.38f;
        addBillboard(out, viewProj, windowSize, billboardRight, crossPos, crossW, crossH, crossTex, sf::Color::White);
        for (float sgn : { -1.f, 1.f }) {
            addGlowBillboard(out, viewProj, windowSize, billboardRight,
                Vec3(crossPos.x + sgn * crossW * 0.75f, crossPos.y + crossH * 0.5f, southZ), 16.f, glowTex, sf::Color(90, 240, 120, 190));
        }

        // ---- Wing (east/right, shorter, same flat-roof treatment, a plain
        // row of 3 windows -- the ground floor's whole point here is just
        // "more building", not another feature room like Bank/Inn's alcove). ----
        float wingWallH = wallH * 0.68f;
        float wingFoundationH = wingWallH * 0.18f;
        float wingUpperH = wingWallH - wingFoundationH;
        addBandedBox(out, viewProj, windowSize, eye, wingPos, Vec3(wingW, wingFoundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(wingPos.x, wingFoundationH, wingPos.z), Vec3(wingW, wingUpperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addFlatRoof(wingPos, wingW, b.size.y, wingWallH, 10.f);

        float wingWinSize = wingW * 0.16f;
        float wingWinY = wingFoundationH + wingUpperH * 0.4f;
        for (int i = 0; i < 3; ++i) {
            float lx = wingW * (0.18f + 0.32f * static_cast<float>(i));
            Vec3 winPos(wingPos.x + lx, wingWinY, southZ - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(wingWinSize, wingWinSize, 3.f), windowColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x, winPos.y + wingWinSize * 0.5f), sf::Vector2f(winPos.x + wingWinSize, winPos.y + wingWinSize * 0.5f), 2.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x + wingWinSize * 0.5f, winPos.y), sf::Vector2f(winPos.x + wingWinSize * 0.5f, winPos.y + wingWinSize), 2.f, beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + wingWinSize * 0.5f, winPos.y + wingWinSize * 0.5f, southZ), wingWinSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        }

        // ---- Shared covered porch straddling the seam between the two
        // volumes -- the arched stone doorway, lanterns, a bench, a potted
        // plant and a couple of steps, matching the reference's own single
        // shared entrance rather than a door on each volume. Reuses
        // addStripedAwning with colorA==colorB for its single-slope canopy
        // math (no new primitive needed for a plain-colored lean-to roof). ----
        float porchW = 40.f, porchDepth = 24.f;
        float porchX0 = seamX - porchW * 0.5f;
        float porchBackH = wallH * 0.60f, porchFrontH = wallH * 0.46f;
        addStripedAwning(out, viewProj, windowSize, eye, Vec3(porchX0, 0.f, southZ - 1.f), porchW, porchDepth, porchBackH, porchFrontH, roofDeck, roofDeck, 1, lc);
        for (float px : { porchX0 + 4.f, porchX0 + porchW - 9.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, southZ - 1.f + porchDepth - 7.f), Vec3(5.f, porchFrontH, 5.f), beamColor, lc);
        }

        float doorW = porchW * 0.42f, doorH = foundationH * 0.92f;
        Vec3 doorPos(seamX - doorW * 0.5f, 0.f, southZ - 1.f + porchDepth - 2.f);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(doorPos.x + doorW * 0.5f, doorH * 0.5f, doorPos.z), doorW * 1.3f, doorH * 1.5f, archTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(doorPos.x - 10.f, doorH * 0.6f, doorPos.z), 16.f, glowTex, sf::Color(255, 200, 120, 160));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(doorPos.x + doorW + 10.f, doorH * 0.6f, doorPos.z), 16.f, glowTex, sf::Color(255, 200, 120, 160));

        addBox(out, viewProj, windowSize, eye, Vec3(porchX0 - 20.f, 0.f, southZ + porchDepth * 0.3f), Vec3(16.f, 8.f, 8.f), benchColor, lc); // bench, west side of the porch
        addBox(out, viewProj, windowSize, eye, Vec3(porchX0 + porchW + 6.f, 0.f, southZ + porchDepth * 0.3f), Vec3(8.f, 6.f, 8.f), plantPotColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(porchX0 + porchW + 5.f, 6.f, southZ + porchDepth * 0.3f - 1.f), Vec3(10.f, 12.f, 10.f), plantColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(seamX - 16.f, 0.f, southZ + porchDepth + 4.f), Vec3(32.f, 3.f, 8.f), stepColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(seamX - 14.f, 3.f, southZ + porchDepth + 8.f), Vec3(28.f, 3.f, 6.f), shade3d(stepColor, -14), lc);

        // ---- Flower planter boxes lining the whole front, skipping the
        // porch's own footprint -- the reference's dense garden bed read. ----
        constexpr int kBoxes = 6;
        for (int i = 0; i < kBoxes; ++i) {
            float fx = b.position.x + b.size.x * (0.06f + 0.88f * static_cast<float>(i) / static_cast<float>(kBoxes - 1));
            if (fx > porchX0 - 16.f && fx < porchX0 + porchW + 16.f) continue;
            Vec3 boxPos(fx, 0.f, southZ + 2.f);
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(14.f, 10.f, 10.f), sf::Color(96, 68, 40), lc);
            addBillboard(out, viewProj, windowSize, billboardRight, Vec3(fx + 7.f, 10.f, southZ + 7.f), 18.f, 18.f, flowerTex, sf::Color::White);
        }
    }

    // Warehouse -- eighth hero building (2026-08-07, from the user's own
    // eighth reference image: "丰收仓库" -- a tall stone-and-timber storage
    // hall, full-height timber corner posts, 4 upper-floor windows in a
    // row, a big flush signboard below them, 2 ground-floor cart-sized
    // double-doors (one shut, one standing open with a dark interior),
    // stone steps up to the open one, and crates/barrels/sacks and a
    // leaning log pile scattered around the entrance). Only
    // `b.id == "warehouse"` uses this. Unlike Staff/Inn's 2-volume massing
    // this is a single tall block (matches the reference's own roughly
    // cubic silhouette) -- closer in spirit to Bank's single-volume
    // grandeur, just with 2 big doors instead of one open counter.
    void addWarehouseBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& plasterTex, const sf::Texture& shingleTex) {
        sf::Color stone(120, 116, 110);
        sf::Color plaster(184, 156, 118);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(84, 52, 36);
        sf::Color windowColor(255, 214, 140);
        sf::Color plankColor(112, 78, 44);
        sf::Color strapColor(40, 34, 28);
        sf::Color darkOpening(28, 24, 22);
        sf::Color signColor(150, 110, 70);
        sf::Color crateColor(120, 86, 50);
        sf::Color sackColor(176, 152, 108);
        sf::Color barrelColor(112, 76, 42);
        sf::Color barrelBandColor(70, 46, 26);
        sf::Color stepColor(150, 146, 138);

        constexpr float kStoneUv = 20.f, kPlasterUv = 11.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = b.position.y + b.size.y + 1.5f;

        // ---- A single block, close to Bank's own wall ratio (1.05x --
        // FIX 2026-08-07, "太高了" -- 1.35x plus a 1.2x roof rise on top of
        // that read as too tall; a 2-floor building doesn't need to be as
        // tall as Town Hall's own grandest-of-5 main block to still look
        // like a real 2-floor warehouse). ----
        float wallH2 = wallH * 1.05f;
        float foundationH = wallH2 * 0.10f;
        float groundH = wallH2 * 0.55f; // ground floor is mostly door, not much wall showing above the foundation
        float trimH = 5.f;
        float upperH = wallH2 - groundH - trimH; // remainder, same "one band derived as the leftover" rule the Town Hall gap-bug taught this file
        float wallTop = wallH2;

        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(b.size.x, groundH - foundationH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);
        Vec3 trimPos(basePos.x, groundH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(b.size.x, trimH, b.size.y), beamColor, lc);
        Vec3 upperPos(basePos.x, groundH + trimH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperPos, Vec3(b.size.x, upperH, b.size.y), plaster, lc, &plasterTex, kPlasterUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, wallTop, b.size.y), wallTop, roofRise, roofColor, lc, &shingleTex, kShingleUv);

        // A small vent/chimney box astride the ridge -- the reference's own
        // roof-peak vent, reusing the plain-box-on-the-ridge trick Town
        // Hall's own ridge cap already used.
        float midX = basePos.x + b.size.x * 0.5f, midZ = basePos.z + b.size.y * 0.5f;
        addBox(out, viewProj, windowSize, eye, Vec3(midX - 6.f, wallTop + roofRise - 4.f, midZ - 6.f), Vec3(12.f, 14.f, 12.f), shade3d(roofColor, -14), lc);

        // ---- Full-height timber corner posts -- the "grandest" treatment
        // (see Town Hall's own main block), fitting for the tallest single-
        // volume hero building so far. ----
        for (float lx : { 6.f, b.size.x - 6.f }) {
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(basePos.x + lx, 0.f), sf::Vector2f(basePos.x + lx, wallTop - 2.f), 7.f, beamColor, lc);
        }

        // ---- 4 upper-floor windows in a row, a big flush signboard
        // (plain, no literal text baked in -- same call every prior hero
        // building's own sign made) sitting just below them.
        //
        // FIX (2026-08-07, "那个仓库很多发光的地方,不懂什么意义" -- every
        // window here used to get its own glow halo, same as every other
        // hero building's own windows do. That convention makes narrative
        // sense for a residence/inn/tavern (cozy lit windows at night), but
        // a working STORAGE warehouse having all 4 of its windows lit up
        // like a home doesn't read as sensible, and 4 separate glow halos
        // in a row is genuinely a lot of glow for one facade -- the user's
        // confusion was a legitimate design question, not a misreading.
        // Cut to a single, subtle glow on the center-right window only
        // (reads as "someone's still in there working/a night-watch lamp",
        // not "every room lit") -- the other 3 keep their plain lit-color
        // pane with no halo. ----
        float winSize = b.size.x * 0.11f, winY = groundH + trimH + upperH * 0.62f;
        const float winXs[] = { b.size.x * 0.10f, b.size.x * 0.30f, b.size.x * 0.58f, b.size.x * 0.78f };
        int wi = 0;
        for (float lx : winXs) {
            Vec3 winPos(basePos.x + lx, winY, southZ - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x, winY + winSize * 0.5f), sf::Vector2f(winPos.x + winSize, winY + winSize * 0.5f), 2.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x + winSize * 0.5f, winY), sf::Vector2f(winPos.x + winSize * 0.5f, winY + winSize), 2.f, beamColor, lc);
            if (wi == 2) {
                addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, southZ), winSize * 1.2f, glowTex, sf::Color(255, 214, 140, 90));
            }
            ++wi;
        }

        float signW = b.size.x * 0.55f, signH = upperH * 0.35f;
        Vec3 signPos(basePos.x + b.size.x * 0.5f - signW * 0.5f, groundH + trimH + upperH * 0.12f, southZ - 1.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // ---- 2 ground-floor cart-sized doors: left CLOSED (plank door +
        // iron straps), right OPEN (a dark recessed opening) -- the
        // reference's own "one shut, one standing open" pair. ----
        float doorGap = b.size.x * 0.05f;
        float doorBayW = (b.size.x - doorGap) * 0.5f;
        float doorH = groundH - foundationH - 4.f;
        float leftX0 = basePos.x + b.size.x * 0.02f, rightX0 = leftX0 + doorBayW + doorGap;

        // Left door -- 2 real panels (a center seam beam down the middle,
        // not one flat slab) plus the iron straps, closer to an actual
        // cart-sized double door.
        addBandedBox(out, viewProj, windowSize, eye, Vec3(leftX0, foundationH, southZ - 4.f), Vec3(doorBayW - 4.f, doorH, 4.f), plankColor, lc, &shingleTex, kShingleUv);
        addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(leftX0 + (doorBayW - 4.f) * 0.5f, foundationH), sf::Vector2f(leftX0 + (doorBayW - 4.f) * 0.5f, foundationH + doorH), 3.f, strapColor, lc);
        for (float t : { 0.32f, 0.68f }) {
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(leftX0, foundationH + doorH * t), sf::Vector2f(leftX0 + doorBayW - 4.f, foundationH + doorH * t), 4.f, strapColor, lc);
        }
        for (float t : { 0.35f, 0.65f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(leftX0 + doorBayW * t, foundationH + doorH * 0.5f - 3.f, southZ - 1.f), Vec3(4.f, 6.f, 3.f), strapColor, lc); // hinge/ring hardware
        }

        addBandedBox(out, viewProj, windowSize, eye, Vec3(rightX0, foundationH, basePos.z + b.size.y - 8.f), Vec3(doorBayW - 4.f, doorH * 0.94f, 8.f), darkOpening, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(rightX0 - 12.f, 0.f, southZ - 1.f + 6.f), Vec3(3.f, 2.f, 6.f), stepColor, lc); // a single low threshold step, right at the open doorway

        // ---- One clear, deliberately-composed goods pile beside the
        // (closed) left door -- crate stack + a sack leaning against it +
        // a barrel, all touching, instead of the previous scatter (2
        // crates west, a 3rd crate off near the log pile at the far east
        // edge, a "floating" sack, a barrel off past the right door --
        // 4 separate spots with no clear read as one thing, 2026-08-07
        // detail pass, "左边很大不懂什么东西...拿这个仓库先做优化看看"). ----
        Vec3 crateBase(leftX0 - 40.f, 0.f, southZ + 4.f); // far enough west to clear the south-west corner quoins below, with margin
        addBandedBox(out, viewProj, windowSize, eye, crateBase, Vec3(14.f, 14.f, 12.f), crateColor, lc);
        addBandedBox(out, viewProj, windowSize, eye, crateBase + Vec3(1.f, 14.f, 1.f), Vec3(12.f, 12.f, 10.f), crateColor, lc);
        addBox(out, viewProj, windowSize, eye, crateBase + Vec3(15.f, 0.f, 1.f), Vec3(11.f, 15.f, 10.f), sackColor, lc); // sack leaning against the stack's east side
        Vec3 barrelPos = crateBase + Vec3(-2.f, 0.f, -10.f);
        addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(14.f, 22.f, 14.f), barrelColor, lc);
        addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.8f, 9.f, -0.8f), Vec3(15.6f, 4.f, 15.6f), barrelBandColor, lc);

        // ---- Corner quoins on the south-west corner -- same alternating-
        // block technique Bank/Kitchen already use, kept clear of the
        // goods pile (which sits further west, off the building's own
        // footprint) and the log pile (east side). ----
        {
            float qy = 0.f;
            bool big = true;
            while (qy < wallTop - 6.f) {
                float qh = big ? 16.f : 10.f, qs = big ? 15.f : 10.f;
                addBox(out, viewProj, windowSize, eye, Vec3(basePos.x - qs * 0.5f, qy, basePos.z + b.size.y - qs * 0.5f), Vec3(qs, qh - 1.f, qs), shade3d(stone, 12), lc);
                qy += qh;
                big = !big;
            }
        }

        // ---- A small leaning log pile against the east wall, logs lying
        // front-to-back (along Z) so their SOUTH face -- the one the camera
        // actually sees -- is already each log's own cut end, not its long
        // side (unlike Lumber's own pile, where logs lie along X and the
        // lighter end-cap is a separate box offset sideways). A smaller,
        // lighter square nudged 1 unit further south than the log's own
        // face reads as that cut end's lighter, ringed cross-section. ----
        float logX = basePos.x + b.size.x - 4.f, logZ = basePos.z + b.size.y * 0.3f;
        const struct { float dy, dz, len; } logRows[] = { {0.f, 0.f, 34.f}, {0.f, 15.f, 30.f}, {13.f, 7.f, 24.f} };
        for (const auto& row : logRows) {
            Vec3 logPos(logX, row.dy, logZ + row.dz);
            addBox(out, viewProj, windowSize, eye, logPos, Vec3(14.f, 13.f, row.len), plankColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(logPos.x + 2.f, logPos.y + 2.f, logPos.z + row.len - 0.5f), Vec3(10.f, 9.f, 1.5f), sf::Color(160, 124, 82), lc);
        }
    }

    // Plot state 1/3 -- "未开发的土地" (2026-08-07, from the user's own
    // reference image: an unmarked natural clearing -- a couple of loose
    // rocks, a stray sapling and bush, wildflowers, grass tufts, nothing
    // built or even fenced off). A business that `requiresConstruction()`
    // and hasn't started yet (`!ci.inProgress`, see draw3DZone's building
    // loop) used to render as a bare flat-colored lot, same treatment as an
    // active construction site just a different color -- but unlike a site
    // that's actually mid-build (which HAS claimed the land, just isn't
    // finished with it), unstarted land hasn't been claimed by anything
    // yet, so it should read as ordinary landscape, not a marked-off plot.
    // Deliberately draws NO ground quad and NO border at all -- the zone's
    // own base grass plane (drawn earlier in draw3DZone) already shows
    // through underneath these props, which is the whole point.
    void addUndevelopedPlot(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight,
        const sf::Texture& treeTex, const sf::Texture& bushTex, const sf::Texture& flowerTex, const sf::Texture& herbTuftTex, sf::Color dayNightTint) {
        sf::Color rockColor(148, 148, 152);

        const sf::Vector2f rocks[] = { { 0.22f, 0.35f }, { 0.68f, 0.60f } };
        int ri = 0;
        for (const auto& r : rocks) {
            float hh = 12.f + static_cast<float>(ri % 2) * 6.f; ++ri;
            Vec3 p(b.position.x + b.size.x * r.x, 0.f, b.position.y + b.size.y * r.y);
            addBox(out, viewProj, windowSize, eye, p, Vec3(16.f, hh, 14.f), rockColor, lc);
        }

        // A young/sparse-looking sapling -- reuses the zone's own tree
        // billboard but a bit smaller than a full-grown decoration tree, so
        // it doesn't read as deliberately planted the way the zone's real
        // trees do.
        addBillboard(out, viewProj, windowSize, billboardRight,
            Vec3(b.position.x + b.size.x * 0.76f, 0.f, b.position.y + b.size.y * 0.30f), 38.f, 48.f, treeTex, dayNightTint);
        addBillboard(out, viewProj, windowSize, billboardRight,
            Vec3(b.position.x + b.size.x * 0.45f, 0.f, b.position.y + b.size.y * 0.72f), 34.f, 30.f, bushTex, dayNightTint);

        const sf::Vector2f flowers[] = { { 0.15f, 0.55f }, { 0.55f, 0.22f } };
        for (const auto& f : flowers) {
            addBillboard(out, viewProj, windowSize, billboardRight,
                Vec3(b.position.x + b.size.x * f.x, 0.f, b.position.y + b.size.y * f.y), 18.f, 18.f, flowerTex, sf::Color::White);
        }
        const sf::Vector2f tufts[] = { { 0.32f, 0.18f }, { 0.86f, 0.78f } };
        for (const auto& t : tufts) {
            addBillboard(out, viewProj, windowSize, billboardRight,
                Vec3(b.position.x + b.size.x * t.x, 0.f, b.position.y + b.size.y * t.y), 16.f, 16.f, herbTuftTex, dayNightTint);
        }
    }

    // Plot state 2/3 -- construction site (2026-08-07, from the user's own
    // tenth reference image: a half-built timber-and-stone house -- an
    // exposed roof-truss skeleton with no cladding yet, one wall section
    // actually up with windows already installed, a ladder, piles of stone
    // blocks/cement sacks/lumber, a workbench, a wheelbarrow, a low picket
    // fence marking the site's own boundary, and a real progress bar).
    // Unlike state 1 (`addUndevelopedPlot`, no marking at all -- see its
    // own header comment), a site that's actually mid-build HAS claimed
    // the land, so this keeps the old bordered-plot treatment (ground fill
    // + `addPlotBorder`) and adds a fence on top of it.
    //
    // This is the GENERIC state-2 fallback -- one shared shape for every
    // business in this state, not a per-`b.id` hero shape (a construction
    // version of every hero building this file already has would be way
    // outside this pass's scope) -- so every dimension here is a fraction
    // of `b.size`/`wallH`/`roofRise`, not a hand-tuned constant, so it
    // holds up across any business's own footprint, including Town Hall's
    // wider-than-default rect.
    void addConstructionSiteProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, const ConstructionInfo& ci, float wallH, float roofRise,
        const sf::Texture& stoneTex) {
        sf::Color dirtColor(120, 96, 62); // same excavated-earth color drawConstructionSiteShape's 2D version already used
        sf::Color borderColor(70, 55, 30);
        sf::Color fenceColor(120, 90, 55);
        sf::Color wallColor(150, 140, 128);
        sf::Color beamColor(90, 62, 34);
        sf::Color windowColor(255, 214, 140);
        sf::Color stoneBlockColor(176, 172, 164);
        sf::Color sackColor(196, 168, 118);
        sf::Color lumberColor(120, 80, 50);
        sf::Color benchColor(110, 78, 46);
        sf::Color cartColor(96, 66, 40);
        sf::Color wheelColor(60, 44, 28);

        constexpr float kStoneUv = 20.f;

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, dirtColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, borderColor, lc);

        // ---- Low picket fence around the whole site -- the boundary
        // marker state 1 deliberately goes without. ----
        {
            constexpr float postH = 16.f, postGap = 18.f;
            auto fenceRun = [&](float x0, float z0, float len, bool alongX) {
                int n = std::max(1, static_cast<int>(len / postGap));
                for (int i = 0; i <= n; ++i) {
                    float t = len * static_cast<float>(i) / static_cast<float>(n);
                    float x = alongX ? x0 + t : x0, z = alongX ? z0 : z0 + t;
                    addBox(out, viewProj, windowSize, eye, Vec3(x, 0.f, z), Vec3(3.f, postH, 3.f), fenceColor, lc);
                }
                Vec3 railSize = alongX ? Vec3(len, 3.f, 3.f) : Vec3(3.f, 3.f, len);
                addBox(out, viewProj, windowSize, eye, Vec3(x0, postH * 0.55f, z0), railSize, fenceColor, lc);
            };
            fenceRun(b.position.x, b.position.y, b.size.x, true);
            fenceRun(b.position.x, b.position.y + b.size.y, b.size.x, true);
            fenceRun(b.position.x, b.position.y, b.size.y, false);
            fenceRun(b.position.x + b.size.x, b.position.y, b.size.y, false);
        }

        // ---- One wall section actually up (back-left corner), with 2
        // windows already installed -- the reference's own "the finished-
        // looking part has glass in it, the rest is still bare" read. Only
        // spans part of the footprint on purpose -- the rest stays open, a
        // real dirt lot under an exposed roof, not a full building. ----
        float wallHPartial = wallH * 0.85f;
        float wallSegW = b.size.x * 0.55f;
        Vec3 wallPos(b.position.x, 0.f, b.position.y);
        addBandedBox(out, viewProj, windowSize, eye, wallPos, Vec3(wallSegW, wallHPartial, 12.f), wallColor, lc, &stoneTex, kStoneUv);
        float wallSouthZ = wallPos.z + 12.f + 1.f;
        float winSize = wallSegW * 0.15f, winY = wallHPartial * 0.4f;
        for (float lx : { wallSegW * 0.2f, wallSegW * 0.65f }) {
            Vec3 winPos(wallPos.x + lx, winY, wallSouthZ - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        }

        // ---- Exposed roof-truss skeleton, spanning the FULL footprint at
        // normal wall/roof height (independent of the partial wall's own
        // shorter height -- a truss frame is temporarily propped up on its
        // own posts before the walls below it are finished, matching the
        // reference). 3 frames, each a flat coplanar "A" shape built from
        // addFacadeBeam at that frame's own fixed Z -- the same king-post-
        // truss trick Staff Office/Town Hall's finished gables already use,
        // just with no gable-end wall filled in behind it. ----
        float trussBaseY = wallH;
        float ridgeY = trussBaseY + roofRise;
        float midX = b.position.x + b.size.x * 0.5f;
        float xL = b.position.x + 6.f, xR = b.position.x + b.size.x - 6.f;
        for (float t : { 0.12f, 0.48f, 0.85f }) {
            float z = b.position.y + b.size.y * t;
            addFacadeBeam(out, viewProj, windowSize, eye, z, sf::Vector2f(xL, trussBaseY), sf::Vector2f(midX, ridgeY), 5.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, z, sf::Vector2f(xR, trussBaseY), sf::Vector2f(midX, ridgeY), 5.f, beamColor, lc);
            addFacadeBeam(out, viewProj, windowSize, eye, z, sf::Vector2f(xL, trussBaseY), sf::Vector2f(xR, trussBaseY), 5.f, beamColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(xL - 2.5f, 0.f, z - 2.5f), Vec3(5.f, trussBaseY, 5.f), beamColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(xR - 2.5f, 0.f, z - 2.5f), Vec3(5.f, trussBaseY, 5.f), beamColor, lc);
        }

        // ---- A ladder leaning against the built wall's own east edge. ----
        float ladderX = wallPos.x + wallSegW + 6.f, ladderZ = wallPos.z + 6.f;
        float ladderH = wallHPartial * 0.9f;
        for (float lx : { 0.f, 10.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(ladderX + lx, 0.f, ladderZ), Vec3(2.5f, ladderH, 2.5f), beamColor, lc);
        }
        for (int i = 0; i < 5; ++i) {
            float ry = ladderH * (0.15f + 0.17f * static_cast<float>(i));
            addBox(out, viewProj, windowSize, eye, Vec3(ladderX, ry, ladderZ + 0.5f), Vec3(10.f, 1.5f, 1.5f), beamColor, lc);
        }

        // ---- Material piles: stone blocks, cement sacks, a lumber stack,
        // a workbench, a wheelbarrow -- scattered across the open half of
        // the footprint, matching the reference's own site-clutter read. ----
        Vec3 stackPos(b.position.x + b.size.x * 0.62f, 0.f, b.position.y + b.size.y * 0.22f);
        for (int i = 0; i < 3; ++i) {
            addBox(out, viewProj, windowSize, eye, stackPos + Vec3(0.f, static_cast<float>(i) * 8.f, 0.f), Vec3(16.f, 7.f, 14.f), stoneBlockColor, lc);
        }
        Vec3 sackPos(b.position.x + b.size.x * 0.82f, 0.f, b.position.y + b.size.y * 0.32f);
        for (int i = 0; i < 2; ++i) {
            addBox(out, viewProj, windowSize, eye, sackPos + Vec3(static_cast<float>(i) * 2.f, static_cast<float>(i) * 7.f, static_cast<float>(i)), Vec3(13.f, 8.f, 10.f), sackColor, lc);
        }
        Vec3 lumberPos(b.position.x + b.size.x * 0.15f, 0.f, b.position.y + b.size.y * 0.72f);
        for (int i = 0; i < 3; ++i) {
            addBox(out, viewProj, windowSize, eye, lumberPos + Vec3(0.f, static_cast<float>(i) * 9.f, static_cast<float>(i)), Vec3(30.f, 8.f, 10.f), lumberColor, lc);
        }
        float benchLegH = 16.f;
        Vec3 benchPos(b.position.x + b.size.x * 0.45f, 0.f, b.position.y + b.size.y * 0.80f);
        for (float lx : { 2.f, 18.f }) {
            for (float lz : { 2.f, 9.f }) {
                addBox(out, viewProj, windowSize, eye, benchPos + Vec3(lx, 0.f, lz), Vec3(2.f, benchLegH, 2.f), benchColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, benchPos + Vec3(0.f, benchLegH, 0.f), Vec3(22.f, 3.f, 12.f), benchColor, lc);

        Vec3 cartPos(b.position.x + b.size.x * 0.30f, 0.f, b.position.y + b.size.y * 0.85f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc); // a single small wheel block -- no curved primitive in this renderer, same call every barrel/cart shape here already makes

        // ---- Real progress bar, flat on the ground near the site's own
        // front edge -- the 3D equivalent of drawConstructionSiteShape's 2D
        // bar (this file's own text label loop adds the matching "N days
        // left" text above this same site, see draw3DZone). Small enough
        // in Z that it doesn't need addGroundQuad's slicing. ----
        double totalDays = std::max(1, ci.totalDays);
        float progress = static_cast<float>(std::clamp(1.0 - (ci.daysRemaining / totalDays), 0.0, 1.0));
        float barW = b.size.x * 0.5f, barD = 6.f;
        Vec3 barPos(b.position.x + b.size.x * 0.5f - barW * 0.5f, 1.f, b.position.y + b.size.y - 14.f);
        addFace(out, viewProj, windowSize, eye, barPos, barPos + Vec3(barW, 0.f, 0.f), barPos + Vec3(barW, 0.f, barD), barPos + Vec3(0.f, 0.f, barD), Vec3(0.f, 1.f, 0.f), sf::Color(30, 30, 34), lc);
        if (progress > 0.01f) {
            // Nudged slightly higher than the background bar (1.f -> 1.4f)
            // so the two coplanar quads don't sit at the exact same Y where
            // they overlap -- same "offset to dodge z-fighting" reasoning
            // as addSlopeBand's own `offset` param.
            Vec3 fillPos = barPos + Vec3(0.f, 0.4f, 0.f);
            addFace(out, viewProj, windowSize, eye, fillPos, fillPos + Vec3(barW * progress, 0.f, 0.f), fillPos + Vec3(barW * progress, 0.f, barD), fillPos + Vec3(0.f, 0.f, barD), Vec3(0.f, 1.f, 0.f), sf::Color(232, 212, 120), lc);
        }
    }

    // Storefront / "Village General Store, Trading Post" -- ninth and
    // final ServiceHall hero building (2026-08-07, from the user's own
    // eleventh reference image), dispatched only for `b.id == "storefront"`.
    // Reuses the exact front-gable/jetty/king-post-truss recipe
    // addRecruitmentCenterBuilding (Staff Office) already established --
    // architecturally this reference reads like a sibling of that one, not
    // a new massing idea -- crossed with Bank/Inn's open-counter-alcove for
    // the ground floor instead of a closed door, since a general store's
    // whole point is the goods on display, not a door. Genuinely new
    // pieces added on top of that shared toolkit: window planter boxes
    // (flower box + `flowerTex` billboard under each pane) and a row of
    // hanging lantern glows along the jetty eave, matching the reference's
    // own well-lit shopfront.
    void addStorefrontBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& goodsTex) {
        sf::Color stone(120, 116, 110);
        sf::Color plankWall(140, 108, 66); // warm wood-plank siding, distinct from every other hero building's stone/plaster-dominant walls
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color woodColor(112, 78, 44);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(150, 110, 70);
        sf::Color plantBoxColor(96, 68, 40);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float wallH2 = wallH * 1.1f;
        // FIX (2026-08-07, "中间的一层是透明的" -- a real gap bug, the same
        // class as the original Town Hall one): this used to also declare a
        // separate `foundationH` band and fold it into `upperH`'s own
        // remainder math, reserving vertical space for it in every band
        // computed below `trimPos`/`upperPos`/the beams/windows/sign all
        // measured "up from foundationH+groundH" -- but no geometry was
        // ever actually drawn to FILL that reserved foundationH band. The
        // ground-floor piers/counter below only ever extended from y=0 to
        // y=groundH directly, leaving a real, totally empty horizontal
        // strip (foundationH tall, ~14 world units) between the top of the
        // piers and the jetty trim sitting above it -- background visible
        // straight through. Bank's own ground floor (same open-alcove
        // recipe this building's based on) never had this extra band at
        // all -- its piers already read as "foundation enough" on their
        // own. Removed `foundationH` entirely rather than adding geometry
        // to fill it; every other reference below updated from
        // `foundationH + groundH` down to plain `groundH`.
        float groundH = wallH2 * 0.36f;
        float trimH = 5.f;
        float upperH = wallH2 - groundH - trimH; // remainder, same "one band derived as the leftover" rule the Town Hall gap-bug taught this file
        float wallTop = wallH2;
        float gableRise = roofRise * 1.4f;
        float jettyDepth = 10.f;
        float depthJettied = b.size.y + jettyDepth;

        auto beamAt = [&](float wallZ, float x1, float y1, float x2, float y2, float thick) {
            addFacadeBeam(out, viewProj, windowSize, eye, wallZ, sf::Vector2f(basePos.x + x1, y1), sf::Vector2f(basePos.x + x2, y2), thick, beamColor, lc);
        };

        // ---- Ground floor: OPEN counter/alcove (Bank/Inn's own recipe)
        // instead of a closed door -- the whole point of a general store is
        // the goods on display, not an entrance. ----
        float pierW = b.size.x * 0.14f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x + b.size.x - pierW, 0.f, basePos.z), Vec3(pierW, groundH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        float openW = b.size.x - pierW * 2.f, openX = basePos.x + pierW;
        float backWallDepth = 14.f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(openX, 0.f, basePos.z), Vec3(openW, groundH * 0.86f, backWallDepth), stone, lc, &stoneTex, kStoneUv);
        float backWallFaceZ = basePos.z + backWallDepth;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(openX, 0.f, basePos.z + b.size.y - 10.f), Vec3(openW, groundH * 0.32f, 10.f), woodColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.5f, groundH * 0.34f, backWallFaceZ), openW * 0.55f, groundH * 0.6f, goodsTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(openX + openW * 0.5f, groundH * 0.5f, basePos.z + b.size.y * 0.55f), 60.f, glowTex, sf::Color(255, 214, 140, 90));

        // ---- Jetty trim + upper (jettied) floor, front gable roof --
        // exactly addRecruitmentCenterBuilding's own recipe. ----
        Vec3 trimPos(basePos.x, groundH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, trimPos, Vec3(b.size.x, trimH, depthJettied), beamColor, lc);
        Vec3 upperPos(basePos.x, groundH + trimH, basePos.z);
        addBandedBox(out, viewProj, windowSize, eye, upperPos, Vec3(b.size.x, upperH, depthJettied), plankWall, lc, &shingleTex, kShingleUv);

        addFrontGableRoof(out, viewProj, windowSize, eye, basePos, sf::Vector2f(b.size.x, depthJettied), wallTop, gableRise, roofColor, lc, &shingleTex, kShingleUv);

        float midXlocal = b.size.x * 0.5f;
        Vec3 gsw(basePos.x, wallTop, basePos.z + depthJettied), gse(basePos.x + b.size.x, wallTop, basePos.z + depthJettied);
        Vec3 gnw(basePos.x, wallTop, basePos.z), gne(basePos.x + b.size.x, wallTop, basePos.z);
        Vec3 ridgeS(basePos.x + midXlocal, wallTop + gableRise, basePos.z + depthJettied);
        Vec3 ridgeN(basePos.x + midXlocal, wallTop + gableRise, basePos.z);
        addTri(out, viewProj, windowSize, eye, gsw, gse, ridgeS, Vec3(0.f, 0.f, 1.f), plankWall, lc, &shingleTex, kShingleUv);
        addTri(out, viewProj, windowSize, eye, gne, gnw, ridgeN, Vec3(0.f, 0.f, -1.f), plankWall, lc, &shingleTex, kShingleUv);
        {
            float run = midXlocal;
            Vec3 westN = Vec3(-gableRise, run, 0.f).normalized(), eastN = Vec3(gableRise, run, 0.f).normalized();
            addShingleRows(out, viewProj, windowSize, eye, gsw, gnw, ridgeS, ridgeN, westN, roofColor, lc, &shingleTex, kShingleUv);
            addShingleRows(out, viewProj, windowSize, eye, gse, gne, ridgeS, ridgeN, eastN, roofColor, lc, &shingleTex, kShingleUv);
            addBox(out, viewProj, windowSize, eye, Vec3(ridgeN.x - 2.5f, ridgeN.y - 1.5f, ridgeN.z), Vec3(5.f, 5.f, depthJettied), shade3d(roofColor, -22), lc);
        }

        float upperSouthZ = basePos.z + depthJettied + 1.5f;
        float southZ = basePos.z + b.size.y + 1.5f;

        beamAt(upperSouthZ, midXlocal, wallTop, midXlocal, wallTop + gableRise - 6.f, 7.f);
        beamAt(upperSouthZ, 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(upperSouthZ, b.size.x - 4.f, wallTop, midXlocal, wallTop + gableRise - 6.f, 6.f);
        beamAt(upperSouthZ, 6.f, groundH + trimH, 6.f, wallTop - 2.f, 7.f);
        beamAt(upperSouthZ, b.size.x - 6.f, groundH + trimH, b.size.x - 6.f, wallTop - 2.f, 7.f);

        // ---- 2 windows on the upper floor, each with a real "+" mullion
        // and, new to this hero building, a small flower planter box just
        // below the sill -- the reference's own window-box read. ----
        float winSize = b.size.x * 0.15f, winY = groundH + trimH + upperH * 0.35f;
        for (float lx : { b.size.x * 0.16f, b.size.x * 0.68f }) {
            Vec3 winPos(basePos.x + lx, winY, upperSouthZ - 1.f);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            beamAt(upperSouthZ, lx, winY + winSize * 0.5f, lx + winSize, winY + winSize * 0.5f, 2.f);
            beamAt(upperSouthZ, lx + winSize * 0.5f, winY, lx + winSize * 0.5f, winY + winSize, 2.f);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, upperSouthZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 140));

            Vec3 boxPos(winPos.x - 2.f, winY - 8.f, upperSouthZ - 3.f);
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winY - 2.f, upperSouthZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

            // Flanking shutters (2026-08-07 detail pass) -- Staff Office's
            // own window shutters, never carried over to this building.
            float shutterW = winSize * 0.4f;
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x - shutterW - 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
            addBox(out, viewProj, windowSize, eye, Vec3(winPos.x + winSize + 2.f, winY, winPos.z), Vec3(shutterW, winSize, 2.5f), shade3d(beamColor, 12), lc);
        }

        // ---- A big flush signboard under the eave -- bigger than every
        // other hero building's own sign, matching how prominently the
        // reference's own 2-line sign reads across most of the facade. No
        // literal text baked in, same call every prior sign made -- the
        // floating name label already covers that. ----
        float signW = b.size.x * 0.7f, signH = upperH * 0.42f;
        Vec3 signPos(basePos.x + b.size.x * 0.5f - signW * 0.5f, groundH + trimH + upperH * 0.3f, upperSouthZ - 1.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // ---- A row of hanging lanterns along the jetty eave -- the
        // reference's own well-lit shopfront. ----
        for (float t : { 0.20f, 0.42f, 0.64f, 0.86f }) {
            Vec3 lanternPos(basePos.x + b.size.x * t, groundH - 6.f, upperSouthZ);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos, 16.f, glowTex, sf::Color(255, 200, 120, 160));
        }

        // ---- A ladder leaning against the east pier, plus barrels/crates/
        // pottery scattered in front -- the reference's own "general store
        // restocking" clutter. ----
        float ladderX = basePos.x + b.size.x - pierW - 4.f, ladderZ = southZ - 2.f;
        float ladderH = groundH * 0.85f;
        for (float lx : { 0.f, 9.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(ladderX + lx, 0.f, ladderZ), Vec3(2.2f, ladderH, 2.2f), beamColor, lc);
        }
        for (int i = 0; i < 5; ++i) {
            float ry = ladderH * (0.15f + 0.17f * static_cast<float>(i));
            addBox(out, viewProj, windowSize, eye, Vec3(ladderX, ry, ladderZ + 0.5f), Vec3(9.f, 1.5f, 1.5f), beamColor, lc);
        }

        Vec3 barrelPos(basePos.x + 6.f, 0.f, southZ + 6.f);
        addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(14.f, 22.f, 14.f), sf::Color(112, 76, 42), lc);
        addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.8f, 9.f, -0.8f), Vec3(15.6f, 4.f, 15.6f), sf::Color(70, 46, 26), lc);

        Vec3 cratePos(basePos.x + b.size.x * 0.5f - 8.f, 0.f, southZ + 12.f);
        addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(16.f, 14.f, 14.f), sf::Color(120, 86, 50), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, cratePos + Vec3(8.f, 14.f, 7.f), 20.f, 18.f, goodsTex, sf::Color::White);

        // Pottery jar -- a narrow-necked box on a wider base box, the crude
        // "no curved primitive here" workaround this file already relies
        // on for barrels/carts.
        Vec3 jarPos(basePos.x + b.size.x - 14.f, 0.f, southZ + 10.f);
        addBox(out, viewProj, windowSize, eye, jarPos, Vec3(12.f, 14.f, 12.f), sf::Color(178, 108, 68), lc);
        addBox(out, viewProj, windowSize, eye, jarPos + Vec3(3.f, 14.f, 3.f), Vec3(6.f, 5.f, 6.f), sf::Color(150, 88, 54), lc);
    }

    // Sawmill -- first hero building outside Town Square's own 9 ServiceHall
    // businesses (2026-08-07, from the user's own reference image -- a
    // water-powered mill: a stone-and-timber enclosed block with a window,
    // an OPEN saw bay on one side showing a circular blade mid-cut through
    // a log, a waterwheel on the exterior west wall fed by a small stream, a
    // sign board, log piles, a plank stack, and a small loading crane). Only
    // `b.id == "sawmill"` uses this. The user first said "伐木场" (Lumber
    // Camp) but that id is the RAW-harvesting flat-plot archetype (see
    // addLumberProps) shared with every other tier-1 producer across every
    // zone -- giving just that one a full building would break the
    // established "tier 1 = flat plot, tier 2+ = built structure" pattern
    // every other zone relies on. Confirmed with the user this reference is
    // actually for "锯木厂"/Sawmill (tier 2, the wood-processing business),
    // which had never gotten a bespoke shape at all before this (still the
    // plain generic box+roof everywhere outside Town Square).
    //
    // Wheel and saw blade are both round shapes this renderer has no
    // curved-geometry primitive for -- same "fake it as a flat sprite"
    // trick as every arch/clock decal already uses, just 2 new bakes.
    void addSawmillBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& wheelTex, const sf::Texture& sawBladeTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plankWall(140, 108, 66);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);
        sf::Color logColor(124, 84, 44);
        sf::Color logCapColor(172, 134, 88);
        sf::Color plankStackColor(180, 148, 96);
        sf::Color waterColor(90, 150, 190);
        sf::Color plantBoxColor(96, 68, 40);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = b.position.y + b.size.y + 1.5f;

        float wallH2 = wallH * 1.05f;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;

        // ---- Enclosed block (west 62% of the footprint): stone
        // foundation, timber-plank upper band, a real side-gable roof
        // (addGableRoof already emits its own gable-end triangles, so no
        // separate shingle-row overlay needed the way a front-gable hero
        // building's dramatic peak wants). ----
        float enclosedW = b.size.x * 0.62f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, roofRise, roofColor, lc, &shingleTex, kShingleUv);

        float winSize = enclosedW * 0.16f;
        Vec3 winPos(basePos.x + enclosedW * 0.5f - winSize * 0.5f, foundationH + upperH * 0.35f, southZ - 1.f);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x, winPos.y + winSize * 0.5f), sf::Vector2f(winPos.x + winSize, winPos.y + winSize * 0.5f), 2.f, beamColor, lc);
        addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(winPos.x + winSize * 0.5f, winPos.y), sf::Vector2f(winPos.x + winSize * 0.5f, winPos.y + winSize), 2.f, beamColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        // Sign, flush on the gable end above the eave.
        float signW = enclosedW * 0.6f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + roofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // ---- Open saw bay (east 38%): 2 stone piers, a recessed back wall
        // with the blade decal mounted on it, a log resting on a support in
        // front of the blade mid-cut, and a plain flat shed roof lower than
        // the enclosed block's own ridge (Kitchen's porch-roof technique). ----
        float bayW = b.size.x - enclosedW;
        float bayX0 = basePos.x + enclosedW;
        float bayWallH = foundationH + upperH * 0.7f;
        float pierW = bayW * 0.15f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(bayX0, 0.f, basePos.z), Vec3(pierW, bayWallH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(bayX0 + bayW - pierW, 0.f, basePos.z), Vec3(pierW, bayWallH, b.size.y), stone, lc, &stoneTex, kStoneUv);

        float backDepth = 14.f;
        float backWallH = foundationH + upperH * 0.55f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(bayX0 + pierW, 0.f, basePos.z), Vec3(bayW - pierW * 2.f, backWallH, backDepth), plankWall, lc, &shingleTex, kShingleUv);
        float backFaceZ = basePos.z + backDepth;

        float bladeSize = bayW * 0.5f;
        Vec3 bladeCenter(bayX0 + bayW * 0.5f, foundationH * 0.15f, backFaceZ);
        addBillboard(out, viewProj, windowSize, billboardRight, bladeCenter, bladeSize, bladeSize, sawBladeTex, sf::Color::White);

        // A log resting on 2 low supports, fed toward the blade -- lying
        // along Z like Warehouse's own log pile, so its south end (the one
        // the camera sees) is already the log's own cut face.
        float logLen = b.size.y * 0.55f;
        Vec3 logRestPos(bayX0 + bayW * 0.5f - 7.f, 14.f, basePos.z + 6.f);
        addBox(out, viewProj, windowSize, eye, Vec3(logRestPos.x - 2.f, 0.f, logRestPos.z + 4.f), Vec3(4.f, 14.f, 6.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(logRestPos.x - 2.f, 0.f, logRestPos.z + logLen - 10.f), Vec3(4.f, 14.f, 6.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, logRestPos, Vec3(14.f, 13.f, logLen), logColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(logRestPos.x + 2.f, logRestPos.y + 2.f, logRestPos.z + logLen - 0.5f), Vec3(10.f, 9.f, 1.5f), logCapColor, lc);

        float bayRoofY = wallTop * 0.78f;
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayRoofY, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -10), lc);

        // ---- Waterwheel on the exterior west wall, fed by a small stream
        // running past it (a genuine ground-level addGroundQuad fill, not
        // an elevated reuse -- safe to keep the default depth bias). ----
        float wheelSize = bayWallH * 1.15f;
        Vec3 wheelBase(basePos.x - wheelSize * 0.35f, 0.f, basePos.z + b.size.y * 0.32f);
        addBillboard(out, viewProj, windowSize, billboardRight, wheelBase, wheelSize, wheelSize, wheelTex, sf::Color::White);
        for (float dz : { -4.f, wheelSize * 0.6f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(wheelBase.x - 2.f, 0.f, wheelBase.z + dz), Vec3(4.f, wheelSize * 0.5f, 4.f), beamColor, lc);
        }
        addGroundQuad(out, viewProj, windowSize, eye, basePos.x - wheelSize * 0.85f, basePos.z, wheelSize * 0.6f, b.size.y, 0.5f, waterColor, lc);

        // ---- Log piles + a plank stack out front, and a small loading
        // crane -- reuses Warehouse's exact log-row technique (front-to-
        // back logs, own south face already reading as the cut end). ----
        float logX = basePos.x + enclosedW * 0.25f, logZ = southZ + 4.f;
        const struct { float dy, dz, len; } logRows[] = { {0.f, 0.f, 32.f}, {0.f, 14.f, 28.f}, {12.f, 7.f, 22.f} };
        for (const auto& row : logRows) {
            Vec3 lp(logX, row.dy, logZ + row.dz);
            addBox(out, viewProj, windowSize, eye, lp, Vec3(13.f, 12.f, row.len), logColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(lp.x + 2.f, lp.y + 2.f, lp.z + row.len - 0.5f), Vec3(9.f, 8.f, 1.5f), logCapColor, lc);
        }

        Vec3 plankBase(bayX0 + bayW * 0.15f, 0.f, southZ + 10.f);
        for (int i = 0; i < 5; ++i) {
            addBox(out, viewProj, windowSize, eye, plankBase + Vec3(0.f, static_cast<float>(i) * 2.6f, 0.f), Vec3(26.f, 2.2f, 11.f), shade3d(plankStackColor, (i % 2) * 8 - 4), lc);
        }

        // Small crane: a post, an angled-looking arm (2 stacked shorter
        // boxes standing in for a brace, no oriented-box primitive here),
        // and a log dangling from it.
        Vec3 cranePost(bayX0 + bayW - 6.f, 0.f, southZ + 18.f);
        addBox(out, viewProj, windowSize, eye, cranePost, Vec3(4.f, 42.f, 4.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(cranePost.x - 22.f, 38.f, cranePost.z + 1.f), Vec3(26.f, 4.f, 4.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(cranePost.x - 20.f, 20.f, cranePost.z + 1.5f), Vec3(2.f, 18.f, 2.f), beamColor, lc); // hoist rope
        addBox(out, viewProj, windowSize, eye, Vec3(cranePost.x - 26.f, 12.f, cranePost.z - 4.f), Vec3(14.f, 12.f, 12.f), logColor, lc); // the hanging log
    }

    // Mason -- from a fourteenth reference image, explicitly labeled "石匠铺"
    // (Stonemason's shop) itself this time, so unlike the Quarry round right
    // before this one, no AskUserQuestion scope-check was needed: a
    // monument-carving workshop -- a log-cabin building with a big stone
    // chimney, fronted by an open-air display yard of finished stonework
    // (a tombstone row, statues on pedestals, a small fountain, stone
    // columns, a bench, potted plants, lanterns). Only `b.id == "mason"`
    // uses this -- tier-2 stone processing, the sibling this file's own
    // Quarry-round header comment flagged as "still fully generic" and the
    // natural next target.
    //
    // Scaled down from the reference's own sprawling plaza (dozens of
    // statues/columns across a much bigger scene) to a representative
    // handful within this business' own shared 110x80 footprint -- same
    // "same silhouette, not a literal item count" call every other hero
    // building's own reference image got. Every yard prop's own footprint
    // was checked by hand against every other one before finalizing
    // coordinates, the same discipline addLumberProps'/addQuarryProps' own
    // layout rounds relied on.
    // v2 (2026-08-10, "我才发现这个石匠铺,我觉得不好看勒,能不能做成我那种版本,
    // 不要太高也可以" -- the user finally looked at it in-game and found it
    // doesn't read well; asked to hew closer to their own reference image
    // again, and explicitly said a lower/shorter building is fine). No
    // fresh screenshot this round, so this is a direct re-read of the
    // original reference against v1's own code rather than a diagnosed bug:
    // v1's yard was comparatively sparse (5 tombstones, 2 statues, all
    // clustered in the west/center) next to the reference's own densely
    // packed monument garden, and the workshop's log walls were flat-color-
    // plus-generic-shingle-texture with no actual log-course detail. Fixed
    // both:
    // - **Building height** cut roughly 25% (wallH ratio 0.95->0.70, per
    //   "不要太高也可以") -- the yard, not the workshop, is this building's
    //   real subject, same framing v1's own header comment already argued
    //   for, just followed through more aggressively this round.
    // - **Real horizontal log-course seams** (new, via addFacadeBeam reused
    //   for horizontal lines instead of its usual vertical/diagonal timber-
    //   frame use) across the upper wall band -- previously the "log
    //   cabin" read relied entirely on a generic shingle-grain texture with
    //   no actual log geometry, which is why it likely read as a plain
    //   plaster box rather than a log building.
    // - **Yard roughly doubled in density**: tombstones 5->9 (now spread
    //   across both the west AND east halves, not just west), statues 2->3,
    //   plus a new stone WALKWAY from the door straight down to the
    //   fountain (the reference's own clear central path, which v1 didn't
    //   have at all -- the yard read as an undifferentiated plaza before).
    //   Every prop's own footprint re-derived and checked by hand against
    //   every other one for the new, much tighter layout -- same discipline
    //   as every round in this file, just against a bigger prop count than
    //   v1 had to juggle.
    void addMasonBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& statueTex) {
        sf::Color logWall(112, 82, 50);
        sf::Color stone(150, 148, 142);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(90, 56, 38);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);
        sf::Color plazaColor(168, 164, 154);
        sf::Color pathColor(198, 194, 184);
        sf::Color tombColor(176, 174, 168);
        sf::Color tombCapColor(198, 196, 190);
        sf::Color columnColor(182, 178, 170);
        sf::Color benchColor(96, 68, 40);
        sf::Color chimneyColor(140, 138, 132);
        sf::Color waterColor(120, 168, 196);
        sf::Color plantBoxColor(96, 68, 40);
        sf::Color logSeamColor(70, 48, 28);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);

        // ---- Workshop building, the north 40% of the footprint -- a
        // modest, deliberately LOW log-cabin volume (log-plank walls over a
        // low stone footing) with a real side-gable roof and an oversized
        // stone chimney. The yard out front is this building's real focal
        // point, not the workshop itself. ----
        float buildingD = b.size.y * 0.40f;
        float wallH2 = wallH * 0.55f; // was 0.95 -> 0.70 -> 0.55, 2026-08-10 follow-up ("屋子可以再矮一点" -- the building can be even shorter) -- door/window/sign/chimney are all derived from wallH2 already, so they all scale down automatically with this one constant
        float footingH = wallH2 * 0.18f;
        float upperH = wallH2 - footingH;
        float wallTop = wallH2;
        float southZ = basePos.z + buildingD + 1.5f;

        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, footingH, buildingD), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, footingH, basePos.z), Vec3(b.size.x, upperH, buildingD), logWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(b.size.x, wallTop, buildingD), wallTop, roofRise, roofColor, lc, &shingleTex, kShingleUv);

        // Real horizontal log-course seams -- addFacadeBeam reused for
        // level (not vertical/diagonal) lines, standing in for the gaps
        // between stacked horizontal logs a real log-cabin wall would show.
        // Without this the wall was just a flat color plus a generic
        // shingle-grain texture with no actual log geometry.
        for (float t : { 0.16f, 0.36f, 0.56f, 0.76f, 0.94f }) {
            float y = footingH + upperH * t;
            addFacadeBeam(out, viewProj, windowSize, eye, southZ, sf::Vector2f(basePos.x, y), sf::Vector2f(basePos.x + b.size.x, y), 2.4f, logSeamColor, lc);
        }

        // Plain timber double door, centered, with a seam beam. `doorW`
        // (2026-08-10 follow-up, "那个门的比例有点奇怪" -- the door's
        // proportions look off) used to be a flat fraction of the whole
        // building's WIDTH (`b.size.x * 0.18f`), independent of `doorH` --
        // when this same round's earlier height cut (wallH ratio 0.95->
        // 0.70) shrank `doorH` along with it, `doorW` didn't shrink to
        // match (it was never tied to wall height at all), silently
        // throwing off the width:height ratio the door was originally
        // tuned at. Now derived FROM `doorH` (a fixed ~1:2.4 ratio) instead
        // of independently from the wall's own width, so any future height
        // retune keeps the door's own proportions correct automatically.
        float doorH = footingH + upperH * 0.6f;
        float doorW = doorH * 0.42f;
        Vec3 doorPos(basePos.x + b.size.x * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        // ---- Dressed-up entrance (2026-08-10 follow-up, "门可以做好看一点"
        // -- make the door look nicer): a stonemason's shop showing off a
        // plain timber slab at its own front door never made much thematic
        // sense next to everything else this building carries -- given a
        // real carved-stone doorway instead. Reuses the same `stoneTex`-
        // banded look the chimney/foundation already have, so it reads as
        // "the same material this shop works with," not a generic
        // decoration. ----
        {
            float jambW = 4.f, jambH = doorH + 5.f;
            Vec3 jambL(doorPos.x - jambW - 0.5f, 0.f, southZ - 1.f);
            Vec3 jambR(doorPos.x + doorW + 0.5f, 0.f, southZ - 1.f);
            addBandedBox(out, viewProj, windowSize, eye, jambL, Vec3(jambW, jambH, 5.f), stone, lc, &stoneTex, kStoneUv);
            addBandedBox(out, viewProj, windowSize, eye, jambR, Vec3(jambW, jambH, 5.f), stone, lc, &stoneTex, kStoneUv);
            // Lintel, spanning both jambs.
            addBandedBox(out, viewProj, windowSize, eye, Vec3(jambL.x, jambH, southZ - 1.5f), Vec3(jambR.x + jambW - jambL.x, 6.f, 6.f), stone, lc, &stoneTex, kStoneUv);
            // A 2nd, horizontal panel seam (the door already had one
            // vertical one) -- a real 4-panel door read instead of one
            // flat slab.
            addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x, doorH * 0.5f, southZ - 0.5f), Vec3(doorW, 1.6f, 2.f), shade3d(beamColor, -10), lc);
            // Handle.
            addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f + 3.f, doorH * 0.45f, southZ + 0.5f), Vec3(1.6f, 1.6f, 1.6f), sf::Color(70, 66, 60), lc);
            // A shallow entrance step -- flush with the jambs (not
            // widened further) so it stays clear of the stone-carving
            // work station just to its west (see that section's own
            // updated footprint below).
            addBox(out, viewProj, windowSize, eye, Vec3(jambL.x, 0.f, southZ + 3.f), Vec3(jambR.x + jambW - jambL.x, 3.f, 5.f), shade3d(stone, -12), lc);
        }

        // A window either side of the door, warm glow.
        float winSize = b.size.x * 0.12f, winY = footingH + upperH * 0.4f;
        for (float lx : { b.size.x * 0.15f, b.size.x * 0.7f }) {
            Vec3 winPos(basePos.x + lx, winY, southZ);
            addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winY + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        }

        // ---- Stone-carving work station (2026-08-10 follow-up round 2,
        // "没有看到你加的东西欸" -- the previous round's work station,
        // tucked by the west window at the building's own far-west edge
        // (dx 2-18), wasn't showing up at all. Root cause: this game's
        // camera centers on the PLAYER, and a player interacting with this
        // building stands at the DOOR (dx ~45-64) -- 30-50 units away from
        // where the station actually was, easily past the edge of frame at
        // any real zoom level, especially the close-in zoom the user's own
        // screenshots keep showing. Moved to flank the door directly
        // instead (dx 26-43) so it's in view exactly where the player
        // actually stands. Also fixes a real overlap the west-edge version
        // had: the old block (dx 4-14) and workbench (dx ~3-17) shared
        // both X AND Z range, silently intersecting -- the workbench here
        // is folded into the block's own south face (tools resting against
        // it) instead of a separate legged structure, both to fit the
        // tighter gap between the west tombstones (ending dx 26) and the
        // door (starting ~dx 45.5) and to remove that overlap risk
        // entirely rather than just re-spacing it. ----
        {
            float workZ = southZ + 1.f;
            sf::Color rawStone = shade3d(stone, -8);
            sf::Color carvedFace = shade3d(stone, 16);
            sf::Color chipColor(206, 202, 194);

            // Raw block with one smoothed/carved face (half-finished, not
            // the fully-worked look the yard's own finished statues have),
            // a couple of stone-chip debris bumps, and a chisel + mallet
            // resting directly against it (no separate workbench legs --
            // keeps the footprint tight enough to fit the gap).
            //
            // 2026-08-10 follow-up ("石像的位置跑掉了" -- the statue's
            // position looks wrong/out of place): re-checked the actual
            // available room in this apron rather than re-guessing --
            // the 2 nearest west tombstones sit at z 45-51, while this
            // whole cluster's own Z range is 34.5-41.5, so there was never
            // any Z overlap with them regardless of X -- meaning the
            // cluster had the FULL x6-41 span (up to the new stone
            // doorway's own jamb) to work with, not the cramped ~15-unit
            // strip the previous 2 rounds kept squeezing it into. Spread
            // back out across that real space instead: block moved
            // dx25->10, pedestal moved dx33->24 (now a genuine 5-unit gap
            // from the block and 11 units clear of the jamb, instead of
            // the ~3-unit gaps both were fighting for before) -- the
            // statue should now read as a deliberately placed piece, not
            // one crammed into a leftover corner.
            Vec3 blockPos(basePos.x + 10.f, 0.f, workZ);
            addBox(out, viewProj, windowSize, eye, blockPos, Vec3(9.f, 13.f, 7.f), rawStone, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 1.f, 3.f, blockPos.z + 7.f - 0.6f), Vec3(7.f, 8.f, 1.f), carvedFace, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 1.f, 0.f, blockPos.z + 7.5f), Vec3(3.f, 2.f, 3.f), chipColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 5.f, 0.f, blockPos.z + 7.f), Vec3(2.5f, 1.5f, 2.5f), chipColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 9.5f, 4.f, blockPos.z + 2.f), Vec3(1.2f, 6.f, 1.2f), sf::Color(150, 148, 142), lc); // chisel, leaned against the block
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 9.f, 3.f, blockPos.z + 4.f), Vec3(2.5f, 2.f, 2.f), sf::Color(90, 62, 34), lc); // mallet head
            addBox(out, viewProj, windowSize, eye, Vec3(blockPos.x + 9.5f, 3.f, blockPos.z + 4.5f), Vec3(1.f, 1.f, 5.f), sf::Color(120, 84, 48), lc); // mallet handle

            // Rough statue-in-progress on its own small pedestal -- the
            // same statueTex decal the yard uses, tinted grey/duller than
            // the finished yard statues' full-white tint to read as
            // unfinished. Pedestal color switched to the plain `stone`
            // tone (was the same darkened `rawStone` as the block) so it
            // doesn't visually blend into the raw block next to it.
            Vec3 pedPos(basePos.x + 24.f, 0.f, workZ);
            addBox(out, viewProj, windowSize, eye, pedPos, Vec3(7.f, 8.f, 7.f), stone, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, Vec3(pedPos.x + 3.5f, 8.f, pedPos.z + 3.5f), 14.f, 20.f, statueTex, sf::Color(190, 190, 186));
        }

        // Sign on the gable end.
        float signW = b.size.x * 0.5f, signH = 16.f;
        Vec3 signPos(basePos.x + b.size.x * 0.5f - signW * 0.5f, wallTop + roofRise * 0.3f, southZ - 1.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc);

        // Big stone chimney, east side, plus a soft smoke puff -- reuses
        // every other hero building's own chimney+glow-smoke trick.
        Vec3 chimneyPos(basePos.x + b.size.x * 0.82f, wallTop * 0.3f, basePos.z + buildingD * 0.5f);
        addBandedBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(14.f, wallTop * 0.85f, 12.f), chimneyColor, lc, &stoneTex, kStoneUv);
        addBox(out, viewProj, windowSize, eye, chimneyPos + Vec3(-2.f, wallTop * 0.85f, -2.f), Vec3(18.f, 3.f, 16.f), shade3d(chimneyColor, -15), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(7.f, wallTop * 0.85f + 14.f, 6.f), 16.f, glowTex, sf::Color(210, 210, 214, 90));

        // ---- Display yard, south of the workshop -- a stone plaza fill,
        // a central walkway from the door to the fountain, tombstones on
        // BOTH sides of the walkway, 3 statues, a small fountain, 2 stone
        // columns, a bench, potted plants, and lantern posts. ----
        float yardZ0 = basePos.z + buildingD + 2.f;
        float yardD = b.size.y - buildingD - 2.f;
        addGroundQuad(out, viewProj, windowSize, eye, basePos.x, yardZ0, b.size.x, yardD, 0.6f, plazaColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, sf::Vector2f(basePos.x, yardZ0), sf::Vector2f(b.size.x, yardD), sf::Color(60, 56, 50), lc);

        // Central stone walkway, door to fountain -- the reference's own
        // clear path through the yard, which v1 didn't have at all.
        addGroundQuad(out, viewProj, windowSize, eye, basePos.x + 48.f, yardZ0, 14.f, yardD, 0.7f, pathColor, lc);

        // Tombstones -- a base slab + a lighter, slightly narrower
        // "rounded cap" box on top (the same up-facing cap trick the tree
        // stumps/quarry rubble already use for a fake rounded silhouette),
        // now spread across BOTH sides of the walkway (west AND east)
        // instead of only the west half, closer to the reference's own
        // denser field of markers.
        {
            struct TombSpec { float dx, dz, w, h; };
            const TombSpec tombs[] = {
                // West side -- the front-most 2 (previously dz 36-37) moved
                // back to dz 45-46 this round, clearing a "work apron" right
                // in front of the building for the new stone-carving station
                // below (see its own comment). The 3rd (previously dx 30,
                // dz 39) is dropped outright this round -- the relocated
                // work station's own block now sits right where it was
                // (dx 26-35, z 34.5-41.5), a genuine overlap; removing it
                // was safer than hunting for yet another cramped gap
                // elsewhere in an already-dense yard.
                { 6.f, 46.f, 9.f, 15.f }, { 18.f, 45.f, 8.f, 12.f },
                { 8.f, 52.f, 8.f, 13.f }, { 20.f, 50.f, 9.f, 16.f }, { 32.f, 53.f, 8.f, 12.f },
                // East side -- the 2nd (previously dx 80, dz 38) moved to
                // dz 72 this round to free room for the stacked-stone
                // request right by the door (see its own updated comment
                // below) -- same "move the thing that's in the way,
                // don't hunt for a different gap" call the west side's
                // own tombstone relocations already made twice.
                { 68.f, 36.f, 9.f, 14.f }, { 76.f, 72.f, 8.f, 16.f }, { 68.f, 58.f, 9.f, 13.f },
            };
            for (const auto& t : tombs) {
                Vec3 tp(basePos.x + t.dx, 0.f, basePos.y + t.dz);
                addBox(out, viewProj, windowSize, eye, tp, Vec3(t.w, t.h, 6.f), tombColor, lc);
                addBox(out, viewProj, windowSize, eye, tp + Vec3(0.5f, t.h, 0.5f), Vec3(t.w - 1.f, 1.5f, 5.f), tombCapColor, lc);
            }
        }

        // 3 statues on stone pedestals -- no curved-geometry primitive in
        // this renderer, so the statue itself is a flat sprite decal (same
        // "fake it as a billboard" convention every arch/clock/cross decal
        // here already uses), standing on a real 3D pedestal box.
        {
            const sf::Vector2f statues[] = { { 36.f, 64.f }, { 66.f, 44.f }, { 80.f, 60.f } };
            for (const auto& s : statues) {
                Vec3 pedPos(basePos.x + s.x, 0.f, basePos.y + s.y);
                addBox(out, viewProj, windowSize, eye, pedPos, Vec3(10.f, 10.f, 10.f), columnColor, lc);
                addBillboard(out, viewProj, windowSize, billboardRight, Vec3(pedPos.x + 5.f, 10.f, pedPos.z + 5.f), 16.f, 26.f, statueTex, sf::Color::White);
            }
        }

        // Small fountain, at the walkway's own south end (its clearing) --
        // a wide basin box + a narrower raised rim box + a pale-blue
        // "water" cap, plus a faint glow for a bit of shimmer (a fixed
        // tint, not tied to the day/night point-light system -- reads as
        // glinting water in daylight too, unlike a real light).
        Vec3 fountainPos(basePos.x + 49.f, 0.f, basePos.y + 60.f);
        addBox(out, viewProj, windowSize, eye, fountainPos, Vec3(16.f, 5.f, 16.f), stone, lc);
        addBox(out, viewProj, windowSize, eye, fountainPos + Vec3(3.f, 5.f, 3.f), Vec3(10.f, 4.f, 10.f), shade3d(stone, -10), lc);
        addBox(out, viewProj, windowSize, eye, fountainPos + Vec3(4.f, 9.f, 4.f), Vec3(8.f, 1.5f, 8.f), waterColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, fountainPos + Vec3(8.f, 10.f, 8.f), 14.f, glowTex, sf::Color(180, 210, 230, 70));

        // 2 stone columns, far east edge -- a plain tall shaft + a wider
        // "capital" cap box, the classic monument-yard column read.
        for (float cz : { 36.f, 66.f }) {
            Vec3 colPos(basePos.x + 96.f, 0.f, basePos.y + cz);
            addBox(out, viewProj, windowSize, eye, colPos, Vec3(8.f, 34.f, 8.f), columnColor, lc);
            addBox(out, viewProj, windowSize, eye, colPos + Vec3(-1.5f, 34.f, -1.5f), Vec3(11.f, 3.f, 11.f), shade3d(columnColor, -10), lc);
        }

        // ---- A few more stone elements (2026-08-10 follow-up, "有石像了,
        // 可以的话在那个院子可以做多几个石头的元素吗" -- now that there's a
        // statue, add a few more stone pieces to the yard). An obelisk, a
        // stone urn on a pedestal, and 2 small stacked-stone cairns -- each
        // placed in a gap the existing dense layout had left clear (checked
        // by hand against every neighboring prop, same discipline as every
        // layout round in this file), not a blanket re-density pass. ----
        {
            // Obelisk -- a tall banded shaft capped with a small pyramid
            // tip, reusing `addPyramid` (the same primitive Mine/Gold
            // Mine's rock mound and Town Hall's clock-tower spire already
            // use), east side between the tombstones and the columns.
            Vec3 obeliskPos(basePos.x + 84.f, 0.f, basePos.y + 46.f);
            addBandedBox(out, viewProj, windowSize, eye, obeliskPos, Vec3(8.f, 34.f, 8.f), columnColor, lc);
            addPyramid(out, viewProj, windowSize, eye, obeliskPos + Vec3(0.f, 34.f, 0.f), sf::Vector2f(8.f, 8.f), 10.f, shade3d(columnColor, -10), lc);

            // A stone urn -- a narrow-necked box on a wider base box (the
            // same "no curved primitive, fake it as stacked boxes" trick
            // Storefront's own pottery jar already uses), on a short
            // pedestal, tucked between the 2 columns.
            Vec3 urnPedPos(basePos.x + 98.f, 0.f, basePos.y + 52.f);
            addBox(out, viewProj, windowSize, eye, urnPedPos, Vec3(6.f, 6.f, 6.f), columnColor, lc);
            addBox(out, viewProj, windowSize, eye, urnPedPos + Vec3(0.5f, 6.f, 0.5f), Vec3(5.f, 6.f, 5.f), stone, lc);
            addBox(out, viewProj, windowSize, eye, urnPedPos + Vec3(1.5f, 12.f, 1.5f), Vec3(3.f, 3.f, 3.f), shade3d(stone, -10), lc);

            // 2 small stone cairns (3 shrinking stacked stones each,
            // reusing the tombstone colors) in the open gap between the
            // bench and the walkway.
            for (const auto& c : { sf::Vector2f(34.f, 70.f), sf::Vector2f(42.f, 75.f) }) {
                Vec3 cp(basePos.x + c.x, 0.f, basePos.y + c.y);
                addBox(out, viewProj, windowSize, eye, cp, Vec3(6.f, 4.f, 6.f), tombColor, lc);
                addBox(out, viewProj, windowSize, eye, cp + Vec3(1.f, 4.f, 1.f), Vec3(4.f, 3.f, 4.f), shade3d(tombColor, -8), lc);
                addBox(out, viewProj, windowSize, eye, cp + Vec3(1.8f, 7.f, 1.8f), Vec3(2.4f, 2.5f, 2.4f), tombCapColor, lc);
            }

            // Stacked stone blocks -- a neat pile (reusing Quarry's own
            // "cut stone block stack" layering technique).
            //
            // 2026-08-10, 3rd attempt at this same request ("依旧没有" --
            // still not there, after the previous round moved it to dx 89
            // near the east column/chimney): moving it closer to the
            // building didn't fix it, which means the earlier "it's just
            // south of the wall, should be in frame" reasoning was
            // incomplete -- the confirmed-visible west work station sits
            // at dx 10-31 (24-45 units WEST of the door's own center),
            // while dx 89 is only 34 units EAST of center, closer to
            // center than the west items yet still not shown. That
            // asymmetry only makes sense if the camera's actual visible
            // window isn't centered on the building's own midpoint at
            // all (most likely the player themselves wasn't standing
            // exactly center-door when either screenshot was taken) --
            // not something predictable from this building's own geometry
            // alone. Rather than guess at an even-more-precise X offset
            // again, moved this as close to the door as the yard's own
            // layout allows (right past the east jamb, mirroring how
            // close the work station's own block sits to the west jamb)
            // -- the 2nd east tombstone (previously dx 80, dz 38) was
            // relocated (see the tombstone list above) specifically to
            // free this spot, the same "move what's in the way" call the
            // west apron's own tombstone relocations already made twice.
            Vec3 stackA(basePos.x + 78.f, 0.f, basePos.y + 37.f);
            for (int i = 0; i < 2; ++i) {
                addBox(out, viewProj, windowSize, eye, stackA + Vec3(0.f, static_cast<float>(i) * 5.4f, 0.f), Vec3(7.f, 5.f, 7.f), shade3d(stone, (i % 2) * 8 - 4), lc);
            }
        }

        // A bench (4 legs + a seat plank), south-west corner of the yard.
        {
            float benchLegH = 8.f;
            Vec3 benchPos(basePos.x + 10.f, 0.f, basePos.y + 68.f);
            for (float lx : { 2.f, 16.f }) {
                for (float lz : { 2.f, 8.f }) {
                    addBox(out, viewProj, windowSize, eye, benchPos + Vec3(lx, 0.f, lz), Vec3(2.f, benchLegH, 2.f), benchColor, lc);
                }
            }
            addBox(out, viewProj, windowSize, eye, benchPos + Vec3(0.f, benchLegH, 0.f), Vec3(20.f, 3.f, 12.f), benchColor, lc);
        }

        // Potted plants flanking the workshop's own door, just inside the
        // yard's own north edge (the walkway's own start).
        for (float px : { 44.f, 66.f }) {
            Vec3 boxPos(basePos.x + px, 0.f, basePos.y + 35.f);
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(6.f, 6.f, 6.f), plantBoxColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, boxPos + Vec3(3.f, 6.f, 3.f), 12.f, 14.f, flowerTex, sf::Color::White);
        }

        // 2 lantern posts flanking the walkway partway down, ahead of the
        // fountain clearing.
        for (float lx : { 44.f, 72.f }) {
            Vec3 lanternPos(basePos.x + lx, 0.f, basePos.y + 55.f);
            addBox(out, viewProj, windowSize, eye, lanternPos, Vec3(3.f, 24.f, 3.f), beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos + Vec3(1.5f, 28.f, 1.5f), 16.f, glowTex, sf::Color(255, 200, 120, 160));
        }
    }
}

const sf::Texture& GameWorld::getBillboard3D(const std::string& key, sf::Vector2u size,
    const std::function<void(sf::RenderTexture&)>& drawFn, bool repeated) {
    auto it = billboardCache3D_.find(key);
    if (it != billboardCache3D_.end()) return it->second->getTexture();

    auto rt = std::make_unique<sf::RenderTexture>(size);
    rt->setRepeated(repeated);
    rt->clear(sf::Color::Transparent);
    drawFn(*rt);
    rt->display();
    const sf::Texture& tex = rt->getTexture();
    billboardCache3D_.emplace(key, std::move(rt));
    return tex;
}

void GameWorld::getZoneCamera3D(Vec3& eye, Vec3& target) const {
    // Same downward-angled offset the fixed Phase 1 camera used, now
    // re-centered on the player each frame instead of the zone's fixed
    // midpoint -- clamped so the camera never pans far enough that the
    // ground plane runs out at the edge of frame (the zone is 1280x820;
    // margin keeps the target comfortably inside that).
    const Vec3 kCamOffset(0.f, 640.f, 870.f); // eye = target + this * cameraZoom3D_
    constexpr float kMarginX = 300.f, kMarginZ = 260.f;
    float px = playerPos_.x + 13.f, pz = playerPos_.y + 13.f; // player center, not top-left
    target = Vec3(std::clamp(px, kMarginX, 1280.f - kMarginX), 0.f, std::clamp(pz, kMarginZ, 820.f - kMarginZ));
    // Mouse-wheel zoom (2026-08-07, see run()'s MouseWheelScrolled handler
    // and cameraZoom3D_'s own comment) -- scales the fixed offset toward/
    // away from the target instead of moving the target or changing FOV,
    // so scrolling in gets the camera physically closer to the player
    // (Y and Z shrink together, keeping the same viewing angle) rather
    // than just "shrinking the picture."
    eye = target + kCamOffset * cameraZoom3D_;
}

sf::Vector2f GameWorld::raycastZoneGround3D(sf::Vector2f screenClick) const {
    Vec3 eye, target;
    getZoneCamera3D(eye, target);
    Vec3 forward = (target - eye).normalized();
    Vec3 right = cross(forward, Vec3(0.f, 1.f, 0.f)).normalized();
    Vec3 up = cross(right, forward).normalized();

    float w = static_cast<float>(windowSize_.x), h = static_cast<float>(windowSize_.y);
    constexpr float kFovY = 45.f * 3.14159265f / 180.f;
    float tanHalfFov = std::tan(kFovY * 0.5f);
    float aspect = w / h;
    float ndcX = (screenClick.x / w) * 2.f - 1.f;
    float ndcY = 1.f - (screenClick.y / h) * 2.f; // screen Y grows down, NDC Y grows up

    Vec3 rayDir = (forward + right * (ndcX * tanHalfFov * aspect) + up * (ndcY * tanHalfFov)).normalized();
    if (std::abs(rayDir.y) < 1e-5f) return sf::Vector2f(-1.f, -1.f); // parallel to the ground, no sensible hit
    float t = -eye.y / rayDir.y;
    if (t < 0.f) return sf::Vector2f(-1.f, -1.f); // ground is behind the camera along this ray
    Vec3 hit = eye + rayDir * t;
    return sf::Vector2f(hit.x, hit.z);
}

void GameWorld::draw3DBuildingHighlight(sf::RenderWindow& window, const WorldBuilding& b) const {
    Vec3 eye, target;
    getZoneCamera3D(eye, target);
    Mat4 view = Mat4::lookAt(eye, target, Vec3(0.f, 1.f, 0.f));
    float w = static_cast<float>(windowSize_.x), h = static_cast<float>(windowSize_.y);
    Mat4 proj = Mat4::perspective(45.f * 3.14159265f / 180.f, w / h, 10.f, 4000.f);
    Mat4 viewProj = Mat4::multiply(proj, view);

    // A little above the ground (y=1) so it doesn't z-fight with the
    // ground plane's own quad, traced around the building's footprint --
    // reads as a highlight ring sitting right at the building's base.
    Vec3 corners[4] = {
        Vec3(b.position.x, 1.f, b.position.y),
        Vec3(b.position.x + b.size.x, 1.f, b.position.y),
        Vec3(b.position.x + b.size.x, 1.f, b.position.y + b.size.y),
        Vec3(b.position.x, 1.f, b.position.y + b.size.y),
    };
    sf::Vector2f screen[4];
    for (int i = 0; i < 4; ++i) {
        Projected p = projectPoint(viewProj, corners[i], windowSize_);
        if (!p.valid) return; // any corner behind the camera -- skip the whole ring rather than draw a broken partial one
        screen[i] = p.screen;
    }

    sf::VertexArray va(sf::PrimitiveType::LineStrip, 5);
    for (int i = 0; i < 4; ++i) va[static_cast<std::size_t>(i)] = sf::Vertex{ screen[i], sf::Color::Yellow };
    va[4] = sf::Vertex{ screen[0], sf::Color::Yellow };
    window.draw(va);
}

void GameWorld::draw3DZone(sf::RenderWindow& window) {
    const Zone& z = zones_[currentZone_];
    float w = static_cast<float>(windowSize_.x), h = static_cast<float>(windowSize_.y);

    Vec3 eye, target;
    getZoneCamera3D(eye, target);
    Mat4 view = Mat4::lookAt(eye, target, Vec3(0.f, 1.f, 0.f));
    Mat4 proj = Mat4::perspective(45.f * 3.14159265f / 180.f, w / h, 10.f, 4000.f);
    Mat4 viewProj = Mat4::multiply(proj, view);

    // Billboard facing basis -- Y-locked (see addBillboard's comment), so
    // just the camera's world-horizontal right vector.
    Vec3 camForward = (target - eye).normalized();
    Vec3 billboardRight = cross(camForward, Vec3(0.f, 1.f, 0.f)).normalized();

    // Day/night tie-in: reuses the exact nightFactor() the 2D world's
    // drawGlow/lamp already key off of, so the 3D scene gets dark and the
    // point lights switch on in sync with everywhere else in the game.
    float night = nightFactor();
    LightingContext lc;
    // 2026-08-10 lighting-strengthen pass ("继续帮我把全部光影,全部加强的" --
    // push every lighting knob in this renderer harder, across the board):
    // ambient floor lowered (day 0.5->0.36, night 0.28->0.15) so sun-facing
    // vs. shaded surfaces read with real contrast instead of everything
    // sitting at least half-lit regardless of angle; sun dims further at
    // night (0.6->0.74 falloff) for a moodier, more differentiated night
    // scene instead of a shallow day/night delta.
    lc.sunStrength = 1.f - 0.74f * night;
    lc.ambient = 0.36f - 0.21f * night;
    float lightBoost = 0.14f + 1.05f * night; // lamps/windows: dim by day, brighter-than-before by night (0.88->1.05 ceiling)
    // Billboards keep their own baked-in pixel-art shading (see
    // addBillboard's comment) -- this is just a flat multiply so they still
    // read as part of the same darkened night scene instead of staying
    // full-bright while everything lit by litColor() dims around them.
    std::uint8_t billboardTint = clamp8_3d(static_cast<int>(255.f * (1.f - 0.35f * night)));
    sf::Color billboardDayNight(billboardTint, billboardTint, billboardTint);

    // Bumped from an initial 70/26 (2026-08-07 follow-up: reported as
    // looking "too small"/"hollow", not chunky enough to read as solid
    // volumes) -- a squat ~0.64:1 height:width box is much harder to
    // perceive as 3D at this camera's distance/tilt than one closer to or
    // taller than its own footprint.
    constexpr float kBuildingHeight = 96.f;
    constexpr float kRoofRise = 34.f;

    // One warm point light per building (approximating a lit window on its
    // south/front face) and one per lamp post -- built before the geometry
    // loop below since every face needs the whole light list. Skipped for a
    // locked business (its full shape is dimmed, not "in use" -- see
    // dimForLock3d below) and for one still at the bare-plot/construction-
    // site stage (nothing built yet to have a lit window).
    for (const auto& b : z.buildings) {
        bool locked = game_.isBusinessLocked(b.id);
        ConstructionInfo ci = locked ? ConstructionInfo{} : game_.businessConstructionInfo(b.id);
        if (locked || ci.requiresConstruction) continue;
        PointLight3D light;
        // 22 units out from the wall, not sitting right on it -- a light
        // placed at point-blank range always blows its own surface out
        // regardless of intensity/falloff tuning (see litColor's contrib
        // cap comment).
        light.pos = Vec3(b.position.x + b.size.x * 0.5f, kBuildingHeight * 0.45f, b.position.y + b.size.y + 22.f);
        light.color = Vec3(1.f, 0.78f, 0.43f);
        light.intensity = 40.f * lightBoost; // 30->40, 2026-08-10 lighting-strengthen pass
        lc.lights.push_back(light);
    }
    for (const auto& d : z.decorations) {
        if (d.kind != Decoration::Kind::Lamp) continue;
        PointLight3D light;
        light.pos = Vec3(d.position.x, 34.f, d.position.y);
        light.color = Vec3(1.f, 0.75f, 0.43f);
        light.intensity = 36.f * lightBoost; // 28->36, 2026-08-10 lighting-strengthen pass
        lc.lights.push_back(light);
    }

    // Billboard textures -- each rendered once via the exact same function
    // the 2D world uses, then cached (see getBillboard3D) and reused every
    // frame. Keyed by whatever actually changes the pixel art (season for
    // foliage, shirt color for people) so a cache hit is the common case.
    std::string season = std::to_string(static_cast<int>(game_.currentSeason()));
    const sf::Texture& treeTex = getBillboard3D("tree_" + season, sf::Vector2u(48, 60),
        [&](sf::RenderTexture& rt) { drawTree(rt, sf::Vector2f(24.f, 44.f)); });
    const sf::Texture& bushTex = getBillboard3D("bush_" + season, sf::Vector2u(34, 30),
        [&](sf::RenderTexture& rt) { drawBush(rt, sf::Vector2f(4.f, 4.f)); });
    const sf::Texture& lampTex = getBillboard3D("lamp", sf::Vector2u(30, 78),
        [&](sf::RenderTexture& rt) { drawLamp(rt, sf::Vector2f(15.f, 50.f)); });
    // Forageable (Highlands' berry pickups) -- same 3-dot cluster
    // drawForageable draws in 2D, baked once into a small billboard.
    const sf::Texture& forageTex = getBillboard3D("forageable", sf::Vector2u(20, 20),
        [&](sf::RenderTexture& rt) {
            const sf::Vector2f dots[] = { { -5.f, 0.f }, { 5.f, 0.f }, { 0.f, -7.f } };
            for (const auto& dp : dots) {
                sf::CircleShape berry(5.f);
                berry.setPosition(sf::Vector2f(10.f, 10.f) + dp - sf::Vector2f(5.f, 5.f));
                berry.setFillColor(sf::Color(210, 60, 90));
                berry.setOutlineThickness(1.f);
                berry.setOutlineColor(sf::Color(25, 20, 15));
                rt.draw(berry);
            }
        });
    // Bloom halo for each point light (see addGlowBillboard) -- reuses the
    // 2D world's own drawGlow soft-circle render, cached once as a plain
    // white sprite so each light can retint/resize it via the billboard's
    // own color multiply instead of baking a separate texture per light.
    const sf::Texture& glowTex = getBillboard3D("glow_dot", sf::Vector2u(96, 96),
        [&](sf::RenderTexture& rt) { drawGlow(rt, sf::Vector2f(48.f, 48.f), 40.f, sf::Color::White); });
    // Tileable material-grain textures (see bakeGrainPattern's own comment)
    // for opaque faces that opt into real texture instead of a flat/banded
    // color -- currently just Staff Office's hero building (see
    // addRecruitmentCenterBuilding), the natural next step being rolling
    // this same texturing onto the generic building box and Cottage too.
    // `repeated=true` is required here (not the default) -- these actually
    // tile past their own texture bounds, unlike every sprite billboard.
    const sf::Texture& stoneTex = getBillboard3D("mat_stone", sf::Vector2u(32, 32),
        [&](sf::RenderTexture& rt) { bakeGrainPattern(rt, sf::Vector2u(32, 32), 4, 4, true); }, /*repeated=*/true);
    const sf::Texture& plasterTex = getBillboard3D("mat_plaster", sf::Vector2u(24, 24),
        [&](sf::RenderTexture& rt) { bakeGrainPattern(rt, sf::Vector2u(24, 24), 6, 6, false); }, true);
    const sf::Texture& shingleTex = getBillboard3D("mat_shingle", sf::Vector2u(24, 16),
        [&](sf::RenderTexture& rt) { bakeGrainPattern(rt, sf::Vector2u(24, 16), 6, 4, true); }, true);
    // Bank interior-decal billboards (see addBankBuilding below) -- a vault
    // door and a wall of small storage drawers, mounted flat on the back
    // wall inside the ground floor's open counter archway (not tiled, so
    // no `repeated`).
    const sf::Texture& vaultTex = getBillboard3D("bank_vault", sf::Vector2u(28, 28), [&](sf::RenderTexture& rt) {
        sf::RectangleShape body(sf::Vector2f(26.f, 26.f));
        body.setPosition(sf::Vector2f(1.f, 1.f));
        body.setFillColor(sf::Color(70, 72, 78));
        body.setOutlineThickness(2.f);
        body.setOutlineColor(sf::Color(30, 30, 34));
        rt.draw(body);
        sf::CircleShape dial(8.f);
        dial.setPosition(sf::Vector2f(6.f, 6.f));
        dial.setFillColor(sf::Color(150, 140, 90));
        dial.setOutlineThickness(1.5f);
        dial.setOutlineColor(sf::Color(40, 38, 30));
        rt.draw(dial);
        sf::RectangleShape handle(sf::Vector2f(10.f, 3.f));
        handle.setPosition(sf::Vector2f(9.f, 13.f));
        handle.setFillColor(sf::Color(40, 38, 30));
        rt.draw(handle);
        const sf::Vector2f rivets[] = { {3.f,3.f},{23.f,3.f},{3.f,23.f},{23.f,23.f} };
        for (const auto& rp : rivets) {
            sf::CircleShape rivet(1.5f);
            rivet.setPosition(rp);
            rivet.setFillColor(sf::Color(150, 140, 90));
            rt.draw(rivet);
        }
        });
    const sf::Texture& cabinetTex = getBillboard3D("bank_cabinet", sf::Vector2u(34, 30), [&](sf::RenderTexture& rt) {
        sf::RectangleShape body(sf::Vector2f(34.f, 30.f));
        body.setFillColor(sf::Color(96, 66, 40));
        rt.draw(body);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 5; ++c) {
                sf::RectangleShape drawer(sf::Vector2f(5.5f, 5.5f));
                drawer.setPosition(sf::Vector2f(1.5f + static_cast<float>(c) * 6.5f, 1.5f + static_cast<float>(r) * 7.f));
                drawer.setFillColor(sf::Color(122, 88, 54));
                drawer.setOutlineThickness(0.6f);
                drawer.setOutlineColor(sf::Color(50, 34, 18));
                rt.draw(drawer);
                sf::CircleShape pull(0.7f);
                pull.setPosition(sf::Vector2f(3.7f + static_cast<float>(c) * 6.5f, 3.7f + static_cast<float>(r) * 7.f));
                pull.setFillColor(sf::Color(180, 150, 90));
                rt.draw(pull);
            }
        }
        });
    // Inn interior decal (see addInnBuilding below) -- a wall-mounted key
    // board, mounted flat on the check-in counter's back wall the same way
    // the Bank mounts its vault/cabinet decals. Reuses `cabinetTex` above
    // for the Inn's own shelf-of-goods decal instead of baking a near-
    // identical second texture.
    const sf::Texture& keysTex = getBillboard3D("inn_keys", sf::Vector2u(34, 22), [&](sf::RenderTexture& rt) {
        sf::RectangleShape board(sf::Vector2f(34.f, 22.f));
        board.setFillColor(sf::Color(90, 62, 36));
        rt.draw(board);
        for (int i = 0; i < 5; ++i) {
            float x = 3.f + static_cast<float>(i) * 6.2f;
            sf::CircleShape peg(1.f);
            peg.setPosition(sf::Vector2f(x, 2.f));
            peg.setFillColor(sf::Color(40, 30, 20));
            rt.draw(peg);
            sf::RectangleShape shaft(sf::Vector2f(1.2f, 10.f));
            shaft.setPosition(sf::Vector2f(x + 0.4f, 5.f));
            shaft.setFillColor(sf::Color(200, 180, 110));
            rt.draw(shaft);
            sf::CircleShape bow(2.2f);
            bow.setPosition(sf::Vector2f(x - 1.f, 13.f));
            bow.setFillColor(sf::Color(200, 180, 110));
            bow.setOutlineThickness(0.6f);
            bow.setOutlineColor(sf::Color(120, 100, 50));
            rt.draw(bow);
        }
        });
    // Kitchen's arched stone doorway decal (see addKitchenBuilding below) --
    // a rounded-top stone frame around a rounded-top dark opening, since
    // this renderer has no curved-geometry primitive to build a real arch
    // out of. A stone-colored CircleShape+RectangleShape "capsule" behind a
    // smaller dark one reads as an arch from a distance without needing
    // real geometry.
    const sf::Texture& archTex = getBillboard3D("kitchen_arch", sf::Vector2u(40, 56), [&](sf::RenderTexture& rt) {
        sf::RectangleShape frameBody(sf::Vector2f(40.f, 40.f));
        frameBody.setPosition(sf::Vector2f(0.f, 16.f));
        frameBody.setFillColor(sf::Color(148, 142, 130));
        rt.draw(frameBody);
        sf::CircleShape frameCap(20.f);
        frameCap.setPosition(sf::Vector2f(0.f, -4.f));
        frameCap.setFillColor(sf::Color(148, 142, 130));
        rt.draw(frameCap);
        sf::RectangleShape doorBody(sf::Vector2f(26.f, 34.f));
        doorBody.setPosition(sf::Vector2f(7.f, 22.f));
        doorBody.setFillColor(sf::Color(34, 24, 16));
        rt.draw(doorBody);
        sf::CircleShape doorCap(13.f);
        doorCap.setPosition(sf::Vector2f(7.f, 5.f));
        doorCap.setFillColor(sf::Color(34, 24, 16));
        rt.draw(doorCap);
        // A few voussoir seam lines radiating from the arch for texture.
        for (float ang : { -0.9f, -0.45f, 0.f, 0.45f, 0.9f }) {
            sf::RectangleShape seam(sf::Vector2f(1.2f, 9.f));
            seam.setPosition(sf::Vector2f(20.f + std::sin(ang) * 15.f - 0.6f, 16.f - std::cos(ang) * 15.f));
            seam.setRotation(sf::degrees(ang * 57.3f));
            seam.setFillColor(sf::Color(112, 106, 96));
            rt.draw(seam);
        }
        });
    // Town Hall's clock-face decal (see addTownHallBuilding below) -- same
    // "fake the round shape with a sprite" trick as the arch above, since
    // there's still no curved-geometry primitive in this renderer.
    const sf::Texture& clockTex = getBillboard3D("townhall_clock", sf::Vector2u(32, 32), [&](sf::RenderTexture& rt) {
        sf::CircleShape rim(16.f);
        rim.setFillColor(sf::Color(58, 46, 30));
        rt.draw(rim);
        sf::CircleShape face(13.f);
        face.setPosition(sf::Vector2f(3.f, 3.f));
        face.setFillColor(sf::Color(238, 230, 210));
        rt.draw(face);
        for (int i = 0; i < 12; ++i) {
            float ang = static_cast<float>(i) * 3.14159265f / 6.f;
            sf::RectangleShape tick(sf::Vector2f(1.f, 2.5f));
            tick.setPosition(sf::Vector2f(16.f + std::sin(ang) * 11.5f - 0.5f, 16.f - std::cos(ang) * 11.5f));
            tick.setRotation(sf::degrees(ang * 57.3f));
            tick.setFillColor(sf::Color(60, 50, 34));
            rt.draw(tick);
        }
        sf::RectangleShape hourHand(sf::Vector2f(1.6f, 7.f));
        hourHand.setPosition(sf::Vector2f(15.2f, 9.5f));
        hourHand.setFillColor(sf::Color(30, 24, 16));
        rt.draw(hourHand);
        sf::RectangleShape minHand(sf::Vector2f(1.2f, 10.5f));
        minHand.setPosition(sf::Vector2f(15.4f, 5.5f));
        minHand.setRotation(sf::degrees(50.f));
        minHand.setFillColor(sf::Color(30, 24, 16));
        rt.draw(minHand);
        });
    // Sawmill's waterwheel + saw-blade decals (see addSawmillBuilding
    // below) -- 2 more round shapes this renderer has no curved-geometry
    // primitive for, same "fake it as a flat sprite" convention as the
    // clock/arch decals above.
    const sf::Texture& wheelTex = getBillboard3D("sawmill_wheel", sf::Vector2u(32, 32), [&](sf::RenderTexture& rt) {
        sf::CircleShape rim(15.f);
        rim.setFillColor(sf::Color(110, 78, 44));
        rim.setOutlineThickness(1.5f);
        rim.setOutlineColor(sf::Color(70, 48, 26));
        rt.draw(rim);
        sf::CircleShape hub(4.5f);
        hub.setPosition(sf::Vector2f(11.5f, 11.5f));
        hub.setFillColor(sf::Color(60, 42, 24));
        rt.draw(hub);
        for (int i = 0; i < 8; ++i) {
            float ang = static_cast<float>(i) * 3.14159265f / 4.f;
            sf::RectangleShape spoke(sf::Vector2f(14.f, 2.2f));
            spoke.setPosition(sf::Vector2f(16.f, 15.9f));
            spoke.setOrigin(sf::Vector2f(0.f, 1.1f));
            spoke.setRotation(sf::degrees(ang * 57.3f));
            spoke.setFillColor(sf::Color(90, 62, 34));
            rt.draw(spoke);
        }
        for (int i = 0; i < 12; ++i) {
            float ang = static_cast<float>(i) * 3.14159265f / 6.f;
            sf::RectangleShape paddle(sf::Vector2f(1.6f, 4.f));
            paddle.setPosition(sf::Vector2f(16.f + std::sin(ang) * 14.f - 0.8f, 16.f - std::cos(ang) * 14.f));
            paddle.setRotation(sf::degrees(ang * 57.3f));
            paddle.setFillColor(sf::Color(60, 88, 106));
            rt.draw(paddle);
        }
        });
    const sf::Texture& sawBladeTex = getBillboard3D("sawmill_blade", sf::Vector2u(28, 28), [&](sf::RenderTexture& rt) {
        sf::CircleShape disc(13.f);
        disc.setFillColor(sf::Color(168, 172, 176));
        disc.setOutlineThickness(1.f);
        disc.setOutlineColor(sf::Color(90, 92, 96));
        rt.draw(disc);
        for (int i = 0; i < 16; ++i) {
            float ang = static_cast<float>(i) * 3.14159265f / 8.f;
            sf::CircleShape tooth(2.f);
            tooth.setPosition(sf::Vector2f(14.f + std::sin(ang) * 12.f - 2.f, 14.f - std::cos(ang) * 12.f - 2.f));
            tooth.setFillColor(sf::Color(70, 72, 76));
            rt.draw(tooth);
        }
        sf::CircleShape hub(3.f);
        hub.setPosition(sf::Vector2f(11.f, 11.f));
        hub.setFillColor(sf::Color(50, 52, 56));
        rt.draw(hub);
        });
    // Market's produce-crate decal (see addMarketBuilding below) -- a wood
    // crate with a handful of round fruits/vegetables poking over the rim,
    // mounted on each stall's own counter the same way Bank mounts its
    // vault/cabinet decals.
    const sf::Texture& crateTex = getBillboard3D("market_crate", sf::Vector2u(30, 28), [&](sf::RenderTexture& rt) {
        sf::RectangleShape crate(sf::Vector2f(28.f, 16.f));
        crate.setPosition(sf::Vector2f(1.f, 12.f));
        crate.setFillColor(sf::Color(150, 108, 62));
        crate.setOutlineThickness(1.2f);
        crate.setOutlineColor(sf::Color(90, 62, 34));
        rt.draw(crate);
        for (float x : { 5.f, 15.f, 25.f }) {
            sf::RectangleShape slat(sf::Vector2f(1.2f, 16.f));
            slat.setPosition(sf::Vector2f(x, 12.f));
            slat.setFillColor(sf::Color(112, 78, 44));
            rt.draw(slat);
        }
        const std::pair<sf::Vector2f, sf::Color> produce[] = {
            { {6.f, 6.f}, sf::Color(196, 48, 40) },   // apple
            { {14.f, 3.f}, sf::Color(226, 140, 40) }, // orange/pumpkin
            { {22.f, 6.f}, sf::Color(96, 148, 62) },  // cabbage
            { {10.f, 8.f}, sf::Color(120, 64, 150) }, // grapes/plum
        };
        for (const auto& [p, col] : produce) {
            sf::CircleShape fruit(4.f);
            fruit.setPosition(p);
            fruit.setFillColor(col);
            fruit.setOutlineThickness(0.8f);
            fruit.setOutlineColor(sf::Color(30, 24, 16));
            rt.draw(fruit);
        }
        });
    // Storefront's general-store goods decal (see addStorefrontBuilding
    // below) -- a shelf of mixed jars/baskets/sacks, mounted flat on the
    // counter's recessed back wall the same way Bank mounts its vault/
    // cabinet decals, and reused a second time on a crate out front.
    const sf::Texture& goodsTex = getBillboard3D("storefront_goods", sf::Vector2u(40, 30), [&](sf::RenderTexture& rt) {
        sf::RectangleShape shelf(sf::Vector2f(40.f, 3.f));
        shelf.setPosition(sf::Vector2f(0.f, 20.f));
        shelf.setFillColor(sf::Color(94, 66, 40));
        rt.draw(shelf);
        const std::pair<sf::Vector2f, sf::Color> jars[] = {
            { {2.f, 10.f}, sf::Color(178, 108, 68) }, { {10.f, 6.f}, sf::Color(150, 88, 54) },
        };
        for (const auto& [p, col] : jars) {
            sf::RectangleShape jar(sf::Vector2f(6.f, 10.f));
            jar.setPosition(p);
            jar.setFillColor(col);
            jar.setOutlineThickness(0.8f);
            jar.setOutlineColor(sf::Color(30, 24, 16));
            rt.draw(jar);
        }
        sf::RectangleShape basket(sf::Vector2f(9.f, 7.f));
        basket.setPosition(sf::Vector2f(18.f, 13.f));
        basket.setFillColor(sf::Color(150, 112, 64));
        basket.setOutlineThickness(0.8f);
        basket.setOutlineColor(sf::Color(30, 24, 16));
        rt.draw(basket);
        for (float x : { 20.f, 23.5f, 27.f }) {
            sf::CircleShape fruit(1.6f);
            fruit.setPosition(sf::Vector2f(x, 9.f));
            fruit.setFillColor(sf::Color(196, 48, 40));
            rt.draw(fruit);
        }
        sf::RectangleShape sack(sf::Vector2f(9.f, 11.f));
        sack.setPosition(sf::Vector2f(29.f, 9.f));
        sack.setFillColor(sf::Color(196, 168, 118));
        sack.setOutlineThickness(0.8f);
        sack.setOutlineColor(sf::Color(30, 24, 16));
        rt.draw(sack);
        });
    // Clinic's medical-cross sign decal and flower-planter decal (see
    // addClinicBuilding below) -- the cross is another "fake it as a flat
    // sprite" case (a plaque background + a painted cross, no separate
    // geometry), the flowers are a small foliage-plus-blooms cluster reused
    // across every planter box lining the building's front.
    const sf::Texture& crossTex = getBillboard3D("clinic_cross", sf::Vector2u(34, 34), [&](sf::RenderTexture& rt) {
        sf::RectangleShape plaque(sf::Vector2f(34.f, 34.f));
        plaque.setFillColor(sf::Color(238, 234, 220));
        plaque.setOutlineThickness(1.5f);
        plaque.setOutlineColor(sf::Color(150, 140, 120));
        rt.draw(plaque);
        sf::RectangleShape vBar(sf::Vector2f(8.f, 26.f));
        vBar.setPosition(sf::Vector2f(13.f, 4.f));
        vBar.setFillColor(sf::Color(60, 150, 78));
        rt.draw(vBar);
        sf::RectangleShape hBar(sf::Vector2f(26.f, 8.f));
        hBar.setPosition(sf::Vector2f(4.f, 13.f));
        hBar.setFillColor(sf::Color(60, 150, 78));
        rt.draw(hBar);
        });
    const sf::Texture& flowerTex = getBillboard3D("clinic_flowers", sf::Vector2u(20, 18), [&](sf::RenderTexture& rt) {
        sf::RectangleShape foliage(sf::Vector2f(20.f, 10.f));
        foliage.setPosition(sf::Vector2f(0.f, 8.f));
        foliage.setFillColor(sf::Color(72, 108, 58));
        rt.draw(foliage);
        const std::pair<sf::Vector2f, sf::Color> blooms[] = {
            { {3.f, 2.f}, sf::Color(178, 92, 168) },
            { {9.f, 0.f}, sf::Color(230, 230, 230) },
            { {14.f, 3.f}, sf::Color(200, 60, 70) },
            { {6.f, 5.f}, sf::Color(230, 180, 70) },
        };
        for (const auto& [p, col] : blooms) {
            sf::CircleShape bloom(2.6f);
            bloom.setPosition(p);
            bloom.setFillColor(col);
            rt.draw(bloom);
        }
        });
    // Mason's statue decal (see addMasonBuilding below) -- another "fake a
    // non-box shape as a flat sprite" case, same convention as the arch/
    // clock/cross decals: a rounded head + a robed/tapered body, pale
    // stone-grey, deliberately generic (reads as any carved figure, not a
    // specific one) since the reference's own statues vary prop to prop.
    const sf::Texture& statueTex = getBillboard3D("mason_statue", sf::Vector2u(20, 34), [&](sf::RenderTexture& rt) {
        sf::ConvexShape robe(4);
        robe.setPoint(0, sf::Vector2f(7.f, 10.f));
        robe.setPoint(1, sf::Vector2f(13.f, 10.f));
        robe.setPoint(2, sf::Vector2f(17.f, 34.f));
        robe.setPoint(3, sf::Vector2f(3.f, 34.f));
        robe.setFillColor(sf::Color(196, 194, 188));
        robe.setOutlineThickness(1.f);
        robe.setOutlineColor(sf::Color(140, 138, 132));
        rt.draw(robe);
        sf::CircleShape head(5.f);
        head.setPosition(sf::Vector2f(5.f, 0.f));
        head.setFillColor(sf::Color(202, 200, 194));
        head.setOutlineThickness(1.f);
        head.setOutlineColor(sf::Color(140, 138, 132));
        rt.draw(head);
        });
    // Farm's own animal/produce decals (see addFarmProps below) -- same
    // small hand-drawn sprite convention as the sheep/fruit-tree/herb-tuft
    // billboards just below, baked once each.
    const sf::Texture& chickenTex = getBillboard3D("farm_chicken", sf::Vector2u(16, 16), [&](sf::RenderTexture& rt) {
        sf::CircleShape body(6.f);
        body.setPosition(sf::Vector2f(3.f, 5.f));
        body.setFillColor(sf::Color(245, 240, 228));
        body.setOutlineThickness(1.f);
        body.setOutlineColor(sf::Color(180, 172, 156));
        rt.draw(body);
        sf::ConvexShape comb(3);
        comb.setPoint(0, sf::Vector2f(6.f, 1.f));
        comb.setPoint(1, sf::Vector2f(9.f, 1.f));
        comb.setPoint(2, sf::Vector2f(7.5f, 5.f));
        comb.setFillColor(sf::Color(196, 60, 50));
        rt.draw(comb);
        sf::ConvexShape beak(3);
        beak.setPoint(0, sf::Vector2f(2.f, 8.f));
        beak.setPoint(1, sf::Vector2f(-1.f, 9.f));
        beak.setPoint(2, sf::Vector2f(2.f, 10.5f));
        beak.setFillColor(sf::Color(226, 160, 60));
        rt.draw(beak);
        });
    const sf::Texture& pigTex = getBillboard3D("farm_pig", sf::Vector2u(24, 18), [&](sf::RenderTexture& rt) {
        sf::CircleShape body(9.f);
        body.setPosition(sf::Vector2f(4.f, 0.f));
        body.setFillColor(sf::Color(232, 168, 172));
        body.setOutlineThickness(1.f);
        body.setOutlineColor(sf::Color(176, 118, 122));
        rt.draw(body);
        sf::CircleShape snout(4.f);
        snout.setPosition(sf::Vector2f(0.f, 6.f));
        snout.setFillColor(sf::Color(210, 140, 146));
        snout.setOutlineThickness(0.8f);
        snout.setOutlineColor(sf::Color(176, 118, 122));
        rt.draw(snout);
        for (float dy : { 3.f, 9.f }) {
            sf::ConvexShape ear(3);
            ear.setPoint(0, sf::Vector2f(6.f, dy));
            ear.setPoint(1, sf::Vector2f(10.f, dy - 3.f));
            ear.setPoint(2, sf::Vector2f(10.f, dy + 1.f));
            ear.setFillColor(sf::Color(220, 150, 156));
            rt.draw(ear);
        }
        });
    // A loose cluster of mixed garden produce, no crate underneath (unlike
    // Market's own crate decal) -- meant to sit directly on a crop bed's
    // own ground fill, reused across several bed types (cabbage/carrot/
    // potato) the same way Bank/Inn/Storefront already reuse one decal
    // across more than one context.
    const sf::Texture& veggieTex = getBillboard3D("farm_veggies", sf::Vector2u(20, 16), [&](sf::RenderTexture& rt) {
        const std::pair<sf::Vector2f, sf::Color> veg[] = {
            { {2.f, 6.f}, sf::Color(96, 148, 62) },   // cabbage
            { {9.f, 3.f}, sf::Color(226, 120, 40) },  // carrot top
            { {14.f, 7.f}, sf::Color(178, 60, 50) },  // tomato/beet
            { {6.f, 10.f}, sf::Color(150, 108, 62) }, // potato mound
        };
        for (const auto& [p, col] : veg) {
            sf::CircleShape v(3.6f);
            v.setPosition(p);
            v.setFillColor(col);
            v.setOutlineThickness(0.8f);
            v.setOutlineColor(sf::Color(30, 24, 16));
            rt.draw(v);
        }
        });
    // A round cabbage head -- 3 concentric circles (dark-outlined base,
    // mid highlight, pale core) standing in for real sphere shading, the
    // same "fake volume with layered flat shapes" trick this renderer
    // already leans on everywhere it has no curved-geometry primitive
    // (see the statue/fountain/pig decals). Replaces the generic
    // farm_veggies cluster for cabbage specifically (2026-08-10 follow-up,
    // "卷心菜就是绿色球体吧" -- cabbage should read as a green sphere).
    const sf::Texture& cabbageTex = getBillboard3D("farm_cabbage", sf::Vector2u(20, 20), [&](sf::RenderTexture& rt) {
        sf::CircleShape head(9.f);
        head.setPosition(sf::Vector2f(1.f, 1.f));
        head.setFillColor(sf::Color(94, 148, 66));
        head.setOutlineThickness(1.2f);
        head.setOutlineColor(sf::Color(52, 88, 40));
        rt.draw(head);
        sf::CircleShape highlight(5.f);
        highlight.setPosition(sf::Vector2f(4.f, 3.f));
        highlight.setFillColor(sf::Color(130, 182, 92));
        rt.draw(highlight);
        sf::CircleShape core(3.f);
        core.setPosition(sf::Vector2f(7.f, 7.f));
        core.setFillColor(sf::Color(176, 208, 128));
        rt.draw(core);
        });
    // Bespoke-shape billboards (see addPastureProps/addOrchardProps/
    // addHerbGardenProps/addVineyardProps below) -- small hand-drawn
    // sprites reproducing their 2D counterpart's shapes, baked once.
    const sf::Texture& sheepTex = getBillboard3D("sheep", sf::Vector2u(24, 20), [&](sf::RenderTexture& rt) {
        sf::CircleShape puff(8.f);
        puff.setPosition(sf::Vector2f(5.f, 4.f));
        puff.setFillColor(sf::Color(240, 240, 235));
        puff.setOutlineThickness(1.f);
        puff.setOutlineColor(sf::Color(180, 180, 175));
        rt.draw(puff);
        sf::CircleShape head(4.5f);
        head.setPosition(sf::Vector2f(1.f, 1.f));
        head.setFillColor(sf::Color(60, 50, 45));
        rt.draw(head);
        });
    const sf::Texture& fruitTreeTex = getBillboard3D("fruit_tree", sf::Vector2u(26, 34), [&](sf::RenderTexture& rt) {
        sf::RectangleShape trunk(sf::Vector2f(4.f, 12.f));
        trunk.setPosition(sf::Vector2f(11.f, 20.f));
        trunk.setFillColor(sf::Color(96, 64, 32));
        rt.draw(trunk);
        sf::CircleShape canopy(9.f);
        canopy.setPosition(sf::Vector2f(4.f, 3.f));
        canopy.setFillColor(sf::Color(200, 90, 70));
        rt.draw(canopy);
        });
    const sf::Texture& herbTuftTex = getBillboard3D("herb_tuft", sf::Vector2u(14, 14), [&](sf::RenderTexture& rt) {
        sf::CircleShape tuft(5.f);
        tuft.setPosition(sf::Vector2f(2.f, 2.f));
        tuft.setFillColor(sf::Color(110, 160, 80));
        rt.draw(tuft);
        });
    const sf::Texture& grapeTex = getBillboard3D("grape_cluster", sf::Vector2u(10, 10), [&](sf::RenderTexture& rt) {
        sf::CircleShape grape(3.5f);
        grape.setPosition(sf::Vector2f(1.5f, 1.5f));
        grape.setFillColor(sf::Color(110, 60, 130));
        rt.draw(grape);
        });
    auto personKey = [](sf::Color c) {
        return "person_" + std::to_string(c.r) + "_" + std::to_string(c.g) + "_" + std::to_string(c.b);
    };
    auto getPersonTex = [&](sf::Color shirt) -> const sf::Texture& {
        return getBillboard3D(personKey(shirt), sf::Vector2u(40, 50),
            [&, shirt](sf::RenderTexture& rt) { drawPixelPerson(rt, sf::Vector2f(20.f, 42.f), shirt, false, 0.f); });
    };

    std::vector<ScreenQuad> quads;
    quads.reserve(384);

    // Ground: base grass plane, then path/water/sand decorations as
    // slightly-raised colored quads on top (raised only so the painter's
    // sort below never has to tie-break against the base plane, not a
    // real height). Both go through addGroundQuad, not a single addFace --
    // see its comment for why a lone full-depth quad here was erasing the
    // north-row buildings.
    addGroundQuad(quads, viewProj, windowSize_, eye, 0.f, 0.f, w, h, 0.f, sf::Color(58, 128, 68), lc);
    for (const auto& d : z.decorations) {
        sf::Color groundColor;
        if (d.kind == Decoration::Kind::Path) groundColor = sf::Color(176, 152, 104);
        else if (d.kind == Decoration::Kind::Water) groundColor = sf::Color(48, 92, 130);
        else if (d.kind == Decoration::Kind::Sand) groundColor = sf::Color(222, 198, 150);
        // GrassPatch is a translucent (alpha 120) tint over the base grass
        // in the 2D world (see drawZone) -- there's no cheap alpha-blended
        // ground layer here (addGroundQuad's quads are opaque, lit like
        // everything else), so this is that same patch color pre-blended
        // over the base grass at the same alpha instead, as a flat opaque
        // quad on top.
        else if (d.kind == Decoration::Kind::GrassPatch) groundColor = sf::Color(50, 125, 60);
        else continue;
        addGroundQuad(quads, viewProj, windowSize_, eye, d.position.x, d.position.y, d.size.x, d.size.y, 0.4f, groundColor, lc);
    }

    // Buildings: walls + a real pitched roof. Wall/roof colors come from
    // the same labelColor the 2D drawCottageShape/etc. already derive from.
    // Mirrors drawBuilding's 2D state machine (see its own comment) for
    // locked/construction, minus the 2D version's signboard/material-list
    // detail -- see this file's header comment for what's left as a
    // follow-up.
    for (const auto& b : z.buildings) {
        bool locked = game_.isBusinessLocked(b.id);
        ConstructionInfo ci = locked ? ConstructionInfo{} : game_.businessConstructionInfo(b.id);
        if (ci.requiresConstruction) {
            if (!ci.inProgress) {
                // State 1/3, "未开发的土地" (2026-08-07, see addUndevelopedPlot's
                // own header comment) -- an unstarted plot hasn't claimed
                // the land yet, so it reads as ordinary landscape (rocks/
                // sapling/wildflowers scattered over the zone's own base
                // grass) instead of a marked-off flat-colored lot. Applies
                // uniformly to every business in this state across every
                // zone, same as the flat-lot placeholder it replaces did --
                // this is the generic per-building loop, not a per-id one.
                addUndevelopedPlot(quads, viewProj, windowSize_, eye, b, lc, billboardRight, treeTex, bushTex, flowerTex, herbTuftTex, billboardDayNight);
                continue;
            }
            // State 2/3, construction site (in progress) -- see
            // addConstructionSiteProps's own header comment.
            addConstructionSiteProps(quads, viewProj, windowSize_, eye, b, lc, ci, kBuildingHeight, kRoofRise, stoneTex);
            continue;
        }
        // Bespoke shapes (see the anon-namespace addFarmProps/addMineProps/
        // etc above, and this file's header comment) -- mirrors drawBuilding's
        // 2D id dispatch for the same 9 archetypes + Cottage. Skipped while
        // locked: dimForLock3d only has a wall/roof color to work with, so a
        // locked one of these falls through to the generic dimmed box below
        // instead of a dimmed bespoke shape (2D dims the full bespoke shape
        // via an overlay; there's no equivalent cheap overlay pass in 3D --
        // see dimForLock3d's own comment).
        if (!locked) {
            if (b.id == "farm") { addFarmProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, chickenTex, pigTex, veggieTex, flowerTex, forageTex, cabbageTex, game_.farmCropId()); continue; }
            if (b.id == "mine") { addMineProps(quads, viewProj, windowSize_, eye, b, lc, sf::Color(114, 106, 100)); continue; }
            if (b.id == "goldmine") { addGoldMineProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
            if (b.id == "lumber") { addLumberProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
            if (b.id == "quarry") { addQuarryProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
            if (b.id == "sheep") { addPastureProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, sheepTex, billboardDayNight); continue; }
            if (b.id == "orchard") { addOrchardProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fruitTreeTex, billboardDayNight); continue; }
            if (b.id == "herbgarden") { addHerbGardenProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, herbTuftTex, billboardDayNight); continue; }
            if (b.id == "vineyard") { addVineyardProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, grapeTex, billboardDayNight); continue; }
            if (b.id == "doctor") { addClinicBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, billboardRight, glowTex, stoneTex, shingleTex, archTex, crossTex, flowerTex); continue; }
            if (b.id == "staff") { addRecruitmentCenterBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex); continue; }
            if (b.id == "bank") { addBankBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, vaultTex, cabinetTex); continue; }
            if (b.id == "sleep") { addInnBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, keysTex, cabinetTex); continue; }
            if (b.id == "eat") { addKitchenBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, archTex); continue; }
            if (b.id == "townhall") { addTownHallBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, archTex, clockTex); continue; }
            if (b.id == "market") { addMarketBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, crateTex); continue; }
            if (b.id == "warehouse") { addWarehouseBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex); continue; }
            if (b.id == "storefront") { addStorefrontBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, goodsTex); continue; }
            if (b.id == "sawmill") { addSawmillBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, wheelTex, sawBladeTex); continue; }
            if (b.id == "mason") { addMasonBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, statueTex); continue; }
        }

        sf::Color wallColor = shade3d(b.labelColor, -70);
        sf::Color roofColor = shade3d(b.labelColor, -110);
        if (locked) {
            wallColor = dimForLock3d(wallColor);
            roofColor = dimForLock3d(roofColor);
        }
        Vec3 basePos(b.position.x, 0.f, b.position.y);
        Vec3 size(b.size.x, kBuildingHeight, b.size.y);
        addBandedBox(quads, viewProj, windowSize_, eye, basePos, size, wallColor, lc);
        addGableRoof(quads, viewProj, windowSize_, eye, basePos, size, kBuildingHeight, kRoofRise, roofColor, lc);
    }

    // Billboarded pixel-art props -- trees/bushes/lamps reuse the exact 2D
    // sprites via the texture cache above.
    for (const auto& d : z.decorations) {
        if (d.kind == Decoration::Kind::Tree) {
            addBillboard(quads, viewProj, windowSize_, billboardRight,
                Vec3(d.position.x, 0.f, d.position.y), 48.f, 60.f, treeTex, billboardDayNight);
        } else if (d.kind == Decoration::Kind::Bush) {
            addBillboard(quads, viewProj, windowSize_, billboardRight,
                Vec3(d.position.x, 0.f, d.position.y), 34.f, 30.f, bushTex, billboardDayNight);
        } else if (d.kind == Decoration::Kind::Lamp) {
            addBillboard(quads, viewProj, windowSize_, billboardRight,
                Vec3(d.position.x, 0.f, d.position.y), 30.f, 78.f, lampTex, billboardDayNight);
        }
    }
    for (const auto& f : z.forageables) {
        if (!f.active) continue;
        addBillboard(quads, viewProj, windowSize_, billboardRight,
            Vec3(f.home.x, 0.f, f.home.y), 20.f, 20.f, forageTex, billboardDayNight);
    }
    for (const auto& npc : z.npcs) {
        addBillboard(quads, viewProj, windowSize_, billboardRight,
            Vec3(npc.pos.x, 0.f, npc.pos.y), 40.f, 50.f, getPersonTex(npc.color), billboardDayNight);
    }
    addBillboard(quads, viewProj, windowSize_, billboardRight,
        Vec3(playerPos_.x + kPlayerBoxSize / 2.f, 0.f, playerPos_.y + kPlayerBoxSize / 2.f), 40.f, 50.f,
        getPersonTex(sf::Color(255, 214, 90)), billboardDayNight);

    // Bloom pass: one additive glow billboard per point light (lamp heads +
    // each building's approximated window light), so a lit light actually
    // reads as a glowing source instead of only ever brightening whatever
    // surface it happens to shine on via litColor(). Skipped near-entirely
    // by day (intensity already carries lightBoost's day/night scaling) --
    // the `< 0.02f` cutoff just avoids pushing a fully-transparent quad
    // (and its draw-call/batch-flush cost) for every light all day long.
    for (const auto& light : lc.lights) {
        float strength = std::clamp(light.intensity / 38.f, 0.f, 1.f); // 38 ~= the un-boosted intensity both light sources above are scaled from (40/36)
        if (strength < 0.02f) continue;
        // 2026-08-10 lighting-strengthen pass: bloom alpha 150->195 and
        // glow radius 34+46*s -> 40+62*s -- every lamp/window bloom halo in
        // the game routes through this one loop, so this is the "all glows"
        // lever without re-tuning each hero building's own decal-flanking
        // glow calls (those were individually calibrated against specific
        // decals across many earlier bugfix rounds -- see this file's own
        // memory log -- and stay untouched here).
        sf::Color tint(
            clamp8_3d(static_cast<int>(light.color.x * 255.f)),
            clamp8_3d(static_cast<int>(light.color.y * 255.f)),
            clamp8_3d(static_cast<int>(light.color.z * 255.f)),
            clamp8_3d(static_cast<int>(195.f * strength)));
        float glowSize = 40.f + 62.f * strength;
        addGlowBillboard(quads, viewProj, windowSize_, billboardRight, light.pos, glowSize, glowTex, tint);
    }

    // Global back-to-front sort across EVERYTHING (opaque geometry and
    // billboards alike) -- this is what makes a billboard genuinely behind
    // a building actually get occluded instead of always drawing on top of
    // it (which a separate "draw all geometry, then all billboards after"
    // pass would do wrong).
    std::sort(quads.begin(), quads.end(), [](const ScreenQuad& a, const ScreenQuad& b) {
        return a.sortDepth > b.sortDepth;
    });

    // Flush in depth order, batching each contiguous run of quads that
    // share the same texture (nullptr counts as its own "texture") AND the
    // same blend mode into one draw call -- SFML can only bind one texture/
    // blend mode per window.draw, so a change in either is exactly where a
    // batch has to end. Glow billboards (additive) are the only quads that
    // ever differ here; everything else stays on the default alpha blend.
    sf::VertexArray va(sf::PrimitiveType::Triangles);
    const sf::Texture* currentTex = quads.empty() ? nullptr : quads.front().texture;
    bool currentAdditive = !quads.empty() && quads.front().additive;
    auto flush = [&]() {
        if (va.getVertexCount() == 0) return;
        sf::RenderStates states;
        states.texture = currentTex;
        states.blendMode = currentAdditive ? sf::BlendAdd : sf::BlendAlpha;
        window.draw(va, states);
        va.clear();
    };
    for (const auto& q : quads) {
        if (q.texture != currentTex || q.additive != currentAdditive) {
            flush();
            currentTex = q.texture;
            currentAdditive = q.additive;
        }
        sf::Vertex v0{ q.p[0], q.color, q.uv[0] }, v1{ q.p[1], q.color, q.uv[1] };
        sf::Vertex v2{ q.p[2], q.color, q.uv[2] }, v3{ q.p[3], q.color, q.uv[3] };
        va.append(v0); va.append(v1); va.append(v2);
        va.append(v0); va.append(v2); va.append(v3);
    }
    flush();

    // Floating name labels -- the 3D equivalent of drawBuilding's label
    // text, minus its icon-sign offset logic (no icon signs in 3D yet, see
    // this file's header comment). Plain screen-space sf::Text projected
    // through the same camera rather than billboarded geometry -- text
    // stays upright/readable this way with no extra work, same as how
    // draw3DBuildingHighlight already projects its ring's corners. Drawn
    // last (on top of everything, unsorted) same as every other floating UI
    // name tag in this game -- always legible, not depth-tested against the
    // world, which is the standard convention for this kind of tag.
    if (fontLoaded_) {
        for (const auto& b : z.buildings) {
            bool locked = game_.isBusinessLocked(b.id);
            ConstructionInfo ci = locked ? ConstructionInfo{} : game_.businessConstructionInfo(b.id);
            float labelY;
            if (ci.requiresConstruction) labelY = ci.inProgress ? (kBuildingHeight + kRoofRise + 20.f) : 34.f; // in-progress sites now have a real truss/wall (see addConstructionSiteProps) reaching wallH+roofRise -- clear of that instead of the old flat lot's low 34
            else if (!locked && (b.id == "mine" || b.id == "goldmine")) labelY = 74.f; // above the mound's own 62-high apex
            else if (!locked && (b.id == "farm" || b.id == "lumber" || b.id == "quarry" || b.id == "sheep" ||
                b.id == "orchard" || b.id == "herbgarden" || b.id == "vineyard")) labelY = 44.f; // flat-plot archetypes -- above their tallest prop (fruit trees at 34), not a roofline that doesn't exist here
            else if (!locked && b.id == "staff") labelY = kBuildingHeight * 1.15f + kRoofRise * 1.5f + 16.f; // taller hero building -- see addRecruitmentCenterBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "bank") labelY = kBuildingHeight * 1.05f + kRoofRise * 0.85f + 16.f; // see addBankBuilding's own wallH2/roofRise math
            else if (!locked && b.id == "sleep") labelY = kBuildingHeight * 1.12f + kRoofRise * 1.4f + 16.f; // see addInnBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "eat") labelY = kBuildingHeight * 1.02f + kRoofRise * 1.35f + 16.f; // see addKitchenBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "townhall") labelY = kBuildingHeight * 1.3f + kRoofRise * 1.3f + 20.f; // above the main gable's own peak (see addTownHallBuilding) -- not the much-taller clock tower/spire, which sits well off the footprint's horizontal center this label anchors to
            else if (!locked && b.id == "market") labelY = 74.f; // no wall/roof here at all -- above the canopy's own back-edge height (58, see addMarketBuilding) with clearance, same "flat-plot archetype" idea as the mine mound/farm rows above
            else if (!locked && b.id == "doctor") labelY = kBuildingHeight * 1.15f + 30.f; // parapet top (14, see addClinicBuilding's wallH2 math) + label clearance (16) -- this renderer's first flat-roof hero building, no gable-rise term needed
            else if (!locked && b.id == "warehouse") labelY = kBuildingHeight * 1.05f + kRoofRise + 20.f; // above the ridge (see addWarehouseBuilding's own wallH2/roofRise math), plus the small vent box sitting right at that height -- retuned down from 1.35x/1.2x after "太高了" feedback
            else if (!locked && b.id == "storefront") labelY = kBuildingHeight * 1.1f + kRoofRise * 1.4f + 16.f; // see addStorefrontBuilding's own wallH2/gableRise math -- same family as Staff Office's front-gable label height
            else if (!locked && b.id == "sawmill") labelY = kBuildingHeight * 1.05f + kRoofRise + 16.f; // see addSawmillBuilding's own wallH2/roofRise math -- plain side-gable ridge peak, same family as Warehouse's own label height
            else labelY = kBuildingHeight + kRoofRise + 14.f;
            Vec3 anchor(b.position.x + b.size.x * 0.5f, labelY, b.position.y + b.size.y * 0.5f);
            Projected p = projectPoint(viewProj, anchor, windowSize_);
            if (!p.valid) continue;

            std::string label = Localization::t(b.labelKey);
            if (b.id == "farm") label += " (" + Localization::t(game_.farmCropId()) + ")";
            sf::Text text(font_, toSfString3d(label), 13);
            sf::FloatRect bounds = text.getLocalBounds();
            text.setPosition(sf::Vector2f(p.screen.x - bounds.size.x / 2.f - bounds.position.x, p.screen.y - bounds.size.y - bounds.position.y));
            text.setFillColor(b.labelColor);
            text.setOutlineColor(sf::Color::Black);
            text.setOutlineThickness(2.f);
            text.setStyle(sf::Text::Bold);
            window.draw(text);

            // "N days left" -- brought back for the 3D construction site
            // (2026-08-07, "记得做回跟之前一样显示还有多久完工"), same
            // localized strings/wording drawConstructionSiteShape's 2D
            // version already uses, just as a second floating text line
            // below the name label instead of centered over the 2D site's
            // own progress bar (the 3D progress bar geometry itself is in
            // addConstructionSiteProps, this is only the text half of it).
            if (ci.requiresConstruction && ci.inProgress) {
                int daysLeft = static_cast<int>(std::ceil(ci.daysRemaining));
                std::string dayText = Localization::t("construction_site_days_left_prefix") + std::to_string(daysLeft) + Localization::t("construction_site_days_left_suffix");
                sf::Text dayLabel(font_, toSfString3d(dayText), 12);
                sf::FloatRect dayBounds = dayLabel.getLocalBounds();
                dayLabel.setPosition(sf::Vector2f(p.screen.x - dayBounds.size.x / 2.f - dayBounds.position.x, p.screen.y + 4.f));
                dayLabel.setFillColor(sf::Color(255, 240, 210));
                dayLabel.setOutlineColor(sf::Color::Black);
                dayLabel.setOutlineThickness(2.f);
                window.draw(dayLabel);
            }
        }
    }
}
