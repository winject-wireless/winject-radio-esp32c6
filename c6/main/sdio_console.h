#ifndef WINJECT_SDIO_CONSOLE_H_
#define WINJECT_SDIO_CONSOLE_H_

#include "config.h"
#include "mplane_reply.h"

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class mplane_commands;

// m-plane over the SDIO link. Each m-plane message from the P4 is one request
// datagram (one or more command lines); the replies go back as m-plane
// messages of up to k_message_max bytes, split at line boundaries, as UDP
// replies continue in further datagrams.
class sdio_console : private mplane_reply
{
public:
    static sdio_console& instance();
    sdio_console(const sdio_console&) = delete;
    sdio_console& operator=(const sdio_console&) = delete;

    // commands must outlive the console. Requests that arrive before start()
    // are dropped.
    bool start(mplane_commands& commands);

    // Link receive task: copies the request for the m-plane task. A full queue
    // drops it, as a full UDP socket would.
    void on_request(const uint8_t* payload, uint16_t len);

private:
    static constexpr size_t k_request_max = 1500;
    static constexpr size_t k_message_max = 1500;
    static constexpr size_t k_reply_max = 16384;

    struct request
    {
        uint16_t len;
        char text[k_request_max];
    };

    sdio_console() = default;
    static void task(void* arg);
    void run();
    void write(const char* data, size_t n) override;
    void flush_reply();

    mplane_commands* commands_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    char buf_[k_request_max + 1]{};
    char reply_[k_reply_max]{};
    size_t reply_len_ = 0;
};

#endif  // WINJECT_SDIO_CONSOLE_H_
