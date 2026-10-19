#pragma once
// Independent PDF inspector for the redaction tests.
//
// Deliberately shares NO code with src/redact/Redaction.cpp and does not use PDFium: it scans
// "N G obj ... endobj" itself, inflates streams with zlib, expands object streams, and applies its
// OWN copy of the output allowlist (docs/REDACTION.md section 6).  If the engine and this file
// ever disagree, the test fails and a human decides which one is wrong.

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace inspect {

using Bytes = std::vector<std::uint8_t>;

struct Image {
    int objNum = 0;
    int width = 0, height = 0;
    Bytes rgb;               // width*height*3, decoded
};

struct Object {
    int num = 0;
    std::string dict;        // text between "obj" and "stream"/"endobj"
    bool hasStream = false;
    Bytes raw;               // stream bytes as stored
    Bytes decoded;           // after FlateDecode (empty if undecodable)
    bool decodedOk = false;
    std::string filter;      // text of /Filter value if any
};

struct Inspection {
    std::vector<Object> objects;
    std::vector<std::string> problems;     // allowlist / structure violations
    std::string everything;                // all dict text + all decoded non-image streams + ObjStm bodies
    std::vector<Image> pageImages;         // one per page, in page order
    int pageCount = 0;
    int startxrefCount = 0, eofCount = 0;
    bool hasObjStm = false;
    bool mappedAllPages = false;
};

inline bool inflateBytes(const std::uint8_t* d, size_t n, Bytes& out, size_t cap = 1ull << 30) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(d);
    zs.avail_in = static_cast<uInt>(n);
    Bytes buf(1 << 16);
    int rc;
    do {
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return false; }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
        if (out.size() > cap) { inflateEnd(&zs); return false; }
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

// Own copy of the output allowlist; intentionally NOT shared with the engine.
inline const std::set<std::string>& allowedNames() {
    static const std::set<std::string> k = {
        "Type", "Catalog", "Pages", "Page", "Kids", "Count", "Parent", "MediaBox", "Resources", "XObject", "Image",
        "Subtype", "Width", "Height", "BitsPerComponent", "ColorSpace", "DeviceRGB", "DeviceGray", "Filter",
        "FlateDecode", "Length", "Contents", "Rotate", "ExtGState", "ca", "CA", "BM", "Normal", "Info", "Creator",
        "CreationDate", "ID", "Root", "Size", "FXE1", "FXX1", "ProcSet", "PDF", "ImageC", "Producer"};
    return k;
}
inline const std::set<std::string>& forbiddenNames() {
    static const std::set<std::string> k = {
        "Prev", "Font", "Annots", "AcroForm", "Names", "Outlines", "Metadata", "EmbeddedFiles", "EmbeddedFile", "JS",
        "JavaScript", "OpenAction", "AA", "Dests", "PageLabels", "StructTreeRoot", "OCProperties", "Thumb", "PieceInfo",
        "Encrypt", "Sig", "ByteRange", "DSS", "Perms", "ObjStm", "XRef", "Lang", "ViewerPreferences", "AF", "Filespec"};
    return k;
}
inline const std::set<std::string>& allowedOps() {
    static const std::set<std::string> k = {"q", "Q", "cm", "Do", "gs", "RG", "rg", "w", "J", "j"};
    return k;
}

inline bool isDelim(unsigned char c) { return c <= ' ' || std::strchr("()<>[]{}/%", c) != nullptr; }

inline std::vector<std::string> namesIn(const std::string& s) {
    std::vector<std::string> out;
    bool inStr = false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '(') ++depth;
            else if (c == ')') { if (depth == 0) inStr = false; else --depth; }
            continue;
        }
        if (c == '(') { inStr = true; depth = 0; continue; }
        if (c != '/') continue;
        size_t j = i + 1;
        while (j < s.size() && !isDelim(static_cast<unsigned char>(s[j]))) ++j;
        out.push_back(s.substr(i + 1, j - i - 1));
        i = j - 1;
    }
    return out;
}

inline int refAfter(const std::string& d, const std::string& key) {
    size_t p = d.find("/" + key);
    while (p != std::string::npos) {
        size_t q = p + key.size() + 1;
        if (q >= d.size() || isDelim(static_cast<unsigned char>(d[q]))) {
            while (q < d.size() && std::isspace(static_cast<unsigned char>(d[q]))) ++q;
            if (q < d.size() && std::isdigit(static_cast<unsigned char>(d[q]))) return std::atoi(d.c_str() + q);
            return -1;
        }
        p = d.find("/" + key, p + 1);
    }
    return -1;
}

inline long long intAfter(const std::string& d, const std::string& key) {
    size_t p = d.find("/" + key);
    while (p != std::string::npos) {
        size_t q = p + key.size() + 1;
        if (q >= d.size() || isDelim(static_cast<unsigned char>(d[q]))) {
            while (q < d.size() && std::isspace(static_cast<unsigned char>(d[q]))) ++q;
            if (q < d.size() && std::isdigit(static_cast<unsigned char>(d[q]))) return std::atoll(d.c_str() + q);
            return -1;
        }
        p = d.find("/" + key, p + 1);
    }
    return -1;
}

inline std::vector<Object> scanObjects(const Bytes& pdf) {
    std::vector<Object> objs;
    const std::string all(reinterpret_cast<const char*>(pdf.data()), pdf.size());
    size_t pos = 0;
    while (true) {
        size_t o = all.find(" obj", pos);
        if (o == std::string::npos) break;
        // walk back over "N G"
        size_t e = o;
        size_t gEnd = e;
        size_t gStart = gEnd;
        while (gStart > 0 && std::isdigit(static_cast<unsigned char>(all[gStart - 1]))) --gStart;
        if (gStart == gEnd || gStart == 0 || all[gStart - 1] != ' ') { pos = o + 4; continue; }
        size_t nEnd = gStart - 1, nStart = nEnd;
        while (nStart > 0 && std::isdigit(static_cast<unsigned char>(all[nStart - 1]))) --nStart;
        if (nStart == nEnd || (nStart > 0 && all[nStart - 1] != '\n' && all[nStart - 1] != '\r' && all[nStart - 1] != ' ')) { pos = o + 4; continue; }
        Object ob;
        ob.num = std::atoi(all.c_str() + nStart);
        size_t bodyStart = o + 4;
        size_t endobj = std::string::npos;
        size_t st = all.find("stream", bodyStart);
        size_t eo = all.find("endobj", bodyStart);
        // Does a stream belong to this object (keyword before endobj, preceded by dict end)?
        bool hasStream = false;
        while (st != std::string::npos && (eo == std::string::npos || st < eo)) {
            if (st >= 3 && all.compare(st - 3, 3, "end") == 0) { st = all.find("stream", st + 6); continue; }
            size_t a = st + 6;
            if (a < all.size() && (all[a] == '\n' || all[a] == '\r')) { hasStream = true; break; }
            st = all.find("stream", st + 6);
        }
        if (hasStream) {
            ob.hasStream = true;
            ob.dict = all.substr(bodyStart, st - bodyStart);
            size_t ds = st + 6;
            if (all[ds] == '\r') ++ds;
            if (ds < all.size() && all[ds] == '\n') ++ds;
            long long len = intAfter(ob.dict, "Length");
            size_t dend = std::string::npos;
            if (len >= 0 && ds + static_cast<size_t>(len) <= all.size()) {
                size_t t = ds + static_cast<size_t>(len);
                while (t < all.size() && (all[t] == '\r' || all[t] == '\n' || all[t] == ' ')) ++t;
                if (all.compare(t, 9, "endstream") == 0) dend = ds + static_cast<size_t>(len);
            }
            if (dend == std::string::npos) {
                size_t es = all.find("endstream", ds);
                dend = es == std::string::npos ? all.size() : es;
            }
            ob.raw.assign(pdf.begin() + ds, pdf.begin() + dend);
            size_t es = all.find("endstream", dend);
            endobj = es == std::string::npos ? all.size() : all.find("endobj", es);
        } else {
            endobj = eo;
            ob.dict = all.substr(bodyStart, (eo == std::string::npos ? all.size() : eo) - bodyStart);
        }
        // filter text
        size_t fp = ob.dict.find("/Filter");
        if (fp != std::string::npos) {
            size_t q = fp + 7;
            size_t end = ob.dict.find_first_of(">/", q + (ob.dict[q] == ' ' ? 1 : 0) + 1);
            // collect up to the matching ']' or next key
            size_t sb = ob.dict.find_first_not_of(" \r\n", q);
            if (sb != std::string::npos && ob.dict[sb] == '[') end = ob.dict.find(']', sb), ob.filter = ob.dict.substr(sb, end - sb + 1);
            else if (sb != std::string::npos) { size_t e2 = ob.dict.find_first_of(" \r\n/>", sb + 1); ob.filter = ob.dict.substr(sb, e2 - sb); }
        }
        if (ob.hasStream) {
            if (ob.filter == "/FlateDecode" || ob.filter == "[/FlateDecode]") {
                ob.decodedOk = inflateBytes(ob.raw.data(), ob.raw.size(), ob.decoded);
            } else if (ob.filter.empty()) {
                ob.decoded = ob.raw; ob.decodedOk = true;
            }
        }
        objs.push_back(std::move(ob));
        pos = endobj == std::string::npos ? all.size() : endobj + 6;
    }
    return objs;
}

inline Inspection inspectOutput(const Bytes& pdf) {
    Inspection r;
    auto problem = [&](const std::string& s) { r.problems.push_back(s); };
    const std::string all(reinterpret_cast<const char*>(pdf.data()), pdf.size());

    r.objects = scanObjects(pdf);
    std::map<int, const Object*> byNum;
    for (const auto& o : r.objects) byNum[o.num] = &o;

    // Plain-text structural counters, computed outside stream bodies only.
    std::string outside;
    for (const auto& o : r.objects) outside += o.dict + "\n";
    // trailer / xref text
    size_t tp = all.rfind("trailer");
    std::string tail = tp == std::string::npos ? all.substr(all.size() > 1024 ? all.size() - 1024 : 0) : all.substr(tp);
    auto countIn = [](const std::string& s, const std::string& n) { size_t c = 0, p = 0; while ((p = s.find(n, p)) != std::string::npos) { ++c; p += n.size(); } return c; };
    // count in whole file but ignoring stream bytes: strip stream bodies
    std::string stripped = all;
    {
        size_t from = 0;
        std::string rebuilt;
        for (const auto& o : r.objects) { (void)o; }
        rebuilt.reserve(all.size());
        size_t p = 0;
        while (true) {
            size_t s = stripped.find("stream", p);
            if (s == std::string::npos) { rebuilt += stripped.substr(p); break; }
            if (s >= 3 && stripped.compare(s - 3, 3, "end") == 0) { rebuilt += stripped.substr(p, s + 6 - p); p = s + 6; continue; }
            size_t a = s + 6;
            if (a >= stripped.size() || (stripped[a] != '\n' && stripped[a] != '\r')) { rebuilt += stripped.substr(p, s + 6 - p); p = s + 6; continue; }
            rebuilt += stripped.substr(p, s + 6 - p);
            size_t e = stripped.find("endstream", a);
            if (e == std::string::npos) { p = stripped.size(); break; }
            p = e;
        }
        (void)from;
        stripped = rebuilt;
    }
    r.startxrefCount = static_cast<int>(countIn(stripped, "startxref"));
    r.eofCount = static_cast<int>(countIn(stripped, "%%EOF"));
    outside += tail;

    // Names allowlist + forbidden tripwire over dictionaries (+ trailer).
    std::set<std::string> seen;
    auto scanNames = [&](const std::string& text, const std::string& where) {
        for (const auto& n : namesIn(text)) {
            if (forbiddenNames().count(n)) problem("forbidden name /" + n + " in " + where);
            else if (!allowedNames().count(n)) problem("name outside allowlist /" + n + " in " + where);
            seen.insert(n);
        }
    };
    for (const auto& o : r.objects) {
        scanNames(o.dict, "object " + std::to_string(o.num));
        r.everything += o.dict + "\n";
        const bool isImage = o.dict.find("/Image") != std::string::npos;
        if (o.dict.find("/ObjStm") != std::string::npos) r.hasObjStm = true;
        if (o.hasStream) {
            if (o.filter != "/FlateDecode" && o.filter != "[/FlateDecode]") problem("object " + std::to_string(o.num) + " stream uses filter '" + o.filter + "' (not allowlisted)");
            else if (!o.decodedOk) problem("object " + std::to_string(o.num) + " stream does not inflate");
            if (!isImage && o.decodedOk) r.everything.append(o.decoded.begin(), o.decoded.end());
            if (!isImage && o.decodedOk) {
                // content-stream operator allowlist
                std::istringstream ss(std::string(o.decoded.begin(), o.decoded.end()));
                std::string tok;
                while (ss >> tok) {
                    if (tok[0] == '/' || tok[0] == '(' || tok[0] == '<' || tok[0] == '[' || tok[0] == '-' || tok[0] == '.' || std::isdigit(static_cast<unsigned char>(tok[0]))) continue;
                    if (std::isalpha(static_cast<unsigned char>(tok[0])) || tok[0] == '\'' || tok[0] == '"') {
                        if (!allowedOps().count(tok)) problem("content operator outside allowlist: " + tok + " (object " + std::to_string(o.num) + ")");
                    }
                }
            }
        }
    }
    scanNames(tail, "trailer");
    if (r.hasObjStm) problem("output contains object streams (inspector would need to expand them; not allowlisted)");
    if (r.startxrefCount != 1) problem("startxref count is " + std::to_string(r.startxrefCount) + ", expected 1 (obsolete revision?)");
    if (r.eofCount != 1) problem("%%EOF count is " + std::to_string(r.eofCount) + ", expected 1");

    // Page -> image mapping and pixel decode.
    int rootNum = refAfter(tail, "Root");
    const Object* cat = rootNum > 0 && byNum.count(rootNum) ? byNum[rootNum] : nullptr;
    if (!cat) { problem("no catalog"); return r; }
    int pagesNum = refAfter(cat->dict, "Pages");
    const Object* pages = pagesNum > 0 && byNum.count(pagesNum) ? byNum[pagesNum] : nullptr;
    if (!pages) { problem("no pages node"); return r; }
    std::vector<int> kids;
    {
        size_t k = pages->dict.find("/Kids");
        size_t lb = pages->dict.find('[', k), rb = pages->dict.find(']', k);
        if (k == std::string::npos || lb == std::string::npos || rb == std::string::npos) { problem("pages without /Kids"); return r; }
        std::istringstream ss(pages->dict.substr(lb + 1, rb - lb - 1));
        int n, g; char R;
        while (ss >> n >> g >> R) kids.push_back(n);
    }
    r.pageCount = static_cast<int>(kids.size());
    bool all_ok = true;
    for (int kn : kids) {
        const Object* page = byNum.count(kn) ? byNum[kn] : nullptr;
        if (!page) { problem("kid object missing"); all_ok = false; continue; }
        int cn = refAfter(page->dict, "Contents");
        const Object* cs = byNum.count(cn) ? byNum[cn] : nullptr;
        if (!cs || !cs->decodedOk) { problem("page content stream missing/undecodable"); all_ok = false; continue; }
        std::string content(cs->decoded.begin(), cs->decoded.end());
        // exactly one painting operator: a single "Do"
        {
            std::istringstream cs2(content);
            std::string t; int doCount = 0;
            while (cs2 >> t) if (t == "Do") ++doCount;
            if (doCount != 1) { problem("page content must paint exactly one XObject"); all_ok = false; continue; }
        }
        size_t xo = page->dict.find("/XObject");
        int imn = -1;
        if (xo != std::string::npos) {
            // first "N 0 R" after /XObject
            std::istringstream ss(page->dict.substr(page->dict.find('<', xo)));
            std::string t; std::vector<std::string> toks;
            while (ss >> t) toks.push_back(t);
            for (size_t i = 0; i + 2 < toks.size(); ++i)
                if (toks[i + 2].rfind("R", 0) == 0 && !toks[i].empty() && std::isdigit(static_cast<unsigned char>(toks[i][0]))) { imn = std::atoi(toks[i].c_str()); break; }
        }
        const Object* im = imn > 0 && byNum.count(imn) ? byNum[imn] : nullptr;
        if (!im || !im->decodedOk) { problem("page image missing/undecodable"); all_ok = false; continue; }
        Image img;
        img.objNum = im->num;
        img.width = static_cast<int>(intAfter(im->dict, "Width"));
        img.height = static_cast<int>(intAfter(im->dict, "Height"));
        if (im->dict.find("/DeviceRGB") == std::string::npos || intAfter(im->dict, "BitsPerComponent") != 8) { problem("image is not 8-bit DeviceRGB"); all_ok = false; continue; }
        if (img.width <= 0 || img.height <= 0 || im->decoded.size() != static_cast<size_t>(img.width) * img.height * 3) { problem("image data length != w*h*3"); all_ok = false; continue; }
        if (im->dict.find("/SMask") != std::string::npos || im->dict.find("/Mask") != std::string::npos) { problem("image has a mask"); all_ok = false; continue; }
        img.rgb = im->decoded;
        r.pageImages.push_back(std::move(img));
    }
    r.mappedAllPages = all_ok && r.pageImages.size() == kids.size();
    return r;
}

// ---- pixel helpers -------------------------------------------------------------------------
inline bool regionAllBlack(const Image& im, int x0, int y0, int x1, int y1, std::string* why = nullptr) {
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(im.width, x1); y1 = std::min(im.height, y1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = &im.rgb[(static_cast<size_t>(y) * im.width + x) * 3];
            if (p[0] | p[1] | p[2]) {
                if (why) *why = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") = " + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
                return false;
            }
        }
    return true;
}

inline long long countColor(const Image& im, int R, int G, int B, int tol, int x0 = 0, int y0 = 0, int x1 = 1 << 30, int y1 = 1 << 30) {
    x1 = std::min(x1, im.width); y1 = std::min(y1, im.height);
    long long n = 0;
    for (int y = std::max(0, y0); y < y1; ++y)
        for (int x = std::max(0, x0); x < x1; ++x) {
            const std::uint8_t* p = &im.rgb[(static_cast<size_t>(y) * im.width + x) * 3];
            if (std::abs(p[0] - R) <= tol && std::abs(p[1] - G) <= tol && std::abs(p[2] - B) <= tol) ++n;
        }
    return n;
}

inline long long countInk(const Image& im, int x0, int y0, int x1, int y1) {  // non-near-white pixels
    x1 = std::min(x1, im.width); y1 = std::min(y1, im.height);
    long long n = 0;
    for (int y = std::max(0, y0); y < y1; ++y)
        for (int x = std::max(0, x0); x < x1; ++x) {
            const std::uint8_t* p = &im.rgb[(static_cast<size_t>(y) * im.width + x) * 3];
            if (p[0] < 200 || p[1] < 200 || p[2] < 200) ++n;
        }
    return n;
}

inline bool containsBytes(const Bytes& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

} // namespace inspect
