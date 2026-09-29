#include "common/Logger.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#  include <sys/syscall.h>
#  define NAS_THREAD_ID() static_cast<unsigned long>(::syscall(SYS_gettid))
#else
#  define NAS_THREAD_ID() static_cast<unsigned long>(::pthread_self())
#endif

namespace nas {

std::atomic<LogLevel> Logger::s_level{LogLevel::INFO};

namespace {

using detail::Buffer;

class LoggerState;
struct ThreadContext;

// Defined further below; declared here because the thread-exit hook and the
// backend need them.
void flushLocked(ThreadContext& c, LoggerState& st);
void flushAll(LoggerState& st);
void registerContext(ThreadContext* c);
void unregisterLocked(ThreadContext* c);
void destroyContext(void* p);

// ---------------------------------------------------------------------
//  Tunables
// ---------------------------------------------------------------------
constexpr std::size_t      kPoolBuffers          = 32;   // initial pool
constexpr std::size_t      kPoolMaxBuffers       = 256;  // hard cap (16 MiB)
constexpr std::size_t      kMaxInlineHeader      = 512;
constexpr std::size_t      kMaxInlineMessage     = 1024;
constexpr std::size_t      kMaxFilePartInHeader  = 160;  // clamp file / func
constexpr std::uint64_t    kAutoFlushMs          = 200;  // max buffering latency

// ---------------------------------------------------------------------
//  Global state. `g_state` is the published backend; `g_inLog` counts
//  threads currently inside log()/flush() so shutdown can quiesce.
// ---------------------------------------------------------------------
std::atomic<LoggerState*>  g_state{nullptr};
std::atomic<int>           g_inLog{0};
std::atomic<std::uint64_t> g_dropped{0};

// Serialises init()/shutdown() and protects every LoggerState field.
std::mutex g_initMtx;

inline std::int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------
//  Fast integer / string writers (no snprintf on the hot path)
// ---------------------------------------------------------------------
inline char* writeUInt2(unsigned v, char* p) {
    *p++ = static_cast<char>('0' + (v / 10) % 10);
    *p++ = static_cast<char>('0' + v % 10);
    return p;
}

inline char* writeUInt3(unsigned v, char* p) {
    *p++ = static_cast<char>('0' + (v / 100) % 10);
    *p++ = static_cast<char>('0' + (v / 10) % 10);
    *p++ = static_cast<char>('0' + v % 10);
    return p;
}

inline char* writeUInt4(unsigned v, char* p) {
    *p++ = static_cast<char>('0' + (v / 1000) % 10);
    *p++ = static_cast<char>('0' + (v / 100) % 10);
    *p++ = static_cast<char>('0' + (v / 10) % 10);
    *p++ = static_cast<char>('0' + v % 10);
    return p;
}

inline char* writeUInt(std::uint64_t v, char* p) {
    char tmp[20];
    int  n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

inline char* writeNChars(char* p, const char* s, std::size_t maxLen) {
    if (s) {
        for (std::size_t i = 0; i < maxLen && s[i] != '\0'; ++i) *p++ = s[i];
    }
    return p;
}

// ---------------------------------------------------------------------
//  Timestamp with a per-thread, once-per-second calendar cache
// ---------------------------------------------------------------------
const char* const kLevelText[6] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

inline const char* levelText(LogLevel l) {
    int i = static_cast<int>(l);
    if (i < 0 || i > 5) i = 5;
    return kLevelText[i];
}

struct TimeCache {
    std::int64_t sec = -1;
    char         text[20]; // "YYYY-MM-DD HH:MM:SS" (19)
};
thread_local TimeCache t_time;

// Writes "YYYY-MM-DD HH:MM:SS" into `out` (>= 19 bytes); returns ms.
inline int buildTimestamp(char* out, std::int64_t nowMs) {
    const std::int64_t sec = nowMs / 1000;
    const int          ms  = static_cast<int>(nowMs % 1000);

    if (sec != t_time.sec) {
        t_time.sec = sec;
        std::time_t t = static_cast<std::time_t>(sec);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char* p = t_time.text;
        p = writeUInt4(static_cast<unsigned>(tm.tm_year + 1900), p);
        *p++ = '-'; p = writeUInt2(static_cast<unsigned>(tm.tm_mon + 1), p);
        *p++ = '-'; p = writeUInt2(static_cast<unsigned>(tm.tm_mday), p);
        *p++ = ' ';
        p = writeUInt2(static_cast<unsigned>(tm.tm_hour), p);
        *p++ = ':'; p = writeUInt2(static_cast<unsigned>(tm.tm_min), p);
        *p++ = ':'; p = writeUInt2(static_cast<unsigned>(tm.tm_sec), p);
    }
    std::memcpy(out, t_time.text, 19);
    return ms;
}

// ---------------------------------------------------------------------
//  Header formatting:
//  [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [ThreadID] [File:Line:Func]
// ---------------------------------------------------------------------
inline std::size_t buildHeader(char* out, LogLevel lvl, const char* file,
                               int line, const char* func, unsigned long tid,
                               const char* ts, int ms) {
    char* p = out;

    *p++ = '[';
    std::memcpy(p, ts, 19); p += 19;
    *p++ = '.';
    p = writeUInt3(static_cast<unsigned>(ms), p);
    *p++ = ']'; *p++ = ' ';

    *p++ = '[';
    std::memcpy(p, levelText(lvl), 5); p += 5;
    *p++ = ']'; *p++ = ' ';

    *p++ = '[';
    p = writeUInt(static_cast<std::uint64_t>(tid), p);
    *p++ = ']'; *p++ = ' ';

    *p++ = '[';
    p = writeNChars(p, file, kMaxFilePartInHeader);
    *p++ = ':';
    p = writeUInt(static_cast<std::uint64_t>(line < 0 ? 0 : line), p);
    *p++ = ':';
    p = writeNChars(p, func, kMaxFilePartInHeader);
    *p++ = ']'; *p++ = ' ';

    return static_cast<std::size_t>(p - out);
}

std::size_t fileSize(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fclose(f);
    return sz > 0 ? static_cast<std::size_t>(sz) : 0;
}

// ---------------------------------------------------------------------
//  Buffer pool - pre-allocated, capped, no malloc in the common case
// ---------------------------------------------------------------------
class BufferPool {
public:
    explicit BufferPool(std::size_t initial) {
        for (std::size_t i = 0; i < initial; ++i) {
            Buffer* b = new Buffer();
            owned_.push_back(b);
            free_.push_back(b);
        }
    }

    ~BufferPool() {
        for (Buffer* b : owned_) delete b;
    }

    // Never blocks. Recycles a free buffer, allocates up to the cap, or
    // returns nullptr when the pool is exhausted (caller drops the line).
    Buffer* acquire() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!free_.empty()) {
            Buffer* b = free_.back();
            free_.pop_back();
            return b;
        }
        if (owned_.size() >= kPoolMaxBuffers) return nullptr;
        Buffer* b = new Buffer();
        owned_.push_back(b);
        return b;
    }

    void release(Buffer* b) noexcept {
        if (!b) return;
        b->reset();
        std::lock_guard<std::mutex> lk(mtx_);
        free_.push_back(b);
    }

private:
    std::mutex           mtx_;
    std::vector<Buffer*> free_;
    std::vector<Buffer*> owned_; // owns every buffer ever created
};

// Intentionally leaked: thread contexts may still hold pool buffers during
// process teardown, so the pool must outlive them.
BufferPool& globalPool() {
    static BufferPool* pool = new BufferPool(kPoolBuffers);
    return *pool;
}

// ---------------------------------------------------------------------
//  Registry of live thread contexts, so the backend and shutdown() can
//  flush threads other than the caller.
//
//  Lock order (must never be inverted):
//      registryMutex -> context.mtx -> queue.mtx -> pool.mtx
//  log()/flush() take context.mtx -> queue/pool only (never the registry),
//  which keeps the order acyclic.
// ---------------------------------------------------------------------
std::mutex& registryMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

ThreadContext*& registryHead() {
    static ThreadContext* head = nullptr;
    return head;
}

// ---------------------------------------------------------------------
//  Per-thread front-end context (double buffering).
//
//  The context is reached through a pthread_key rather than a
//  `thread_local` object, so its thread-exit cleanup can flush the tail
//  and recycle buffers explicitly.
// ---------------------------------------------------------------------
struct ThreadContext {
    Buffer*        active     = nullptr; // currently being written
    Buffer*        standby    = nullptr; // swap target
    unsigned long  tid        = 0;
    bool           registered = false;
    ThreadContext* next       = nullptr; // intrusive registry link
    std::mutex     mtx;                  // guards active/standby
};

// ---------------------------------------------------------------------
//  Thread-exit hook (pthread_key).
// ---------------------------------------------------------------------
void onThreadExit(void* p) { destroyContext(p); }

pthread_key_t& tlsSlot() {
    static pthread_key_t key = [] {
        pthread_key_t k;
        ::pthread_key_create(&k, onThreadExit);
        return k;
    }();
    return key;
}
void* tlsGet() { return ::pthread_getspecific(tlsSlot()); }
void tlsSet(void* p) { ::pthread_setspecific(tlsSlot(), p); }

// ---------------------------------------------------------------------
//  Queue - the single seam to the backend. Swap for per-thread SPSC ring
//  buffers later by keeping the same ownership model (only published
//  buffers cross this boundary; the backend never touches a live
//  context's active buffer).
// ---------------------------------------------------------------------
class BufferQueue {
public:
    void start() {
        std::lock_guard<std::mutex> lk(mtx_);
        stopped_ = false;
    }

    void push(Buffer* b) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(b);
        }
        cv_.notify_one();
    }

    // Blocks until a buffer is available or the queue is stopped.
    // Returns nullptr only when stopped and empty.
    Buffer* pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return nullptr;
        Buffer* b = queue_.front();
        queue_.pop_front();
        return b;
    }

    // Non-blocking drain of everything currently queued.
    void popAll(std::vector<Buffer*>& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        out.insert(out.end(), queue_.begin(), queue_.end());
        queue_.clear();
    }

    bool stopped() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return stopped_;
    }

    // Remove and hand back everything (used on restart).
    std::vector<Buffer*> drain() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<Buffer*> out(queue_.begin(), queue_.end());
        queue_.clear();
        return out;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::deque<Buffer*>     queue_;
    bool                    stopped_ = false;
};

// ---------------------------------------------------------------------
//  Backend state (file, rotation, worker thread)
// ---------------------------------------------------------------------
class LoggerState {
public:
    LoggerState() { iobuf_.resize(1 << 20); }

    void start(const std::string& path, std::size_t maxFileSize, int maxFiles) {
        path_        = path;
        maxFileSize_ = maxFileSize;
        maxFiles_    = maxFiles;

        // Recycle anything left over from a previous run.
        for (Buffer* b : queue_.drain()) globalPool().release(b);

        openFile();
        queue_.start();
        running_ = true;
        timerStop_.store(false, std::memory_order_relaxed);
        worker_  = std::thread([this] { run(); });
        timer_   = std::thread([this] { timerLoop(); });
    }

    void stop() {
        if (!running_) return;
        timerStop_.store(true, std::memory_order_release);
        if (timer_.joinable()) timer_.join();
        queue_.shutdown();
        if (worker_.joinable()) worker_.join();
        running_ = false;
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void push(Buffer* b) { queue_.push(b); }

private:
    void run() {
        std::vector<Buffer*> batch;
        for (;;) {
            Buffer* first = queue_.pop();
            if (!first) break;          // stopped and drained
            batch.clear();
            batch.push_back(first);
            queue_.popAll(batch);       // batch everything available
            writeBatch(batch);
            for (Buffer* b : batch) globalPool().release(b);
        }
        if (file_) std::fflush(file_);
    }

    // Periodically flush every live thread's active buffer, so a low-rate
    // (or idle) thread's last line still reaches disk within ~kAutoFlushMs.
    void timerLoop() {
        while (!timerStop_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kAutoFlushMs));
            if (timerStop_.load(std::memory_order_acquire)) break;
            flushAll(*this);
        }
    }

    void writeBatch(const std::vector<Buffer*>& batch) {
        if (!file_) openFile();
        if (!file_) return;

        for (Buffer* b : batch) {
            if (b->len == 0) continue;
            if (maxFileSize_ > 0 && bytesWritten_ + b->len > maxFileSize_) {
                rotate();
                if (!file_) return;
            }
            std::fwrite(b->data, 1, b->len, file_);
            bytesWritten_ += b->len;
        }
        std::fflush(file_);
    }

    void openFile() {
        file_ = std::fopen(path_.c_str(), "a");
        if (!file_) {
            std::fprintf(stderr, "[nas::Logger] cannot open log file: %s\n",
                         path_.c_str());
            return;
        }
        std::setvbuf(file_, iobuf_.data(), _IOFBF, iobuf_.size());
        bytesWritten_ = fileSize(path_);
    }

    void rotate() {
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
        if (path_.empty()) return;

        if (maxFiles_ <= 0) {
            std::remove(path_.c_str());
        } else {
            // Drop the oldest, then shift .(N-1) -> .N ... .1 -> .2
            std::remove((path_ + "." + std::to_string(maxFiles_)).c_str());
            for (int i = maxFiles_ - 1; i >= 1; --i) {
                const std::string from = path_ + "." + std::to_string(i);
                const std::string to   = path_ + "." + std::to_string(i + 1);
                std::rename(from.c_str(), to.c_str());
            }
            std::rename(path_.c_str(), (path_ + ".1").c_str());
        }
        openFile();
        if (file_) bytesWritten_ = 0;
    }

    BufferQueue         queue_;
    std::thread         worker_;
    std::thread         timer_;
    std::atomic<bool>   timerStop_{false};
    std::string         path_;
    std::size_t         maxFileSize_  = 10 * 1024 * 1024;
    int                 maxFiles_     = 5;
    std::FILE*          file_         = nullptr;
    std::size_t         bytesWritten_ = 0;
    bool                running_      = false;
    std::vector<char>   iobuf_; // user-space stdio buffer
};

// Never deleted: keeps thread contexts valid across init/shutdown.
LoggerState& state() {
    static LoggerState* s = new LoggerState();
    return *s;
}

// ---------------------------------------------------------------------
//  Registry / flush helpers
// ---------------------------------------------------------------------
void registerContext(ThreadContext* c) {
    std::lock_guard<std::mutex> reg(registryMutex());
    c->next       = registryHead();
    registryHead() = c;
    c->registered = true;
}

// Caller must hold registryMutex().
void unregisterLocked(ThreadContext* c) {
    ThreadContext** pp = &registryHead();
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            c->next = nullptr;
            return;
        }
        pp = &(*pp)->next;
    }
}

// Publish `active` if it holds data, then make sure `active`/`standby`
// are populated for continued use. Caller must hold c.mtx.
void flushLocked(ThreadContext& c, LoggerState& st) {
    if (c.active && c.active->len > 0) {
        st.push(c.active);      // ownership -> backend
        c.active = nullptr;
    }
    if (!c.active) {
        if (c.standby) { c.active = c.standby; c.standby = nullptr; }
        else           { c.active = globalPool().acquire(); }
    }
    if (!c.standby) c.standby = globalPool().acquire();
}

// Flush every live thread context. Safe to call from the backend worker
// or from shutdown(); registryMutex serialises it against thread exit.
void flushAll(LoggerState& st) {
    std::lock_guard<std::mutex> reg(registryMutex());
    for (ThreadContext* c = registryHead(); c; c = c->next) {
        std::lock_guard<std::mutex> lk(c->mtx);
        flushLocked(*c, st);
    }
}

// Make room for `need` bytes in c.active. Caller must hold c.mtx.
// Returns false (and counts a drop) when the pool is exhausted.
bool ensureSpaceLocked(ThreadContext& c, LoggerState& st, std::size_t need) {
    if (c.active && c.active->available() >= need) return true;
    flushLocked(c, st);
    if (!c.active) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return c.active->available() >= need;
}

// Streaming append used only for lines larger than a full buffer.
void appendChunkedLocked(ThreadContext& c, LoggerState& st,
                         const char* data, std::size_t n) {
    while (n > 0) {
        if (!ensureSpaceLocked(c, st, 1)) return; // dropped mid-line
        Buffer* b = c.active;
        const std::size_t space = b->available();
        const std::size_t chunk = n < space ? n : space;
        std::memcpy(b->data + b->len, data, chunk);
        b->len += chunk;
        data   += chunk;
        n      -= chunk;
    }
}

// Runs on the exiting thread (FLS / pthread_key callback). Flush the tail,
// unregister, recycle buffers and free the context.
void destroyContext(void* p) {
    ThreadContext* c = static_cast<ThreadContext*>(p);
    if (!c) return;

    LoggerState* st = g_state.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> reg(registryMutex());
        if (c->registered) {
            if (st) {                       // still running: flush tail
                std::lock_guard<std::mutex> lk(c->mtx);
                flushLocked(*c, *st);
            }
            unregisterLocked(c);
        }
    }
    // Safe now: no flushAll can observe this context anymore.
    if (c->active)  globalPool().release(c->active);
    if (c->standby) globalPool().release(c->standby);
    delete c;
}

// Lazily create + register the calling thread's context.
ThreadContext* currentContext() {
    auto* c = static_cast<ThreadContext*>(tlsGet());
    if (!c) {
        c = new ThreadContext();
        tlsSet(c);              // arm the thread-exit hook
        registerContext(c);
    }
    return c;
}

void waitForQuiescence() {
    while (g_inLog.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

} // namespace

// ---------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------
const char* Logger::baseName(const char* path) noexcept {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

std::uint64_t Logger::droppedCount() noexcept {
    return g_dropped.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel lvl, const char* file, int line, const char* func,
                 const char* fmt, ...) {
    if (!shouldLog(lvl)) return;

    // Count ourselves as in-flight *before* reading g_state, so shutdown()
    // can never observe an empty counter while we still use the backend.
    g_inLog.fetch_add(1, std::memory_order_relaxed);
    LoggerState* st = g_state.load(std::memory_order_acquire);

    if (st) {
        ThreadContext* c = currentContext();
        std::lock_guard<std::mutex> lk(c->mtx);
        if (c->tid == 0) c->tid = NAS_THREAD_ID();
        if (!c->active) {
            c->active  = globalPool().acquire();
            c->standby = globalPool().acquire();
        }

        if (!c->active) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::int64_t nowMs = nowMillis();
            char ts[20];
            const int ms = buildTimestamp(ts, nowMs);

            char header[kMaxInlineHeader];
            const std::size_t hlen = buildHeader(header, lvl, baseName(file),
                                                 line, func, c->tid, ts, ms);

            char        stackMsg[kMaxInlineMessage];
            std::string heapMsg;
            std::size_t mlen = 0;
            {
                va_list ap;
                va_start(ap, fmt);
                va_list ap2;
                va_copy(ap2, ap);

                const int n = std::vsnprintf(stackMsg, sizeof(stackMsg), fmt, ap2);
                va_end(ap2);

                if (n > 0) {
                    if (static_cast<std::size_t>(n) < sizeof(stackMsg)) {
                        mlen = static_cast<std::size_t>(n);
                    } else {
                        heapMsg.resize(static_cast<std::size_t>(n) + 1);
                        std::vsnprintf(&heapMsg[0], heapMsg.size(), fmt, ap);
                        mlen = static_cast<std::size_t>(n);
                    }
                }
                va_end(ap);
            }
            const char* msg = heapMsg.empty() ? stackMsg : heapMsg.c_str();

            const std::size_t total = hlen + mlen + 1; // + '\n'
            if (total > detail::Buffer::kCapacity) {
                appendChunkedLocked(*c, *st, header, hlen);
                appendChunkedLocked(*c, *st, msg, mlen);
                appendChunkedLocked(*c, *st, "\n", 1);
            } else if (ensureSpaceLocked(*c, *st, total)) {
                Buffer* b = c->active;
                std::memcpy(b->data + b->len, header, hlen); b->len += hlen;
                std::memcpy(b->data + b->len, msg, mlen);    b->len += mlen;
                b->data[b->len++] = '\n';
            }

            if (lvl == LogLevel::FATAL) flushLocked(*c, *st);
        }
    }

    g_inLog.fetch_sub(1, std::memory_order_relaxed);
}

void Logger::init(const std::string& filepath, LogLevel level,
                  std::size_t maxFileSizeBytes, int maxFiles) {
    std::lock_guard<std::mutex> lk(g_initMtx);

    LoggerState& st = state();

    if (g_state.exchange(nullptr, std::memory_order_acq_rel)) {
        waitForQuiescence();   // let in-flight log() calls finish
        flushAll(st);          // flush every live thread into the old run
        st.stop();             // drain, join worker, close file
    }

    st.start(filepath, maxFileSizeBytes, maxFiles);
    s_level.store(level, std::memory_order_relaxed);
    g_state.store(&st, std::memory_order_release);
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lk(g_initMtx);

    LoggerState* st = g_state.exchange(nullptr, std::memory_order_acq_rel);
    if (!st) return;

    waitForQuiescence();       // no thread is still using the backend
    flushAll(*st);             // flush every live thread's tail
    st->stop();                // drain, join worker, close file
    s_level.store(LogLevel::OFF, std::memory_order_relaxed);
}

void Logger::flush() {
    g_inLog.fetch_add(1, std::memory_order_relaxed);
    LoggerState* st = g_state.load(std::memory_order_acquire);
    if (st) {
        auto* c = static_cast<ThreadContext*>(tlsGet());
        if (c) {
            std::lock_guard<std::mutex> lk(c->mtx);
            if (c->active) flushLocked(*c, *st);
        }
    }
    g_inLog.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace nas
