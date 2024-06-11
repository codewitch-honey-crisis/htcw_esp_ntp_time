#pragma once
#if __has_include(<Arduino.h>)
#include <Arduino.h>
#else
#include <esp_idf_version.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <lwip/sockets.h>
#endif
#ifndef ESP_PLATFORM
#error "This library only supports the ESP32 MCU."
#endif
#ifdef ARDUINO
namespace arduino {
#else
namespace esp_idf {
#endif
/// @brief Called when the NTP request finishes (success or failure)
typedef void(*ntp_time_callback)(void*);
/// @brief Provides NTP services
class ntp_time final {
    constexpr static const char* domain = "pool.ntp.org";
    bool m_requesting;
    time_t m_request_result;
    uint8_t m_packet_buffer[48];
    uint32_t m_retry_timeout;
    uint32_t m_retry_ts;
    size_t m_retry_count;
    size_t m_retries;
    ntp_time_callback m_callback;
    void* m_callback_state;
#ifndef ARDUINO
    int m_socket;
    sockaddr_in m_addr;
    inline static uint32_t millis() {
        return pdTICKS_TO_MS(xTaskGetTickCount());
    }
#endif
    bool send_packet();
   public:
    /// @brief Constructs an instance
    inline ntp_time() : m_requesting(false),m_request_result(0),m_retry_timeout(0),m_retry_ts(0),m_retry_count(0),m_retries(0),m_callback(nullptr),m_callback_state(nullptr)
#ifndef ARDUINO
,m_socket(-1)
#endif
     {}
    /// @brief Initiates an NTP request
    /// @param address The IP address of the server
    /// @param retry_count The retry count or zero to retry indefinitely
    /// @param retry_ms The time between retries, in millisecons
    /// @param callback The callback to call on success or failure
    /// @param callback_state User defined state to pass with the callback
    /// @return true if successful, otherwise false
    bool begin_request(size_t retry_count = 0,
                    uint32_t retry_ms = 3000,
                    ntp_time_callback callback = nullptr, 
                    void* callback_state = nullptr);
    /// @brief Indicates whether or not the request has been received
    /// @return True if received, otherwise false
    inline bool request_received() const { return m_request_result!=0;}
    /// @brief The result of the request, if request_received() is true
    /// @return The result of the request, or zero if not received yet
    inline time_t request_result() const { return m_request_result; }
    /// @brief Indicates if the NTP service is currently fatching
    /// @return True if fetch in process, otherwise false
    inline bool requesting() const { return m_requesting; }
    /// @brief Pumps the NTP request. Must be called repeatedly in the master loop
    /// @return True if more work to do, otherwise false
    bool update();
};
}  // namespace arduino