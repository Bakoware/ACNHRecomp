// movie::Decoder (NintendoSDK_movie, subsdk0) for VP8/VP9 video, decoded on the CPU with libvpx.
// First needed by Animal Crossing: New Horizons: the opening movie (Movie/OpeningMovie.webm, VP9 1080p + Vorbis).
// The SDK's decoder is Android's stagefright on the console's NVDEC block (NvMM/OMX): it opens the nvhost driver
// and aborts in process. The game demuxes with movie::Extractor (software, it runs recompiled) and drives the
// decoder like Android's MediaCodec: input buffer indexes, output buffer indexes and four os::Events
// (movie::DecoderEvents: input available, output available, format changed, error).
//
// main creates the decoder with `new movie::Decoder(type, output format, mode, priority)` and calls everything
// else through its vtable, so the constructor is replaced: for video types the object gets a vtable of our own whose
// slots are HLE thunks (the slot order is movie::Decoder's, read from _ZTVN5movie7DecoderE). Audio decoders
// (Vorbis, software in the SDK) keep the SDK's implementation.
//
// Output format 4 is NV12, tightly packed (a width x height luma plane, then interleaved chroma at half
// resolution), copied into the buffer the game passes to GetOutputBuffer, as DecoderImpl::ConvertDecoderOutputFormat
// does. Without libvpx (NX_NO_VPX) frames are black and the movie still plays to its end.
#include <algorithm>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if !defined(NX_NO_VPX)
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif

#include <nx/runtime.h>

namespace nx {
namespace nvn {
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);
uint64_t scratch(CPU& c, size_t size);
}  // namespace nvn

namespace {

using nvn::guest_call;

constexpr const char* kCtor = "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi";
constexpr uint32_t kOk = 0, kInvalid = 0x100b, kNoBuffer = 0x1011;
constexpr uint32_t kFlagEos = 4;          // BufferFlag_EndOfStream (what main passes when the extractor runs out)
constexpr int kInputs = 4, kOutputs = 4;  // buffers in flight, as a hardware decoder would have
constexpr uint64_t kInputCapacity = 4ull << 20;

enum Type { kAvc = 1, kVp8 = 2, kVp9 = 3, kAac = 4, kVorbis = 5 };

struct Frame {
    std::vector<uint8_t> nv12;
    int64_t pts = 0;
    uint32_t flags = 0;
    bool acquired = false;
    int index = -1;
};

struct Decoder {
    std::mutex lock;
    int type = 0, out_format = 0;
    int width = 0, height = 0;
    uint64_t events[4] = {};  // input available, output available, format changed, error (os::EventType*)
    uint64_t inputs[kInputs] = {};
    std::deque<int> free_inputs, held_inputs;  // held: decoded, waiting for an output slot to give them back
    std::deque<Frame> outputs;
    int next_out = 0;
    bool started = false, format_sent = false;
    uint64_t frames = 0;
#if !defined(NX_NO_VPX)
    vpx_codec_ctx_t vpx{};
    bool vpx_ok = false;
#endif
};

std::mutex g_lock;
std::map<uint64_t, std::unique_ptr<Decoder>> g_decoders;  // by guest object
uint64_t g_vtable = 0;                                   // guest copy of our vtable (address point)
uint64_t g_strings = 0;                                  // guest copies of the MediaData keys

Decoder* of(uint64_t self) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(self);
    return it == g_decoders.end() ? nullptr : it->second.get();
}

uint64_t sym(const char* name) {
    uint64_t a = find_symbol(name);
    if (!a) fatal("movie: %s not found", name);
    return a;
}

// MediaData keys live in guest memory: the SDK's setters keep the pointer they are given only for the call.
uint64_t key(const char* k) {
    static std::mutex m;
    static std::map<std::string, uint64_t> keys;
    static uint64_t next = 0;
    std::lock_guard lk(m);
    auto it = keys.find(k);
    if (it != keys.end()) return it->second;
    if (!g_strings || next + std::strlen(k) + 1 > 0x1000) g_strings = kernel_alloc(0x1000), next = 0;
    uint64_t a = g_strings + next;
    std::memcpy(reinterpret_cast<void*>(a), k, std::strlen(k) + 1);
    next += (std::strlen(k) + 16) & ~uint64_t(15);
    keys[k] = a;
    return a;
}

void signal(CPU& c, Decoder& d, int which) {
    static const uint64_t fn = sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (d.events[which]) guest_call(c, fn, {d.events[which]});
}

// ---- decoding ----------------------------------------------------------------------------------------------------

void open_codec(Decoder& d) {
#if !defined(NX_NO_VPX)
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 4;
    vpx_codec_iface_t* iface = d.type == kVp8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx();
    d.vpx_ok = vpx_codec_dec_init(&d.vpx, iface, &cfg, 0) == VPX_CODEC_OK;
    log("movie: %s decoder %s (libvpx %s)", d.type == kVp8 ? "VP8" : "VP9", d.vpx_ok ? "ready" : "failed",
        vpx_codec_version_str());
#else
    log("movie: built without libvpx: video frames are black");
#endif
}

// One compressed frame in, zero or more NV12 frames out (appended to d.outputs).
void decode(Decoder& d, const uint8_t* data, size_t size, int64_t pts, uint32_t flags) {
    auto push = [&](std::vector<uint8_t>&& nv12, uint32_t fl) {
        Frame f;
        f.nv12 = std::move(nv12);
        f.pts = pts;
        f.flags = fl;
        f.index = d.next_out++ % 1024;
        d.outputs.push_back(std::move(f));
    };
#if !defined(NX_NO_VPX)
    if (d.vpx_ok && size) {
        if (vpx_codec_decode(&d.vpx, data, unsigned(size), nullptr, 0) != VPX_CODEC_OK) {
            static int warned = 0;
            if (warned++ < 8) log("movie: frame at %lld us: %s", (long long)pts, vpx_codec_error(&d.vpx));
        }
        vpx_codec_iter_t it = nullptr;
        while (vpx_image_t* img = vpx_codec_get_frame(&d.vpx, &it)) {
            if (img->fmt != VPX_IMG_FMT_I420) continue;  // profile 0 (8-bit 4:2:0) only, what the games ship
            int w = int(img->d_w) & ~1, h = int(img->d_h) & ~1;
            if (!d.width) d.width = w, d.height = h;
            std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2);
            for (int y = 0; y < h; y++)
                std::memcpy(&nv12[size_t(y) * w], img->planes[0] + size_t(y) * img->stride[0], size_t(w));
            uint8_t* uv = nv12.data() + size_t(w) * h;
            for (int y = 0; y < h / 2; y++) {
                const uint8_t* u = img->planes[1] + size_t(y) * img->stride[1];
                const uint8_t* v = img->planes[2] + size_t(y) * img->stride[2];
                uint8_t* o = uv + size_t(y) * w;
                for (int x = 0; x < w / 2; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
            }
            push(std::move(nv12), 0);
            d.frames++;
        }
    } else
#endif
        if (size && d.width) {
        std::vector<uint8_t> nv12(size_t(d.width) * d.height * 3 / 2, 16);
        std::fill(nv12.begin() + size_t(d.width) * d.height, nv12.end(), uint8_t(128));
        push(std::move(nv12), 0);
        d.frames++;
    }
    (void)data;
    if (flags & kFlagEos) {
        push({}, kFlagEos);
        log("movie: end of stream after %llu frames", (unsigned long long)d.frames);
    }
}

// ---- the vtable ------------------------------------------------------------------------------------------------

void Destroy(CPU& c) {  // ~Decoder() (complete)
    uint64_t self = c.x[0];
    std::unique_ptr<Decoder> d;
    {
        std::lock_guard lk(g_lock);
        auto it = g_decoders.find(self);
        if (it != g_decoders.end()) d = std::move(it->second), g_decoders.erase(it);
    }
#if !defined(NX_NO_VPX)
    if (d && d->vpx_ok) vpx_codec_destroy(&d->vpx);
#endif
    // The input buffers stay allocated: kernel memory is not given back (a few MiB per movie played).
    st<uint64_t>(self, 0);
}

void DestroyDelete(CPU& c) {  // ~Decoder() (deleting)
    uint64_t self = c.x[0];
    Destroy(c);
    if (uint64_t del = find_symbol("_ZdlPv")) guest_call(c, del, {self});
}

void Configure(CPU& c) {  // Status (MediaData* format)
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    uint64_t out = nvn::scratch(c, 16);
    static const uint64_t find = sym("_ZNK5movie9MediaData9FindInt32EPKcPi");
    int w = 0, h = 0;
    st<int32_t>(out, 0);
    if (c.x[1] && (guest_call(c, find, {c.x[1], key("width"), out}) & 1)) w = ld<int32_t>(out);
    st<int32_t>(out, 0);
    if (c.x[1] && (guest_call(c, find, {c.x[1], key("height"), out}) & 1)) h = ld<int32_t>(out);
    std::lock_guard lk(d->lock);
    d->width = w & ~1, d->height = h & ~1;
    log("movie: configure %dx%d, output format %d", w, h, d->out_format);
    open_codec(*d);
    c.x[0] = kOk;
}

void RegisterClientEvents(CPU& c) {  // Status (DecoderEvents*)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    for (int k = 0; k < 4; k++) d->events[k] = ld<uint64_t>(c.x[1] + 8 * k);
    c.x[0] = kOk;
}

void GetOutputFormat(CPU& c) {  // Status (MediaData* out)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1]) return void(c.x[0] = kInvalid);
    static const uint64_t set32 = sym("_ZN5movie9MediaData8SetInt32EPKci");
    static const uint64_t rect = sym("_ZN5movie9MediaData7SetRectEPKciiii");
    static const uint64_t setstr = sym("_ZN5movie9MediaData9SetStringEPKcS2_");
    int w, h;
    {
        std::lock_guard lk(d->lock);
        w = d->width, h = d->height;
    }
    uint64_t md = c.x[1];
    guest_call(c, setstr, {md, key("mime"), key(d->type == kVp8 ? "video/x-vnd.on2.vp8" : "video/x-vnd.on2.vp9")});
    guest_call(c, set32, {md, key("width"), uint64_t(w)});
    guest_call(c, set32, {md, key("height"), uint64_t(h)});
    guest_call(c, set32, {md, key("stride"), uint64_t(w)});
    guest_call(c, set32, {md, key("slice-height"), uint64_t(h)});
    guest_call(c, rect, {md, key("crop"), 0, 0, uint64_t(w - 1), uint64_t(h - 1)});  // inclusive, as in Android
    c.x[0] = kOk;
}

void Start(CPU& c) {
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    {
        std::lock_guard lk(d->lock);
        d->started = true;
        d->free_inputs.clear();
        for (int k = 0; k < kInputs; k++) {
            if (!d->inputs[k]) d->inputs[k] = kernel_alloc(kInputCapacity);
            d->free_inputs.push_back(k);
        }
    }
    signal(c, *d, 0);
    c.x[0] = kOk;
}

void Flush(CPU& c) {  // also Stop and Reset: drop everything in flight, every input buffer free again
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    bool started;
    {
        std::lock_guard lk(d->lock);
        d->outputs.clear();
        d->held_inputs.clear();
        d->free_inputs.clear();
        for (int k = 0; k < kInputs; k++) d->free_inputs.push_back(k);
        started = d->started;
    }
    if (started) signal(c, *d, 0);
    c.x[0] = kOk;
}

void Stop(CPU& c) {
    if (Decoder* d = of(c.x[0])) {
        std::lock_guard lk(d->lock);
        d->started = false;
    }
    Flush(c);
}

void AcquireInputBufferIndex(CPU& c) {  // Status (int* index, int* remaining)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    st<int32_t>(c.x[1], -1);
    st<int32_t>(c.x[2], 0);
    if (d->free_inputs.empty()) return void(c.x[0] = kNoBuffer);
    st<int32_t>(c.x[1], d->free_inputs.front());
    d->free_inputs.pop_front();
    st<int32_t>(c.x[2], int32_t(d->free_inputs.size()));
    c.x[0] = kOk;
}

void GetInputBuffer(CPU& c) {  // Status (int index, Buffer*)
    Decoder* d = of(c.x[0]);
    int index = int(c.x[1]);
    if (!d || !c.x[2] || index < 0 || index >= kInputs || !d->inputs[index]) return void(c.x[0] = kInvalid);
    static const uint64_t set_data = sym("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    static const uint64_t set_range = sym("_ZN5movie6Buffer8SetRangeEii");
    guest_call(c, set_data, {c.x[2], d->inputs[index], kInputCapacity});
    guest_call(c, set_range, {c.x[2], 0, 0});
    c.x[0] = kOk;
}

// Status (int index, int offset, int size, int64_t presentation time (us), uint32_t flags)
void SendInputBufferForDecode(CPU& c) {
    Decoder* d = of(c.x[0]);
    int index = int(c.x[1]), offset = int(c.x[2]), size = int(c.x[3]);
    int64_t pts = int64_t(c.x[4]);
    uint32_t flags = uint32_t(c.x[5]);
    if (!d || index < 0 || index >= kInputs || offset < 0 || size < 0 || uint64_t(offset) + size > kInputCapacity)
        return void(c.x[0] = kInvalid);
    bool format_changed = false, input_back = false, output = false;
    {
        std::lock_guard lk(d->lock);
        size_t before = d->outputs.size();
        decode(*d, reinterpret_cast<const uint8_t*>(d->inputs[index] + offset), size_t(size), pts, flags);
        output = d->outputs.size() > before;
        if (output && !d->format_sent && d->width) d->format_sent = format_changed = true;
        if (d->outputs.size() < kOutputs)
            d->free_inputs.push_back(index), input_back = true;
        else
            d->held_inputs.push_back(index);
    }
    if (format_changed) signal(c, *d, 2);
    if (output) signal(c, *d, 1);
    if (input_back) signal(c, *d, 0);
    c.x[0] = kOk;
}

// Status (int* index, int64_t* presentation time, uint32_t* flags, int* remaining)
void AcquireOutputBufferIndex(CPU& c) {
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2] || !c.x[3] || !c.x[4]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    st<int32_t>(c.x[1], -1);
    st<int64_t>(c.x[2], 0);
    st<uint32_t>(c.x[3], 0);
    st<int32_t>(c.x[4], 0);
    auto it = std::find_if(d->outputs.begin(), d->outputs.end(), [](const Frame& f) { return !f.acquired; });
    if (it == d->outputs.end()) return void(c.x[0] = kNoBuffer);
    it->acquired = true;
    st<int32_t>(c.x[1], it->index);
    st<int64_t>(c.x[2], it->pts);
    st<uint32_t>(c.x[3], it->flags);
    st<int32_t>(c.x[4], int32_t(std::count_if(it + 1, d->outputs.end(), [](const Frame& f) { return !f.acquired; })));
    c.x[0] = kOk;
}

void GetOutputBuffer(CPU& c) {  // Status (int index, Buffer* out): the frame copied into the game's buffer
    Decoder* d = of(c.x[0]);
    uint64_t buf = c.x[2];
    if (!d || !buf) return void(c.x[0] = kInvalid);
    static const uint64_t base = sym("_ZNK5movie6Buffer4BaseEv");
    static const uint64_t capacity = sym("_ZNK5movie6Buffer8CapacityEv");
    static const uint64_t set_range = sym("_ZN5movie6Buffer8SetRangeEii");
    uint64_t dst = guest_call(c, base, {buf}), cap = guest_call(c, capacity, {buf});
    size_t n = 0;
    {
        std::lock_guard lk(d->lock);
        auto it = std::find_if(d->outputs.begin(), d->outputs.end(), [&](const Frame& f) { return f.index == int(c.x[1]); });
        if (it == d->outputs.end()) return void(c.x[0] = kInvalid);
        n = std::min<size_t>(it->nv12.size(), cap);
        if (n && dst) std::memcpy(reinterpret_cast<void*>(dst), it->nv12.data(), n);
    }
    guest_call(c, set_range, {buf, 0, n});
    c.x[0] = kOk;
}

void ReleaseOutputBufferIndex(CPU& c) {  // Status (int index)
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    bool input_back = false;
    {
        std::lock_guard lk(d->lock);
        auto it = std::find_if(d->outputs.begin(), d->outputs.end(), [&](const Frame& f) { return f.index == int(c.x[1]); });
        if (it != d->outputs.end()) d->outputs.erase(it);
        while (!d->held_inputs.empty() && d->outputs.size() < kOutputs) {
            d->free_inputs.push_back(d->held_inputs.front());
            d->held_inputs.pop_front();
            input_back = true;
        }
    }
    if (input_back) signal(c, *d, 0);
    c.x[0] = kOk;
}

void GetOutputBufferSize(CPU& c) {  // Status (int* size, int* count)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    st<int32_t>(c.x[1], d->width * d->height * 3 / 2);
    st<int32_t>(c.x[2], 1);
    c.x[0] = kOk;
}

void GetLastError(CPU& c) { c.x[0] = kOk; }
void GetOutputBufferWithFormat(CPU& c) {  // Status (int index, Buffer*, MediaData*)
    uint64_t self = c.x[0], md = c.x[3];
    GetOutputBuffer(c);
    if (c.x[0] == kOk && md) {
        uint64_t saved = c.x[1];
        c.x[0] = self, c.x[1] = md;
        GetOutputFormat(c);
        c.x[1] = saved;
    }
}
void GetOutputBufferFormat(CPU& c) {  // Status (int index, MediaData*)
    c.x[1] = c.x[2];
    GetOutputFormat(c);
}

// The slots after the address point, in movie::Decoder's order.
struct Slot {
    const char* name;
    HleFn fn;
};
constexpr Slot kSlots[] = {
    {"movie::Decoder::~Decoder", Destroy},
    {"movie::Decoder::~Decoder(deleting)", DestroyDelete},
    {"movie::Decoder::Configure", Configure},
    {"movie::Decoder::RegisterClientEvents", RegisterClientEvents},
    {"movie::Decoder::GetOutputFormat", GetOutputFormat},
    {"movie::Decoder::Start", Start},
    {"movie::Decoder::Stop", Stop},
    {"movie::Decoder::Reset", Flush},
    {"movie::Decoder::Flush", Flush},
    {"movie::Decoder::AcquireInputBufferIndex", AcquireInputBufferIndex},
    {"movie::Decoder::GetInputBuffer", GetInputBuffer},
    {"movie::Decoder::SendInputBufferForDecode", SendInputBufferForDecode},
    {"movie::Decoder::AcquireOutputBufferIndex", AcquireOutputBufferIndex},
    {"movie::Decoder::GetOutputBuffer", GetOutputBuffer},
    {"movie::Decoder::GetOutputBufferSize", GetOutputBufferSize},
    {"movie::Decoder::ReleaseOutputBufferIndex", ReleaseOutputBufferIndex},
    {"movie::Decoder::GetLastError", GetLastError},
    {"movie::Decoder::GetOutputBuffer(format)", GetOutputBufferWithFormat},
    {"movie::Decoder::GetOutputBufferFormat", GetOutputBufferFormat},
};

uint64_t vtable() {
    static std::once_flag once;
    std::call_once(once, [] {
        uint64_t vt = kernel_alloc(0x1000);
        st<uint64_t>(vt, 0);      // offset to top
        st<uint64_t>(vt + 8, 0);  // typeinfo: none (the game does not use RTTI on it)
        int k = 0;
        for (const Slot& s : kSlots) st<uint64_t>(vt + 16 + 8 * k++, hle_address_or(s.name, s.fn));
        g_vtable = vt + 16;
    });
    return g_vtable;
}

// Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int thread priority)
void Construct(CPU& c) {
    uint64_t self = c.x[0];
    int type = int(c.x[1]), format = int(c.x[2]);
    if (type != kVp8 && type != kVp9) {
        static const uint64_t real = sym(kCtor);
        call(c, real);
        return;
    }
    auto d = std::make_unique<Decoder>();
    d->type = type, d->out_format = format;
    if (format != 4) log("movie: video output format %d asked; NV12 (4) given", format);
    st<uint64_t>(self, vtable());
    st<uint64_t>(self + 8, 0);
    std::lock_guard lk(g_lock);
    g_decoders[self] = std::move(d);
}

}  // namespace

NX_HLE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", Construct);

}  // namespace nx
