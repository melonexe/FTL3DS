#include "anim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xml.h"

bool animdb_open(AnimDb* db, FtlDat* dat) {
    db->xml[0] = (char*)ftldat_read(dat, "data/animations.xml", NULL);
    db->xml[1] = (char*)ftldat_read(dat, "data/dlcAnimations.xml", NULL);
    return db->xml[0] != NULL;
}

void animdb_close(AnimDb* db) {
    free(db->xml[0]);
    free(db->xml[1]);
    memset(db, 0, sizeof(*db));
}

// Finds <tag name="name" ...> in either file. Some names are defined twice; like the game,
// the last definition wins.
static const char* find_named(const AnimDb* db, const char* tag, const char* name, const char** end) {
    const char* found = NULL;
    const char* foundEnd = NULL;
    for (int i = 0; i < 2; i++) {
        const char* p = db->xml[i];
        const char* t;
        char buf[48];
        while (p && (t = xml_find_tag(p, tag, end)) != NULL) {
            p = *end + 1;
            if (xml_attr(t, *end, "name", buf, sizeof(buf)) && strcmp(buf, name) == 0) {
                found = t;
                foundEnd = *end;
            }
        }
    }
    *end = foundEnd;
    return found;
}

bool animdb_sheet(const AnimDb* db, const char* name, SheetInfo* out) {
    const char* end;
    const char* t = find_named(db, "animSheet", name, &end);
    char file[48];
    if (!t || !xml_text(end, file, sizeof(file))) return false;
    snprintf(out->file, sizeof(out->file), "img/%s", file);
    return xml_attr_int(t, end, "w", &out->w) && xml_attr_int(t, end, "h", &out->h) &&
           xml_attr_int(t, end, "fw", &out->fw) && xml_attr_int(t, end, "fh", &out->fh) && out->fw > 0 &&
           out->fh > 0;
}

bool animdb_anim(const AnimDb* db, const char* name, AnimInfo* out) {
    const char* end;
    const char* t = find_named(db, "anim", name, &end);
    if (!t) return false;
    const char* close = strstr(end, "</anim>");
    const char* e;
    const char* s;
    if (!(s = xml_find_tag_before(end, close, "sheet", &e)) || !xml_text(e, out->sheet, sizeof(out->sheet)))
        return false;
    if (!(s = xml_find_tag_before(end, close, "desc", &e)) || !xml_attr_int(s, e, "length", &out->length) ||
        !xml_attr_int(s, e, "x", &out->x) || !xml_attr_int(s, e, "y", &out->y))
        return false;
    char buf[16];
    out->time = 1.0f;
    if ((s = xml_find_tag_before(end, close, "time", &e)) && xml_text(e, buf, sizeof(buf))) out->time = (float)atof(buf);
    return out->length > 0;
}

bool animdb_weapon(const AnimDb* db, const char* name, WeaponAnimInfo* out) {
    const char* end;
    const char* t = find_named(db, "weaponAnim", name, &end);
    if (!t) return false;
    const char* close = strstr(end, "</weaponAnim>");
    const char* e;
    const char* s;
    char buf[16];
    memset(out, 0, sizeof(*out));
    if (!(s = xml_find_tag_before(end, close, "sheet", &e)) || !xml_text(e, out->sheet, sizeof(out->sheet))) return false;
    if (!(s = xml_find_tag_before(end, close, "desc", &e)) || !xml_attr_int(s, e, "length", &out->length)) return false;
    xml_attr_int(s, e, "x", &out->x);
    xml_attr_int(s, e, "y", &out->y);
    if ((s = xml_find_tag_before(end, close, "chargedFrame", &e)) && xml_text(e, buf, sizeof(buf)))
        out->chargedFrame = atoi(buf);
    if ((s = xml_find_tag_before(end, close, "fireFrame", &e)) && xml_text(e, buf, sizeof(buf))) out->fireFrame = atoi(buf);
    if ((s = xml_find_tag_before(end, close, "mountPoint", &e))) {
        xml_attr_int(s, e, "x", &out->mountX);
        xml_attr_int(s, e, "y", &out->mountY);
    }
    return out->length > 0;
}

static Tex3DS_SubTexture frame_subtex(const Texture* t, int fw, int fh, int col, int rowFromTop) {
    const C3D_Tex* tex = t->image.tex;
    Tex3DS_SubTexture sub = {
        .width = (u16)fw,
        .height = (u16)fh,
        .left = (float)(col * fw) / tex->width,
        .right = (float)((col + 1) * fw) / tex->width,
        .top = 1.0f - (float)(rowFromTop * fh) / tex->height,
        .bottom = 1.0f - (float)((rowFromTop + 1) * fh) / tex->height,
    };
    return sub;
}

void sheet_draw_frame_rotated(const Texture* t, int fw, int fh, int col, int rowFromTop, float cx, float cy, float scale,
                              float angle) {
    if (!t->loaded) return;
    Tex3DS_SubTexture sub = frame_subtex(t, fw, fh, col, rowFromTop);
    C2D_Image img = {t->image.tex, &sub};
    C2D_DrawImageAtRotated(img, cx, cy, 0.0f, angle, NULL, scale, scale);
}

void sheet_draw_frame(const Texture* t, int fw, int fh, int col, int rowFromTop, float x, float y, float scale) {
    if (!t->loaded) return;
    const C3D_Tex* tex = t->image.tex;
    Tex3DS_SubTexture sub = {
        .width = (u16)fw,
        .height = (u16)fh,
        .left = (float)(col * fw) / tex->width,
        .right = (float)((col + 1) * fw) / tex->width,
        .top = 1.0f - (float)(rowFromTop * fh) / tex->height,
        .bottom = 1.0f - (float)((rowFromTop + 1) * fh) / tex->height,
    };
    C2D_Image img = {t->image.tex, &sub};
    C2D_DrawImageAt(img, x, y, 0.0f, NULL, scale, scale);
}
