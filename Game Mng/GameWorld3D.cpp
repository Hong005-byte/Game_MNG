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
    // Used to desaturate+darken a locked business's wall/roof color toward
    // flat gray (the 3D equivalent of the 2D world's drawLockOverlay
    // dimming rectangle) -- removed 2026-08-12 once locked businesses
    // stopped rendering as a dimmed box+roof at all (see draw3DZone's
    // building loop and its own "一个色块方块在那边很丑" comment) in favor
    // of the same natural-clearing look unstarted-but-buildable plots use,
    // plus a padlock signpost.

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
        // 2026-08-12 ("现在每个门口前面都有一个圆形的光环了...路灯为自己
        // 一个的物体" -- every doorway now has a circular glow ring, only
        // street lamps should get one): true means the bloom pass (see
        // draw3DZone's own loop) also draws a visible glow billboard at
        // this light's position; false means it only contributes shading to
        // nearby surfaces (see litColor), same as before the bloom pass's
        // centering got fixed -- until that fix (see addGlowBillboard's own
        // comment) EVERY light's bloom quad rendered offset upward by half
        // its own size, which for the generic per-building "approximate a
        // lit window" light (see the building loop below) happened to land
        // it well clear of the door/ground area rather than centered right
        // on top of it -- so this bug had been quietly hiding a second,
        // separate problem: that light was never meant to be its own
        // visible glow at all, since every building already bakes its OWN
        // hand-placed window/chimney/hearth glow billboards into its
        // bespoke shape (addGlowBillboard, called throughout this file's
        // addXBuilding functions) -- the generic per-building light's whole
        // job is background fill-light shading for buildings whose bespoke
        // shape doesn't happen to place one there itself, not a second
        // visible light source layered on top of those. Only actual light
        // FIXTURES the player can see (street lamps) still bloom.
        bool bloom = true;
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
    // face BY DEFAULT -- every wall call site that needs one already has a
    // roof (or, for Clinic's flat-roofed blocks, a parapet deck) going on
    // top of it.
    //
    // `includeTop` (2026-08-11, "我看很多这种的图层都是拿泥土层来放成桌子
    // 勒" -- a lot of these layers are using the dirt layer as the table):
    // this function got reused, well after the "walls always get a roof"
    // assumption above was written, for plenty of free-standing furniture
    // that never gets one -- market/gemshop/etc counters, display tables,
    // fruit crates, oven pedestals, baskets. Every one of those was
    // rendering with NO top face at all, so looking down through the
    // missing lid showed whatever ground-level geometry sat behind it --
    // exactly the "counter reads as a flat dirt-colored patch" bug this
    // was reported against, not something specific to one building.
    // `includeTop` defaults to false so every existing wall-under-a-roof
    // call site is completely unaffected; every free-standing-furniture
    // call site now passes true.
    void addBandedBox(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, Vec3 size, sf::Color baseColor, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f, bool includeTop = false) {
        Vec3 p000 = pos;
        Vec3 p100 = pos + Vec3(size.x, 0.f, 0.f);
        Vec3 p010 = pos + Vec3(0.f, size.y, 0.f);
        Vec3 p001 = pos + Vec3(0.f, 0.f, size.z);
        Vec3 p110 = pos + Vec3(size.x, size.y, 0.f);
        Vec3 p101 = pos + Vec3(size.x, 0.f, size.z);
        Vec3 p011 = pos + Vec3(0.f, size.y, size.z);
        Vec3 p111 = pos + Vec3(size.x, size.y, size.z);

        if (includeTop) addFace(out, viewProj, windowSize, eye, p010, p110, p111, p011, Vec3(0.f, 1.f, 0.f), baseColor, lc, tex, uvWorldPerTile);
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
    // `tex`/`uvWorldPerTile` are optional (default nullptr = flat `color`
    // fill, the original behavior every pre-2026-08-11 call site still
    // gets) -- added so Bakery's own oven mound could pick up a stone
    // texture instead of reading as a solid-color shape (see
    // addBakeryBuilding's own oven rework comment below).
    void addPyramid(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        Vec3 pos, sf::Vector2f size, float height, sf::Color color, const LightingContext& lc,
        const sf::Texture* tex = nullptr, float uvWorldPerTile = 40.f) {
        Vec3 nw(pos.x, pos.y, pos.z), ne(pos.x + size.x, pos.y, pos.z);
        Vec3 se(pos.x + size.x, pos.y, pos.z + size.y), sw(pos.x, pos.y, pos.z + size.y);
        Vec3 apex(pos.x + size.x * 0.5f, pos.y + height, pos.z + size.y * 0.5f);
        addTri(out, viewProj, windowSize, eye, nw, ne, apex, Vec3(0.f, 0.6f, -0.8f).normalized(), color, lc, tex, uvWorldPerTile);
        addTri(out, viewProj, windowSize, eye, se, sw, apex, Vec3(0.f, 0.6f, 0.8f).normalized(), color, lc, tex, uvWorldPerTile);
        addTri(out, viewProj, windowSize, eye, ne, se, apex, Vec3(0.8f, 0.6f, 0.f).normalized(), color, lc, tex, uvWorldPerTile);
        addTri(out, viewProj, windowSize, eye, sw, nw, apex, Vec3(-0.8f, 0.6f, 0.f).normalized(), color, lc, tex, uvWorldPerTile);
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
    //
    // 2026-08-12 fix ("我觉得全部给我的感觉就是蒙蒙的,而且路灯的光出现在
    // 路灯的头上" -- everything looks hazy, and the lamp's light appears
    // above the lamp head): `pos` is meant to be the CENTER of the glow --
    // every call site passes something like a light's own position, a
    // window's center, a hearth's center, etc. But addBillboard's own `base`
    // param is a BOTTOM anchor (the quad extends upward by `height` from
    // it, see its own comment) -- passing `pos` straight through as `base`
    // put every glow's actual visual center `size/2` ABOVE `pos` instead of
    // ON it. Harmless for a small 12-20 unit prop glow (chimney smoke, a
    // hearth fire) where that offset is barely noticeable, but the bloom
    // pass's per-light glow (see draw3DZone's own loop, `glowSize` up to
    // 46+70=116) made it obvious: a big soft halo floating well above
    // whatever it was supposed to be marking, and with THAT many oversized,
    // wrongly-centered halos stacked across a whole lit-up night zone (every
    // window + every lamp), the general "everything looks hazy" read follows
    // directly from the same bug. Shifting `base` down by half the glow's
    // own size re-centers it on `pos` without touching any of the ~70 call
    // sites' own position math.
    void addGlowBillboard(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize,
        Vec3 billboardRight, Vec3 pos, float size, const sf::Texture& glowTex, sf::Color tint) {
        Vec3 base(pos.x, pos.y - size * 0.5f, pos.z);
        addBillboard(out, viewProj, windowSize, billboardRight, base, size, size, glowTex, tint, /*additive=*/true);
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

    // Shared "Workshop family" cabin -- the log-cabin shop block (stone
    // foundation, plank upper band, side-gable roof, door, window+flower-
    // box, gable-end sign, chimney) that Bakery/Preserve/Goldsmith each
    // hand-copied verbatim with only their own wall/roof/sign colors
    // differing (2026-08-11, after the user's "有的话就试看每一间你自己
    // 设计" -- go ahead and design the rest -- made it clear this recipe
    // was about to get copy-pasted several more times over). Factored out
    // now, for every NEW Workshop-family building from here on, rather
    // than retrofitting the 3 that already shipped and were already
    // confirmed against a screenshot (no reason to risk regressing
    // working, user-checked geometry for a pure refactor). Returns the
    // measurements (enclosedW/wallTop/foundationH/southZ) every caller
    // needs to keep placing its own annex/apparatus flush against the
    // cabin.
    struct WorkshopCabinInfo { float enclosedW, wallTop, foundationH, southZ; };
    WorkshopCabinInfo addWorkshopCabin(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        sf::Color plankWall, sf::Color roofColor, sf::Color signColor, float enclosedWFrac = 0.38f) {
        sf::Color stone(118, 114, 108);
        sf::Color beamColor(58, 40, 26);
        sf::Color windowColor(255, 214, 140);
        sf::Color plantBoxColor(96, 68, 40);
        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = basePos.z + b.size.y + 1.5f;

        float wallH2 = wallH * 0.72f;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;
        float cabinRoofRise = roofRise * 0.6f;

        float enclosedW = b.size.x * enclosedWFrac;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, cabinRoofRise, roofColor, lc, &shingleTex, kShingleUv);

        float doorW = enclosedW * 0.22f, doorH = foundationH + upperH * 0.55f;
        Vec3 doorPos(basePos.x + enclosedW * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        float winSize = enclosedW * 0.16f;
        Vec3 winPos(basePos.x + enclosedW * 0.16f, foundationH + upperH * 0.4f, southZ);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        float signW = enclosedW * 0.62f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + cabinRoofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

        Vec3 chimneyPos(basePos.x + enclosedW * 0.78f, wallTop * 0.3f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, wallTop * 0.75f, 11.f), sf::Color(96, 90, 86), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, wallTop * 0.75f + 12.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));

        return { enclosedW, wallTop, foundationH, southZ };
    }

    // Shared perimeter picket fence (all 4 sides, gate gap centered on the
    // south edge) -- Pasture's own `fenceRun` lambda, hand-copied into
    // Orchard and then Herb Garden (2026-08-11, each round adding another
    // near-identical copy). Factored out now for the same reason
    // `addWorkshopCabin` was -- every NEW flat-plot business from here on
    // uses this instead of a 4th/5th/6th copy; Pasture/Orchard/Herb Garden
    // keep their own already-shipped, already-screenshotted copies rather
    // than being retrofitted for a pure refactor.
    void addPerimeterFence(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, sf::Color fenceColor, float gateW = 22.f) {
        auto fenceRun = [&](float x0, float z0, float x1, float z1) {
            float dx = x1 - x0, dz = z1 - z0;
            float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-3f) return;
            int posts = std::max(2, static_cast<int>(len / 16.f) + 1);
            for (int i = 0; i < posts; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(posts - 1);
                addBox(out, viewProj, windowSize, eye, Vec3(x0 + dx * t, 0.f, z0 + dz * t), Vec3(3.f, 16.f, 3.f), fenceColor, lc);
            }
            if (std::fabs(dz) < 1e-3f) addBox(out, viewProj, windowSize, eye, Vec3(x0, 9.f, z0 - 1.5f), Vec3(dx, 3.f, 3.f), fenceColor, lc);
            else addBox(out, viewProj, windowSize, eye, Vec3(x0 - 1.5f, 9.f, z0), Vec3(3.f, 3.f, dz), fenceColor, lc);
        };
        float fx0 = b.position.x + 4.f, fx1 = b.position.x + b.size.x - 4.f;
        float fz0 = b.position.y + 4.f, fz1 = b.position.y + b.size.y - 4.f;
        float gateMid = b.position.x + b.size.x * 0.5f;
        fenceRun(fx0, fz1, gateMid - gateW * 0.5f, fz1);
        fenceRun(gateMid + gateW * 0.5f, fz1, fx1, fz1);
        fenceRun(fx0, fz0, fx1, fz0);
        fenceRun(fx0, fz0, fx0, fz1);
        fenceRun(fx1, fz0, fx1, fz1);
    }

    // Shared open-air market-stall shell -- ground fill + border, 4 corner
    // posts, a striped canopy (`addStripedAwning`, the same helper Market's
    // own hero building already established), and a counter -- for the
    // Stall family (isStallId in GameWorld.cpp: popcornstand/juicebar/
    // teahouse/giftbasket/sushibar). Mirrors the 2D world's own
    // drawStallShape ("a canvas awning over a counter instead of an
    // enclosed shed... reads as buy here rather than goods are made here")
    // -- these stay flat-plot-plus-props like Farm/Market, no walls at all,
    // unlike every Workshop-family building. One stall filling most of the
    // lot (Market's OWN hero building fits 3 side by side because it
    // represents the whole market, not a single vendor). Returns the
    // counter's own box so each caller can place its own goods on top.
    struct MarketStallInfo { Vec3 counterPos; float counterW, counterD; };
    MarketStallInfo addMarketStallShell(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, sf::Color stripeA, sf::Color stripeB) {
        sf::Color groundColor(176, 158, 122), postColor(74, 52, 32), counterColor(120, 84, 46);
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, groundColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        float postT = 5.f, backH = 58.f, frontH = 42.f;
        float backMargin = b.size.y * 0.12f, awningDepth = b.size.y * 0.5f;
        float counterGap = b.size.y * 0.03f, counterDepth = b.size.y * 0.22f;
        float x0 = b.position.x + b.size.x * 0.08f, stallW = b.size.x * 0.84f;
        float zBack = b.position.y + backMargin, zFront = zBack + awningDepth;

        for (float px : { x0, x0 + stallW - postT }) {
            addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, zBack), Vec3(postT, backH, postT), postColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, zFront - postT), Vec3(postT, frontH, postT), postColor, lc);
        }
        addStripedAwning(out, viewProj, windowSize, eye, Vec3(x0, 0.f, zBack), stallW, awningDepth, backH, frontH, stripeA, stripeB, 6, lc);

        float counterZ0 = zFront + counterGap;
        Vec3 counterPos(x0 + stallW * 0.06f, 0.f, counterZ0);
        float counterW = stallW * 0.88f;
        addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(counterW, 18.f, counterDepth), counterColor, lc, nullptr, 40.f, true);
        return { counterPos, counterW, counterDepth };
    }

    // Shared dock shell for the Dock family (isDockId in GameWorld.cpp:
    // fishing/shipyard/cannery/port/deepsea/island_ferry/fishermanplatter --
    // spans every tier, unlike the other 8 archetype families, since even
    // the 2D world's own drawDockShape gives tier-1 Fishing Dock the same
    // deck+water base every other Dock business gets). Mirrors that 2D
    // shape directly: a planked wood deck over most of the lot, a strip of
    // water along the south edge with a low rail, no walls/roof at all --
    // flat-plot-plus-props family, same as Market/the Stall shell above.
    struct DockShellInfo { float deckD, waterZ0; };
    DockShellInfo addDockShell(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc) {
        sf::Color deckColor(126, 92, 56), waterColor(60, 110, 150), plankColor(100, 72, 42), railColor(90, 64, 38);
        float deckD = b.size.y * 0.72f;
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, deckD, 0.6f, deckColor, lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        for (float x = b.position.x + 8.f; x < b.position.x + b.size.x - 4.f; x += 14.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(x, 0.f, b.position.y + 3.f), Vec3(2.f, 1.5f, deckD - 6.f), plankColor, lc);
        }
        float waterZ0 = b.position.y + deckD;
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, waterZ0, b.size.x, b.size.y - deckD, 0.5f, waterColor, lc);
        for (float x = b.position.x + 6.f; x < b.position.x + b.size.x - 4.f; x += 20.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(x, 0.f, waterZ0 - 2.f), Vec3(3.f, 10.f, 3.f), railColor, lc);
        }
        return { deckD, waterZ0 };
    }

    // Shared shell for the Oven family's remaining 7 businesses (jamkitchen/
    // pieshop/roaststand/picklinghouse/honeyrefinery/cakeshop/artisanbakery
    // -- Bakery/Preserve already covered the first 2 with their own bespoke
    // builds). `addWorkshopCabin` + an open post-and-roof bay (the Winery/
    // Alchemist lesson applied up front: no enclosed wall hiding what's
    // inside) holding a simple stone hearth with a fire glow -- every Oven-
    // family business shares that same "something's cooking" read (see the
    // 2D world's own drawOvenShape comment: "an arched, glowing oven mouth
    // out front is the one visual every one of these businesses has in
    // common"). Returns the cabin info AND the bay's own bounds so each
    // caller can place its own 1-2 signature props (a cake stand, a pickle
    // rack, ...) without every business re-deriving that math.
    struct OvenShellInfo { WorkshopCabinInfo cab; float bayX0, bayW, bayH; };
    OvenShellInfo addOvenFamilyShell(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        sf::Color plankWall, sf::Color roofColor, sf::Color signColor, sf::Color fireGlow) {
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.68f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        sf::Color hearthStone = shade3d(sf::Color(118, 114, 108), -6);
        Vec3 hearthPos(bayX0 + bayW * 0.32f, 0.f, basePos.z + b.size.y * 0.5f - 9.f);
        addBandedBox(out, viewProj, windowSize, eye, hearthPos, Vec3(18.f, 7.f, 18.f), hearthStone, lc, &stoneTex, 20.f, true);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, hearthPos + Vec3(9.f, 10.f, 9.f), 18.f, glowTex, fireGlow);

        return { cab, bayX0, bayW, bayH };
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
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(6.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
            addBillboard(out, viewProj, windowSize, billboardRight, boxPos + Vec3(3.f, 6.f, 3.f), 12.f, 14.f, flowerTex, sf::Color::White);
        }
    }

    // Shared rocky-mound mouth, now used by BOTH Mine and Gold Mine
    // (2026-08-11 follow-up, "那个矿场不好看,我觉得没有那个金矿好看" --
    // regular Mine doesn't look as good as Gold Mine): this used to be 2
    // separate shapes -- Mine kept a single flat 4-sided pyramid (never
    // flagged UNTIL now), Gold Mine got its own 3-peak cluster + framed
    // sign a couple of rounds back. Since the user now wants Mine brought
    // up to the same standard, this factors the cluster+entrance geometry
    // out into one shared function parameterized by rock/sign color, and
    // BOTH addMineProps and Gold Mine's own dispatch call it -- no more
    // duplicated mound code, and any future mound fix (like the "matched
    // the pyramid to the door" and "unburied the door" rounds already
    // logged below) only needs to happen once.
    void addRockyMound(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex,
        sf::Color rockColor, sf::Color signColor) {
        // Entrance dimensions computed up front (2026-08-11, "矿门不见了"
        // -- the door disappeared, from Gold Mine's own earlier round):
        // each peak's own south base is capped at the arch's BACK face
        // (`b.size.y - archD`), so the mound stops exactly where the
        // doorway begins -- flush with it, but standing clear in front
        // instead of buried inside the mound's own opaque rock.
        float archW = b.size.x * 0.20f, archD = b.size.y * 0.22f, archHeight = 24.f;
        float moundMaxDepth = b.size.y - archD;
        auto peak = [&](float xOff, float widthFrac, float zNorthOff, float height, sf::Color col) {
            addPyramid(out, viewProj, windowSize, eye, Vec3(b.position.x + xOff, 0.f, b.position.y + zNorthOff),
                sf::Vector2f(b.size.x * widthFrac, moundMaxDepth - zNorthOff), height, col, lc);
        };
        peak(0.f, 0.9f, 0.f, 128.f, rockColor);                                    // main mass, full depth
        peak(-8.f, 0.5f, moundMaxDepth * 0.10f, 88.f, shade3d(rockColor, -14));     // 2nd, shorter peak, west
        peak(b.size.x * 0.58f, 0.4f, moundMaxDepth * 0.12f, 70.f, shade3d(rockColor, 10)); // 3rd, smaller peak, east

        // ---- Entrance, forward of the mound cluster's own south face --
        // dark arch with a real crossbeam over the 2 posts and a
        // signboard mounted above it. ----
        Vec3 archPos(b.position.x + b.size.x * 0.5f - archW * 0.5f, 0.f, b.position.y + b.size.y - archD);
        addBox(out, viewProj, windowSize, eye, archPos, Vec3(archW, archHeight, archD), sf::Color(18, 18, 20), lc);
        sf::Color beamColor(94, 62, 32);
        addBox(out, viewProj, windowSize, eye, archPos - Vec3(5.f, 0.f, 0.f), Vec3(5.f, archHeight + 6.f, archD), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, archPos + Vec3(archW, 0.f, 0.f), Vec3(5.f, archHeight + 6.f, archD), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(archPos.x - 5.f, archHeight + 6.f, archPos.z), Vec3(archW + 10.f, 4.f, archD), shade3d(beamColor, -10), lc);

        float signW = archW + 20.f, signH = 14.f;
        Vec3 signPos(archPos.x + archW * 0.5f - signW * 0.5f, archHeight + 12.f, archPos.z + archD - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archPos.x - 9.f, archHeight * 0.6f, archPos.z + archD * 0.5f), 12.f, glowTex, sf::Color(255, 200, 120, 150));
    }

    // Mine's own mound -- now routes through the shared addRockyMound
    // above instead of its own flat single pyramid (see that function's
    // own header comment for why).
    void addMineProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex, sf::Color rockColor) {
        addRockyMound(out, viewProj, windowSize, eye, b, lc, billboardRight, glowTex, rockColor, shade3d(rockColor, -20));
    }

    // Gold Mine's own mound -- same addRockyMound, gold rock + a
    // gold-toned sign instead of Mine's grey/stone tones.
    void addGoldMineMound(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex) {
        addRockyMound(out, viewProj, windowSize, eye, b, lc, billboardRight, glowTex, sf::Color(150, 130, 80), sf::Color(150, 120, 50));
    }

    // 2026-08-11 rework ("现在到金矿场" -- against the same style of
    // reference image Orchard/Preserve's own rounds used, showing a full
    // mine-cart operation): stays a raw-tier flat-plot archetype like
    // Mine/Orchard/etc (the reference's own smelting cauldrons/jars belong
    // to Goldsmith, Gold Mine's tier-2 processor sibling, a separate
    // build, same "raw producer vs. its processor" split Orchard/Preserve
    // just established) -- just the mound's own yard got richer instead
    // of a new walled building. Everything new here sits SOUTH of the
    // footprint's own edge (the mound itself, from addGoldMineMound above,
    // covers the FULL lot -- there's no flat ground inside the footprint
    // to put props on without them clipping into the sloped rock, so this
    // reuses the same "props spill past the lot's south edge" convention
    // Bakery/Orchard already established, anchored right at the entrance
    // archway's own mouth).
    void addGoldMineProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex, const sf::Texture& goldOreTex) {
        addGoldMineMound(out, viewProj, windowSize, eye, b, lc, billboardRight, glowTex);

        float southZ = b.position.y + b.size.y + 1.5f;
        float midX = b.position.x + b.size.x * 0.5f;

        // ---- Mine-cart rail track running out of the entrance, into the
        // yard -- 2 parallel rails + cross-ties every 8 units, the same
        // "posts every N units" spacing idea Pasture/Orchard's own fences
        // use, just laid flat. ----
        float railGap = 10.f, trackLen = 46.f;
        sf::Color railColor(70, 66, 62), tieColor(90, 62, 34);
        addBox(out, viewProj, windowSize, eye, Vec3(midX - railGap * 0.5f - 1.f, 0.f, southZ), Vec3(2.f, 1.5f, trackLen), railColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(midX + railGap * 0.5f - 1.f, 0.f, southZ), Vec3(2.f, 1.5f, trackLen), railColor, lc);
        for (float t = 0.f; t <= trackLen; t += 8.f) {
            addBox(out, viewProj, windowSize, eye, Vec3(midX - railGap * 0.5f - 4.f, 0.f, southZ + t), Vec3(railGap + 8.f, 1.f, 3.f), tieColor, lc);
        }

        // An ore cart sitting on the rails, loaded with gold nuggets
        // (`goldOreTex`, the same "outlined circle + shine" fake-volume
        // fruit this file already uses, just tinted gold).
        sf::Color cartColor(90, 78, 70), wheelColor(40, 38, 36);
        Vec3 cartPos(midX - 9.f, 0.f, southZ + trackLen - 22.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(18.f, 9.f, 14.f), cartColor, lc);
        for (float wx : { 2.f, 13.f }) {
            addBox(out, viewProj, windowSize, eye, cartPos + Vec3(wx, -3.f, -1.f), Vec3(3.f, 3.f, 3.f), wheelColor, lc);
            addBox(out, viewProj, windowSize, eye, cartPos + Vec3(wx, -3.f, 12.f), Vec3(3.f, 3.f, 3.f), wheelColor, lc);
        }
        addBillboard(out, viewProj, windowSize, billboardRight, cartPos + Vec3(9.f, 9.f, 7.f), 20.f, 14.f, goldOreTex, sf::Color::White);

        // A small ore pile, west of the track -- a few stacked rock chunks
        // topped with visible nuggets.
        sf::Color rockColor(120, 112, 100);
        Vec3 pilePos(b.position.x + 8.f, 0.f, southZ + 6.f);
        for (const auto& off : { sf::Vector2f(0.f, 0.f), sf::Vector2f(8.f, 3.f), sf::Vector2f(3.f, 7.f) }) {
            addBox(out, viewProj, windowSize, eye, pilePos + Vec3(off.x, 0.f, off.y), Vec3(9.f, 7.f, 9.f), rockColor, lc);
        }
        addBillboard(out, viewProj, windowSize, billboardRight, pilePos + Vec3(6.f, 9.f, 5.f), 18.f, 12.f, goldOreTex, sf::Color::White);

        // A wooden A-frame winch, east of the track, for hauling ore up
        // out of the shaft.
        sf::Color beamColor(94, 62, 32);
        float winchX = b.position.x + b.size.x - 20.f, winchZ = southZ + 4.f;
        for (float lean : { -1.f, 1.f }) {
            addBox(out, viewProj, windowSize, eye, Vec3(winchX + lean * 7.f, 0.f, winchZ), Vec3(4.f, 30.f, 4.f), beamColor, lc);
        }
        addBox(out, viewProj, windowSize, eye, Vec3(winchX - 7.f, 27.f, winchZ), Vec3(14.f, 3.f, 4.f), shade3d(beamColor, -10), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winchX, 16.f, winchZ + 2.f), 10.f, glowTex, sf::Color(255, 200, 120, 120));

        const sf::Vector2f sparkles[] = { { 0.28f, 0.55f }, { 0.68f, 0.45f }, { 0.5f, 0.3f } };
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

    // v2 (2026-08-11, from the user's own sixteenth reference image -- a
    // working sheep ranch: a walled barn, dense flock, a water trough with
    // a pig and chicken nearby, hay bales, a wool-loaded wheelbarrow, a
    // sheepdog, lanterns, picket fencing, and crop beds/gravestones that
    // read as borrowed from the same reference-image family as Farm's own
    // rather than sheep-ranch-specific). Scope-checked with the user first
    // (via AskUserQuestion, same caution as Quarry/Farm's own rounds) --
    // `sheep` (Sheep Farm, wool, tier 1) has a real tier-2 sibling
    // (`textile`/Textile Mill, wool->cloth) the reference's own barn could
    // have meant instead, same pairing shape as lumber/sawmill and
    // quarry/mason. User confirmed: `sheep` itself, still no building --
    // stays in the flat-plot family alongside Farm/Lumber/Quarry, just far
    // richer than the original 3-sheep-and-a-rail placeholder.
    void addPastureProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& sheepTex, const sf::Texture& chickenTex, const sf::Texture& pigTex, const sf::Texture& dogTex,
        sf::Color dayNightTint) {
        sf::Color fenceColor(150, 118, 76);
        sf::Color beamColor(58, 40, 26);
        sf::Color troughColor(110, 108, 104);
        sf::Color waterColor(120, 168, 196);
        sf::Color hayColor(198, 168, 78);
        sf::Color hayCapColor(220, 196, 108);
        sf::Color cartColor(118, 86, 52);
        sf::Color wheelColor(60, 44, 28);
        sf::Color woolColor(240, 238, 230);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(102, 140, 70), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // ---- Perimeter picket fence, all 4 sides, with a gate gap centered
        // on the south (front/camera-facing) edge -- the original version
        // only ever fenced the south edge; this reads as a genuinely
        // enclosed pen like the reference, not a single rail. A local
        // `fenceRun` lambda (posts every ~16 units + one connecting rail
        // per straight run) mirrors the exact technique Inn's own picket
        // fence already established. ----
        auto fenceRun = [&](float x0, float z0, float x1, float z1) {
            float dx = x1 - x0, dz = z1 - z0;
            float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-3f) return;
            int posts = std::max(2, static_cast<int>(len / 16.f) + 1);
            for (int i = 0; i < posts; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(posts - 1);
                addBox(out, viewProj, windowSize, eye, Vec3(x0 + dx * t, 0.f, z0 + dz * t), Vec3(3.f, 16.f, 3.f), fenceColor, lc);
            }
            if (std::fabs(dz) < 1e-3f) addBox(out, viewProj, windowSize, eye, Vec3(x0, 9.f, z0 - 1.5f), Vec3(dx, 3.f, 3.f), fenceColor, lc);
            else addBox(out, viewProj, windowSize, eye, Vec3(x0 - 1.5f, 9.f, z0), Vec3(3.f, 3.f, dz), fenceColor, lc);
        };
        float fx0 = b.position.x + 4.f, fx1 = b.position.x + b.size.x - 4.f;
        float fz0 = b.position.y + 4.f, fz1 = b.position.y + b.size.y - 4.f;
        float gateW = 22.f, gateMid = b.position.x + b.size.x * 0.5f;
        fenceRun(fx0, fz1, gateMid - gateW * 0.5f, fz1); // south, west half (up to the gate)
        fenceRun(gateMid + gateW * 0.5f, fz1, fx1, fz1);  // south, east half
        fenceRun(fx0, fz0, fx1, fz0);  // north
        fenceRun(fx0, fz0, fx0, fz1);  // west
        fenceRun(fx1, fz0, fx1, fz1);  // east

        // Water trough (west side) -- same box+blue-cap trough Farm's own
        // yard already established, with a pig and chicken nearby.
        Vec3 troughPos(b.position.x + 8.f, 0.f, b.position.y + 30.f);
        addBox(out, viewProj, windowSize, eye, troughPos, Vec3(22.f, 8.f, 10.f), troughColor, lc);
        addBox(out, viewProj, windowSize, eye, troughPos + Vec3(2.f, 5.f, 2.f), Vec3(18.f, 2.5f, 6.f), waterColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(b.position.x + 10.f, 0.f, b.position.y + 44.f), 22.f, 18.f, pigTex, dayNightTint);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(b.position.x + 26.f, 0.f, b.position.y + 42.f), 16.f, 16.f, chickenTex, dayNightTint);

        // Hay bales (east side), 2x2 grid -- Farm's own bale-stack technique.
        Vec3 hayBase(b.position.x + 80.f, 0.f, b.position.y + 10.f);
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                Vec3 hp = hayBase + Vec3(static_cast<float>(i) * 11.f, 0.f, static_cast<float>(j) * 11.f);
                addBox(out, viewProj, windowSize, eye, hp, Vec3(9.f, 8.f, 9.f), hayColor, lc);
                addBox(out, viewProj, windowSize, eye, hp + Vec3(1.f, 8.f, 1.f), Vec3(7.f, 1.5f, 7.f), hayCapColor, lc);
            }
        }

        // Wool-loaded wheelbarrow, just inside the south gate -- same crude
        // cart-body-plus-wheel convention every barrow/cart in this file
        // already uses, topped with a few small rounded white "wool"
        // clumps instead of the produce decal Farm's own cart used.
        Vec3 cartPos(gateMid - 8.f, 0.f, b.position.y + b.size.y - 20.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc);
        for (const auto& wp : { sf::Vector2f(2.f, 2.f), sf::Vector2f(8.f, 3.f), sf::Vector2f(5.f, 6.f) }) {
            addBox(out, viewProj, windowSize, eye, cartPos + Vec3(wp.x, 8.f, wp.y), Vec3(5.f, 4.f, 5.f), woolColor, lc);
        }

        // 2 lantern posts flanking the gate.
        for (float lx : { -22.f, 22.f }) {
            Vec3 lanternPos(gateMid + lx, 0.f, fz1 - 6.f);
            addBox(out, viewProj, windowSize, eye, lanternPos, Vec3(3.f, 24.f, 3.f), beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos + Vec3(1.5f, 28.f, 1.5f), 16.f, glowTex, sf::Color(255, 200, 120, 160));
        }

        // The sheepdog, near the wheelbarrow (watching the flock).
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(cartPos.x + 20.f, 0.f, cartPos.z + 4.f), 20.f, 16.f, dogTex, dayNightTint);

        // A denser flock -- 7 sheep scattered across the interior, clear of
        // the trough/hay/cart/dog footprints above.
        const sf::Vector2f puffs[] = {
            { 0.22f, 0.20f }, { 0.42f, 0.15f }, { 0.60f, 0.22f }, { 0.78f, 0.42f },
            { 0.30f, 0.55f }, { 0.55f, 0.60f }, { 0.68f, 0.68f },
        };
        for (const auto& pf : puffs) {
            Vec3 p(b.position.x + b.size.x * pf.x, 0.f, b.position.y + b.size.y * pf.y);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 26.f, 22.f, sheepTex, dayNightTint);
        }
    }

    // 2026-08-11 rework ("现在到果园的模型...只升级果树本身" -- keep
    // Orchard a flat plot, just upgrade the trees themselves), against a
    // reference image of a much fuller orchard homestead. Scoped down to
    // 3 things instead of the reference's full shop+stalls+press scene
    // (Orchard stays a raw-tier flat plot like Farm/Lumber/Quarry/Pasture
    // -- the reference's own shop/stalls belong to Preserve, Orchard's
    // tier-2 sibling, a separate build): (a) Apple and Pear as 2 distinct
    // tree sprites instead of one shared red blob, each with a real
    // layered canopy (shadow/highlight split, same "fake volume" trick
    // veggieTex/cabbageTex above already use) and actual visible fruit
    // dots rather than a single flat color, (b) a proper perimeter picket
    // fence with a south gate gap -- Pasture's own `fenceRun` technique
    // reused outright, since a raw-tier plot fenced only by a flat border
    // line was the odd one out once Pasture had a real fence, and (c) 2
    // fruit crates by the gate, the same "small yard prop" convention
    // Bakery's flour sack / Pasture's hay bales already use.
    void addOrchardProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight,
        const sf::Texture& appleTreeTex, const sf::Texture& pearTreeTex, const sf::Texture& fruitCrateTex, sf::Color dayNightTint) {
        sf::Color fenceColor(150, 118, 76);
        sf::Color crateColor(150, 108, 62);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(90, 130, 66), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        // ---- Perimeter picket fence, all 4 sides, gate gap centered on
        // the south edge -- verbatim copy of addPastureProps's own
        // `fenceRun` lambda above. ----
        auto fenceRun = [&](float x0, float z0, float x1, float z1) {
            float dx = x1 - x0, dz = z1 - z0;
            float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-3f) return;
            int posts = std::max(2, static_cast<int>(len / 16.f) + 1);
            for (int i = 0; i < posts; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(posts - 1);
                addBox(out, viewProj, windowSize, eye, Vec3(x0 + dx * t, 0.f, z0 + dz * t), Vec3(3.f, 16.f, 3.f), fenceColor, lc);
            }
            if (std::fabs(dz) < 1e-3f) addBox(out, viewProj, windowSize, eye, Vec3(x0, 9.f, z0 - 1.5f), Vec3(dx, 3.f, 3.f), fenceColor, lc);
            else addBox(out, viewProj, windowSize, eye, Vec3(x0 - 1.5f, 9.f, z0), Vec3(3.f, 3.f, dz), fenceColor, lc);
        };
        float fx0 = b.position.x + 4.f, fx1 = b.position.x + b.size.x - 4.f;
        float fz0 = b.position.y + 4.f, fz1 = b.position.y + b.size.y - 4.f;
        float gateW = 22.f, gateMid = b.position.x + b.size.x * 0.5f;
        fenceRun(fx0, fz1, gateMid - gateW * 0.5f, fz1); // south, west half (up to the gate)
        fenceRun(gateMid + gateW * 0.5f, fz1, fx1, fz1);  // south, east half
        fenceRun(fx0, fz0, fx1, fz0);  // north
        fenceRun(fx0, fz0, fx0, fz1);  // west
        fenceRun(fx1, fz0, fx1, fz1);  // east

        // ---- 2x3 grid of trees, alternating Apple/Pear by column for the
        // "mixed orchard" read the reference image itself has -- same grid
        // math as before, just 2 textures instead of 1. ----
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                Vec3 p(b.position.x + b.size.x * (0.2f + 0.3f * static_cast<float>(col)), 0.f,
                    b.position.y + b.size.y * (0.35f + 0.4f * static_cast<float>(row)));
                const sf::Texture& tex = (col % 2 == 0) ? appleTreeTex : pearTreeTex;
                addBillboard(out, viewProj, windowSize, billboardRight, p, 36.f, 46.f, tex, dayNightTint);
            }
        }

        // ---- 2 fruit crates flanking the gate, just outside the fence
        // (south of fz1, same "props spill past the lot's own edge into
        // the shared yard" convention Bakery/Sawmill already use). ----
        for (float cx : { -20.f, 14.f }) {
            Vec3 cratePos(gateMid + cx, 0.f, fz1 + 6.f);
            addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(14.f, 10.f, 14.f), crateColor, lc, nullptr, 40.f, true);
            addBillboard(out, viewProj, windowSize, billboardRight, cratePos + Vec3(7.f, 10.f, 7.f), 14.f, 10.f, fruitCrateTex, sf::Color::White);
        }
    }

    // 2026-08-11 rework ("那个药草园可以细节一点吗" -- can the Herb Garden
    // get more detail): the old version was 8 flat herbTuftTex billboards
    // (all the same green, all the same size) floating directly on bare
    // dirt with nothing marking the plot but the flat border line --
    // barely different from an empty lot. Now: a real perimeter picket
    // fence with a south gate gap (same `fenceRun` technique Pasture/
    // Orchard already established), each herb spot gets its own small
    // raised soil bed (instead of floating on flat ground) with 2
    // billboards per bed for fuller foliage, cycling through 3 tint colors
    // on the SAME `herbTuftTex` (green/lavender/gold -- the same multiply-
    // tint reuse trick bottleRackTex's own comment describes) so the beds
    // read as different herb varieties instead of one repeated tuft, and a
    // watering can prop near the gate.
    void addHerbGardenProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& herbTuftTex, sf::Color dayNightTint) {
        sf::Color fenceColor(150, 118, 76);
        sf::Color soilColor(90, 66, 44);

        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(74, 58, 40), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        auto fenceRun = [&](float x0, float z0, float x1, float z1) {
            float dx = x1 - x0, dz = z1 - z0;
            float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-3f) return;
            int posts = std::max(2, static_cast<int>(len / 16.f) + 1);
            for (int i = 0; i < posts; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(posts - 1);
                addBox(out, viewProj, windowSize, eye, Vec3(x0 + dx * t, 0.f, z0 + dz * t), Vec3(3.f, 16.f, 3.f), fenceColor, lc);
            }
            if (std::fabs(dz) < 1e-3f) addBox(out, viewProj, windowSize, eye, Vec3(x0, 9.f, z0 - 1.5f), Vec3(dx, 3.f, 3.f), fenceColor, lc);
            else addBox(out, viewProj, windowSize, eye, Vec3(x0 - 1.5f, 9.f, z0), Vec3(3.f, 3.f, dz), fenceColor, lc);
        };
        float fx0 = b.position.x + 4.f, fx1 = b.position.x + b.size.x - 4.f;
        float fz0 = b.position.y + 4.f, fz1 = b.position.y + b.size.y - 4.f;
        float gateW = 20.f, gateMid = b.position.x + b.size.x * 0.5f;
        fenceRun(fx0, fz1, gateMid - gateW * 0.5f, fz1);
        fenceRun(gateMid + gateW * 0.5f, fz1, fx1, fz1);
        fenceRun(fx0, fz0, fx1, fz0);
        fenceRun(fx0, fz0, fx0, fz1);
        fenceRun(fx1, fz0, fx1, fz1);

        struct Bed { sf::Vector2f off; sf::Color tint; };
        const Bed beds[] = {
            { {0.15f, 0.25f}, sf::Color(110, 160, 80) },
            { {0.35f, 0.55f}, sf::Color(150, 110, 190) },
            { {0.55f, 0.30f}, sf::Color(210, 180, 70) },
            { {0.75f, 0.60f}, sf::Color(110, 160, 80) },
            { {0.25f, 0.75f}, sf::Color(150, 110, 190) },
            { {0.65f, 0.20f}, sf::Color(210, 180, 70) },
            { {0.85f, 0.40f}, sf::Color(110, 160, 80) },
            { {0.45f, 0.80f}, sf::Color(150, 110, 190) },
        };
        for (const auto& bed : beds) {
            Vec3 p(b.position.x + b.size.x * bed.off.x, 0.f, b.position.y + b.size.y * bed.off.y);
            addBandedBox(out, viewProj, windowSize, eye, p - Vec3(9.f, 0.f, 9.f), Vec3(18.f, 3.f, 18.f), soilColor, lc, nullptr, 40.f, true);
            addBillboard(out, viewProj, windowSize, billboardRight, p + Vec3(-4.f, 3.f, 0.f), 16.f, 16.f, herbTuftTex, bed.tint);
            addBillboard(out, viewProj, windowSize, billboardRight, p + Vec3(4.f, 3.f, 3.f), 14.f, 14.f, herbTuftTex, shade3d(bed.tint, -10));
        }

        // A watering can, just inside the gate.
        sf::Color canColor(96, 98, 102);
        Vec3 canPos(gateMid - 4.f, 0.f, fz1 - 10.f);
        addBox(out, viewProj, windowSize, eye, canPos, Vec3(8.f, 8.f, 8.f), canColor, lc);
        addBox(out, viewProj, windowSize, eye, canPos + Vec3(6.f, 3.f, 2.f), Vec3(6.f, 2.f, 2.f), canColor, lc);
    }

    // 2026-08-11 rework ("那个葡萄有点少,可以做那种栏杆然后有葡萄挂下来
    // 的样子吗" -- too few grapes, make it a railing with grapes hanging
    // off it): the old version's own "trellis" was a single 4-wide, 24-
    // tall solid post-wall per row (read as a thin fence, not a trellis)
    // with just 3 tiny (10-unit) grape dots floating at its mid-height --
    // easy to miss. Now each row gets real trellis structure -- 3 posts
    // (2 ends + 1 middle) holding up a horizontal top rail, a leafy vine
    // band hanging just under that rail, and 5 (was 3) bigger (14, was
    // 10) grape clusters hanging below the leaves -- reads as an actual
    // arbor with fruit hanging off it instead of a fence with sparse dots.
    void addVineyardProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& grapeTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(107, 84, 48), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        constexpr int rows = 4;
        float gap = b.size.x / static_cast<float>(rows);
        sf::Color postColor(112, 86, 52), railColor(126, 96, 58), leafColor(76, 108, 58);
        for (int i = 0; i < rows; ++i) {
            float x = b.position.x + gap * static_cast<float>(i) + gap * 0.5f;
            float z0 = b.position.y + 5.f, z1 = b.position.y + b.size.y - 5.f;
            float railH = 28.f;
            for (float pz : { z0, (z0 + z1) * 0.5f - 2.f, z1 - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(x - 2.f, 0.f, pz), Vec3(4.f, railH, 4.f), postColor, lc);
            }
            addBox(out, viewProj, windowSize, eye, Vec3(x - 2.f, railH, z0), Vec3(4.f, 3.f, z1 - z0), railColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(x - 3.f, railH - 9.f, z0), Vec3(6.f, 9.f, z1 - z0), leafColor, lc);
            for (float t = 0.12f; t < 0.98f; t += 0.18f) {
                Vec3 p(x, railH - 15.f, z0 + (z1 - z0) * t);
                addBillboard(out, viewProj, windowSize, billboardRight, p, 14.f, 14.f, grapeTex, dayNightTint);
            }
        }
    }

    // ---- Zone 5 (Highlands District) Field-family batch, 2026-08-11
    // ("剩下的屋子一样可以开始进行了" -- go ahead and start on the rest):
    // the 5 remaining raw-tier businesses in isFieldId (GameWorld.cpp) --
    // Dairy Farm/Beehive/Trapper/Tea Field/Flax Field. Until now these
    // fell all the way through to the plain box+gable-roof fallback (no
    // tier-1 dispatch entry existed for them at all, unlike Farm/Orchard/
    // etc) -- the worst offenders of the "still generic" list, since a
    // raw-tier business rendering as a walled building was exactly the
    // inconsistency Bakery's own very first round was created to avoid
    // for tier-2s. All 5 are flat-plot archetypes (ground fill + border +
    // `addPerimeterFence` + themed props), same family as every other
    // tier-1 producer. ----

    // Dairy Farm -- fenced pasture, a water trough, hay, and grazing cows
    // (`cowTex`) -- same overall shape Pasture's own sheep pen uses, cows
    // instead of sheep/chickens/pigs since a dairy farm's only output is
    // milk.
    void addDairyFarmProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& cowTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(102, 140, 70), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        addPerimeterFence(out, viewProj, windowSize, eye, b, lc, sf::Color(150, 118, 76));

        sf::Color troughColor(110, 108, 104), waterColor(120, 168, 196);
        Vec3 troughPos(b.position.x + 8.f, 0.f, b.position.y + 12.f);
        addBox(out, viewProj, windowSize, eye, troughPos, Vec3(22.f, 8.f, 10.f), troughColor, lc);
        addBox(out, viewProj, windowSize, eye, troughPos + Vec3(2.f, 5.f, 2.f), Vec3(18.f, 2.5f, 6.f), waterColor, lc);

        sf::Color hayColor(198, 168, 78), hayCapColor(220, 196, 108);
        Vec3 hayBase(b.position.x + b.size.x - 26.f, 0.f, b.position.y + 10.f);
        addBox(out, viewProj, windowSize, eye, hayBase, Vec3(9.f, 8.f, 9.f), hayColor, lc);
        addBox(out, viewProj, windowSize, eye, hayBase + Vec3(1.f, 8.f, 1.f), Vec3(7.f, 1.5f, 7.f), hayCapColor, lc);

        const sf::Vector2f cows[] = { { 0.25f, 0.55f }, { 0.55f, 0.42f }, { 0.72f, 0.68f } };
        for (const auto& c : cows) {
            Vec3 p(b.position.x + b.size.x * c.x, 0.f, b.position.y + b.size.y * c.y);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 26.f, 22.f, cowTex, dayNightTint);
        }
    }

    // Beehive -- a small cluster of striped wooden hive boxes in a fenced
    // clearing, with a couple of wildflower patches and a "bee sparkle"
    // glow drifting above each hive (the same glow-billboard trick this
    // file already uses for fireflies/sparkles elsewhere).
    //
    // 2026-08-11 follow-up ("蜂箱上面为什么有草方块" -- why is there a
    // grass block on top of the hive): found it -- the 3 stacked bands
    // were built with `addBandedBox`, which by its own header comment
    // NEVER draws a top face ("every call site that needs one already has
    // a roof... going on top of it") -- meant for building walls with a
    // real roof over them, not a free-standing stack. With no roof placed
    // over the topmost band and no top face of its own, looking down into
    // the hive showed straight through to the ground plane behind it --
    // exactly the "grass square" reported. Switched every band to plain
    // `addBox` (which does include a top face) and added a real peaked
    // roof cap + a dark entrance slit on the front band, so there's
    // nothing left un-capped.
    void addBeehiveProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex, const sf::Texture& flowerTex) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(102, 140, 70), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        addPerimeterFence(out, viewProj, windowSize, eye, b, lc, sf::Color(150, 118, 76));

        const sf::Vector2f hives[] = { { 0.30f, 0.45f }, { 0.45f, 0.55f }, { 0.62f, 0.40f } };
        for (const auto& h : hives) {
            Vec3 p(b.position.x + b.size.x * h.x, 0.f, b.position.y + b.size.y * h.y);
            for (int i = 0; i < 3; ++i) {
                sf::Color band = (i % 2 == 0) ? sf::Color(230, 180, 90) : sf::Color(200, 148, 68);
                addBox(out, viewProj, windowSize, eye, p + Vec3(0.f, static_cast<float>(i) * 5.5f, 0.f), Vec3(12.f, 5.5f, 12.f), band, lc);
            }
            // A shallow peaked roof cap, wider than the boxes below it.
            addBox(out, viewProj, windowSize, eye, p + Vec3(-1.5f, 16.5f, -1.5f), Vec3(15.f, 3.f, 15.f), sf::Color(96, 62, 40), lc);
            addBox(out, viewProj, windowSize, eye, p + Vec3(-0.5f, 19.5f, -0.5f), Vec3(13.f, 2.f, 13.f), sf::Color(120, 78, 50), lc);
            // A dark entrance slit on the bottom band's own south face.
            addBox(out, viewProj, windowSize, eye, p + Vec3(3.5f, 1.5f, 12.f), Vec3(5.f, 2.f, 0.6f), sf::Color(40, 32, 24), lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, p + Vec3(6.f, 24.f, 6.f), 16.f, glowTex, sf::Color(255, 220, 110, 150));
        }

        for (const auto& fp : { sf::Vector2f(0.18f, 0.72f), sf::Vector2f(0.80f, 0.68f) }) {
            Vec3 p(b.position.x + b.size.x * fp.x, 0.f, b.position.y + b.size.y * fp.y);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 16.f, 14.f, flowerTex, sf::Color::White);
        }
    }

    // Trapper -- a rustic camp instead of a farmed plot: a hide-drying
    // rack hung with pelts (`peltTex`), a couple of ground traps, and a
    // small campfire -- no fence, a trapper's camp doesn't pen anything in.
    //
    // 2026-08-11 follow-up ("猎人小屋我觉得这个不好看,可能在旁边加个营地
    // 之类的" -- doesn't look good, maybe add a camp beside it): the rack
    // + 2 traps + fire read as sparse props scattered on bare ground, not
    // an actual lived-in camp. Added a real canvas tent (reusing
    // `addGableRoof` at ground level -- wallY near 0 with no walls under
    // it is exactly a low A-frame tent shape, no new geometry needed), a
    // bedroll beside it, and a cooking pot on a tripod over the fire.
    void addTrapperProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex, const sf::Texture& peltTex) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(96, 128, 66), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        sf::Color postColor(90, 62, 34);
        float rackX = b.position.x + b.size.x * 0.35f, rackZ0 = b.position.y + b.size.y * 0.3f, rackZ1 = b.position.y + b.size.y * 0.7f;
        addBox(out, viewProj, windowSize, eye, Vec3(rackX - 2.f, 0.f, rackZ0), Vec3(4.f, 26.f, 4.f), postColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(rackX - 2.f, 0.f, rackZ1 - 4.f), Vec3(4.f, 26.f, 4.f), postColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(rackX - 2.f, 24.f, rackZ0), Vec3(4.f, 3.f, rackZ1 - rackZ0), postColor, lc);
        for (float t : { 0.2f, 0.5f, 0.8f }) {
            Vec3 p(rackX, 14.f, rackZ0 + (rackZ1 - rackZ0) * t);
            addBillboard(out, viewProj, windowSize, billboardRight, p, 12.f, 18.f, peltTex, sf::Color::White);
        }

        sf::Color trapColor(70, 72, 76);
        for (const auto& tp : { sf::Vector2f(0.65f, 0.35f), sf::Vector2f(0.78f, 0.55f) }) {
            Vec3 p(b.position.x + b.size.x * tp.x, 0.f, b.position.y + b.size.y * tp.y);
            addBox(out, viewProj, windowSize, eye, p, Vec3(8.f, 1.5f, 8.f), trapColor, lc);
        }

        Vec3 firePos(b.position.x + b.size.x * 0.25f, 0.f, b.position.y + b.size.y * 0.75f);
        sf::Color logColor(90, 60, 34);
        for (float rot : { 0.f, 1.f }) {
            addBox(out, viewProj, windowSize, eye, firePos + Vec3(rot * -6.f, 0.f, rot * 4.f), Vec3(12.f, 3.f, 3.f), logColor, lc);
        }
        addGlowBillboard(out, viewProj, windowSize, billboardRight, firePos + Vec3(2.f, 4.f, 2.f), 16.f, glowTex, sf::Color(255, 140, 60, 200));

        // A cooking pot on a 3-leg tripod straddling the fire.
        sf::Color tripodColor(60, 44, 30), potColor(58, 56, 58);
        Vec3 potCenter = firePos + Vec3(2.f, 0.f, 2.f);
        for (const auto& lean : { sf::Vector2f(-6.f, -3.f), sf::Vector2f(6.f, -3.f), sf::Vector2f(0.f, 6.f) }) {
            addBox(out, viewProj, windowSize, eye, potCenter + Vec3(lean.x, 0.f, lean.y), Vec3(2.f, 18.f, 2.f), tripodColor, lc);
        }
        addBox(out, viewProj, windowSize, eye, potCenter + Vec3(-4.f, 12.f, -4.f), Vec3(8.f, 6.f, 8.f), potColor, lc);

        // A canvas A-frame tent (`addGableRoof` at ground level -- no
        // walls under it, just the 2 roof slopes + gable ends, reads as a
        // low tent) plus a bedroll beside it.
        sf::Color tentColor(178, 148, 100);
        Vec3 tentPos(b.position.x + b.size.x * 0.62f, 0.f, b.position.y + b.size.y * 0.12f);
        addGableRoof(out, viewProj, windowSize, eye, tentPos, Vec3(22.f, 0.f, 18.f), 2.f, 15.f, tentColor, lc);

        sf::Color bedrollColor(120, 60, 60);
        Vec3 bedrollPos(tentPos.x - 14.f, 0.f, tentPos.z + 3.f);
        addBox(out, viewProj, windowSize, eye, bedrollPos, Vec3(9.f, 2.5f, 18.f), bedrollColor, lc);
        addBox(out, viewProj, windowSize, eye, bedrollPos + Vec3(0.f, 2.5f, 0.f), Vec3(9.f, 1.5f, 4.f), shade3d(bedrollColor, -20), lc);
    }

    // Tea Field -- dense rows of tea bushes (`teaBushTex`) instead of the
    // flat single-color crop fill every other raw field uses, plus a
    // woven picking basket.
    void addTeaFieldProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& teaBushTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(90, 130, 66), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);

        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                Vec3 p(b.position.x + b.size.x * (0.14f + 0.24f * static_cast<float>(col)), 0.f,
                    b.position.y + b.size.y * (0.22f + 0.28f * static_cast<float>(row)));
                addBillboard(out, viewProj, windowSize, billboardRight, p, 20.f, 16.f, teaBushTex, dayNightTint);
            }
        }

        sf::Color basketColor(150, 108, 62);
        Vec3 basketPos(b.position.x + b.size.x * 0.5f - 7.f, 0.f, b.position.y + b.size.y - 14.f);
        addBandedBox(out, viewProj, windowSize, eye, basketPos, Vec3(14.f, 10.f, 14.f), basketColor, lc, nullptr, 40.f, true);
    }

    // Flax Field -- 2026-08-11 detail pass ("亚麻田可以再细节一点吗" --
    // can Flax Field get more detail): the old version was 20 flat
    // `flaxFlowerTex` billboards (a lying-flat stem rectangle + 3 dots)
    // floating directly on bare dirt, plus one lone sheaf -- no actual
    // stalk geometry, same "flat billboard on flat ground" gap Vineyard's
    // own trellis round already fixed for grapes. Now each spot gets a
    // real raised stem (a thin green box, real height instead of a flat
    // sprite) with the flower billboard sitting on top of it, a perimeter
    // fence (`addPerimeterFence`, same as Herb Garden), and 2 tied sheaves
    // (with a visible binding band) instead of 1 untied stack.
    void addFlaxFieldProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& flaxFlowerTex, sf::Color dayNightTint) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(108, 132, 78), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        addPerimeterFence(out, viewProj, windowSize, eye, b, lc, sf::Color(150, 118, 76));

        sf::Color stemColor(96, 132, 80);
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 5; ++col) {
                Vec3 p(b.position.x + b.size.x * (0.10f + 0.20f * static_cast<float>(col)), 0.f,
                    b.position.y + b.size.y * (0.16f + 0.22f * static_cast<float>(row)));
                float stemH = 10.f + (((row + col) % 2 == 0) ? 2.f : 0.f); // slight height variance -- a perfectly even field reads as artificial
                addBox(out, viewProj, windowSize, eye, p - Vec3(1.f, 0.f, 1.f), Vec3(2.f, stemH, 2.f), stemColor, lc);
                addBillboard(out, viewProj, windowSize, billboardRight, p + Vec3(0.f, stemH, 0.f), 14.f, 12.f, flaxFlowerTex, dayNightTint);
            }
        }

        // 2 tied sheaves (was 1, untied) near the south edge.
        sf::Color sheafColor(198, 182, 96), bandColor(120, 70, 40);
        for (float sx : { -8.f, 10.f }) {
            Vec3 sheafPos(b.position.x + b.size.x * 0.5f + sx, 0.f, b.position.y + b.size.y - 14.f);
            addBox(out, viewProj, windowSize, eye, sheafPos, Vec3(8.f, 16.f, 8.f), sheafColor, lc);
            addBox(out, viewProj, windowSize, eye, sheafPos + Vec3(-1.f, 15.f, -1.f), Vec3(10.f, 3.f, 10.f), shade3d(sheafColor, -20), lc);
            addBox(out, viewProj, windowSize, eye, sheafPos + Vec3(-1.f, 5.f, -1.f), Vec3(10.f, 2.5f, 10.f), bandColor, lc);
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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
            addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(stallW * 0.84f, 18.f, counterDepth), counterColor, lc, nullptr, 40.f, true);

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
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(14.f, 10.f, 10.f), sf::Color(96, 68, 40), lc, nullptr, 40.f, true);
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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, crateBase, Vec3(14.f, 14.f, 12.f), crateColor, lc, nullptr, 40.f, true);
        addBandedBox(out, viewProj, windowSize, eye, crateBase + Vec3(1.f, 14.f, 1.f), Vec3(12.f, 12.f, 10.f), crateColor, lc, nullptr, 40.f, true);
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
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(16.f, 14.f, 14.f), sf::Color(120, 86, 50), lc, nullptr, 40.f, true);
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
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        // Sign, flush on the gable end above the eave.
        float signW = enclosedW * 0.6f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + roofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

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

        // ---- 2 small stone cairns (3 shrinking stacked stones each,
        // reusing the tombstone colors) in the open gap between the bench
        // and the walkway.
        //
        // 2026-08-11: the obelisk, stone urn, and stacked-stone pile that
        // used to live in this same block are gone -- removed outright at
        // the user's own explicit request ("一样还在删了吧" -- still
        // showing up, just delete them) after the whole "pale shapes near
        // Sawmill" saga (see this file's own long log above) survived 3
        // real fixes in a row (relocating the cluster, shrinking the
        // obelisk, moving Mason's own position off Sawmill's shared X
        // column) without resolving. The cairns were never implicated in
        // any of those reports and are kept. ----
        for (const auto& c : { sf::Vector2f(34.f, 70.f), sf::Vector2f(42.f, 75.f) }) {
            Vec3 cp(basePos.x + c.x, 0.f, basePos.y + c.y);
            addBox(out, viewProj, windowSize, eye, cp, Vec3(6.f, 4.f, 6.f), tombColor, lc);
            addBox(out, viewProj, windowSize, eye, cp + Vec3(1.f, 4.f, 1.f), Vec3(4.f, 3.f, 4.f), shade3d(tombColor, -8), lc);
            addBox(out, viewProj, windowSize, eye, cp + Vec3(1.8f, 7.f, 1.8f), Vec3(2.4f, 2.5f, 2.4f), tombCapColor, lc);
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
            addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(6.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
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

    // Bakery -- from a seventeenth reference image, explicitly labeled
    // "面包坊" (Bakery) itself, no scope-check needed (`bakery` is a plain
    // tier-2 wheat->bread processor -- no raw-tier sibling to confuse it
    // with, unlike the lumber/quarry/sheep pairings): a log-cabin shop
    // building next to a big outdoor beehive-shaped brick oven with its
    // own chimney, display tables of bread out front, hay bales, and a
    // flour sack pile. Only `b.id == "bakery"` uses this -- one of the
    // ~33 non-Town-Square processor businesses this file's own header
    // comment flagged as still generic box+roof, restored the same
    // "Workshop family" way Sawmill/Mason already were.
    void addBakeryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& breadTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plankWall(150, 112, 68);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(96, 60, 40);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);
        sf::Color plantBoxColor(96, 68, 40);
        sf::Color brickColor(150, 82, 56);
        sf::Color hayColor(198, 168, 78);
        sf::Color hayCapColor(220, 196, 108);
        sf::Color sackColor(196, 168, 118);
        sf::Color tableColor(110, 78, 46);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = basePos.z + b.size.y + 1.5f;

        // 2026-08-11 follow-up ("面包房可以做矮和拉宽吗,高高尖尖的有点丑" --
        // make it shorter and wider, it reads too tall/pointy): wallH2 cut
        // to 0.72x (was the full, un-reduced wallH -- the tallest cabin of
        // any Workshop-family building so far) and the gable roof gets its
        // own locally-reduced rise (`bakeryRoofRise`, 0.6x the shared
        // `roofRise` every other building's roof uses) instead of the flat
        // constant -- between this and the widened WorldBuilding rect (see
        // buildZones()'s own comment), the cabin's own height:width ratio
        // drops on both ends at once instead of just one.
        float wallH2 = wallH * 0.72f;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;
        float bakeryRoofRise = roofRise * 0.6f;

        // ---- Enclosed cabin (west 40% of the -- now widened -- footprint,
        // narrowed from 52% a round ago to free room for the boiler-room
        // annex right next to it -- see its own comment below): stone
        // foundation, timber-plank upper band, a real side-gable roof --
        // same recipe Sawmill's own enclosed block already established. ----
        float enclosedW = b.size.x * 0.40f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, bakeryRoofRise, roofColor, lc, &shingleTex, kShingleUv);

        // Plain timber door, centered on the cabin's own width.
        float doorW = enclosedW * 0.22f, doorH = foundationH + upperH * 0.55f;
        Vec3 doorPos(basePos.x + enclosedW * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        // A window with a flower box, west of the door.
        float winSize = enclosedW * 0.15f;
        Vec3 winPos(basePos.x + enclosedW * 0.15f, foundationH + upperH * 0.4f, southZ);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        // Sign on the gable end.
        float signW = enclosedW * 0.6f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + bakeryRoofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

        // The cabin's own chimney, plus a smoke puff.
        Vec3 chimneyPos(basePos.x + enclosedW * 0.78f, wallTop * 0.3f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, wallTop * 0.75f, 11.f), sf::Color(96, 90, 86), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, wallTop * 0.75f + 12.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));

        // ---- Boiler-room annex (2026-08-11 follow-up, "面包房可以做多一
        // 个锅炉房的造型...就做在屋子的隔壁吧" -- add a boiler-room shape,
        // right next to the house): a narrow stone-walled lean-to flush
        // against the cabin's own east wall, between it and the oven bay
        // -- a real walled room reading as "where the heat for baking
        // actually comes from," distinct from the outdoor oven mound.
        // Kept shallower than the cabin's own full depth (40 units, not
        // 80) specifically so its own flat roof stays under
        // `kGroundSliceZ`'s 60-unit single-quad-depth-sort threshold (see
        // `addGroundQuad`'s own header comment on the bug class a wider
        // unsliced flat quad already caused once, on Clinic's own roof
        // deck) -- a plain `addBox` roof this shallow can't trip that
        // bug the way a full-depth one could. ----
        float annexX0 = basePos.x + enclosedW;
        float annexW = 16.f, annexD = 40.f;
        float annexZ0 = basePos.z + (b.size.y - annexD);
        float annexWallH = wallTop * 0.65f;
        sf::Color boilerStone = shade3d(stone, -6);
        sf::Color boilerMetal(96, 98, 102);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, annexZ0), Vec3(annexW, annexWallH, annexD), boilerStone, lc, &stoneTex, kStoneUv);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, annexZ0), Vec3(annexW, 3.f, annexD), shade3d(roofColor, -14), lc);

        // A dark open archway (Kitchen's own `archTex`, reused) on the
        // annex's south face, showing the boiler within, plus 2 small
        // flanking firebox glows -- offset beside the arch, not
        // overlapping it, the same lesson the oven's own firebox below
        // (and Clinic's cross-decal round before it) already learned.
        float annexArchCenterX = annexX0 + annexW * 0.5f, annexArchZ = annexZ0 + annexD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX, 0.f, annexArchZ), 12.f, 18.f, archTex, sf::Color(50, 46, 44));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX - 9.f, 5.f, annexArchZ), 12.f, glowTex, sf::Color(255, 140, 60, 160));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX + 9.f, 5.f, annexArchZ), 12.f, glowTex, sf::Color(255, 140, 60, 160));

        // A glimpse of the boiler tank itself, just inside the archway --
        // 2 stacked grey boxes with a darker rim band, the same barrel
        // technique this file already reuses for barrels/dye vats.
        Vec3 boilerPos(annexX0 + annexW * 0.5f - 5.f, 0.f, annexZ0 + annexD - 14.f);
        addBox(out, viewProj, windowSize, eye, boilerPos, Vec3(10.f, 20.f, 10.f), boilerMetal, lc);
        addBox(out, viewProj, windowSize, eye, boilerPos + Vec3(-0.8f, 18.f, -0.8f), Vec3(11.6f, 2.f, 11.6f), shade3d(boilerMetal, -18), lc);

        // A tall, thick brick smokestack -- noticeably bigger than the
        // cabin's own chimney, the industrial-boiler read a mere room
        // shape alone wouldn't sell on its own.
        Vec3 boilerChimneyPos(annexX0 + annexW * 0.5f - 7.f, annexWallH, annexZ0 + 10.f);
        addBox(out, viewProj, windowSize, eye, boilerChimneyPos, Vec3(14.f, wallTop * 0.75f, 14.f), sf::Color(120, 66, 50), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, boilerChimneyPos + Vec3(7.f, wallTop * 0.75f + 14.f, 7.f), 20.f, glowTex, sf::Color(150, 150, 154, 130));

        // ---- Big outdoor brick oven (east of the annex) -- a beehive-
        // shaped brick mound (addPyramid, same primitive Mine/Gold Mine's
        // own rock mound and Mason's obelisk already use, here low/wide
        // instead of tall for a dome-ish read) on its own low stone
        // hearth platform, a firebox arch decal on its south face
        // (Kitchen's own `archTex` reused outright rather than baking a
        // near-duplicate), 2 flanking warm glows (offset beside the arch,
        // not overlapping it -- the same lesson Clinic's own cross-decal
        // round learned the hard way), and its own small chimney.
        //
        // 2026-08-11 rework ("面包房的模型...那个三角形的东西...看起来很
        // 奇怪" -- the pyramid read as an odd solid-orange shape floating
        // alone in the grass): the previous version centered a flat-color
        // mound of a fixed size inside whatever leftover space the annex
        // left behind, which after the footprint's own widen left large
        // empty gaps on every side with nothing tying the mound to the
        // rest of the building cluster. Now the mound (a) scales with the
        // oven area instead of a flat constant, so it actually fills most
        // of its own yard the way Mine's identical-primitive mountain
        // fills its whole lot (see addMineProps above), (b) sits a fixed
        // small gap off the annex instead of centered with slack on both
        // sides, (c) gets a stoneTex-textured pedestal underneath (a few
        // units wider than the pyramid's own base on every side) so it
        // reads as something built on a hearth platform rather than a
        // solid-color shape resting straight on grass, and (d) the
        // pyramid itself now takes `stoneTex` too (tinted by `brickColor`,
        // via addPyramid's new optional-texture param above) instead of a
        // flat fill. ----
        float ovenX0 = annexX0 + annexW;
        float ovenAreaW = b.size.x - enclosedW - annexW;
        float pedestalH = 6.f, pedMargin = 6.f;
        float moundW = ovenAreaW * 0.62f, moundD = b.size.y * 0.5f;
        Vec3 pedPos(ovenX0 + pedMargin, 0.f, basePos.z + (b.size.y - moundD) * 0.5f - pedMargin);
        Vec3 moundPos(pedPos.x + pedMargin, pedestalH, pedPos.z + pedMargin);
        addBandedBox(out, viewProj, windowSize, eye, pedPos, Vec3(moundW + pedMargin * 2.f, pedestalH, moundD + pedMargin * 2.f), stone, lc, &stoneTex, kStoneUv, true);
        float moundH = 30.f;
        addPyramid(out, viewProj, windowSize, eye, moundPos, sf::Vector2f(moundW, moundD), moundH, brickColor, lc, &stoneTex, kStoneUv);
        float archW = 16.f, archCenterX = moundPos.x + moundW * 0.5f, archZ = moundPos.z + moundD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX - archW * 0.5f, pedestalH, archZ), archW, 16.f, archTex, sf::Color(80, 60, 50));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX - archW * 0.9f, pedestalH + 6.f, archZ), 16.f, glowTex, sf::Color(255, 150, 70, 170));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX + archW * 0.9f, pedestalH + 6.f, archZ), 16.f, glowTex, sf::Color(255, 150, 70, 170));

        // No chimney stack on the mound itself anymore (2026-08-11
        // follow-up, "可以让那个橙色三角形头上的石柱子消失吗" -- make the
        // stone column on top of the orange triangle disappear): the oven
        // already has its own smoke tell from the annex's boiler
        // smokestack right next to it (see boilerChimneyPos above) and the
        // cabin's own chimney further west, so this 3rd grey box balanced
        // on the pyramid's apex wasn't pulling its own weight -- just an
        // odd stick poking out of a triangle from most angles.

        // Hay bales, north of the oven mound, clear of its own footprint.
        Vec3 hayBase(ovenX0 + 8.f, 0.f, basePos.z + 2.f);
        for (int i = 0; i < 2; ++i) {
            Vec3 hp = hayBase + Vec3(static_cast<float>(i) * 11.f, 0.f, 0.f);
            addBox(out, viewProj, windowSize, eye, hp, Vec3(9.f, 8.f, 9.f), hayColor, lc);
            addBox(out, viewProj, windowSize, eye, hp + Vec3(1.f, 8.f, 1.f), Vec3(7.f, 1.5f, 7.f), hayCapColor, lc);
        }

        // ---- 2 outdoor display tables with bread baskets, south of the
        // whole building (the same "props extend past the lot's own south
        // edge into the shared yard" convention Sawmill's own log piles
        // already use). ----
        for (float tx : { 8.f, 42.f }) {
            float tableLegH = 12.f;
            Vec3 tablePos(basePos.x + tx, 0.f, southZ + 3.f);
            for (float lx : { 2.f, 22.f }) {
                for (float lz : { 2.f, 9.f }) {
                    addBox(out, viewProj, windowSize, eye, tablePos + Vec3(lx, 0.f, lz), Vec3(2.f, tableLegH, 2.f), beamColor, lc);
                }
            }
            addBandedBox(out, viewProj, windowSize, eye, tablePos + Vec3(0.f, tableLegH, 0.f), Vec3(26.f, 3.f, 12.f), tableColor, lc, nullptr, 40.f, true);
            addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(8.f, tableLegH + 3.f, 6.f), 22.f, 15.f, breadTex, sf::Color::White);
            addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(18.f, tableLegH + 3.f, 6.f), 18.f, 12.f, breadTex, sf::Color::White);
        }

        // Flour sack pile, south of the gap between the cabin and the oven
        // -- south of both display tables (z+18, past their own z+15
        // southern edge) to stay clear of the 2nd table's own footprint.
        Vec3 sackPos(basePos.x + enclosedW - 3.f, 0.f, southZ + 18.f);
        addBox(out, viewProj, windowSize, eye, sackPos, Vec3(9.f, 9.f, 9.f), sackColor, lc);
        addBox(out, viewProj, windowSize, eye, sackPos + Vec3(2.f, 8.f, 1.f), Vec3(7.f, 7.f, 7.f), shade3d(sackColor, -12), lc);
    }

    // Preserve -- from the same reference image as Orchard's own
    // 2026-08-11 tree rework (see addOrchardProps above), this time asked
    // for by name ("现在到果酱坊" -- now onto the Jam Workshop): the
    // reference's own shop building, fruit press, and jar-lined stalls
    // belong here, not on Orchard itself (Orchard stayed a raw-tier flat
    // plot; Preserve is its tier-2 processor sibling, the exact "not yet
    // built" business npc_orchardist's own dialogue already name-drops).
    // Same 3-volume "Workshop family" recipe Bakery established (cabin +
    // annex + outdoor apparatus), reskinned for jam instead of bread: a
    // log-cabin shop, a press annex housing a wooden screw press over a
    // catch-barrel, and an open stone hearth with a cauldron simmering
    // over a real fire (no chimney stack on the cauldron itself -- see
    // Bakery's own 2026-08-11 "remove the pointless stone column" fix
    // just above, same lesson applied up front here instead of having to
    // walk it back later), plus a jam-jar display table and a fruit
    // crate/sack out front. Only `b.id == "preserve"` uses this -- the
    // 3rd Workshop-family processor building after Bakery/Textile.
    void addPreserveBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& jamJarsTex, const sf::Texture& pressWheelTex, const sf::Texture& fruitCrateTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plankWall(148, 96, 92);   // a touch more red than Bakery's plankWall -- this shop's own identity color, echoing preserve's 2D accent (190,70,70)
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(92, 54, 52);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 60, 58);
        sf::Color plantBoxColor(96, 68, 40);
        sf::Color tableColor(110, 78, 46);
        sf::Color sackColor(196, 150, 118);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = basePos.z + b.size.y + 1.5f;

        // Same shortened/widened proportions Bakery's own 2026-08-11 fix
        // established for this file's 3-volume Workshop-family shape.
        float wallH2 = wallH * 0.72f;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;
        float presRoofRise = roofRise * 0.6f;

        // ---- Enclosed cabin (west ~38% of the footprint) -- same recipe
        // Bakery's own cabin already established. ----
        float enclosedW = b.size.x * 0.38f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, presRoofRise, roofColor, lc, &shingleTex, kShingleUv);

        float doorW = enclosedW * 0.22f, doorH = foundationH + upperH * 0.55f;
        Vec3 doorPos(basePos.x + enclosedW * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        float winSize = enclosedW * 0.16f;
        Vec3 winPos(basePos.x + enclosedW * 0.16f, foundationH + upperH * 0.4f, southZ);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        float signW = enclosedW * 0.62f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + presRoofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

        Vec3 chimneyPos(basePos.x + enclosedW * 0.78f, wallTop * 0.3f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, wallTop * 0.75f, 11.f), sf::Color(96, 90, 86), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, wallTop * 0.75f + 12.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));

        // ---- Press annex (east of the cabin) -- a narrow stone lean-to
        // like Bakery's own boiler room, but housing a wooden screw press
        // over a catch-barrel instead of a boiler: a central post topped
        // with `pressWheelTex`'s cross-handle, and a squat barrel below it
        // (the same "2 stacked boxes, darker rim band" barrel technique
        // Bakery's boiler tank already used). No fire/glow in here -- a
        // press has nothing burning, unlike the boiler room it's replacing. ----
        float annexX0 = basePos.x + enclosedW;
        float annexW = 18.f, annexD = 40.f;
        float annexZ0 = basePos.z + (b.size.y - annexD);
        float annexWallH = wallTop * 0.65f;
        sf::Color annexStone = shade3d(stone, -6);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, annexZ0), Vec3(annexW, annexWallH, annexD), annexStone, lc, &stoneTex, kStoneUv);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, annexZ0), Vec3(annexW, 3.f, annexD), shade3d(roofColor, -14), lc);

        float annexArchCenterX = annexX0 + annexW * 0.5f, annexArchZ = annexZ0 + annexD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX - 6.f, 0.f, annexArchZ), 12.f, 18.f, archTex, sf::Color(60, 50, 44));

        sf::Color pressPostColor(110, 78, 44);
        Vec3 pressPostPos(annexX0 + annexW * 0.5f - 3.f, 0.f, annexZ0 + annexD - 16.f);
        addBox(out, viewProj, windowSize, eye, pressPostPos, Vec3(6.f, annexWallH * 0.9f, 6.f), pressPostColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, pressPostPos + Vec3(3.f, annexWallH * 0.9f, 3.f), 16.f, 16.f, pressWheelTex, sf::Color::White);

        sf::Color barrelColor(120, 86, 50);
        Vec3 barrelPos(annexX0 + annexW * 0.5f - 5.f, 0.f, annexZ0 + 6.f);
        addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(10.f, 14.f, 10.f), barrelColor, lc);
        addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.6f, 12.f, -0.6f), Vec3(11.2f, 2.f, 11.2f), shade3d(barrelColor, -18), lc);

        // ---- Open stone hearth with a simmering cauldron (east of the
        // annex) -- same low stoneTex-textured pedestal Bakery's own oven
        // rework introduced, but topped with a real pot silhouette (wide
        // iron body + a narrower darker rim band, the same "2-box taper"
        // trick the barrel above just used) instead of a brick pyramid,
        // since a jam cauldron reads as a pot, not a beehive oven. A
        // visible dark-red "jam surface" strip sits just inside the rim,
        // and steam (not smoke) rises off it -- no separate chimney/
        // column on top, learning Bakery's own "pointless stone stick"
        // lesson up front. ----
        float hearthX0 = annexX0 + annexW;
        float hearthAreaW = b.size.x - enclosedW - annexW;
        float pedestalH = 6.f, pedMargin = 6.f;
        float cauldronW = std::min(hearthAreaW * 0.4f, 40.f), cauldronD = b.size.y * 0.42f;
        sf::Color potColor(50, 48, 50);
        sf::Color jamColor(150, 40, 46);
        Vec3 pedPos(hearthX0 + pedMargin, 0.f, basePos.z + (b.size.y - cauldronD) * 0.5f - pedMargin);
        Vec3 potPos(pedPos.x + pedMargin, pedestalH, pedPos.z + pedMargin);
        addBandedBox(out, viewProj, windowSize, eye, pedPos, Vec3(cauldronW + pedMargin * 2.f, pedestalH, cauldronD + pedMargin * 2.f), stone, lc, &stoneTex, kStoneUv, true);
        addBox(out, viewProj, windowSize, eye, potPos, Vec3(cauldronW, 20.f, cauldronD), potColor, lc);
        addBox(out, viewProj, windowSize, eye, potPos + Vec3(cauldronW * 0.08f, 20.f, cauldronD * 0.08f), Vec3(cauldronW * 0.84f, 3.f, cauldronD * 0.84f), shade3d(potColor, -16), lc);
        addBox(out, viewProj, windowSize, eye, potPos + Vec3(cauldronW * 0.16f, 22.5f, cauldronD * 0.16f), Vec3(cauldronW * 0.68f, 1.5f, cauldronD * 0.68f), jamColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, potPos + Vec3(cauldronW * 0.5f, 26.f, cauldronD * 0.5f), 20.f, glowTex, sf::Color(230, 230, 235, 110));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(potPos.x + cauldronW * 0.3f, 4.f, potPos.z + cauldronD + 1.f), 14.f, glowTex, sf::Color(255, 150, 60, 170));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(potPos.x + cauldronW * 0.7f, 4.f, potPos.z + cauldronD + 1.f), 14.f, glowTex, sf::Color(255, 150, 60, 170));

        // A couple of firewood logs at the hearth's own base.
        sf::Color logColor(90, 60, 34);
        for (float lx : { -4.f, 8.f }) {
            Vec3 logPos(potPos.x + cauldronW * 0.5f + lx, 0.f, potPos.z + cauldronD + 5.f);
            addBox(out, viewProj, windowSize, eye, logPos, Vec3(12.f, 4.f, 4.f), logColor, lc);
        }

        // ---- Jam-jar display table, south of the cabin (Bakery's own
        // bread-table convention, one table instead of 2, topped with
        // `jamJarsTex` instead of bread). ----
        float tableLegH = 12.f;
        Vec3 tablePos(basePos.x + 10.f, 0.f, southZ + 3.f);
        for (float lx : { 2.f, 22.f }) {
            for (float lz : { 2.f, 9.f }) {
                addBox(out, viewProj, windowSize, eye, tablePos + Vec3(lx, 0.f, lz), Vec3(2.f, tableLegH, 2.f), beamColor, lc);
            }
        }
        addBandedBox(out, viewProj, windowSize, eye, tablePos + Vec3(0.f, tableLegH, 0.f), Vec3(26.f, 3.f, 12.f), tableColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(13.f, tableLegH + 3.f, 6.f), 24.f, 14.f, jamJarsTex, sf::Color::White);

        // A fruit crate and a fruit-mash sack, south of the press annex --
        // same "props spill past the lot's south edge" convention as
        // Bakery's own flour sack, `fruitCrateTex` reused outright from
        // Orchard's own 2026-08-11 rework rather than baking a near-dupe.
        Vec3 cratePos(annexX0 + 2.f, 0.f, southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(14.f, 10.f, 14.f), sf::Color(150, 108, 62), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, cratePos + Vec3(7.f, 10.f, 7.f), 14.f, 10.f, fruitCrateTex, sf::Color::White);

        Vec3 sackPos(basePos.x + enclosedW - 3.f, 0.f, southZ + 18.f);
        addBox(out, viewProj, windowSize, eye, sackPos, Vec3(9.f, 9.f, 9.f), sackColor, lc);
        addBox(out, viewProj, windowSize, eye, sackPos + Vec3(2.f, 8.f, 1.f), Vec3(7.f, 7.f, 7.f), shade3d(sackColor, -12), lc);
    }

    // Goldsmith -- from the same reference image family as Preserve above
    // (2026-08-11, "现在到金匠铺" -- now onto the Goldsmith), Gold Mine's
    // own tier-2 processor sibling (the ore->ingot/jewelry step
    // npc_prospector2's own dialogue already name-drops). Same 3-volume
    // Workshop-family recipe, reskinned around fire and metal instead of
    // dough or fruit: a log-cabin shop (gold-accented trim, matching this
    // business's own 2D accent color), a forge annex with a real fire
    // glow, an anvil, and a bellows -- instead of Preserve's press or
    // Bakery's oven, a goldsmith's own outdoor apparatus is a display
    // counter (glass-look case top, stacked gold bars, a gem tray) plus a
    // nugget-loaded wheelbarrow tying it back visually to Gold Mine's own
    // ore cart. Only `b.id == "goldsmith"` uses this -- the 4th Workshop-
    // family processor building after Bakery/Textile/Preserve.
    void addGoldsmithBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& goldBarTex, const sf::Texture& gemTrayTex, const sf::Texture& goldOreTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plankWall(120, 104, 76);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(84, 66, 40);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(150, 120, 50);   // gold-accented trim, echoing goldsmith's 2D accent (220,180,60)
        sf::Color plantBoxColor(96, 68, 40);
        sf::Color tableColor(110, 78, 46);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = basePos.z + b.size.y + 1.5f;

        float wallH2 = wallH * 0.72f;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;
        float smithRoofRise = roofRise * 0.6f;

        // ---- Enclosed cabin (west ~38%) -- same recipe Bakery/Preserve
        // already established. ----
        float enclosedW = b.size.x * 0.38f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, smithRoofRise, roofColor, lc, &shingleTex, kShingleUv);

        float doorW = enclosedW * 0.22f, doorH = foundationH + upperH * 0.55f;
        Vec3 doorPos(basePos.x + enclosedW * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        float winSize = enclosedW * 0.16f;
        Vec3 winPos(basePos.x + enclosedW * 0.16f, foundationH + upperH * 0.4f, southZ);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        float signW = enclosedW * 0.62f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + smithRoofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

        Vec3 chimneyPos(basePos.x + enclosedW * 0.78f, wallTop * 0.3f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, wallTop * 0.75f, 11.f), sf::Color(96, 90, 86), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, wallTop * 0.75f + 12.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));

        // ---- Forge annex (east of the cabin) -- same narrow stone
        // lean-to shape as Preserve's press annex, but this one actually
        // has fire in it: an anvil block, a real firebox glow instead of a
        // cold interior, and a bellows-shaped box beside it. ----
        float annexX0 = basePos.x + enclosedW;
        float annexW = 18.f, annexD = 40.f;
        float annexZ0 = basePos.z + (b.size.y - annexD);
        float annexWallH = wallTop * 0.65f;
        sf::Color annexStone = shade3d(stone, -6);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, annexZ0), Vec3(annexW, annexWallH, annexD), annexStone, lc, &stoneTex, kStoneUv);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, annexZ0), Vec3(annexW, 3.f, annexD), shade3d(roofColor, -14), lc);

        float annexArchCenterX = annexX0 + annexW * 0.5f, annexArchZ = annexZ0 + annexD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX - 6.f, 0.f, annexArchZ), 12.f, 18.f, archTex, sf::Color(50, 40, 36));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexArchCenterX, 6.f, annexArchZ), 14.f, glowTex, sf::Color(255, 140, 50, 190));

        sf::Color anvilColor(58, 56, 58);
        Vec3 anvilPos(annexX0 + annexW * 0.5f - 5.f, 0.f, annexZ0 + annexD - 16.f);
        addBox(out, viewProj, windowSize, eye, anvilPos, Vec3(4.f, 8.f, 4.f), shade3d(anvilColor, -10), lc);
        addBox(out, viewProj, windowSize, eye, anvilPos + Vec3(-3.f, 8.f, -1.f), Vec3(10.f, 3.f, 6.f), anvilColor, lc);

        sf::Color bellowsColor(120, 78, 46);
        Vec3 bellowsPos(annexX0 + annexW * 0.5f - 4.f, 0.f, annexZ0 + 6.f);
        addBox(out, viewProj, windowSize, eye, bellowsPos, Vec3(8.f, 6.f, 10.f), bellowsColor, lc);
        addBox(out, viewProj, windowSize, eye, bellowsPos + Vec3(1.f, 6.f, 1.f), Vec3(6.f, 4.f, 8.f), shade3d(bellowsColor, 14), lc);

        // ---- Display counter (east of the annex) -- a wood counter
        // topped with a glass-look case (a pale translucent-tinted box),
        // stacked gold bars, and a gem tray, flanked by 2 lantern posts
        // (the same "2 lanterns beside a shop counter" convention Farm's
        // own yard already uses). ----
        float counterX0 = annexX0 + annexW;
        float counterAreaW = b.size.x - enclosedW - annexW;
        float counterW = std::min(counterAreaW * 0.7f, 70.f), counterD = b.size.y * 0.4f;
        Vec3 counterPos(counterX0 + (counterAreaW - counterW) * 0.5f, 0.f, basePos.z + (b.size.y - counterD) * 0.5f);
        addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(counterW, 14.f, counterD), tableColor, lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, counterPos + Vec3(2.f, 14.f, 2.f), Vec3(counterW - 4.f, 6.f, counterD - 4.f), sf::Color(190, 214, 224, 200), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(counterPos.x + counterW * 0.28f, 20.f, counterPos.z + counterD * 0.5f), 20.f, 12.f, goldBarTex, sf::Color::White);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(counterPos.x + counterW * 0.72f, 20.f, counterPos.z + counterD * 0.5f), 20.f, 12.f, gemTrayTex, sf::Color::White);
        for (float lx : { -6.f, 6.f }) {
            Vec3 lanternPos(counterPos.x + counterW * 0.5f + lx, 0.f, counterPos.z - 10.f);
            addBox(out, viewProj, windowSize, eye, lanternPos, Vec3(3.f, 24.f, 3.f), beamColor, lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos + Vec3(1.5f, 28.f, 1.5f), 16.f, glowTex, sf::Color(255, 200, 120, 160));
        }

        // ---- Nugget-loaded wheelbarrow, south of the cabin -- ties back
        // to Gold Mine's own ore cart visually (`goldOreTex` reused
        // outright), same crude cart-body-plus-wheel convention every
        // barrow in this file already uses. ----
        sf::Color cartColor(118, 86, 52), wheelColor(60, 44, 28);
        Vec3 cartPos(basePos.x + 10.f, 0.f, southZ + 4.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, cartPos + Vec3(8.f, 8.f, 5.f), 18.f, 12.f, goldOreTex, sf::Color::White);
    }

    // ---- Zone 2 (Mining District) batch, 2026-08-11 ("有的话就试看每一
    // 间你自己设计" -- go ahead and design the rest yourself): the 5
    // remaining Mining District businesses, using the shared
    // `addWorkshopCabin` helper above instead of each hand-copying the
    // cabin recipe again. The 2D world already sorts every non-Town-
    // Square tier-2/3 processor into 8 themed archetype families
    // (isOvenId/isForgeId/isSawmillId/isFiberId/isMasonGemId/isBreweryId/
    // isStallId/isSmokehouseId, see their own comments near the top of
    // GameWorld.cpp) -- these 5 mirror that same grouping instead of
    // inventing fresh themes per building: Smelter/Blacksmith are Forge
    // family (fire + metal, like Goldsmith), Carpenter is Sawmill family
    // (timber), Tailor is Fiber family (cloth, like Textile), Gemshop is
    // MasonGem family (stone/gems, like Mason). ----

    // Smelter -- Forge family, tier-2 ore->ingot processor. Un-widened
    // (110x80, see buildZones()'s own comment on column 2's path-spine
    // clearance) -- the furnace annex fills the lot's entire remaining
    // east span instead of leaving a side margin, same convention Sawmill/
    // Mason/Textile used before any of them got widened. Bigger/louder
    // than Goldsmith's own forge annex (a full-depth furnace + its own
    // smokestack, not a lean-to) since smelting ore is the loud industrial
    // step, not a finishing one.
    void addSmelterBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& ironBarTex) {
        sf::Color plankWall(112, 98, 92), roofColor(80, 60, 54), signColor(178, 96, 44); // smelter's own 2D accent (230,110,40), muted for a sign board
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);
        sf::Color annexStone = shade3d(sf::Color(118, 114, 108), -6);

        float annexX0 = basePos.x + cab.enclosedW;
        float annexW = b.size.x - cab.enclosedW;
        float annexWallH = cab.wallTop * 0.85f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, basePos.z), Vec3(annexW, annexWallH, b.size.y), annexStone, lc, &stoneTex, 20.f);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, basePos.z), Vec3(annexW, 3.f, b.size.y), shade3d(roofColor, -14), lc);
        float archCenterX = annexX0 + annexW * 0.5f, archZ = basePos.z + b.size.y + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX - 8.f, 0.f, archZ), 16.f, 22.f, archTex, sf::Color(40, 30, 26));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX, 8.f, archZ), 22.f, glowTex, sf::Color(255, 120, 40, 220));
        Vec3 stackPos(annexX0 + annexW * 0.5f - 7.f, annexWallH, basePos.z + 8.f);
        addBox(out, viewProj, windowSize, eye, stackPos, Vec3(14.f, cab.wallTop * 0.9f, 14.f), shade3d(annexStone, -14), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, stackPos + Vec3(7.f, cab.wallTop * 0.9f + 14.f, 7.f), 20.f, glowTex, sf::Color(150, 150, 154, 140));

        // ---- Iron ingot stack + a raw-ore pile, south yard. ----
        Vec3 ingotPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, ingotPos, Vec3(20.f, 8.f, 14.f), sf::Color(150, 108, 62), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, ingotPos + Vec3(10.f, 8.f, 7.f), 18.f, 10.f, ironBarTex, sf::Color::White);

        sf::Color rockColor(120, 112, 100);
        Vec3 pilePos(annexX0 + 6.f, 0.f, cab.southZ + 6.f);
        for (const auto& off : { sf::Vector2f(0.f, 0.f), sf::Vector2f(7.f, 3.f), sf::Vector2f(2.f, 6.f) }) {
            addBox(out, viewProj, windowSize, eye, pilePos + Vec3(off.x, 0.f, off.y), Vec3(9.f, 7.f, 9.f), rockColor, lc);
        }
    }

    // Blacksmith -- Forge family, tier-3 tools/weapons processor.
    // Widened to 190x80 (column 1 has room, see buildZones()). The forge
    // annex itself is near-verbatim Goldsmith's own (a blacksmith's and a
    // goldsmith's forge are the same basic shape) -- what tells them apart
    // is the outdoor apparatus: a weapon rack + water-quench barrel here
    // instead of a jewelry display counter.
    void addBlacksmithBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& weaponRackTex) {
        sf::Color plankWall(96, 92, 96), roofColor(64, 62, 68), signColor(90, 90, 100); // blacksmith's own 2D accent (80,80,90), a cool steel palette
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);
        sf::Color annexStone = shade3d(sf::Color(118, 114, 108), -6);

        float annexX0 = basePos.x + cab.enclosedW;
        float annexW = 18.f, annexD = 40.f;
        float annexZ0 = basePos.z + (b.size.y - annexD);
        float annexWallH = cab.wallTop * 0.65f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, annexZ0), Vec3(annexW, annexWallH, annexD), annexStone, lc, &stoneTex, 20.f);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, annexZ0), Vec3(annexW, 3.f, annexD), shade3d(roofColor, -14), lc);
        float archCenterX = annexX0 + annexW * 0.5f, archZ = annexZ0 + annexD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX - 6.f, 0.f, archZ), 12.f, 18.f, archTex, sf::Color(50, 40, 36));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX, 6.f, archZ), 14.f, glowTex, sf::Color(255, 140, 50, 190));

        sf::Color anvilColor(58, 56, 58);
        Vec3 anvilPos(annexX0 + annexW * 0.5f - 5.f, 0.f, annexZ0 + annexD - 16.f);
        addBox(out, viewProj, windowSize, eye, anvilPos, Vec3(4.f, 8.f, 4.f), shade3d(anvilColor, -10), lc);
        addBox(out, viewProj, windowSize, eye, anvilPos + Vec3(-3.f, 8.f, -1.f), Vec3(10.f, 3.f, 6.f), anvilColor, lc);

        sf::Color bellowsColor(120, 78, 46);
        Vec3 bellowsPos(annexX0 + annexW * 0.5f - 4.f, 0.f, annexZ0 + 6.f);
        addBox(out, viewProj, windowSize, eye, bellowsPos, Vec3(8.f, 6.f, 10.f), bellowsColor, lc);
        addBox(out, viewProj, windowSize, eye, bellowsPos + Vec3(1.f, 6.f, 1.f), Vec3(6.f, 4.f, 8.f), shade3d(bellowsColor, 14), lc);

        // ---- Weapon rack + water-quench barrel, east of the annex. ----
        float rackX0 = annexX0 + annexW + 10.f;
        Vec3 rackPos(rackX0, 0.f, basePos.z + b.size.y * 0.35f);
        addBox(out, viewProj, windowSize, eye, rackPos, Vec3(3.f, 26.f, 3.f), sf::Color(90, 62, 34), lc);
        addBox(out, viewProj, windowSize, eye, rackPos + Vec3(-9.f, 20.f, -1.f), Vec3(21.f, 3.f, 2.f), sf::Color(90, 62, 34), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(-9.f, 8.f, 0.f), 22.f, 20.f, weaponRackTex, sf::Color::White);

        sf::Color barrelColor(90, 100, 108);
        Vec3 barrelPos(rackX0 + 4.f, 0.f, cab.southZ + 6.f);
        addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(12.f, 16.f, 12.f), barrelColor, lc);
        addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.8f, 14.f, -0.8f), Vec3(13.6f, 2.f, 13.6f), shade3d(barrelColor, 20), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, barrelPos + Vec3(6.f, 16.f, 6.f), 12.f, glowTex, sf::Color(150, 190, 210, 90));
    }

    // Gemshop -- MasonGem family, tier-2 cut-gem processor. Widened to
    // 170x80 (column 3 has plenty of room, see buildZones()). A boutique
    // display counter (Goldsmith's own "wood counter + tinted glass case"
    // recipe) plus a stone cutting wheel and a raw-rock pile -- no forge,
    // cutting gems doesn't need fire.
    void addGemshopBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& gemTrayTex, const sf::Texture& grindWheelTex) {
        sf::Color plankWall(140, 150, 156), roofColor(70, 92, 100), signColor(90, 170, 180); // gemshop's own 2D accent (110,210,220), a cool teal palette
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, 0.42f);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float counterX0 = basePos.x + cab.enclosedW + 6.f;
        float counterAreaW = b.size.x - cab.enclosedW - 6.f;
        float counterW = std::min(counterAreaW * 0.6f, 70.f), counterD = b.size.y * 0.4f;
        Vec3 counterPos(counterX0, 0.f, basePos.z + (b.size.y - counterD) * 0.5f);
        addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(counterW, 14.f, counterD), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, counterPos + Vec3(2.f, 14.f, 2.f), Vec3(counterW - 4.f, 6.f, counterD - 4.f), sf::Color(190, 214, 224, 200), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(counterPos.x + counterW * 0.5f, 20.f, counterPos.z + counterD * 0.5f), 24.f, 14.f, gemTrayTex, sf::Color::White);

        Vec3 wheelPos(counterX0 + counterW + 14.f, 14.f, counterPos.z + counterD * 0.5f);
        addBox(out, viewProj, windowSize, eye, Vec3(wheelPos.x - 2.f, 0.f, wheelPos.z - 2.f), Vec3(4.f, 14.f, 4.f), sf::Color(94, 62, 32), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, wheelPos, 18.f, 18.f, grindWheelTex, sf::Color::White);

        sf::Color rockColor(120, 112, 100);
        Vec3 pilePos(counterX0 + 4.f, 0.f, cab.southZ + 4.f);
        for (const auto& off : { sf::Vector2f(0.f, 0.f), sf::Vector2f(7.f, 3.f) }) {
            addBox(out, viewProj, windowSize, eye, pilePos + Vec3(off.x, 0.f, off.y), Vec3(8.f, 6.f, 8.f), rockColor, lc);
        }
    }

    // Carpenter -- Sawmill family, tier-3 furniture processor. Un-widened
    // (110x80, same column-2 path-spine constraint as Smelter) -- an open
    // workshop bay (posts + roof slab, no walls, the same open-bay
    // convention Sawmill's own saw bay established) fills the lot's
    // remaining east span, holding a workbench.
    void addCarpenterBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& furnitureTex) {
        sf::Color plankWall(150, 116, 72), roofColor(96, 66, 40), signColor(160, 116, 66); // carpenter's own 2D accent (170,120,70)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.75f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -10), lc);

        sf::Color benchColor(120, 86, 50);
        Vec3 benchPos(bayX0 + bayW * 0.5f - 16.f, 0.f, basePos.z + b.size.y * 0.5f - 7.f);
        addBandedBox(out, viewProj, windowSize, eye, benchPos, Vec3(32.f, 12.f, 14.f), benchColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, benchPos + Vec3(16.f, 12.f, 7.f), 22.f, 14.f, furnitureTex, sf::Color::White);

        // ---- Plank stack + sawdust pile, south yard. ----
        sf::Color plankStackColor(180, 148, 96);
        Vec3 stackPos(basePos.x + 10.f, 0.f, cab.southZ + 4.f);
        for (int i = 0; i < 4; ++i) {
            addBox(out, viewProj, windowSize, eye, stackPos + Vec3(0.f, static_cast<float>(i) * 3.f, 0.f), Vec3(26.f, 2.5f, 10.f), i % 2 == 0 ? plankStackColor : shade3d(plankStackColor, -14), lc);
        }
        sf::Color sawdustColor(214, 190, 140);
        Vec3 dustPos(bayX0 + 6.f, 0.f, cab.southZ + 2.f);
        addBox(out, viewProj, windowSize, eye, dustPos, Vec3(14.f, 3.f, 14.f), sawdustColor, lc);
    }

    // Tailor -- Fiber family, tier-3 fine-clothing processor. Widened to
    // 170x80 (column 3, same room as Gemshop). No 2nd walled volume --
    // unlike the fire-needing Forge-family annexes, a tailor's own
    // workspace doesn't need one -- just a dress form on a small platform
    // flanked by fabric bolts (`yarnTex` reused outright from Textile
    // Mill, a bolt display isn't different enough from spooled yarn to
    // earn its own decal).
    void addTailorBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& dressFormTex, const sf::Texture& yarnTex) {
        sf::Color plankWall(150, 128, 140), roofColor(92, 66, 78), signColor(190, 140, 170); // tailor's own 2D accent (190,140,170)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, 0.42f);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float dispX0 = basePos.x + cab.enclosedW + 10.f;
        float dispAreaW = b.size.x - cab.enclosedW - 10.f;
        Vec3 platformPos(dispX0 + dispAreaW * 0.3f, 0.f, basePos.z + b.size.y * 0.3f);
        addBandedBox(out, viewProj, windowSize, eye, platformPos - Vec3(10.f, 0.f, 10.f), Vec3(20.f, 4.f, 20.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, platformPos + Vec3(0.f, 4.f, 0.f), 20.f, 34.f, dressFormTex, sf::Color::White);

        for (float fx : { -1.f, 1.f }) {
            Vec3 rollPos(dispX0 + dispAreaW * 0.5f + fx * 22.f, 0.f, basePos.z + b.size.y * 0.62f);
            addBox(out, viewProj, windowSize, eye, rollPos, Vec3(8.f, 16.f, 8.f), sf::Color(70, 60, 66), lc);
            addBillboard(out, viewProj, windowSize, billboardRight, rollPos + Vec3(4.f, 16.f, 4.f), 14.f, 10.f, yarnTex, sf::Color::White);
        }

        // A folded-fabric stack, south yard.
        Vec3 foldPos(basePos.x + 10.f, 0.f, cab.southZ + 4.f);
        const sf::Color foldColors[] = { sf::Color(150, 90, 110), sf::Color(90, 110, 150), sf::Color(180, 160, 90) };
        for (int i = 0; i < 3; ++i) {
            addBox(out, viewProj, windowSize, eye, foldPos + Vec3(0.f, static_cast<float>(i) * 3.5f, 0.f), Vec3(18.f, 3.f, 12.f), foldColors[i], lc);
        }
    }

    // ---- Zone 3 (Valley District) finishing batch, 2026-08-11 ("其他的
    //你可以开始设计了" -- go ahead and design the rest): Apothecary/
    // Alchemist/Winery are Brewery family (isBreweryId in GameWorld.cpp --
    // liquids simmered/fermented/distilled into something bottled), Jeweler
    // is MasonGem family (like Mason/Gemshop). All 4 use the shared
    // `addWorkshopCabin` helper. ----

    // Apothecary -- Brewery family, tier-2 herbal-tincture processor.
    // Widened to 170x80. Annex holds a simmering herbal brew (green glow,
    // not fire-orange) and a hanging dried-herb bundle (`herbTuftTex`
    // reused, tinted brown); outdoor a mortar-and-pestle and a
    // green-tinted bottle rack.
    void addApothecaryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& archTex, const sf::Texture& herbTuftTex, const sf::Texture& bottleRackTex) {
        sf::Color plankWall(122, 140, 108), roofColor(72, 90, 66), signColor(96, 138, 82); // apothecary's own 2D accent (110,160,90)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);
        sf::Color annexStone = shade3d(sf::Color(118, 114, 108), -6);

        float annexX0 = basePos.x + cab.enclosedW;
        float annexW = 20.f, annexD = 42.f;
        float annexZ0 = basePos.z + (b.size.y - annexD);
        float annexWallH = cab.wallTop * 0.68f;
        addBandedBox(out, viewProj, windowSize, eye, Vec3(annexX0, 0.f, annexZ0), Vec3(annexW, annexWallH, annexD), annexStone, lc, &stoneTex, 20.f);
        addBox(out, viewProj, windowSize, eye, Vec3(annexX0, annexWallH, annexZ0), Vec3(annexW, 3.f, annexD), shade3d(roofColor, -14), lc);
        float archCenterX = annexX0 + annexW * 0.5f, archZ = annexZ0 + annexD + 0.5f;
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX - 6.f, 0.f, archZ), 12.f, 18.f, archTex, sf::Color(40, 50, 40));
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(archCenterX, 8.f, archZ), 16.f, glowTex, sf::Color(120, 220, 130, 190));

        // A dried-herb bundle, hanging under the annex's own eave.
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(annexX0 + 4.f, annexWallH - 4.f, annexZ0 + 6.f), 14.f, 16.f, herbTuftTex, sf::Color(150, 120, 70));

        // A mortar-and-pestle, south yard.
        sf::Color mortarColor(150, 148, 140);
        Vec3 mortarPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBox(out, viewProj, windowSize, eye, mortarPos, Vec3(10.f, 6.f, 10.f), mortarColor, lc);
        addBox(out, viewProj, windowSize, eye, mortarPos + Vec3(6.f, 5.f, 3.f), Vec3(2.5f, 8.f, 2.5f), shade3d(mortarColor, -16), lc);

        // Green-tinted bottle rack, east of the mortar.
        Vec3 rackPos(mortarPos.x + 18.f, 0.f, cab.southZ + 2.f);
        addBandedBox(out, viewProj, windowSize, eye, rackPos, Vec3(20.f, 4.f, 10.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(10.f, 4.f, 5.f), 18.f, 14.f, bottleRackTex, sf::Color(150, 200, 130));
    }

    // Alchemist -- Brewery family, tier-3 potion processor. Un-widened
    // (only 60 units clear to the zone's own east tree wall) -- annex
    // fills the lot's remaining east span, same convention Smelter/
    // Carpenter used in Zone 2.
    //
    // 2026-08-11 follow-up ("这个酒庄可以把右边的墙打掉让可以看到里面大
    // 小姐...炼金坊同理" -- knock the annex's own wall down so the inside
    // is actually visible, same for Alchemist): the annex was a fully
    // enclosed `addBandedBox` with just a flat archway BILLBOARD glued to
    // its south face standing in for an opening -- the wall behind that
    // decal was still a solid opaque box, so the alembic/press/barrels
    // "inside" were sitting in near-total shadow of their own walls,
    // barely readable. Switched to the same open post-and-roof-slab bay
    // Carpenter's own workshop already uses (no wall boxes at all) --
    // genuinely open on every side instead of one fake doorway.
    void addAlchemistBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& bottleRackTex) {
        sf::Color plankWall(112, 98, 130), roofColor(64, 54, 90), signColor(132, 88, 186); // alchemist's own 2D accent (140,90,190)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.7f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        // The alembic (2 stacked tinted glass-look boxes, the same "fake a
        // curved vessel with flat shapes" trick this file leans on
        // everywhere) bubbling with a magical glow instead of fire.
        sf::Color glassColor(150, 190, 210, 190);
        Vec3 flaskPos(bayX0 + bayW * 0.5f - 5.f, 0.f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, flaskPos, Vec3(10.f, 14.f, 10.f), glassColor, lc);
        addBox(out, viewProj, windowSize, eye, flaskPos + Vec3(2.5f, 14.f, 2.5f), Vec3(5.f, 8.f, 5.f), shade3d(glassColor, -20), lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, flaskPos + Vec3(5.f, 20.f, 5.f), 18.f, glowTex, sf::Color(150, 90, 220, 210));

        // A small shelf of spare vials, beside the alembic -- now that the
        // bay is genuinely open, worth a 2nd prop to fill it.
        Vec3 shelfPos(bayX0 + 6.f, 0.f, basePos.z + 6.f);
        addBandedBox(out, viewProj, windowSize, eye, shelfPos, Vec3(16.f, 4.f, 8.f), sf::Color(90, 62, 34), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, shelfPos + Vec3(8.f, 4.f, 4.f), 14.f, 10.f, bottleRackTex, sf::Color(160, 130, 210));

        // Purple-tinted bottle rack, south yard.
        Vec3 rackPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, rackPos, Vec3(20.f, 4.f, 10.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(10.f, 4.f, 5.f), 18.f, 14.f, bottleRackTex, sf::Color(160, 130, 210));
    }

    // Winery -- Brewery family, tier-2 wine processor. Un-widened (only 60
    // units clear to the zone's own east tree wall) -- annex fills the
    // lot's remaining east span holding a wine press (near-verbatim
    // Preserve's own screw press over a catch-barrel, wine and jam are
    // both "crush the fruit" businesses) instead of a cauldron/still.
    //
    // 2026-08-11 follow-up ("这个酒庄可以把右边的墙打掉让可以看到里面大
    // 小姐" -- open the annex wall up): same enclosed-box-plus-fake-arch
    // problem Alchemist's own comment above describes, same fix -- an
    // open post-and-roof bay instead of walls, so the press/barrels are
    // actually visible rather than sitting behind a solid wall with only a
    // flat archway decal hinting at what's inside.
    void addWineryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& pressWheelTex, const sf::Texture& bottleRackTex) {
        sf::Color plankWall(150, 108, 104), roofColor(90, 56, 54), signColor(120, 50, 60); // winery's own 2D accent (120,50,60)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.65f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        // ---- Wine press, bay center -- 2026-08-11 follow-up ("压酒机看
        // 起来不明显" -- the press doesn't read clearly): it used to be
        // just a thin 6-wide post topped with a 16-unit wheel, squeezed
        // between 2 barrels that were each bigger than it -- easy to miss
        // entirely. Now built around a real press BASKET (a wide banded
        // box, wood-slat colored, bigger than either barrel) with a plate
        // on top, so the post+wheel reads as sitting on top of an actual
        // press mechanism instead of floating alone, and the wheel itself
        // is bigger (22, was 16). ----
        sf::Color basketColor(150, 112, 68), pressPostColor(94, 62, 32);
        Vec3 basketPos(bayX0 + bayW * 0.5f - 9.f, 0.f, basePos.z + b.size.y * 0.55f);
        addBandedBox(out, viewProj, windowSize, eye, basketPos, Vec3(18.f, 12.f, 18.f), basketColor, lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, basketPos + Vec3(-1.f, 12.f, -1.f), Vec3(20.f, 2.f, 20.f), shade3d(basketColor, -20), lc);
        Vec3 pressPostPos(basketPos.x + 6.f, 14.f, basketPos.z + 6.f);
        addBox(out, viewProj, windowSize, eye, pressPostPos, Vec3(6.f, bayH * 0.75f, 6.f), pressPostColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, pressPostPos + Vec3(3.f, bayH * 0.75f, 3.f), 22.f, 22.f, pressWheelTex, sf::Color::White);

        sf::Color barrelColor(120, 86, 50);
        for (float bx : { -22.f, 22.f }) {
            Vec3 barrelPos(bayX0 + bayW * 0.5f + bx - 5.f, 0.f, basePos.z + b.size.y * 0.25f);
            addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(10.f, 14.f, 10.f), barrelColor, lc);
            addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.6f, 12.f, -0.6f), Vec3(11.2f, 2.f, 11.2f), shade3d(barrelColor, -18), lc);
        }

        // A crate of empty wine bottles, beside the press -- now that the
        // bay is genuinely open, worth a 2nd prop to fill it.
        Vec3 crateBottlesPos(bayX0 + 6.f, 0.f, basePos.z + 6.f);
        addBandedBox(out, viewProj, windowSize, eye, crateBottlesPos, Vec3(16.f, 4.f, 8.f), sf::Color(90, 62, 34), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, crateBottlesPos + Vec3(8.f, 4.f, 4.f), 14.f, 10.f, bottleRackTex, sf::Color(170, 70, 80));

        // Dark-red-tinted bottle rack, south yard.
        Vec3 rackPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, rackPos, Vec3(20.f, 4.f, 10.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(10.f, 4.f, 5.f), 18.f, 14.f, bottleRackTex, sf::Color(170, 70, 80));
    }

    // Jeweler -- MasonGem family, tier-3 finished-jewelry processor.
    // Widened to 170x80. Gemshop's own counter recipe, plus a small
    // canopy awning over it (a boutique's own "storefront" read) and
    // `jewelryTex` instead of loose gems -- a jeweler sells finished
    // pieces, its raw-gem sibling doesn't.
    void addJewelerBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& jewelryTex) {
        sf::Color plankWall(148, 128, 142), roofColor(90, 66, 96), signColor(200, 130, 168); // jeweler's own 2D accent (220,140,180)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, 0.42f);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float counterX0 = basePos.x + cab.enclosedW + 6.f;
        float counterAreaW = b.size.x - cab.enclosedW - 6.f;
        float counterW = std::min(counterAreaW * 0.7f, 76.f), counterD = b.size.y * 0.42f;
        Vec3 counterPos(counterX0, 0.f, basePos.z + (b.size.y - counterD) * 0.5f);
        addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(counterW, 14.f, counterD), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, counterPos + Vec3(2.f, 14.f, 2.f), Vec3(counterW - 4.f, 6.f, counterD - 4.f), sf::Color(220, 200, 220, 200), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(counterPos.x + counterW * 0.5f, 20.f, counterPos.z + counterD * 0.5f), 24.f, 14.f, jewelryTex, sf::Color::White);

        // A small canopy awning over the counter -- 4 thin posts + a flat
        // slab roof, the reference's own "boutique storefront" read.
        sf::Color postColor(150, 130, 145), canopyColor(200, 130, 168);
        float canopyH = 34.f;
        for (float cx : { counterPos.x + 4.f, counterPos.x + counterW - 4.f }) {
            for (float cz : { counterPos.z + 4.f, counterPos.z + counterD - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(cx, 14.f, cz), Vec3(2.5f, canopyH - 14.f, 2.5f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, counterPos + Vec3(-3.f, canopyH, -3.f), Vec3(counterW + 6.f, 3.f, counterD + 6.f), canopyColor, lc);

        // A small raw-gem pile, south of the counter -- even a finished-
        // jewelry shop keeps a bit of uncut stock on hand.
        sf::Color rockColor(120, 112, 100);
        Vec3 pilePos(counterX0 + 4.f, 0.f, cab.southZ + 4.f);
        for (const auto& off : { sf::Vector2f(0.f, 0.f), sf::Vector2f(7.f, 3.f) }) {
            addBox(out, viewProj, windowSize, eye, pilePos + Vec3(off.x, 0.f, off.y), Vec3(8.f, 6.f, 8.f), rockColor, lc);
        }
    }

    // ---- Zone 5 (Highlands District) batch, 2026-08-11 ("其他的屋子可以
    // 继续进行了" -- carry on with the rest): Creamery/Meadery are Brewery
    // family (like Winery/Alchemist), Tannery/Linen Mill are Fiber family
    // (like Textile/Tailor). All 4 use the open post-and-roof bay
    // (`addWorkshopCabin` + posts/roof-slab, no wall boxes) up front this
    // time -- Winery/Alchemist's own "enclosed box + fake archway billboard
    // hid everything inside" mistake is now a known lesson, not something
    // to re-learn per building. ----

    // Creamery -- Brewery family, tier-2 milk->cheese processor. A butter
    // churn (barrel + `pressWheelTex` crank, same shape Winery's own press
    // post uses) and a stack of cheese wheels.
    void addCreameryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& pressWheelTex, const sf::Texture& bottleRackTex) {
        sf::Color plankWall(150, 140, 118), roofColor(96, 84, 64), signColor(140, 128, 104); // creamery's own 2D accent (235,225,200), muted for a sign
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.65f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        sf::Color churnColor(150, 132, 96);
        Vec3 churnPos(bayX0 + bayW * 0.3f, 0.f, basePos.z + b.size.y * 0.5f - 7.f);
        addBox(out, viewProj, windowSize, eye, churnPos, Vec3(14.f, 20.f, 14.f), churnColor, lc);
        addBox(out, viewProj, windowSize, eye, churnPos + Vec3(4.f, 20.f, 4.f), Vec3(6.f, 3.f, 6.f), shade3d(churnColor, -16), lc);
        Vec3 crankPost(churnPos.x + 7.f, 23.f, churnPos.z + 7.f);
        addBox(out, viewProj, windowSize, eye, crankPost, Vec3(3.f, 8.f, 3.f), sf::Color(94, 62, 32), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, crankPost + Vec3(1.5f, 8.f, 1.5f), 14.f, 14.f, pressWheelTex, sf::Color::White);

        sf::Color cheeseColor(230, 200, 110);
        Vec3 cheesePos(bayX0 + bayW * 0.65f, 0.f, basePos.z + b.size.y * 0.4f);
        for (int i = 0; i < 3; ++i) {
            addBox(out, viewProj, windowSize, eye, cheesePos + Vec3(0.f, static_cast<float>(i) * 7.f, 0.f), Vec3(16.f, 7.f, 16.f), (i % 2 == 0) ? cheeseColor : shade3d(cheeseColor, -10), lc);
        }

        Vec3 rackPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, rackPos, Vec3(20.f, 4.f, 10.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(10.f, 4.f, 5.f), 18.f, 14.f, bottleRackTex, sf::Color(235, 232, 224));
    }

    // Meadery -- Brewery family, tier-2 honey->mead processor. Fermentation
    // barrels (same pair Winery's own press annex uses) plus a honeycomb
    // stack (Beehive's own striped-box hive, reused at a smaller scale).
    void addMeaderyBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& bottleRackTex) {
        sf::Color plankWall(150, 128, 92), roofColor(96, 74, 44), signColor(180, 132, 44); // meadery's own 2D accent (220,160,50)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.65f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        sf::Color barrelColor(120, 86, 50);
        for (float bx : { 0.f, 22.f }) {
            Vec3 barrelPos(bayX0 + bayW * 0.3f + bx, 0.f, basePos.z + b.size.y * 0.6f);
            addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(10.f, 14.f, 10.f), barrelColor, lc);
            addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.6f, 12.f, -0.6f), Vec3(11.2f, 2.f, 11.2f), shade3d(barrelColor, -18), lc);
        }

        Vec3 combPos(bayX0 + bayW * 0.68f, 0.f, basePos.z + b.size.y * 0.3f);
        for (int i = 0; i < 2; ++i) {
            sf::Color band = (i % 2 == 0) ? sf::Color(230, 180, 90) : sf::Color(200, 148, 68);
            addBox(out, viewProj, windowSize, eye, combPos + Vec3(0.f, static_cast<float>(i) * 6.f, 0.f), Vec3(13.f, 6.f, 13.f), band, lc);
        }
        addGlowBillboard(out, viewProj, windowSize, billboardRight, combPos + Vec3(6.5f, 16.f, 6.5f), 14.f, glowTex, sf::Color(255, 220, 110, 140));

        Vec3 rackPos(basePos.x + 8.f, 0.f, cab.southZ + 4.f);
        addBandedBox(out, viewProj, windowSize, eye, rackPos, Vec3(20.f, 4.f, 10.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, rackPos + Vec3(10.f, 4.f, 5.f), 18.f, 14.f, bottleRackTex, sf::Color(220, 168, 70));
    }

    // Tannery -- Fiber family, tier-2 hide->leather processor. Hides
    // stretched on drying frames (`peltTex`, bigger scale than Trapper's
    // own hanging pelts) plus a tanning vat and a stack of finished
    // leather.
    void addTanneryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& peltTex) {
        sf::Color plankWall(138, 108, 88), roofColor(84, 62, 48), signColor(140, 100, 70); // tannery's own 2D accent (140,100,70)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.65f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        // 2 stretching frames, each holding a big hide.
        for (float fx : { 0.f, 32.f }) {
            float frameX = bayX0 + 14.f + fx, frameZ0 = basePos.z + 6.f, frameZ1 = basePos.z + b.size.y - 6.f;
            addBox(out, viewProj, windowSize, eye, Vec3(frameX - 2.f, 0.f, frameZ0), Vec3(4.f, 24.f, 4.f), postColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(frameX - 2.f, 0.f, frameZ1 - 4.f), Vec3(4.f, 24.f, 4.f), postColor, lc);
            addBox(out, viewProj, windowSize, eye, Vec3(frameX - 2.f, 22.f, frameZ0), Vec3(4.f, 3.f, frameZ1 - frameZ0), postColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, Vec3(frameX, 12.f, (frameZ0 + frameZ1) * 0.5f), 20.f, 26.f, peltTex, sf::Color::White);
        }

        sf::Color vatColor(90, 90, 84);
        Vec3 vatPos(bayX0 + 6.f, 0.f, basePos.z + b.size.y - 20.f);
        addBox(out, viewProj, windowSize, eye, vatPos, Vec3(16.f, 10.f, 16.f), vatColor, lc);
        addBox(out, viewProj, windowSize, eye, vatPos + Vec3(2.f, 9.f, 2.f), Vec3(12.f, 1.5f, 12.f), sf::Color(90, 70, 50), lc);

        sf::Color leatherColor(140, 92, 58);
        Vec3 stackPos(basePos.x + 10.f, 0.f, cab.southZ + 4.f);
        for (int i = 0; i < 4; ++i) {
            addBox(out, viewProj, windowSize, eye, stackPos + Vec3(0.f, static_cast<float>(i) * 3.f, 0.f), Vec3(20.f, 2.5f, 12.f), (i % 2 == 0) ? leatherColor : shade3d(leatherColor, -14), lc);
        }
    }

    // Linen Mill -- Fiber family, tier-2 flax->linen processor. A spinning
    // wheel (`pressWheelTex` reused as a plain wheel decal -- a spinning
    // wheel and a press crank are both "wooden wheel on a post" shapes)
    // plus stacked linen bolts (`yarnTex` reused from Textile Mill).
    void addLinenMillBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& pressWheelTex, const sf::Texture& yarnTex) {
        sf::Color plankWall(160, 158, 142), roofColor(100, 96, 84), signColor(180, 176, 158); // linen mill's own 2D accent (220,215,195)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.65f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        Vec3 wheelPostPos(bayX0 + bayW * 0.35f, 0.f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, wheelPostPos - Vec3(2.f, 0.f, 2.f), Vec3(4.f, 16.f, 4.f), sf::Color(94, 62, 32), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, wheelPostPos + Vec3(0.f, 16.f, 0.f), 22.f, 22.f, pressWheelTex, sf::Color::White);

        sf::Color boltColor(200, 196, 178);
        for (float lx : { -1.f, 1.f }) {
            Vec3 boltPos(bayX0 + bayW * 0.68f + lx * 10.f, 0.f, basePos.z + b.size.y * 0.35f);
            addBox(out, viewProj, windowSize, eye, boltPos, Vec3(8.f, 16.f, 8.f), boltColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, boltPos + Vec3(4.f, 16.f, 4.f), 14.f, 10.f, yarnTex, sf::Color::White);
        }

        // A folded-linen stack, south yard.
        Vec3 foldPos(basePos.x + 10.f, 0.f, cab.southZ + 4.f);
        for (int i = 0; i < 3; ++i) {
            addBox(out, viewProj, windowSize, eye, foldPos + Vec3(0.f, static_cast<float>(i) * 3.5f, 0.f), Vec3(18.f, 3.f, 12.f), (i % 2 == 0) ? boltColor : shade3d(boltColor, -14), lc);
        }
    }

    // ---- Stall family, 2026-08-11 ("继续进行" -- carry on) -- first 2 of
    // isStallId's 5 businesses, using the new shared `addMarketStallShell`
    // above (mirrors the 2D world's own drawStallShape: an open awning +
    // counter, no walls at all, unlike every Workshop-family building so
    // far). ----

    // Teahouse -- Stall family, tier-2. A teapot + cups (`teapotTex`) on
    // the counter, steam rising, plus a couple of tea crates beside it.
    void addTeahouseBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& teapotTex, const sf::Texture& teaBushTex) {
        auto stall = addMarketStallShell(out, viewProj, windowSize, eye, b, lc, sf::Color(120, 150, 90), sf::Color(232, 228, 214)); // teahouse's own 2D accent (120,150,90)

        Vec3 potPos(stall.counterPos.x + stall.counterW * 0.5f, 18.f, stall.counterPos.z + stall.counterD * 0.5f);
        addBillboard(out, viewProj, windowSize, billboardRight, potPos, 26.f, 16.f, teapotTex, sf::Color::White);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, potPos + Vec3(-6.f, 12.f, 0.f), 12.f, glowTex, sf::Color(230, 230, 235, 100));

        sf::Color crateColor(150, 108, 62);
        for (float cx : { stall.counterPos.x + 4.f, stall.counterPos.x + stall.counterW - 16.f }) {
            Vec3 cratePos(cx, 0.f, stall.counterPos.z + stall.counterD + 4.f);
            addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(12.f, 9.f, 10.f), crateColor, lc, nullptr, 40.f, true);
            addBillboard(out, viewProj, windowSize, billboardRight, cratePos + Vec3(6.f, 9.f, 5.f), 12.f, 9.f, teaBushTex, sf::Color(70, 116, 58));
        }
    }

    // Country Gift Basket -- Stall family, tier-3, the multi-input recipe
    // sourced from Creamery/Meadery/Teahouse within this same district
    // (see buildZones()'s own comment on this business). A big basket on
    // the counter with cheese, honey, and tea all poking out of it --
    // reusing this file's own existing decals (`bottleRackTex`,
    // `teaBushTex`) rather than baking a bespoke basket-contents sprite.
    void addGiftBasketBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight,
        const sf::Texture& bottleRackTex, const sf::Texture& teaBushTex) {
        auto stall = addMarketStallShell(out, viewProj, windowSize, eye, b, lc, sf::Color(210, 160, 200), sf::Color(232, 228, 214)); // giftbasket's own 2D accent (210,160,200)

        sf::Color basketColor(150, 108, 62), cheeseColor(230, 200, 110);
        Vec3 basketPos(stall.counterPos.x + stall.counterW * 0.5f - 10.f, 18.f, stall.counterPos.z + stall.counterD * 0.5f - 8.f);
        addBandedBox(out, viewProj, windowSize, eye, basketPos, Vec3(20.f, 11.f, 16.f), basketColor, lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, basketPos + Vec3(2.f, 11.f, 2.f), Vec3(8.f, 6.f, 8.f), cheeseColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, basketPos + Vec3(15.f, 11.f, 10.f), 14.f, 10.f, bottleRackTex, sf::Color(220, 168, 70));
        addBillboard(out, viewProj, windowSize, billboardRight, basketPos + Vec3(8.f, 14.f, 14.f), 14.f, 10.f, teaBushTex, sf::Color(70, 116, 58));
    }

    // ============================================================
    // 2026-08-11 final batch ("剩下一次过都来吧" -- get the rest done in
    // one go): every business still left across Zone 4 (Harbor District),
    // Zone 6 (Market Row), and Zone 7 (Fisher's Isle). None of these are
    // widened -- each design fits the plain 110x80 `bSize` the same way
    // Sawmill/Mason/Textile did before Bakery's own widen precedent, so
    // this whole batch needed zero position/size edits in
    // buildZones() -- just the dispatch wiring below.
    // ============================================================

    // ---- Zone 4: Harbor District ----

    // Sea Salt -- Field family, tier-1. Shallow evaporation pools with a
    // salt crust mound in each, instead of a farmed-crop row.
    void addSeaSaltProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(196, 190, 168), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        sf::Color poolColor(224, 222, 210), saltColor(245, 245, 240);
        const sf::Vector2f pools[] = { {0.22f, 0.3f}, {0.5f, 0.32f}, {0.78f, 0.3f}, {0.22f, 0.68f}, {0.5f, 0.66f}, {0.78f, 0.68f} };
        for (const auto& pp : pools) {
            Vec3 p(b.position.x + b.size.x * pp.x, 0.f, b.position.y + b.size.y * pp.y);
            addBox(out, viewProj, windowSize, eye, p - Vec3(11.f, 0.f, 11.f), Vec3(22.f, 1.5f, 22.f), poolColor, lc);
            addBox(out, viewProj, windowSize, eye, p - Vec3(7.f, -1.5f, 7.f), Vec3(14.f, 3.f, 14.f), saltColor, lc);
        }
    }

    // Pearl Farm -- Field family, tier-1. The plot itself reads as open
    // water (blue ground fill) with floating oyster-cage rafts instead of
    // dry farmland.
    void addPearlFarmProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& pearlTex) {
        addGroundQuad(out, viewProj, windowSize, eye, b.position.x, b.position.y, b.size.x, b.size.y, 0.6f, sf::Color(70, 130, 160), lc);
        addPlotBorder(out, viewProj, windowSize, eye, b.position, b.size, sf::Color(25, 20, 15), lc);
        sf::Color cageColor(90, 80, 70);
        const sf::Vector2f cages[] = { {0.25f, 0.35f}, {0.55f, 0.45f}, {0.75f, 0.3f}, {0.4f, 0.7f} };
        for (const auto& cp : cages) {
            Vec3 p(b.position.x + b.size.x * cp.x, 0.f, b.position.y + b.size.y * cp.y);
            addBox(out, viewProj, windowSize, eye, p - Vec3(7.f, 0.f, 7.f), Vec3(14.f, 3.f, 14.f), cageColor, lc);
            addBillboard(out, viewProj, windowSize, billboardRight, p + Vec3(0.f, 4.f, 0.f), 14.f, 10.f, pearlTex, sf::Color::White);
        }
    }

    // Fishing Dock -- Dock family, tier-1 (see addDockShell's own header
    // comment on why even a raw-tier Dock business gets the deck+water
    // shell, unlike every other tier-1 producer). A net-drying rack and a
    // basket of the day's catch (`fishTex`).
    void addFishingProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fishTex) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color postColor(90, 62, 34);
        Vec3 rackBase(b.position.x + b.size.x * 0.2f, 0.f, b.position.y + 8.f);
        addBox(out, viewProj, windowSize, eye, rackBase, Vec3(4.f, 26.f, 4.f), postColor, lc);
        addBox(out, viewProj, windowSize, eye, rackBase + Vec3(30.f, 0.f, 0.f), Vec3(4.f, 26.f, 4.f), postColor, lc);
        addBox(out, viewProj, windowSize, eye, rackBase + Vec3(0.f, 24.f, 0.f), Vec3(34.f, 3.f, 4.f), postColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, rackBase + Vec3(17.f, 12.f, 2.f), 26.f, 18.f, fishTex, sf::Color::White);

        Vec3 basketPos(b.position.x + b.size.x * 0.62f, 0.f, b.position.y + dock.deckD * 0.55f);
        addBandedBox(out, viewProj, windowSize, eye, basketPos, Vec3(14.f, 10.f, 14.f), sf::Color(150, 108, 62), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, basketPos + Vec3(7.f, 10.f, 7.f), 16.f, 12.f, fishTex, sf::Color::White);
    }

    // Shipyard -- Dock family, tier-2. A ship's hull skeleton (keel + rib
    // posts) under construction, the classic "half-built ship" read.
    void addShipyardBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color hullColor(160, 120, 80), ribColor(120, 86, 50);
        Vec3 keelPos(b.position.x + b.size.x * 0.15f, 0.f, b.position.y + 8.f);
        float keelLen = b.size.x * 0.7f;
        addBox(out, viewProj, windowSize, eye, keelPos, Vec3(keelLen, 4.f, dock.deckD - 14.f), hullColor, lc);
        for (float t : { 0.15f, 0.4f, 0.65f, 0.9f }) {
            Vec3 rp(keelPos.x + keelLen * t, 4.f, keelPos.z + 6.f);
            addBox(out, viewProj, windowSize, eye, rp, Vec3(3.f, 22.f, 3.f), ribColor, lc);
            addBox(out, viewProj, windowSize, eye, rp + Vec3(-9.f, 18.f, 0.f), Vec3(12.f, 3.f, 3.f), ribColor, lc);
        }
        Vec3 cratePos(b.position.x + 8.f, 0.f, dock.waterZ0 - 16.f);
        addBandedBox(out, viewProj, windowSize, eye, cratePos, Vec3(14.f, 10.f, 14.f), sf::Color(150, 108, 62), lc, nullptr, 40.f, true);
    }

    // Port -- Dock family, tier-3. Stacked cargo crates/barrels and a
    // signal mast flying a flag in the business's own accent color.
    void addPortBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color crateColor(150, 108, 62), barrelColor(120, 86, 50);
        for (const auto& cp : { sf::Vector2f(0.2f, 0.3f), sf::Vector2f(0.36f, 0.36f), sf::Vector2f(0.2f, 0.5f) }) {
            Vec3 p(b.position.x + b.size.x * cp.x, 0.f, b.position.y + dock.deckD * cp.y);
            addBandedBox(out, viewProj, windowSize, eye, p, Vec3(14.f, 12.f, 14.f), crateColor, lc, nullptr, 40.f, true);
        }
        Vec3 barrelPos(b.position.x + b.size.x * 0.6f, 0.f, b.position.y + dock.deckD * 0.4f);
        addBox(out, viewProj, windowSize, eye, barrelPos, Vec3(10.f, 14.f, 10.f), barrelColor, lc);
        addBox(out, viewProj, windowSize, eye, barrelPos + Vec3(-0.6f, 12.f, -0.6f), Vec3(11.2f, 2.f, 11.2f), shade3d(barrelColor, -18), lc);

        Vec3 mastPos(b.position.x + b.size.x * 0.75f, 0.f, b.position.y + dock.deckD * 0.55f);
        addBox(out, viewProj, windowSize, eye, mastPos, Vec3(4.f, 40.f, 4.f), sf::Color(94, 62, 32), lc);
        addBox(out, viewProj, windowSize, eye, mastPos + Vec3(4.f, 32.f, -1.f), Vec3(14.f, 8.f, 1.f), sf::Color(90, 160, 200), lc); // port's own 2D accent (90,160,200)
    }

    // Pearl Atelier -- MasonGem family, tier-2 (like Mason/Gemshop/
    // Jeweler). Gemshop's own counter recipe, `pearlTex` instead of loose
    // gems/gold.
    void addPearlAtelierBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& pearlTex) {
        sf::Color plankWall(150, 155, 160), roofColor(84, 96, 104), signColor(190, 195, 200); // pearlatelier's own 2D accent (225,225,230)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, 0.42f);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float counterX0 = basePos.x + cab.enclosedW + 6.f;
        float counterAreaW = b.size.x - cab.enclosedW - 6.f;
        float counterW = std::min(counterAreaW * 0.7f, 70.f), counterD = b.size.y * 0.4f;
        Vec3 counterPos(counterX0, 0.f, basePos.z + (b.size.y - counterD) * 0.5f);
        addBandedBox(out, viewProj, windowSize, eye, counterPos, Vec3(counterW, 14.f, counterD), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, counterPos + Vec3(2.f, 14.f, 2.f), Vec3(counterW - 4.f, 6.f, counterD - 4.f), sf::Color(210, 225, 230, 200), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(counterPos.x + counterW * 0.5f, 20.f, counterPos.z + counterD * 0.5f), 20.f, 14.f, pearlTex, sf::Color::White);
    }

    // ---- Zone 6: Market Row (Oven family x7, Stall family x2) ----

    // Jam Kitchen -- Oven family, tier-2. A smaller-scale sibling of
    // Preserve's own jam operation -- same `jamJarsTex`.
    void addJamKitchenBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& jamJarsTex) {
        sf::Color plankWall(148, 96, 92), roofColor(92, 54, 52), signColor(150, 60, 58); // jamkitchen's own 2D accent (200,90,110)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 150, 60, 200));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 18.f, 12.f, jamJarsTex, sf::Color::White);
    }

    // Pie Shop -- Oven family, tier-2. `pieTex` on the display table.
    void addPieShopBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& pieTex) {
        sf::Color plankWall(160, 130, 90), roofColor(100, 70, 40), signColor(180, 140, 80); // pieshop's own 2D accent (210,170,110)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 150, 60, 200));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 18.f, 12.f, pieTex, sf::Color::White);
    }

    // Roast Stand -- Oven family, tier-2. A meat roast on a spit
    // (`roastTex`) right over the hearth's own fire instead of a table off
    // to the side -- a roast stand's whole point is the open flame.
    void addRoastStandBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& roastTex) {
        sf::Color plankWall(140, 100, 80), roofColor(90, 60, 40), signColor(180, 110, 60); // roaststand's own 2D accent (200,120,60)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 120, 40, 220));
        Vec3 hearthCenter(shell.bayX0 + shell.bayW * 0.32f, 0.f, b.position.y + b.size.y * 0.5f - 9.f);
        addBillboard(out, viewProj, windowSize, billboardRight, hearthCenter + Vec3(9.f, 13.f, 9.f), 22.f, 16.f, roastTex, sf::Color::White);
    }

    // Pickling House -- Oven family, tier-2. A pale steam instead of the
    // usual firebox orange (pickling is brine, not an open flame), and a
    // green-tinted `bottleRackTex` for pickle jars.
    void addPicklingHouseBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& bottleRackTex) {
        sf::Color plankWall(130, 150, 110), roofColor(80, 100, 70), signColor(110, 140, 76); // picklinghouse's own 2D accent (140,170,90)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(220, 230, 220, 130));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 18.f, 14.f, bottleRackTex, sf::Color(150, 190, 110));
    }

    // Honey Refinery -- Oven family, tier-2. Gold-tinted `bottleRackTex`,
    // same convention Meadery's own bottle rack uses.
    void addHoneyRefineryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& bottleRackTex) {
        sf::Color plankWall(150, 128, 92), roofColor(96, 74, 44), signColor(190, 142, 48); // honeyrefinery's own 2D accent (230,180,60)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 210, 100, 190));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 18.f, 14.f, bottleRackTex, sf::Color(220, 168, 70));
    }

    // Cake Shop -- Oven family, tier-3. A mini layer cake (`cakeTex`) on
    // the display table -- a pastel palette, distinct from the plainer
    // Bakery down the road.
    void addCakeShopBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& cakeTex) {
        sf::Color plankWall(200, 180, 190), roofColor(130, 100, 110), signColor(220, 170, 190); // cakeshop's own 2D accent (235,200,210)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 190, 210, 170));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(200, 180, 190), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 16.f, 18.f, cakeTex, sf::Color::White);
    }

    // Artisan Bakery -- Oven family, tier-3. `breadTex` reused outright
    // from Bakery -- a finer sibling of the same product, not a different
    // one.
    void addArtisanBakeryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& breadTex) {
        sf::Color plankWall(150, 112, 68), roofColor(96, 60, 40), signColor(150, 110, 60); // artisanbakery's own 2D accent (200,150,90)
        auto shell = addOvenFamilyShell(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor, sf::Color(255, 150, 60, 200));
        Vec3 tablePos(shell.bayX0 + shell.bayW * 0.65f, 0.f, b.position.y + b.size.y * 0.5f - 6.f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(20.f, 10.f, 14.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(10.f, 10.f, 7.f), 20.f, 14.f, breadTex, sf::Color::White);
    }

    // Popcorn Stand -- Stall family, tier-2. `popcornTex` on the counter.
    void addPopcornStandBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& popcornTex) {
        auto stall = addMarketStallShell(out, viewProj, windowSize, eye, b, lc, sf::Color(230, 210, 120), sf::Color(232, 228, 214)); // popcornstand's own 2D accent (230,210,120)
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(stall.counterPos.x + stall.counterW * 0.5f, 18.f, stall.counterPos.z + stall.counterD * 0.5f), 18.f, 20.f, popcornTex, sf::Color::White);
    }

    // Juice Bar -- Stall family, tier-2. Red-tinted `bottleRackTex` for
    // bottled juice -- fresh produce squeezed into glass, same "outlined
    // near-white shape, tint per business" trick every Brewery-family
    // bottle rack already uses.
    void addJuiceBarBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& bottleRackTex) {
        auto stall = addMarketStallShell(out, viewProj, windowSize, eye, b, lc, sf::Color(200, 60, 70), sf::Color(232, 228, 214)); // juicebar's own 2D accent (200,60,70)
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(stall.counterPos.x + stall.counterW * 0.5f, 18.f, stall.counterPos.z + stall.counterD * 0.5f), 22.f, 16.f, bottleRackTex, sf::Color(230, 110, 70));
    }

    // ---- Zone 7: Fisher's Isle ----

    // Cannery -- Dock family, tier-2. Stacked tin cans instead of crates,
    // plus a fish net rack (`fishTex` reused from Fishing Dock).
    void addCanneryBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fishTex) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color canColor(180, 182, 186);
        for (const auto& cp : { sf::Vector2f(0.2f, 0.3f), sf::Vector2f(0.3f, 0.3f), sf::Vector2f(0.25f, 0.42f) }) {
            Vec3 p(b.position.x + b.size.x * cp.x, 0.f, b.position.y + dock.deckD * cp.y);
            addBox(out, viewProj, windowSize, eye, p, Vec3(8.f, 10.f, 8.f), canColor, lc);
            addBox(out, viewProj, windowSize, eye, p + Vec3(0.f, 10.f, 0.f), Vec3(8.f, 1.f, 8.f), shade3d(canColor, -20), lc);
        }
        Vec3 basketPos(b.position.x + b.size.x * 0.6f, 0.f, b.position.y + dock.deckD * 0.4f);
        addBandedBox(out, viewProj, windowSize, eye, basketPos, Vec3(14.f, 10.f, 14.f), sf::Color(150, 108, 62), lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, basketPos + Vec3(7.f, 10.f, 7.f), 16.f, 12.f, fishTex, sf::Color::White);
    }

    // Smokehouse -- its own single-business family (isSmokehouseId). Same
    // Workshop-family cabin, plus a smoking rack (fish hung to cure,
    // `fishTex`) and a HEAVY grey smoke glow instead of the usual warm
    // firebox orange -- a smokehouse's whole point is the smoke, not a
    // flame you'd actually see.
    void addSmokehouseBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex, const sf::Texture& fishTex) {
        sf::Color plankWall(120, 108, 100), roofColor(72, 62, 56), signColor(120, 120, 120); // smokehouse's own 2D accent (140,140,140)
        auto cab = addWorkshopCabin(out, viewProj, windowSize, eye, b, lc, wallH, roofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, plankWall, roofColor, signColor);
        Vec3 basePos(b.position.x, 0.f, b.position.y);

        float bayX0 = basePos.x + cab.enclosedW;
        float bayW = b.size.x - cab.enclosedW;
        float bayH = cab.wallTop * 0.68f;
        sf::Color postColor(90, 62, 34);
        for (float px : { bayX0 + 4.f, bayX0 + bayW - 4.f }) {
            for (float pz : { basePos.z + 4.f, basePos.z + b.size.y - 4.f }) {
                addBox(out, viewProj, windowSize, eye, Vec3(px, 0.f, pz), Vec3(4.f, bayH, 4.f), postColor, lc);
            }
        }
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayH, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -14), lc);

        Vec3 rackMid(bayX0 + bayW * 0.5f, bayH - 4.f, basePos.z + b.size.y * 0.5f);
        addBox(out, viewProj, windowSize, eye, rackMid - Vec3(bayW * 0.3f, 0.f, 2.f), Vec3(bayW * 0.6f, 2.f, 4.f), postColor, lc);
        for (float t : { -0.25f, 0.f, 0.25f }) {
            addBillboard(out, viewProj, windowSize, billboardRight, rackMid + Vec3(bayW * 0.3f * t, -10.f, 0.f), 14.f, 16.f, fishTex, sf::Color(150, 130, 110));
        }
        addGlowBillboard(out, viewProj, windowSize, billboardRight, rackMid + Vec3(0.f, 12.f, 0.f), 30.f, glowTex, sf::Color(140, 138, 134, 160));
    }

    // Deep Sea Fishing -- Dock family, tier-1. Diving/net gear on the
    // deck and 1 big prize fish.
    void addDeepSeaProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fishTex) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color coilColor(70, 90, 110);
        Vec3 coilPos(b.position.x + b.size.x * 0.25f, 0.f, b.position.y + dock.deckD * 0.4f);
        addBox(out, viewProj, windowSize, eye, coilPos, Vec3(16.f, 6.f, 16.f), coilColor, lc);
        addBox(out, viewProj, windowSize, eye, coilPos + Vec3(3.f, 6.f, 3.f), Vec3(10.f, 5.f, 10.f), shade3d(coilColor, 16), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(b.position.x + b.size.x * 0.62f, 0.f, b.position.y + dock.deckD * 0.5f), 34.f, 22.f, fishTex, sf::Color::White);
    }

    // Sushi Bar -- Stall family, tier-2. A plated fish (`fishTex`) on the
    // counter instead of a bottle rack or basket.
    void addSushiBarBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fishTex) {
        auto stall = addMarketStallShell(out, viewProj, windowSize, eye, b, lc, sf::Color(230, 100, 110), sf::Color(232, 228, 214)); // sushibar's own 2D accent (230,100,110)
        addBox(out, viewProj, windowSize, eye, Vec3(stall.counterPos.x + stall.counterW * 0.5f - 10.f, 18.f, stall.counterPos.z + stall.counterD * 0.5f - 8.f), Vec3(20.f, 1.5f, 16.f), sf::Color(230, 230, 225), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(stall.counterPos.x + stall.counterW * 0.5f, 20.f, stall.counterPos.z + stall.counterD * 0.5f), 20.f, 14.f, fishTex, sf::Color::White);
    }

    // Fisherman's Platter -- Dock family, tier-3, the multi-input recipe
    // combining Cannery/Smokehouse's output with Harbor's salt (see
    // buildZones()'s own comment). A big serving platter with a whole fish
    // on the deck, standing in for the finished dish.
    void addFishermanPlatterBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& fishTex) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        Vec3 tablePos(b.position.x + b.size.x * 0.3f, 0.f, b.position.y + dock.deckD * 0.35f);
        addBandedBox(out, viewProj, windowSize, eye, tablePos, Vec3(26.f, 12.f, 18.f), sf::Color(110, 78, 46), lc, nullptr, 40.f, true);
        addBox(out, viewProj, windowSize, eye, tablePos + Vec3(3.f, 12.f, 3.f), Vec3(20.f, 1.5f, 12.f), sf::Color(230, 230, 225), lc);
        addBillboard(out, viewProj, windowSize, billboardRight, tablePos + Vec3(13.f, 14.f, 9.f), 24.f, 16.f, fishTex, sf::Color::White);
    }

    // Island Ferry -- not a real business (see buildZones()'s own comment
    // -- a plain interactive world object), but still deserves better than
    // the generic box+roof: a boat hull moored at the dock plus a small
    // ticket booth.
    void addIslandFerryProps(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, Vec3 billboardRight, const sf::Texture& glowTex) {
        auto dock = addDockShell(out, viewProj, windowSize, eye, b, lc);
        sf::Color hullColor(210, 210, 215), trimColor(90, 160, 200);
        Vec3 hullPos(b.position.x + b.size.x * 0.15f, 0.f, dock.waterZ0 - 6.f);
        addBox(out, viewProj, windowSize, eye, hullPos, Vec3(b.size.x * 0.7f, 10.f, 22.f), hullColor, lc);
        addBox(out, viewProj, windowSize, eye, hullPos + Vec3(4.f, 10.f, 4.f), Vec3(b.size.x * 0.7f - 8.f, 2.f, 14.f), trimColor, lc);
        Vec3 cabinPos(hullPos.x + b.size.x * 0.7f * 0.3f, 12.f, hullPos.z + 4.f);
        addBox(out, viewProj, windowSize, eye, cabinPos, Vec3(20.f, 12.f, 12.f), sf::Color(230, 230, 232), lc);

        sf::Color boothColor(150, 118, 76);
        Vec3 boothPos(b.position.x + b.size.x * 0.1f, 0.f, b.position.y + 8.f);
        addBandedBox(out, viewProj, windowSize, eye, boothPos, Vec3(16.f, 24.f, 14.f), boothColor, lc, nullptr, 40.f, true);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, boothPos + Vec3(8.f, 16.f, 15.f), 12.f, glowTex, sf::Color(255, 214, 140, 130));
    }

    // Textile Mill -- from an eighteenth reference image, explicitly
    // labeled "纺织厂" (Textile Mill) itself, no scope-check needed (this
    // is the exact `textile` business the Sheep Farm round 2 rounds back
    // deferred -- the reference's own barn building was confirmed then to
    // belong here, not to `sheep`): a log-cabin shop with 2 chimneys next
    // to an open weaving bay holding a loom, dye vats, a yarn-loaded
    // wheelbarrow, and stacked folded fabric. Only `b.id == "textile"`
    // uses this -- the 2nd Workshop-family processor building after
    // Bakery.
    void addTextileMillBuilding(std::vector<ScreenQuad>& out, const Mat4& viewProj, sf::Vector2u windowSize, Vec3 eye,
        const WorldBuilding& b, const LightingContext& lc, float wallH, float roofRise, Vec3 billboardRight, const sf::Texture& glowTex,
        const sf::Texture& stoneTex, const sf::Texture& shingleTex, const sf::Texture& flowerTex,
        const sf::Texture& loomTex, const sf::Texture& yarnTex) {
        sf::Color stone(118, 114, 108);
        sf::Color plankWall(150, 112, 68);
        sf::Color beamColor(58, 40, 26);
        sf::Color roofColor(96, 60, 40);
        sf::Color windowColor(255, 214, 140);
        sf::Color signColor(120, 84, 48);
        sf::Color plantBoxColor(96, 68, 40);
        sf::Color hayColor(198, 168, 78);
        sf::Color hayCapColor(220, 196, 108);
        sf::Color cartColor(118, 86, 52);
        sf::Color wheelColor(60, 44, 28);

        constexpr float kStoneUv = 20.f, kShingleUv = 15.f;

        Vec3 basePos(b.position.x, 0.f, b.position.y);
        float southZ = basePos.z + b.size.y + 1.5f;

        float wallH2 = wallH;
        float foundationH = wallH2 * 0.18f;
        float upperH = wallH2 - foundationH;
        float wallTop = wallH2;

        // ---- Enclosed cabin (west 52%) -- same recipe Bakery's own
        // cabin just established. ----
        float enclosedW = b.size.x * 0.52f;
        addBandedBox(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, foundationH, b.size.y), stone, lc, &stoneTex, kStoneUv);
        addBandedBox(out, viewProj, windowSize, eye, Vec3(basePos.x, foundationH, basePos.z), Vec3(enclosedW, upperH, b.size.y), plankWall, lc, &shingleTex, kShingleUv);
        addGableRoof(out, viewProj, windowSize, eye, basePos, Vec3(enclosedW, wallTop, b.size.y), wallTop, roofRise, roofColor, lc, &shingleTex, kShingleUv);

        float doorW = enclosedW * 0.22f, doorH = foundationH + upperH * 0.55f;
        Vec3 doorPos(basePos.x + enclosedW * 0.5f - doorW * 0.5f, 0.f, southZ);
        addBox(out, viewProj, windowSize, eye, doorPos, Vec3(doorW, doorH, 3.f), beamColor, lc);
        addBox(out, viewProj, windowSize, eye, Vec3(doorPos.x + doorW * 0.5f - 1.f, 0.f, southZ - 0.5f), Vec3(2.f, doorH, 2.f), shade3d(beamColor, -10), lc);

        float winSize = enclosedW * 0.15f;
        Vec3 winPos(basePos.x + enclosedW * 0.15f, foundationH + upperH * 0.4f, southZ);
        addBox(out, viewProj, windowSize, eye, winPos, Vec3(winSize, winSize, 3.f), windowColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, Vec3(winPos.x + winSize * 0.5f, winPos.y + winSize * 0.5f, southZ), winSize * 1.5f, glowTex, sf::Color(255, 214, 140, 130));
        Vec3 boxPos(winPos.x - 2.f, winPos.y - 8.f, southZ - 3.f);
        addBandedBox(out, viewProj, windowSize, eye, boxPos, Vec3(winSize + 4.f, 6.f, 6.f), plantBoxColor, lc, nullptr, 40.f, true);
        addBillboard(out, viewProj, windowSize, billboardRight, Vec3(boxPos.x + (winSize + 4.f) * 0.5f, winPos.y - 2.f, southZ), winSize * 0.9f, 14.f, flowerTex, sf::Color::White);

        float signW = enclosedW * 0.6f, signH = 16.f;
        Vec3 signPos(basePos.x + enclosedW * 0.5f - signW * 0.5f, wallTop + roofRise * 0.35f, southZ - 2.f);
        addBandedBox(out, viewProj, windowSize, eye, signPos, Vec3(signW, signH, 3.f), signColor, lc, nullptr, 40.f, true);

        // 2 chimneys (hearth + dye-vat steam), matching the reference's
        // own pair of tall stone stacks.
        for (float cx : { 0.30f, 0.78f }) {
            Vec3 chimneyPos(basePos.x + enclosedW * cx, wallTop * 0.3f, basePos.z + b.size.y * 0.5f);
            addBox(out, viewProj, windowSize, eye, chimneyPos, Vec3(11.f, wallTop * 0.75f, 11.f), sf::Color(96, 90, 86), lc);
            addGlowBillboard(out, viewProj, windowSize, billboardRight, chimneyPos + Vec3(5.5f, wallTop * 0.75f + 12.f, 5.5f), 16.f, glowTex, sf::Color(210, 210, 214, 90));
        }

        // ---- Open weaving bay (east 48%): 2 stone piers, a recessed back
        // wall with the loom decal mounted on it, and a flat shed roof
        // lower than the cabin's own ridge -- Sawmill's exact open-bay
        // recipe, loom standing in for the saw blade. ----
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

        Vec3 loomBase(bayX0 + bayW * 0.5f, 0.f, backFaceZ);
        addBillboard(out, viewProj, windowSize, billboardRight, loomBase, 30.f, 36.f, loomTex, sf::Color::White);

        float bayRoofY = wallTop * 0.78f;
        addBox(out, viewProj, windowSize, eye, Vec3(bayX0, bayRoofY, basePos.z), Vec3(bayW, 3.f, b.size.y), shade3d(roofColor, -10), lc);

        // 3 dye vats on the open floor between the piers -- a box body +
        // a darker rim band (Warehouse's own barrel technique) tinted
        // 3 different dye colors, deliberately kept inboard of both piers
        // (dx 65.12-94.16 clear span).
        {
            const std::pair<float, sf::Color> vats[] = {
                { 66.f, sf::Color(60, 90, 130) },  // blue dye
                { 76.f, sf::Color(120, 50, 55) },  // red dye
                { 86.f, sf::Color(90, 62, 40) },   // brown dye
            };
            for (const auto& [vx, col] : vats) {
                Vec3 vatPos(basePos.x + vx, 0.f, basePos.z + 36.f);
                addBox(out, viewProj, windowSize, eye, vatPos, Vec3(8.f, 14.f, 8.f), col, lc);
                addBox(out, viewProj, windowSize, eye, vatPos + Vec3(-0.6f, 12.f, -0.6f), Vec3(9.2f, 2.f, 9.2f), shade3d(col, -20), lc);
            }
        }

        // Folded fabric stack, south of the vats -- Sawmill's own plank-
        // stack layering, alternating bright dyed colors instead of one
        // wood tone.
        {
            const sf::Color fabricColors[] = { sf::Color(140, 70, 130), sf::Color(60, 110, 130), sf::Color(196, 150, 40), sf::Color(150, 50, 60) };
            Vec3 fabricBase(basePos.x + 68.f, 0.f, southZ + 4.f);
            for (int i = 0; i < 4; ++i) {
                addBox(out, viewProj, windowSize, eye, fabricBase + Vec3(0.f, static_cast<float>(i) * 3.2f, 0.f), Vec3(20.f, 3.f, 12.f), fabricColors[i], lc);
            }
        }

        // Hay bales, north of the bay's own footprint.
        Vec3 hayBase(bayX0 + 8.f, 0.f, basePos.z + 2.f);
        for (int i = 0; i < 2; ++i) {
            Vec3 hp = hayBase + Vec3(static_cast<float>(i) * 11.f, 0.f, 0.f);
            addBox(out, viewProj, windowSize, eye, hp, Vec3(9.f, 8.f, 9.f), hayColor, lc);
            addBox(out, viewProj, windowSize, eye, hp + Vec3(1.f, 8.f, 1.f), Vec3(7.f, 1.5f, 7.f), hayCapColor, lc);
        }

        // Yarn-loaded wheelbarrow, south of the gap between the cabin and
        // the bay -- the same crude cart-plus-wheel convention every
        // barrow in this file uses, topped with 2 yarn-spool decals
        // instead of Sheep's own wool clumps.
        Vec3 cartPos(basePos.x + enclosedW - 6.f, 0.f, southZ + 4.f);
        addBox(out, viewProj, windowSize, eye, cartPos, Vec3(16.f, 8.f, 10.f), cartColor, lc);
        addBox(out, viewProj, windowSize, eye, cartPos + Vec3(6.f, -4.f, 4.f), Vec3(4.f, 4.f, 4.f), wheelColor, lc);
        addBillboard(out, viewProj, windowSize, billboardRight, cartPos + Vec3(5.f, 8.f, 5.f), 16.f, 13.f, yarnTex, sf::Color::White);
        addBillboard(out, viewProj, windowSize, billboardRight, cartPos + Vec3(11.f, 8.f, 5.f), 14.f, 11.f, yarnTex, sf::Color::White);

        // Lantern post, west of the wheelbarrow (clear of its own footprint,
        // dx 51.2-67.2 -- an earlier position at dx 58 sat right inside it).
        Vec3 lanternPos(basePos.x + 38.f, 0.f, southZ + 8.f);
        addBox(out, viewProj, windowSize, eye, lanternPos, Vec3(3.f, 24.f, 3.f), beamColor, lc);
        addGlowBillboard(out, viewProj, windowSize, billboardRight, lanternPos + Vec3(1.5f, 28.f, 1.5f), 16.f, glowTex, sf::Color(255, 200, 120, 160));
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
    float lightBoost = 0.14f + 1.3f * night; // lamps/windows: dim by day, brighter-than-before by night (0.88->1.05->1.3 ceiling, 2026-08-10 "夜晚的灯有点暗" follow-up)
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
    // locked business (renders as a natural clearing + padlock signpost
    // now, not a building at all -- see draw3DZone's building loop) and
    // for one still at the bare-plot/construction-site stage (nothing
    // built yet to have a lit window).
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
        light.intensity = 48.f * lightBoost; // 30->40->48, 2026-08-10 "夜晚的灯有点暗" follow-up
        light.bloom = false; // shading only -- see PointLight3D's own comment on why this one doesn't also get a visible glow ring
        lc.lights.push_back(light);
    }
    for (const auto& d : z.decorations) {
        if (d.kind != Decoration::Kind::Lamp) continue;
        PointLight3D light;
        light.pos = Vec3(d.position.x, 34.f, d.position.y);
        light.color = Vec3(1.f, 0.75f, 0.43f);
        light.intensity = 44.f * lightBoost; // 28->36->44, 2026-08-10 "夜晚的灯有点暗" follow-up
        // bloom stays true (default) -- an actual visible lamp fixture,
        // this IS the light the ring is supposed to mark.
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
    // 2026-08-12: bumped 78 -> 84 tall -- drawLamp's own pole reaches down
    // to pos.y + 34 = 84 (drawGroundShadow's anchor, the lowest thing it
    // draws), so the old 78-tall canvas clipped the bottom ~6px of the pole
    // off before it ever reached this texture's own bottom edge (world
    // billboard height stays 78 at the call site below, so this just gives
    // the bake itself enough room to not clip -- the sprite lands very
    // slightly more compressed, not stretched).
    const sf::Texture& lampTex = getBillboard3D("lamp", sf::Vector2u(30, 84),
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
    //
    // 2026-08-10 bugfix ("每个屋子的前面都有一个四方形的光环" -- every
    // building's own glow reads as a square, not a soft circle): drawGlow's
    // own outermost ring is radius * 3.4 * sizeScale (sizeScale up to 1.2 at
    // night -- see its own comment), which at the old radius=40 into a 96x96
    // (48-half-width) canvas comes out to 40*3.4*1.2=163 -- more than 3x
    // wider than the canvas can hold. The circle's own curve is so far
    // outside the visible square that the small arc actually inside the
    // canvas reads as almost flat, tinting the whole square a near-uniform
    // faint color instead of fading to transparent toward the corners --
    // exactly a "square halo," not a soft one. This was always slightly
    // wrong, but only became visible once this file's earlier lighting-
    // strengthen pass enlarged the on-screen billboard size that stretches
    // this same texture (`addGlowBillboard`'s `glowSize`), making the
    // clipped-square artifact big enough to actually notice. Fixed by
    // baking at a canvas/radius ratio that keeps the outer ring safely
    // inside the bounds even at the maximum night sizeScale (128x128 canvas,
    // radius 14: 14*3.4*1.2=57.1, comfortably under the 64-unit half-width) --
    // a real soft circular falloff now, at any billboard size it gets
    // stretched to.
    const sf::Texture& glowTex = getBillboard3D("glow_dot", sf::Vector2u(128, 128),
        [&](sf::RenderTexture& rt) { drawGlow(rt, sf::Vector2f(64.f, 64.f), 14.f, sf::Color::White); });
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
    // Bakery's bread-basket decal (see addBakeryBuilding below) -- a
    // basket with a few oblong loaves poking over the rim, mounted on the
    // outdoor display tables the same way Market mounts its own produce
    // crate.
    const sf::Texture& breadTex = getBillboard3D("bread_basket", sf::Vector2u(32, 22), [&](sf::RenderTexture& rt) {
        sf::RectangleShape basket(sf::Vector2f(30.f, 10.f));
        basket.setPosition(sf::Vector2f(1.f, 12.f));
        basket.setFillColor(sf::Color(150, 108, 62));
        basket.setOutlineThickness(1.f);
        basket.setOutlineColor(sf::Color(90, 62, 34));
        rt.draw(basket);
        const std::pair<sf::Vector2f, float> loaves[] = { { {2.f, 4.f}, 20.f }, { {12.f, 2.f}, 22.f }, { {22.f, 5.f}, 18.f } };
        for (const auto& [p, w] : loaves) {
            sf::RectangleShape loaf(sf::Vector2f(w * 0.4f, 8.f));
            loaf.setPosition(p);
            loaf.setFillColor(sf::Color(198, 148, 88));
            loaf.setOutlineThickness(0.8f);
            loaf.setOutlineColor(sf::Color(110, 74, 38));
            rt.draw(loaf);
        }
        });
    // Textile Mill's loom decal (see addTextileMillBuilding below) -- a
    // wooden frame with vertical warp threads and one horizontal beam,
    // the same "fake a mechanism as a flat sprite" convention Sawmill's
    // own wheel/blade decals already use.
    const sf::Texture& loomTex = getBillboard3D("loom", sf::Vector2u(30, 36), [&](sf::RenderTexture& rt) {
        sf::RectangleShape frame(sf::Vector2f(30.f, 36.f));
        frame.setFillColor(sf::Color::Transparent);
        frame.setOutlineThickness(2.f);
        frame.setOutlineColor(sf::Color(96, 66, 40));
        rt.draw(frame);
        for (float x = 4.f; x < 28.f; x += 3.5f) {
            sf::RectangleShape thread(sf::Vector2f(1.f, 30.f));
            thread.setPosition(sf::Vector2f(x, 3.f));
            thread.setFillColor(sf::Color(224, 218, 200));
            rt.draw(thread);
        }
        sf::RectangleShape beam(sf::Vector2f(26.f, 3.f));
        beam.setPosition(sf::Vector2f(2.f, 17.f));
        beam.setFillColor(sf::Color(110, 78, 46));
        rt.draw(beam);
        });
    // A loose cluster of colored yarn spools -- same "no crate, sits
    // directly on a surface" idea as Farm's own veggie cluster, just a
    // brighter dyed-thread palette.
    const sf::Texture& yarnTex = getBillboard3D("yarn_spools", sf::Vector2u(20, 16), [&](sf::RenderTexture& rt) {
        const std::pair<sf::Vector2f, sf::Color> spools[] = {
            { {2.f, 5.f}, sf::Color(60, 110, 130) },  // teal
            { {8.f, 2.f}, sf::Color(150, 50, 60) },   // maroon
            { {13.f, 6.f}, sf::Color(196, 150, 40) }, // mustard
            { {6.f, 9.f}, sf::Color(110, 70, 140) },  // purple
        };
        for (const auto& [p, col] : spools) {
            sf::CircleShape s(3.4f);
            s.setPosition(p);
            s.setFillColor(col);
            s.setOutlineThickness(0.8f);
            s.setOutlineColor(sf::Color(30, 24, 16));
            rt.draw(s);
        }
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
    // Sheepdog decal (see addPastureProps below) -- same small hand-drawn
    // sprite convention as the pig/chicken decals just above.
    const sf::Texture& dogTex = getBillboard3D("farm_dog", sf::Vector2u(22, 20), [&](sf::RenderTexture& rt) {
        sf::CircleShape body(7.f);
        body.setPosition(sf::Vector2f(6.f, 6.f));
        body.setFillColor(sf::Color(120, 96, 72));
        body.setOutlineThickness(1.f);
        body.setOutlineColor(sf::Color(70, 54, 40));
        rt.draw(body);
        sf::CircleShape head(4.5f);
        head.setPosition(sf::Vector2f(0.f, 3.f));
        head.setFillColor(sf::Color(140, 114, 86));
        head.setOutlineThickness(0.8f);
        head.setOutlineColor(sf::Color(70, 54, 40));
        rt.draw(head);
        sf::ConvexShape ear(3);
        ear.setPoint(0, sf::Vector2f(1.f, 3.f));
        ear.setPoint(1, sf::Vector2f(-1.f, -2.f));
        ear.setPoint(2, sf::Vector2f(3.f, 1.f));
        ear.setFillColor(sf::Color(90, 70, 52));
        rt.draw(ear);
        sf::ConvexShape tail(3);
        tail.setPoint(0, sf::Vector2f(18.f, 8.f));
        tail.setPoint(1, sf::Vector2f(21.f, 3.f));
        tail.setPoint(2, sf::Vector2f(20.f, 10.f));
        tail.setFillColor(sf::Color(120, 96, 72));
        rt.draw(tail);
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
    // 2026-08-11 batch (Zone 5's remaining Field-family businesses) -- 4
    // new decals: a cow, a hanging pelt, a tea bush, and a flax flower.
    // 2026-08-11 detail pass ("那个奶牛可以再细节一点吗" -- can the cow
    // get more detail): the old version was just a body rectangle + 2
    // perfectly round black dots + a head rectangle -- no legs, no face,
    // no tail, patches too geometric to read as fur. Bigger canvas (was
    // 26x22), 4 thin legs, small ears + horns + a pink snout on the head,
    // a tail, and the patches redrawn as lopsided convex blobs (real cow
    // markings are never perfect circles) instead of CircleShapes.
    const sf::Texture& cowTex = getBillboard3D("cow", sf::Vector2u(32, 28), [&](sf::RenderTexture& rt) {
        sf::Color coat(245, 245, 240), outline(40, 38, 36);
        for (float lx : { 5.f, 10.f, 17.f, 22.f }) {
            sf::RectangleShape leg(sf::Vector2f(3.f, 8.f));
            leg.setPosition(sf::Vector2f(lx, 18.f));
            leg.setFillColor(sf::Color(60, 56, 50));
            rt.draw(leg);
        }
        sf::RectangleShape tail(sf::Vector2f(2.f, 9.f));
        tail.setPosition(sf::Vector2f(25.f, 9.f));
        tail.setRotation(sf::degrees(20.f));
        tail.setFillColor(sf::Color(60, 56, 50));
        rt.draw(tail);

        sf::RectangleShape body(sf::Vector2f(22.f, 13.f));
        body.setPosition(sf::Vector2f(4.f, 7.f));
        body.setFillColor(coat);
        body.setOutlineThickness(1.f);
        body.setOutlineColor(outline);
        rt.draw(body);

        const sf::Vector2f patchPts[2][5] = {
            { {5.f, 8.f}, {9.f, 7.f}, {10.f, 11.f}, {7.f, 13.f}, {4.f, 11.f} },
            { {15.f, 10.f}, {20.f, 9.f}, {21.f, 13.f}, {17.f, 16.f}, {14.f, 13.f} },
        };
        for (const auto& pts : patchPts) {
            sf::ConvexShape patch;
            patch.setPointCount(5);
            for (std::size_t i = 0; i < 5; ++i) patch.setPoint(i, pts[i]);
            patch.setFillColor(sf::Color(50, 48, 46));
            rt.draw(patch);
        }

        sf::RectangleShape head(sf::Vector2f(8.f, 8.f));
        head.setPosition(sf::Vector2f(0.f, 3.f));
        head.setFillColor(coat);
        head.setOutlineThickness(1.f);
        head.setOutlineColor(outline);
        rt.draw(head);
        sf::RectangleShape snout(sf::Vector2f(6.f, 3.5f));
        snout.setPosition(sf::Vector2f(0.5f, 9.f));
        snout.setFillColor(sf::Color(214, 160, 168));
        rt.draw(snout);
        for (float ex : { -1.5f, 7.f }) {
            sf::CircleShape ear(2.2f);
            ear.setPosition(sf::Vector2f(ex, 1.5f));
            ear.setFillColor(coat);
            ear.setOutlineThickness(0.6f);
            ear.setOutlineColor(outline);
            rt.draw(ear);
        }
        for (float hx : { 1.f, 6.f }) {
            sf::RectangleShape horn(sf::Vector2f(1.4f, 3.f));
            horn.setPosition(sf::Vector2f(hx, 0.f));
            horn.setFillColor(sf::Color(214, 204, 180));
            rt.draw(horn);
        }
        });
    const sf::Texture& peltTex = getBillboard3D("pelt", sf::Vector2u(14, 20), [&](sf::RenderTexture& rt) {
        sf::ConvexShape pelt;
        pelt.setPointCount(6);
        pelt.setPoint(0, sf::Vector2f(7.f, 0.f));
        pelt.setPoint(1, sf::Vector2f(13.f, 5.f));
        pelt.setPoint(2, sf::Vector2f(11.f, 14.f));
        pelt.setPoint(3, sf::Vector2f(7.f, 19.f));
        pelt.setPoint(4, sf::Vector2f(3.f, 14.f));
        pelt.setPoint(5, sf::Vector2f(1.f, 5.f));
        pelt.setFillColor(sf::Color(150, 110, 75));
        pelt.setOutlineThickness(0.8f);
        pelt.setOutlineColor(sf::Color(90, 64, 42));
        rt.draw(pelt);
        });
    // 2026-08-11 detail pass ("茶田的话可以做茶叶的样子出来吗" -- can the
    // Tea Field show actual tea leaves): the old version was just 2 plain
    // green circles (a shadow blob + a highlight blob), no leaf shapes at
    // all -- read as a generic round shrub, not tea. Now the base/
    // highlight blobs stay (still the cheapest way to fake canopy volume)
    // but get a scatter of individual pointed-oval leaf shapes on top (2
    // green tones) plus a few pale "new growth" tips at the crown -- the
    // lighter shoots real tea bushes are actually picked for -- so the
    // silhouette reads as leaves, not a blob.
    const sf::Texture& teaBushTex = getBillboard3D("tea_bush", sf::Vector2u(22, 18), [&](sf::RenderTexture& rt) {
        sf::CircleShape base(10.f);
        base.setPosition(sf::Vector2f(1.f, 4.f));
        base.setFillColor(sf::Color(70, 116, 58));
        rt.draw(base);
        sf::CircleShape hi(7.f);
        hi.setPosition(sf::Vector2f(4.f, 2.f));
        hi.setFillColor(sf::Color(96, 146, 72));
        rt.draw(hi);

        auto leaf = [&](sf::Vector2f p, float rotDeg, sf::Color col) {
            sf::ConvexShape lf;
            lf.setPointCount(4);
            lf.setPoint(0, sf::Vector2f(2.f, 0.f));
            lf.setPoint(1, sf::Vector2f(4.f, 2.f));
            lf.setPoint(2, sf::Vector2f(2.f, 4.f));
            lf.setPoint(3, sf::Vector2f(0.f, 2.f));
            lf.setOrigin(sf::Vector2f(2.f, 2.f));
            lf.setPosition(p);
            lf.setRotation(sf::degrees(rotDeg));
            lf.setFillColor(col);
            rt.draw(lf);
        };
        const std::pair<sf::Vector2f, float> leaves[] = {
            { {5.f, 5.f}, 20.f }, { {10.f, 4.f}, -25.f }, { {14.f, 7.f}, 40.f },
            { {7.f, 9.f}, -10.f }, { {12.f, 10.f}, 15.f }, { {3.f, 9.f}, -30.f },
        };
        for (const auto& [p, rot] : leaves) leaf(p, rot, sf::Color(58, 100, 48));

        // Pale new-growth tips near the crown, the part actually picked.
        for (const auto& p : { sf::Vector2f(6.f, 2.f), sf::Vector2f(11.f, 1.f) }) leaf(p, 0.f, sf::Color(150, 190, 110));
        });
    // 2026-08-11 detail pass (Flax Field's own rework above) -- was a flat
    // lying-down "stem rectangle + 3 plain dots" sprite meant to BE the
    // whole plant; now that a real 3D stem box carries the plant's own
    // height, this only needs to be the flower cluster sitting on top of
    // it -- 2 actual 5-petal flax flowers (real flax blooms are 5-petaled,
    // pale blue, with a small yellow center) instead of solid dots.
    const sf::Texture& flaxFlowerTex = getBillboard3D("flax_flower", sf::Vector2u(16, 14), [&](sf::RenderTexture& rt) {
        auto bloom = [&](sf::Vector2f center) {
            for (int i = 0; i < 5; ++i) {
                float ang = static_cast<float>(i) * 6.2831853f / 5.f;
                sf::CircleShape petal(1.8f);
                petal.setPosition(center + sf::Vector2f(std::cos(ang) * 2.2f - 1.8f, std::sin(ang) * 2.2f - 1.8f));
                petal.setFillColor(sf::Color(160, 180, 226));
                rt.draw(petal);
            }
            sf::CircleShape core(1.f);
            core.setPosition(center - sf::Vector2f(1.f, 1.f));
            core.setFillColor(sf::Color(230, 200, 90));
            rt.draw(core);
        };
        bloom(sf::Vector2f(5.f, 5.f));
        bloom(sf::Vector2f(11.f, 8.f));
        });
    // Apple/Pear tree sprites (2026-08-11, replacing the old single
    // "fruit_tree" flat red blob -- see addOrchardProps's own rework
    // comment): a shared `paintFruitTree` lambda draws the trunk, a short
    // angled branch stub, and a layered canopy (dark base blob + lighter
    // highlight blob on top, the same shadow/highlight "fake volume"
    // split every foliage sprite in this file already uses), then the
    // caller scatters its own fruit-colored dots (each with a small
    // corner highlight, the veggieTex/cabbageTex "fake sphere" trick)
    // across the canopy -- only the fruit color/positions differ between
    // the 2 trees, so the shared shape code stays in one place.
    auto paintFruitTree = [&](sf::RenderTexture& rt) {
        sf::RectangleShape trunk(sf::Vector2f(5.f, 14.f));
        trunk.setPosition(sf::Vector2f(12.5f, 24.f));
        trunk.setFillColor(sf::Color(90, 60, 34));
        rt.draw(trunk);
        sf::RectangleShape branch(sf::Vector2f(9.f, 3.f));
        branch.setPosition(sf::Vector2f(8.f, 21.f));
        branch.setRotation(sf::degrees(-20.f));
        branch.setFillColor(sf::Color(90, 60, 34));
        rt.draw(branch);
        sf::CircleShape base(12.f);
        base.setPosition(sf::Vector2f(3.f, 2.f));
        base.setFillColor(sf::Color(64, 108, 50));
        rt.draw(base);
        sf::CircleShape hi(10.f);
        hi.setPosition(sf::Vector2f(4.5f, 1.f));
        hi.setFillColor(sf::Color(92, 140, 66));
        rt.draw(hi);
    };
    const sf::Texture& appleTreeTex = getBillboard3D("apple_tree", sf::Vector2u(30, 40), [&](sf::RenderTexture& rt) {
        paintFruitTree(rt);
        const sf::Vector2f apples[] = { {6.f, 10.f}, {17.f, 6.f}, {22.f, 13.f}, {9.f, 18.f}, {19.f, 19.f}, {13.f, 3.5f} };
        for (const auto& a : apples) {
            sf::CircleShape fruit(2.6f);
            fruit.setPosition(a);
            fruit.setFillColor(sf::Color(198, 48, 44));
            fruit.setOutlineThickness(0.6f);
            fruit.setOutlineColor(sf::Color(90, 24, 20));
            rt.draw(fruit);
            sf::CircleShape shine(1.f);
            shine.setPosition(a + sf::Vector2f(0.7f, 0.4f));
            shine.setFillColor(sf::Color(232, 116, 96));
            rt.draw(shine);
        }
        });
    const sf::Texture& pearTreeTex = getBillboard3D("pear_tree", sf::Vector2u(30, 40), [&](sf::RenderTexture& rt) {
        paintFruitTree(rt);
        const sf::Vector2f pears[] = { {7.f, 9.f}, {18.f, 7.f}, {21.f, 14.f}, {8.f, 17.f}, {20.f, 20.f}, {12.f, 4.f} };
        for (const auto& p : pears) {
            sf::CircleShape fruit(2.3f);
            fruit.setPosition(p);
            fruit.setFillColor(sf::Color(196, 186, 66));
            fruit.setOutlineThickness(0.6f);
            fruit.setOutlineColor(sf::Color(96, 88, 24));
            rt.draw(fruit);
            sf::CircleShape shine(0.9f);
            shine.setPosition(p + sf::Vector2f(0.6f, 0.3f));
            shine.setFillColor(sf::Color(228, 224, 140));
            rt.draw(shine);
        }
        });
    // Fruit-crate decal (2026-08-11, addOrchardProps's own new gate
    // crates) -- a mixed pile of apples and pears, same layered-circle
    // "fake sphere" fruit this file's own veggieTex/appleTreeTex already
    // established, just packed tighter into a crate-sized sprite.
    const sf::Texture& fruitCrateTex = getBillboard3D("fruit_crate", sf::Vector2u(22, 16), [&](sf::RenderTexture& rt) {
        const std::pair<sf::Vector2f, sf::Color> pile[] = {
            { {3.f, 6.f}, sf::Color(198, 48, 44) },
            { {9.f, 3.f}, sf::Color(196, 186, 66) },
            { {15.f, 6.f}, sf::Color(198, 48, 44) },
            { {6.f, 10.f}, sf::Color(196, 186, 66) },
            { {13.f, 10.f}, sf::Color(198, 48, 44) },
        };
        for (const auto& [p, col] : pile) {
            sf::CircleShape fruit(3.f);
            fruit.setPosition(p);
            fruit.setFillColor(col);
            fruit.setOutlineThickness(0.7f);
            fruit.setOutlineColor(sf::Color(40, 30, 16));
            rt.draw(fruit);
        }
        });
    // Jam-jar decal (2026-08-11, addPreserveBuilding's own display table)
    // -- a row of 4 differently-colored jars (fill = the jam flavor, a
    // pale lid band, and a small corner shine each), the same "outlined
    // circle + shine" fake-volume trick veggieTex/fruitCrateTex above use.
    const sf::Texture& jamJarsTex = getBillboard3D("jam_jars", sf::Vector2u(30, 20), [&](sf::RenderTexture& rt) {
        const sf::Color jarColors[] = { sf::Color(190, 70, 70), sf::Color(200, 140, 40), sf::Color(96, 70, 150), sf::Color(90, 120, 60) };
        for (int i = 0; i < 4; ++i) {
            float x = 1.f + static_cast<float>(i) * 7.f;
            sf::RectangleShape body(sf::Vector2f(5.5f, 10.f));
            body.setPosition(sf::Vector2f(x, 8.f));
            body.setFillColor(jarColors[i]);
            body.setOutlineThickness(0.6f);
            body.setOutlineColor(sf::Color(30, 24, 16));
            rt.draw(body);
            sf::RectangleShape lid(sf::Vector2f(6.5f, 2.5f));
            lid.setPosition(sf::Vector2f(x - 0.5f, 6.f));
            lid.setFillColor(sf::Color(184, 182, 176));
            rt.draw(lid);
            sf::CircleShape shine(1.1f);
            shine.setPosition(sf::Vector2f(x + 0.8f, 10.f));
            shine.setFillColor(sf::Color(255, 255, 255, 120));
            rt.draw(shine);
        }
        });
    // Fruit-press wheel decal (2026-08-11, addPreserveBuilding's own
    // press annex) -- a small wooden cross-handle over a hub, mounted on
    // top of the press post; a scaled-down cousin of `sawmill_wheel`
    // above without the paddle ring (a hand-cranked fruit press has no
    // waterwheel to pretend).
    const sf::Texture& pressWheelTex = getBillboard3D("press_wheel", sf::Vector2u(22, 22), [&](sf::RenderTexture& rt) {
        sf::RectangleShape barH(sf::Vector2f(20.f, 3.f));
        barH.setPosition(sf::Vector2f(1.f, 9.5f));
        barH.setFillColor(sf::Color(110, 78, 44));
        rt.draw(barH);
        sf::RectangleShape barV(sf::Vector2f(3.f, 20.f));
        barV.setPosition(sf::Vector2f(9.5f, 1.f));
        barV.setFillColor(sf::Color(110, 78, 44));
        rt.draw(barV);
        sf::CircleShape hub(4.f);
        hub.setPosition(sf::Vector2f(7.f, 7.f));
        hub.setFillColor(sf::Color(70, 48, 26));
        rt.draw(hub);
        });
    // Gold-nugget decal (2026-08-11, addGoldMineProps's own ore
    // cart/pile) -- the same "outlined circle + shine" fake-volume trick
    // as fruitCrateTex/jamJarsTex above, just gold-tinted.
    const sf::Texture& goldOreTex = getBillboard3D("gold_ore", sf::Vector2u(20, 14), [&](sf::RenderTexture& rt) {
        const sf::Vector2f nuggets[] = { {2.f, 7.f}, {7.f, 3.f}, {12.f, 6.f}, {16.f, 8.f}, {5.f, 10.f}, {10.f, 10.f} };
        for (const auto& n : nuggets) {
            sf::CircleShape nug(2.2f);
            nug.setPosition(n);
            nug.setFillColor(sf::Color(224, 186, 60));
            nug.setOutlineThickness(0.6f);
            nug.setOutlineColor(sf::Color(120, 92, 24));
            rt.draw(nug);
            sf::CircleShape shine(0.8f);
            shine.setPosition(n + sf::Vector2f(0.5f, 0.3f));
            shine.setFillColor(sf::Color(255, 240, 180));
            rt.draw(shine);
        }
        });
    // Gold-bar decal (2026-08-11, addGoldsmithBuilding's own display
    // counter) -- 3 stacked ingots, each a trapezoid-ish rectangle (wider
    // base, narrower lighter top face standing in for the sloped ingot
    // sides, same "fake volume with flat shapes" trick this file leans on
    // everywhere it has no bevel primitive).
    const sf::Texture& goldBarTex = getBillboard3D("gold_bar", sf::Vector2u(24, 16), [&](sf::RenderTexture& rt) {
        for (int i = 0; i < 3; ++i) {
            float x = 1.f + static_cast<float>(i) * 7.5f, y = 9.f - static_cast<float>(i % 2) * 1.5f;
            sf::RectangleShape bar(sf::Vector2f(7.f, 5.f));
            bar.setPosition(sf::Vector2f(x, y));
            bar.setFillColor(sf::Color(198, 162, 54));
            bar.setOutlineThickness(0.6f);
            bar.setOutlineColor(sf::Color(110, 84, 20));
            rt.draw(bar);
            sf::RectangleShape top(sf::Vector2f(5.f, 1.6f));
            top.setPosition(sf::Vector2f(x + 1.f, y + 0.6f));
            top.setFillColor(sf::Color(232, 202, 108));
            rt.draw(top);
        }
        });
    // Gem-tray decal (2026-08-11, same display counter) -- a small tray of
    // loose cut gems, same "outlined circle + shine" fake-volume trick as
    // fruitCrateTex/jamJarsTex, just multi-colored per gem instead of one
    // flavor.
    const sf::Texture& gemTrayTex = getBillboard3D("gem_tray", sf::Vector2u(24, 16), [&](sf::RenderTexture& rt) {
        const std::pair<sf::Vector2f, sf::Color> gems[] = {
            { {2.f, 5.f}, sf::Color(200, 50, 60) },
            { {8.f, 3.f}, sf::Color(60, 110, 200) },
            { {14.f, 6.f}, sf::Color(140, 70, 190) },
            { {19.f, 4.f}, sf::Color(70, 170, 120) },
            { {6.f, 10.f}, sf::Color(60, 110, 200) },
            { {16.f, 10.f}, sf::Color(200, 50, 60) },
        };
        for (const auto& [p, col] : gems) {
            sf::CircleShape gem(2.f);
            gem.setPosition(p);
            gem.setFillColor(col);
            gem.setOutlineThickness(0.5f);
            gem.setOutlineColor(sf::Color(30, 24, 30));
            rt.draw(gem);
            sf::CircleShape shine(0.7f);
            shine.setPosition(p + sf::Vector2f(0.5f, 0.3f));
            shine.setFillColor(sf::Color(255, 255, 255, 170));
            rt.draw(shine);
        }
        });
    // 2026-08-11 batch (Zone 2: Mining District's own remaining 5
    // businesses, after "有的话就试看每一间你自己设计" -- go ahead and
    // design the rest yourself): 5 new bespoke decals, one per building,
    // same fake-volume conventions this file already established.
    //
    // Iron-bar decal (Smelter's own ingot stack) -- goldBarTex's same
    // "wide base + lighter top face" ingot shape, just steel-grey instead
    // of gold, plain iron rather than a precious metal.
    const sf::Texture& ironBarTex = getBillboard3D("iron_bar", sf::Vector2u(24, 16), [&](sf::RenderTexture& rt) {
        for (int i = 0; i < 3; ++i) {
            float x = 1.f + static_cast<float>(i) * 7.5f, y = 9.f - static_cast<float>(i % 2) * 1.5f;
            sf::RectangleShape bar(sf::Vector2f(7.f, 5.f));
            bar.setPosition(sf::Vector2f(x, y));
            bar.setFillColor(sf::Color(140, 142, 146));
            bar.setOutlineThickness(0.6f);
            bar.setOutlineColor(sf::Color(70, 72, 76));
            rt.draw(bar);
            sf::RectangleShape top(sf::Vector2f(5.f, 1.6f));
            top.setPosition(sf::Vector2f(x + 1.f, y + 0.6f));
            top.setFillColor(sf::Color(178, 180, 184));
            rt.draw(top);
        }
        });
    // Weapon-rack decal (Blacksmith's own outdoor rack) -- a sword and an
    // axe, each a plain blade rectangle + a darker hilt/haft, mounted flat
    // against the rack's own crossbar.
    const sf::Texture& weaponRackTex = getBillboard3D("weapon_rack", sf::Vector2u(24, 22), [&](sf::RenderTexture& rt) {
        sf::RectangleShape swordBlade(sf::Vector2f(3.f, 16.f));
        swordBlade.setPosition(sf::Vector2f(4.f, 1.f));
        swordBlade.setFillColor(sf::Color(180, 184, 190));
        swordBlade.setOutlineThickness(0.5f);
        swordBlade.setOutlineColor(sf::Color(70, 72, 76));
        rt.draw(swordBlade);
        sf::RectangleShape swordHilt(sf::Vector2f(6.f, 3.f));
        swordHilt.setPosition(sf::Vector2f(2.5f, 15.f));
        swordHilt.setFillColor(sf::Color(90, 62, 34));
        rt.draw(swordHilt);
        sf::ConvexShape axeHead;
        axeHead.setPointCount(4);
        axeHead.setPoint(0, sf::Vector2f(15.f, 2.f));
        axeHead.setPoint(1, sf::Vector2f(21.f, 5.f));
        axeHead.setPoint(2, sf::Vector2f(19.f, 10.f));
        axeHead.setPoint(3, sf::Vector2f(15.f, 8.f));
        axeHead.setFillColor(sf::Color(160, 164, 170));
        axeHead.setOutlineThickness(0.5f);
        axeHead.setOutlineColor(sf::Color(70, 72, 76));
        rt.draw(axeHead);
        sf::RectangleShape axeHaft(sf::Vector2f(2.5f, 17.f));
        axeHaft.setPosition(sf::Vector2f(14.f, 3.f));
        axeHaft.setFillColor(sf::Color(90, 62, 34));
        rt.draw(axeHaft);
        });
    // Grinding-wheel decal (Gemshop's own cutting wheel) -- a plain grey
    // stone disc, the same "flat billboard standing in for a round shape"
    // trick Sawmill's own waterwheel/blade already established.
    const sf::Texture& grindWheelTex = getBillboard3D("grind_wheel", sf::Vector2u(22, 22), [&](sf::RenderTexture& rt) {
        sf::CircleShape rim(10.f);
        rim.setPosition(sf::Vector2f(1.f, 1.f));
        rim.setFillColor(sf::Color(150, 148, 144));
        rim.setOutlineThickness(1.2f);
        rim.setOutlineColor(sf::Color(90, 88, 84));
        rt.draw(rim);
        sf::CircleShape hub(3.f);
        hub.setPosition(sf::Vector2f(8.f, 8.f));
        hub.setFillColor(sf::Color(70, 48, 26));
        rt.draw(hub);
        });
    // Furniture decal (Carpenter's own workbench) -- a plain wooden stool
    // (seat + 4 short legs), standing in for "finished goods" the same
    // way veggieTex/goldBarTex stand in for their own business's output.
    const sf::Texture& furnitureTex = getBillboard3D("furniture_stool", sf::Vector2u(22, 20), [&](sf::RenderTexture& rt) {
        sf::RectangleShape seat(sf::Vector2f(16.f, 4.f));
        seat.setPosition(sf::Vector2f(3.f, 2.f));
        seat.setFillColor(sf::Color(150, 108, 62));
        seat.setOutlineThickness(0.6f);
        seat.setOutlineColor(sf::Color(80, 56, 30));
        rt.draw(seat);
        for (float lx : { 4.f, 14.f }) {
            sf::RectangleShape leg(sf::Vector2f(2.5f, 13.f));
            leg.setPosition(sf::Vector2f(lx, 6.f));
            leg.setFillColor(sf::Color(120, 86, 50));
            rt.draw(leg);
        }
        });
    // Dress-form decal (Tailor's own boutique display) -- a torso
    // silhouette (shoulders/waist/hips as 3 stacked shapes, standing in
    // for a mannequin curve the same way this file fakes every other
    // round/organic form as flat layered shapes) on a thin center stand.
    const sf::Texture& dressFormTex = getBillboard3D("dress_form", sf::Vector2u(20, 34), [&](sf::RenderTexture& rt) {
        sf::RectangleShape stand(sf::Vector2f(2.f, 20.f));
        stand.setPosition(sf::Vector2f(9.f, 14.f));
        stand.setFillColor(sf::Color(70, 48, 26));
        rt.draw(stand);
        sf::Color fabricColor(196, 172, 148);
        sf::ConvexShape torso;
        torso.setPointCount(6);
        torso.setPoint(0, sf::Vector2f(4.f, 2.f));
        torso.setPoint(1, sf::Vector2f(16.f, 2.f));
        torso.setPoint(2, sf::Vector2f(13.f, 9.f));
        torso.setPoint(3, sf::Vector2f(15.f, 16.f));
        torso.setPoint(4, sf::Vector2f(5.f, 16.f));
        torso.setPoint(5, sf::Vector2f(7.f, 9.f));
        torso.setFillColor(fabricColor);
        torso.setOutlineThickness(0.7f);
        torso.setOutlineColor(sf::Color(120, 100, 80));
        rt.draw(torso);
        sf::CircleShape neck(2.2f);
        neck.setPosition(sf::Vector2f(7.8f, -1.f));
        neck.setFillColor(shade3d(fabricColor, -20));
        rt.draw(neck);
        });
    // 2026-08-11 2nd batch (finishing out Zone 3): 2 more decals for
    // Apothecary/Alchemist/Winery (Brewery family) and Jeweler (MasonGem
    // family).
    //
    // Bottle-rack decal, shared by all 3 Brewery-family buildings this
    // round -- drawn near-white/neutral (235,235,235) specifically so
    // each caller's own `addBillboard` tint color (green for herbal
    // tinctures, purple for potions, dark red for wine) recolors the SAME
    // baked sprite instead of baking 3 near-identical bottle shapes, the
    // same multiply-tint trick dayNightTint already relies on everywhere.
    const sf::Texture& bottleRackTex = getBillboard3D("bottle_rack", sf::Vector2u(22, 18), [&](sf::RenderTexture& rt) {
        for (int i = 0; i < 3; ++i) {
            float x = 1.f + static_cast<float>(i) * 7.f;
            sf::RectangleShape body(sf::Vector2f(5.f, 10.f));
            body.setPosition(sf::Vector2f(x, 6.f));
            body.setFillColor(sf::Color(235, 235, 235));
            body.setOutlineThickness(0.6f);
            body.setOutlineColor(sf::Color(80, 80, 80));
            rt.draw(body);
            sf::RectangleShape neck(sf::Vector2f(2.f, 4.f));
            neck.setPosition(sf::Vector2f(x + 1.5f, 2.f));
            neck.setFillColor(sf::Color(235, 235, 235));
            rt.draw(neck);
            sf::RectangleShape cork(sf::Vector2f(2.6f, 1.6f));
            cork.setPosition(sf::Vector2f(x + 1.2f, 1.f));
            cork.setFillColor(sf::Color(150, 108, 62));
            rt.draw(cork);
        }
        });
    // Jewelry-display decal (Jeweler's own counter) -- a ring (outlined
    // circle, no fill) with a set gem, plus a beaded chain-and-pendant
    // necklace, distinct from Goldsmith/Gemshop's own loose-gem/ingot
    // decals since a jeweler sells FINISHED pieces, not raw material.
    const sf::Texture& jewelryTex = getBillboard3D("jewelry_display", sf::Vector2u(24, 16), [&](sf::RenderTexture& rt) {
        sf::CircleShape ring(4.f);
        ring.setPosition(sf::Vector2f(2.f, 5.f));
        ring.setFillColor(sf::Color::Transparent);
        ring.setOutlineThickness(1.8f);
        ring.setOutlineColor(sf::Color(220, 180, 60));
        rt.draw(ring);
        sf::CircleShape gem(1.6f);
        gem.setPosition(sf::Vector2f(5.f, 7.f));
        gem.setFillColor(sf::Color(200, 60, 90));
        rt.draw(gem);
        for (int i = 0; i < 4; ++i) {
            sf::CircleShape link(1.f);
            link.setPosition(sf::Vector2f(12.f + static_cast<float>(i) * 2.2f, 3.f + std::sin(static_cast<float>(i) * 0.8f) * 1.5f));
            link.setFillColor(sf::Color(220, 180, 60));
            rt.draw(link);
        }
        sf::CircleShape pendant(2.4f);
        pendant.setPosition(sf::Vector2f(17.f, 9.f));
        pendant.setFillColor(sf::Color(140, 70, 190));
        pendant.setOutlineThickness(0.6f);
        pendant.setOutlineColor(sf::Color(80, 40, 110));
        rt.draw(pendant);
        });
    // Teapot decal (Teahouse's own counter) -- a round pot body, spout,
    // lid, handle, and 2 cups beside it.
    const sf::Texture& teapotTex = getBillboard3D("teapot", sf::Vector2u(28, 18), [&](sf::RenderTexture& rt) {
        sf::CircleShape body(7.f);
        body.setPosition(sf::Vector2f(2.f, 4.f));
        body.setFillColor(sf::Color(200, 60, 70));
        body.setOutlineThickness(0.8f);
        body.setOutlineColor(sf::Color(90, 24, 30));
        rt.draw(body);
        sf::RectangleShape spout(sf::Vector2f(6.f, 2.5f));
        spout.setPosition(sf::Vector2f(14.f, 7.f));
        spout.setRotation(sf::degrees(-20.f));
        spout.setFillColor(sf::Color(200, 60, 70));
        rt.draw(spout);
        sf::CircleShape lid(2.f);
        lid.setPosition(sf::Vector2f(7.5f, 1.f));
        lid.setFillColor(sf::Color(220, 90, 100));
        rt.draw(lid);
        sf::RectangleShape handle(sf::Vector2f(2.f, 6.f));
        handle.setPosition(sf::Vector2f(0.f, 6.f));
        handle.setFillColor(sf::Color(200, 60, 70));
        rt.draw(handle);
        for (float cx : { 20.f, 24.f }) {
            sf::RectangleShape cup(sf::Vector2f(3.f, 3.5f));
            cup.setPosition(sf::Vector2f(cx, 10.f));
            cup.setFillColor(sf::Color(235, 230, 220));
            cup.setOutlineThickness(0.5f);
            cup.setOutlineColor(sf::Color(120, 110, 100));
            rt.draw(cup);
        }
        });
    // 2026-08-11 3rd batch ("剩下一次过都来吧" -- get the rest done in one
    // go): Zone 4/6/7's remaining decals -- a fish, loose pearls, a pie, a
    // roast, and a mini layer cake.
    const sf::Texture& fishTex = getBillboard3D("fish", sf::Vector2u(22, 14), [&](sf::RenderTexture& rt) {
        sf::ConvexShape body;
        body.setPointCount(4);
        body.setPoint(0, sf::Vector2f(2.f, 7.f));
        body.setPoint(1, sf::Vector2f(14.f, 2.f));
        body.setPoint(2, sf::Vector2f(16.f, 7.f));
        body.setPoint(3, sf::Vector2f(14.f, 12.f));
        body.setFillColor(sf::Color(120, 150, 180));
        body.setOutlineThickness(0.8f);
        body.setOutlineColor(sf::Color(50, 70, 90));
        rt.draw(body);
        sf::ConvexShape tail;
        tail.setPointCount(3);
        tail.setPoint(0, sf::Vector2f(2.f, 7.f));
        tail.setPoint(1, sf::Vector2f(-4.f, 2.f));
        tail.setPoint(2, sf::Vector2f(-4.f, 12.f));
        tail.setFillColor(sf::Color(90, 120, 150));
        rt.draw(tail);
        sf::CircleShape eye(1.2f);
        eye.setPosition(sf::Vector2f(12.f, 5.f));
        eye.setFillColor(sf::Color(20, 20, 20));
        rt.draw(eye);
        });
    const sf::Texture& pearlTex = getBillboard3D("pearls", sf::Vector2u(20, 14), [&](sf::RenderTexture& rt) {
        const sf::Vector2f pearls[] = { {2.f, 6.f}, {8.f, 3.f}, {13.f, 6.f}, {17.f, 4.f}, {5.f, 9.f}, {11.f, 10.f} };
        for (const auto& p : pearls) {
            sf::CircleShape pearl(2.4f);
            pearl.setPosition(p);
            pearl.setFillColor(sf::Color(240, 236, 228));
            pearl.setOutlineThickness(0.4f);
            pearl.setOutlineColor(sf::Color(190, 186, 176));
            rt.draw(pearl);
            sf::CircleShape shine(0.8f);
            shine.setPosition(p + sf::Vector2f(0.5f, 0.3f));
            shine.setFillColor(sf::Color(255, 255, 255, 200));
            rt.draw(shine);
        }
        });
    const sf::Texture& pieTex = getBillboard3D("pie", sf::Vector2u(20, 14), [&](sf::RenderTexture& rt) {
        sf::CircleShape crust(9.f);
        crust.setPosition(sf::Vector2f(1.f, 1.f));
        crust.setFillColor(sf::Color(200, 150, 90));
        crust.setOutlineThickness(1.f);
        crust.setOutlineColor(sf::Color(120, 84, 48));
        rt.draw(crust);
        sf::CircleShape filling(6.f);
        filling.setPosition(sf::Vector2f(4.f, 4.f));
        filling.setFillColor(sf::Color(178, 60, 54));
        rt.draw(filling);
        for (int i = 0; i < 4; ++i) {
            float ang = static_cast<float>(i) * 0.7854f;
            sf::RectangleShape lattice(sf::Vector2f(13.f, 1.4f));
            lattice.setOrigin(sf::Vector2f(6.5f, 0.7f));
            lattice.setPosition(sf::Vector2f(10.f, 10.f));
            lattice.setRotation(sf::degrees(ang * 57.3f));
            lattice.setFillColor(sf::Color(160, 112, 62));
            rt.draw(lattice);
        }
        });
    const sf::Texture& roastTex = getBillboard3D("roast", sf::Vector2u(22, 14), [&](sf::RenderTexture& rt) {
        sf::RectangleShape skewer(sf::Vector2f(22.f, 1.6f));
        skewer.setPosition(sf::Vector2f(0.f, 6.f));
        skewer.setFillColor(sf::Color(90, 90, 90));
        rt.draw(skewer);
        sf::CircleShape meat(6.5f);
        meat.setPosition(sf::Vector2f(5.f, 0.5f));
        meat.setFillColor(sf::Color(160, 90, 50));
        meat.setOutlineThickness(1.f);
        meat.setOutlineColor(sf::Color(90, 46, 24));
        rt.draw(meat);
        sf::CircleShape shine(2.2f);
        shine.setPosition(sf::Vector2f(8.f, 3.f));
        shine.setFillColor(sf::Color(210, 150, 100));
        rt.draw(shine);
        });
    const sf::Texture& cakeTex = getBillboard3D("cake", sf::Vector2u(18, 20), [&](sf::RenderTexture& rt) {
        sf::RectangleShape tier1(sf::Vector2f(16.f, 7.f));
        tier1.setPosition(sf::Vector2f(1.f, 11.f));
        tier1.setFillColor(sf::Color(240, 220, 225));
        tier1.setOutlineThickness(0.8f);
        tier1.setOutlineColor(sf::Color(200, 160, 170));
        rt.draw(tier1);
        sf::RectangleShape tier2(sf::Vector2f(11.f, 6.f));
        tier2.setPosition(sf::Vector2f(3.5f, 5.f));
        tier2.setFillColor(sf::Color(250, 235, 238));
        tier2.setOutlineThickness(0.8f);
        tier2.setOutlineColor(sf::Color(200, 160, 170));
        rt.draw(tier2);
        sf::CircleShape cherry(1.6f);
        cherry.setPosition(sf::Vector2f(8.f, 1.f));
        cherry.setFillColor(sf::Color(190, 40, 60));
        rt.draw(cherry);
        });
    const sf::Texture& herbTuftTex = getBillboard3D("herb_tuft", sf::Vector2u(14, 14), [&](sf::RenderTexture& rt) {
        sf::CircleShape tuft(5.f);
        tuft.setPosition(sf::Vector2f(2.f, 2.f));
        tuft.setFillColor(sf::Color(110, 160, 80));
        rt.draw(tuft);
        });
    // Popcorn-bucket decal (Popcorn Stand's own counter).
    const sf::Texture& popcornTex = getBillboard3D("popcorn", sf::Vector2u(18, 20), [&](sf::RenderTexture& rt) {
        sf::ConvexShape bucket;
        bucket.setPointCount(4);
        bucket.setPoint(0, sf::Vector2f(3.f, 10.f));
        bucket.setPoint(1, sf::Vector2f(15.f, 10.f));
        bucket.setPoint(2, sf::Vector2f(13.f, 20.f));
        bucket.setPoint(3, sf::Vector2f(5.f, 20.f));
        bucket.setFillColor(sf::Color(220, 60, 60));
        bucket.setOutlineThickness(0.8f);
        bucket.setOutlineColor(sf::Color(140, 30, 30));
        rt.draw(bucket);
        const sf::Vector2f puffs[] = { {2.f, 4.f}, {7.f, 1.f}, {12.f, 3.f}, {16.f, 6.f}, {5.f, 8.f}, {10.f, 6.f} };
        for (const auto& p : puffs) {
            sf::CircleShape puff(2.6f);
            puff.setPosition(p);
            puff.setFillColor(sf::Color(250, 235, 200));
            puff.setOutlineThickness(0.5f);
            puff.setOutlineColor(sf::Color(210, 190, 150));
            rt.draw(puff);
        }
        });
    // 2026-08-11 rework (Vineyard's own trellis redo above) -- was a
    // single flat purple dot; now an actual tapering grape-bunch
    // silhouette (9 overlapping circles narrowing to a point, each with a
    // small shine) under a small leaf, matching the bigger 14-unit display
    // size the new hanging-cluster billboards use.
    const sf::Texture& grapeTex = getBillboard3D("grape_cluster", sf::Vector2u(16, 20), [&](sf::RenderTexture& rt) {
        sf::ConvexShape leaf;
        leaf.setPointCount(4);
        leaf.setPoint(0, sf::Vector2f(8.f, 0.f));
        leaf.setPoint(1, sf::Vector2f(14.f, 3.f));
        leaf.setPoint(2, sf::Vector2f(8.f, 6.f));
        leaf.setPoint(3, sf::Vector2f(2.f, 3.f));
        leaf.setFillColor(sf::Color(80, 120, 60));
        rt.draw(leaf);
        const sf::Vector2f grapes[] = {
            {4.f, 5.f}, {9.f, 5.f}, {6.5f, 7.5f}, {2.5f, 9.f}, {7.f, 9.5f}, {11.f, 9.f},
            {4.5f, 12.5f}, {9.f, 12.5f}, {6.5f, 15.f},
        };
        for (const auto& g : grapes) {
            sf::CircleShape grape(2.6f);
            grape.setPosition(g);
            grape.setFillColor(sf::Color(110, 60, 130));
            grape.setOutlineThickness(0.4f);
            grape.setOutlineColor(sf::Color(60, 30, 74));
            rt.draw(grape);
            sf::CircleShape shine(0.8f);
            shine.setPosition(g + sf::Vector2f(0.6f, 0.4f));
            shine.setFillColor(sf::Color(170, 120, 190, 180));
            rt.draw(shine);
        }
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

    // Locked-business padlock badge (2026-08-12, "那些还没有解锁产业的...
    // 一个色块方块在那边很丑" -- locked businesses looking like an ugly
    // flat-colored block): same gold-lock look the 2D world's drawLockOverlay
    // used (body + circular shackle outline), baked to a small billboard
    // texture -- see the building loop below for how it replaces the old
    // "just desaturate the box toward gray" treatment entirely.
    const sf::Texture& lockTex = getBillboard3D("locked_padlock", sf::Vector2u(32, 40), [&](sf::RenderTexture& rt) {
        sf::Color goldColor(225, 205, 110);
        sf::Color darkColor(40, 30, 10);
        sf::CircleShape shackle(9.f);
        shackle.setPosition(sf::Vector2f(7.f, 2.f));
        shackle.setFillColor(sf::Color::Transparent);
        shackle.setOutlineThickness(3.5f);
        shackle.setOutlineColor(goldColor);
        rt.draw(shackle);
        sf::RectangleShape body(sf::Vector2f(24.f, 20.f));
        body.setPosition(sf::Vector2f(4.f, 18.f));
        body.setFillColor(goldColor);
        body.setOutlineThickness(1.5f);
        body.setOutlineColor(darkColor);
        rt.draw(body);
        sf::CircleShape keyhole(3.f);
        keyhole.setPosition(sf::Vector2f(13.f, 24.f));
        keyhole.setFillColor(darkColor);
        rt.draw(keyhole);
        });

    // Buildings: walls + a real pitched roof. Wall/roof colors come from
    // the same labelColor the 2D drawCottageShape/etc. already derive from.
    // Mirrors drawBuilding's 2D state machine (see its own comment) for
    // locked/construction, minus the 2D version's signboard/material-list
    // detail -- see this file's header comment for what's left as a
    // follow-up.
    for (const auto& b : z.buildings) {
        bool locked = game_.isBusinessLocked(b.id);
        // 2026-08-12 ("那些还没有解锁产业的...一个色块方块在那边很丑" --
        // locked businesses looking like an ugly flat-colored block): used
        // to fall all the way through this loop to the generic dimmed
        // box+roof at the bottom (just a desaturated/darkened version of
        // whatever labelColor the business would normally use -- still the
        // same BOX SHAPE as a built one, just grayer, which is exactly what
        // read as "ugly"). A locked business hasn't even unlocked the ability to
        // start building here yet, which is arguably less "claimed" than an
        // unstarted-but-buildable plot (state 1/3 below, addUndevelopedPlot)
        // -- so it gets that exact same natural-clearing treatment (no
        // building shape at all, just scattered rocks/sapling/flowers over
        // the zone's own grass) instead, plus one thing state 1 doesn't
        // need: a small padlock-topped signpost, so "buildable now, just
        // haven't started" and "still locked" stay visually distinct from
        // each other rather than looking identical.
        if (locked) {
            addUndevelopedPlot(quads, viewProj, windowSize_, eye, b, lc, billboardRight, treeTex, bushTex, flowerTex, herbTuftTex, billboardDayNight);
            Vec3 postBase(b.position.x + b.size.x * 0.5f, 0.f, b.position.y + b.size.y * 0.5f);
            addBox(quads, viewProj, windowSize_, eye, postBase, Vec3(3.f, 22.f, 3.f), sf::Color(94, 62, 32), lc);
            addBillboard(quads, viewProj, windowSize_, billboardRight, postBase + Vec3(0.f, 22.f, 0.f), 20.f, 24.f, lockTex, sf::Color::White);
            continue;
        }
        ConstructionInfo ci = game_.businessConstructionInfo(b.id);
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
        // 2D id dispatch for the same 9 archetypes + Cottage. `locked` is
        // always false down here now (see this loop's own early `continue`
        // above) -- kept as an explicit guard rather than removed, since
        // stripping it would mean re-indenting this entire ~40-business
        // dispatch block for no behavior change.
        if (!locked) {
            if (b.id == "farm") { addFarmProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, chickenTex, pigTex, veggieTex, flowerTex, forageTex, cabbageTex, game_.farmCropId()); continue; }
            if (b.id == "mine") { addMineProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, sf::Color(114, 106, 100)); continue; }
            if (b.id == "goldmine") { addGoldMineProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, goldOreTex); continue; }
            if (b.id == "lumber") { addLumberProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
            if (b.id == "quarry") { addQuarryProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
            if (b.id == "sheep") { addPastureProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, sheepTex, chickenTex, pigTex, dogTex, billboardDayNight); continue; }
            if (b.id == "orchard") { addOrchardProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, appleTreeTex, pearTreeTex, fruitCrateTex, billboardDayNight); continue; }
            if (b.id == "herbgarden") { addHerbGardenProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, herbTuftTex, billboardDayNight); continue; }
            if (b.id == "vineyard") { addVineyardProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, grapeTex, billboardDayNight); continue; }
            if (b.id == "dairyfarm") { addDairyFarmProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, cowTex, billboardDayNight); continue; }
            if (b.id == "beehive") { addBeehiveProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, flowerTex); continue; }
            if (b.id == "trapper") { addTrapperProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, peltTex); continue; }
            if (b.id == "teafield") { addTeaFieldProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, teaBushTex, billboardDayNight); continue; }
            if (b.id == "flaxfield") { addFlaxFieldProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, flaxFlowerTex, billboardDayNight); continue; }
            if (b.id == "doctor") { addClinicBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, billboardRight, glowTex, stoneTex, shingleTex, archTex, crossTex, flowerTex); continue; }
            if (b.id == "staff") { addRecruitmentCenterBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex); continue; }
            if (b.id == "bank") { addBankBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, vaultTex, cabinetTex); continue; }
            if (b.id == "sleep") { addInnBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, keysTex, cabinetTex); continue; }
            if (b.id == "eat") { addKitchenBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, archTex); continue; }
            if (b.id == "townhall") { addTownHallBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex, archTex, clockTex); continue; }
            if (b.id == "market") { addMarketBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, crateTex); continue; }
            if (b.id == "warehouse") { addWarehouseBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, plasterTex, shingleTex); continue; }
            if (b.id == "storefront") { addStorefrontBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, goodsTex); continue; }
            // 2026-08-11: re-enabled after a temporary diagnostic hide -- the
            // "pale shapes" report near Sawmill turned out to be Mason's own
            // stone cluster (see addMasonBuilding's own updated comment on
            // why, and the real fix applied there) bleeding into view
            // because Mason (`{370,480}`) shares its exact X with Sawmill
            // (`{370,180}`), not anything about Sawmill's own model -- this
            // hide served its diagnostic purpose and is reverted.
            if (b.id == "sawmill") { addSawmillBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, wheelTex, sawBladeTex); continue; }
            if (b.id == "mason") { addMasonBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, statueTex); continue; }
            if (b.id == "bakery") { addBakeryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, breadTex); continue; }
            if (b.id == "preserve") { addPreserveBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, jamJarsTex, pressWheelTex, fruitCrateTex); continue; }
            if (b.id == "goldsmith") { addGoldsmithBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, goldBarTex, gemTrayTex, goldOreTex); continue; }
            if (b.id == "textile") { addTextileMillBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, loomTex, yarnTex); continue; }
            if (b.id == "smelter") { addSmelterBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, ironBarTex); continue; }
            if (b.id == "blacksmith") { addBlacksmithBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, weaponRackTex); continue; }
            if (b.id == "gemshop") { addGemshopBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, gemTrayTex, grindWheelTex); continue; }
            if (b.id == "carpenter") { addCarpenterBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, furnitureTex); continue; }
            if (b.id == "tailor") { addTailorBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, dressFormTex, yarnTex); continue; }
            if (b.id == "apothecary") { addApothecaryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, archTex, herbTuftTex, bottleRackTex); continue; }
            if (b.id == "alchemist") { addAlchemistBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, bottleRackTex); continue; }
            if (b.id == "winery") { addWineryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, pressWheelTex, bottleRackTex); continue; }
            if (b.id == "jeweler") { addJewelerBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, jewelryTex); continue; }
            if (b.id == "creamery") { addCreameryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, pressWheelTex, bottleRackTex); continue; }
            if (b.id == "meadery") { addMeaderyBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, bottleRackTex); continue; }
            if (b.id == "tannery") { addTanneryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, peltTex); continue; }
            if (b.id == "linenmill") { addLinenMillBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, pressWheelTex, yarnTex); continue; }
            if (b.id == "teahouse") { addTeahouseBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex, teapotTex, teaBushTex); continue; }
            if (b.id == "giftbasket") { addGiftBasketBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, bottleRackTex, teaBushTex); continue; }
            if (b.id == "seasalt") { addSeaSaltProps(quads, viewProj, windowSize_, eye, b, lc); continue; }
            if (b.id == "pearlfarm") { addPearlFarmProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, pearlTex); continue; }
            if (b.id == "fishing") { addFishingProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fishTex); continue; }
            if (b.id == "shipyard") { addShipyardBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight); continue; }
            if (b.id == "port") { addPortBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight); continue; }
            if (b.id == "pearlatelier") { addPearlAtelierBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, pearlTex); continue; }
            if (b.id == "jamkitchen") { addJamKitchenBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, jamJarsTex); continue; }
            if (b.id == "pieshop") { addPieShopBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, pieTex); continue; }
            if (b.id == "roaststand") { addRoastStandBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, roastTex); continue; }
            if (b.id == "picklinghouse") { addPicklingHouseBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, bottleRackTex); continue; }
            if (b.id == "honeyrefinery") { addHoneyRefineryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, bottleRackTex); continue; }
            if (b.id == "cakeshop") { addCakeShopBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, cakeTex); continue; }
            if (b.id == "artisanbakery") { addArtisanBakeryBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, breadTex); continue; }
            if (b.id == "popcornstand") { addPopcornStandBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, popcornTex); continue; }
            if (b.id == "juicebar") { addJuiceBarBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, bottleRackTex); continue; }
            if (b.id == "cannery") { addCanneryBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fishTex); continue; }
            if (b.id == "smokehouse") { addSmokehouseBuilding(quads, viewProj, windowSize_, eye, b, lc, kBuildingHeight, kRoofRise, billboardRight, glowTex, stoneTex, shingleTex, flowerTex, fishTex); continue; }
            if (b.id == "deepsea") { addDeepSeaProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fishTex); continue; }
            if (b.id == "sushibar") { addSushiBarBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fishTex); continue; }
            if (b.id == "fishermanplatter") { addFishermanPlatterBuilding(quads, viewProj, windowSize_, eye, b, lc, billboardRight, fishTex); continue; }
            if (b.id == "island_ferry") { addIslandFerryProps(quads, viewProj, windowSize_, eye, b, lc, billboardRight, glowTex); continue; }
        }

        // `locked` never reaches here (this loop's own early `continue`
        // above handles it with the natural-clearing + padlock signpost
        // treatment instead) -- this generic box+roof is purely the
        // fallback for any built-but-not-yet-hero-shaped business.
        sf::Color wallColor = shade3d(b.labelColor, -70);
        sf::Color roofColor = shade3d(b.labelColor, -110);
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

    // Bloom pass: one additive glow billboard per BLOOM-marked point light
    // (street lamps -- see PointLight3D's own comment on why the generic
    // per-building shading light is excluded, 2026-08-12), so a lit light
    // actually reads as a glowing source instead of only ever brightening
    // whatever surface it happens to shine on via litColor(). Skipped
    // near-entirely by day (intensity already carries lightBoost's
    // day/night scaling) -- the `< 0.02f` cutoff just avoids pushing a
    // fully-transparent quad (and its draw-call/batch-flush cost) for every
    // light all day long.
    for (const auto& light : lc.lights) {
        if (!light.bloom) continue;
        float strength = std::clamp(light.intensity / 46.f, 0.f, 1.f); // 46 ~= the un-boosted intensity both light sources above are scaled from (48/44)
        if (strength < 0.02f) continue;
        // 2026-08-10, 2nd lighting-strengthen follow-up ("夜晚的灯有点暗" --
        // nighttime lights still read a bit dim): bloom alpha 195->225 and
        // glow radius 40+62*s -> 46+70*s, on top of the earlier 150->195/
        // 34+46*s pass -- same "every lamp/window bloom halo routes through
        // this one loop" lever as before. Also now safe to push bigger than
        // last time specifically because glowTex itself just got fixed
        // (see its own bake-site comment) -- it used to clip into a visible
        // square at any size much past its old baked proportions, so this
        // round's bigger stretch would have made that worse; now it's a
        // genuine soft circle at any size.
        sf::Color tint(
            clamp8_3d(static_cast<int>(light.color.x * 255.f)),
            clamp8_3d(static_cast<int>(light.color.y * 255.f)),
            clamp8_3d(static_cast<int>(light.color.z * 255.f)),
            clamp8_3d(static_cast<int>(225.f * strength)));
        float glowSize = 46.f + 70.f * strength;
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
            if (locked) labelY = 52.f; // 2026-08-12: locked businesses no longer render as a box+roof (see the building loop's own comment above) -- clears the padlock signpost's own top (post height 22 + billboard height 24 = 46), same "flat-plot archetype" clearance idea as the 48/54 values below
            else if (ci.requiresConstruction) labelY = ci.inProgress ? (kBuildingHeight + kRoofRise + 20.f) : 34.f; // in-progress sites now have a real truss/wall (see addConstructionSiteProps) reaching wallH+roofRise -- clear of that instead of the old flat lot's low 34
            else if (!locked && (b.id == "mine" || b.id == "goldmine")) labelY = 74.f; // above the mound's own 62-high apex
            else if (!locked && (b.id == "farm" || b.id == "lumber" || b.id == "quarry" || b.id == "sheep" ||
                b.id == "orchard" || b.id == "herbgarden" || b.id == "vineyard" || b.id == "dairyfarm" ||
                b.id == "beehive" || b.id == "trapper" || b.id == "teafield" || b.id == "flaxfield" ||
                b.id == "seasalt" || b.id == "pearlfarm")) labelY = 48.f; // flat-plot archetypes -- above their tallest prop, not a roofline that doesn't exist here (44 -> 48 alongside Orchard's own 2026-08-11 tree-height bump, 42 -> 46). 2026-08-11 2nd/3rd follow-ups: added the Zone 5 and Zone 4 flat-plot businesses here too -- they'd have fallen through to the generic `kBuildingHeight + kRoofRise + 14` default otherwise, floating the label way above a lot with no actual building on it.
            else if (!locked && (b.id == "fishing" || b.id == "shipyard" || b.id == "port" || b.id == "cannery" ||
                b.id == "deepsea" || b.id == "fishermanplatter" || b.id == "island_ferry")) labelY = 54.f; // Dock family (addDockShell) -- no wall/roof, just a flat deck; 54 clears every Dock business's own tallest prop (Port's own signal mast, the tallest at ~48)
            else if (!locked && b.id == "staff") labelY = kBuildingHeight * 1.15f + kRoofRise * 1.5f + 16.f; // taller hero building -- see addRecruitmentCenterBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "bank") labelY = kBuildingHeight * 1.05f + kRoofRise * 0.85f + 16.f; // see addBankBuilding's own wallH2/roofRise math
            else if (!locked && b.id == "sleep") labelY = kBuildingHeight * 1.12f + kRoofRise * 1.4f + 16.f; // see addInnBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "eat") labelY = kBuildingHeight * 1.02f + kRoofRise * 1.35f + 16.f; // see addKitchenBuilding's own wallH2/gableRise math
            else if (!locked && b.id == "townhall") labelY = kBuildingHeight * 1.3f + kRoofRise * 1.3f + 20.f; // above the main gable's own peak (see addTownHallBuilding) -- not the much-taller clock tower/spire, which sits well off the footprint's horizontal center this label anchors to
            else if (!locked && b.id == "market") labelY = 74.f; // no wall/roof here at all -- above the canopy's own back-edge height (58, see addMarketBuilding) with clearance, same "flat-plot archetype" idea as the mine mound/farm rows above
            else if (!locked && (b.id == "teahouse" || b.id == "giftbasket" || b.id == "popcornstand" ||
                b.id == "juicebar" || b.id == "sushibar")) labelY = 74.f; // Stall family -- same addStripedAwning backH (58) as Market above, same clearance math
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
