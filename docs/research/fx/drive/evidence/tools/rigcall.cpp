// rigcall: load a plugin binary in-process and call named DSP class methods by vm address,
// to measure an internal effect class black-box without the plugin's GUI.
//   rigcall <binary> <recipe> [in.wav out.wav]
// Recipe lines (# comments):
//   alloc N | this OFF | c A | cf A f | cff A f f | cd A d | ci A i | cfi A f i | cfii A f i i | cif A i f
//   cp A (passes pointer to a zeroed 64K scratch) | poke{f,i,d} OFF v | peek{f,i,d} OFF N | gpeekf VM N
//   ctrl A N | proc_ni A | proc_ik A | proc_er A | proc_er2 A | block N | chans N
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <new>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/ucontext.h>

static int g_step = 0; static std::string g_line;
static intptr_t g_slide = 0;
static void onCrash (int sig, siginfo_t* si, void* uc) { auto* c = (ucontext_t*) uc; uint64_t pc = c->uc_mcontext->__ss.__pc, lr = c->uc_mcontext->__ss.__lr;
    fprintf (stderr, "CRASH signal %d at recipe line %d: %s\n  fault addr %p pc vm %#llx lr vm %#llx x0 %#llx x19 %#llx\n", sig, g_step, g_line.c_str(), si->si_addr, (unsigned long long) (pc - g_slide), (unsigned long long) (lr - g_slide), (unsigned long long) c->uc_mcontext->__ss.__x[0], (unsigned long long) c->uc_mcontext->__ss.__x[19]); _exit (3); }

static bool readWav (const char* path, std::vector<std::vector<float>>& ch, int& sr)
{
    FILE* f = fopen (path, "rb"); if (! f) return false;
    std::vector<uint8_t> raw; uint8_t buf[65536]; size_t n;
    while ((n = fread (buf, 1, sizeof buf, f)) > 0) raw.insert (raw.end(), buf, buf + n);
    fclose (f);
    size_t pos = 12; int channels = 1, bits = 32, code = 3; const uint8_t* data = nullptr; size_t dlen = 0;
    while (pos + 8 <= raw.size()) {
        uint32_t size; memcpy (&size, &raw[pos + 4], 4);
        if (! memcmp (&raw[pos], "fmt ", 4)) { uint16_t c, chn, b; uint32_t r; memcpy (&c, &raw[pos+8], 2); memcpy (&chn, &raw[pos+10], 2); memcpy (&r, &raw[pos+12], 4); memcpy (&b, &raw[pos+22], 2); code = c; channels = chn; sr = (int) r; bits = b; }
        else if (! memcmp (&raw[pos], "data", 4)) { data = &raw[pos + 8]; dlen = size; }
        pos += 8 + size + (size & 1);
    }
    if (! data || bits != 32 || (code != 3 && code != 0xFFFE)) return false;
    size_t frames = dlen / (4 * channels); ch.assign (channels, std::vector<float> (frames));
    for (size_t i = 0; i < frames; ++i) for (int c = 0; c < channels; ++c) memcpy (&ch[c][i], data + (i * channels + c) * 4, 4);
    return true;
}
static void writeWav (const char* path, const std::vector<std::vector<float>>& ch, int sr)
{
    FILE* f = fopen (path, "wb"); uint32_t channels = (uint32_t) ch.size(), frames = (uint32_t) ch[0].size(), bytes = frames * channels * 4;
    auto w32 = [&] (uint32_t v) { fwrite (&v, 4, 1, f); }; auto w16 = [&] (uint16_t v) { fwrite (&v, 2, 1, f); };
    fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); fwrite ("WAVEfmt ", 1, 8, f); w32 (16); w16 (3); w16 ((uint16_t) channels); w32 ((uint32_t) sr); w32 ((uint32_t) sr * channels * 4); w16 ((uint16_t) (channels * 4)); w16 (32);
    fwrite ("data", 1, 4, f); w32 (bytes);
    for (uint32_t i = 0; i < frames; ++i) for (uint32_t c = 0; c < channels; ++c) fwrite (&ch[c][i], 4, 1, f);
    fclose (f);
}

static void dummyNoop (void*, float) {}
static void* g_dummyVt[16]; static void** g_dummyObj[1];
using ParamMap = std::unordered_map<std::string, void*>;

int main (int argc, char** argv)
{
    for (auto& e : g_dummyVt) e = (void*) dummyNoop; g_dummyObj[0] = g_dummyVt;
    if (argc < 3) { fprintf (stderr, "usage: rigcall <binary> <recipe> [in.wav out.wav]\n"); return 2; }
    { struct sigaction sa; memset (&sa, 0, sizeof sa); sa.sa_sigaction = onCrash; sa.sa_flags = SA_SIGINFO; for (int sg : { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE }) sigaction (sg, &sa, nullptr); }
    void* h = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (! h) { fprintf (stderr, "dlopen failed: %s\n", dlerror()); return 2; }
    intptr_t slide = 0; bool found = false; std::string want = argv[1]; auto base = want.substr (want.rfind ('/') + 1);
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) { std::string n = _dyld_get_image_name (i); if (n.size() >= base.size() && n.compare (n.size() - base.size(), base.size(), base) == 0 && n.find (".component") != std::string::npos) { slide = _dyld_get_image_vmaddr_slide (i); found = true; } }
    if (! found) { fprintf (stderr, "image not found after dlopen\n"); return 2; }
    g_slide = slide;
    auto fn = [&] (uint64_t vm) { return (void*) (slide + (intptr_t) vm); };

    uint8_t* obj = nullptr; uint8_t* self = nullptr; static uint8_t scratch[1 << 16]; static uint8_t wrap[0x1000];
    void* ctrlFn = nullptr; int ctrlN = 0; void* procFn = nullptr; std::string procKind; int block = 512, chans = 2;
    std::ifstream in (argv[2]); std::string line;
    while (std::getline (in, line)) {
        ++g_step; g_line = line; auto hash = line.find ('#'); if (hash != std::string::npos) line = line.substr (0, hash);
        std::istringstream ss (line); std::string op; if (! (ss >> op)) continue;
        auto A = [&] { std::string s; ss >> s; return (uint64_t) strtoull (s.c_str(), nullptr, 0); };
        auto F = [&] { double d; ss >> d; return d; };
        if (op == "alloc") { size_t n = A(); posix_memalign ((void**) &obj, 64, n); memset (obj, 0, n); self = obj; }
        else if (op == "this") self = obj + A();
        else if (op == "c") ((void (*) (void*)) fn (A())) (self);
        else if (op == "cf") { auto a = A(); float f = (float) F(); ((void (*) (void*, float)) fn (a)) (self, f); }
        else if (op == "cff") { auto a = A(); float f = (float) F(), g = (float) F(); ((void (*) (void*, float, float)) fn (a)) (self, f, g); }
        else if (op == "cd") { auto a = A(); double d = F(); ((void (*) (void*, double)) fn (a)) (self, d); }
        else if (op == "cdd") { auto a = A(); double d = F(), e = F(); ((void (*) (void*, double, double)) fn (a)) (self, d, e); }
        else if (op == "ci") { auto a = A(); long i = (long) F(); ((void (*) (void*, long)) fn (a)) (self, i); }
        else if (op == "cii") { auto a = A(); long i = (long) F(), j = (long) F(); ((void (*) (void*, long, long)) fn (a)) (self, i, j); }
        else if (op == "cuf") { auto a = A(); unsigned u = (unsigned) F(); float f = (float) F(); ((void (*) (void*, unsigned, float)) fn (a)) (self, u, f); }
        else if (op == "cfi") { auto a = A(); float f = (float) F(); long i = (long) F(); ((void (*) (void*, float, long)) fn (a)) (self, f, i); }
        else if (op == "cif") { auto a = A(); long i = (long) F(); float f = (float) F(); ((void (*) (void*, long, float)) fn (a)) (self, i, f); }
        else if (op == "cfii") { auto a = A(); float f = (float) F(); long i = (long) F(), j = (long) F(); ((void (*) (void*, float, long, long)) fn (a)) (self, f, i, j); }
        else if (op == "cffi") { auto a = A(); float f = (float) F(), g = (float) F(); long i = (long) F(); ((void (*) (void*, float, float, long)) fn (a)) (self, f, g, i); }
        else if (op == "cp") { auto a = A(); ((void (*) (void*, void*)) fn (a)) (self, scratch); }
        else if (op == "vt") { auto o = A(); auto vm = A(); void* p = (uint8_t*) fn (vm) + 0x10; memcpy (self + o, &p, 8); }
        else if (op == "ik_factory") { auto inst = A(), smb = A(), ssr = A(), acq = A(); static uint8_t guid[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf1, 0x02 };
            void* f = ((void* (*) ()) fn (inst)) ();
            if (smb) ((void (*) (void*, int, void*)) fn (smb)) (f, 4096, guid);
            if (ssr) ((void (*) (void*, float, void*)) fn (ssr)) (f, 48000.0f, guid);
            void* m = ((void* (*) (void*, void*)) fn (acq)) (f, guid);
            obj = self = (uint8_t*) m; memset (wrap, 0, sizeof wrap); memcpy (wrap + 0x140, &m, 8); new (wrap + 0x28) ParamMap(); printf ("ik module %p\n", m); }
        else if (op == "ik_set") { auto a = A(); auto sv = A(); float v = (float) F(); (*(ParamMap*) (wrap + 0x28))[*(std::string*) fn (sv)] = (void*) g_dummyObj; ((void (*) (void*, void*, float)) fn (a)) (wrap, fn (sv), v); }
        else if (op == "gstr") { auto vm = A(); uint8_t* p = (uint8_t*) fn (vm); const char* c = (p[23] & 0x80) ? *(const char**) p : (const char*) p; printf ("gstr %#llx: %s\n", (unsigned long long) vm, c); }
        else if (op == "pokef") { auto o = A(); float f = (float) F(); memcpy (self + o, &f, 4); }
        else if (op == "pokei") { auto o = A(); int32_t i = (int32_t) F(); memcpy (self + o, &i, 4); }
        else if (op == "poked") { auto o = A(); double d = F(); memcpy (self + o, &d, 8); }
        else if (op == "peekf") { auto o = A(); int n = (int) F(); printf ("peekf +%#llx:", (unsigned long long) o); for (int i = 0; i < n; ++i) { float f; memcpy (&f, self + o + 4 * i, 4); printf (" %.9g", f); } printf ("\n"); }
        else if (op == "peekd") { auto o = A(); int n = (int) F(); printf ("peekd +%#llx:", (unsigned long long) o); for (int i = 0; i < n; ++i) { double f; memcpy (&f, self + o + 8 * i, 8); printf (" %.12g", f); } printf ("\n"); }
        else if (op == "peeki") { auto o = A(); int n = (int) F(); printf ("peeki +%#llx:", (unsigned long long) o); for (int i = 0; i < n; ++i) { int32_t f; memcpy (&f, self + o + 4 * i, 4); printf (" %d", f); } printf ("\n"); }
        else if (op == "peekpf") { auto o = A(); auto o2 = A(); int n = (int) F(); uint8_t* p; memcpy (&p, self + o, 8); printf ("peekpf *(+%#llx)+%#llx:", (unsigned long long) o, (unsigned long long) o2); for (int i = 0; i < n; ++i) { float f; memcpy (&f, p + o2 + 4 * i, 4); printf (" %.9g", f); } printf ("\n"); }
        else if (op == "peekpi") { auto o = A(); auto o2 = A(); int n = (int) F(); uint8_t* p; memcpy (&p, self + o, 8); printf ("peekpi *(+%#llx)+%#llx:", (unsigned long long) o, (unsigned long long) o2); for (int i = 0; i < n; ++i) { int32_t f; memcpy (&f, p + o2 + 4 * i, 4); printf (" %d", f); } printf ("\n"); }
        else if (op == "peekppf") { auto o = A(); auto o2 = A(); auto o3 = A(); int n = (int) F(); int stride = (int) F(); uint8_t* p; memcpy (&p, self + o, 8); uint8_t* q; memcpy (&q, p + o2, 8); printf ("peekppf:"); for (int i = 0; i < n; ++i) { float f; memcpy (&f, q + o3 + 4 * i * stride, 4); printf (" %.6g", f); } printf ("\n"); }
        else if (op == "gpeekf") { auto o = A(); int n = (int) F(); printf ("gpeekf %#llx:", (unsigned long long) o); for (int i = 0; i < n; ++i) { float f; memcpy (&f, (uint8_t*) fn (o) + 4 * i, 4); printf (" %.9g", f); } printf ("\n"); }
        else if (op == "ctrl") { ctrlFn = fn (A()); ctrlN = (int) F(); }
        else if (op == "proc_ni" || op == "proc_ik" || op == "proc_er" || op == "proc_er5") { procKind = op; procFn = fn (A()); }
        else if (op == "block") block = (int) F();
        else if (op == "chans") chans = (int) F();
        else { fprintf (stderr, "unknown op %s\n", op.c_str()); return 2; }
        fflush (stdout);
    }
    g_line = "(render)";
    if (argc >= 5 && procFn) {
        std::vector<std::vector<float>> inCh; int sr = 48000;
        if (! readWav (argv[3], inCh, sr)) { fprintf (stderr, "cannot read %s\n", argv[3]); return 2; }
        size_t frames = inCh[0].size(); std::vector<std::vector<float>> io (4, std::vector<float> (frames));
        for (int c = 0; c < 4; ++c) io[c] = inCh[(size_t) c % inCh.size()];
        std::vector<std::vector<float>> out (4, std::vector<float> (frames, 0.0f));
        int sinceCtrl = 0;
        for (size_t pos = 0; pos < frames;) {
            int n = (int) std::min<size_t> ((size_t) block, frames - pos);
            if (ctrlFn) { n = std::min (n, ctrlN - sinceCtrl); if (sinceCtrl == 0) ((void (*) (void*)) ctrlFn) (self); }
            const float* ip[4] = { &io[0][pos], &io[1][pos], &io[2][pos], &io[3][pos] }; float* op[4] = { &out[0][pos], &out[1][pos], &out[2][pos], &out[3][pos] };
            if (procKind == "proc_ni") ((void (*) (void*, const float* const*, float**, int)) procFn) (self, ip, op, n);
            else if (procKind == "proc_er5") ((void (*) (void*, const float* const*, float* const*, unsigned, const void*, unsigned)) procFn) (self, ip, op, (unsigned) n, nullptr, 0u);
            else if (procKind == "proc_er") ((void (*) (void*, const float* const*, float* const*, unsigned)) procFn) (self, ip, op, (unsigned) n);
            else ((void (*) (void*, const float*, float*, int)) procFn) (self, ip[0], op[0], n);
            pos += (size_t) n; if (ctrlFn) { sinceCtrl += n; if (sinceCtrl >= ctrlN) sinceCtrl = 0; }
        }
        out.resize ((size_t) chans); writeWav (argv[4], out, sr);
    }
    fflush (stdout); _exit (0);
}
