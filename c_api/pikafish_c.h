#pragma once

#if defined(_WIN32)
  #define PIKA_API extern "C" __declspec(dllexport)
#else
  #define PIKA_API extern "C"
#endif

// Callback kiểu phẳng
typedef void (*pika_info_cb)(const char* line);
typedef void (*pika_bestmove_cb)(const char* best, const char* ponder);

PIKA_API int  pika_init(void) noexcept;
PIKA_API void pika_quit(void) noexcept;
PIKA_API int  pika_setoption(const char* key, const char* value) noexcept;
PIKA_API int  pika_position_fen(const char* fen, const char* moves_csv) noexcept;
PIKA_API int  pika_go_movetime(int ms) noexcept;
PIKA_API int  pika_go_depth(int depth) noexcept;
PIKA_API int  pika_go_infinite(void) noexcept;
PIKA_API int  pika_stop(void) noexcept;
PIKA_API void pika_set_info_callback(pika_info_cb cb) noexcept;
PIKA_API void pika_set_bestmove_callback(pika_bestmove_cb cb) noexcept;
PIKA_API void pika_set_callbacks(pika_info_cb info_cb, pika_bestmove_cb bestmove_cb) noexcept;
PIKA_API void pika_emit_test_callbacks(void) noexcept;
PIKA_API int  pika_get_callback_state(void) noexcept; // Returns: 0=none, 1=info only, 2=bestmove only, 3=both
PIKA_API void pika_test_emit_from_thread(void) noexcept;
PIKA_API int  pika_global_init(void) noexcept;

// Freeze-threads API: chỉ gọi ngay sau init
// Đặt số threads và resize ngay, propagate callbacks; trả 1 nếu OK
PIKA_API int  pika_init_threads(int threads) noexcept;
// Trả 1 nếu threads đã freeze (đã resize), 0 nếu chưa
PIKA_API int  pika_is_threads_frozen(void) noexcept;
// Xác thực cài đặt callbacks (ghi đè bằng safe callbacks); 1 nếu OK, 0/-1 nếu lỗi
PIKA_API int  pika_validate_callbacks(void) noexcept;
PIKA_API int  pika_selftest_callbacks(void) noexcept;

// -------- Polling message queue (không cần callback từ C#) ---------
typedef enum {
    PIKA_MSG_INFO = 1,
    PIKA_MSG_BESTMOVE = 2
} PikaMsgType;

typedef struct PikaMsg {
    unsigned int type;   // PikaMsgType
    unsigned int reserved;
    char a[64];           // bestmove hoặc phụ
    char b[256];          // info string hoặc ponder
} PikaMsg;

// Lấy tối đa out_len message từ hàng đợi; trả số lượng thực lấy được
PIKA_API int  pika_poll_messages(struct PikaMsg* out, int out_len) noexcept;
// Xoá hết message hiện có trong hàng đợi
PIKA_API void pika_clear_messages(void) noexcept;
// Trạng thái search hiện tại (1 đang search, 0 rảnh)
PIKA_API int  pika_is_searching(void) noexcept;
