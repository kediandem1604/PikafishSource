#include "pikafish_c.h"
#include "../src/engine.h"
#include "../src/uci.h"  // UCIEngine::format_score
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <new>
#include <csignal>
#include <cstring>

static std::mutex s_mutex;
static std::unique_ptr<Stockfish::Engine> s_engine;
static std::thread s_search_thread;
static std::atomic<bool> s_searching{false};
static std::atomic<bool> s_threads_frozen{false};

static std::once_flag s_global_init;
static bool s_inited = false;

// Forward declarations for message queue
static void push_info_msg(const char* s) noexcept;
static void push_bestmove_msg(const char* best, const char* ponder) noexcept;
extern "C" void pika_clear_messages(void) noexcept; // forward decl so SearchGo can clear queue

// Default no-op callbacks để tránh nullptr
static void noop_info(const char*) noexcept {}
static void noop_bestmove(const char*, const char*) noexcept {}

// Dùng atomic để thread-safe callback pointers
// Mặc định là no-op để tránh crash khi chưa được set từ C#
static std::atomic<pika_info_cb>     s_info_cb{noop_info};
static std::atomic<pika_bestmove_cb> s_bestmove_cb{noop_bestmove};
static std::atomic<bool>             s_cb_ready{false}; // Flag: callbacks đã được set từ C# và ready

namespace {
    // Helper an toàn để gọi callback nếu có
    static inline void EmitInfo(const char* msg) noexcept {
        auto cb = s_info_cb.load(std::memory_order_acquire);
        if (cb) {
            try {
                cb(msg);
            } catch (...) {
            }
        }
    }
    
    // Cài đặt crash guards để vẫn có log nếu std::terminate xảy ra ở thread sâu
    static void InstallCrashGuardsOnce() noexcept {
        static std::once_flag once;
        std::call_once(once, [](){
            try {
                std::set_terminate([](){
                    try { 
                        push_info_msg("[native] std::terminate called"); 
                    } catch (...) {}
                    std::abort();
                });
            } catch (...) {
            }
            // Note: std::set_unexpected deprecated/removed in C++17+, không dùng nữa
        });
    }
    
    static inline void EmitBestMove(const char* best, const char* ponder) noexcept {
        auto cb = s_bestmove_cb.load(std::memory_order_acquire);
        if (cb) {
            try {
                cb(best, ponder);
            } catch (...) {
                // Swallow exception để không ném ra ngoài DLL
            }
        }
    }
    
    // Thunk functions luôn hợp lệ để tránh std::bad_function_call
    // Đây là các static thunk được attach vào Engine, không bao giờ rỗng
    static void Thunk_OnInfoShort(const Stockfish::Engine::InfoShort& info) noexcept {
        try {
            // InfoShort chỉ có depth và score, format tối thiểu
            std::ostringstream oss;
            oss << "info depth " << info.depth
                << " score " << Stockfish::UCIEngine::format_score(info.score);
            const auto s = oss.str();
            push_info_msg(s.c_str());
        } catch (...) {
        }
    }
    
    static void Thunk_OnInfoFull(const Stockfish::Engine::InfoFull& info) noexcept {
        try {
            std::ostringstream oss;
            oss << "info depth " << info.depth;
            if (info.selDepth > 0)
                oss << " seldepth " << info.selDepth;
            if (info.nodes > 0)
                oss << " nodes " << info.nodes;
            if (info.nps > 0)
                oss << " nps " << info.nps;
            // Score là kiểu Score -> format qua UCIEngine
            oss << " score " << Stockfish::UCIEngine::format_score(info.score);
            if (!info.pv.empty()) {
                oss << " pv " << info.pv;
            }
            const auto s = oss.str();
            push_info_msg(s.c_str());
        } catch (...) {
        }
    }
    
    static void Thunk_OnBestMove(std::string_view best, std::string_view ponder) noexcept {
        try {
            push_bestmove_msg(best.data(), ponder.data());
        } catch (...) {
        }
    }
    
    // Helper nhẹ: chỉ gán callbacks, KHÔNG resize/không wait để tránh race
    static inline void EnsureEngineCallbacksInstalled(Stockfish::Engine& e) noexcept {
        try {
            push_info_msg("[native] EnsureEngineCallbacksInstalled: setting callbacks");
            
            // Gán 5 callbacks public của Engine - đảm bảo KHÔNG callback nào là empty
            // Dùng static thunks thay vì OnInfo/OnBestMove để tránh scope issues
            e.set_on_update_no_moves(Thunk_OnInfoShort);
            push_info_msg("[native] EnsureEngineCallbacksInstalled: set_on_update_no_moves OK");
            
            // set_on_update_full dùng InfoFull (có đầy đủ fields)
            e.set_on_update_full(Thunk_OnInfoFull);
            e.set_on_iter([](const Stockfish::Engine::InfoIter&) noexcept {});
            e.set_on_bestmove(Thunk_OnBestMove);
            e.set_on_verify_networks([](std::string_view) noexcept {});
            // Không resize ở đây: threads được freeze/resize tại pika_init_threads()
            push_info_msg("[native] EnsureEngineCallbacksInstalled: callbacks set (no resize)");
        } catch (const std::bad_function_call&) {
            push_info_msg("[native] EnsureEngineCallbacksInstalled ERROR: bad_function_call (callback empty during install)");
        } catch (const std::exception& ex) {
            try {
                std::string msg = "[native] EnsureEngineCallbacksInstalled exception: ";
                msg += ex.what();
                push_info_msg(msg.c_str());
            } catch (...) {
                push_info_msg("[native] EnsureEngineCallbacksInstalled: exception formatting failed");
            }
        } catch (...) {
            push_info_msg("[native] EnsureEngineCallbacksInstalled: unknown exception");
        }
    }
    
    void SearchGo(Stockfish::Engine* engine, Stockfish::Search::LimitsType limit) noexcept {
        // Clear message queue trước khi bắt đầu search mới
        pika_clear_messages();
        
        push_info_msg("[native] SearchGo: thread started");
        
        // CRITICAL: Bọc toàn bộ engine->go() trong try/catch để bắt mọi exception
        // bao gồm cả std::bad_function_call từ bên trong Engine
        try {
            push_info_msg("[native] SearchGo: starting engine->go()");
            
            // s_searching đã được set trong pika_go_* trước khi spawn thread
            engine->go(limit);
            
            push_info_msg("[native] SearchGo: engine->go() returned, waiting for finish");
            engine->wait_for_search_finished();
            push_info_msg("[native] SearchGo: search completed successfully");
        } catch (const std::bad_function_call& e) {
            // Đây là exception chính gây crash - một std::function rỗng đã được gọi
            try {
                std::ostringstream oss;
                oss << "[native] SearchGo ERROR: caught std::bad_function_call (empty std::function in engine). "
                    << "This means Engine has an uninitialized callback that was called during search.";
                push_info_msg(oss.str().c_str());
            } catch (...) {
                push_info_msg("[native] SearchGo ERROR: caught std::bad_function_call (failed to format)");
            }
            push_bestmove_msg("", "");
        } catch (const std::exception& ex) {
            try {
                std::string msg = "[native] SearchGo ERROR: std::exception: ";
                msg += ex.what();
                push_info_msg(msg.c_str());
            } catch (...) {
                push_info_msg("[native] SearchGo ERROR: failed to format exception message");
            }
            push_bestmove_msg("", "");
        } catch (...) {
            push_info_msg("[native] SearchGo ERROR: unknown exception caught");
            push_bestmove_msg("", "");
        }
        s_searching.store(false, std::memory_order_release);
    }
}

// ======================= POLLING MESSAGE QUEUE =======================
static constexpr size_t PIKA_QCAP = 4096;
static PikaMsg g_msgq[PIKA_QCAP];
static std::atomic<uint32_t> g_qw{0}, g_qr{0};

static inline bool q_push_msg(const PikaMsg& m) noexcept {
    uint32_t w = g_qw.load(std::memory_order_relaxed);
    uint32_t r = g_qr.load(std::memory_order_acquire);
    if (w - r >= PIKA_QCAP) return false;
    g_msgq[w % PIKA_QCAP] = m;
    g_qw.store(w + 1, std::memory_order_release);
    return true;
}

static inline int q_pop_bulk_msgs(PikaMsg* out, int nmax) noexcept {
    if (!out || nmax <= 0) return 0;
    uint32_t r = g_qr.load(std::memory_order_relaxed);
    uint32_t w = g_qw.load(std::memory_order_acquire);
    uint32_t avail = w - r;
    if (!avail) return 0;
    uint32_t n = (uint32_t)((avail < (uint32_t)nmax) ? avail : (uint32_t)nmax);
    for (uint32_t i = 0; i < n; ++i) out[i] = g_msgq[(r + i) % PIKA_QCAP];
    g_qr.store(r + n, std::memory_order_release);
    return (int)n;
}

// Helper functions để push messages vào queue
static inline void push_info_msg(const char* s) noexcept {
    if (!s) return;
    PikaMsg m{};
    m.type = PIKA_MSG_INFO;
    std::strncpy(m.b, s, sizeof(m.b) - 1);
    m.b[sizeof(m.b) - 1] = '\0'; // Ensure null-termination
    q_push_msg(m);
}

static inline void push_bestmove_msg(const char* best, const char* ponder) noexcept {
    PikaMsg m{};
    m.type = PIKA_MSG_BESTMOVE;
    if (best) {
        std::strncpy(m.a, best, sizeof(m.a) - 1);
        m.a[sizeof(m.a) - 1] = '\0';
    }
    if (ponder) {
        std::strncpy(m.b, ponder, sizeof(m.b) - 1);
        m.b[sizeof(m.b) - 1] = '\0';
    }
    q_push_msg(m);
}

// Freeze-threads: đặt số threads và resize duy nhất ở đây
extern "C" int pika_init_threads(int threads) noexcept {
    try {
        if (threads < 1) threads = 1;
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_engine) return 0;
        auto& e = *s_engine;
        // Dừng search nếu đang chạy
        e.wait_for_search_finished();
        // Set option Threads qua OptionsMap::setoption (UCI style)
        try {
            std::ostringstream oss;
            oss << "name Threads value " << threads;
            std::istringstream iss(oss.str());
            e.get_options().setoption(iss);
        } catch (...) {}
        // Cài TRƯỚC để seed callback nếu engine copy trong resize
        EnsureEngineCallbacksInstalled(e);
        // Resize ngay bây giờ
        e.resize_threads();
        // Cài SAU để đảm bảo worker mới có functor hợp lệ
        EnsureEngineCallbacksInstalled(e);
        s_threads_frozen.store(true, std::memory_order_release);
        return 1;
    } catch (...) {
        return -1;
    }
}

extern "C" int pika_is_threads_frozen(void) noexcept {
    return s_threads_frozen.load(std::memory_order_acquire) ? 1 : 0;
}

// Ghi đè callbacks bằng bản safe để đảm bảo không rỗng
extern "C" int pika_validate_callbacks(void) noexcept {
    try {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_engine) return -1;
        EnsureEngineCallbacksInstalled(*s_engine);
        return 1;
    } catch (...) {
        return 0;
    }
}

// Selftest: gọi thunks ngay trên main thread để xác nhận không rỗng
extern "C" int pika_selftest_callbacks(void) noexcept {
    try {
        if (!s_engine) return -1;
        EnsureEngineCallbacksInstalled(*s_engine);
        push_info_msg("[selftest] info");
        push_bestmove_msg("0000", "");
        return 1;
    } catch (const std::bad_function_call&) {
        push_info_msg("[selftest] bad_function_call");
        return -2;
    } catch (...) {
        push_info_msg("[selftest] exception");
        return -3;
    }
}
// Polling queue APIs
extern "C" int pika_poll_messages(PikaMsg* out, int out_len) noexcept {
    return q_pop_bulk_msgs(out, out_len);
}

extern "C" void pika_clear_messages(void) noexcept {
    g_qr.store(g_qw.load(std::memory_order_acquire), std::memory_order_release);
}

extern "C" int pika_is_searching(void) noexcept {
    return s_searching.load(std::memory_order_acquire) ? 1 : 0;
}
// ------------------ API implementation -------------------
extern "C" int pika_global_init(void) noexcept {
    try {
        std::call_once(s_global_init, [](){
            Stockfish::Bitboards::init();
            Stockfish::Position::init();
            // ... bổ sung các init toàn cục khác nếu có ...
            s_inited = true;
        });
        return s_inited ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

extern "C" int pika_init(void) noexcept {
    try {
        InstallCrashGuardsOnce();
        if (!s_inited) pika_global_init();
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_engine) return 1;
        s_engine = std::make_unique<Stockfish::Engine>();
        
        // Set tất cả callbacks lần đầu (không resize ở đây để tránh race sớm)
        EnsureEngineCallbacksInstalled(*s_engine);
        return 1;
    } catch (...) {
        return -1;
    }
}

extern "C" void pika_quit(void) noexcept {
    try {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_engine) {
            s_info_cb.store(noop_info, std::memory_order_release);
            s_bestmove_cb.store(noop_bestmove, std::memory_order_release);
            s_cb_ready.store(false, std::memory_order_release);
            s_threads_frozen.store(false, std::memory_order_release);
            
            s_engine->stop();
            if (s_search_thread.joinable()) s_search_thread.join();
            s_engine.reset();
        }
    } catch (...) {
    }
}

extern "C" int pika_setoption(const char* key, const char* value) noexcept {
    try {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_engine) return 0;
        auto& opts = s_engine->get_options();
        std::ostringstream oss;
        oss << "name " << key << " value " << (value ? value : "");
        std::istringstream iss(oss.str());
        opts.setoption(iss);
        return 1;
    } catch (...) {
        return -1;
    }
}

extern "C" int pika_position_fen(const char* fen, const char* moves_csv) noexcept {
    try {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_engine) return 0;
        std::vector<std::string> moves;
        if (moves_csv && moves_csv[0] != '\0') {
            std::string s(moves_csv);
            size_t start = 0, end;
            while ((end = s.find(',', start)) != std::string::npos) {
                auto mv = s.substr(start, end - start);
                // Trim whitespace
                mv.erase(0, mv.find_first_not_of(" \t\r\n"));
                mv.erase(mv.find_last_not_of(" \t\r\n") + 1);
                if (!mv.empty())
                    moves.emplace_back(std::move(mv));
                start = end + 1;
            }
            if (start < s.length()) {
                auto mv = s.substr(start);
                mv.erase(0, mv.find_first_not_of(" \t\r\n"));
                mv.erase(mv.find_last_not_of(" \t\r\n") + 1);
                if (!mv.empty())
                    moves.emplace_back(std::move(mv));
            }
        }
        s_engine->set_position(std::string(fen ? fen : "startpos"), moves);
        return 1;
    } catch (...) {
        return -1;
    }
}

extern "C" int pika_go_movetime(int ms) noexcept {
    try {
        std::thread old;
        Stockfish::Search::LimitsType limit;
        limit.movetime = ms;
        
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (!s_engine || s_searching.load()) return 0;
            if (s_search_thread.joinable())
                old = std::move(s_search_thread);
        }
        
        // Join old thread ngoài lock để tránh deadlock
        if (old.joinable())
            old.join();
        
        push_info_msg("[native] pika_go_movetime: callbacks ready, installing to engine");
        
        EnsureEngineCallbacksInstalled(*s_engine);
        if (!s_threads_frozen.load(std::memory_order_acquire)) {
            push_info_msg("[native] pika_go_movetime ERROR: threads not frozen (call pika_init_threads first)");
            return -2;
        }
        
        push_info_msg("[native] pika_go_movetime: spawning search thread (about to create thread)");
        
        // Spawn search thread
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_searching.store(true, std::memory_order_release);
            s_search_thread = std::thread(SearchGo, s_engine.get(), limit);
        }
        
        return 1;
    } catch (const std::bad_function_call&) {
        return -2;
    } catch (const std::exception& e) {
        return -2;
    } catch (...) {
        return -3;
    }
}

extern "C" int pika_go_depth(int depth) noexcept {
    try {
        if (depth < 1) depth = 1; // clamp để tránh nhánh bất thường
        std::thread old;
        Stockfish::Search::LimitsType limit;
        limit.depth = depth;
        
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (!s_engine || s_searching.load()) return 0;
            if (s_search_thread.joinable())
                old = std::move(s_search_thread);
        }
        
        if (old.joinable())
            old.join();
        
        // Debug log
        push_info_msg("[native] pika_go_depth: callbacks ready, installing to engine");
        
        EnsureEngineCallbacksInstalled(*s_engine);
        if (!s_threads_frozen.load(std::memory_order_acquire)) {
            push_info_msg("[native] pika_go_depth ERROR: threads not frozen (call pika_init_threads first)");
            return -2;
        }
        
        push_info_msg("[native] pika_go_depth: spawning search thread");
        
        // Spawn search thread
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_searching.store(true, std::memory_order_release);
            s_search_thread = std::thread(SearchGo, s_engine.get(), limit);
        }
        
        return 1;
    } catch (const std::bad_function_call&) {
        return -2;
    } catch (const std::exception& e) {
        return -2;
    } catch (...) {
        return -3;
    }
}

extern "C" int pika_go_infinite(void) noexcept {
    try {
        std::thread old;
        Stockfish::Search::LimitsType limit;
        limit.infinite = true;
        
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (!s_engine || s_searching.load()) return 0;
            if (s_search_thread.joinable())
                old = std::move(s_search_thread);
        }
        
        if (old.joinable())
            old.join();
        
        push_info_msg("[native] pika_go_infinite: callbacks ready, installing to engine");
        EnsureEngineCallbacksInstalled(*s_engine);
        if (!s_threads_frozen.load(std::memory_order_acquire)) {
            push_info_msg("[native] pika_go_infinite ERROR: threads not frozen (call pika_init_threads first)");
            return -2;
        }
        
        push_info_msg("[native] pika_go_infinite: spawning search thread");
        
        // Spawn search thread
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_searching.store(true, std::memory_order_release);
            s_search_thread = std::thread(SearchGo, s_engine.get(), limit);
        }
        
        return 1;
    } catch (const std::bad_function_call&) {
        return -2;
    } catch (const std::exception& e) {
        return -2;
    } catch (...) {
        return -3;
    }
}

extern "C" int pika_stop(void) noexcept {
    try {
        std::thread old;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (!s_engine) return 0;
            s_engine->stop();
            s_searching.store(false, std::memory_order_release);
            if (s_search_thread.joinable())
                old = std::move(s_search_thread);
        }
        
        if (old.joinable())
            old.join();
        
        return 1;
    } catch (...) {
        return -1;
    }
}

// Publish callbacks an toàn với memory fence mạnh để tránh race
static inline void publish_callbacks(pika_info_cb info_cb, pika_bestmove_cb bestmove_cb) noexcept {
    // Store với release semantics
    s_info_cb.store(info_cb ? info_cb : noop_info, std::memory_order_release);
    s_bestmove_cb.store(bestmove_cb ? bestmove_cb : noop_bestmove, std::memory_order_release);
    
    // Update ready flag
    bool info_ok = (info_cb != nullptr && info_cb != noop_info);
    bool best_ok = (bestmove_cb != nullptr && bestmove_cb != noop_bestmove);
    s_cb_ready.store(info_ok && best_ok, std::memory_order_release);
    
    // Hàng rào mạnh (seq_cst fence) để đảm bảo publish hoàn tất trước khi thread khác đọc
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

extern "C" void pika_set_info_callback(pika_info_cb cb) noexcept {
    try {
        auto best = s_bestmove_cb.load(std::memory_order_acquire);
        publish_callbacks(cb, best);
    } catch (...) {
    }
}

extern "C" void pika_set_bestmove_callback(pika_bestmove_cb cb) noexcept {
    try {
        auto info = s_info_cb.load(std::memory_order_acquire);
        publish_callbacks(info, cb);
    } catch (...) {
    }
}

extern "C" void pika_set_callbacks(pika_info_cb info_cb, pika_bestmove_cb bestmove_cb) noexcept {
    try {
        // Tripwire: Log pointers trước khi set
        void* old_info = (void*)s_info_cb.load(std::memory_order_acquire);
        void* old_best = (void*)s_bestmove_cb.load(std::memory_order_acquire);
        void* new_info = (void*)(info_cb ? info_cb : noop_info);
        void* new_best = (void*)(bestmove_cb ? bestmove_cb : noop_bestmove);
        
        try {
            std::ostringstream oss;
            oss << "[native] pika_set_callbacks: old_info=" << old_info << " old_best=" << old_best
                << " new_info=" << new_info << " new_best=" << new_best;
            EmitInfo(oss.str().c_str());
        } catch (...) {
            EmitInfo("[native] pika_set_callbacks: logging ptrs failed");
        }
        
        // Publish với memory fence mạnh
        publish_callbacks(info_cb, bestmove_cb);
        
        // Debug log với pointer confirmation
        bool ready = s_cb_ready.load(std::memory_order_acquire);
        if (ready) {
            try {
                std::ostringstream oss;
                oss << "[native] pika_set_callbacks: both callbacks set and ready (info=" 
                    << new_info << " best=" << new_best << ") with memory fence";
                EmitInfo(oss.str().c_str());
            } catch (...) {
                EmitInfo("[native] pika_set_callbacks: both callbacks set and ready with memory fence");
            }
        } else {
            EmitInfo("[native] pika_set_callbacks: callbacks NOT ready (null or no-op)");
        }
    } catch (...) {
        // Swallow exception để không ném ra ngoài DLL
    }
}

// Test function để verify callbacks đã được set đúng
// Gọi ngay sau pika_set_callbacks() để confirm native side thấy được callbacks
extern "C" void pika_emit_test_callbacks(void) noexcept {
    try {
        // Test info callback
        EmitInfo("[native] test: info callback OK");
        
        // Test bestmove callback
        EmitBestMove("a0a0", "a1a1");
    } catch (...) {
        // Swallow exception để không ném ra ngoài DLL
    }
}

// Debug function để check callback state từ C#
// Returns: 0=none, 1=info only, 2=bestmove only, 3=both
extern "C" int pika_get_callback_state(void) noexcept {
    try {
        auto info = s_info_cb.load(std::memory_order_acquire);
        auto best = s_bestmove_cb.load(std::memory_order_acquire);
        
        bool has_info = (info != nullptr && info != noop_info);
        bool has_best = (best != nullptr && best != noop_bestmove);
        
        if (has_info && has_best) return 3; // Both set
        if (has_info) return 1; // Info only
        if (has_best) return 2; // BestMove only
        return 0; // None set (only no-ops)
    } catch (...) {
        return -1; // Error
    }
}

// Test function: emit từ một thread riêng để verify callback hoạt động từ worker thread
// Gọi từ C# sau khi set callbacks để confirm không có race condition
extern "C" void pika_test_emit_from_thread(void) noexcept {
    try {
        // Check callbacks ready
        if (!s_cb_ready.load(std::memory_order_acquire)) {
            EmitInfo("[native] pika_test_emit_from_thread ERROR: callbacks not ready");
            return;
        }
        
        // Tripwire: Log pointers trước khi spawn test thread
        auto info_ptr = (void*)s_info_cb.load(std::memory_order_acquire);
        auto best_ptr = (void*)s_bestmove_cb.load(std::memory_order_acquire);
        try {
            std::ostringstream oss;
            oss << "[native] pika_test_emit_from_thread: cb ptrs info=" << info_ptr 
                << " best=" << best_ptr << ", spawning test thread";
            EmitInfo(oss.str().c_str());
        } catch (...) {
            EmitInfo("[native] pika_test_emit_from_thread: spawning test thread");
        }
        
        // Spawn một thread test và emit từ đó
        std::thread([]() noexcept {
            try {
                // Small delay để simulate real thread
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                // Double-check callbacks vẫn valid trong thread
                auto info_check = s_info_cb.load(std::memory_order_acquire);
                auto best_check = s_bestmove_cb.load(std::memory_order_acquire);
                
                if (!info_check || info_check == noop_info || 
                    !best_check || best_check == noop_bestmove) {
                    EmitInfo("[native] pika_test_emit_from_thread ERROR: callbacks became invalid in worker thread");
                    return;
                }
                
                EmitInfo("[native] pika_test_emit_from_thread: emitting from worker thread");
                EmitBestMove("a0a0", "a1a1");
                
                EmitInfo("[native] pika_test_emit_from_thread: SUCCESS - callbacks work from worker thread");
            } catch (const std::bad_function_call&) {
                EmitInfo("[native] pika_test_emit_from_thread ERROR: bad_function_call in worker thread");
            } catch (const std::exception& ex) {
                try {
                    std::string msg = "[native] pika_test_emit_from_thread ERROR: ";
                    msg += ex.what();
                    EmitInfo(msg.c_str());
                } catch (...) {
                    EmitInfo("[native] pika_test_emit_from_thread ERROR: exception in test thread");
                }
            } catch (...) {
                EmitInfo("[native] pika_test_emit_from_thread ERROR: unknown exception in test thread");
            }
        }).detach();
    } catch (...) {
        EmitInfo("[native] pika_test_emit_from_thread ERROR: failed to spawn test thread");
    }
}
